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

#ifndef ART_DEX2OAT_LINKER_OAT_WRITER_H_
#define ART_DEX2OAT_LINKER_OAT_WRITER_H_

#include <stdint.h>
#include <cstddef>
#include <list>
#include <memory>
#include <vector>

#include "base/array_ref.h"
#include "base/dchecked_vector.h"
#include "base/os.h"
#include "base/mem_map.h"
#include "base/safe_map.h"
#include "debug/debug_info.h"
#include "dex/method_reference.h"
#include "dex/string_reference.h"
#include "dex/proto_reference.h"
#include "dex/type_reference.h"
#include "linker/relative_patcher.h"  // For RelativePatcherTargetProvider.
#include "mirror/class.h"

namespace art {

class BitVector;
class CompiledMethod;
class CompilerDriver;
class CompilerOptions;
class OatHeader;
class OutputStream;
class ProfileCompilationInfo;
class TimingLogger;
class TypeLookupTable;
class VdexFile;
class VerificationResults;
class ZipEntry;

namespace debug {
struct MethodDebugInfo;
}  // namespace debug

namespace verifier {
class VerifierDeps;
}  // namespace verifier

namespace linker {

class ImageWriter;
class MultiOatRelativePatcher;

enum class CopyOption {
  kNever,
  kAlways,
  kOnlyIfCompressed
};

class OatKeyValueStore {
 public:
  // Puts a key value pair whose key is in `OatHeader::kNonDeterministicFieldsAndLengths`.
  bool PutNonDeterministic(const std::string& k,
                           const std::string& v,
                           bool allow_truncation = false);

  // Puts a key value pair whose key is in `OatHeader::kDeterministicFields`.
  void Put(const std::string& k, const std::string& v);

  // Puts a key value pair whose key is in `OatHeader::kDeterministicFields`.
  void Put(const std::string& k, bool v);

  // Makes sure calls with `const char*` falls into the overload for `std::string`, not the one for
  // `bool`.
  void Put(const std::string& k, const char* v) { Put(k, std::string(v)); }

 private:
  SafeMap<std::string, std::string> map_;

  friend class OatWriter;
};

// OatHeader         variable length with count of D OatDexFiles
//
// TypeLookupTable[0] one descriptor to class def index hash table for each OatDexFile.
// TypeLookupTable[1]
// ...
// TypeLookupTable[D]
//
// ClassOffsets[0]   one table of OatClass offsets for each class def for each OatDexFile.
// ClassOffsets[1]
// ...
// ClassOffsets[D]
//
// OatClass[0]       one variable sized OatClass for each of C DexFile::ClassDefs
// OatClass[1]       contains OatClass entries with class status, offsets to code, etc.
// ...
// OatClass[C]
//
// MethodBssMapping  one variable sized MethodBssMapping for each dex file, optional.
// MethodBssMapping
// ...
// MethodBssMapping
//
// VmapTable         one variable sized VmapTable blob (CodeInfo).
// VmapTable         VmapTables are deduplicated.
// ...
// VmapTable
//
// OatDexFile[0]     one variable sized OatDexFile with offsets to Dex and OatClasses
// OatDexFile[1]
// ...
// OatDexFile[D]
//
// padding           if necessary so that the following code will be page aligned
//
// OatMethodHeader   fixed size header for a CompiledMethod including the size of the MethodCode.
// MethodCode        one variable sized blob with the code of a CompiledMethod.
// OatMethodHeader   (OatMethodHeader, MethodCode) pairs are deduplicated.
// MethodCode
// ...
// OatMethodHeader
// MethodCode
//
class OatWriter {
 public:
  OatWriter(const CompilerOptions& compiler_options,
            TimingLogger* timings,
            ProfileCompilationInfo* info);

