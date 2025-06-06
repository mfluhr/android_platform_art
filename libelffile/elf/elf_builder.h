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

#ifndef ART_LIBELFFILE_ELF_ELF_BUILDER_H_
#define ART_LIBELFFILE_ELF_ELF_BUILDER_H_

#include <vector>
#include <deque>

#include "arch/instruction_set.h"
#include "base/array_ref.h"
#include "base/bit_utils.h"
#include "base/casts.h"
#include "base/leb128.h"
#include "base/unix_file/fd_file.h"
#include "elf/elf_utils.h"
#include "stream/error_delaying_output_stream.h"

namespace art {

// Writes ELF file.
//
// The basic layout of the elf file:
//   Elf_Ehdr                    - The ELF header.
//   Elf_Phdr[]                  - Program headers for the linker.
//   .note.gnu.build-id          - Optional build ID section (SHA-1 digest).
//   .dynstr                     - Names for .dynsym.
//   .dynsym                     - A few oat-specific dynamic symbols.
//   .hash                       - Hash-table for .dynsym.
//   .dynamic                    - Tags which let the linker locate .dynsym.
//   .rodata                     - Oat metadata.
//   .text                       - Compiled code.
//   .bss                        - Zero-initialized writeable section.
//   .dex                        - Reserved NOBITS space for dex-related data.
//   .strtab                     - Names for .symtab.
//   .symtab                     - Debug symbols.
//   .debug_frame                - Unwind information (CFI).
//   .debug_info                 - Debug information.
//   .debug_abbrev               - Decoding information for .debug_info.
//   .debug_str                  - Strings for .debug_info.
//   .debug_line                 - Line number tables.
//   .shstrtab                   - Names of ELF sections.
//   Elf_Shdr[]                  - Section headers.
//
// Some section are optional (the debug sections in particular).
//
// To reduce the amount of padding necessary to page-align sections with
// different permissions (and thus reduce disk usage), we group most read-only
// data sections together at the start of the file. This includes .dynstr,
// .dynsym, .hash, and .dynamic, whose contents are dependent on other sections.
// Therefore, when building the ELF we initially just reserve space for them,
// and write their contents later.
//
// In the cases where we need to buffer, we write the larger section first
// and buffer the smaller one (e.g. .strtab is bigger than .symtab).
//
// The debug sections are written last for easier stripping.
//
template <typename ElfTypes>
class ElfBuilder final {
 public:
  static constexpr size_t kMaxProgramHeaders = 16;
  // SHA-1 digest.  Not using SHA_DIGEST_LENGTH from openssl/sha.h to avoid
  // spreading this header dependency for just this single constant.
  static constexpr size_t kBuildIdLen = 20;

  using Elf_Addr = typename ElfTypes::Addr;
  using Elf_Off = typename ElfTypes::Off;
  using Elf_Word = typename ElfTypes::Word;
  using Elf_Sword = typename ElfTypes::Sword;
  using Elf_Ehdr = typename ElfTypes::Ehdr;
  using Elf_Shdr = typename ElfTypes::Shdr;
  using Elf_Sym = typename ElfTypes::Sym;
  using Elf_Phdr = typename ElfTypes::Phdr;
  using Elf_Dyn = typename ElfTypes::Dyn;

  // Base class of all sections.
  class Section : public OutputStream {
   public:
    Section(ElfBuilder<ElfTypes>* owner,
            const std::string& name,
            Elf_Word type,
            Elf_Word flags,
            const Section* link,
            Elf_Word info,
            Elf_Word align,
            Elf_Word entsize)
        : OutputStream(name),
          owner_(owner),
          header_(),
          section_index_(0),
          name_(name),
          link_(link),
          phdr_flags_(PF_R),
          phdr_type_(0) {
      DCHECK_GE(align, 1u);
      header_.sh_type = type;
      header_.sh_flags = flags;
      header_.sh_info = info;
      header_.sh_addralign = align;
      header_.sh_entsize = entsize;
    }

    // Allocate chunk of virtual memory for this section from the owning ElfBuilder.
    // This must be done at the start for all SHF_ALLOC sections (i.e. mmaped by linker).
    // It is fine to allocate section but never call Start/End() (e.g. the .bss section).
    void AllocateVirtualMemory(Elf_Word size) {
      AllocateVirtualMemory(owner_->virtual_address_, size);
    }

    void AllocateVirtualMemory(Elf_Addr addr, Elf_Word size) {
      CHECK_NE(header_.sh_flags & SHF_ALLOC, 0u);
      Elf_Word align = AddSection();
      CHECK_EQ(header_.sh_addr, 0u);
      header_.sh_addr = RoundUp(addr, align);
      CHECK(header_.sh_size == 0u || header_.sh_size == size);
      header_.sh_size = size;
      CHECK_LE(owner_->virtual_address_, header_.sh_addr);
      owner_->virtual_address_ = header_.sh_addr + header_.sh_size;
    }

    // Start writing file data of this section.
    virtual void Start() {
      CHECK(owner_->current_section_ == nullptr);
      Elf_Word align = AddSection();
      CHECK_EQ(header_.sh_offset, 0u);
      header_.sh_offset = owner_->AlignFileOffset(align);
      owner_->current_section_ = this;
    }

    // Finish writing file data of this section.
    virtual void End() {
      CHECK(owner_->current_section_ == this);
      Elf_Word position = GetPosition();
      CHECK(header_.sh_size == 0u || header_.sh_size == position);
      header_.sh_size = position;
      owner_->current_section_ = nullptr;
    }

    // Get the number of bytes written so far.
    // Only valid while writing the section.
    Elf_Word GetPosition() const {
      CHECK(owner_->current_section_ == this);
      off_t file_offset = owner_->stream_.Seek(0, kSeekCurrent);
      DCHECK_GE(file_offset, (off_t)header_.sh_offset);
      return file_offset - header_.sh_offset;
    }

    // Get the location of this section in virtual memory.
    Elf_Addr GetAddress() const {
      DCHECK_NE(header_.sh_flags & SHF_ALLOC, 0u);
      DCHECK_NE(header_.sh_addr, 0u);
      return header_.sh_addr;
    }

