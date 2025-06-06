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

#ifndef ART_RUNTIME_OAT_OAT_FILE_H_
#define ART_RUNTIME_OAT_OAT_FILE_H_

#include <list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/array_ref.h"
#include "base/compiler_filter.h"
#include "base/macros.h"
#include "base/mutex.h"
#include "base/os.h"
#include "base/safe_map.h"
#include "base/tracking_safe_map.h"
#include "class_status.h"
#include "dex/dex_file.h"
#include "dex/dex_file_layout.h"
#include "dex/type_lookup_table.h"
#include "dex/utf.h"
#include "index_bss_mapping.h"
#include "mirror/object.h"

namespace art HIDDEN {

class BitVector;
class ClassLinker;
class ClassLoaderContext;
class ElfFile;
class DexLayoutSections;
template <class MirrorType> class GcRoot;
class MemMap;
class OatDexFile;
class OatHeader;
class OatMethodOffsets;
class OatQuickMethodHeader;
class VdexFile;

namespace dex {
struct ClassDef;
}  // namespace dex

namespace gc {
namespace collector {
class FakeOatFile;
}  // namespace collector
}  // namespace gc

// A special compilation reason to indicate that only the VDEX file is usable. Keep in sync with
// `ArtConstants::REASON_VDEX` in artd/binder/com/android/server/art/ArtConstants.aidl.
static constexpr const char* kReasonVdex = "vdex";

// OatMethodOffsets are currently 5x32-bits=160-bits long, so if we can
// save even one OatMethodOffsets struct, the more complicated encoding
// using a bitmap pays for itself since few classes will have 160
// methods.
enum class OatClassType : uint8_t {
  kAllCompiled = 0,   // OatClass is followed by an OatMethodOffsets for each method.
  kSomeCompiled = 1,  // A bitmap of OatMethodOffsets that are present follows the OatClass.
  kNoneCompiled = 2,  // All methods are interpreted so no OatMethodOffsets are necessary.
  kLast = kNoneCompiled
};

EXPORT std::ostream& operator<<(std::ostream& os, OatClassType rhs);

class PACKED(4) OatMethodOffsets {
 public:
  explicit OatMethodOffsets(uint32_t code_offset = 0) : code_offset_(code_offset) {}

  ~OatMethodOffsets() {}

  OatMethodOffsets(const OatMethodOffsets&) = default;
  OatMethodOffsets& operator=(const OatMethodOffsets&) = default;

  uint32_t code_offset_;
};

// Runtime representation of the OAT file format which holds compiler output.
// The class opens an OAT file from storage and maps it to memory, typically with
// dlopen and provides access to its internal data structures (see OatWriter for
// for more details about the OAT format).
// In the process of loading OAT, the class also loads the associated VDEX file
// with the input DEX files (see VdexFile for details about the VDEX format).
// The raw DEX data are accessible transparently through the OatDexFile objects.

class OatFile {
 public:
  // The zip separator. This has to be the one that Bionic's dlopen recognizes because oat files are
  // opened through dlopen in `DlOpenOatFile`. This is different from the ART's zip separator for
  // MultiDex.
  static constexpr const char* kZipSeparator = "!/";

  // Open an oat file. Returns null on failure.
  // The `dex_filenames` argument, if provided, overrides the dex locations
  // from oat file when opening the dex files if they are not embedded in the
  // vdex file. These may differ for cross-compilation (the dex file name is
  // the host path and dex location is the future path on target) and testing.
  EXPORT static OatFile* Open(int zip_fd,
                              const std::string& filename,
                              const std::string& location,
                              bool executable,
                              bool low_4gb,
                              ArrayRef<const std::string> dex_filenames,
                              ArrayRef<File> dex_files,
                              /*inout*/ MemMap* reservation,  // Where to load if not null.
                              /*out*/ std::string* error_msg);
  // Helper overload that takes a single dex filename and no reservation.
  EXPORT static OatFile* Open(int zip_fd,
                              const std::string& filename,
                              const std::string& location,
                              bool executable,
                              bool low_4gb,
                              const std::string& dex_filename,
                              /*out*/ std::string* error_msg) {
    return Open(zip_fd,
                filename,
                location,
                executable,
                low_4gb,
                ArrayRef<const std::string>(&dex_filename, /*size=*/1u),
                /*dex_files=*/{},  // not currently supported
                /*reservation=*/nullptr,
                error_msg);
  }
  // Helper overload that takes no dex filename and no reservation.
  static OatFile* Open(int zip_fd,
                       const std::string& filename,
                       const std::string& location,
                       bool executable,
                       bool low_4gb,
                       /*out*/std::string* error_msg) {
    return Open(zip_fd,
                filename,
                location,
                executable,
                low_4gb,
                /*dex_filenames=*/{},
                /*dex_files=*/{},  // not currently supported
                /*reservation=*/nullptr,
                error_msg);
  }

