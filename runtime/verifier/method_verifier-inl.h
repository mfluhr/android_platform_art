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

#ifndef ART_RUNTIME_VERIFIER_METHOD_VERIFIER_INL_H_
#define ART_RUNTIME_VERIFIER_METHOD_VERIFIER_INL_H_

#include "method_verifier.h"

namespace art HIDDEN {
namespace verifier {

inline RegisterLine* MethodVerifier::GetRegLine(uint32_t dex_pc) {
  return reg_table_.GetLine(dex_pc);
}

inline const InstructionFlags& MethodVerifier::GetInstructionFlags(size_t index) const {
  return insn_flags_[index];
}

inline MethodReference MethodVerifier::GetMethodReference() const {
  return MethodReference(dex_file_, dex_method_idx_);
}

inline bool MethodVerifier::HasFailures() const {
  return !failures_.empty();
}

}  // namespace verifier
}  // namespace art

#endif  // ART_RUNTIME_VERIFIER_METHOD_VERIFIER_INL_H_