  // To produce a valid oat file, the user must first add sources with any combination of
  //   - AddDexFileSource(),
  //   - AddRawDexFileSource(),
  //   - AddVdexDexFilesSource().
  // Then the user must call in order
  //   - WriteAndOpenDexFiles()
  //   - StartRoData()
  //   - FinishVdexFile()
  //   - PrepareLayout(),
  //   - WriteRodata(),
  //   - WriteCode(),
  //   - WriteDataImgRelRo() iff GetDataImgRelRoSize() != 0,
  //   - WriteHeader().

  // Add dex file source(s) from a file, either a plain dex file or
  // a zip file with one or more dex files.
  bool AddDexFileSource(
      const char* filename,
      const char* location);
  // Add dex file source(s) from a file specified by a file handle.
  // Note: The `dex_file_fd` specifies a plain dex file or a zip file.
  bool AddDexFileSource(
      File&& dex_file_fd,
      const char* location);
  // Add dex file source from raw memory.
  bool AddRawDexFileSource(const std::shared_ptr<DexFileContainer>& container,
                           const uint8_t* dex_file_begin,
                           const char* location,
                           uint32_t location_checksum);
  // Add dex file source(s) from a vdex file.
  bool AddVdexDexFilesSource(
      const VdexFile& vdex_file,
      const char* location);
  dchecked_vector<std::string> GetSourceLocations() const;

  // Write raw dex files to the vdex file, mmap the file and open the dex files from it.
  // The `verify` setting dictates whether the dex file verifier should check the dex files.
  // This is generally the case, and should only be false for tests.
  // If `use_existing_vdex` is true, then this method won't actually write the dex files,
  // and the compiler will just re-use the existing vdex file.
  bool WriteAndOpenDexFiles(File* vdex_file,
                            bool verify,
                            bool use_existing_vdex,
                            CopyOption copy_dex_files,
                            /*out*/ std::vector<MemMap>* opened_dex_files_map,
                            /*out*/ std::vector<std::unique_ptr<const DexFile>>* opened_dex_files);
  // Start writing .rodata, including supporting data structures for dex files.
  bool StartRoData(const std::vector<const DexFile*>& dex_files,
                   OutputStream* oat_rodata,
                   OatKeyValueStore* key_value_store);
  // Initialize the writer with the given parameters.
  void Initialize(const CompilerDriver* compiler_driver,
                  const VerificationResults* verification_results,
                  ImageWriter* image_writer,
                  const std::vector<const DexFile*>& dex_files);
  bool FinishVdexFile(File* vdex_file, verifier::VerifierDeps* verifier_deps);

  // Prepare layout of remaining data.
  void PrepareLayout(MultiOatRelativePatcher* relative_patcher);
  // Write the rest of .rodata section (ClassOffsets[], OatClass[], maps).
  bool WriteRodata(OutputStream* out);
  // Write the code to the .text section.
  bool WriteCode(OutputStream* out);
  // Write the image relocation data to the .data.img.rel.ro section.
  bool WriteDataImgRelRo(OutputStream* out);
  // Check the size of the written oat file.
  bool CheckOatSize(OutputStream* out, size_t file_offset, size_t relative_offset);
  // Write the oat header. This finalizes the oat file.
  bool WriteHeader(OutputStream* out);

  // Returns whether the oat file has an associated image.
  bool HasImage() const {
    // Since the image is being created at the same time as the oat file,
    // check if there's an image writer.
    return image_writer_ != nullptr;
  }

  const OatHeader& GetOatHeader() const {
    return *oat_header_;
  }

  size_t GetCodeSize() const {
    return code_size_;
  }

  size_t GetOatSize() const {
    return oat_size_;
  }

  size_t GetDataImgRelRoSize() const {
    return data_img_rel_ro_size_;
  }

  size_t GetDataImgRelRoAppImageOffset() const {
    return data_img_rel_ro_app_image_offset_;
  }

  size_t GetBssSize() const {
    return bss_size_;
  }

  size_t GetBssMethodsOffset() const {
    return bss_methods_offset_;
  }

  size_t GetBssRootsOffset() const {
    return bss_roots_offset_;
  }

  size_t GetVdexSize() const {
    return vdex_size_;
  }

