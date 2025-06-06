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

#include "oat_file.h"

#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>

#include "android-base/file.h"
#include "android-base/logging.h"
#include "android-base/stringprintf.h"
#include "arch/instruction_set_features.h"
#include "art_method.h"
#include "base/array_ref.h"
#include "base/bit_vector.h"
#include "base/file_utils.h"
#include "base/globals.h"
#include "base/logging.h"  // For VLOG_IS_ON.
#include "base/macros.h"
#include "base/mem_map.h"
#include "base/os.h"
#include "base/pointer_size.h"
#include "base/stl_util.h"
#include "base/systrace.h"
#include "base/time_utils.h"
#include "base/unix_file/fd_file.h"
#include "base/utils.h"
#include "base/zip_archive.h"
#include "class_loader_context.h"
#include "dex/art_dex_file_loader.h"
#include "dex/dex_file.h"
#include "dex/dex_file_loader.h"
#include "dex/dex_file_structs.h"
#include "dex/dex_file_types.h"
#include "dex/standard_dex_file.h"
#include "dex/type_lookup_table.h"
#include "dex/utf-inl.h"
#include "elf/elf_utils.h"
#include "elf_file.h"
#include "gc/heap.h"
#include "gc/space/image_space.h"
#include "gc_root.h"
#include "mirror/class.h"
#include "mirror/object-inl.h"
#include "oat.h"
#include "oat/sdc_file.h"
#include "oat_file-inl.h"
#include "oat_file_manager.h"
#include "runtime-inl.h"
#include "vdex_file.h"
#include "verifier/verifier_deps.h"

#ifndef __APPLE__
#include <link.h>  // for dl_iterate_phdr.
#endif

#ifdef __GLIBC__
#include <gnu/libc-version.h>  // for gnu_get_libc_version.
// strverscmp is part of the GNU/Linux extension, so define _GNU_SOURCE before including
// string.h, and undefine it afterward if it is not already defined.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#define DEFINED_GNU_SOURCE
#endif
#include <string.h>  // for strverscmp
#ifdef DEFINED_GNU_SOURCE
#undef _GNU_SOURCE
#undef DEFINED_GNU_SOURCE
#endif
#endif

// dlopen_ext support from bionic.
#ifdef ART_TARGET_ANDROID
#include "android/dlext.h"
#include "nativeloader/dlext_namespaces.h"
#endif

namespace art HIDDEN {

using android::base::StringAppendV;
using android::base::StringPrintf;

// Whether OatFile::Open will try dlopen. Fallback is our own ELF loader.
static constexpr bool kUseDlopen = true;

// Whether OatFile::Open will try dlopen on the host. On the host we're not linking against
// bionic, so cannot take advantage of the support for changed semantics (loading the same soname
// multiple times). However, if/when we switch the above, we likely want to switch this, too,
// to get test coverage of the code paths.
static constexpr bool kUseDlopenOnHost = true;

// For debugging, Open will print DlOpen error message if set to true.
static constexpr bool kPrintDlOpenErrorMessage = false;

// Returns whether dlopen can load dynamic shared objects with a read-only .dynamic section.
// According to the ELF spec whether .dynamic is writable or not is determined by the operating
// system and processor (Book I, part 1 "Object Files", "Special sections"). Bionic and glibc
// >= 2.35 support read-only .dynamic. Older glibc versions have a bug that causes a crash if
// this section is read-only: https://sourceware.org/bugzilla/show_bug.cgi?id=28340.
bool IsReadOnlyDynamicSupportedByDlOpen() {
  // The following lambda will be executed only once as a part of a static
  // variable initialization.
#ifdef __GLIBC__
  static bool is_ro_dynamic_supported = []() {
    // libc version has the following format:
    //   "X.Y"
    // where:
    //   X - major version in the decimal format.
    //   Y - minor version in the decimal format.
    // for example:
    //    "2.34"
    const char* libc_version = gnu_get_libc_version();
    return strverscmp(libc_version, "2.35") >= 0;
  }();
  return is_ro_dynamic_supported;
#else
  return true;
#endif
}

// Note for OatFileBase and descendents:
//
// These are used in OatFile::Open to try all our loaders.
//
// The process is simple:
//
// 1) Allocate an instance through the standard constructor (location, executable)
// 2) Load() to try to open the file.
// 3) ComputeFields() to populate the OatFile fields like begin_, using FindDynamicSymbolAddress.
// 4) PreSetup() for any steps that should be done before the final setup.
// 5) Setup() to complete the procedure.

class OatFileBase : public OatFile {
 public:
  virtual ~OatFileBase() {}

  template <typename kOatFileBaseSubType>
  static OatFileBase* OpenOatFile(int zip_fd,
                                  const std::string& vdex_filename,
                                  const std::string& elf_filename,
                                  const std::string& location,
                                  bool executable,
                                  bool low_4gb,
                                  ArrayRef<const std::string> dex_filenames,
                                  ArrayRef<File> dex_files,
                                  /*inout*/ MemMap* reservation,  // Where to load if not null.
                                  /*out*/ std::string* error_msg);

  template <typename kOatFileBaseSubType>
  static OatFileBase* OpenOatFile(int zip_fd,
                                  int vdex_fd,
                                  int oat_fd,
                                  const std::string& vdex_filename,
                                  const std::string& oat_filename,
                                  bool executable,
                                  bool low_4gb,
                                  ArrayRef<const std::string> dex_filenames,
                                  ArrayRef<File> dex_files,
                                  /*inout*/ MemMap* reservation,  // Where to load if not null.
                                  /*out*/ std::string* error_msg);

  template <typename kOatFileBaseSubType>
  static OatFileBase* OpenOatFileFromSdm(const std::string& sdm_filename,
                                         const std::string& sdc_filename,
                                         const std::string& dm_filename,
                                         const std::string& dex_filename,
                                         bool executable,
                                         /*out*/ std::string* error_msg);

 protected:
  OatFileBase(const std::string& filename, bool executable) : OatFile(filename, executable) {}

  virtual const uint8_t* FindDynamicSymbolAddress(const std::string& symbol_name,
                                                  std::string* error_msg) const = 0;

  virtual void PreLoad() = 0;

  bool LoadVdex(const std::string& vdex_filename, bool low_4gb, std::string* error_msg);

  bool LoadVdex(int vdex_fd,
                const std::string& vdex_filename,
                bool low_4gb,
                std::string* error_msg);

  virtual bool Load(const std::string& elf_filename,
                    bool executable,
                    bool low_4gb,
                    /*inout*/ MemMap* reservation,  // Where to load if not null.
                    /*out*/ std::string* error_msg) = 0;

  virtual bool Load(int oat_fd,
                    bool executable,
                    bool low_4gb,
                    /*inout*/ MemMap* reservation,  // Where to load if not null.
                    /*out*/ std::string* error_msg) = 0;

  bool ComputeFields(const std::string& file_path, std::string* error_msg);

  virtual void PreSetup(const std::string& elf_filename) = 0;

  bool Setup(int zip_fd,
             ArrayRef<const std::string> dex_filenames,
             ArrayRef<File> dex_files,
             std::string* error_msg);

  bool Setup(const std::vector<const DexFile*>& dex_files, std::string* error_msg);

  // Setters exposed for ElfOatFile.

  void SetBegin(const uint8_t* begin) {
    begin_ = begin;
  }

  void SetEnd(const uint8_t* end) {
    end_ = end;
  }

  void SetVdex(VdexFile* vdex) {
    vdex_.reset(vdex);
  }

 private:
  std::string ErrorPrintf(const char* fmt, ...) __attribute__((__format__(__printf__, 2, 3)));
  bool ReadIndexBssMapping(/*inout*/const uint8_t** oat,
                           const char* container_tag,
                           size_t dex_file_index,
                           const std::string& dex_file_location,
                           const char* entry_tag,
                           /*out*/const IndexBssMapping** mapping,
                           std::string* error_msg);
  bool ReadBssMappingInfo(/*inout*/const uint8_t** oat,
                          const char* container_tag,
                          size_t dex_file_index,
                          const std::string& dex_file_location,
                          /*out*/BssMappingInfo* bss_mapping_info,
                          std::string* error_msg);

