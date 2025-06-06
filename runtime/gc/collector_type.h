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

#ifndef ART_RUNTIME_GC_COLLECTOR_TYPE_H_
#define ART_RUNTIME_GC_COLLECTOR_TYPE_H_

#include <iosfwd>

#include "base/macros.h"

namespace art HIDDEN {
namespace gc {

// Which types of collections are able to be performed.
enum CollectorType {
  // No collector selected.
  kCollectorTypeNone,
  // Non concurrent mark-sweep.
  kCollectorTypeMS,
  // Concurrent mark-sweep.
  kCollectorTypeCMS,
  // Concurrent mark-compact.
  kCollectorTypeCMC,
  // The background compaction of the Concurrent mark-compact GC.
  kCollectorTypeCMCBackground,
  // Semi-space / mark-sweep hybrid, enables compaction.
  kCollectorTypeSS,
  // Heap trimming collector, doesn't do any actual collecting.
  kCollectorTypeHeapTrim,
  // A (mostly) concurrent copying collector.
  kCollectorTypeCC,
  // The background compaction of the concurrent copying collector.
  kCollectorTypeCCBackground,
  // Instrumentation critical section fake collector.
  kCollectorTypeInstrumentation,
  // Fake collector for adding or removing application image spaces.
  kCollectorTypeAddRemoveAppImageSpace,
  // Fake collector used to implement exclusion between GC and debugger.
  kCollectorTypeDebugger,
  // A homogeneous space compaction collector used in background transition
  // when both foreground and background collector are CMS.
  kCollectorTypeHomogeneousSpaceCompact,
  // Class linker fake collector.
  kCollectorTypeClassLinker,
  // JIT Code cache fake collector.
  kCollectorTypeJitCodeCache,
  // Hprof fake collector.
  kCollectorTypeHprof,
  // Fake collector for installing/removing a system-weak holder.
  kCollectorTypeAddRemoveSystemWeakHolder,
  // Fake collector type for GetObjectsAllocated
  kCollectorTypeGetObjectsAllocated,
  // Fake collector type for ScopedGCCriticalSection
  kCollectorTypeCriticalSection,
};
std::ostream& operator<<(std::ostream& os, CollectorType collector_type);

static constexpr CollectorType kCollectorTypeDefault =
#if ART_DEFAULT_GC_TYPE_IS_CMC
    kCollectorTypeCMC
#elif ART_DEFAULT_GC_TYPE_IS_SS
    kCollectorTypeSS
#elif ART_DEFAULT_GC_TYPE_IS_CMS
    kCollectorTypeCMS
#elif ART_DEFAULT_GC_TYPE_IS_MS
    kCollectorTypeMS
#else
#error "ART default GC type must be set"
#endif
    ;  // NOLINT [whitespace/semicolon] [5]

}  // namespace gc
}  // namespace art

#endif  // ART_RUNTIME_GC_COLLECTOR_TYPE_H_