    // This function always succeeds to simplify code.
    // Use builder's Good() to check the actual status.
    bool WriteFully(const void* buffer, size_t byte_count) override {
      CHECK(owner_->current_section_ == this);
      return owner_->stream_.WriteFully(buffer, byte_count);
    }

    // This function always succeeds to simplify code.
    // Use builder's Good() to check the actual status.
    off_t Seek(off_t offset, Whence whence) override {
      // Forward the seek as-is and trust the caller to use it reasonably.
      return owner_->stream_.Seek(offset, whence);
    }

    // This function flushes the output and returns whether it succeeded.
    // If there was a previous failure, this does nothing and returns false, i.e. failed.
    bool Flush() override {
      return owner_->stream_.Flush();
    }

    Elf_Word GetSectionIndex() const {
      DCHECK_NE(section_index_, 0u);
      return section_index_;
    }

    // Returns true if this section has been added.
    bool Exists() const {
      return section_index_ != 0;
    }

   protected:
    // Add this section to the list of generated ELF sections (if not there already).
    // It also ensures the alignment is sufficient to generate valid program headers,
    // since that depends on the previous section. It returns the required alignment.
    Elf_Word AddSection() {
      if (section_index_ == 0) {
        std::vector<Section*>& sections = owner_->sections_;
        Elf_Word last = sections.empty() ? PF_R : sections.back()->phdr_flags_;
        if (phdr_flags_ != last) {
          header_.sh_addralign = kElfSegmentAlignment;  // Page-align if R/W/X flags changed.
        }
        sections.push_back(this);
        section_index_ = sections.size();  // First ELF section has index 1.
      }
      return owner_->write_program_headers_ ? header_.sh_addralign : 1;
    }

    ElfBuilder<ElfTypes>* owner_;
    Elf_Shdr header_;
    Elf_Word section_index_;
    const std::string name_;
    const Section* const link_;
    Elf_Word phdr_flags_;
    Elf_Word phdr_type_;

    friend class ElfBuilder;

    DISALLOW_COPY_AND_ASSIGN(Section);
  };

  class CachedSection : public Section {
   public:
    CachedSection(ElfBuilder<ElfTypes>* owner,
                  const std::string& name,
                  Elf_Word type,
                  Elf_Word flags,
                  const Section* link,
                  Elf_Word info,
                  Elf_Word align,
                  Elf_Word entsize)
        : Section(owner, name, type, flags, link, info, align, entsize), cache_() { }

    Elf_Word Add(const void* data, size_t length) {
      Elf_Word offset = cache_.size();
      const uint8_t* d = reinterpret_cast<const uint8_t*>(data);
      cache_.insert(cache_.end(), d, d + length);
      return offset;
    }

    Elf_Word GetCacheSize() {
      return cache_.size();
    }

    void Write() {
      this->WriteFully(cache_.data(), cache_.size());
      cache_.clear();
      cache_.shrink_to_fit();
    }

    void WriteCachedSection() {
      this->Start();
      Write();
      this->End();
    }

   private:
    std::vector<uint8_t> cache_;
  };

  // Writer of .dynstr section.
  class CachedStringSection final : public CachedSection {
   public:
    CachedStringSection(ElfBuilder<ElfTypes>* owner,
                        const std::string& name,
                        Elf_Word flags,
                        Elf_Word align)
        : CachedSection(owner,
                        name,
                        SHT_STRTAB,
                        flags,
                        /* link= */ nullptr,
                        /* info= */ 0,
                        align,
                        /* entsize= */ 0) { }

    Elf_Word Add(const std::string& name) {
      if (CachedSection::GetCacheSize() == 0u) {
        DCHECK(name.empty());
      }
      return CachedSection::Add(name.c_str(), name.length() + 1);
    }
  };

  // Writer of .strtab and .shstrtab sections.
  class StringSection final : public Section {
   public:
    StringSection(ElfBuilder<ElfTypes>* owner,
                  const std::string& name,
                  Elf_Word flags,
                  Elf_Word align)
        : Section(owner,
                  name,
                  SHT_STRTAB,
                  flags,
                  /* link= */ nullptr,
                  /* info= */ 0,
                  align,
                  /* entsize= */ 0) {
      Reset();
    }

    void Reset() {
      current_offset_ = 0;
      last_name_ = "";
      last_offset_ = 0;
    }

    void Start() {
      Section::Start();
      Write("");  // ELF specification requires that the section starts with empty string.
    }

    Elf_Word Write(std::string_view name) {
      if (current_offset_ == 0) {
        DCHECK(name.empty());
      } else if (name == last_name_) {
        return last_offset_;  // Very simple string de-duplication.
      }
      last_name_ = name;
      last_offset_ = current_offset_;
      this->WriteFully(name.data(), name.length());
      char null_terminator = '\0';
      this->WriteFully(&null_terminator, sizeof(null_terminator));
      current_offset_ += name.length() + 1;
      return last_offset_;
    }

   private:
    Elf_Word current_offset_;
    std::string last_name_;
    Elf_Word last_offset_;
  };

  // Writer of .dynsym and .symtab sections.
  class SymbolSection final : public Section {
   public:
    SymbolSection(ElfBuilder<ElfTypes>* owner,
                  const std::string& name,
                  Elf_Word type,
                  Elf_Word flags,
                  Section* strtab)
        : Section(owner,
                  name,
                  type,
                  flags,
                  strtab,
                  /* info= */ 1,
                  sizeof(Elf_Off),
                  sizeof(Elf_Sym)) {
      syms_.push_back(Elf_Sym());  // The symbol table always has to start with NULL symbol.
    }

    // Buffer symbol for this section.  It will be written later.
    void Add(Elf_Word name,
             const Section* section,
             Elf_Addr addr,
             Elf_Word size,
             uint8_t binding,
             uint8_t type) {
      Elf_Sym sym = Elf_Sym();
      sym.st_name = name;
      sym.st_value = addr;
      sym.st_size = size;
      sym.st_other = 0;
      sym.st_info = (binding << 4) + (type & 0xf);
      Add(sym, section);
    }

