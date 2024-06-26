/*
 * Copyright (C) 2014 The Android Open Source Project
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

#ifndef ART_RUNTIME_GC_ACCOUNTING_REMEMBERED_SET_H_
#define ART_RUNTIME_GC_ACCOUNTING_REMEMBERED_SET_H_

#include "base/allocator.h"
#include "base/locks.h"
#include "base/safe_map.h"
#include "runtime_globals.h"

#include <set>
#include <vector>

namespace art HIDDEN {
namespace gc {

namespace collector {
class GarbageCollector;
class MarkSweep;
}  // namespace collector
namespace space {
class ContinuousSpace;
}  // namespace space

class Heap;

namespace accounting {

// The remembered set keeps track of cards that may contain references
// from the free list spaces to the bump pointer spaces.
class RememberedSet {
 public:
  using CardSet = std::set<uint8_t*,
                           std::less<uint8_t*>,
                           TrackingAllocator<uint8_t*, kAllocatorTagRememberedSet>>;

  explicit RememberedSet(const std::string& name, Heap* heap, space::ContinuousSpace* space)
      : name_(name), heap_(heap), space_(space) {}

  // Clear dirty cards and add them to the dirty card set.
  void ClearCards();

  // Mark through all references to the target space.
  void UpdateAndMarkReferences(space::ContinuousSpace* target_space,
                               collector::GarbageCollector* collector)
      REQUIRES(Locks::heap_bitmap_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);

  void Dump(std::ostream& os);

  space::ContinuousSpace* GetSpace() {
    return space_;
  }
  Heap* GetHeap() const {
    return heap_;
  }
  const std::string& GetName() const {
    return name_;
  }
  void AssertAllDirtyCardsAreWithinSpace() const;

 private:
  const std::string name_;
  Heap* const heap_;
  space::ContinuousSpace* const space_;

  CardSet dirty_cards_;
};

}  // namespace accounting
}  // namespace gc
}  // namespace art

#endif  // ART_RUNTIME_GC_ACCOUNTING_REMEMBERED_SET_H_
