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

#include "code_item_accessors-inl.h"

#include <sys/mman.h>
#include <memory>
#include <vector>

#include "base/mem_map.h"
#include "dex_file_loader.h"
#include "gtest/gtest.h"

namespace art {

class CodeItemAccessorsTest : public ::testing::Test {};

std::unique_ptr<const DexFile> CreateFakeDex(bool compact_dex, std::vector<uint8_t>* data) {
  data->resize(MemMap::GetPageSize());
  if (compact_dex) {
    CompactDexFile::Header* header =
        const_cast<CompactDexFile::Header*>(CompactDexFile::Header::At(data->data()));
    CompactDexFile::WriteMagic(header->magic_.data());
    CompactDexFile::WriteCurrentVersion(header->magic_.data());
    header->data_off_ = 0;
    header->data_size_ = data->size();
    header->file_size_ = data->size();
  } else {
    auto* header = reinterpret_cast<DexFile::Header*>(data->data());
    StandardDexFile::WriteMagic(data->data());
    StandardDexFile::WriteCurrentVersion(data->data());
    header->header_size_ = sizeof(*header);
    header->file_size_ = data->size();
  }
  DexFileLoader dex_file_loader(data->data(), data->size(), "location");
  std::string error_msg;
  std::unique_ptr<const DexFile> dex(dex_file_loader.Open(/*location_checksum=*/123,
                                                          /*oat_dex_file=*/nullptr,
                                                          /*verify=*/false,
                                                          /*verify_checksum=*/false,
                                                          &error_msg));
  CHECK(dex != nullptr) << error_msg;
  return dex;
}

TEST(CodeItemAccessorsTest, TestDexInstructionsAccessor) {
  std::vector<uint8_t> standard_dex_data;
  std::unique_ptr<const DexFile> standard_dex(CreateFakeDex(/*compact_dex=*/false,
                                                            &standard_dex_data));
  ASSERT_TRUE(standard_dex != nullptr);
  static constexpr uint16_t kRegisterSize = 2;
  static constexpr uint16_t kInsSize = 1;
  static constexpr uint16_t kOutsSize = 3;
  static constexpr uint16_t kTriesSize = 4;
  // debug_info_off_ is not accessible from the helpers yet.
  static constexpr size_t kInsnsSizeInCodeUnits = 5;

  auto verify_code_item = [&](const DexFile* dex,
                              const dex::CodeItem* item,
                              const uint16_t* insns) {
    CodeItemInstructionAccessor insns_accessor(*dex, item);
    EXPECT_TRUE(insns_accessor.HasCodeItem());
    ASSERT_EQ(insns_accessor.InsnsSizeInCodeUnits(), kInsnsSizeInCodeUnits);
    EXPECT_EQ(insns_accessor.Insns(), insns);

    CodeItemDataAccessor data_accessor(*dex, item);
    EXPECT_TRUE(data_accessor.HasCodeItem());
    EXPECT_EQ(data_accessor.InsnsSizeInCodeUnits(), kInsnsSizeInCodeUnits);
    EXPECT_EQ(data_accessor.Insns(), insns);
    EXPECT_EQ(data_accessor.RegistersSize(), kRegisterSize);
    EXPECT_EQ(data_accessor.InsSize(), kInsSize);
    EXPECT_EQ(data_accessor.OutsSize(), kOutsSize);
    EXPECT_EQ(data_accessor.TriesSize(), kTriesSize);
  };

  StandardDexFile::CodeItem* dex_code_item =
      reinterpret_cast<StandardDexFile::CodeItem*>(const_cast<uint8_t*>(standard_dex->Begin()));
  dex_code_item->registers_size_ = kRegisterSize;
  dex_code_item->ins_size_ = kInsSize;
  dex_code_item->outs_size_ = kOutsSize;
  dex_code_item->tries_size_ = kTriesSize;
  dex_code_item->insns_size_in_code_units_ = kInsnsSizeInCodeUnits;
  verify_code_item(standard_dex.get(), dex_code_item, dex_code_item->insns_);
}

}  // namespace art
