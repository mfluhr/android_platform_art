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

#ifndef ART_RUNTIME_GC_SPACE_DLMALLOC_SPACE_H_
#define ART_RUNTIME_GC_SPACE_DLMALLOC_SPACE_H_

#include "malloc_space.h"
#include "space.h"

namespace art HIDDEN {
namespace gc {

namespace collector {
class MarkSweep;
}  // namespace collector

namespace space {

// An alloc space is a space where objects may be allocated and garbage collected. Not final as may
// be overridden by a MemoryToolMallocSpace.
class DlMallocSpace : public MallocSpace {
 public:
  // Create a DlMallocSpace from an existing mem_map.
  static DlMallocSpace* CreateFromMemMap(MemMap&& mem_map,
                                         const std::string& name,
                                         size_t starting_size,
                                         size_t initial_size,
                                         size_t growth_limit,
                                         size_t capacity,
                                         bool can_move_objects);

  // Create a DlMallocSpace with the requested sizes. The requested
  // base address is not guaranteed to be granted, if it is required,
  // the caller should call Begin on the returned space to confirm the
  // request was granted.
  static DlMallocSpace* Create(const std::string& name,
                               size_t initial_size,
                               size_t growth_limit,
                               size_t capacity,
                               bool can_move_objects);

  // Virtual to allow MemoryToolMallocSpace to intercept.
  mirror::Object* AllocWithGrowth(Thread* self,
                                  size_t num_bytes,
                                  size_t* bytes_allocated,
                                  size_t* usable_size,
                                  size_t* bytes_tl_bulk_allocated) override REQUIRES(!lock_);
  // Virtual to allow MemoryToolMallocSpace to intercept.
  mirror::Object* Alloc(Thread* self,
                        size_t num_bytes,
                        size_t* bytes_allocated,
                        size_t* usable_size,
                        size_t* bytes_tl_bulk_allocated) override REQUIRES(!lock_) {
    return AllocNonvirtual(self, num_bytes, bytes_allocated, usable_size,
                           bytes_tl_bulk_allocated);
  }
  // Virtual to allow MemoryToolMallocSpace to intercept.
  size_t AllocationSize(mirror::Object* obj, size_t* usable_size) override {
    return AllocationSizeNonvirtual(obj, usable_size);
  }
  // Virtual to allow MemoryToolMallocSpace to intercept.
  size_t Free(Thread* self, mirror::Object* ptr) override
      REQUIRES(!lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);
  // Virtual to allow MemoryToolMallocSpace to intercept.
  size_t FreeList(Thread* self, size_t num_ptrs, mirror::Object** ptrs) override
      REQUIRES(!lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);

  size_t MaxBytesBulkAllocatedFor(size_t num_bytes) override {
    return num_bytes;
  }

  // DlMallocSpaces don't have thread local state.
  size_t RevokeThreadLocalBuffers(art::Thread*) override {
    return 0U;
  }
  size_t RevokeAllThreadLocalBuffers() override {
    return 0U;
  }

  // Faster non-virtual allocation path.
  mirror::Object* AllocNonvirtual(Thread* self, size_t num_bytes, size_t* bytes_allocated,
                                  size_t* usable_size, size_t* bytes_tl_bulk_allocated)
      REQUIRES(!lock_);

  // Faster non-virtual allocation size path.
  size_t AllocationSizeNonvirtual(mirror::Object* obj, size_t* usable_size);

#ifndef NDEBUG
  // Override only in the debug build.
  void CheckMoreCoreForPrecondition() override;
#endif

  void* GetMspace() const {
    return mspace_;
  }

  size_t Trim() override;

  // Perform a mspace_inspect_all which calls back for each allocation chunk. The chunk may not be
  // in use, indicated by num_bytes equaling zero.
  void Walk(WalkCallback callback, void* arg) override REQUIRES(!lock_);

  // Returns the number of bytes that the space has currently obtained from the system. This is
  // greater or equal to the amount of live data in the space.
  size_t GetFootprint() override;

  // Returns the number of bytes that the heap is allowed to obtain from the system via MoreCore.
  size_t GetFootprintLimit() override;

  // Set the maximum number of bytes that the heap is allowed to obtain from the system via
  // MoreCore. Note this is used to stop the mspace growing beyond the limit to Capacity. When
  // allocations fail we GC before increasing the footprint limit and allowing the mspace to grow.
  void SetFootprintLimit(size_t limit) override;

  MallocSpace* CreateInstance(MemMap&& mem_map,
                              const std::string& name,
                              void* allocator,
                              uint8_t* begin,
                              uint8_t* end,
                              uint8_t* limit,
                              size_t growth_limit,
                              bool can_move_objects) override;

  uint64_t GetBytesAllocated() override;
  uint64_t GetObjectsAllocated() override;

  void Clear() override;

  bool IsDlMallocSpace() const override {
    return true;
  }

  DlMallocSpace* AsDlMallocSpace() override {
    return this;
  }

  bool LogFragmentationAllocFailure(std::ostream& os, size_t failed_alloc_bytes) override
      REQUIRES_SHARED(Locks::mutator_lock_);

 protected:
  DlMallocSpace(MemMap&& mem_map,
                size_t initial_size,
                const std::string& name,
                void* mspace,
                uint8_t* begin,
                uint8_t* end,
                uint8_t* limit,
                size_t growth_limit,
                bool can_move_objects,
                size_t starting_size);

 private:
  mirror::Object* AllocWithoutGrowthLocked(Thread* self, size_t num_bytes, size_t* bytes_allocated,
                                           size_t* usable_size,
                                           size_t* bytes_tl_bulk_allocated)
      REQUIRES(lock_);

  void* CreateAllocator(void* base, size_t morecore_start, size_t initial_size,
                        size_t /*maximum_size*/, bool /*low_memory_mode*/) override {
    return CreateMspace(base, morecore_start, initial_size);
  }
  static void* CreateMspace(void* base, size_t morecore_start, size_t initial_size);

  // The boundary tag overhead.
  static const size_t kChunkOverhead = sizeof(intptr_t);

  // Underlying malloc space.
  void* mspace_;

  friend class collector::MarkSweep;

  DISALLOW_COPY_AND_ASSIGN(DlMallocSpace);
};

}  // namespace space

namespace allocator {

// Callback from dlmalloc when it needs to increase the footprint.
// Must be implemented outside of art-dlmalloc.cc.
void* ArtDlMallocMoreCore(void* mspace, intptr_t increment);

}  // namespace allocator
}  // namespace gc
}  // namespace art

#endif  // ART_RUNTIME_GC_SPACE_DLMALLOC_SPACE_H_