  DISALLOW_COPY_AND_ASSIGN(OatFileBase);
};

template <typename kOatFileBaseSubType>
OatFileBase* OatFileBase::OpenOatFile(int zip_fd,
                                      const std::string& vdex_filename,
                                      const std::string& elf_filename,
                                      const std::string& location,
                                      bool executable,
                                      bool low_4gb,
                                      ArrayRef<const std::string> dex_filenames,
                                      ArrayRef<File> dex_files,
                                      /*inout*/ MemMap* reservation,
                                      /*out*/ std::string* error_msg) {
  std::unique_ptr<OatFileBase> ret(new kOatFileBaseSubType(location, executable));

  ret->PreLoad();

  if (!ret->Load(elf_filename, executable, low_4gb, reservation, error_msg)) {
    return nullptr;
  }

  if (!ret->ComputeFields(elf_filename, error_msg)) {
    return nullptr;
  }

  ret->PreSetup(elf_filename);

  if (!ret->LoadVdex(vdex_filename, low_4gb, error_msg)) {
    return nullptr;
  }

  if (!ret->Setup(zip_fd, dex_filenames, dex_files, error_msg)) {
    return nullptr;
  }

  return ret.release();
}

template <typename kOatFileBaseSubType>
OatFileBase* OatFileBase::OpenOatFile(int zip_fd,
                                      int vdex_fd,
                                      int oat_fd,
                                      const std::string& vdex_location,
                                      const std::string& oat_location,
                                      bool executable,
                                      bool low_4gb,
                                      ArrayRef<const std::string> dex_filenames,
                                      ArrayRef<File> dex_files,
                                      /*inout*/ MemMap* reservation,
                                      /*out*/ std::string* error_msg) {
  std::unique_ptr<OatFileBase> ret(new kOatFileBaseSubType(oat_location, executable));

  if (!ret->Load(oat_fd, executable, low_4gb, reservation, error_msg)) {
    return nullptr;
  }

  if (!ret->ComputeFields(oat_location, error_msg)) {
    return nullptr;
  }

  ret->PreSetup(oat_location);

  if (!ret->LoadVdex(vdex_fd, vdex_location, low_4gb, error_msg)) {
    return nullptr;
  }

  if (!ret->Setup(zip_fd, dex_filenames, dex_files, error_msg)) {
    return nullptr;
  }

  return ret.release();
}

template <typename kOatFileBaseSubType>
OatFileBase* OatFileBase::OpenOatFileFromSdm(const std::string& sdm_filename,
                                             const std::string& sdc_filename,
                                             const std::string& dm_filename,
                                             const std::string& dex_filename,
                                             bool executable,
                                             /*out*/ std::string* error_msg) {
  std::string elf_filename = sdm_filename + kZipSeparator + "primary.odex";
  std::unique_ptr<OatFileBase> ret(new kOatFileBaseSubType(elf_filename, executable));

  struct stat sdm_st;
  if (stat(sdm_filename.c_str(), &sdm_st) != 0) {
    *error_msg = ART_FORMAT("Failed to stat sdm file '{}': {}", sdm_filename, strerror(errno));
    return nullptr;
  }

  std::unique_ptr<SdcReader> sdc_reader = SdcReader::Load(sdc_filename, error_msg);
  if (sdc_reader == nullptr) {
    return nullptr;
  }
  if (sdc_reader->GetSdmTimestampNs() != TimeSpecToNs(sdm_st.st_mtim)) {
    // The sdm file had been replaced after the sdc file was created.
    *error_msg = ART_FORMAT("Obsolete sdc file '{}'", sdc_filename);
    return nullptr;
  }
  // The apex-versions value in the sdc file, written by ART Service, is the value of
  // `Runtime::GetApexVersions` at the time where the sdm file was first seen on device. We use it
  // to override the APEX versions in the oat header. This is for detecting samegrade placebos.
  ret->override_apex_versions_ = sdc_reader->GetApexVersions();

  if (!ret->Load(elf_filename, executable, /*low_4gb=*/false, /*reservation=*/nullptr, error_msg)) {
    return nullptr;
  }

  if (!ret->ComputeFields(elf_filename, error_msg)) {
    return nullptr;
  }

  ret->PreSetup(elf_filename);

  ret->vdex_ = VdexFile::OpenFromDm(dm_filename, ret->vdex_begin_, ret->vdex_end_, error_msg);
  if (ret->vdex_ == nullptr) {
    return nullptr;
  }

  if (!ret->Setup(/*zip_fd=*/-1,
                  ArrayRef<const std::string>(&dex_filename, /*size=*/1u),
                  /*dex_files=*/{},
                  error_msg)) {
    return nullptr;
  }

  return ret.release();
}

bool OatFileBase::LoadVdex(const std::string& vdex_filename, bool low_4gb, std::string* error_msg) {
  vdex_ = VdexFile::OpenAtAddress(vdex_begin_,
                                  vdex_end_ - vdex_begin_,
                                  /*mmap_reuse=*/vdex_begin_ != nullptr,
                                  vdex_filename,
                                  low_4gb,
                                  error_msg);
  if (vdex_.get() == nullptr) {
    *error_msg = StringPrintf("Failed to load vdex file '%s' %s",
                              vdex_filename.c_str(),
                              error_msg->c_str());
    return false;
  }
  return true;
}

bool OatFileBase::LoadVdex(int vdex_fd,
                           const std::string& vdex_filename,
                           bool low_4gb,
                           std::string* error_msg) {
  if (vdex_fd != -1) {
    struct stat s;
    int rc = TEMP_FAILURE_RETRY(fstat(vdex_fd, &s));
    if (rc == -1) {
      PLOG(WARNING) << "Failed getting length of vdex file";
    } else {
      vdex_ = VdexFile::OpenAtAddress(vdex_begin_,
                                      vdex_end_ - vdex_begin_,
                                      /*mmap_reuse=*/vdex_begin_ != nullptr,
                                      vdex_fd,
                                      /*start=*/0,
                                      s.st_size,
                                      vdex_filename,
                                      low_4gb,
                                      error_msg);
      if (vdex_.get() == nullptr) {
        *error_msg = "Failed opening vdex file.";
        return false;
      }
    }
  }
  return true;
}

bool OatFileBase::ComputeFields(const std::string& file_path, std::string* error_msg) {
  std::string symbol_error_msg;
  begin_ = FindDynamicSymbolAddress("oatdata", &symbol_error_msg);
  if (begin_ == nullptr) {
    *error_msg = StringPrintf("Failed to find oatdata symbol in '%s' %s",
                              file_path.c_str(),
                              symbol_error_msg.c_str());
    return false;
  }
  end_ = FindDynamicSymbolAddress("oatlastword", &symbol_error_msg);
  if (end_ == nullptr) {
    *error_msg = StringPrintf("Failed to find oatlastword symbol in '%s' %s",
                              file_path.c_str(),
                              symbol_error_msg.c_str());
    return false;
  }
  // Readjust to be non-inclusive upper bound.
  end_ += sizeof(uint32_t);

  data_img_rel_ro_begin_ = FindDynamicSymbolAddress("oatdataimgrelro", &symbol_error_msg);
  if (data_img_rel_ro_begin_ != nullptr) {
    data_img_rel_ro_end_ =
        FindDynamicSymbolAddress("oatdataimgrelrolastword", &symbol_error_msg);
    if (data_img_rel_ro_end_ == nullptr) {
      *error_msg =
          StringPrintf("Failed to find oatdataimgrelrolastword symbol in '%s'", file_path.c_str());
      return false;
    }
    // Readjust to be non-inclusive upper bound.
    data_img_rel_ro_end_ += sizeof(uint32_t);
    data_img_rel_ro_app_image_ =
        FindDynamicSymbolAddress("oatdataimgrelroappimage", &symbol_error_msg);
    if (data_img_rel_ro_app_image_ == nullptr) {
      data_img_rel_ro_app_image_ = data_img_rel_ro_end_;
    }
  }

  bss_begin_ = const_cast<uint8_t*>(FindDynamicSymbolAddress("oatbss", &symbol_error_msg));
  if (bss_begin_ == nullptr) {
    // No .bss section.
    bss_end_ = nullptr;
  } else {
    bss_end_ = const_cast<uint8_t*>(FindDynamicSymbolAddress("oatbsslastword", &symbol_error_msg));
    if (bss_end_ == nullptr) {
      *error_msg = StringPrintf("Failed to find oatbsslastword symbol in '%s'", file_path.c_str());
      return false;
    }
    // Readjust to be non-inclusive upper bound.
    bss_end_ += sizeof(uint32_t);
    // Find bss methods if present.
    bss_methods_ =
        const_cast<uint8_t*>(FindDynamicSymbolAddress("oatbssmethods", &symbol_error_msg));
    // Find bss roots if present.
    bss_roots_ = const_cast<uint8_t*>(FindDynamicSymbolAddress("oatbssroots", &symbol_error_msg));
  }

  vdex_begin_ = const_cast<uint8_t*>(FindDynamicSymbolAddress("oatdex", &symbol_error_msg));
  if (vdex_begin_ == nullptr) {
    // No .vdex section.
    vdex_end_ = nullptr;
  } else {
    vdex_end_ = const_cast<uint8_t*>(FindDynamicSymbolAddress("oatdexlastword", &symbol_error_msg));
    if (vdex_end_ == nullptr) {
      *error_msg = StringPrintf("Failed to find oatdexlastword symbol in '%s'", file_path.c_str());
      return false;
    }
    // Readjust to be non-inclusive upper bound.
    vdex_end_ += sizeof(uint32_t);
  }

  return true;
}

// Read an unaligned entry from the OatDexFile data in OatFile and advance the read
// position by the number of bytes read, i.e. sizeof(T).
// Return true on success, false if the read would go beyond the end of the OatFile.
template <typename T>
inline static bool ReadOatDexFileData(const OatFile& oat_file,
                                      /*inout*/const uint8_t** oat,
                                      /*out*/T* value) {
  DCHECK(oat != nullptr);
  DCHECK(value != nullptr);
  DCHECK_LE(*oat, oat_file.End());
  if (UNLIKELY(static_cast<size_t>(oat_file.End() - *oat) < sizeof(T))) {
    return false;
  }
  static_assert(std::is_trivial<T>::value, "T must be a trivial type");
  using unaligned_type __attribute__((__aligned__(1))) = T;
  *value = *reinterpret_cast<const unaligned_type*>(*oat);
  *oat += sizeof(T);
  return true;
}

std::string OatFileBase::ErrorPrintf(const char* fmt, ...) {
  std::string error_msg = StringPrintf("In oat file '%s': ", GetLocation().c_str());
  va_list args;
  va_start(args, fmt);
  StringAppendV(&error_msg, fmt, args);
  va_end(args);
  return error_msg;
}

bool OatFileBase::ReadIndexBssMapping(/*inout*/const uint8_t** oat,
                                      const char* container_tag,
                                      size_t dex_file_index,
                                      const std::string& dex_file_location,
                                      const char* entry_tag,
                                      /*out*/const IndexBssMapping** mapping,
                                      std::string* error_msg) {
  uint32_t index_bss_mapping_offset;
  if (UNLIKELY(!ReadOatDexFileData(*this, oat, &index_bss_mapping_offset))) {
    *error_msg = ErrorPrintf("%s #%zd for '%s' truncated, missing %s bss mapping offset",
                             container_tag,
                             dex_file_index,
                             dex_file_location.c_str(),
                             entry_tag);
    return false;
  }
  const bool readable_index_bss_mapping_size =
      index_bss_mapping_offset != 0u &&
      index_bss_mapping_offset <= Size() &&
      IsAligned<alignof(IndexBssMapping)>(index_bss_mapping_offset) &&
      Size() - index_bss_mapping_offset >= IndexBssMapping::ComputeSize(0);
  const IndexBssMapping* index_bss_mapping = readable_index_bss_mapping_size
      ? reinterpret_cast<const IndexBssMapping*>(Begin() + index_bss_mapping_offset)
      : nullptr;
  if (index_bss_mapping_offset != 0u &&
      (UNLIKELY(index_bss_mapping == nullptr) ||
          UNLIKELY(index_bss_mapping->size() == 0u) ||
          UNLIKELY(Size() - index_bss_mapping_offset <
                   IndexBssMapping::ComputeSize(index_bss_mapping->size())))) {
    *error_msg = ErrorPrintf("%s #%zu for '%s' with unaligned or "
                                 "truncated %s bss mapping, offset %u of %zu, length %zu",
                             container_tag,
                             dex_file_index,
                             dex_file_location.c_str(),
                             entry_tag,
                             index_bss_mapping_offset,
                             Size(),
                             index_bss_mapping != nullptr ? index_bss_mapping->size() : 0u);
    return false;
  }

  *mapping = index_bss_mapping;
  return true;
}

bool OatFileBase::ReadBssMappingInfo(/*inout*/const uint8_t** oat,
                                     const char* container_tag,
                                     size_t dex_file_index,
                                     const std::string& dex_file_location,
                                     /*out*/BssMappingInfo* bss_mapping_info,
                                     std::string* error_msg) {
  auto read_index_bss_mapping = [&](const char* tag, /*out*/const IndexBssMapping** mapping) {
    return ReadIndexBssMapping(
        oat, container_tag, dex_file_index, dex_file_location, tag, mapping, error_msg);
  };
  return read_index_bss_mapping("method", &bss_mapping_info->method_bss_mapping) &&
         read_index_bss_mapping("type", &bss_mapping_info->type_bss_mapping) &&
         read_index_bss_mapping("public type", &bss_mapping_info->public_type_bss_mapping) &&
         read_index_bss_mapping("package type", &bss_mapping_info->package_type_bss_mapping) &&
         read_index_bss_mapping("string", &bss_mapping_info->string_bss_mapping) &&
         read_index_bss_mapping("method type", &bss_mapping_info->method_type_bss_mapping);
}

static bool ComputeAndCheckTypeLookupTableData(const DexFile::Header& header,
                                               const uint8_t* type_lookup_table_start,
                                               const VdexFile* vdex_file,
                                               const uint8_t** type_lookup_table_data,
                                               std::string* error_msg) {
  if (type_lookup_table_start == nullptr) {
    *type_lookup_table_data = nullptr;
    return true;
  }

  if (UNLIKELY(!vdex_file->Contains(type_lookup_table_start, sizeof(uint32_t)))) {
    *error_msg =
        StringPrintf("In vdex file '%s' found invalid type lookup table start %p of size %zu "
                         "not in [%p, %p]",
                     vdex_file->GetName().c_str(),
                     type_lookup_table_start,
                     sizeof(uint32_t),
                     vdex_file->Begin(),
                     vdex_file->End());
    return false;
  }

  size_t found_size = reinterpret_cast<const uint32_t*>(type_lookup_table_start)[0];
  size_t expected_table_size = TypeLookupTable::RawDataLength(header.class_defs_size_);
  if (UNLIKELY(found_size != expected_table_size)) {
    *error_msg =
        StringPrintf("In vdex file '%s' unexpected type lookup table size: found %zu, expected %zu",
                     vdex_file->GetName().c_str(),
                     found_size,
                     expected_table_size);
    return false;
  }

  if (found_size == 0) {
    *type_lookup_table_data = nullptr;
    return true;
  }

  *type_lookup_table_data = type_lookup_table_start + sizeof(uint32_t);
  if (UNLIKELY(!vdex_file->Contains(*type_lookup_table_data, found_size))) {
    *error_msg =
        StringPrintf("In vdex file '%s' found invalid type lookup table data %p of size %zu "
                         "not in [%p, %p]",
                     vdex_file->GetName().c_str(),
                     type_lookup_table_data,
                     found_size,
                     vdex_file->Begin(),
                     vdex_file->End());
    return false;
  }
  if (UNLIKELY(!IsAligned<4>(type_lookup_table_start))) {
    *error_msg =
        StringPrintf("In vdex file '%s' found invalid type lookup table alignment %p",
                     vdex_file->GetName().c_str(),
                     type_lookup_table_start);
    return false;
  }
  return true;
}

bool OatFileBase::Setup(const std::vector<const DexFile*>& dex_files, std::string* error_msg) {
  uint32_t i = 0;
  const uint8_t* type_lookup_table_start = nullptr;
  for (const DexFile* dex_file : dex_files) {
    // Defensively verify external dex file checksum. `OatFileAssistant`
    // expects this check to happen during oat file setup when the oat file
    // does not contain dex code.
    if (dex_file->GetLocationChecksum() != vdex_->GetLocationChecksum(i)) {
      *error_msg = StringPrintf("Dex checksum does not match for %s, dex has %d, vdex has %d",
                                dex_file->GetLocation().c_str(),
                                dex_file->GetLocationChecksum(),
                                vdex_->GetLocationChecksum(i));
      return false;
    }
    std::string dex_location = dex_file->GetLocation();
    std::string canonical_location = DexFileLoader::GetDexCanonicalLocation(dex_location.c_str());

    type_lookup_table_start = vdex_->GetNextTypeLookupTableData(type_lookup_table_start, i++);
    const uint8_t* type_lookup_table_data = nullptr;
    if (!ComputeAndCheckTypeLookupTableData(dex_file->GetHeader(),
                                            type_lookup_table_start,
                                            vdex_.get(),
                                            &type_lookup_table_data,
                                            error_msg)) {
      return false;
    }
    // Create an OatDexFile and add it to the owning container.
    OatDexFile* oat_dex_file = new OatDexFile(this,
                                              dex_file->GetContainer(),
                                              dex_file->Begin(),
                                              dex_file->GetHeader().magic_,
                                              dex_file->GetLocationChecksum(),
                                              dex_file->GetSha1(),
                                              dex_location,
                                              canonical_location,
                                              type_lookup_table_data);
    oat_dex_files_storage_.push_back(oat_dex_file);

    // Add the location and canonical location (if different) to the oat_dex_files_ table.
    std::string_view key(oat_dex_file->GetDexFileLocation());
    oat_dex_files_.Put(key, oat_dex_file);
    if (canonical_location != dex_location) {
      std::string_view canonical_key(oat_dex_file->GetCanonicalDexFileLocation());
      oat_dex_files_.Put(canonical_key, oat_dex_file);
    }
  }
  // Now that we've created all the OatDexFile, update the dex files.
  for (i = 0; i < dex_files.size(); ++i) {
    dex_files[i]->SetOatDexFile(oat_dex_files_storage_[i]);
  }
  return true;
}

bool OatFileBase::Setup(int zip_fd,
                        ArrayRef<const std::string> dex_filenames,
                        ArrayRef<File> dex_files,
                        std::string* error_msg) {
  if (!GetOatHeader().IsValid()) {
    std::string cause = GetOatHeader().GetValidationErrorMessage();
    *error_msg = ErrorPrintf("invalid oat header: %s", cause.c_str());
    return false;
  }
  PointerSize pointer_size = GetInstructionSetPointerSize(GetOatHeader().GetInstructionSet());
  size_t key_value_store_size =
      (Size() >= sizeof(OatHeader)) ? GetOatHeader().GetKeyValueStoreSize() : 0u;
  if (Size() < sizeof(OatHeader) + key_value_store_size) {
    *error_msg = ErrorPrintf("truncated oat header, size = %zu < %zu + %zu",
                             Size(),
                             sizeof(OatHeader),
                             key_value_store_size);
    return false;
  }

  size_t oat_dex_files_offset = GetOatHeader().GetOatDexFilesOffset();
  if (oat_dex_files_offset < GetOatHeader().GetHeaderSize() || oat_dex_files_offset > Size()) {
    *error_msg = ErrorPrintf("invalid oat dex files offset: %zu is not in [%zu, %zu]",
                             oat_dex_files_offset,
                             GetOatHeader().GetHeaderSize(),
                             Size());
    return false;
  }
  const uint8_t* oat = Begin() + oat_dex_files_offset;  // Jump to the OatDexFile records.

  if (!IsAligned<sizeof(uint32_t)>(data_img_rel_ro_begin_) ||
      !IsAligned<sizeof(uint32_t)>(data_img_rel_ro_end_) ||
      !IsAligned<sizeof(uint32_t)>(data_img_rel_ro_app_image_) ||
      data_img_rel_ro_begin_ > data_img_rel_ro_end_ ||
      data_img_rel_ro_begin_ > data_img_rel_ro_app_image_ ||
      data_img_rel_ro_app_image_ > data_img_rel_ro_end_) {
    *error_msg = ErrorPrintf(
        "unaligned or unordered databimgrelro symbol(s): begin = %p, end = %p, app_image = %p",
        data_img_rel_ro_begin_,
        data_img_rel_ro_end_,
        data_img_rel_ro_app_image_);
    return false;
  }

  DCHECK_GE(static_cast<size_t>(pointer_size), alignof(GcRoot<mirror::Object>));
  // In certain cases, ELF can be mapped at an address which is page aligned,
  // however not aligned to kElfSegmentAlignment. While technically this isn't
  // correct as per requirement in the ELF header, it has to be supported for
  // now. See also the comment at ImageHeader::RelocateImageReferences.
  if (!IsAlignedParam(bss_begin_, MemMap::GetPageSize()) ||
      !IsAlignedParam(bss_methods_, static_cast<size_t>(pointer_size)) ||
      !IsAlignedParam(bss_roots_, static_cast<size_t>(pointer_size)) ||
      !IsAligned<alignof(GcRoot<mirror::Object>)>(bss_end_)) {
    *error_msg = ErrorPrintf(
        "unaligned bss symbol(s): begin = %p, methods_ = %p, roots = %p, end = %p",
        bss_begin_,
        bss_methods_,
        bss_roots_,
        bss_end_);
    return false;
  }

  if ((bss_methods_ != nullptr && (bss_methods_ < bss_begin_ || bss_methods_ > bss_end_)) ||
      (bss_roots_ != nullptr && (bss_roots_ < bss_begin_ || bss_roots_ > bss_end_)) ||
      (bss_methods_ != nullptr && bss_roots_ != nullptr && bss_methods_ > bss_roots_)) {
    *error_msg = ErrorPrintf(
        "bss symbol(s) outside .bss or unordered: begin = %p, methods = %p, roots = %p, end = %p",
        bss_begin_,
        bss_methods_,
        bss_roots_,
        bss_end_);
    return false;
  }

  if (bss_methods_ != nullptr && bss_methods_ != bss_begin_) {
    *error_msg = ErrorPrintf("unexpected .bss gap before 'oatbssmethods': begin = %p, methods = %p",
                             bss_begin_,
                             bss_methods_);
    return false;
  }

  std::string_view primary_location;
  std::string_view primary_location_replacement;
  File no_file;
  File* dex_file = &no_file;
  size_t dex_filenames_pos = 0u;
  uint32_t dex_file_count = GetOatHeader().GetDexFileCount();
  oat_dex_files_storage_.reserve(dex_file_count);
  for (size_t i = 0; i < dex_file_count; i++) {
    uint32_t dex_file_location_size;
    if (UNLIKELY(!ReadOatDexFileData(*this, &oat, &dex_file_location_size))) {
      *error_msg = ErrorPrintf("OatDexFile #%zu truncated after dex file location size", i);
      return false;
    }
    if (UNLIKELY(dex_file_location_size == 0U)) {
      *error_msg = ErrorPrintf("OatDexFile #%zu with empty location name", i);
      return false;
    }
    if (UNLIKELY(static_cast<size_t>(End() - oat) < dex_file_location_size)) {
      *error_msg = ErrorPrintf("OatDexFile #%zu with truncated dex file location", i);
      return false;
    }
    const char* dex_file_location_data = reinterpret_cast<const char*>(oat);
    oat += dex_file_location_size;

    // Location encoded in the oat file. We will use this for multidex naming.
    std::string_view oat_dex_file_location(dex_file_location_data, dex_file_location_size);
    std::string dex_file_location(oat_dex_file_location);
    bool is_multidex = DexFileLoader::IsMultiDexLocation(dex_file_location);
    // Check that `is_multidex` does not clash with other indicators. The first dex location
    // must be primary location and, if we're opening external dex files, the location must
    // be multi-dex if and only if we already have a dex file opened for it.
    if ((i == 0 && is_multidex) ||
        (!external_dex_files_.empty() && (is_multidex != (i < external_dex_files_.size())))) {
      *error_msg = ErrorPrintf("unexpected %s location '%s'",
                               is_multidex ? "multi-dex" : "primary",
                               dex_file_location.c_str());
      return false;
    }
    // Remember the primary location and, if provided, the replacement from `dex_filenames`.
    if (!is_multidex) {
      primary_location = oat_dex_file_location;
      if (!dex_filenames.empty()) {
        if (dex_filenames_pos == dex_filenames.size()) {
          *error_msg = ErrorPrintf(
              "excessive primary location '%s', expected only %zu primary locations",
              dex_file_location.c_str(),
              dex_filenames.size());
          return false;
        }
        primary_location_replacement = dex_filenames[dex_filenames_pos];
        dex_file = dex_filenames_pos < dex_files.size() ? &dex_files[dex_filenames_pos] : &no_file;
        ++dex_filenames_pos;
      }
    }
    // Check that the base location of a multidex location matches the last seen primary location.
    if (is_multidex &&
        (!dex_file_location.starts_with(primary_location) ||
             dex_file_location[primary_location.size()] != DexFileLoader::kMultiDexSeparator)) {
      *error_msg = ErrorPrintf("unexpected multidex location '%s', unrelated to '%s'",
                               dex_file_location.c_str(),
                               std::string(primary_location).c_str());
      return false;
    }
    std::string dex_file_name = dex_file_location;
    if (!dex_filenames.empty()) {
      dex_file_name.replace(/*pos*/ 0u, primary_location.size(), primary_location_replacement);
      // If the location (the `--dex-location` passed to dex2oat) only contains the basename and
      // matches the basename in the provided file name, use the provided file name also as the
      // location.
      // This is needed when the location on device is unknown at compile-time, typically during
      // Cloud Compilation because the compilation is done on the server and the apk is later
      // installed on device into `/data/app/<random_string>`.
      // This is not needed during dexpreopt because the location on device is known to be a certain
      // location in /system, /product, etc.
      if (dex_file_location.find('/') == std::string::npos &&
          dex_file_name.size() > dex_file_location.size() &&
          dex_file_name[dex_file_name.size() - dex_file_location.size() - 1u] == '/' &&
          dex_file_name.ends_with(dex_file_location)) {
        dex_file_location = dex_file_name;
      }
    }

    DexFile::Magic dex_file_magic;
    if (UNLIKELY(!ReadOatDexFileData(*this, &oat, &dex_file_magic))) {
      *error_msg = ErrorPrintf("OatDexFile #%zu for '%s' truncated after dex file magic",
                               i,
                               dex_file_location.c_str());
      return false;
    }

    uint32_t dex_file_checksum;
    if (UNLIKELY(!ReadOatDexFileData(*this, &oat, &dex_file_checksum))) {
      *error_msg = ErrorPrintf("OatDexFile #%zu for '%s' truncated after dex file checksum",
                               i,
                               dex_file_location.c_str());
      return false;
    }

    DexFile::Sha1 dex_file_sha1;
    if (UNLIKELY(!ReadOatDexFileData(*this, &oat, &dex_file_sha1))) {
      *error_msg = ErrorPrintf("OatDexFile #%zu for '%s' truncated after dex file sha1",
                               i,
                               dex_file_location.c_str());
      return false;
    }

    uint32_t dex_file_offset;
    if (UNLIKELY(!ReadOatDexFileData(*this, &oat, &dex_file_offset))) {
      *error_msg = ErrorPrintf("OatDexFile #%zu for '%s' truncated after dex file offsets",
                               i,
                               dex_file_location.c_str());
      return false;
    }
    if (UNLIKELY(dex_file_offset > DexSize())) {
      *error_msg = ErrorPrintf("OatDexFile #%zu for '%s' with dex file offset %u > %zu",
                               i,
                               dex_file_location.c_str(),
                               dex_file_offset,
                               DexSize());
      return false;
    }
    std::shared_ptr<DexFileContainer> dex_file_container;
    const uint8_t* dex_file_pointer = nullptr;
    if (UNLIKELY(dex_file_offset == 0U)) {
      // Do not support mixed-mode oat files.
      if (i != 0u && external_dex_files_.empty()) {
        *error_msg = ErrorPrintf("unsupported uncompressed-dex-file for dex file %zu (%s)",
                                 i,
                                 dex_file_location.c_str());
        return false;
      }
      DCHECK_LE(i, external_dex_files_.size());
      if (i == external_dex_files_.size()) {
        std::vector<std::unique_ptr<const DexFile>> new_dex_files;
        // No dex files, load it from location.
        bool loaded = false;
        CHECK(zip_fd == -1 || dex_files.empty());  // Allow only the supported combinations.
        if (zip_fd != -1) {
          File file(zip_fd, /*check_usage=*/false);
          ArtDexFileLoader dex_file_loader(&file, dex_file_location);
          loaded = dex_file_loader.Open(
              /*verify=*/false, /*verify_checksum=*/false, error_msg, &new_dex_files);
        } else if (dex_file->IsValid()) {
          // Note that we assume dex_fds are backing by jars.
          ArtDexFileLoader dex_file_loader(dex_file, dex_file_location);
          loaded = dex_file_loader.Open(
              /*verify=*/false, /*verify_checksum=*/false, error_msg, &new_dex_files);
        } else {
          ArtDexFileLoader dex_file_loader(dex_file_name.c_str(), dex_file_location);
          loaded = dex_file_loader.Open(
              /*verify=*/false, /*verify_checksum=*/false, error_msg, &new_dex_files);
        }
        if (!loaded) {
          if (Runtime::Current() == nullptr) {
            // If there's no runtime, we're running oatdump, so return
            // a half constructed oat file that oatdump knows how to deal with.
            LOG(WARNING) << "Could not find associated dex files of oat file. "
                         << "Oatdump will only dump the header.";
            return true;
          }
          return false;
        }
        // The oat file may be out of date wrt/ the dex-file location. We need to be defensive
        // here and ensure that at least the number of dex files still matches.
        // If we have a zip_fd, or reached the end of provided `dex_filenames`, we must
        // load all dex files from that file, otherwise we may open multiple files.
        // Note: actual checksum comparisons are the duty of the OatFileAssistant and will be
        //       done after loading the OatFile.
        size_t max_dex_files = dex_file_count - external_dex_files_.size();
        bool expect_all =
            (zip_fd != -1) || (!dex_filenames.empty() && dex_filenames_pos == dex_filenames.size());
        if (expect_all ? new_dex_files.size() != max_dex_files
                       : new_dex_files.size() > max_dex_files) {
          *error_msg = ErrorPrintf("expected %s%zu uncompressed dex files, but found %zu in '%s'",
                                   (expect_all ? "" : "<="),
                                   max_dex_files,
                                   new_dex_files.size(),
                                   dex_file_location.c_str());
          return false;
        }
        for (std::unique_ptr<const DexFile>& dex_file_ptr : new_dex_files) {
          external_dex_files_.push_back(std::move(dex_file_ptr));
        }
      }
      // Defensively verify external dex file checksum. `OatFileAssistant`
      // expects this check to happen during oat file setup when the oat file
      // does not contain dex code.
      if (dex_file_checksum != external_dex_files_[i]->GetLocationChecksum()) {
        CHECK(dex_file_sha1 != external_dex_files_[i]->GetSha1());
        *error_msg = ErrorPrintf("dex file checksum 0x%08x does not match"
                                     " checksum 0x%08x of external dex file '%s'",
                                 dex_file_checksum,
                                 external_dex_files_[i]->GetLocationChecksum(),
                                 external_dex_files_[i]->GetLocation().c_str());
        return false;
      }
      CHECK(dex_file_sha1 == external_dex_files_[i]->GetSha1());
      dex_file_container = external_dex_files_[i]->GetContainer();
      dex_file_pointer = external_dex_files_[i]->Begin();
    } else {
      // Do not support mixed-mode oat files.
      if (!external_dex_files_.empty()) {
        *error_msg = ErrorPrintf("unsupported embedded dex-file for dex file %zu (%s)",
                                 i,
                                 dex_file_location.c_str());
        return false;
      }
      if (UNLIKELY(DexSize() - dex_file_offset < sizeof(DexFile::Header))) {
        *error_msg = ErrorPrintf("OatDexFile #%zu for '%s' with dex file "
                                     "offset %u of %zu but the size of dex file header is %zu",
                                 i,
                                 dex_file_location.c_str(),
                                 dex_file_offset,
                                 DexSize(),
                                 sizeof(DexFile::Header));
        return false;
      }
      dex_file_container = std::make_shared<MemoryDexFileContainer>(DexBegin(), DexEnd());
      dex_file_pointer = DexBegin() + dex_file_offset;
    }

    const bool valid_magic = DexFileLoader::IsMagicValid(dex_file_pointer);
    if (UNLIKELY(!valid_magic)) {
      *error_msg = ErrorPrintf("OatDexFile #%zu for '%s' with invalid dex file magic",
                               i,
                               dex_file_location.c_str());
      return false;
    }
    if (UNLIKELY(!DexFileLoader::IsVersionAndMagicValid(dex_file_pointer))) {
      *error_msg = ErrorPrintf("OatDexFile #%zu for '%s' with invalid dex file version",
                               i,
                               dex_file_location.c_str());
      return false;
    }
    const DexFile::Header* header = reinterpret_cast<const DexFile::Header*>(dex_file_pointer);
    if (dex_file_offset != 0 && (DexSize() - dex_file_offset < header->file_size_)) {
      *error_msg = ErrorPrintf(
          "OatDexFile #%zu for '%s' with dex file offset %u and size %u truncated at %zu",
          i,
          dex_file_location.c_str(),
          dex_file_offset,
          header->file_size_,
          DexSize());
      return false;
    }

    uint32_t class_offsets_offset;
    if (UNLIKELY(!ReadOatDexFileData(*this, &oat, &class_offsets_offset))) {
      *error_msg = ErrorPrintf("OatDexFile #%zu for '%s' truncated after class offsets offset",
                               i,
                               dex_file_location.c_str());
      return false;
    }
    if (UNLIKELY(class_offsets_offset > Size()) ||
        UNLIKELY((Size() - class_offsets_offset) / sizeof(uint32_t) < header->class_defs_size_)) {
      *error_msg = ErrorPrintf("OatDexFile #%zu for '%s' with truncated "
                                   "class offsets, offset %u of %zu, class defs %u",
                               i,
                               dex_file_location.c_str(),
                               class_offsets_offset,
                               Size(),
                               header->class_defs_size_);
      return false;
    }
    if (UNLIKELY(!IsAligned<alignof(uint32_t)>(class_offsets_offset))) {
      *error_msg = ErrorPrintf("OatDexFile #%zu for '%s' with unaligned "
                                   "class offsets, offset %u",
                               i,
                               dex_file_location.c_str(),
                               class_offsets_offset);
      return false;
    }
    const uint32_t* class_offsets_pointer =
        reinterpret_cast<const uint32_t*>(Begin() + class_offsets_offset);

    uint32_t lookup_table_offset;
    if (UNLIKELY(!ReadOatDexFileData(*this, &oat, &lookup_table_offset))) {
      *error_msg = ErrorPrintf("OatDexFile #%zd for '%s' truncated after lookup table offset",
                               i,
                               dex_file_location.c_str());
      return false;
    }
    const uint8_t* lookup_table_data = lookup_table_offset != 0u
        ? DexBegin() + lookup_table_offset
        : nullptr;
    if (lookup_table_offset != 0u &&
        (UNLIKELY(lookup_table_offset > DexSize()) ||
            UNLIKELY(DexSize() - lookup_table_offset <
                     TypeLookupTable::RawDataLength(header->class_defs_size_)))) {
      *error_msg = ErrorPrintf("OatDexFile #%zu for '%s' with truncated type lookup table, "
                                   "offset %u of %zu, class defs %u",
                               i,
                               dex_file_location.c_str(),
                               lookup_table_offset,
                               Size(),
                               header->class_defs_size_);
      return false;
    }

    uint32_t dex_layout_sections_offset;
    if (UNLIKELY(!ReadOatDexFileData(*this, &oat, &dex_layout_sections_offset))) {
      *error_msg = ErrorPrintf(
          "OatDexFile #%zd for '%s' truncated after dex layout sections offset",
          i,
          dex_file_location.c_str());
      return false;
    }
    const DexLayoutSections* const dex_layout_sections = dex_layout_sections_offset != 0
        ? reinterpret_cast<const DexLayoutSections*>(Begin() + dex_layout_sections_offset)
        : nullptr;

    BssMappingInfo bss_mapping_info;
    if (!ReadBssMappingInfo(
            &oat, "OatDexFile", i, dex_file_location, &bss_mapping_info, error_msg)) {
      return false;
    }

    // Create the OatDexFile and add it to the owning container.
    OatDexFile* oat_dex_file =
        new OatDexFile(this,
                       dex_file_location,
                       DexFileLoader::GetDexCanonicalLocation(dex_file_name.c_str()),
                       dex_file_magic,
                       dex_file_checksum,
                       dex_file_sha1,
                       dex_file_container,
                       dex_file_pointer,
                       lookup_table_data,
                       bss_mapping_info,
                       class_offsets_pointer,
                       dex_layout_sections);
    oat_dex_files_storage_.push_back(oat_dex_file);

    // Add the location and canonical location (if different) to the oat_dex_files_ table.
    // Note: We do not add the non-canonical `dex_file_name`. If it is different from both
    // the location and canonical location, GetOatDexFile() shall canonicalize it when
    // requested and match the canonical path.
    std::string_view key = oat_dex_file_location;  // References oat file data.
    std::string_view canonical_key(oat_dex_file->GetCanonicalDexFileLocation());
    oat_dex_files_.Put(key, oat_dex_file);
    if (canonical_key != key) {
      oat_dex_files_.Put(canonical_key, oat_dex_file);
    }
  }

  size_t bcp_info_offset = GetOatHeader().GetBcpBssInfoOffset();
  // `bcp_info_offset` will be 0 for multi-image, or for the case of no mappings.
  if (bcp_info_offset != 0) {
    // Consistency check.
    if (bcp_info_offset < GetOatHeader().GetHeaderSize() || bcp_info_offset > Size()) {
      *error_msg = ErrorPrintf("invalid bcp info offset: %zu is not in [%zu, %zu]",
                               bcp_info_offset,
                               GetOatHeader().GetHeaderSize(),
                               Size());
      return false;
    }
    const uint8_t* bcp_info_begin = Begin() + bcp_info_offset;  // Jump to the BCP_info records.

    uint32_t number_of_bcp_dexfiles;
    if (UNLIKELY(!ReadOatDexFileData(*this, &bcp_info_begin, &number_of_bcp_dexfiles))) {
      *error_msg = ErrorPrintf("failed to read the number of BCP dex files");
      return false;
    }
    Runtime* const runtime = Runtime::Current();
    ClassLinker* const linker = runtime != nullptr ? runtime->GetClassLinker() : nullptr;
    if (linker != nullptr && UNLIKELY(number_of_bcp_dexfiles > linker->GetBootClassPath().size())) {
      // If we compiled with more DexFiles than what we have at runtime, we expect to discard this
      // OatFile after verifying its checksum in OatFileAssistant. Therefore, we set
      // `number_of_bcp_dexfiles` to 0 to avoid reading data that will ultimately be discarded.
      number_of_bcp_dexfiles = 0;
    }

    DCHECK(bcp_bss_info_.empty());
    bcp_bss_info_.resize(number_of_bcp_dexfiles);
    // At runtime, there might be more DexFiles added to the BCP that we didn't compile with.
    // We only care about the ones in [0..number_of_bcp_dexfiles).
    for (size_t i = 0, size = number_of_bcp_dexfiles; i != size; ++i) {
      const std::string& dex_file_location = linker != nullptr
          ? linker->GetBootClassPath()[i]->GetLocation()
          : "No runtime/linker therefore no DexFile location";
      if (!ReadBssMappingInfo(
              &bcp_info_begin, "BcpBssInfo", i, dex_file_location, &bcp_bss_info_[i], error_msg)) {
        return false;
      }
    }
  }

  if (!dex_filenames.empty() && dex_filenames_pos != dex_filenames.size()) {
    *error_msg = ErrorPrintf("only %zu primary dex locations, expected %zu",
                             dex_filenames_pos,
                             dex_filenames.size());
    return false;
  }

  if (DataImgRelRoBegin() != nullptr) {
    // Make .data.img.rel.ro read only. ClassLinker shall temporarily make it writable for
    // relocation when we register a dex file from this oat file. We do not do the relocation
    // here to avoid dirtying the pages if the code is never actually ready to be executed.
    uint8_t* reloc_begin = const_cast<uint8_t*>(DataImgRelRoBegin());
    CheckedCall(mprotect, "protect relocations", reloc_begin, DataImgRelRoSize(), PROT_READ);
    // Make sure the file lists a boot image dependency, otherwise the .data.img.rel.ro
    // section is bogus. The full dependency is checked before the code is executed.
    // We cannot do this check if we do not have a key-value store, i.e. for secondary
    // oat files for boot image extensions.
    if (GetOatHeader().GetKeyValueStoreSize() != 0u) {
      const char* boot_class_path_checksum =
          GetOatHeader().GetStoreValueByKey(OatHeader::kBootClassPathChecksumsKey);
      if (boot_class_path_checksum == nullptr ||
          boot_class_path_checksum[0] != gc::space::ImageSpace::kImageChecksumPrefix) {
        *error_msg = ErrorPrintf(".data.img.rel.ro section present without boot image dependency.");
        return false;
      }
    }
  }

  return true;
}

////////////////////////
// OatFile via dlopen //
////////////////////////

class DlOpenOatFile final : public OatFileBase {
 public:
  DlOpenOatFile(const std::string& filename, bool executable)
      : OatFileBase(filename, executable),
        dlopen_handle_(nullptr),
        shared_objects_before_(0) {
  }