    // Buffer symbol for this section.  It will be written later.
    void Add(Elf_Sym sym, const Section* section) {
      if (section != nullptr) {
        DCHECK_LE(section->GetAddress(), sym.st_value);
        DCHECK_LE(sym.st_value, section->GetAddress() + section->header_.sh_size);
        sym.st_shndx = section->GetSectionIndex();
      } else {
        sym.st_shndx = SHN_UNDEF;
      }
      syms_.push_back(sym);
    }

    Elf_Word GetCacheSize() { return syms_.size() * sizeof(Elf_Sym); }

    void WriteCachedSection() {
      auto is_local = [](const Elf_Sym& sym) { return ELF_ST_BIND(sym.st_info) == STB_LOCAL; };
      auto less_then = [is_local](const Elf_Sym& a, const Elf_Sym b) {
        auto tuple_a = std::make_tuple(!is_local(a), a.st_value, a.st_name);
        auto tuple_b = std::make_tuple(!is_local(b), b.st_value, b.st_name);
        return tuple_a < tuple_b;  // Locals first, then sort by address and name offset.
      };
      if (!std::is_sorted(syms_.begin(), syms_.end(), less_then)) {
        std::sort(syms_.begin(), syms_.end(), less_then);
      }
      auto locals_end = std::partition_point(syms_.begin(), syms_.end(), is_local);
      this->header_.sh_info = locals_end - syms_.begin();  // Required by the spec.

      this->Start();
      for (; !syms_.empty(); syms_.pop_front()) {
        this->WriteFully(&syms_.front(), sizeof(Elf_Sym));
      }
      this->End();
    }

   private:
    std::deque<Elf_Sym> syms_;  // Buffered/cached content of the whole section.
  };

  class BuildIdSection final : public Section {
   public:
    BuildIdSection(ElfBuilder<ElfTypes>* owner,
                   const std::string& name,
                   Elf_Word type,
                   Elf_Word flags,
                   const Section* link,
                   Elf_Word info,
                   Elf_Word align,
                   Elf_Word entsize)
        : Section(owner, name, type, flags, link, info, align, entsize),
          digest_start_(-1) {
    }

    Elf_Word GetSize() {
      return 16 + kBuildIdLen;
    }

    void Write() {
      // The size fields are 32-bit on both 32-bit and 64-bit systems, confirmed
      // with the 64-bit linker and libbfd code. The size of name and desc must
      // be a multiple of 4 and it currently is.
      this->WriteUint32(4);  // namesz.
      this->WriteUint32(kBuildIdLen);  // descsz.
      this->WriteUint32(3);  // type = NT_GNU_BUILD_ID.
      this->WriteFully("GNU", 4);  // name.
      digest_start_ = this->Seek(0, kSeekCurrent);
      static_assert(kBuildIdLen % 4 == 0, "expecting a mutliple of 4 for build ID length");
      this->WriteFully(std::string(kBuildIdLen, '\0').c_str(), kBuildIdLen);  // desc.
      DCHECK_EQ(this->GetPosition(), GetSize());
    }

    off_t GetDigestStart() {
      CHECK_GT(digest_start_, 0);
      return digest_start_;
    }

   private:
    bool WriteUint32(uint32_t v) {
      return this->WriteFully(&v, sizeof(v));
    }

    // File offset where the build ID digest starts.
    // Populated with zeros first, then updated with the actual value as the
    // very last thing in the output file creation.
    off_t digest_start_;
  };

  ElfBuilder(InstructionSet isa, OutputStream* output)
      : isa_(isa),
        stream_(output),
        rodata_(this, ".rodata", SHT_PROGBITS, SHF_ALLOC, nullptr, 0, 4u, 0),
        text_(this, ".text", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, nullptr, 0,
            kElfSegmentAlignment, 0),
        data_img_rel_ro_(this, ".data.img.rel.ro", SHT_PROGBITS, SHF_ALLOC, nullptr, 0,
            kElfSegmentAlignment, 0),
        bss_(this, ".bss", SHT_NOBITS, SHF_ALLOC, nullptr, 0, kElfSegmentAlignment, 0),
        dex_(this, ".dex", SHT_NOBITS, SHF_ALLOC, nullptr, 0, kElfSegmentAlignment, 0),
        dynstr_(this, ".dynstr", SHF_ALLOC, 1),
        dynsym_(this, ".dynsym", SHT_DYNSYM, SHF_ALLOC, &dynstr_),
        hash_(this, ".hash", SHT_HASH, SHF_ALLOC, &dynsym_, 0, sizeof(Elf_Word), sizeof(Elf_Word)),
        dynamic_(this, ".dynamic", SHT_DYNAMIC, SHF_ALLOC, &dynstr_, 0, sizeof(Elf_Addr),
            sizeof(Elf_Dyn)),
        strtab_(this, ".strtab", 0, 1),
        symtab_(this, ".symtab", SHT_SYMTAB, 0, &strtab_),
        debug_frame_(this, ".debug_frame", SHT_PROGBITS, 0, nullptr, 0, sizeof(Elf_Addr), 0),
        debug_frame_hdr_(
            this, ".debug_frame_hdr.android", SHT_PROGBITS, 0, nullptr, 0, sizeof(Elf_Addr), 0),
        debug_info_(this, ".debug_info", SHT_PROGBITS, 0, nullptr, 0, 1, 0),
        debug_line_(this, ".debug_line", SHT_PROGBITS, 0, nullptr, 0, 1, 0),
        shstrtab_(this, ".shstrtab", 0, 1),
        build_id_(this, ".note.gnu.build-id", SHT_NOTE, SHF_ALLOC, nullptr, 0, 4, 0),
        current_section_(nullptr),
        started_(false),
        finished_(false),
        write_program_headers_(false),
        loaded_size_(0u),
        virtual_address_(0),
        dynamic_sections_start_(0),
        dynamic_sections_reserved_size_(0u) {
    text_.phdr_flags_ = PF_R | PF_X;
    data_img_rel_ro_.phdr_flags_ = PF_R | PF_W;  // Shall be made read-only at run time.
    bss_.phdr_flags_ = PF_R | PF_W;
    dex_.phdr_flags_ = PF_R;
    dynamic_.phdr_flags_ = PF_R;
    dynamic_.phdr_type_ = PT_DYNAMIC;
    build_id_.phdr_type_ = PT_NOTE;
  }
  ~ElfBuilder() {}