  size_t GetOatDataOffset() const {
    return oat_data_offset_;
  }

  ~OatWriter();

  debug::DebugInfo GetDebugInfo() const;

  const CompilerDriver* GetCompilerDriver() const {
    return compiler_driver_;
  }

  const CompilerOptions& GetCompilerOptions() const {
    return compiler_options_;
  }

 private:
  struct BssMappingInfo;
  class ChecksumUpdatingOutputStream;
  class OatClassHeader;
  class OatClass;
  class OatDexFile;

  // The function VisitDexMethods() below iterates through all the methods in all
  // the compiled dex files in order of their definitions. The method visitor
  // classes provide individual bits of processing for each of the passes we need to
  // first collect the data we want to write to the oat file and then, in later passes,
  // to actually write it.
  class DexMethodVisitor;
  class OatDexMethodVisitor;
  class InitOatClassesMethodVisitor;
  class LayoutCodeMethodVisitor;
  class LayoutReserveOffsetCodeMethodVisitor;
  struct OrderedMethodData;
  class OrderedMethodVisitor;
  class InitCodeMethodVisitor;
  template <bool kDeduplicate> class InitMapMethodVisitor;
  class InitImageMethodVisitor;
  class WriteCodeMethodVisitor;
  class WriteMapMethodVisitor;

  // Visit all the methods in all the compiled dex files in their definition order
  // with a given DexMethodVisitor.
  bool VisitDexMethods(DexMethodVisitor* visitor);

  // If `update_input_vdex` is true, then this method won't actually write the dex files,
  // and the compiler will just re-use the existing vdex file.
  bool WriteDexFiles(File* file,
                     bool verify,
                     bool use_existing_vdex,
                     CopyOption copy_dex_files,
                     /*out*/ std::vector<MemMap>* opened_dex_files_map);
  bool LayoutDexFile(OatDexFile* oat_dex_file);
  bool OpenDexFiles(File* file,
                    /*inout*/ std::vector<MemMap>* opened_dex_files_map,
                    /*out*/ std::vector<std::unique_ptr<const DexFile>>* opened_dex_files);
  void WriteTypeLookupTables(/*out*/std::vector<uint8_t>* buffer);
  void WriteVerifierDeps(verifier::VerifierDeps* verifier_deps,
                         /*out*/std::vector<uint8_t>* buffer);

  size_t InitOatHeader(uint32_t num_dex_files, OatKeyValueStore* key_value_store);
  size_t InitClassOffsets(size_t offset);
  size_t InitOatClasses(size_t offset);
  size_t InitOatMaps(size_t offset);
  size_t InitIndexBssMappings(size_t offset);
  size_t InitOatDexFiles(size_t offset);
  size_t InitBcpBssInfo(size_t offset);
  size_t InitOatCode(size_t offset);
  size_t InitOatCodeDexFiles(size_t offset);
  size_t InitDataImgRelRoLayout(size_t offset);
  void InitBssAndRelRoData();
  void InitBssLayout(InstructionSet instruction_set);
  void AddBssReference(const DexFileReference& ref,
                       size_t number_of_indexes,
                       /*inout*/ SafeMap<const DexFile*, BitVector>* references);