  ~DlOpenOatFile() {
    if (dlopen_handle_ != nullptr) {
      if (!kIsTargetBuild) {
        MutexLock mu(Thread::Current(), *Locks::host_dlopen_handles_lock_);
        host_dlopen_handles_.erase(dlopen_handle_);
        dlclose(dlopen_handle_);
      } else {
        dlclose(dlopen_handle_);
      }
    }
  }

 protected:
  const uint8_t* FindDynamicSymbolAddress(const std::string& symbol_name,
                                          std::string* error_msg) const override {
    const uint8_t* ptr =
        reinterpret_cast<const uint8_t*>(dlsym(dlopen_handle_, symbol_name.c_str()));
    if (ptr == nullptr) {
      *error_msg = dlerror();
    }
    return ptr;
  }

  void PreLoad() override;

  bool Load(const std::string& elf_filename,
            bool executable,
            bool low_4gb,
            /*inout*/ MemMap* reservation,  // Where to load if not null.
            /*out*/ std::string* error_msg) override;

  bool Load([[maybe_unused]] int oat_fd,
            [[maybe_unused]] bool executable,
            [[maybe_unused]] bool low_4gb,
            [[maybe_unused]] /*inout*/ MemMap* reservation,
            [[maybe_unused]] /*out*/ std::string* error_msg) override {
    return false;
  }

