/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_RUNTIME_GC_ACCOUNTING_CARD_TABLE_H_
#define ART_RUNTIME_GC_ACCOUNTING_CARD_TABLE_H_

#include <memory>

#include "base/locks.h"
#include "base/mem_map.h"
#include "base/utils.h"
#include "runtime_globals.h"

namespace art HIDDEN {

namespace mirror {
class Object;
}  // namespace mirror

namespace gc {

namespace space {
class ContinuousSpace;
}  // namespace space

class Heap;

namespace accounting {

template<size_t kAlignment> class SpaceBitmap;

// Maintain a card table from the the write barrier. All writes of
// non-null values to heap addresses should go through an entry in
// WriteBarrier, and from there to here.
class CardTable {
 public:
  static constexpr size_t kCardShift = 10;
  static constexpr size_t kCardSize = 1 << kCardShift;
  static constexpr uint8_t kCardClean = 0x0;
  // Value written into the card by the write-barrier to indicate that
  // reference(s) to some object starting in this card has been modified.
  static constexpr uint8_t kCardDirty = 0x70;
  // Value to indicate that a dirty card is 'aged' now in the sense that it has
  // been noticed by the GC and will be visited.
  static constexpr uint8_t kCardAged = kCardDirty - 1;
  // Further ageing an aged card usually means clearing the card as we have
  // already visited it when ageing it the first time. This value is used to
  // avoid re-visiting (in the second pass of CMC marking phase) cards which
  // contain old-to-young references and have not been dirtied since the first
  // pass of marking. We can't simply clean these cards as they are needed later
  // in compaction phase to update the old-to-young references.
  static constexpr uint8_t kCardAged2 = kCardAged - 1;

  static CardTable* Create(const uint8_t* heap_begin, size_t heap_capacity);
  ~CardTable();

  // Set the card associated with the given address to `kCardDirty`.
  ALWAYS_INLINE void MarkCard(const void *addr) {
    *CardFromAddr(addr) = kCardDirty;
  }

  // Is the object on a dirty card?
  bool IsDirty(const mirror::Object* obj) const {
    return GetCard(obj) == kCardDirty;
  }

  // Is the object on a clean card?
  bool IsClean(const mirror::Object* obj) const {
    return GetCard(obj) == kCardClean;
  }

  // Return the state of the card at an address.
  uint8_t GetCard(const mirror::Object* obj) const {
    return *CardFromAddr(obj);
  }

  // Visit and clear cards within memory range, only visits dirty cards.
  template <typename Visitor>
  void VisitClear(const void* start, const void* end, const Visitor& visitor) {
    uint8_t* card_start = CardFromAddr(start);
    uint8_t* card_end = CardFromAddr(end);
    for (uint8_t* it = card_start; it != card_end; ++it) {
      if (*it == kCardDirty) {
        *it = kCardClean;
        visitor(it);
      }
    }
  }

  // Returns a value that when added to a heap address >> `kCardShift` will address the appropriate
  // card table byte. For convenience this value is cached in every Thread.
  uint8_t* GetBiasedBegin() const {
    return biased_begin_;
  }

  void* MemMapBegin() const {
    return mem_map_.BaseBegin();
  }

  size_t MemMapSize() const {
    return mem_map_.BaseSize();
  }

  /*
   * Modify cards in the range from scan_begin (inclusive) to scan_end (exclusive). Each card
   * value v is replaced by visitor(v). Visitor() should not have side-effects.
   * Whenever a card value is changed, modified(card_address, old_value, new_value) is invoked.
   * For opportunistic performance reasons, this assumes that visitor(kCardClean) is kCardClean!
   */
  template <typename Visitor, typename ModifiedVisitor>
  void ModifyCardsAtomic(uint8_t* scan_begin,
                         uint8_t* scan_end,
                         const Visitor& visitor,
                         const ModifiedVisitor& modified);

  // For every dirty (at least minimum age) card between begin and end invoke
  // bitmap's VisitMarkedRange() to invoke 'visitor' on every object in the
  // card. Calls 'mod_visitor' for each such card in case the caller wants to
  // modify the value. Returns how many cards the visitor was run on.
  // NOTE: 'visitor' is called on one whole card at a time. Therefore,
  // 'scan_begin' and 'scan_end' are aligned to card-size before visitor is
  // called. Therefore visitor may get called on objects before 'scan_begin'
  // and/or after 'scan_end'. Visitor shall detect that and act appropriately.
  template <bool kClearCard, typename Visitor, typename ModifyVisitor>
  size_t Scan(SpaceBitmap<kObjectAlignment>* bitmap,
              uint8_t* scan_begin,
              uint8_t* scan_end,
              const Visitor& visitor,
              const ModifyVisitor& mod_visitor,
              const uint8_t minimum_age) REQUIRES(Locks::heap_bitmap_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template <bool kClearCard, typename Visitor>
  size_t Scan(SpaceBitmap<kObjectAlignment>* bitmap,
              uint8_t* scan_begin,
              uint8_t* scan_end,
              const Visitor& visitor,
              const uint8_t minimum_age = kCardDirty) REQUIRES(Locks::heap_bitmap_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    return Scan<kClearCard>(bitmap, scan_begin, scan_end, visitor, VoidFunctor(), minimum_age);
  }

  // Assertion used to check the given address is covered by the card table
  void CheckAddrIsInCardTable(const uint8_t* addr) const;

  // Resets all of the bytes in the card table to clean.
  void ClearCardTable();

  // Clear a range of cards that covers start to end, start and end must be aligned to kCardSize.
  void ClearCardRange(uint8_t* start, uint8_t* end);

  // Returns the first address in the heap which maps to this card.
  void* AddrFromCard(const uint8_t *card_addr) const ALWAYS_INLINE;

  // Returns the address of the relevant byte in the card table, given an address on the heap.
  uint8_t* CardFromAddr(const void *addr) const ALWAYS_INLINE;

  bool AddrIsInCardTable(const void* addr) const;

 private:
  CardTable(MemMap&& mem_map, uint8_t* biased_begin, size_t offset);

  // Returns true iff the card table address is within the bounds of the card table.
  bool IsValidCard(const uint8_t* card_addr) const ALWAYS_INLINE;

  void CheckCardValid(uint8_t* card) const ALWAYS_INLINE;

  // Verifies that all gray objects are on a dirty card.
  void VerifyCardTable();

  // Mmapped pages for the card table
  MemMap mem_map_;
  // Value used to compute card table addresses from object addresses, see GetBiasedBegin
  uint8_t* const biased_begin_;
  // Card table doesn't begin at the beginning of the mem_map_, instead it is displaced by offset
  // to allow the byte value of `biased_begin_` to equal `kCardDirty`.
  const size_t offset_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(CardTable);
};

}  // namespace accounting

class AgeCardVisitor {
 public:
  uint8_t operator()(uint8_t card) const {
    return (card == accounting::CardTable::kCardDirty) ? accounting::CardTable::kCardAged
                                                       : accounting::CardTable::kCardClean;
  }
};

}  // namespace gc
}  // namespace art

#endif  // ART_RUNTIME_GC_ACCOUNTING_CARD_TABLE_H_