  size_t WriteClassOffsets(OutputStream* out, size_t file_offset, size_t relative_offset);
  size_t WriteClasses(OutputStream* out, size_t file_offset, size_t relative_offset);
  size_t WriteMaps(OutputStream* out, size_t file_offset, size_t relative_offset);
  size_t WriteIndexBssMappings(OutputStream* out, size_t file_offset, size_t relative_offset);
  size_t WriteOatDexFiles(OutputStream* out, size_t file_offset, size_t relative_offset);
  size_t WriteBcpBssInfo(OutputStream* out, size_t file_offset, size_t relative_offset);
  size_t WriteCode(OutputStream* out, size_t file_offset, size_t relative_offset);
  size_t WriteCodeDexFiles(OutputStream* out, size_t file_offset, size_t relative_offset);
  size_t WriteDataImgRelRo(OutputStream* out, size_t file_offset, size_t relative_offset);
  // These helpers extract common code from BCP and non-BCP DexFiles from its corresponding methods.
  size_t WriteIndexBssMappingsHelper(OutputStream* out,
                                     size_t file_offset,
                                     size_t relative_offset,
                                     const DexFile* dex_file,
                                     uint32_t method_bss_mapping_offset,
                                     uint32_t type_bss_mapping_offset,
                                     uint32_t public_type_bss_mapping_offset,
                                     uint32_t package_type_bss_mapping_offset,
                                     uint32_t string_bss_mapping_offset,
                                     uint32_t method_type_bss_mapping_offset);
  size_t InitIndexBssMappingsHelper(size_t offset,
                                    const DexFile* dex_file,
                                    /*inout*/ size_t& number_of_method_dex_files,
                                    /*inout*/ size_t& number_of_type_dex_files,
                                    /*inout*/ size_t& number_of_public_type_dex_files,
                                    /*inout*/ size_t& number_of_package_type_dex_files,
                                    /*inout*/ size_t& number_of_string_dex_files,
                                    /*inout*/ size_t& number_of_method_type_dex_files,
                                    /*inout*/ uint32_t& method_bss_mapping_offset,
                                    /*inout*/ uint32_t& type_bss_mapping_offset,
                                    /*inout*/ uint32_t& public_type_bss_mapping_offset,
                                    /*inout*/ uint32_t& package_type_bss_mapping_offset,
                                    /*inout*/ uint32_t& string_bss_mapping_offset,
                                    /*inout*/ uint32_t& method_type_bss_mapping_offset);

  bool RecordOatDataOffset(OutputStream* out);
  void InitializeTypeLookupTables(
      const std::vector<std::unique_ptr<const DexFile>>& opened_dex_files);
  bool WriteDexLayoutSections(OutputStream* oat_rodata,
                              const std::vector<const DexFile*>& opened_dex_files);
  bool WriteCodeAlignment(OutputStream* out, uint32_t aligned_code_delta);
  bool WriteUpTo16BytesAlignment(OutputStream* out, uint32_t size, uint32_t* stat);
  void SetMultiOatRelativePatcherAdjustment();
  void CloseSources();

  bool MayHaveCompiledMethods() const;

  bool VdexWillContainDexFiles() const {
    return dex_files_ != nullptr && extract_dex_files_into_vdex_;
  }

  // Return the file offset that corresponds to `offset_from_oat_data`.
  size_t GetFileOffset(size_t offset_from_oat_data) const {
    DCHECK_NE(oat_data_offset_, 0u);
    return offset_from_oat_data + oat_data_offset_;
  }

  // Return the next offset (relative to the oat data) that is on or after `offset_from_oat_data`,
  // that is aligned by `alignment` to the beginning of the file.
  size_t GetOffsetFromOatDataAlignedToFile(size_t offset_from_oat_data, size_t alignment) const {
    return RoundUp(GetFileOffset(offset_from_oat_data), alignment) - oat_data_offset_;
  }

  enum class WriteState {
    kAddingDexFileSources,
    kStartRoData,
    kInitialize,
    kPrepareLayout,
    kWriteRoData,
    kWriteText,
    kWriteDataImgRelRo,
    kWriteHeader,
    kDone
  };

  WriteState write_state_;
  TimingLogger* timings_;

  dchecked_vector<debug::MethodDebugInfo> method_info_;

  std::vector<uint8_t> code_info_data_;

  const CompilerDriver* compiler_driver_;
  const CompilerOptions& compiler_options_;
  const VerificationResults* verification_results_;
  ImageWriter* image_writer_;
  // Whether the dex files being compiled are going to be extracted to the vdex.
  bool extract_dex_files_into_vdex_;
  // The start of the vdex file section mmapped for writing dex files.
  uint8_t* vdex_begin_;

  // note OatFile does not take ownership of the DexFiles
  const std::vector<const DexFile*>* dex_files_;