  InstructionSet GetIsa() { return isa_; }
  BuildIdSection* GetBuildId() { return &build_id_; }
  Section* GetRoData() { return &rodata_; }
  Section* GetText() { return &text_; }
  Section* GetDataImgRelRo() { return &data_img_rel_ro_; }
  Section* GetBss() { return &bss_; }
  Section* GetDex() { return &dex_; }
  StringSection* GetStrTab() { return &strtab_; }
  SymbolSection* GetSymTab() { return &symtab_; }
  Section* GetDebugFrame() { return &debug_frame_; }
  Section* GetDebugFrameHdr() { return &debug_frame_hdr_; }
  Section* GetDebugInfo() { return &debug_info_; }
  Section* GetDebugLine() { return &debug_line_; }

  void WriteSection(const char* name, const std::vector<uint8_t>* buffer) {
    std::unique_ptr<Section> s(new Section(this, name, SHT_PROGBITS, 0, nullptr, 0, 1, 0));
    s->Start();
    s->WriteFully(buffer->data(), buffer->size());
    s->End();
    other_sections_.push_back(std::move(s));
  }

  // Reserve space for ELF header and program headers.
  // We do not know the number of headers until later, so
  // it is easiest to just reserve a fixed amount of space.
  // Program headers are required for loading by the linker.
  // It is possible to omit them for ELF files used for debugging.
  void Start(bool write_program_headers = true) {
    int size = sizeof(Elf_Ehdr);
    if (write_program_headers) {
      size += sizeof(Elf_Phdr) * kMaxProgramHeaders;
    }
    stream_.Seek(size, kSeekSet);
    started_ = true;
    virtual_address_ += size;
    write_program_headers_ = write_program_headers;
  }

  off_t End() {
    DCHECK(started_);
    DCHECK(!finished_);
    finished_ = true;

    // Note: loaded_size_ == 0 for tests that don't write .rodata, .text, .bss,
    // .dynstr, dynsym, .hash and .dynamic. These tests should not read loaded_size_.
    CHECK(loaded_size_ == 0 || loaded_size_ == RoundUp(virtual_address_, kElfSegmentAlignment))
        << loaded_size_ << " " << virtual_address_;

    // Write section names and finish the section headers.
    shstrtab_.Start();
    shstrtab_.Write("");
    for (auto* section : sections_) {
      section->header_.sh_name = shstrtab_.Write(section->name_);
      if (section->link_ != nullptr) {
        section->header_.sh_link = section->link_->GetSectionIndex();
      }
      if (section->header_.sh_offset == 0) {
        section->header_.sh_type = SHT_NOBITS;
      }
    }
    shstrtab_.End();

    // Write section headers at the end of the ELF file.
    std::vector<Elf_Shdr> shdrs;
    shdrs.reserve(1u + sections_.size());
    shdrs.push_back(Elf_Shdr());  // NULL at index 0.
    for (auto* section : sections_) {
      shdrs.push_back(section->header_);
    }
    Elf_Off section_headers_offset;
    section_headers_offset = AlignFileOffset(sizeof(Elf_Off));
    stream_.WriteFully(shdrs.data(), shdrs.size() * sizeof(shdrs[0]));
    off_t file_size = stream_.Seek(0, kSeekCurrent);

    // Flush everything else before writing the program headers. This should prevent
    // the OS from reordering writes, so that we don't end up with valid headers
    // and partially written data if we suddenly lose power, for example.
    stream_.Flush();

    // The main ELF header.
    Elf_Ehdr elf_header = MakeElfHeader(isa_);
    elf_header.e_shoff = section_headers_offset;
    elf_header.e_shnum = shdrs.size();
    elf_header.e_shstrndx = shstrtab_.GetSectionIndex();

    // Program headers (i.e. mmap instructions).
    std::vector<Elf_Phdr> phdrs;
    if (write_program_headers_) {
      phdrs = MakeProgramHeaders();
      CHECK_LE(phdrs.size(), kMaxProgramHeaders);
      elf_header.e_phoff = sizeof(Elf_Ehdr);
      elf_header.e_phnum = phdrs.size();
    }

    stream_.Seek(0, kSeekSet);
    stream_.WriteFully(&elf_header, sizeof(elf_header));
    stream_.WriteFully(phdrs.data(), phdrs.size() * sizeof(phdrs[0]));
    stream_.Flush();

    return file_size;
  }

  // This has the same effect as running the "strip" command line tool.
  // It removes all debugging sections (but it keeps mini-debug-info).
  // It returns the ELF file size (as the caller needs to truncate it).
  off_t Strip() {
    DCHECK(finished_);
    finished_ = false;
    Elf_Off end = 0;
    std::vector<Section*> non_debug_sections;
    for (Section* section : sections_) {
      if (section == &shstrtab_ ||  // Section names will be recreated.
          section == &symtab_ ||
          section == &strtab_ ||
          section->name_.find(".debug_") == 0) {
        section->header_.sh_offset = 0;
        section->header_.sh_size = 0;
        section->section_index_ = 0;
      } else {
        if (section->header_.sh_type != SHT_NOBITS) {
          DCHECK_LE(section->header_.sh_offset, end + kElfSegmentAlignment)
              << "Large gap between sections";
          end = std::max<off_t>(end, section->header_.sh_offset + section->header_.sh_size);
        }
        non_debug_sections.push_back(section);
      }
    }
    shstrtab_.Reset();
    // Write the non-debug section headers, program headers, and ELF header again.
    sections_ = std::move(non_debug_sections);
    stream_.Seek(end, kSeekSet);
    return End();
  }

