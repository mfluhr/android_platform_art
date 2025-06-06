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

#include "oat.h"

#include <string.h>
#include <zlib.h>

#include "android-base/logging.h"
#include "android-base/stringprintf.h"
#include "arch/instruction_set.h"
#include "arch/instruction_set_features.h"
#include "base/bit_utils.h"
#include "base/strlcpy.h"

namespace art HIDDEN {

using android::base::StringPrintf;

static size_t ComputeOatHeaderSize(const SafeMap<std::string, std::string>* variable_data) {
  size_t estimate = 0U;
  if (variable_data != nullptr) {
    SafeMap<std::string, std::string>::const_iterator it = variable_data->begin();
    SafeMap<std::string, std::string>::const_iterator end = variable_data->end();
    for ( ; it != end; ++it) {
      estimate += it->first.length() + 1;
      estimate += it->second.length() + 1;

      size_t non_deterministic_field_length = OatHeader::GetNonDeterministicFieldLength(it->first);
      if (non_deterministic_field_length > 0u) {
        DCHECK_LE(it->second.length(), non_deterministic_field_length);
        size_t padding = non_deterministic_field_length - it->second.length();
        estimate += padding;
      }
    }
  }
  return sizeof(OatHeader) + estimate;
}

OatHeader* OatHeader::Create(InstructionSet instruction_set,
                             const InstructionSetFeatures* instruction_set_features,
                             uint32_t dex_file_count,
                             const SafeMap<std::string, std::string>* variable_data,
                             uint32_t base_oat_offset) {
  // Estimate size of optional data.
  size_t needed_size = ComputeOatHeaderSize(variable_data);

  // Reserve enough memory.
  void* memory = operator new (needed_size);

  // Create the OatHeader in-place.
  return new (memory) OatHeader(instruction_set,
                                instruction_set_features,
                                dex_file_count,
                                variable_data,
                                base_oat_offset);
}

void OatHeader::Delete(OatHeader* header) {
  if (header != nullptr) {
    size_t size = header->GetHeaderSize();
    header->~OatHeader();
    operator delete(header, size);
  }
}

OatHeader::OatHeader(InstructionSet instruction_set,
                     const InstructionSetFeatures* instruction_set_features,
                     uint32_t dex_file_count,
                     const SafeMap<std::string, std::string>* variable_data,
                     uint32_t base_oat_offset)
    : oat_checksum_(0u),
      instruction_set_(instruction_set),
      instruction_set_features_bitmap_(instruction_set_features->AsBitmap()),
      dex_file_count_(dex_file_count),
      oat_dex_files_offset_(0),
      bcp_bss_info_offset_(0),
      base_oat_offset_(base_oat_offset),
      executable_offset_(0),
      jni_dlsym_lookup_trampoline_offset_(0),
      jni_dlsym_lookup_critical_trampoline_offset_(0),
      quick_generic_jni_trampoline_offset_(0),
      quick_imt_conflict_trampoline_offset_(0),
      quick_resolution_trampoline_offset_(0),
      quick_to_interpreter_bridge_offset_(0),
      nterp_trampoline_offset_(0) {
  // Don't want asserts in header as they would be checked in each file that includes it. But the
  // fields are private, so we check inside a method.
  static_assert(decltype(magic_)().size() == kOatMagic.size(),
                "Oat magic and magic_ have different lengths.");
  static_assert(decltype(version_)().size() == kOatVersion.size(),
                "Oat version and version_ have different lengths.");

  magic_ = kOatMagic;
  version_ = kOatVersion;

  CHECK_NE(instruction_set, InstructionSet::kNone);

  // Flatten the map. Will also update variable_size_data_size_.
  Flatten(variable_data);
}

bool OatHeader::IsValid() const {
  if (magic_ != kOatMagic) {
    return false;
  }
  if (version_ != kOatVersion) {
    return false;
  }
  // Only check the offset is valid after it has been set.
  if (executable_offset_ != 0u &&
      !IsAligned<kElfSegmentAlignment>(executable_offset_ + base_oat_offset_)) {
    return false;
  }
  if (!IsValidInstructionSet(instruction_set_)) {
    return false;
  }
  return true;
}

std::string OatHeader::GetValidationErrorMessage() const {
  if (magic_ != kOatMagic) {
    static_assert(kOatMagic.size() == 4, "kOatMagic has unexpected length");
    return StringPrintf("Invalid oat magic, expected 0x%02x%02x%02x%02x, got 0x%02x%02x%02x%02x.",
                        kOatMagic[0], kOatMagic[1], kOatMagic[2], kOatMagic[3],
                        magic_[0], magic_[1], magic_[2], magic_[3]);
  }
  if (version_ != kOatVersion) {
    static_assert(kOatVersion.size() == 4, "kOatVersion has unexpected length");
    return StringPrintf("Invalid oat version, expected 0x%02x%02x%02x%02x, got 0x%02x%02x%02x%02x.",
                        kOatVersion[0], kOatVersion[1], kOatVersion[2], kOatVersion[3],
                        version_[0], version_[1], version_[2], version_[3]);
  }
  // Only check the offset is valid after it has been set.
  if (executable_offset_ != 0u &&
      !IsAligned<kElfSegmentAlignment>(executable_offset_ + base_oat_offset_)) {
    return "Executable offset not properly aligned.";
  }
  if (!IsValidInstructionSet(instruction_set_)) {
    return StringPrintf("Invalid instruction set, %d.", static_cast<int>(instruction_set_));
  }
  return "";
}

// Do not move this into the header.  The method must be compiled in the runtime library,
// so that we can check that the compile-time oat version matches the version in the caller.
void OatHeader::CheckOatVersion(std::array<uint8_t, 4> version) {
  constexpr std::array<uint8_t, 4> expected = kOatVersion;  // Runtime oat version.
  if (version != kOatVersion) {
    LOG(FATAL) << StringPrintf("Invalid oat version, expected 0x%02x%02x%02x%02x, "
                                   "got 0x%02x%02x%02x%02x.",
                               expected[0], expected[1], expected[2], expected[3],
                               version[0], version[1], version[2], version[3]);
  }
}

const char* OatHeader::GetMagic() const {
  CHECK(IsValid());
  return reinterpret_cast<const char*>(magic_.data());
}

uint32_t OatHeader::GetChecksum() const {
  CHECK(IsValid());
  return oat_checksum_;
}

void OatHeader::SetChecksum(uint32_t oat_checksum) {
  oat_checksum_ = oat_checksum;
}

InstructionSet OatHeader::GetInstructionSet() const {
  CHECK(IsValid());
  return instruction_set_;
}

uint32_t OatHeader::GetInstructionSetFeaturesBitmap() const {
  CHECK(IsValid());
  return instruction_set_features_bitmap_;
}

uint32_t OatHeader::GetOatDexFilesOffset() const {
  DCHECK(IsValid());
  DCHECK_GT(oat_dex_files_offset_, sizeof(OatHeader));
  return oat_dex_files_offset_;
}

void OatHeader::SetOatDexFilesOffset(uint32_t oat_dex_files_offset) {
  DCHECK_GT(oat_dex_files_offset, sizeof(OatHeader));
  DCHECK(IsValid());
  DCHECK_EQ(oat_dex_files_offset_, 0u);

  oat_dex_files_offset_ = oat_dex_files_offset;
}

uint32_t OatHeader::GetBcpBssInfoOffset() const {
  DCHECK(IsValid());
  DCHECK(bcp_bss_info_offset_ == 0u || bcp_bss_info_offset_ > sizeof(OatHeader))
      << "bcp_bss_info_offset_: " << bcp_bss_info_offset_
      << "sizeof(OatHeader): " << sizeof(OatHeader);
  return bcp_bss_info_offset_;
}

void OatHeader::SetBcpBssInfoOffset(uint32_t bcp_info_offset) {
  DCHECK_GT(bcp_info_offset, sizeof(OatHeader));
  DCHECK(IsValid());
  DCHECK_EQ(bcp_bss_info_offset_, 0u);

  bcp_bss_info_offset_ = bcp_info_offset;
}

uint32_t OatHeader::GetExecutableOffset() const {
  DCHECK(IsValid());
  DCHECK_ALIGNED(executable_offset_ + base_oat_offset_, kElfSegmentAlignment);
  CHECK_GT(executable_offset_, sizeof(OatHeader));
  return executable_offset_;
}

void OatHeader::SetExecutableOffset(uint32_t executable_offset) {
  DCHECK_ALIGNED(executable_offset + base_oat_offset_, kElfSegmentAlignment);
  CHECK_GT(executable_offset, sizeof(OatHeader));
  DCHECK(IsValid());
  DCHECK_EQ(executable_offset_, 0U);

  executable_offset_ = executable_offset;
}

static const void* GetTrampoline(const OatHeader& header, uint32_t offset) {
  return (offset != 0u) ? reinterpret_cast<const uint8_t*>(&header) + offset : nullptr;
}

const void* OatHeader::GetJniDlsymLookupTrampoline() const {
  return GetTrampoline(*this, GetJniDlsymLookupTrampolineOffset());
}

uint32_t OatHeader::GetJniDlsymLookupTrampolineOffset() const {
  DCHECK(IsValid());
  return jni_dlsym_lookup_trampoline_offset_;
}

void OatHeader::SetJniDlsymLookupTrampolineOffset(uint32_t offset) {
  DCHECK(IsValid());
  DCHECK_EQ(jni_dlsym_lookup_trampoline_offset_, 0U) << offset;

  jni_dlsym_lookup_trampoline_offset_ = offset;
}

const void* OatHeader::GetJniDlsymLookupCriticalTrampoline() const {
  return GetTrampoline(*this, GetJniDlsymLookupCriticalTrampolineOffset());
}

uint32_t OatHeader::GetJniDlsymLookupCriticalTrampolineOffset() const {
  DCHECK(IsValid());
  return jni_dlsym_lookup_critical_trampoline_offset_;
}

void OatHeader::SetJniDlsymLookupCriticalTrampolineOffset(uint32_t offset) {
  DCHECK(IsValid());
  DCHECK_EQ(jni_dlsym_lookup_critical_trampoline_offset_, 0U) << offset;

  jni_dlsym_lookup_critical_trampoline_offset_ = offset;
}

const void* OatHeader::GetQuickGenericJniTrampoline() const {
  return GetTrampoline(*this, GetQuickGenericJniTrampolineOffset());
}

uint32_t OatHeader::GetQuickGenericJniTrampolineOffset() const {
  DCHECK(IsValid());
  CHECK_GE(quick_generic_jni_trampoline_offset_, jni_dlsym_lookup_trampoline_offset_);
  return quick_generic_jni_trampoline_offset_;
}

void OatHeader::SetQuickGenericJniTrampolineOffset(uint32_t offset) {
  CHECK(offset == 0 || offset >= jni_dlsym_lookup_trampoline_offset_);
  DCHECK(IsValid());
  DCHECK_EQ(quick_generic_jni_trampoline_offset_, 0U) << offset;

  quick_generic_jni_trampoline_offset_ = offset;
}

const void* OatHeader::GetQuickImtConflictTrampoline() const {
  return GetTrampoline(*this, GetQuickImtConflictTrampolineOffset());
}

uint32_t OatHeader::GetQuickImtConflictTrampolineOffset() const {
  DCHECK(IsValid());
  CHECK_GE(quick_imt_conflict_trampoline_offset_, quick_generic_jni_trampoline_offset_);
  return quick_imt_conflict_trampoline_offset_;
}

void OatHeader::SetQuickImtConflictTrampolineOffset(uint32_t offset) {
  CHECK(offset == 0 || offset >= quick_generic_jni_trampoline_offset_);
  DCHECK(IsValid());
  DCHECK_EQ(quick_imt_conflict_trampoline_offset_, 0U) << offset;

  quick_imt_conflict_trampoline_offset_ = offset;
}

const void* OatHeader::GetQuickResolutionTrampoline() const {
  return GetTrampoline(*this, GetQuickResolutionTrampolineOffset());
}

uint32_t OatHeader::GetQuickResolutionTrampolineOffset() const {
  DCHECK(IsValid());
  CHECK_GE(quick_resolution_trampoline_offset_, quick_imt_conflict_trampoline_offset_);
  return quick_resolution_trampoline_offset_;
}

void OatHeader::SetQuickResolutionTrampolineOffset(uint32_t offset) {
  CHECK(offset == 0 || offset >= quick_imt_conflict_trampoline_offset_);
  DCHECK(IsValid());
  DCHECK_EQ(quick_resolution_trampoline_offset_, 0U) << offset;

  quick_resolution_trampoline_offset_ = offset;
}

const void* OatHeader::GetQuickToInterpreterBridge() const {
  return GetTrampoline(*this, GetQuickToInterpreterBridgeOffset());
}

uint32_t OatHeader::GetQuickToInterpreterBridgeOffset() const {
  DCHECK(IsValid());
  CHECK_GE(quick_to_interpreter_bridge_offset_, quick_resolution_trampoline_offset_);
  return quick_to_interpreter_bridge_offset_;
}

void OatHeader::SetQuickToInterpreterBridgeOffset(uint32_t offset) {
  CHECK(offset == 0 || offset >= quick_resolution_trampoline_offset_);
  DCHECK(IsValid());
  DCHECK_EQ(quick_to_interpreter_bridge_offset_, 0U) << offset;

  quick_to_interpreter_bridge_offset_ = offset;
}

const void* OatHeader::GetNterpTrampoline() const {
  return GetTrampoline(*this, GetNterpTrampolineOffset());
}

uint32_t OatHeader::GetNterpTrampolineOffset() const {
  DCHECK(IsValid());
  CHECK_GE(nterp_trampoline_offset_, quick_to_interpreter_bridge_offset_);
  return nterp_trampoline_offset_;
}

void OatHeader::SetNterpTrampolineOffset(uint32_t offset) {
  CHECK(offset == 0 || offset >= quick_to_interpreter_bridge_offset_);
  DCHECK(IsValid());
  DCHECK_EQ(nterp_trampoline_offset_, 0U) << offset;

  nterp_trampoline_offset_ = offset;
}

uint32_t OatHeader::GetKeyValueStoreSize() const {
  CHECK(IsValid());
  return key_value_store_size_;
}

const uint8_t* OatHeader::GetKeyValueStore() const {
  CHECK(IsValid());
  return key_value_store_;
}

const char* OatHeader::GetStoreValueByKeyUnsafe(const char* key) const {
  std::string_view key_view(key);

  uint32_t offset = 0;
  const char* current_key;
  const char* value;
  while (GetNextStoreKeyValuePair(&offset, &current_key, &value)) {
    if (key_view == current_key) {
      // Same as key.
      return value;
    }
  }

  // Not found.
  return nullptr;
}

bool OatHeader::GetNextStoreKeyValuePair(/*inout*/ uint32_t* offset,
                                         /*out*/ const char** key,
                                         /*out*/ const char** value) const {
  if (*offset >= key_value_store_size_) {
    return false;
  }

  const char* start = reinterpret_cast<const char*>(&key_value_store_);
  const char* ptr = start + *offset;
  const char* end = start + key_value_store_size_;

  // Scan for a closing zero.
  const char* str_end = reinterpret_cast<const char*>(memchr(ptr, 0, end - ptr));
  if (UNLIKELY(str_end == nullptr)) {
    LOG(WARNING) << "OatHeader: Unterminated key in key value store.";
    return false;
  }
  const char* value_start = str_end + 1;
  const char* value_end = reinterpret_cast<const char*>(memchr(value_start, 0, end - value_start));
  if (UNLIKELY(value_end == nullptr)) {
    LOG(WARNING) << "OatHeader: Unterminated value in key value store.";
    return false;
  }

  *key = ptr;
  *value = value_start;

  // Advance over the value.
  size_t key_len = str_end - ptr;
  size_t value_len = value_end - value_start;
  size_t non_deterministic_field_length = GetNonDeterministicFieldLength(*key);
  if (non_deterministic_field_length > 0u) {
    if (UNLIKELY(value_len > non_deterministic_field_length)) {
      LOG(WARNING) << "OatHeader: Non-deterministic field too long in key value store.";
      return false;
    }
    *offset += key_len + 1 + non_deterministic_field_length + 1;
  } else {
    *offset += key_len + 1 + value_len + 1;
  }

  return true;
}

void OatHeader::ComputeChecksum(/*inout*/ uint32_t* checksum) const {
  *checksum = adler32(*checksum, reinterpret_cast<const uint8_t*>(this), sizeof(OatHeader));

  uint32_t last_offset = 0;
  uint32_t offset = 0;
  const char* key;
  const char* value;
  while (GetNextStoreKeyValuePair(&offset, &key, &value)) {
    if (IsDeterministicField(key)) {
      // Update the checksum.
      *checksum = adler32(*checksum, GetKeyValueStore() + last_offset, offset - last_offset);
    }
    last_offset = offset;
  }
}

size_t OatHeader::GetHeaderSize() const {
  return sizeof(OatHeader) + key_value_store_size_;
}

bool OatHeader::IsDebuggable() const {
  return IsKeyEnabled(OatHeader::kDebuggableKey);
}

bool OatHeader::IsConcurrentCopying() const {
  return IsKeyEnabled(OatHeader::kConcurrentCopying);
}

bool OatHeader::IsNativeDebuggable() const {
  return IsKeyEnabled(OatHeader::kNativeDebuggableKey);
}

bool OatHeader::RequiresImage() const {
  return IsKeyEnabled(OatHeader::kRequiresImage);
}

CompilerFilter::Filter OatHeader::GetCompilerFilter() const {
  CompilerFilter::Filter filter;
  const char* key_value = GetStoreValueByKey(kCompilerFilter);
  CHECK(key_value != nullptr) << "compiler-filter not found in oat header";
  CHECK(CompilerFilter::ParseCompilerFilter(key_value, &filter))
      << "Invalid compiler-filter in oat header: " << key_value;
  return filter;
}

bool OatHeader::KeyHasValue(const char* key, const char* value, size_t value_size) const {
  const char* key_value = GetStoreValueByKey(key);
  return (key_value != nullptr && strncmp(key_value, value, value_size) == 0);
}

bool OatHeader::IsKeyEnabled(const char* key) const {
  return KeyHasValue(key, kTrueValue, sizeof(kTrueValue));
}

void OatHeader::Flatten(const SafeMap<std::string, std::string>* key_value_store) {
  char* data_ptr = reinterpret_cast<char*>(&key_value_store_);
  if (key_value_store != nullptr) {
    SafeMap<std::string, std::string>::const_iterator it = key_value_store->begin();
    SafeMap<std::string, std::string>::const_iterator end = key_value_store->end();
    for ( ; it != end; ++it) {
      strlcpy(data_ptr, it->first.c_str(), it->first.length() + 1);
      data_ptr += it->first.length() + 1;
      strlcpy(data_ptr, it->second.c_str(), it->second.length() + 1);
      data_ptr += it->second.length() + 1;

      size_t non_deterministic_field_length = GetNonDeterministicFieldLength(it->first);
      if (non_deterministic_field_length > 0u) {
        DCHECK_LE(it->second.length(), non_deterministic_field_length);
        size_t padding = non_deterministic_field_length - it->second.length();
        memset(data_ptr, 0, padding);
        data_ptr += padding;
      }
    }
  }
  key_value_store_size_ = data_ptr - reinterpret_cast<char*>(&key_value_store_);
}

const uint8_t* OatHeader::GetOatAddress(StubType type) const {
  DCHECK_LE(type, StubType::kLast);
  switch (type) {
    // TODO: We could maybe clean this up if we stored them in an array in the oat header.
    case StubType::kQuickGenericJNITrampoline:
      return static_cast<const uint8_t*>(GetQuickGenericJniTrampoline());
    case StubType::kJNIDlsymLookupTrampoline:
      return static_cast<const uint8_t*>(GetJniDlsymLookupTrampoline());
    case StubType::kJNIDlsymLookupCriticalTrampoline:
      return static_cast<const uint8_t*>(GetJniDlsymLookupCriticalTrampoline());
    case StubType::kQuickIMTConflictTrampoline:
      return static_cast<const uint8_t*>(GetQuickImtConflictTrampoline());
    case StubType::kQuickResolutionTrampoline:
      return static_cast<const uint8_t*>(GetQuickResolutionTrampoline());
    case StubType::kQuickToInterpreterBridge:
      return static_cast<const uint8_t*>(GetQuickToInterpreterBridge());
    case StubType::kNterpTrampoline:
      return static_cast<const uint8_t*>(GetNterpTrampoline());
  }
}

}  // namespace art