  // Similar to OatFile::Open(const std::string...), but accepts input vdex and
  // odex files as file descriptors. We also take zip_fd in case the vdex does not
  // contain the dex code, and we need to read it from the zip file.
  static OatFile* Open(int zip_fd,
                       int vdex_fd,
                       int oat_fd,
                       const std::string& oat_location,
                       bool executable,
                       bool low_4gb,
                       ArrayRef<const std::string> dex_filenames,
                       ArrayRef<File> dex_files,
                       /*inout*/ MemMap* reservation,  // Where to load if not null.
                       /*out*/ std::string* error_msg);

  // Initialize OatFile instance from an already loaded VdexFile. This assumes
  // the vdex does not have a dex section and accepts a vector of DexFiles separately.
  static OatFile* OpenFromVdex(const std::vector<const DexFile*>& dex_files,
                               std::unique_ptr<VdexFile>&& vdex_file,
                               const std::string& location,
                               ClassLoaderContext* context);

  // Initialize OatFile instance from an already loaded VdexFile. The dex files
  // will be opened through `zip_fd` or `dex_location` if `zip_fd` is -1.
  static OatFile* OpenFromVdex(int zip_fd,
                               std::unique_ptr<VdexFile>&& vdex_file,
                               const std::string& location,
                               ClassLoaderContext* context,
                               std::string* error_msg);

  static OatFile* OpenFromSdm(const std::string& sdm_filename,
                              const std::string& sdc_filename,
                              const std::string& dm_filename,
                              const std::string& dex_filename,
                              bool executable,
                              /*out*/ std::string* error_msg);

  // Set the start of the app image.
  // Needed for initializing app image relocations in the .data.img.rel.ro section.
  void SetAppImageBegin(uint8_t* app_image_begin) const {
    app_image_begin_ = app_image_begin;
  }

  // Return whether the `OatFile` uses a vdex-only file.
  bool IsBackedByVdexOnly() const;

  virtual ~OatFile();

  bool IsExecutable() const {
    return is_executable_;
  }

  // Indicates whether the oat file was compiled with full debugging capability.
  bool IsDebuggable() const;

  EXPORT CompilerFilter::Filter GetCompilerFilter() const;

  std::string GetClassLoaderContext() const;

  const char* GetCompilationReason() const;

  const std::string& GetLocation() const {
    return location_;
  }

  EXPORT const OatHeader& GetOatHeader() const;

  class OatMethod final {
   public:
    uint32_t GetCodeOffset() const { return code_offset_; }

    const void* GetQuickCode() const;

    // Returns size of quick code.
    uint32_t GetQuickCodeSize() const;

    // Returns OatQuickMethodHeader for debugging. Most callers should
    // use more specific methods such as GetQuickCodeSize.
    const OatQuickMethodHeader* GetOatQuickMethodHeader() const;
    uint32_t GetOatQuickMethodHeaderOffset() const;

    size_t GetFrameSizeInBytes() const;
    uint32_t GetCoreSpillMask() const;
    uint32_t GetFpSpillMask() const;

    const uint8_t* GetVmapTable() const;
    uint32_t GetVmapTableOffset() const;

    // Create an OatMethod with offsets relative to the given base address
    OatMethod(const uint8_t* base, const uint32_t code_offset)
        : begin_(base), code_offset_(code_offset) {
    }
    OatMethod(const OatMethod&) = default;
    ~OatMethod() {}

    OatMethod& operator=(const OatMethod&) = default;

    // A representation of an invalid OatMethod, used when an OatMethod or OatClass can't be found.
    // See ClassLinker::FindOatMethodFor.
    static const OatMethod Invalid() {
      return OatMethod(nullptr, -1);
    }