  // Reserve space for: .dynstr, .dynsym, .hash and .dynamic.
  //
  // Dynamic section content is dependent on subsequent sections. Here, reserve enough
  // space for it. We will write the content later (in PrepareDynamicSection).
  void ReserveSpaceForDynamicSection(const std::string& elf_file_path) {
    CHECK_EQ(dynamic_sections_start_, 0);
    CHECK_EQ(dynamic_sections_reserved_size_, 0u);
    CHECK(!rodata_.Exists());

    off_t offset = stream_.Seek(0, kSeekCurrent);
    dynamic_sections_start_ = offset;

    dynstr_.AddSection();
    // We don't expect that .dynstr section can have any alignment requirements.
    DCHECK_EQ(dynstr_.header_.sh_addralign, 1u);
    offset += []() consteval {
      size_t size = 0;
      for (size_t i = 0; i < kDynamicSymbolCount; i++) {
        DynamicSymbol sym = static_cast<DynamicSymbol>(i);
        size += GetDynamicSymbolName(sym).length() + 1;
      }
      return size;
    }();
    offset += GetSoname(elf_file_path).length() + 1;

    dynsym_.AddSection();
    offset = RoundUp(offset, dynsym_.header_.sh_addralign);
    offset += kDynamicSymbolCount * sizeof(Elf_Sym);

    hash_.AddSection();
    offset = RoundUp(offset, hash_.header_.sh_addralign);
    offset += PrepareDynamicSymbolHashtable(kDynamicSymbolCount, /*hashtable=*/ nullptr);

    dynamic_.AddSection();
    offset = RoundUp(offset, dynamic_.header_.sh_addralign);
    offset += kDynamicEntriesCount * sizeof(Elf_Dyn);

    dynamic_sections_reserved_size_ = offset - dynamic_sections_start_;

    stream_.Seek(offset, kSeekSet);
  }

