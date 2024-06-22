/*
 * Copyright 2024 The Android Open Source Project
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

#ifndef ART_RUNTIME_JIT_JIT_CODE_CACHE_INL_H_
#define ART_RUNTIME_JIT_JIT_CODE_CACHE_INL_H_

#include "gc_root.h"
#include "jit/jit_code_cache.h"
#include "thread.h"

namespace art HIDDEN {

class ArtMethod;

namespace jit {

template<typename RootVisitorType>
EXPORT void JitCodeCache::VisitRootTables(RootVisitorType& visitor) {
  Thread* self = Thread::Current();
  ScopedDebugDisallowReadBarriers sddrb(self);
  MutexLock mu(self, *Locks::jit_lock_);

  for (auto& [_, method_types] : method_types_map_) {
    for (auto& method_type : method_types) {
      visitor.VisitRoot(method_type.AddressWithoutBarrier());
    }
  }
}

}  // namespace jit
}  // namespace art

#endif  // ART_RUNTIME_JIT_JIT_CODE_CACHE_INL_H_