  // Ask the linker where it mmaped the file and notify our mmap wrapper of the regions.
  void PreSetup(const std::string& elf_filename) override;

  const uint8_t* ComputeElfBegin(std::string* error_msg) const override {
    Dl_info info;
    if (dladdr(Begin(), &info) == 0) {
      *error_msg =
          StringPrintf("Failed to dladdr '%s': %s", GetLocation().c_str(), strerror(errno));
      return nullptr;
    }
    return reinterpret_cast<const uint8_t*>(info.dli_fbase);
  }

 private:
  bool Dlopen(const std::string& elf_filename,
              /*inout*/MemMap* reservation,  // Where to load if not null.
              /*out*/std::string* error_msg);

  // On the host, if the same library is loaded again with dlopen the same
  // file handle is returned. This differs from the behavior of dlopen on the
  // target, where dlopen reloads the library at a different address every
  // time you load it. The runtime relies on the target behavior to ensure
  // each instance of the loaded library has a unique dex cache. To avoid
  // problems, we fall back to our own linker in the case when the same
  // library is opened multiple times on host. dlopen_handles_ is used to
  // detect that case.
  // Guarded by host_dlopen_handles_lock_;
  static std::unordered_set<void*> host_dlopen_handles_;

  // Reservation and placeholder memory map objects corresponding to the regions mapped by dlopen.
  // Note: Must be destroyed after dlclose() as it can hold the owning reservation.
  std::vector<MemMap> dlopen_mmaps_;

  // dlopen handle during runtime.
  void* dlopen_handle_;  // TODO: Unique_ptr with custom deleter.

  // The number of shared objects the linker told us about before loading. Used to
  // (optimistically) optimize the PreSetup stage (see comment there).
  size_t shared_objects_before_;