  // The running program does not have access to section headers
  // and the loader is not supposed to use them either.
  // The dynamic sections therefore replicates some of the layout
  // information like the address and size of .rodata and .text.
  // It also contains other metadata like the SONAME.
  // The .dynamic section is found using the PT_DYNAMIC program header.
  void PrepareDynamicSection(const std::string& elf_file_path,
                             Elf_Word rodata_size,
                             Elf_Word text_size,
                             Elf_Word data_img_rel_ro_size,
                             Elf_Word data_img_rel_ro_app_image_offset,
                             Elf_Word bss_size,
                             Elf_Word bss_methods_offset,
                             Elf_Word bss_roots_offset,
                             Elf_Word dex_size) {
    CHECK_NE(dynamic_sections_reserved_size_, 0u);

    // Skip over the reserved memory for dynamic sections - we prepare them later
    // due to dependencies.
    Elf_Addr dynamic_sections_address = virtual_address_;
    virtual_address_ += dynamic_sections_reserved_size_;

    rodata_.AllocateVirtualMemory(rodata_size);
    text_.AllocateVirtualMemory(text_size);
    if (data_img_rel_ro_size != 0) {
      data_img_rel_ro_.AllocateVirtualMemory(data_img_rel_ro_size);
    }
    if (bss_size != 0) {
      bss_.AllocateVirtualMemory(bss_size);
    }
    if (dex_size != 0) {
      dex_.AllocateVirtualMemory(dex_size);
    }

    // Cache .dynstr, .dynsym and .hash data.
    dynstr_.Add("");  // dynstr should start with empty string.
    Elf_Word oatdata = dynstr_.Add(GetDynamicSymbolName(DynamicSymbol::kOatData));
    dynsym_.Add(oatdata, &rodata_, rodata_.GetAddress(), rodata_size, STB_GLOBAL, STT_OBJECT);
    if (text_size != 0u) {
      // The runtime does not care about the size of this symbol (it uses the "lastword" symbol).
      // We use size 0 (meaning "unknown size" in ELF) to prevent overlap with the debug symbols.
      Elf_Word oatexec = dynstr_.Add(GetDynamicSymbolName(DynamicSymbol::kOatExec));
      dynsym_.Add(oatexec, &text_, text_.GetAddress(), /* size= */ 0, STB_GLOBAL, STT_OBJECT);
      Elf_Word oatlastword = dynstr_.Add(GetDynamicSymbolName(DynamicSymbol::kOatLastWord));
      Elf_Word oatlastword_address = text_.GetAddress() + text_size - 4;
      dynsym_.Add(oatlastword, &text_, oatlastword_address, 4, STB_GLOBAL, STT_OBJECT);
    } else if (rodata_size != 0) {
      // rodata_ can be size 0 for dwarf_test.
      Elf_Word oatlastword = dynstr_.Add(GetDynamicSymbolName(DynamicSymbol::kOatLastWord));
      Elf_Word oatlastword_address = rodata_.GetAddress() + rodata_size - 4;
      dynsym_.Add(oatlastword, &rodata_, oatlastword_address, 4, STB_GLOBAL, STT_OBJECT);
    }
    DCHECK_LE(data_img_rel_ro_app_image_offset, data_img_rel_ro_size);
    if (data_img_rel_ro_size != 0u) {
      Elf_Word oatdataimgrelro = dynstr_.Add(GetDynamicSymbolName(DynamicSymbol::kOatDataImgRelRo));
      dynsym_.Add(oatdataimgrelro,
                  &data_img_rel_ro_,
                  data_img_rel_ro_.GetAddress(),
                  data_img_rel_ro_size,
                  STB_GLOBAL,
                  STT_OBJECT);
      Elf_Word oatdataimgrelrolastword =
          dynstr_.Add(GetDynamicSymbolName(DynamicSymbol::kOatDataImgRelRoLastWord));
      dynsym_.Add(oatdataimgrelrolastword,
                  &data_img_rel_ro_,
                  data_img_rel_ro_.GetAddress() + data_img_rel_ro_size - 4,
                  4,
                  STB_GLOBAL,
                  STT_OBJECT);
      if (data_img_rel_ro_app_image_offset != data_img_rel_ro_size) {
        Elf_Word oatdataimgrelroappimage =
            dynstr_.Add(GetDynamicSymbolName(DynamicSymbol::kOatDataImgRelRoAppImage));
        dynsym_.Add(oatdataimgrelroappimage,
                    &data_img_rel_ro_,
                    data_img_rel_ro_.GetAddress() + data_img_rel_ro_app_image_offset,
                    data_img_rel_ro_app_image_offset,
                    STB_GLOBAL,
                    STT_OBJECT);
      }
    }
    DCHECK_LE(bss_roots_offset, bss_size);
    if (bss_size != 0u) {
      Elf_Word oatbss = dynstr_.Add(GetDynamicSymbolName(DynamicSymbol::kOatBss));
      dynsym_.Add(oatbss, &bss_, bss_.GetAddress(), bss_roots_offset, STB_GLOBAL, STT_OBJECT);
      DCHECK_LE(bss_methods_offset, bss_roots_offset);
      DCHECK_LE(bss_roots_offset, bss_size);
      // Add a symbol marking the start of the methods part of the .bss, if not empty.
      if (bss_methods_offset != bss_roots_offset) {
        Elf_Word bss_methods_address = bss_.GetAddress() + bss_methods_offset;
        Elf_Word bss_methods_size = bss_roots_offset - bss_methods_offset;
        Elf_Word oatbssroots = dynstr_.Add(GetDynamicSymbolName(DynamicSymbol::kOatBssMethods));
        dynsym_.Add(
            oatbssroots, &bss_, bss_methods_address, bss_methods_size, STB_GLOBAL, STT_OBJECT);
      }
      // Add a symbol marking the start of the GC roots part of the .bss, if not empty.
      if (bss_roots_offset != bss_size) {
        Elf_Word bss_roots_address = bss_.GetAddress() + bss_roots_offset;
        Elf_Word bss_roots_size = bss_size - bss_roots_offset;
        Elf_Word oatbssroots = dynstr_.Add(GetDynamicSymbolName(DynamicSymbol::kOatBssRoots));
        dynsym_.Add(
            oatbssroots, &bss_, bss_roots_address, bss_roots_size, STB_GLOBAL, STT_OBJECT);
      }
      Elf_Word oatbsslastword = dynstr_.Add(GetDynamicSymbolName(DynamicSymbol::kOatBssLastWord));
      Elf_Word bsslastword_address = bss_.GetAddress() + bss_size - 4;
      dynsym_.Add(oatbsslastword, &bss_, bsslastword_address, 4, STB_GLOBAL, STT_OBJECT);
    }
    if (dex_size != 0u) {
      Elf_Word oatdex = dynstr_.Add(GetDynamicSymbolName(DynamicSymbol::kOatDex));
      dynsym_.Add(oatdex, &dex_, dex_.GetAddress(), /* size= */ 0, STB_GLOBAL, STT_OBJECT);
      Elf_Word oatdexlastword = dynstr_.Add(GetDynamicSymbolName(DynamicSymbol::kOatDexLastWord));
      Elf_Word oatdexlastword_address = dex_.GetAddress() + dex_size - 4;
      dynsym_.Add(oatdexlastword, &dex_, oatdexlastword_address, 4, STB_GLOBAL, STT_OBJECT);
    }

    Elf_Word soname_offset = dynstr_.Add(GetSoname(elf_file_path));

    // We do not really need a hash-table since there is so few entries.
    // However, the hash-table is the only way the linker can actually
    // determine the number of symbols in .dynsym so it is required.
    int count = dynsym_.GetCacheSize() / sizeof(Elf_Sym);  // Includes NULL.
    std::vector<Elf_Word> hash;
    PrepareDynamicSymbolHashtable(count, &hash);
    hash_.Add(hash.data(), hash.size() * sizeof(hash[0]));

    Elf_Addr current_virtual_address = virtual_address_;
    virtual_address_ = dynamic_sections_address;

    // Allocate all remaining sections.
    dynstr_.AllocateVirtualMemory(dynstr_.GetCacheSize());
    dynsym_.AllocateVirtualMemory(dynsym_.GetCacheSize());
    hash_.AllocateVirtualMemory(hash_.GetCacheSize());

    Elf_Dyn dyns[] = {
      { .d_tag = DT_HASH,   .d_un = { .d_ptr = hash_.GetAddress() }, },
      { .d_tag = DT_STRTAB, .d_un = { .d_ptr = dynstr_.GetAddress() }, },
      { .d_tag = DT_SYMTAB, .d_un = { .d_ptr = dynsym_.GetAddress() }, },
      { .d_tag = DT_SYMENT, .d_un = { .d_ptr = sizeof(Elf_Sym) }, },
      { .d_tag = DT_STRSZ,  .d_un = { .d_ptr = dynstr_.GetCacheSize() }, },
      { .d_tag = DT_SONAME, .d_un = { .d_ptr = soname_offset }, },
      { .d_tag = DT_NULL,   .d_un = { .d_ptr = 0 }, },
    };
    static_assert(sizeof(dyns) == kDynamicEntriesCount * sizeof(dyns[0]));

    dynamic_.Add(&dyns, sizeof(dyns));
    dynamic_.AllocateVirtualMemory(dynamic_.GetCacheSize());

    CHECK_LE(virtual_address_, rodata_.GetAddress());
    virtual_address_ = current_virtual_address;

    loaded_size_ = RoundUp(virtual_address_, kElfSegmentAlignment);
  }

  void WriteDynamicSection() {
    CHECK_NE(dynamic_sections_start_, 0);
    CHECK_NE(dynamic_sections_reserved_size_, 0u);

    off_t current_offset = stream_.Seek(0, kSeekCurrent);
    stream_.Seek(dynamic_sections_start_, kSeekSet);

    dynstr_.WriteCachedSection();
    dynsym_.WriteCachedSection();
    hash_.WriteCachedSection();
    dynamic_.WriteCachedSection();

    DCHECK_LE(stream_.Seek(0, kSeekCurrent),
        static_cast<off_t>(dynamic_sections_start_ + dynamic_sections_reserved_size_));
    stream_.Seek(current_offset, kSeekSet);
  }

  Elf_Word GetLoadedSize() {
    CHECK_NE(loaded_size_, 0u);
    return loaded_size_;
  }

  void WriteBuildIdSection() {
    build_id_.Start();
    build_id_.Write();
    build_id_.End();
  }

