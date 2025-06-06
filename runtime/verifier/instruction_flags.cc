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

#include "instruction_flags.h"

#include <string.h>

namespace art HIDDEN {
namespace verifier {

std::string InstructionFlags::ToString() const {
  char encoding[7];
  if (!IsOpcode()) {
    strncpy(encoding, "XXXXXX", sizeof(encoding));
  } else {
    strncpy(encoding, "------", sizeof(encoding));
    if (IsVisited())               encoding[kVisited] = 'V';
    if (IsChanged())               encoding[kChanged] = 'C';
    if (IsOpcode())                encoding[kOpcode] = 'O';
    if (IsInTry())                 encoding[kInTry] = 'T';
    if (IsBranchTarget())          encoding[kBranchTarget] = 'B';
    if (IsReturn())                encoding[kReturn] = 'R';
  }
  return encoding;
}

}  // namespace verifier
}  // namespace art