  DISALLOW_COPY_AND_ASSIGN(DlOpenOatFile);
};

std::unordered_set<void*> DlOpenOatFile::host_dlopen_handles_;

void DlOpenOatFile::PreLoad() {
#ifdef __APPLE__
  UNUSED(shared_objects_before_);
  LOG(FATAL) << "Should not reach here.";
  UNREACHABLE();
#else
  // Count the entries in dl_iterate_phdr we get at this point in time.
  struct dl_iterate_context {
    static int callback([[maybe_unused]] dl_phdr_info* info,
                        [[maybe_unused]] size_t size,
                        void* data) {
      reinterpret_cast<dl_iterate_context*>(data)->count++;
      return 0;  // Continue iteration.
    }
    size_t count = 0;
  } context;

  dl_iterate_phdr(dl_iterate_context::callback, &context);
  shared_objects_before_ = context.count;
#endif
}

bool DlOpenOatFile::Load(const std::string& elf_filename,
                         bool executable,
                         bool low_4gb,
                         /*inout*/ MemMap* reservation,  // Where to load if not null.
                         /*out*/ std::string* error_msg) {
  // Use dlopen only when flagged to do so, and when it's OK to load things executable.
  // TODO: Also try when not executable? The issue here could be re-mapping as writable (as
  //       !executable is a sign that we may want to patch), which may not be allowed for
  //       various reasons.
  if (!kUseDlopen) {
    *error_msg = "DlOpen is disabled.";
    return false;
  }
  if (low_4gb) {
    *error_msg = "DlOpen does not support low 4gb loading.";
    return false;
  }
  if (!executable) {
    *error_msg = "DlOpen does not support non-executable loading.";
    return false;
  }

  if (!IsReadOnlyDynamicSupportedByDlOpen()) {
    *error_msg = "DlOpen does not support read-only .dynamic section.";
    return false;
  }

  // dlopen always returns the same library if it is already opened on the host. For this reason
  // we only use dlopen if we are the target or we do not already have the dex file opened. Having
  // the same library loaded multiple times at different addresses is required for class unloading
  // and for having dex caches arrays in the .bss section.
  if (!kIsTargetBuild) {
    if (!kUseDlopenOnHost) {
      *error_msg = "DlOpen disabled for host.";
      return false;
    }
  }

  bool success = Dlopen(elf_filename, reservation, error_msg);
  DCHECK_IMPLIES(dlopen_handle_ == nullptr, !success);

  return success;
}

#ifdef ART_TARGET_ANDROID
static struct android_namespace_t* GetSystemLinkerNamespace() {
  static struct android_namespace_t* system_ns = []() {
    // The system namespace is called "default" for binaries in /system and
    // "system" for those in the ART APEX. Try "system" first since "default"
    // always exists.
    // TODO(b/185587109): Get rid of this error prone logic.
    struct android_namespace_t* ns = android_get_exported_namespace("system");
    if (ns == nullptr) {
      ns = android_get_exported_namespace("default");
      if (ns == nullptr) {
        LOG(FATAL) << "Failed to get system namespace for loading OAT files";
      }
    }
    return ns;
  }();
  return system_ns;
}
#endif  // ART_TARGET_ANDROID

bool DlOpenOatFile::Dlopen(const std::string& elf_filename,
                           /*inout*/MemMap* reservation,
                           /*out*/std::string* error_msg) {
#ifdef __APPLE__
  // The dl_iterate_phdr syscall is missing.  There is similar API on OSX,
  // but let's fallback to the custom loading code for the time being.
  UNUSED(elf_filename, reservation);
  *error_msg = "Dlopen unsupported on Mac.";
  return false;
#else
  {
    // `elf_filename` is in the format of `/path/to/oat` or `/path/to/zip!/primary.odex`. We can
    // reuse `GetDexCanonicalLocation` to resolve the real path of the part before "!" even though
    // `elf_filename` does not refer to a dex file.
    static_assert(std::string_view(kZipSeparator).starts_with(DexFileLoader::kMultiDexSeparator));
    std::string absolute_path = DexFileLoader::GetDexCanonicalLocation(elf_filename.c_str());
#ifdef ART_TARGET_ANDROID
    android_dlextinfo extinfo = {};
    extinfo.flags = ANDROID_DLEXT_FORCE_LOAD;   // Force-load, don't reuse handle
                                                //   (open oat files multiple times).
    if (reservation != nullptr) {
      if (!reservation->IsValid()) {
        *error_msg = StringPrintf("Invalid reservation for %s", elf_filename.c_str());
        return false;
      }
      extinfo.flags |= ANDROID_DLEXT_RESERVED_ADDRESS;          // Use the reserved memory range.
      extinfo.reserved_addr = reservation->Begin();
      extinfo.reserved_size = reservation->Size();
    }

    if (strncmp(kAndroidArtApexDefaultPath,
                absolute_path.c_str(),
                sizeof(kAndroidArtApexDefaultPath) - 1) != 0 ||
        absolute_path.c_str()[sizeof(kAndroidArtApexDefaultPath) - 1] != '/') {
      // Use the system namespace for OAT files outside the ART APEX. Search
      // paths and links don't matter here, but permitted paths do, and the
      // system namespace is configured to allow loading from all appropriate
      // locations.
      extinfo.flags |= ANDROID_DLEXT_USE_NAMESPACE;
      extinfo.library_namespace = GetSystemLinkerNamespace();
    }

    dlopen_handle_ = android_dlopen_ext(absolute_path.c_str(), RTLD_NOW, &extinfo);
    if (reservation != nullptr && dlopen_handle_ != nullptr) {
      // Find used pages from the reservation.
      struct dl_iterate_context {
        static int callback(dl_phdr_info* info, [[maybe_unused]] size_t size, void* data) {
          auto* context = reinterpret_cast<dl_iterate_context*>(data);
          static_assert(std::is_same<Elf32_Half, Elf64_Half>::value, "Half must match");
          using Elf_Half = Elf64_Half;

          // See whether this callback corresponds to the file which we have just loaded.
          uint8_t* reservation_begin = context->reservation->Begin();
          bool contained_in_reservation = false;
          for (Elf_Half i = 0; i < info->dlpi_phnum; i++) {
            if (info->dlpi_phdr[i].p_type == PT_LOAD) {
              uint8_t* vaddr = reinterpret_cast<uint8_t*>(info->dlpi_addr +
                  info->dlpi_phdr[i].p_vaddr);
              size_t memsz = info->dlpi_phdr[i].p_memsz;
              size_t offset = static_cast<size_t>(vaddr - reservation_begin);
              if (offset < context->reservation->Size()) {
                contained_in_reservation = true;
                DCHECK_LE(memsz, context->reservation->Size() - offset);
              } else if (vaddr < reservation_begin) {
                // Check that there's no overlap with the reservation.
                DCHECK_LE(memsz, static_cast<size_t>(reservation_begin - vaddr));
              }
              break;  // It is sufficient to check the first PT_LOAD header.
            }
          }

          if (contained_in_reservation) {
            for (Elf_Half i = 0; i < info->dlpi_phnum; i++) {
              if (info->dlpi_phdr[i].p_type == PT_LOAD) {
                uint8_t* vaddr = reinterpret_cast<uint8_t*>(info->dlpi_addr +
                    info->dlpi_phdr[i].p_vaddr);
                size_t memsz = info->dlpi_phdr[i].p_memsz;
                size_t offset = static_cast<size_t>(vaddr - reservation_begin);
                DCHECK_LT(offset, context->reservation->Size());
                DCHECK_LE(memsz, context->reservation->Size() - offset);
                context->max_size = std::max(context->max_size, offset + memsz);
              }
            }

            return 1;  // Stop iteration and return 1 from dl_iterate_phdr.
          }
          return 0;  // Continue iteration and return 0 from dl_iterate_phdr when finished.
        }

        const MemMap* const reservation;
        size_t max_size = 0u;
      };
      dl_iterate_context context = { reservation };

      if (dl_iterate_phdr(dl_iterate_context::callback, &context) == 0) {
        LOG(FATAL) << "Could not find the shared object mmapped to the reservation.";
        UNREACHABLE();
      }

      // Take ownership of the memory used by the shared object. dlopen() does not assume
      // full ownership of this memory and dlclose() shall just remap it as zero pages with
      // PROT_NONE. We need to unmap the memory when destroying this oat file.
      // The reserved memory size is aligned up to kElfSegmentAlignment to ensure
      // that the next reserved area will be aligned to the value.
      dlopen_mmaps_.push_back(reservation->TakeReservedMemory(
          CondRoundUp<kPageSizeAgnostic>(context.max_size, kElfSegmentAlignment)));
    }
#else
    static_assert(!kIsTargetBuild || kIsTargetLinux || kIsTargetFuchsia,
                  "host_dlopen_handles_ will leak handles");
    if (reservation != nullptr) {
      *error_msg = StringPrintf("dlopen() into reserved memory is unsupported on host for '%s'.",
                                elf_filename.c_str());
      return false;
    }
    MutexLock mu(Thread::Current(), *Locks::host_dlopen_handles_lock_);
    dlopen_handle_ = dlopen(absolute_path.c_str(), RTLD_NOW);
    if (dlopen_handle_ != nullptr) {
      if (!host_dlopen_handles_.insert(dlopen_handle_).second) {
        dlclose(dlopen_handle_);
        dlopen_handle_ = nullptr;
        *error_msg = StringPrintf("host dlopen re-opened '%s'", elf_filename.c_str());
        return false;
      }
    }
#endif  // ART_TARGET_ANDROID
  }
  if (dlopen_handle_ == nullptr) {
    *error_msg = StringPrintf("Failed to dlopen '%s': %s", elf_filename.c_str(), dlerror());
    return false;
  }
  return true;
#endif
}

void DlOpenOatFile::PreSetup(const std::string& elf_filename) {
#ifdef __APPLE__
  UNUSED(elf_filename);
  LOG(FATAL) << "Should not reach here.";
  UNREACHABLE();
#else
  struct PlaceholderMapData {
    const char* name;
    uint8_t* vaddr;
    size_t memsz;
  };
  struct dl_iterate_context {
    static int callback(dl_phdr_info* info, [[maybe_unused]] size_t size, void* data) {
      auto* context = reinterpret_cast<dl_iterate_context*>(data);
      static_assert(std::is_same<Elf32_Half, Elf64_Half>::value, "Half must match");
      using Elf_Half = Elf64_Half;

      context->shared_objects_seen++;
      if (context->shared_objects_seen < context->shared_objects_before) {
        // We haven't been called yet for anything we haven't seen before. Just continue.
        // Note: this is aggressively optimistic. If another thread was unloading a library,
        //       we may miss out here. However, this does not happen often in practice.
        return 0;
      }

      // See whether this callback corresponds to the file which we have just loaded.
      bool contains_begin = false;
      for (Elf_Half i = 0; i < info->dlpi_phnum; i++) {
        if (info->dlpi_phdr[i].p_type == PT_LOAD) {
          uint8_t* vaddr = reinterpret_cast<uint8_t*>(info->dlpi_addr +
              info->dlpi_phdr[i].p_vaddr);
          size_t memsz = info->dlpi_phdr[i].p_memsz;
          if (vaddr <= context->begin_ && context->begin_ < vaddr + memsz) {
            contains_begin = true;
            break;
          }
        }
      }
      // Add placeholder mmaps for this file.
      if (contains_begin) {
        for (Elf_Half i = 0; i < info->dlpi_phnum; i++) {
          if (info->dlpi_phdr[i].p_type == PT_LOAD) {
            uint8_t* vaddr = reinterpret_cast<uint8_t*>(info->dlpi_addr +
                info->dlpi_phdr[i].p_vaddr);
            size_t memsz = info->dlpi_phdr[i].p_memsz;
            size_t name_size = strlen(info->dlpi_name) + 1u;
            std::vector<char>* placeholder_maps_names = context->placeholder_maps_names_;
            // We must not allocate any memory in the callback, see b/156312036 .
            if (name_size < placeholder_maps_names->capacity() - placeholder_maps_names->size() &&
                context->placeholder_maps_data_->size() <
                    context->placeholder_maps_data_->capacity()) {
              placeholder_maps_names->insert(
                  placeholder_maps_names->end(), info->dlpi_name, info->dlpi_name + name_size);
              const char* name =
                  &(*placeholder_maps_names)[placeholder_maps_names->size() - name_size];
              context->placeholder_maps_data_->push_back({ name, vaddr, memsz });
            }
            context->num_placeholder_maps_ += 1u;
            context->placeholder_maps_names_size_ += name_size;
          }
        }
        return 1;  // Stop iteration and return 1 from dl_iterate_phdr.
      }
      return 0;  // Continue iteration and return 0 from dl_iterate_phdr when finished.
    }
    const uint8_t* const begin_;
    std::vector<PlaceholderMapData>* placeholder_maps_data_;
    size_t num_placeholder_maps_;
    std::vector<char>* placeholder_maps_names_;
    size_t placeholder_maps_names_size_;
    size_t shared_objects_before;
    size_t shared_objects_seen;
  };

  // We must not allocate any memory in the callback, see b/156312036 .
  // Therefore we pre-allocate storage for the data we need for creating the placeholder maps.
  std::vector<PlaceholderMapData> placeholder_maps_data;
  placeholder_maps_data.reserve(32);  // 32 should be enough. If not, we'll retry.
  std::vector<char> placeholder_maps_names;
  placeholder_maps_names.reserve(4 * KB);  // 4KiB should be enough. If not, we'll retry.

  dl_iterate_context context = {
      Begin(),
      &placeholder_maps_data,
      /*num_placeholder_maps_*/ 0u,
      &placeholder_maps_names,
      /*placeholder_maps_names_size_*/ 0u,
      shared_objects_before_,
      /*shared_objects_seen*/ 0u
  };

  if (dl_iterate_phdr(dl_iterate_context::callback, &context) == 0) {
    // Hm. Maybe our optimization went wrong. Try another time with shared_objects_before == 0
    // before giving up. This should be unusual.
    VLOG(oat) << "Need a second run in PreSetup, didn't find with shared_objects_before="
              << shared_objects_before_;
    DCHECK(placeholder_maps_data.empty());
    DCHECK_EQ(context.num_placeholder_maps_, 0u);
    DCHECK(placeholder_maps_names.empty());
    DCHECK_EQ(context.placeholder_maps_names_size_, 0u);
    context.shared_objects_before = 0u;
    context.shared_objects_seen = 0u;
    if (dl_iterate_phdr(dl_iterate_context::callback, &context) == 0) {
      // OK, give up and print an error.
      PrintFileToLog("/proc/self/maps", android::base::LogSeverity::WARNING);
      LOG(ERROR) << "File " << elf_filename << " loaded with dlopen but cannot find its mmaps.";
    }
  }

  if (placeholder_maps_data.size() < context.num_placeholder_maps_) {
    // Insufficient capacity. Reserve more space and retry.
    placeholder_maps_data.clear();
    placeholder_maps_data.reserve(context.num_placeholder_maps_);
    context.num_placeholder_maps_ = 0u;
    placeholder_maps_names.clear();
    placeholder_maps_names.reserve(context.placeholder_maps_names_size_);
    context.placeholder_maps_names_size_ = 0u;
    context.shared_objects_before = 0u;
    context.shared_objects_seen = 0u;
    bool success = (dl_iterate_phdr(dl_iterate_context::callback, &context) != 0);
    CHECK(success);
  }

  CHECK_EQ(placeholder_maps_data.size(), context.num_placeholder_maps_);
  CHECK_EQ(placeholder_maps_names.size(), context.placeholder_maps_names_size_);
  DCHECK_EQ(static_cast<size_t>(std::count(placeholder_maps_names.begin(),
                                           placeholder_maps_names.end(), '\0')),
            context.num_placeholder_maps_);
  for (const PlaceholderMapData& data : placeholder_maps_data) {
    MemMap mmap = MemMap::MapPlaceholder(data.name, data.vaddr, data.memsz);
    dlopen_mmaps_.push_back(std::move(mmap));
  }
#endif
}

////////////////////////////////////////////////
// OatFile via our own ElfFile implementation //
////////////////////////////////////////////////

class ElfOatFile final : public OatFileBase {
 public:
  ElfOatFile(const std::string& filename, bool executable) : OatFileBase(filename, executable) {}

 protected:
  const uint8_t* FindDynamicSymbolAddress(const std::string& symbol_name,
                                          std::string* error_msg) const override {
    const uint8_t* ptr = elf_file_->FindDynamicSymbolAddress(symbol_name);
    if (ptr == nullptr) {
      *error_msg = "(Internal implementation could not find symbol)";
    }
    return ptr;
  }

  void PreLoad() override {
  }

  bool Load(const std::string& elf_filename,
            bool executable,
            bool low_4gb,
            /*inout*/ MemMap* reservation,  // Where to load if not null.
            /*out*/ std::string* error_msg) override;

  bool Load(int oat_fd,
            bool executable,
            bool low_4gb,
            /*inout*/ MemMap* reservation,  // Where to load if not null.
            /*out*/ std::string* error_msg) override;

  void PreSetup([[maybe_unused]] const std::string& elf_filename) override {}

  const uint8_t* ComputeElfBegin(std::string*) const override {
    return elf_file_->GetBaseAddress();
  }