  void WriteBuildId(uint8_t build_id[kBuildIdLen]) {
    stream_.Seek(build_id_.GetDigestStart(), kSeekSet);
    stream_.WriteFully(build_id, kBuildIdLen);
    stream_.Flush();
  }

  // Returns true if all writes and seeks on the output stream succeeded.
  bool Good() {
    return stream_.Good();
  }

  // Returns the builder's internal stream.
  OutputStream* GetStream() {
    return &stream_;
  }

  off_t AlignFileOffset(size_t alignment) {
     return stream_.Seek(RoundUp(stream_.Seek(0, kSeekCurrent), alignment), kSeekSet);
  }

  static InstructionSet GetIsaFromHeader(const Elf_Ehdr& header) {
    switch (header.e_machine) {
      case EM_ARM:
        return InstructionSet::kThumb2;
      case EM_AARCH64:
        return InstructionSet::kArm64;
      case EM_RISCV:
        return InstructionSet::kRiscv64;
      case EM_386:
        return InstructionSet::kX86;
      case EM_X86_64:
        return InstructionSet::kX86_64;
    }
    LOG(FATAL) << "Unknown architecture: " << header.e_machine;
    UNREACHABLE();
  }

 private:
  static Elf_Ehdr MakeElfHeader(InstructionSet isa) {
    Elf_Ehdr elf_header = Elf_Ehdr();
    switch (isa) {
      case InstructionSet::kArm:
        // Fall through.
      case InstructionSet::kThumb2: {
        elf_header.e_machine = EM_ARM;
        elf_header.e_flags = EF_ARM_EABI_VER5;
        break;
      }
      case InstructionSet::kArm64: {
        elf_header.e_machine = EM_AARCH64;
        elf_header.e_flags = 0;
        break;
      }
      case InstructionSet::kRiscv64: {
        elf_header.e_machine = EM_RISCV;
        elf_header.e_flags = EF_RISCV_RVC | EF_RISCV_FLOAT_ABI_DOUBLE;
        break;
      }
      case InstructionSet::kX86: {
        elf_header.e_machine = EM_386;
        elf_header.e_flags = 0;
        break;
      }
      case InstructionSet::kX86_64: {
        elf_header.e_machine = EM_X86_64;
        elf_header.e_flags = 0;
        break;
      }
      case InstructionSet::kNone: {
        LOG(FATAL) << "No instruction set";
        break;
      }
      default: {
        LOG(FATAL) << "Unknown instruction set " << isa;
      }
    }
    DCHECK_EQ(GetIsaFromHeader(elf_header),
              (isa == InstructionSet::kArm) ? InstructionSet::kThumb2 : isa);

    elf_header.e_ident[EI_MAG0]       = ELFMAG0;
    elf_header.e_ident[EI_MAG1]       = ELFMAG1;
    elf_header.e_ident[EI_MAG2]       = ELFMAG2;
    elf_header.e_ident[EI_MAG3]       = ELFMAG3;
    elf_header.e_ident[EI_CLASS]      = (sizeof(Elf_Addr) == sizeof(Elf32_Addr))
                                         ? ELFCLASS32 : ELFCLASS64;
    elf_header.e_ident[EI_DATA]       = ELFDATA2LSB;
    elf_header.e_ident[EI_VERSION]    = EV_CURRENT;
    elf_header.e_ident[EI_OSABI]      = ELFOSABI_LINUX;
    elf_header.e_ident[EI_ABIVERSION] = 0;
    elf_header.e_type = ET_DYN;
    elf_header.e_version = 1;
    elf_header.e_entry = 0;
    elf_header.e_ehsize = sizeof(Elf_Ehdr);
    elf_header.e_phentsize = sizeof(Elf_Phdr);
    elf_header.e_shentsize = sizeof(Elf_Shdr);
    return elf_header;
  }

  // Create program headers based on written sections.
  std::vector<Elf_Phdr> MakeProgramHeaders() {
    CHECK(!sections_.empty());
    std::vector<Elf_Phdr> phdrs;
    {
      // The program headers must start with PT_PHDR which is used in
      // loaded process to determine the number of program headers.
      Elf_Phdr phdr = Elf_Phdr();
      phdr.p_type    = PT_PHDR;
      phdr.p_flags   = PF_R;
      phdr.p_offset  = phdr.p_vaddr = phdr.p_paddr = sizeof(Elf_Ehdr);
      phdr.p_filesz  = phdr.p_memsz = 0;  // We need to fill this later.
      phdr.p_align   = sizeof(Elf_Off);
      phdrs.push_back(phdr);
      // Tell the linker to mmap the start of file to memory.
      Elf_Phdr load = Elf_Phdr();
      load.p_type    = PT_LOAD;
      load.p_flags   = PF_R;
      load.p_offset  = load.p_vaddr = load.p_paddr = 0;
      load.p_filesz  = load.p_memsz = sizeof(Elf_Ehdr) + sizeof(Elf_Phdr) * kMaxProgramHeaders;
      load.p_align   = kElfSegmentAlignment;
      phdrs.push_back(load);
    }
    // Create program headers for sections.
    for (auto* section : sections_) {
      const Elf_Shdr& shdr = section->header_;
      if ((shdr.sh_flags & SHF_ALLOC) != 0 && shdr.sh_size != 0) {
        DCHECK(shdr.sh_addr != 0u) << "Allocate virtual memory for the section";
        // PT_LOAD tells the linker to mmap part of the file.
        // The linker can only mmap page-aligned sections.
        // Single PT_LOAD may contain several ELF sections.
        Elf_Phdr& prev = phdrs.back();
        Elf_Phdr load = Elf_Phdr();
        load.p_type   = PT_LOAD;
        load.p_flags  = section->phdr_flags_;
        load.p_offset = shdr.sh_offset;
        load.p_vaddr  = load.p_paddr = shdr.sh_addr;
        load.p_filesz = (shdr.sh_type != SHT_NOBITS ? shdr.sh_size : 0u);
        load.p_memsz  = shdr.sh_size;
        load.p_align  = shdr.sh_addralign;
        if (prev.p_type == load.p_type &&
            prev.p_flags == load.p_flags &&
            prev.p_filesz == prev.p_memsz &&  // Do not merge .bss
            load.p_filesz == load.p_memsz) {  // Do not merge .bss
          // Merge this PT_LOAD with the previous one.
          Elf_Word size = shdr.sh_offset + shdr.sh_size - prev.p_offset;
          prev.p_filesz = size;
          prev.p_memsz  = size;
        } else {
          // If we are adding new load, it must be aligned.
          CHECK_EQ(shdr.sh_addralign, (Elf_Word)kElfSegmentAlignment);
          phdrs.push_back(load);
        }
      }
    }
    for (auto* section : sections_) {
      const Elf_Shdr& shdr = section->header_;
      if ((shdr.sh_flags & SHF_ALLOC) != 0 && shdr.sh_size != 0) {
        // Other PT_* types allow the program to locate interesting
        // parts of memory at runtime. They must overlap with PT_LOAD.
        if (section->phdr_type_ != 0) {
          Elf_Phdr phdr = Elf_Phdr();
          phdr.p_type   = section->phdr_type_;
          phdr.p_flags  = section->phdr_flags_;
          phdr.p_offset = shdr.sh_offset;
          phdr.p_vaddr  = phdr.p_paddr = shdr.sh_addr;
          phdr.p_filesz = phdr.p_memsz = shdr.sh_size;
          phdr.p_align  = shdr.sh_addralign;
          phdrs.push_back(phdr);
        }
      }
    }
    // Set the size of the initial PT_PHDR.
    CHECK_EQ(phdrs[0].p_type, (Elf_Word)PT_PHDR);
    phdrs[0].p_filesz = phdrs[0].p_memsz = phdrs.size() * sizeof(Elf_Phdr);

    return phdrs;
  }