  // Whether this is the primary oat file.
  bool primary_oat_file_;

  // Size required for Vdex data structures.
  size_t vdex_size_;

  // Offset of section holding Dex files inside Vdex.
  size_t vdex_dex_files_offset_;

  // Offset of section holding VerifierDeps inside Vdex.
  size_t vdex_verifier_deps_offset_;

  // Offset of type lookup tables inside Vdex.
  size_t vdex_lookup_tables_offset_;

  // OAT checksum.
  uint32_t oat_checksum_;

  // Size of the .text segment.
  size_t code_size_;

  // Size required for Oat data structures.
  size_t oat_size_;

  // The start of the optional .data.img.rel.ro section.
  size_t data_img_rel_ro_start_;

  // The size of the optional .data.img.rel.ro section holding the image relocations.
  size_t data_img_rel_ro_size_;

  // The start of app image relocations in the .data.img.rel.ro section.
  size_t data_img_rel_ro_app_image_offset_;

  // The start of the optional .bss section.
  size_t bss_start_;

  // The size of the optional .bss section holding the DexCache data and GC roots.
  size_t bss_size_;

  // The offset of the methods in .bss section.
  size_t bss_methods_offset_;

  // The offset of the GC roots in .bss section.
  size_t bss_roots_offset_;

  // OatFile's information regarding the bss metadata for BCP DexFiles. Empty for boot image
  // compiles.
  std::vector<BssMappingInfo> bcp_bss_info_;

  // Map for allocating boot image .data.img.rel.ro entries. Indexed by the boot image offset
  // of the relocation. The value is the assigned offset within the .data.img.rel.ro section.
  SafeMap<uint32_t, size_t> boot_image_rel_ro_entries_;

  // Map for recording references to ArtMethod entries in .bss.
  SafeMap<const DexFile*, BitVector> bss_method_entry_references_;

  // Map for recording references to GcRoot<mirror::Class> entries in .bss.
  SafeMap<const DexFile*, BitVector> bss_type_entry_references_;

  // Map for recording references to public GcRoot<mirror::Class> entries in .bss.
  SafeMap<const DexFile*, BitVector> bss_public_type_entry_references_;

  // Map for recording references to package GcRoot<mirror::Class> entries in .bss.
  SafeMap<const DexFile*, BitVector> bss_package_type_entry_references_;

  // Map for recording references to GcRoot<mirror::String> entries in .bss.
  SafeMap<const DexFile*, BitVector> bss_string_entry_references_;

  // Map for recording references to GcRoot<mirror::MethodType> entries in .bss.
  SafeMap<const DexFile*, BitVector> bss_method_type_entry_references_;

  // Map for allocating app image ArtMethod entries in .data.img.rel.ro. Indexed by MethodReference
  // for the target method in the dex file with the "method reference value comparator" for
  // deduplication. The value is the target offset for patching, starting at
  // `data_img_rel_ro_start_`.
  SafeMap<MethodReference, size_t, MethodReferenceValueComparator> app_image_rel_ro_method_entries_;

  // Map for allocating ArtMethod entries in .bss. Indexed by MethodReference for the target
  // method in the dex file with the "method reference value comparator" for deduplication.
  // The value is the target offset for patching, starting at `bss_start_ + bss_methods_offset_`.
  SafeMap<MethodReference, size_t, MethodReferenceValueComparator> bss_method_entries_;

  // Map for allocating app image Class entries in .data.img.rel.ro. Indexed by TypeReference for
  // the source type in the dex file with the "type value comparator" for deduplication. The value
  // is the target offset for patching, starting at `data_img_rel_ro_start_`.
  SafeMap<TypeReference, size_t, TypeReferenceValueComparator> app_image_rel_ro_type_entries_;

  // Map for allocating Class entries in .bss. Indexed by TypeReference for the source
  // type in the dex file with the "type value comparator" for deduplication. The value
  // is the target offset for patching, starting at `bss_start_ + bss_roots_offset_`.
  SafeMap<TypeReference, size_t, TypeReferenceValueComparator> bss_type_entries_;

