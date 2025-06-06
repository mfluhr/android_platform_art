/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "source_transform.h"

#include <inttypes.h>

#include <memory>

#include <android-base/logging.h>

#include "dex/code_item_accessors-inl.h"
#include "dex/art_dex_file_loader.h"
#include "dex/class_accessor-inl.h"
#include "dex/dex_file.h"
#include "dex/dex_file_loader.h"
#include "dex/dex_instruction.h"

namespace art {
namespace Test983SourceTransformVerify {

// The hook we are using.
void VerifyClassData(jint class_data_len, const unsigned char* class_data) {
  // Due to b/72402467 the class_data_len might just be an estimate.
  CHECK_GE(static_cast<size_t>(class_data_len), sizeof(DexFile::Header));
  const DexFile::Header* header = reinterpret_cast<const DexFile::Header*>(class_data);
  uint32_t header_file_size = header->file_size_;
  CHECK_LE(static_cast<jint>(header_file_size), class_data_len);
  class_data_len = static_cast<jint>(header_file_size);

  ArtDexFileLoader dex_file_loader(class_data, class_data_len, "fake_location.dex");
  std::string error;
  std::unique_ptr<const DexFile> dex(dex_file_loader.Open(/*location_checksum*/ 0,
                                                          /*oat_dex_file*/ nullptr,
                                                          /*verify*/ true,
                                                          /*verify_checksum*/ true,
                                                          &error));
  CHECK(dex.get() != nullptr) << "Failed to verify dex: " << error;

  for (ClassAccessor accessor : dex->GetClasses()) {
    for (const ClassAccessor::Method& method : accessor.GetMethods()) {
      for (const DexInstructionPcPair& pair : method.GetInstructions()) {
        const Instruction& inst = pair.Inst();
        int forbidden_flags = Instruction::kVerifyError;
        if ((inst.GetVerifyExtraFlags() & forbidden_flags) != 0) {
          LOG(FATAL) << "Unexpected instruction found in " << dex->PrettyMethod(method.GetIndex())
                     << " [Dex PC: 0x" << std::hex << pair.DexPc() << std::dec << "] : "
                     << inst.DumpString(dex.get()) << std::endl;
        }
      }
    }
  }
}

}  // namespace Test983SourceTransformVerify
}  // namespace art