 private:
  bool ElfFileOpen(File* file,
                   off_t start,
                   size_t file_length,
                   const std::string& file_location,
                   bool executable,
                   bool low_4gb,
                   /*inout*/ MemMap* reservation,  // Where to load if not null.
                   /*out*/ std::string* error_msg);

 private:
  // Backing memory map for oat file during cross compilation.
  std::unique_ptr<ElfFile> elf_file_;

  DISALLOW_COPY_AND_ASSIGN(ElfOatFile);
};

bool ElfOatFile::Load(const std::string& elf_filename,
                      bool executable,
                      bool low_4gb,
                      /*inout*/ MemMap* reservation,
                      /*out*/ std::string* error_msg) {
  ScopedTrace trace(__PRETTY_FUNCTION__);

  // Mirrors the alignment in the Bionic's dlopen. Actually, ART's MemMap only requires 4096 byte
  // alignment, but we want to be more strict here, to reflect what the Bionic's dlopen would be
  // able to load.
  auto [file, start, length] = OS::OpenFileDirectlyOrFromZip(
      elf_filename, kZipSeparator, /*alignment=*/MemMap::GetPageSize(), error_msg);
  if (file == nullptr) {
    return false;
  }

  return ElfOatFile::ElfFileOpen(
      file.get(), start, length, elf_filename, executable, low_4gb, reservation, error_msg);
}

bool ElfOatFile::Load(int oat_fd,
                      bool executable,
                      bool low_4gb,
                      /*inout*/ MemMap* reservation,
                      /*out*/ std::string* error_msg) {
  ScopedTrace trace(__PRETTY_FUNCTION__);
  if (oat_fd != -1) {
    int duped_fd = DupCloexec(oat_fd);
    std::unique_ptr<File> file = std::make_unique<File>(duped_fd, false);
    if (file == nullptr) {
      *error_msg = StringPrintf("Failed to open oat file for reading: %s", strerror(errno));
      return false;
    }
    int64_t file_length = file->GetLength();
    if (file_length < 0) {
      *error_msg = StringPrintf("Failed to get file length of oat file: %s", strerror(errno));
      return false;
    }
    return ElfOatFile::ElfFileOpen(file.get(),
                                   /*start=*/0,
                                   file_length,
                                   file->GetPath(),
                                   executable,
                                   low_4gb,
                                   reservation,
                                   error_msg);
  }
  return false;
}

bool ElfOatFile::ElfFileOpen(File* file,
                             off_t start,
                             size_t file_length,
                             const std::string& file_location,
                             bool executable,
                             bool low_4gb,
                             /*inout*/ MemMap* reservation,
                             /*out*/ std::string* error_msg) {
  ScopedTrace trace(__PRETTY_FUNCTION__);
  elf_file_.reset(ElfFile::Open(file, start, file_length, file_location, low_4gb, error_msg));
  if (elf_file_ == nullptr) {
    DCHECK(!error_msg->empty());
    return false;
  }
  bool loaded = elf_file_->Load(executable, low_4gb, reservation, error_msg);
  DCHECK(loaded || !error_msg->empty());
  return loaded;
}

class OatFileBackedByVdex final : public OatFileBase {
 public:
  explicit OatFileBackedByVdex(const std::string& filename)
      : OatFileBase(filename, /*executable=*/false),
        oat_header_(nullptr) {}

  ~OatFileBackedByVdex() {
    OatHeader::Delete(oat_header_);
  }

  static OatFileBackedByVdex* Open(const std::vector<const DexFile*>& dex_files,
                                   std::unique_ptr<VdexFile>&& vdex_file,
                                   const std::string& location,
                                   ClassLoaderContext* context) {
    std::unique_ptr<OatFileBackedByVdex> oat_file(new OatFileBackedByVdex(location));
    // SetVdex will take ownership of the VdexFile.
    oat_file->SetVdex(vdex_file.release());
    oat_file->SetupHeader(dex_files.size(), context);
    // Initialize OatDexFiles.
    std::string error_msg;
    if (!oat_file->Setup(dex_files, &error_msg)) {
      LOG(WARNING) << "Could not create in-memory vdex file: " << error_msg;
      return nullptr;
    }
    return oat_file.release();
  }

  static OatFileBackedByVdex* Open(int zip_fd,
                                   std::unique_ptr<VdexFile>&& unique_vdex_file,
                                   const std::string& dex_location,
                                   ClassLoaderContext* context,
                                   std::string* error_msg) {
    VdexFile* vdex_file = unique_vdex_file.get();
    std::unique_ptr<OatFileBackedByVdex> oat_file(new OatFileBackedByVdex(vdex_file->GetName()));
    // SetVdex will take ownership of the VdexFile.
    oat_file->SetVdex(unique_vdex_file.release());
    if (vdex_file->HasDexSection()) {
      uint32_t i = 0;
      const uint8_t* type_lookup_table_start = nullptr;
      auto dex_file_container =
          std::make_shared<MemoryDexFileContainer>(vdex_file->Begin(), vdex_file->End());
      for (const uint8_t* dex_file_start = vdex_file->GetNextDexFileData(nullptr, i);
           dex_file_start != nullptr;
           dex_file_start = vdex_file->GetNextDexFileData(dex_file_start, ++i)) {
        if (UNLIKELY(!vdex_file->Contains(dex_file_start, sizeof(DexFile::Header)))) {
          *error_msg =
              StringPrintf("In vdex file '%s' found invalid dex header %p of size %zu "
                               "not in [%p, %p]",
                           dex_location.c_str(),
                           dex_file_start,
                           sizeof(DexFile::Header),
                           vdex_file->Begin(),
                           vdex_file->End());
          return nullptr;
        }
        const DexFile::Header* header = reinterpret_cast<const DexFile::Header*>(dex_file_start);
        if (UNLIKELY(!vdex_file->Contains(dex_file_start, header->file_size_))) {
          *error_msg =
              StringPrintf("In vdex file '%s' found invalid dex file pointer %p of size %d "
                               "not in [%p, %p]",
                           dex_location.c_str(),
                           dex_file_start,
                           header->file_size_,
                           vdex_file->Begin(),
                           vdex_file->End());
          return nullptr;
        }
        if (UNLIKELY(!DexFileLoader::IsVersionAndMagicValid(dex_file_start))) {
          *error_msg =
              StringPrintf("In vdex file '%s' found dex file with invalid dex file version",
                           dex_location.c_str());
          return nullptr;
        }
        // Create the OatDexFile and add it to the owning container.
        std::string location = DexFileLoader::GetMultiDexLocation(i, dex_location.c_str());
        std::string canonical_location = DexFileLoader::GetDexCanonicalLocation(location.c_str());
        type_lookup_table_start = vdex_file->GetNextTypeLookupTableData(type_lookup_table_start, i);
        const uint8_t* type_lookup_table_data = nullptr;
        if (!ComputeAndCheckTypeLookupTableData(*header,
                                                type_lookup_table_start,
                                                vdex_file,
                                                &type_lookup_table_data,
                                                error_msg)) {
          return nullptr;
        }

        OatDexFile* oat_dex_file = new OatDexFile(oat_file.get(),
                                                  dex_file_container,
                                                  dex_file_start,
                                                  header->magic_,
                                                  vdex_file->GetLocationChecksum(i),
                                                  header->signature_,
                                                  location,
                                                  canonical_location,
                                                  type_lookup_table_data);
        oat_file->oat_dex_files_storage_.push_back(oat_dex_file);

        std::string_view key(oat_dex_file->GetDexFileLocation());
        oat_file->oat_dex_files_.Put(key, oat_dex_file);
        if (canonical_location != location) {
          std::string_view canonical_key(oat_dex_file->GetCanonicalDexFileLocation());
          oat_file->oat_dex_files_.Put(canonical_key, oat_dex_file);
        }
      }
      oat_file->SetupHeader(oat_file->oat_dex_files_storage_.size(), context);
    } else {
      // No need for any verification when loading dex files as we already have
      // a vdex file.
      bool loaded = false;
      if (zip_fd != -1) {
        File file(zip_fd, /*check_usage=*/false);
        ArtDexFileLoader dex_file_loader(&file, dex_location);
        loaded = dex_file_loader.Open(/*verify=*/false,
                                      /*verify_checksum=*/false,
                                      error_msg,
                                      &oat_file->external_dex_files_);
      } else {
        ArtDexFileLoader dex_file_loader(dex_location);
        loaded = dex_file_loader.Open(/*verify=*/false,
                                      /*verify_checksum=*/false,
                                      error_msg,
                                      &oat_file->external_dex_files_);
      }
      if (!loaded) {
        return nullptr;
      }
      oat_file->SetupHeader(oat_file->external_dex_files_.size(), context);
      if (!oat_file->Setup(MakeNonOwningPointerVector(oat_file->external_dex_files_), error_msg)) {
        return nullptr;
      }
    }

    return oat_file.release();
  }

  void SetupHeader(size_t number_of_dex_files, ClassLoaderContext* context) {
    DCHECK(!IsExecutable());

    // Create a fake OatHeader with a key store to help debugging.
    std::unique_ptr<const InstructionSetFeatures> isa_features =
        InstructionSetFeatures::FromCppDefines();
    SafeMap<std::string, std::string> store;
    store.Put(OatHeader::kCompilerFilter, CompilerFilter::NameOfFilter(CompilerFilter::kVerify));
    store.Put(OatHeader::kCompilationReasonKey, kReasonVdex);
    store.Put(OatHeader::kConcurrentCopying,
              gUseReadBarrier ? OatHeader::kTrueValue : OatHeader::kFalseValue);
    if (context != nullptr) {
      store.Put(OatHeader::kClassPathKey, context->EncodeContextForOatFile(""));
    }

    oat_header_ = OatHeader::Create(kRuntimeQuickCodeISA,
                                    isa_features.get(),
                                    number_of_dex_files,
                                    &store);
    const uint8_t* begin = reinterpret_cast<const uint8_t*>(oat_header_);
    SetBegin(begin);
    SetEnd(begin + oat_header_->GetHeaderSize());
  }

 protected:
  void PreLoad() override {}

  bool Load([[maybe_unused]] const std::string& elf_filename,
            [[maybe_unused]] bool executable,
            [[maybe_unused]] bool low_4gb,
            [[maybe_unused]] MemMap* reservation,
            [[maybe_unused]] std::string* error_msg) override {
    LOG(FATAL) << "Unsupported";
    UNREACHABLE();
  }

  bool Load([[maybe_unused]] int oat_fd,
            [[maybe_unused]] bool executable,
            [[maybe_unused]] bool low_4gb,
            [[maybe_unused]] MemMap* reservation,
            [[maybe_unused]] std::string* error_msg) override {
    LOG(FATAL) << "Unsupported";
    UNREACHABLE();
  }

  void PreSetup([[maybe_unused]] const std::string& elf_filename) override {}

  const uint8_t* FindDynamicSymbolAddress([[maybe_unused]] const std::string& symbol_name,
                                          std::string* error_msg) const override {
    *error_msg = "Unsupported";
    return nullptr;
  }

  const uint8_t* ComputeElfBegin(std::string* error_msg) const override {
    *error_msg = StringPrintf("Cannot get ELF begin because '%s' is not backed by an ELF file",
                              GetLocation().c_str());
    return nullptr;
  }

 private:
  OatHeader* oat_header_;