  // Map for allocating public Class entries in .bss. Indexed by TypeReference for the source
  // type in the dex file with the "type value comparator" for deduplication. The value
  // is the target offset for patching, starting at `bss_start_ + bss_roots_offset_`.
  SafeMap<TypeReference, size_t, TypeReferenceValueComparator> bss_public_type_entries_;

  // Map for allocating package Class entries in .bss. Indexed by TypeReference for the source
  // type in the dex file with the "type value comparator" for deduplication. The value
  // is the target offset for patching, starting at `bss_start_ + bss_roots_offset_`.
  SafeMap<TypeReference, size_t, TypeReferenceValueComparator> bss_package_type_entries_;

  // Map for allocating String entries in .bss. Indexed by StringReference for the source
  // string in the dex file with the "string value comparator" for deduplication. The value
  // is the target offset for patching, starting at `bss_start_ + bss_roots_offset_`.
  SafeMap<StringReference, size_t, StringReferenceValueComparator> bss_string_entries_;

  // Map for allocating MethodType entries in .bss. Indexed by ProtoReference for the source
  // proto in the dex file with the "proto value comparator" for deduplication. The value
  // is the target offset for patching, starting at `bss_start_ + bss_roots_offset_`.
  SafeMap<ProtoReference, size_t, ProtoReferenceValueComparator> bss_method_type_entries_;

  // Offset of the oat data from the start of the mmapped region of the elf file.
  size_t oat_data_offset_;

  // Fake OatDexFiles to hold type lookup tables for the compiler.
  std::vector<std::unique_ptr<art::OatDexFile>> type_lookup_table_oat_dex_files_;

  // data to write
  OatHeader* oat_header_;
  dchecked_vector<OatDexFile> oat_dex_files_;
  dchecked_vector<OatClassHeader> oat_class_headers_;
  dchecked_vector<OatClass> oat_classes_;
  std::unique_ptr<const std::vector<uint8_t>> jni_dlsym_lookup_trampoline_;
  std::unique_ptr<const std::vector<uint8_t>> jni_dlsym_lookup_critical_trampoline_;
  std::unique_ptr<const std::vector<uint8_t>> quick_generic_jni_trampoline_;
  std::unique_ptr<const std::vector<uint8_t>> quick_imt_conflict_trampoline_;
  std::unique_ptr<const std::vector<uint8_t>> quick_resolution_trampoline_;
  std::unique_ptr<const std::vector<uint8_t>> quick_to_interpreter_bridge_;
  std::unique_ptr<const std::vector<uint8_t>> nterp_trampoline_;