   private:
    const uint8_t* begin_;
    uint32_t code_offset_;

    friend class OatClass;
  };

  class OatClass final {
   public:
    ClassStatus GetStatus() const {
      return status_;
    }

    OatClassType GetType() const {
      return type_;
    }

    // Get the OatMethod entry based on its index into the class
    // defintion. Direct methods come first, followed by virtual
    // methods. Note that runtime created methods such as miranda
    // methods are not included.
    EXPORT const OatMethod GetOatMethod(uint32_t method_index) const;

    // Return a pointer to the OatMethodOffsets for the requested
    // method_index, or null if none is present. Note that most
    // callers should use GetOatMethod.
    EXPORT const OatMethodOffsets* GetOatMethodOffsets(uint32_t method_index) const;

    // Return the offset from the start of the OatFile to the
    // OatMethodOffsets for the requested method_index, or 0 if none
    // is present. Note that most callers should use GetOatMethod.
    EXPORT uint32_t GetOatMethodOffsetsOffset(uint32_t method_index) const;

    // A representation of an invalid OatClass, used when an OatClass can't be found.
    // See FindOatClass().
    static OatClass Invalid() {
      return OatClass(/* oat_file= */ nullptr,
                      ClassStatus::kErrorUnresolved,
                      OatClassType::kNoneCompiled,
                      /* num_methods= */ 0,
                      /* bitmap_pointer= */ nullptr,
                      /* methods_pointer= */ nullptr);
    }

   private:
    OatClass(const OatFile* oat_file,
             ClassStatus status,
             OatClassType type,
             uint32_t num_methods,
             const uint32_t* bitmap_pointer,
             const OatMethodOffsets* methods_pointer);

    const OatFile* const oat_file_;
    const ClassStatus status_;
    const OatClassType type_;
    const uint32_t num_methods_;
    const uint32_t* const bitmap_;
    const OatMethodOffsets* const methods_pointer_;

    friend class ClassLinker;
    friend class OatDexFile;
  };

  // Get the OatDexFile for the given dex_location within this oat file.
  // If dex_location_checksum is non-null, the OatDexFile will only be
  // returned if it has a matching checksum.
  // If error_msg is non-null and no OatDexFile is returned, error_msg will
  // be updated with a description of why no OatDexFile was returned.
  const OatDexFile* GetOatDexFile(const char* dex_location,
                                  /*out*/ std::string* error_msg = nullptr) const
      REQUIRES(!secondary_lookup_lock_);

  const std::vector<const OatDexFile*>& GetOatDexFiles() const {
    return oat_dex_files_storage_;
  }

  size_t Size() const {
    return End() - Begin();
  }

  bool Contains(const void* p) const {
    return p >= Begin() && p < End();
  }

  size_t DataImgRelRoSize() const {
    return DataImgRelRoEnd() - DataImgRelRoBegin();
  }

  size_t DataImgRelRoAppImageOffset() const {
    return DataImgRelRoAppImage() - DataImgRelRoBegin();
  }

  size_t BssSize() const {
    return BssEnd() - BssBegin();
  }

  size_t VdexSize() const {
    return VdexEnd() - VdexBegin();
  }

  size_t BssMethodsOffset() const {
    // Note: This is used only for symbolizer and needs to return a valid .bss offset.
    return (bss_methods_ != nullptr) ? bss_methods_ - BssBegin() : BssRootsOffset();
  }

  size_t BssRootsOffset() const {
    // Note: This is used only for symbolizer and needs to return a valid .bss offset.
    return (bss_roots_ != nullptr) ? bss_roots_ - BssBegin() : BssSize();
  }

  size_t DexSize() const {
    return DexEnd() - DexBegin();
  }

  // Returns the base address of the ELF file, or nullptr if the oat file is not backed by an ELF
  // file or an error occurred.
  virtual const uint8_t* ComputeElfBegin(std::string* error_msg) const = 0;

  EXPORT const uint8_t* Begin() const;
  EXPORT const uint8_t* End() const;

  const uint8_t* DataImgRelRoBegin() const { return data_img_rel_ro_begin_; }
  const uint8_t* DataImgRelRoEnd() const { return data_img_rel_ro_end_; }
  const uint8_t* DataImgRelRoAppImage() const { return data_img_rel_ro_app_image_; }