  DISALLOW_COPY_AND_ASSIGN(OatFileBackedByVdex);
};

//////////////////////////
// General OatFile code //
//////////////////////////

static void CheckLocation(const std::string& location) {
  CHECK(!location.empty());
}

OatFile* OatFile::Open(int zip_fd,
                       const std::string& oat_filename,
                       const std::string& oat_location,
                       bool executable,
                       bool low_4gb,
                       ArrayRef<const std::string> dex_filenames,
                       ArrayRef<File> dex_files,
                       /*inout*/ MemMap* reservation,
                       /*out*/ std::string* error_msg) {
  ScopedTrace trace("Open oat file " + oat_location);
  CHECK(!oat_filename.empty()) << oat_location;
  CheckLocation(oat_location);

  std::string vdex_filename = GetVdexFilename(oat_filename);

  // Check that the vdex file even exists, fast-fail. We don't check the odex
  // file as we use the absence of an odex file for test the functionality of
  // vdex-only.
  if (!OS::FileExists(vdex_filename.c_str())) {
    *error_msg = StringPrintf("File %s does not exist.", vdex_filename.c_str());
    return nullptr;
  }

  // Try dlopen first, as it is required for native debuggability. This will fail fast if dlopen is
  // disabled.
  OatFile* with_dlopen = OatFileBase::OpenOatFile<DlOpenOatFile>(zip_fd,
                                                                 vdex_filename,
                                                                 oat_filename,
                                                                 oat_location,
                                                                 executable,
                                                                 low_4gb,
                                                                 dex_filenames,
                                                                 dex_files,
                                                                 reservation,
                                                                 error_msg);
  if (with_dlopen != nullptr) {
    return with_dlopen;
  }
  if (kPrintDlOpenErrorMessage) {
    LOG(ERROR) << "Failed to dlopen: " << oat_filename << " with error " << *error_msg;
  }
  // If we aren't trying to execute, we just use our own ElfFile loader for a couple reasons:
  //
  // On target, dlopen may fail when compiling due to selinux restrictions on installd.
  //
  // We use our own ELF loader for Quick to deal with legacy apps that
  // open a generated dex file by name, remove the file, then open
  // another generated dex file with the same name. http://b/10614658
  //
  // On host, dlopen is expected to fail when cross compiling, so fall back to ElfOatFile.
  //
  //
  // Another independent reason is the absolute placement of boot.oat. dlopen on the host usually
  // does honor the virtual address encoded in the ELF file only for ET_EXEC files, not ET_DYN.
  OatFile* with_internal = OatFileBase::OpenOatFile<ElfOatFile>(zip_fd,
                                                                vdex_filename,
                                                                oat_filename,
                                                                oat_location,
                                                                executable,
                                                                low_4gb,
                                                                dex_filenames,
                                                                dex_files,
                                                                reservation,
                                                                error_msg);
  return with_internal;
}

OatFile* OatFile::Open(int zip_fd,
                       int vdex_fd,
                       int oat_fd,
                       const std::string& oat_location,
                       bool executable,
                       bool low_4gb,
                       ArrayRef<const std::string> dex_filenames,
                       ArrayRef<File> dex_files,
                       /*inout*/ MemMap* reservation,
                       /*out*/ std::string* error_msg) {
  CHECK(!oat_location.empty()) << oat_location;

  std::string vdex_location = GetVdexFilename(oat_location);

  OatFile* with_internal = OatFileBase::OpenOatFile<ElfOatFile>(zip_fd,
                                                                vdex_fd,
                                                                oat_fd,
                                                                vdex_location,
                                                                oat_location,
                                                                executable,
                                                                low_4gb,
                                                                dex_filenames,
                                                                dex_files,
                                                                reservation,
                                                                error_msg);
  return with_internal;
}

OatFile* OatFile::OpenFromVdex(const std::vector<const DexFile*>& dex_files,
                               std::unique_ptr<VdexFile>&& vdex_file,
                               const std::string& location,
                               ClassLoaderContext* context) {
  CheckLocation(location);
  return OatFileBackedByVdex::Open(dex_files, std::move(vdex_file), location, context);
}

OatFile* OatFile::OpenFromVdex(int zip_fd,
                               std::unique_ptr<VdexFile>&& vdex_file,
                               const std::string& location,
                               ClassLoaderContext* context,
                               std::string* error_msg) {
  CheckLocation(location);
  return OatFileBackedByVdex::Open(zip_fd, std::move(vdex_file), location, context, error_msg);
}

OatFile* OatFile::OpenFromSdm(const std::string& sdm_filename,
                              const std::string& sdc_filename,
                              const std::string& dm_filename,
                              const std::string& dex_filename,
                              bool executable,
                              /*out*/ std::string* error_msg) {
  ScopedTrace trace("Open sdm file " + sdm_filename);
  CHECK(!sdm_filename.empty());
  CHECK(!sdc_filename.empty());
  CHECK(!dm_filename.empty());
  CHECK(!dex_filename.empty());

  // Check if the dm file exists, to fail fast. The dm file contains the vdex that is essential for
  // using the odex in the sdm file.
  if (!OS::FileExists(dm_filename.c_str())) {
    *error_msg =
        ART_FORMAT("Not loading sdm file because dm file '{}' does not exist", dm_filename);
    return nullptr;
  }

  // Try dlopen first, as it is required for native debuggability. This will fail fast if dlopen is
  // disabled.
  OatFile* with_dlopen = OatFileBase::OpenOatFileFromSdm<DlOpenOatFile>(
      sdm_filename, sdc_filename, dm_filename, dex_filename, executable, error_msg);
  if (with_dlopen != nullptr) {
    return with_dlopen;
  }

  return OatFileBase::OpenOatFileFromSdm<ElfOatFile>(
      sdm_filename, sdc_filename, dm_filename, dex_filename, executable, error_msg);
}

OatFile::OatFile(const std::string& location, bool is_executable)
    : location_(location),
      vdex_(nullptr),
      begin_(nullptr),
      end_(nullptr),
      data_img_rel_ro_begin_(nullptr),
      data_img_rel_ro_end_(nullptr),
      data_img_rel_ro_app_image_(nullptr),
      bss_begin_(nullptr),
      bss_end_(nullptr),
      bss_methods_(nullptr),
      bss_roots_(nullptr),
      is_executable_(is_executable),
      vdex_begin_(nullptr),
      vdex_end_(nullptr),
      app_image_begin_(nullptr),
      secondary_lookup_lock_("OatFile secondary lookup lock", kOatFileSecondaryLookupLock) {
  CHECK(!location_.empty());
}

OatFile::~OatFile() {
  STLDeleteElements(&oat_dex_files_storage_);
}

const OatHeader& OatFile::GetOatHeader() const {
  return *reinterpret_cast<const OatHeader*>(Begin());
}

const uint8_t* OatFile::Begin() const {
  CHECK(begin_ != nullptr);
  return begin_;
}

const uint8_t* OatFile::End() const {
  CHECK(end_ != nullptr);
  return end_;
}

const uint8_t* OatFile::DexBegin() const {
  return vdex_->Begin();
}

const uint8_t* OatFile::DexEnd() const {
  return vdex_->End();
}

ArrayRef<const uint32_t> OatFile::GetBootImageRelocations() const {
  if (data_img_rel_ro_begin_ != nullptr) {
    const uint32_t* boot_image_relocations =
        reinterpret_cast<const uint32_t*>(data_img_rel_ro_begin_);
    const uint32_t* boot_image_relocations_end =
        reinterpret_cast<const uint32_t*>(data_img_rel_ro_app_image_);
    return ArrayRef<const uint32_t>(
        boot_image_relocations, boot_image_relocations_end - boot_image_relocations);
  } else {
    return ArrayRef<const uint32_t>();
  }
}

ArrayRef<const uint32_t> OatFile::GetAppImageRelocations() const {
  if (data_img_rel_ro_begin_ != nullptr) {
    const uint32_t* app_image_relocations =
        reinterpret_cast<const uint32_t*>(data_img_rel_ro_app_image_);
    const uint32_t* app_image_relocations_end =
        reinterpret_cast<const uint32_t*>(data_img_rel_ro_end_);
    return ArrayRef<const uint32_t>(
        app_image_relocations, app_image_relocations_end - app_image_relocations);
  } else {
    return ArrayRef<const uint32_t>();
  }
}

ArrayRef<ArtMethod*> OatFile::GetBssMethods() const {
  if (bss_methods_ != nullptr) {
    ArtMethod** methods = reinterpret_cast<ArtMethod**>(bss_methods_);
    ArtMethod** methods_end =
        reinterpret_cast<ArtMethod**>(bss_roots_ != nullptr ? bss_roots_ : bss_end_);
    return ArrayRef<ArtMethod*>(methods, methods_end - methods);
  } else {
    return ArrayRef<ArtMethod*>();
  }
}

ArrayRef<GcRoot<mirror::Object>> OatFile::GetBssGcRoots() const {
  if (bss_roots_ != nullptr) {
    auto* roots = reinterpret_cast<GcRoot<mirror::Object>*>(bss_roots_);
    auto* roots_end = reinterpret_cast<GcRoot<mirror::Object>*>(bss_end_);
    return ArrayRef<GcRoot<mirror::Object>>(roots, roots_end - roots);
  } else {
    return ArrayRef<GcRoot<mirror::Object>>();
  }
}

const OatDexFile* OatFile::GetOatDexFile(const char* dex_location, std::string* error_msg) const {
  // NOTE: We assume here that the canonical location for a given dex_location never
  // changes. If it does (i.e. some symlink used by the filename changes) we may return
  // an incorrect OatDexFile. As long as we have a checksum to check, we shall return
  // an identical file or fail; otherwise we may see some unpredictable failures.

  // TODO: Additional analysis of usage patterns to see if this can be simplified
  // without any performance loss, for example by not doing the first lock-free lookup.

  const OatDexFile* oat_dex_file = nullptr;
  std::string_view key(dex_location);
  // Try to find the key cheaply in the oat_dex_files_ map which holds dex locations
  // directly mentioned in the oat file and doesn't require locking.
  auto primary_it = oat_dex_files_.find(key);
  if (primary_it != oat_dex_files_.end()) {
    oat_dex_file = primary_it->second;
    DCHECK(oat_dex_file != nullptr);
  } else {
    // This dex_location is not one of the dex locations directly mentioned in the
    // oat file. The correct lookup is via the canonical location but first see in
    // the secondary_oat_dex_files_ whether we've looked up this location before.
    MutexLock mu(Thread::Current(), secondary_lookup_lock_);
    auto secondary_lb = secondary_oat_dex_files_.lower_bound(key);
    if (secondary_lb != secondary_oat_dex_files_.end() && key == secondary_lb->first) {
      oat_dex_file = secondary_lb->second;  // May be null.
    } else {
      // We haven't seen this dex_location before, we must check the canonical location.
      std::string dex_canonical_location = DexFileLoader::GetDexCanonicalLocation(dex_location);
      if (dex_canonical_location != dex_location) {
        std::string_view canonical_key(dex_canonical_location);
        auto canonical_it = oat_dex_files_.find(canonical_key);
        if (canonical_it != oat_dex_files_.end()) {
          oat_dex_file = canonical_it->second;
        }  // else keep null.
      }  // else keep null.

      // Copy the key to the string_cache_ and store the result in secondary map.
      string_cache_.emplace_back(key.data(), key.length());
      std::string_view key_copy(string_cache_.back());
      secondary_oat_dex_files_.PutBefore(secondary_lb, key_copy, oat_dex_file);
    }
  }

  if (oat_dex_file == nullptr) {
    if (error_msg != nullptr) {
      std::string dex_canonical_location = DexFileLoader::GetDexCanonicalLocation(dex_location);
      *error_msg = "Failed to find OatDexFile for DexFile " + std::string(dex_location)
          + " (canonical path " + dex_canonical_location + ") in OatFile " + GetLocation();
    }
    return nullptr;
  }

  return oat_dex_file;
}

OatDexFile::OatDexFile(const OatFile* oat_file,
                       const std::string& dex_file_location,
                       const std::string& canonical_dex_file_location,
                       DexFile::Magic dex_file_magic,
                       uint32_t dex_file_location_checksum,
                       DexFile::Sha1 dex_file_sha1,
                       const std::shared_ptr<DexFileContainer>& dex_file_container,
                       const uint8_t* dex_file_pointer,
                       const uint8_t* lookup_table_data,
                       const OatFile::BssMappingInfo& bss_mapping_info,
                       const uint32_t* oat_class_offsets_pointer,
                       const DexLayoutSections* dex_layout_sections)
    : oat_file_(oat_file),
      dex_file_location_(dex_file_location),
      canonical_dex_file_location_(canonical_dex_file_location),
      dex_file_magic_(dex_file_magic),
      dex_file_location_checksum_(dex_file_location_checksum),
      dex_file_sha1_(dex_file_sha1),
      dex_file_container_(dex_file_container),
      dex_file_pointer_(dex_file_pointer),
      lookup_table_data_(lookup_table_data),
      bss_mapping_info_(bss_mapping_info),
      oat_class_offsets_pointer_(oat_class_offsets_pointer),
      lookup_table_(),
      dex_layout_sections_(dex_layout_sections) {
  InitializeTypeLookupTable();
  DCHECK(!IsBackedByVdexOnly());
}

void OatDexFile::InitializeTypeLookupTable() {
  // Initialize TypeLookupTable.
  if (lookup_table_data_ != nullptr) {
    // Peek the number of classes from the DexFile.
    auto* dex_header = reinterpret_cast<const DexFile::Header*>(dex_file_pointer_);
    const uint32_t num_class_defs = dex_header->class_defs_size_;
    if (lookup_table_data_ + TypeLookupTable::RawDataLength(num_class_defs) >
            GetOatFile()->DexEnd()) {
      LOG(WARNING) << "found truncated lookup table in " << dex_file_location_;
    } else {
      const uint8_t* dex_data = dex_file_pointer_;
      // TODO: Clean this up to create the type lookup table after the dex file has been created?
      if (StandardDexFile::IsMagicValid(dex_header->magic_)) {
        dex_data -= dex_header->HeaderOffset();
      }
      if (CompactDexFile::IsMagicValid(dex_header->magic_)) {
        dex_data += dex_header->data_off_;
      }
      lookup_table_ = TypeLookupTable::Open(dex_data, lookup_table_data_, num_class_defs);
    }
  }
}

OatDexFile::OatDexFile(const OatFile* oat_file,
                       const std::shared_ptr<DexFileContainer>& dex_file_container,
                       const uint8_t* dex_file_pointer,
                       DexFile::Magic dex_file_magic,
                       uint32_t dex_file_location_checksum,
                       DexFile::Sha1 dex_file_sha1,
                       const std::string& dex_file_location,
                       const std::string& canonical_dex_file_location,
                       const uint8_t* lookup_table_data)
    : oat_file_(oat_file),
      dex_file_location_(dex_file_location),
      canonical_dex_file_location_(canonical_dex_file_location),
      dex_file_magic_(dex_file_magic),
      dex_file_location_checksum_(dex_file_location_checksum),
      dex_file_sha1_(dex_file_sha1),
      dex_file_container_(dex_file_container),
      dex_file_pointer_(dex_file_pointer),
      lookup_table_data_(lookup_table_data) {
  InitializeTypeLookupTable();
  DCHECK(IsBackedByVdexOnly());
}

OatDexFile::OatDexFile(TypeLookupTable&& lookup_table) : lookup_table_(std::move(lookup_table)) {
  // Stripped-down OatDexFile only allowed in the compiler, the zygote, or the system server.
  CHECK(Runtime::Current() == nullptr ||
        Runtime::Current()->IsAotCompiler() ||
        Runtime::Current()->IsZygote() ||
        Runtime::Current()->IsSystemServer());
}

OatDexFile::~OatDexFile() {}

size_t OatDexFile::FileSize() const {
  DCHECK(dex_file_pointer_ != nullptr);
  return reinterpret_cast<const DexFile::Header*>(dex_file_pointer_)->file_size_;
}

std::unique_ptr<const DexFile> OatDexFile::OpenDexFile(std::string* error_msg) const {
  ScopedTrace trace(__PRETTY_FUNCTION__);
  static constexpr bool kVerify = false;
  static constexpr bool kVerifyChecksum = false;
  ArtDexFileLoader dex_file_loader(dex_file_container_, dex_file_location_);
  return dex_file_loader.OpenOne(dex_file_pointer_ - dex_file_container_->Begin(),
                                 dex_file_location_checksum_,
                                 this,
                                 kVerify,
                                 kVerifyChecksum,
                                 error_msg);
}

uint32_t OatDexFile::GetOatClassOffset(uint16_t class_def_index) const {
  DCHECK(oat_class_offsets_pointer_ != nullptr);
  return oat_class_offsets_pointer_[class_def_index];
}

bool OatDexFile::IsBackedByVdexOnly() const {
  return oat_class_offsets_pointer_ == nullptr;
}

OatFile::OatClass OatDexFile::GetOatClass(uint16_t class_def_index) const {
  if (IsBackedByVdexOnly()) {
    // If there is only a vdex file, return that the class is not ready. The
    // caller will have to call `VdexFile::ComputeClassStatus` to compute the
    // actual class status, because we need to do the assignability type checks.
    return OatFile::OatClass(oat_file_,
                             ClassStatus::kNotReady,
                             /* type= */ OatClassType::kNoneCompiled,
                             /* num_methods= */ 0u,
                             /* bitmap_pointer= */ nullptr,
                             /* methods_pointer= */ nullptr);
  }

  uint32_t oat_class_offset = GetOatClassOffset(class_def_index);
  CHECK_GE(oat_class_offset, sizeof(OatHeader)) << oat_file_->GetLocation();
  CHECK_LT(oat_class_offset, oat_file_->Size()) << oat_file_->GetLocation();
  CHECK_LE(/* status */ sizeof(uint16_t) + /* type */ sizeof(uint16_t),
           oat_file_->Size() - oat_class_offset) << oat_file_->GetLocation();
  const uint8_t* current_pointer = oat_file_->Begin() + oat_class_offset;

  uint16_t status_value = *reinterpret_cast<const uint16_t*>(current_pointer);
  current_pointer += sizeof(uint16_t);
  uint16_t type_value = *reinterpret_cast<const uint16_t*>(current_pointer);
  current_pointer += sizeof(uint16_t);
  CHECK_LE(status_value, enum_cast<uint8_t>(ClassStatus::kLast))
      << static_cast<uint32_t>(status_value) << " at " << oat_file_->GetLocation();
  CHECK_LE(type_value, enum_cast<uint8_t>(OatClassType::kLast)) << oat_file_->GetLocation();
  ClassStatus status = enum_cast<ClassStatus>(status_value);
  OatClassType type = enum_cast<OatClassType>(type_value);

  uint32_t num_methods = 0;
  const uint32_t* bitmap_pointer = nullptr;
  const OatMethodOffsets* methods_pointer = nullptr;
  if (type != OatClassType::kNoneCompiled) {
    CHECK_LE(sizeof(uint32_t), static_cast<size_t>(oat_file_->End() - current_pointer))
        << oat_file_->GetLocation();
    num_methods = *reinterpret_cast<const uint32_t*>(current_pointer);
    current_pointer += sizeof(uint32_t);
    CHECK_NE(num_methods, 0u) << oat_file_->GetLocation();
    uint32_t num_method_offsets;
    if (type == OatClassType::kSomeCompiled) {
      uint32_t bitmap_size = BitVector::BitsToWords(num_methods) * BitVector::kWordBytes;
      CHECK_LE(bitmap_size, static_cast<size_t>(oat_file_->End() - current_pointer))
          << oat_file_->GetLocation();
      bitmap_pointer = reinterpret_cast<const uint32_t*>(current_pointer);
      current_pointer += bitmap_size;
      // Note: The bits in range [num_methods, bitmap_size * kBitsPerByte)
      // should be zero but we're not verifying that.
      num_method_offsets = BitVector::NumSetBits(bitmap_pointer, num_methods);
    } else {
      num_method_offsets = num_methods;
    }
    CHECK_LE(num_method_offsets,
             static_cast<size_t>(oat_file_->End() - current_pointer) / sizeof(OatMethodOffsets))
        << oat_file_->GetLocation();
    methods_pointer = reinterpret_cast<const OatMethodOffsets*>(current_pointer);
  }

  return OatFile::OatClass(oat_file_, status, type, num_methods, bitmap_pointer, methods_pointer);
}

const dex::ClassDef* OatDexFile::FindClassDef(const DexFile& dex_file,
                                              std::string_view descriptor,
                                              size_t hash) {
  const OatDexFile* oat_dex_file = dex_file.GetOatDexFile();
  DCHECK_EQ(ComputeModifiedUtf8Hash(descriptor), hash);
  bool used_lookup_table = false;
  const dex::ClassDef* lookup_table_classdef = nullptr;
  if (LIKELY((oat_dex_file != nullptr) && oat_dex_file->GetTypeLookupTable().Valid())) {
    used_lookup_table = true;
    const uint32_t class_def_idx = oat_dex_file->GetTypeLookupTable().Lookup(descriptor, hash);
    if (class_def_idx != dex::kDexNoIndex) {
      CHECK_LT(class_def_idx, dex_file.NumClassDefs()) << oat_dex_file->GetOatFile()->GetLocation();
      lookup_table_classdef = &dex_file.GetClassDef(class_def_idx);
    }
    if (!kIsDebugBuild) {
      return lookup_table_classdef;
    }
  }
  // Fast path for rare no class defs case.
  const uint32_t num_class_defs = dex_file.NumClassDefs();
  if (num_class_defs == 0) {
    DCHECK(!used_lookup_table);
    return nullptr;
  }
  const dex::TypeId* type_id = dex_file.FindTypeId(descriptor);
  if (type_id != nullptr) {
    dex::TypeIndex type_idx = dex_file.GetIndexForTypeId(*type_id);
    const dex::ClassDef* found_class_def = dex_file.FindClassDef(type_idx);
    if (kIsDebugBuild && used_lookup_table) {
      DCHECK_EQ(found_class_def, lookup_table_classdef);
    }
    return found_class_def;
  }
  return nullptr;
}

OatFile::OatClass::OatClass(const OatFile* oat_file,
                            ClassStatus status,
                            OatClassType type,
                            uint32_t num_methods,
                            const uint32_t* bitmap_pointer,
                            const OatMethodOffsets* methods_pointer)
    : oat_file_(oat_file),
      status_(status),
      type_(type),
      num_methods_(num_methods),
      bitmap_(bitmap_pointer),
      methods_pointer_(methods_pointer) {
  DCHECK_EQ(num_methods != 0u, type != OatClassType::kNoneCompiled);
  DCHECK_EQ(bitmap_pointer != nullptr, type == OatClassType::kSomeCompiled);
  DCHECK_EQ(methods_pointer != nullptr, type != OatClassType::kNoneCompiled);
}

uint32_t OatFile::OatClass::GetOatMethodOffsetsOffset(uint32_t method_index) const {
  const OatMethodOffsets* oat_method_offsets = GetOatMethodOffsets(method_index);
  if (oat_method_offsets == nullptr) {
    return 0u;
  }
  return reinterpret_cast<const uint8_t*>(oat_method_offsets) - oat_file_->Begin();
}

const OatMethodOffsets* OatFile::OatClass::GetOatMethodOffsets(uint32_t method_index) const {
  // NOTE: We don't keep the number of methods for `kNoneCompiled` and cannot do
  // a bounds check for `method_index` in that case.
  if (methods_pointer_ == nullptr) {
    CHECK_EQ(OatClassType::kNoneCompiled, type_);
    return nullptr;
  }
  CHECK_LT(method_index, num_methods_) << oat_file_->GetLocation();
  size_t methods_pointer_index;
  if (bitmap_ == nullptr) {
    CHECK_EQ(OatClassType::kAllCompiled, type_);
    methods_pointer_index = method_index;
  } else {
    CHECK_EQ(OatClassType::kSomeCompiled, type_);
    if (!BitVector::IsBitSet(bitmap_, method_index)) {
      return nullptr;
    }
    size_t num_set_bits = BitVector::NumSetBits(bitmap_, method_index);
    methods_pointer_index = num_set_bits;
  }
  if (kIsDebugBuild) {
    size_t size_until_end = dchecked_integral_cast<size_t>(
        oat_file_->End() - reinterpret_cast<const uint8_t*>(methods_pointer_));
    CHECK_LE(methods_pointer_index, size_until_end / sizeof(OatMethodOffsets))
        << oat_file_->GetLocation();
  }
  const OatMethodOffsets& oat_method_offsets = methods_pointer_[methods_pointer_index];
  return &oat_method_offsets;
}

const OatFile::OatMethod OatFile::OatClass::GetOatMethod(uint32_t method_index) const {
  const OatMethodOffsets* oat_method_offsets = GetOatMethodOffsets(method_index);
  if (oat_method_offsets == nullptr) {
    return OatMethod(nullptr, 0);
  }
  if (oat_file_->IsExecutable() ||
      Runtime::Current() == nullptr ||        // This case applies for oatdump.
      Runtime::Current()->IsAotCompiler()) {
    return OatMethod(oat_file_->Begin(), oat_method_offsets->code_offset_);
  }
  // We aren't allowed to use the compiled code. We just force it down the interpreted / jit
  // version.
  return OatMethod(oat_file_->Begin(), 0);
}

bool OatFile::IsDebuggable() const {
  return GetOatHeader().IsDebuggable();
}

CompilerFilter::Filter OatFile::GetCompilerFilter() const {
  return GetOatHeader().GetCompilerFilter();
}

std::string OatFile::GetClassLoaderContext() const {
  const char* value = GetOatHeader().GetStoreValueByKey(OatHeader::kClassPathKey);
  return (value == nullptr) ? "" : value;
}

const char* OatFile::GetCompilationReason() const {
  return GetOatHeader().GetStoreValueByKey(OatHeader::kCompilationReasonKey);
}

OatFile::OatClass OatFile::FindOatClass(const DexFile& dex_file,
                                        uint16_t class_def_idx,
                                        bool* found) {
  CHECK_LT(class_def_idx, dex_file.NumClassDefs());
  const OatDexFile* oat_dex_file = dex_file.GetOatDexFile();
  if (oat_dex_file == nullptr || oat_dex_file->GetOatFile() == nullptr) {
    *found = false;
    return OatFile::OatClass::Invalid();
  }
  *found = true;
  return oat_dex_file->GetOatClass(class_def_idx);
}

bool OatFile::RequiresImage() const { return GetOatHeader().RequiresImage(); }

static void DCheckIndexToBssMapping(const OatFile* oat_file,
                                    uint32_t number_of_indexes,
                                    size_t slot_size,
                                    const IndexBssMapping* index_bss_mapping) {
  if (kIsDebugBuild && index_bss_mapping != nullptr) {
    size_t index_bits = IndexBssMappingEntry::IndexBits(number_of_indexes);
    const IndexBssMappingEntry* prev_entry = nullptr;
    for (const IndexBssMappingEntry& entry : *index_bss_mapping) {
      CHECK_ALIGNED_PARAM(entry.bss_offset, slot_size);
      CHECK_LT(entry.bss_offset, oat_file->BssSize());
      uint32_t mask = entry.GetMask(index_bits);
      CHECK_LE(POPCOUNT(mask) * slot_size, entry.bss_offset);
      size_t index_mask_span = (mask != 0u) ? 32u - index_bits - CTZ(mask) : 0u;
      CHECK_LE(index_mask_span, entry.GetIndex(index_bits));
      if (prev_entry != nullptr) {
        CHECK_LT(prev_entry->GetIndex(index_bits), entry.GetIndex(index_bits) - index_mask_span);
      }
      prev_entry = &entry;
    }
    CHECK(prev_entry != nullptr);
    CHECK_LT(prev_entry->GetIndex(index_bits), number_of_indexes);
  }
}

void OatFile::InitializeRelocations() const {
  DCHECK(IsExecutable());

  // Initialize the .data.img.rel.ro section.
  if (DataImgRelRoEnd() != DataImgRelRoBegin()) {
    uint8_t* reloc_begin = const_cast<uint8_t*>(DataImgRelRoBegin());
    CheckedCall(mprotect,
                "un-protect boot image relocations",
                reloc_begin,
                DataImgRelRoSize(),
                PROT_READ | PROT_WRITE);
    uint32_t boot_image_begin = Runtime::Current()->GetHeap()->GetBootImagesStartAddress();
    for (const uint32_t& relocation : GetBootImageRelocations()) {
      const_cast<uint32_t&>(relocation) += boot_image_begin;
    }
    if (!GetAppImageRelocations().empty()) {
      CHECK(app_image_begin_ != nullptr);
      uint32_t app_image_begin = reinterpret_cast32<uint32_t>(app_image_begin_);
      for (const uint32_t& relocation : GetAppImageRelocations()) {
        const_cast<uint32_t&>(relocation) += app_image_begin;
      }
    }
    CheckedCall(mprotect,
                "protect boot image relocations",
                reloc_begin,
                DataImgRelRoSize(),
                PROT_READ);
  }

  // Before initializing .bss, check the .bss mappings in debug mode.
  if (kIsDebugBuild) {
    PointerSize pointer_size = GetInstructionSetPointerSize(GetOatHeader().GetInstructionSet());
    for (const OatDexFile* odf : GetOatDexFiles()) {
      const DexFile::Header* header =
          reinterpret_cast<const DexFile::Header*>(odf->GetDexFilePointer());
      DCheckIndexToBssMapping(this,
                              header->method_ids_size_,
                              static_cast<size_t>(pointer_size),
                              odf->GetMethodBssMapping());
      DCheckIndexToBssMapping(this,
                              header->type_ids_size_,
                              sizeof(GcRoot<mirror::Class>),
                              odf->GetTypeBssMapping());
      DCheckIndexToBssMapping(this,
                              header->string_ids_size_,
                              sizeof(GcRoot<mirror::String>),
                              odf->GetStringBssMapping());
    }
  }

  // Initialize the .bss section.
  // TODO: Pre-initialize from boot/app image?
  ArtMethod* resolution_method = Runtime::Current()->GetResolutionMethod();
  for (ArtMethod*& entry : GetBssMethods()) {
    entry = resolution_method;
  }
}

void OatDexFile::AssertAotCompiler() {
  CHECK(Runtime::Current()->IsAotCompiler());
}

uint32_t OatDexFile::GetDexVersion() const {
  return atoi(reinterpret_cast<const char*>(&dex_file_magic_[4]));
}

bool OatFile::IsBackedByVdexOnly() const {
  return oat_dex_files_storage_.size() >= 1 && oat_dex_files_storage_[0]->IsBackedByVdexOnly();
}

std::optional<std::string_view> OatFile::GetApexVersions() const {
  if (override_apex_versions_.has_value()) {
    return override_apex_versions_;
  }
  const char* oat_apex_versions =
      GetOatHeader().GetStoreValueByKeyUnsafe(OatHeader::kApexVersionsKey);
  return oat_apex_versions != nullptr ? std::make_optional(oat_apex_versions) : std::nullopt;
}

}  // namespace art