  // output stats
  uint32_t size_vdex_header_ = 0;
  uint32_t size_vdex_checksums_ = 0;
  uint32_t size_dex_file_alignment_ = 0;
  uint32_t size_executable_offset_alignment_ = 0;
  uint32_t size_oat_header_ = 0;
  uint32_t size_oat_header_key_value_store_ = 0;
  uint32_t size_dex_file_ = 0;
  uint32_t size_verifier_deps_ = 0;
  uint32_t size_verifier_deps_alignment_ = 0;
  uint32_t size_vdex_lookup_table_alignment_ = 0;
  uint32_t size_vdex_lookup_table_ = 0;
  uint32_t size_interpreter_to_interpreter_bridge_ = 0;
  uint32_t size_interpreter_to_compiled_code_bridge_ = 0;
  uint32_t size_jni_dlsym_lookup_trampoline_ = 0;
  uint32_t size_jni_dlsym_lookup_critical_trampoline_ = 0;
  uint32_t size_quick_generic_jni_trampoline_ = 0;
  uint32_t size_quick_imt_conflict_trampoline_ = 0;
  uint32_t size_quick_resolution_trampoline_ = 0;
  uint32_t size_quick_to_interpreter_bridge_ = 0;
  uint32_t size_nterp_trampoline_ = 0;
  uint32_t size_trampoline_alignment_ = 0;
  uint32_t size_method_header_ = 0;
  uint32_t size_code_ = 0;
  uint32_t size_code_alignment_ = 0;
  uint32_t size_data_img_rel_ro_ = 0;
  uint32_t size_data_img_rel_ro_alignment_ = 0;
  uint32_t size_relative_call_thunks_ = 0;
  uint32_t size_misc_thunks_ = 0;
  uint32_t size_vmap_table_ = 0;
  uint32_t size_method_info_ = 0;
  uint32_t size_oat_dex_file_location_size_ = 0;
  uint32_t size_oat_dex_file_location_data_ = 0;
  uint32_t size_oat_dex_file_magic_ = 0;
  uint32_t size_oat_dex_file_location_checksum_ = 0;
  uint32_t size_oat_dex_file_sha1_ = 0;
  uint32_t size_oat_dex_file_offset_ = 0;
  uint32_t size_oat_dex_file_class_offsets_offset_ = 0;
  uint32_t size_oat_dex_file_lookup_table_offset_ = 0;
  uint32_t size_oat_dex_file_dex_layout_sections_offset_ = 0;
  uint32_t size_oat_dex_file_dex_layout_sections_ = 0;
  uint32_t size_oat_dex_file_dex_layout_sections_alignment_ = 0;
  uint32_t size_oat_dex_file_method_bss_mapping_offset_ = 0;
  uint32_t size_oat_dex_file_type_bss_mapping_offset_ = 0;
  uint32_t size_oat_dex_file_public_type_bss_mapping_offset_ = 0;
  uint32_t size_oat_dex_file_package_type_bss_mapping_offset_ = 0;
  uint32_t size_oat_dex_file_string_bss_mapping_offset_ = 0;
  uint32_t size_oat_dex_file_method_type_bss_mapping_offset_ = 0;
  uint32_t size_bcp_bss_info_size_ = 0;
  uint32_t size_bcp_bss_info_method_bss_mapping_offset_ = 0;
  uint32_t size_bcp_bss_info_type_bss_mapping_offset_ = 0;
  uint32_t size_bcp_bss_info_public_type_bss_mapping_offset_ = 0;
  uint32_t size_bcp_bss_info_package_type_bss_mapping_offset_ = 0;
  uint32_t size_bcp_bss_info_string_bss_mapping_offset_ = 0;
  uint32_t size_bcp_bss_info_method_type_bss_mapping_offset_ = 0;
  uint32_t size_oat_class_offsets_alignment_ = 0;
  uint32_t size_oat_class_offsets_ = 0;
  uint32_t size_oat_class_type_ = 0;
  uint32_t size_oat_class_status_ = 0;
  uint32_t size_oat_class_num_methods_ = 0;
  uint32_t size_oat_class_method_bitmaps_ = 0;
  uint32_t size_oat_class_method_offsets_ = 0;
  uint32_t size_method_bss_mappings_ = 0;
  uint32_t size_type_bss_mappings_ = 0;
  uint32_t size_public_type_bss_mappings_ = 0;
  uint32_t size_package_type_bss_mappings_ = 0;
  uint32_t size_string_bss_mappings_ = 0;
  uint32_t size_method_type_bss_mappings_ = 0;

  // The helper for processing relative patches is external so that we can patch across oat files.
  MultiOatRelativePatcher* relative_patcher_;

  // Profile info used to generate new layout of files.
  ProfileCompilationInfo* profile_compilation_info_;

  using OrderedMethodList = std::vector<OrderedMethodData>;

  // List of compiled methods, sorted by the order defined in OrderedMethodData.
  // Methods can be inserted more than once in case of duplicated methods.
  // This pointer is only non-null after InitOatCodeDexFiles succeeds.
  std::unique_ptr<OrderedMethodList> ordered_methods_;

  DISALLOW_COPY_AND_ASSIGN(OatWriter);
};

}  // namespace linker
}  // namespace art

#endif  // ART_DEX2OAT_LINKER_OAT_WRITER_H_