  const uint8_t* BssBegin() const { return bss_begin_; }
  const uint8_t* BssEnd() const { return bss_end_; }

  const uint8_t* VdexBegin() const { return vdex_begin_; }
  const uint8_t* VdexEnd() const { return vdex_end_; }

  EXPORT const uint8_t* DexBegin() const;
  EXPORT const uint8_t* DexEnd() const;

  EXPORT ArrayRef<const uint32_t> GetBootImageRelocations() const;
  EXPORT ArrayRef<const uint32_t> GetAppImageRelocations() const;
  EXPORT ArrayRef<ArtMethod*> GetBssMethods() const;
  EXPORT ArrayRef<GcRoot<mirror::Object>> GetBssGcRoots() const;

  // Initialize relocation sections (.data.img.rel.ro and .bss).
  void InitializeRelocations() const;

  // Finds the associated oat class for a dex_file and descriptor. Returns an invalid OatClass on
  // error and sets found to false.
  EXPORT static OatClass FindOatClass(const DexFile& dex_file, uint16_t class_def_idx, bool* found);

  VdexFile* GetVdexFile() const {
    return vdex_.get();
  }

  // Whether the OatFile embeds the Dex code.
  bool ContainsDexCode() const {
    return external_dex_files_.empty();
  }

  // Returns whether an image (e.g. app image) is required to safely execute this OAT file.
  bool RequiresImage() const;

  struct BssMappingInfo {
    const IndexBssMapping* method_bss_mapping = nullptr;
    const IndexBssMapping* type_bss_mapping = nullptr;
    const IndexBssMapping* public_type_bss_mapping = nullptr;
    const IndexBssMapping* package_type_bss_mapping = nullptr;
    const IndexBssMapping* string_bss_mapping = nullptr;
    const IndexBssMapping* method_type_bss_mapping = nullptr;
  };

  ArrayRef<const BssMappingInfo> GetBcpBssInfo() const {
    return ArrayRef<const BssMappingInfo>(bcp_bss_info_);
  }

  // Returns the mapping info of `dex_file` if found in the BcpBssInfo, or nullptr otherwise.
  const BssMappingInfo* FindBcpMappingInfo(const DexFile* dex_file) const;

  std::optional<std::string_view> GetApexVersions() const;

 protected:
  OatFile(const std::string& filename, bool executable);

 private:
  // The oat file name.
  //
  // The image will embed this to link its associated oat file.
  const std::string location_;

  // Pointer to the Vdex file with the Dex files for this Oat file.
  std::unique_ptr<VdexFile> vdex_;

  // Pointer to OatHeader.
  const uint8_t* begin_;

  // Pointer to end of oat region for bounds checking.
  const uint8_t* end_;

  // Pointer to the .data.img.rel.ro section, if present, otherwise null.
  const uint8_t* data_img_rel_ro_begin_;

  // Pointer to the end of the .data.img.rel.ro section, if present, otherwise null.
  const uint8_t* data_img_rel_ro_end_;

  // Pointer to the beginning of the app image relocations in the .data.img.rel.ro section,
  // if present, otherwise null.
  const uint8_t* data_img_rel_ro_app_image_;

  // Pointer to the .bss section, if present, otherwise null.
  uint8_t* bss_begin_;

  // Pointer to the end of the .bss section, if present, otherwise null.
  uint8_t* bss_end_;

  // Pointer to the beginning of the ArtMethod*s in the .bss section, if present, otherwise null.
  uint8_t* bss_methods_;

  // Pointer to the beginning of the GC roots in the .bss section, if present, otherwise null.
  uint8_t* bss_roots_;

  // Was this oat_file loaded executable?
  const bool is_executable_;

  // Pointer to the .vdex section, if present, otherwise null.
  uint8_t* vdex_begin_;

  // Pointer to the end of the .vdex section, if present, otherwise null.
  uint8_t* vdex_end_;

  // Pointer to the beginning of the app image, if any.
  mutable uint8_t* app_image_begin_;

  // Owning storage for the OatDexFile objects.
  std::vector<const OatDexFile*> oat_dex_files_storage_;

  // Mapping info for DexFiles in the BCP.
  std::vector<BssMappingInfo> bcp_bss_info_;

