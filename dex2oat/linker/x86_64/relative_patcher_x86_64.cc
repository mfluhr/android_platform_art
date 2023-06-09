/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "linker/x86_64/relative_patcher_x86_64.h"

#include "linker/linker_patch.h"

namespace art {
namespace linker {

void X86_64RelativePatcher::PatchPcRelativeReference(std::vector<uint8_t>* code,
                                                     const LinkerPatch& patch,
                                                     uint32_t patch_offset,
                                                     uint32_t target_offset) {
  DCHECK_LE(patch.LiteralOffset() + 4u, code->size());
  // Unsigned arithmetic with its well-defined overflow behavior is just fine here.
  uint32_t displacement = target_offset - patch_offset;
  displacement -= kPcDisplacement;  // The base PC is at the end of the 4-byte patch.

  using unaligned_int32_t __attribute__((__aligned__(1))) = int32_t;
  reinterpret_cast<unaligned_int32_t*>(&(*code)[patch.LiteralOffset()])[0] = displacement;
}

void X86_64RelativePatcher::PatchEntrypointCall([[maybe_unused]] std::vector<uint8_t>* code,
                                                [[maybe_unused]] const LinkerPatch& patch,
                                                [[maybe_unused]] uint32_t patch_offset) {
  LOG(FATAL) << "UNIMPLEMENTED";
}

void X86_64RelativePatcher::PatchBakerReadBarrierBranch([[maybe_unused]] std::vector<uint8_t>* code,
                                                        [[maybe_unused]] const LinkerPatch& patch,
                                                        [[maybe_unused]] uint32_t patch_offset) {
  LOG(FATAL) << "UNIMPLEMENTED";
}

}  // namespace linker
}  // namespace art