  enum class DynamicSymbol {
    kNull,
    kOatData,
    kOatExec,
    kOatLastWord,
    kOatDataImgRelRo,
    kOatDataImgRelRoLastWord,
    kOatDataImgRelRoAppImage,
    kOatBss,
    kOatBssMethods,
    kOatBssRoots,
    kOatBssLastWord,
    kOatDex,
    kOatDexLastWord,
    kLast = kOatDexLastWord
  };

  static constexpr size_t kDynamicSymbolCount = static_cast<size_t>(DynamicSymbol::kLast) + 1;
  static constexpr size_t kDynamicEntriesCount = 7;

  static constexpr std::string GetDynamicSymbolName(DynamicSymbol sym) {
    switch (sym) {
      case DynamicSymbol::kNull:
        return "";
      case DynamicSymbol::kOatData:
        return "oatdata";
      case DynamicSymbol::kOatExec:
        return "oatexec";
      case DynamicSymbol::kOatLastWord:
        return "oatlastword";
      case DynamicSymbol::kOatDataImgRelRo:
        return "oatdataimgrelro";
      case DynamicSymbol::kOatDataImgRelRoLastWord:
        return "oatdataimgrelrolastword";
      case DynamicSymbol::kOatDataImgRelRoAppImage:
        return "oatdataimgrelroappimage";
      case DynamicSymbol::kOatBss:
        return "oatbss";
      case DynamicSymbol::kOatBssMethods:
        return "oatbssmethods";
      case DynamicSymbol::kOatBssRoots:
        return "oatbssroots";
      case DynamicSymbol::kOatBssLastWord:
        return "oatbsslastword";
      case DynamicSymbol::kOatDex:
        return "oatdex";
      case DynamicSymbol::kOatDexLastWord:
        return "oatdexlastword";
    }
  }

  // This method builds a hashtable for dynamic symbols using `hashtable` as a storage.
  // If `hashtable` is nullptr, it just calculate its size in bytes and returns it.
  static size_t PrepareDynamicSymbolHashtable(size_t count, std::vector<Elf_Word> *hashtable) {
    size_t size = 0;
    auto write = [&size, hashtable](Elf_Word value) {
      if (hashtable) {
        hashtable->push_back(value);
      }
      size += sizeof(value);
    };

    write(1);  // Number of buckets.
    write(count);  // Number of chains.
    // Buckets.  Having just one makes it linear search.
    write(1);  // Point to first non-NULL symbol.
    // Chains.  This creates linked list of symbols.
    write(0);  // Placeholder entry for the NULL symbol.
    for (size_t i = 1; i < count - 1; i++) {
      write(i + 1);  // Each symbol points to the next one.
    }
    write(0);  // Last symbol terminates the chain.

    return size;
  }

  static std::string GetSoname(const std::string& elf_file_path) {
    std::string soname(elf_file_path);
    size_t directory_separator_pos = soname.rfind('/');
    if (directory_separator_pos != std::string::npos) {
      soname = soname.substr(directory_separator_pos + 1);
    }
    return soname;
  }

  InstructionSet isa_;

  ErrorDelayingOutputStream stream_;

  Section rodata_;
  Section text_;
  Section data_img_rel_ro_;
  Section bss_;
  Section dex_;
  CachedStringSection dynstr_;
  SymbolSection dynsym_;
  CachedSection hash_;
  CachedSection dynamic_;
  StringSection strtab_;
  SymbolSection symtab_;
  Section debug_frame_;
  Section debug_frame_hdr_;
  Section debug_info_;
  Section debug_line_;
  StringSection shstrtab_;
  BuildIdSection build_id_;
  std::vector<std::unique_ptr<Section>> other_sections_;

  // List of used section in the order in which they were written.
  std::vector<Section*> sections_;
  Section* current_section_;  // The section which is currently being written.

  bool started_;
  bool finished_;
  bool write_program_headers_;

  // The size of the memory taken by the ELF file when loaded.
  size_t loaded_size_;

  // Used for allocation of virtual address space.
  Elf_Addr virtual_address_;

  // Offset in the ELF where the first dynamic section is written (.dynstr).
  off_t dynamic_sections_start_;

  // Size reserved for dynamic sections: .dynstr, .dynsym, .hash and .dynamic.
  size_t dynamic_sections_reserved_size_;

  DISALLOW_COPY_AND_ASSIGN(ElfBuilder);
};

}  // namespace art

#endif  // ART_LIBELFFILE_ELF_ELF_BUILDER_H_