  // NOTE: We use a std::string_view as the key type to avoid a memory allocation on every
  // lookup with a const char* key. The std::string_view doesn't own its backing storage,
  // therefore we're using the OatFile's stored dex location as the backing storage
  // for keys in oat_dex_files_ and the string_cache_ entries for the backing storage
  // of keys in secondary_oat_dex_files_ and oat_dex_files_by_canonical_location_.
  using Table =
      AllocationTrackingSafeMap<std::string_view, const OatDexFile*, kAllocatorTagOatFile>;

  // Map each location and canonical location (if different) retrieved from the
  // oat file to its OatDexFile. This map doesn't change after it's constructed in Setup()
  // and therefore doesn't need any locking and provides the cheapest dex file lookup
  // for GetOatDexFile() for a very frequent use case. Never contains a null value.
  Table oat_dex_files_;

  // Lock guarding all members needed for secondary lookup in GetOatDexFile().
  mutable Mutex secondary_lookup_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;

  // If the primary oat_dex_files_ lookup fails, use a secondary map. This map stores
  // the results of all previous secondary lookups, whether successful (non-null) or
  // failed (null). If it doesn't contain an entry we need to calculate the canonical
  // location and use oat_dex_files_by_canonical_location_.
  mutable Table secondary_oat_dex_files_ GUARDED_BY(secondary_lookup_lock_);

  // Cache of strings. Contains the backing storage for keys in the secondary_oat_dex_files_
  // and the lazily initialized oat_dex_files_by_canonical_location_.
  // NOTE: We're keeping references to contained strings in form of std::string_view and adding
  // new strings to the end. The adding of a new element must not touch any previously stored
  // elements. std::list<> and std::deque<> satisfy this requirement, std::vector<> doesn't.
  mutable std::list<std::string> string_cache_ GUARDED_BY(secondary_lookup_lock_);

  // Dex files opened directly from a file referenced from the oat file or specifed
  // by the `dex_filenames` parameter, in case the OatFile does not embed the dex code.
  std::vector<std::unique_ptr<const DexFile>> external_dex_files_;

  // If set, overrides the APEX versions in the header.
  std::optional<std::string> override_apex_versions_ = std::nullopt;

  friend class gc::collector::FakeOatFile;  // For modifying begin_ and end_.
  friend class OatClass;
  friend class art::OatDexFile;
  friend class OatDumper;  // For GetBase and GetLimit
  friend class OatFileBackedByVdex;
  friend class OatFileBase;
  DISALLOW_COPY_AND_ASSIGN(OatFile);
};

// OatDexFile should be an inner class of OatFile. Unfortunately, C++ doesn't
// support forward declarations of inner classes, and we want to
// forward-declare OatDexFile so that we can store an opaque pointer to an
// OatDexFile in DexFile.
class OatDexFile final {
 public:
  // Opens the DexFile referred to by this OatDexFile from within the containing OatFile.
  EXPORT std::unique_ptr<const DexFile> OpenDexFile(std::string* error_msg) const;

  // May return null if the OatDexFile only contains a type lookup table. This case only happens
  // for the compiler to speed up compilation, or in jitzygote.
  const OatFile* GetOatFile() const {
    return oat_file_;
  }

  // Returns the size of the DexFile refered to by this OatDexFile.
  EXPORT size_t FileSize() const;

  // Returns original path of DexFile that was the source of this OatDexFile.
  const std::string& GetDexFileLocation() const {
    return dex_file_location_;
  }

  // Returns original path of DexFile that was the source of this OatDexFile.
  const std::string& GetLocation() const { return dex_file_location_; }

  // Returns the canonical location of DexFile that was the source of this OatDexFile.
  const std::string& GetCanonicalDexFileLocation() const {
    return canonical_dex_file_location_;
  }

  DexFile::Magic GetMagic() const { return dex_file_magic_; }

  uint32_t GetDexVersion() const;

  // Returns checksum of original DexFile that was the source of this OatDexFile;
  uint32_t GetDexFileLocationChecksum() const {
    return dex_file_location_checksum_;
  }

  // Returns checksum of original DexFile that was the source of this OatDexFile;
  uint32_t GetLocationChecksum() const { return dex_file_location_checksum_; }

  DexFile::Sha1 GetSha1() const { return dex_file_sha1_; }

