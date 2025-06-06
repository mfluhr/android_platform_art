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

#ifndef ART_RUNTIME_GC_ACCOUNTING_CARD_TABLE_INL_H_
#define ART_RUNTIME_GC_ACCOUNTING_CARD_TABLE_INL_H_

#include "card_table.h"

#include <android-base/logging.h>

#include "base/atomic.h"
#include "base/bit_utils.h"
#include "base/mem_map.h"
#include "space_bitmap.h"

namespace art HIDDEN {
namespace gc {
namespace accounting {

static inline bool byte_cas(uint8_t old_value, uint8_t new_value, uint8_t* address) {
#if defined(__i386__) || defined(__x86_64__)
  Atomic<uint8_t>* byte_atomic = reinterpret_cast<Atomic<uint8_t>*>(address);
  return byte_atomic->CompareAndSetWeakRelaxed(old_value, new_value);
#else
  // Little endian means most significant byte is on the left.
  const size_t shift_in_bytes = reinterpret_cast<uintptr_t>(address) % sizeof(uintptr_t);
  // Align the address down.
  address -= shift_in_bytes;
  const size_t shift_in_bits = shift_in_bytes * kBitsPerByte;
  Atomic<uintptr_t>* word_atomic = reinterpret_cast<Atomic<uintptr_t>*>(address);

  // Word with the byte we are trying to cas cleared.
  const uintptr_t cur_word = word_atomic->load(std::memory_order_relaxed) &
      ~(static_cast<uintptr_t>(0xFF) << shift_in_bits);
  const uintptr_t old_word = cur_word | (static_cast<uintptr_t>(old_value) << shift_in_bits);
  const uintptr_t new_word = cur_word | (static_cast<uintptr_t>(new_value) << shift_in_bits);
  return word_atomic->CompareAndSetWeakRelaxed(old_word, new_word);
#endif
}

template <bool kClearCard, typename Visitor, typename ModifyVisitor>
inline size_t CardTable::Scan(ContinuousSpaceBitmap* bitmap,
                              uint8_t* const scan_begin,
                              uint8_t* const scan_end,
                              const Visitor& visitor,
                              const ModifyVisitor& mod_visitor,
                              const uint8_t minimum_age) {
  DCHECK_GE(scan_begin, reinterpret_cast<uint8_t*>(bitmap->HeapBegin()));
  // scan_end is the byte after the last byte we scan.
  DCHECK_LE(scan_end, reinterpret_cast<uint8_t*>(bitmap->HeapLimit()));
  uint8_t* const card_begin = CardFromAddr(scan_begin);
  uint8_t* const card_end = CardFromAddr(AlignUp(scan_end, kCardSize));
  uint8_t* card_cur = card_begin;
  CheckCardValid(card_cur);
  CheckCardValid(card_end);
  size_t cards_scanned = 0;

  // Handle any unaligned cards at the start.
  while (!IsAligned<sizeof(intptr_t)>(card_cur) && card_cur < card_end) {
    uint8_t cur_val = *card_cur;
    if (cur_val >= minimum_age) {
      uintptr_t start = reinterpret_cast<uintptr_t>(AddrFromCard(card_cur));
      bitmap->VisitMarkedRange(start, start + kCardSize, visitor);
      mod_visitor(card_cur, cur_val);
      ++cards_scanned;
    }
    ++card_cur;
  }

  if (card_cur < card_end) {
    DCHECK_ALIGNED(card_cur, sizeof(intptr_t));
    uint8_t* aligned_end = card_end -
        (reinterpret_cast<uintptr_t>(card_end) & (sizeof(uintptr_t) - 1));
    DCHECK_LE(card_cur, aligned_end);

    uintptr_t* word_end = reinterpret_cast<uintptr_t*>(aligned_end);
    for (uintptr_t* word_cur = reinterpret_cast<uintptr_t*>(card_cur); word_cur < word_end;
        ++word_cur) {
      while (LIKELY(*word_cur == 0)) {
        ++word_cur;
        if (UNLIKELY(word_cur >= word_end)) {
          goto exit_for;
        }
      }

      // Find the first dirty card.
      uintptr_t start_word = *word_cur;
      uintptr_t start =
          reinterpret_cast<uintptr_t>(AddrFromCard(reinterpret_cast<uint8_t*>(word_cur)));
      // TODO: Investigate if processing continuous runs of dirty cards with
      // a single bitmap visit is more efficient.
      for (size_t i = 0; i < sizeof(uintptr_t); ++i) {
        uint8_t cur_val = static_cast<uint8_t>(start_word);
        if (cur_val >= minimum_age) {
          auto* card = reinterpret_cast<uint8_t*>(word_cur) + i;
          DCHECK(*card == static_cast<uint8_t>(start_word) || *card == kCardDirty)
              << "card " << static_cast<size_t>(*card) << " intptr_t " << cur_val;
          bitmap->VisitMarkedRange(start, start + kCardSize, visitor);
          mod_visitor(card, cur_val);
          ++cards_scanned;
        }
        start_word >>= 8;
        start += kCardSize;
      }
    }
    exit_for:

    // Handle any unaligned cards at the end.
    card_cur = reinterpret_cast<uint8_t*>(word_end);
    while (card_cur < card_end) {
      uint8_t cur_val = *card_cur;
      if (cur_val >= minimum_age) {
        uintptr_t start = reinterpret_cast<uintptr_t>(AddrFromCard(card_cur));
        bitmap->VisitMarkedRange(start, start + kCardSize, visitor);
        mod_visitor(card_cur, cur_val);
        ++cards_scanned;
      }
      ++card_cur;
    }
  }

  if (kClearCard) {
    ClearCardRange(scan_begin, scan_end);
  }

  return cards_scanned;
}

template <typename Visitor, typename ModifiedVisitor>
inline void CardTable::ModifyCardsAtomic(uint8_t* scan_begin,
                                         uint8_t* scan_end,
                                         const Visitor& visitor,
                                         const ModifiedVisitor& modified) {
  uint8_t* card_cur = CardFromAddr(scan_begin);
  uint8_t* card_end = CardFromAddr(AlignUp(scan_end, kCardSize));
  CheckCardValid(card_cur);
  CheckCardValid(card_end);
  DCHECK(visitor(kCardClean) == kCardClean);

  // Handle any unaligned cards at the start.
  while (!IsAligned<sizeof(intptr_t)>(card_cur) && card_cur < card_end) {
    uint8_t expected, new_value;
    do {
      expected = *card_cur;
      new_value = visitor(expected);
    } while (expected != new_value && UNLIKELY(!byte_cas(expected, new_value, card_cur)));
    if (expected != new_value) {
      modified(card_cur, expected, new_value);
    }
    ++card_cur;
  }

  // Handle unaligned cards at the end.
  while (!IsAligned<sizeof(intptr_t)>(card_end) && card_end > card_cur) {
    --card_end;
    uint8_t expected, new_value;
    do {
      expected = *card_end;
      new_value = visitor(expected);
    } while (expected != new_value && UNLIKELY(!byte_cas(expected, new_value, card_end)));
    if (expected != new_value) {
      modified(card_end, expected, new_value);
    }
  }

  // Now we have the words, we can process words in parallel.
  uintptr_t* word_cur = reinterpret_cast<uintptr_t*>(card_cur);
  uintptr_t* word_end = reinterpret_cast<uintptr_t*>(card_end);
  // TODO: This is not big endian safe.
  union {
    uintptr_t expected_word;
    uint8_t expected_bytes[sizeof(uintptr_t)];
  };
  union {
    uintptr_t new_word;
    uint8_t new_bytes[sizeof(uintptr_t)];
  };

  // TODO: Parallelize.
  while (word_cur < word_end) {
    while (true) {
      expected_word = *word_cur;
      static_assert(kCardClean == 0);
      if (LIKELY(expected_word == 0 /* All kCardClean */ )) {
        break;
      }
      for (size_t i = 0; i < sizeof(uintptr_t); ++i) {
        new_bytes[i] = visitor(expected_bytes[i]);
      }
      Atomic<uintptr_t>* atomic_word = reinterpret_cast<Atomic<uintptr_t>*>(word_cur);
      if (LIKELY(atomic_word->CompareAndSetWeakRelaxed(expected_word, new_word))) {
        for (size_t i = 0; i < sizeof(uintptr_t); ++i) {
          const uint8_t expected_byte = expected_bytes[i];
          const uint8_t new_byte = new_bytes[i];
          if (expected_byte != new_byte) {
            modified(reinterpret_cast<uint8_t*>(word_cur) + i, expected_byte, new_byte);
          }
        }
        break;
      }
    }
    ++word_cur;
  }
}

inline void* CardTable::AddrFromCard(const uint8_t *card_addr) const {
  DCHECK(IsValidCard(card_addr))
    << " card_addr: " << reinterpret_cast<const void*>(card_addr)
    << " begin: " << reinterpret_cast<void*>(mem_map_.Begin() + offset_)
    << " end: " << reinterpret_cast<void*>(mem_map_.End());
  uintptr_t offset = card_addr - biased_begin_;
  return reinterpret_cast<void*>(offset << kCardShift);
}

inline uint8_t* CardTable::CardFromAddr(const void *addr) const {
  uint8_t *card_addr = biased_begin_ + (reinterpret_cast<uintptr_t>(addr) >> kCardShift);
  // Check that the caller was asking for an address covered by the card table.
  DCHECK(IsValidCard(card_addr)) << "addr: " << addr
      << " card_addr: " << reinterpret_cast<void*>(card_addr);
  return card_addr;
}

inline bool CardTable::IsValidCard(const uint8_t* card_addr) const {
  uint8_t* begin = mem_map_.Begin() + offset_;
  uint8_t* end = mem_map_.End();
  return card_addr >= begin && card_addr < end;
}

inline void CardTable::CheckCardValid(uint8_t* card) const {
  DCHECK(IsValidCard(card))
      << " card_addr: " << reinterpret_cast<const void*>(card)
      << " begin: " << reinterpret_cast<void*>(mem_map_.Begin() + offset_)
      << " end: " << reinterpret_cast<void*>(mem_map_.End());
}

}  // namespace accounting
}  // namespace gc
}  // namespace art

#endif  // ART_RUNTIME_GC_ACCOUNTING_CARD_TABLE_INL_H_
