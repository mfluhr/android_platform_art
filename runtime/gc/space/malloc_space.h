/*
 * Copyright (C) 2013 The Android Open Source Project
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

#ifndef ART_RUNTIME_GC_SPACE_MALLOC_SPACE_H_
#define ART_RUNTIME_GC_SPACE_MALLOC_SPACE_H_

#include "space.h"

#include <iosfwd>

#include "base/memory_tool.h"
#include "base/mutex.h"

namespace art HIDDEN {
namespace gc {

namespace collector {
class MarkSweep;
}  // namespace collector

namespace space {

class ZygoteSpace;

// A common parent of DlMallocSpace and RosAllocSpace.
class MallocSpace : public ContinuousMemMapAllocSpace {
 public:
  using WalkCallback = void (*)(void *start, void *end, size_t num_bytes, void* callback_arg);

  SpaceType GetType() const override {
    return kSpaceTypeMallocSpace;
  }

  // Allocate num_bytes allowing the underlying space to grow.
  virtual mirror::Object* AllocWithGrowth(Thread* self, size_t num_bytes,
                                          size_t* bytes_allocated, size_t* usable_size,
                                          size_t* bytes_tl_bulk_allocated) = 0;
  // Allocate num_bytes without allowing the underlying space to grow.
  mirror::Object* Alloc(Thread* self, size_t num_bytes, size_t* bytes_allocated,
                        size_t* usable_size, size_t* bytes_tl_bulk_allocated) override = 0;
  // Return the storage space required by obj. If usable_size isn't null then it is set to the
  // amount of the storage space that may be used by obj.
  size_t AllocationSize(mirror::Object* obj, size_t* usable_size) override = 0;
  size_t Free(Thread* self, mirror::Object* ptr) override
      REQUIRES_SHARED(Locks::mutator_lock_) = 0;
  size_t FreeList(Thread* self, size_t num_ptrs, mirror::Object** ptrs) override
      REQUIRES_SHARED(Locks::mutator_lock_) = 0;

  // Returns the maximum bytes that could be allocated for the given
  // size in bulk, that is the maximum value for the
  // bytes_allocated_bulk out param returned by MallocSpace::Alloc().
  virtual size_t MaxBytesBulkAllocatedFor(size_t num_bytes) = 0;

#ifndef NDEBUG
  virtual void CheckMoreCoreForPrecondition() {}  // to be overridden in the debug build.
#else
  void CheckMoreCoreForPrecondition() {}  // no-op in the non-debug build.
#endif

  void* MoreCore(intptr_t increment);

  // Hands unused pages back to the system.
  virtual size_t Trim() = 0;

  // Perform a mspace_inspect_all which calls back for each allocation chunk. The chunk may not be
  // in use, indicated by num_bytes equaling zero.
  virtual void Walk(WalkCallback callback, void* arg) = 0;

  // Returns the number of bytes that the space has currently obtained from the system. This is
  // greater or equal to the amount of live data in the space.
  virtual size_t GetFootprint() = 0;

  // Returns the number of bytes that the heap is allowed to obtain from the system via MoreCore.
  virtual size_t GetFootprintLimit() = 0;

  // Set the maximum number of bytes that the heap is allowed to obtain from the system via
  // MoreCore. Note this is used to stop the mspace growing beyond the limit to Capacity. When
  // allocations fail we GC before increasing the footprint limit and allowing the mspace to grow.
  virtual void SetFootprintLimit(size_t limit) = 0;

  // Removes the fork time growth limit on capacity, allowing the application to allocate up to the
  // maximum reserved size of the heap.
  void ClearGrowthLimit() {
    growth_limit_ = NonGrowthLimitCapacity();
  }

  // Override capacity so that we only return the possibly limited capacity
  size_t Capacity() const override {
    return growth_limit_;
  }

  // The total amount of memory reserved for the alloc space.
  size_t NonGrowthLimitCapacity() const override {
    return GetMemMap()->Size();
  }

  // Change the non growth limit capacity by shrinking or expanding the map. Currently, only
  // shrinking is supported.
  void ClampGrowthLimit();

  void Dump(std::ostream& os) const override;

  void SetGrowthLimit(size_t growth_limit);

  virtual MallocSpace* CreateInstance(MemMap&& mem_map,
                                      const std::string& name,
                                      void* allocator,
                                      uint8_t* begin,
                                      uint8_t* end,
                                      uint8_t* limit,
                                      size_t growth_limit,
                                      bool can_move_objects) = 0;

  // Splits ourself into a zygote space and new malloc space which has our unused memory. When true,
  // the low memory mode argument specifies that the heap wishes the created space to be more
  // aggressive in releasing unused pages. Invalidates the space its called on.
  ZygoteSpace* CreateZygoteSpace(const char* alloc_space_name, bool low_memory_mode,
                                 MallocSpace** out_malloc_space) NO_THREAD_SAFETY_ANALYSIS;
  uint64_t GetBytesAllocated() override = 0;
  uint64_t GetObjectsAllocated() override = 0;

  // Returns the class of a recently freed object.
  mirror::Class* FindRecentFreedObject(const mirror::Object* obj);

  bool CanMoveObjects() const override {
    return can_move_objects_;
  }

  void DisableMovingObjects() {
    can_move_objects_ = false;
  }

 protected:
  MallocSpace(const std::string& name,
              MemMap&& mem_map,
              uint8_t* begin,
              uint8_t* end,
              uint8_t* limit,
              size_t growth_limit,
              bool create_bitmaps,
              bool can_move_objects,
              size_t starting_size,
              size_t initial_size);

  static MemMap CreateMemMap(const std::string& name,
                             size_t starting_size,
                             size_t* initial_size,
                             size_t* growth_limit,
                             size_t* capacity);

  // When true the low memory mode argument specifies that the heap wishes the created allocator to
  // be more aggressive in releasing unused pages.
  virtual void* CreateAllocator(void* base, size_t morecore_start, size_t initial_size,
                                size_t maximum_size, bool low_memory_mode) = 0;

  virtual void RegisterRecentFree(mirror::Object* ptr)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(lock_);

  accounting::ContinuousSpaceBitmap::SweepCallback* GetSweepCallback() override {
    return &SweepCallback;
  }

  // Recent allocation buffer.
  static constexpr size_t kRecentFreeCount = kDebugSpaces ? (1 << 16) : 0;
  static constexpr size_t kRecentFreeMask = kRecentFreeCount - 1;
  std::pair<const mirror::Object*, mirror::Class*> recent_freed_objects_[kRecentFreeCount];
  size_t recent_free_pos_;

  static size_t bitmap_index_;

  // Used to ensure mutual exclusion when the allocation spaces data structures are being modified.
  Mutex lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;

  // The capacity of the alloc space until such time that ClearGrowthLimit is called.
  // The underlying mem_map_ controls the maximum size we allow the heap to grow to. The growth
  // limit is a value <= to the mem_map_ capacity used for ergonomic reasons because of the zygote.
  // Prior to forking the zygote the heap will have a maximally sized mem_map_ but the growth_limit_
  // will be set to a lower value. The growth_limit_ is used as the capacity of the alloc_space_,
  // however, capacity normally can't vary. In the case of the growth_limit_ it can be cleared
  // one time by a call to ClearGrowthLimit.
  size_t growth_limit_;

  // True if objects in the space are movable.
  bool can_move_objects_;

  // Starting and initial sized, used when you reset the space.
  const size_t starting_size_;
  const size_t initial_size_;

 private:
  static void SweepCallback(size_t num_ptrs, mirror::Object** ptrs, void* arg)
      REQUIRES_SHARED(Locks::mutator_lock_);

  DISALLOW_COPY_AND_ASSIGN(MallocSpace);
};

}  // namespace space
}  // namespace gc
}  // namespace art

#endif  // ART_RUNTIME_GC_SPACE_MALLOC_SPACE_H_