  // Returns the OatClass for the class specified by the given DexFile class_def_index.
  EXPORT OatFile::OatClass GetOatClass(uint16_t class_def_index) const;

  // Returns the offset to the OatClass information. Most callers should use GetOatClass.
  EXPORT uint32_t GetOatClassOffset(uint16_t class_def_index) const;

  const uint8_t* GetLookupTableData() const {
    return lookup_table_data_;
  }

  const IndexBssMapping* GetMethodBssMapping() const {
    return bss_mapping_info_.method_bss_mapping;
  }

  const IndexBssMapping* GetTypeBssMapping() const {
    return bss_mapping_info_.type_bss_mapping;
  }

  const IndexBssMapping* GetPublicTypeBssMapping() const {
    return bss_mapping_info_.public_type_bss_mapping;
  }

  const IndexBssMapping* GetPackageTypeBssMapping() const {
    return bss_mapping_info_.package_type_bss_mapping;
  }

  const IndexBssMapping* GetStringBssMapping() const {
    return bss_mapping_info_.string_bss_mapping;
  }

  const IndexBssMapping* GetMethodTypeBssMapping() const {
    return bss_mapping_info_.method_type_bss_mapping;
  }

  const uint8_t* GetDexFilePointer() const {
    return dex_file_pointer_;
  }

  // Looks up a class definition by its class descriptor. Hash must be
  // ComputeModifiedUtf8Hash(descriptor).
  EXPORT static const dex::ClassDef* FindClassDef(const DexFile& dex_file,
                                                  std::string_view descriptor,
                                                  size_t hash);

  const TypeLookupTable& GetTypeLookupTable() const {
    return lookup_table_;
  }

  EXPORT ~OatDexFile();

  // Create only with a type lookup table, used by the compiler to speed up compilation.
  EXPORT explicit OatDexFile(TypeLookupTable&& lookup_table);

  // Return the dex layout sections.
  const DexLayoutSections* GetDexLayoutSections() const {
    return dex_layout_sections_;
  }

 private:
  OatDexFile(const OatFile* oat_file,
             const std::string& dex_file_location,
             const std::string& canonical_dex_file_location,
             DexFile::Magic dex_file_magic,
             uint32_t dex_file_checksum,
             DexFile::Sha1 dex_file_sha1,
             const std::shared_ptr<DexFileContainer>& dex_file_container_,
             const uint8_t* dex_file_pointer,
             const uint8_t* lookup_table_data,
             const OatFile::BssMappingInfo& bss_mapping_info,
             const uint32_t* oat_class_offsets_pointer,
             const DexLayoutSections* dex_layout_sections);

  // Create an OatDexFile wrapping an existing DexFile. Will set the OatDexFile
  // pointer in the DexFile.
  OatDexFile(const OatFile* oat_file,
             const std::shared_ptr<DexFileContainer>& dex_file_container_,
             const uint8_t* dex_file_pointer,
             DexFile::Magic dex_file_magic,
             uint32_t dex_file_checksum,
             DexFile::Sha1 dex_file_sha1,
             const std::string& dex_file_location,
             const std::string& canonical_dex_file_location,
             const uint8_t* lookup_table_data);

  bool IsBackedByVdexOnly() const;
  void InitializeTypeLookupTable();

  static void AssertAotCompiler();

  const OatFile* const oat_file_ = nullptr;
  const std::string dex_file_location_;
  const std::string canonical_dex_file_location_;
  const DexFile::Magic dex_file_magic_ = {};
  const uint32_t dex_file_location_checksum_ = 0u;
  const DexFile::Sha1 dex_file_sha1_ = {};
  const std::shared_ptr<DexFileContainer> dex_file_container_;
  const uint8_t* const dex_file_pointer_ = nullptr;
  const uint8_t* const lookup_table_data_ = nullptr;
  const OatFile::BssMappingInfo bss_mapping_info_;
  const uint32_t* const oat_class_offsets_pointer_ = nullptr;
  TypeLookupTable lookup_table_;
  const DexLayoutSections* const dex_layout_sections_ = nullptr;

  friend class OatFile;
  friend class OatFileBase;
  friend class OatFileBackedByVdex;
  DISALLOW_COPY_AND_ASSIGN(OatDexFile);
};

}  // namespace art

#endif  // ART_RUNTIME_OAT_OAT_FILE_H_
