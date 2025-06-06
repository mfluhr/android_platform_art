/*
 * Copyright (C) 2008 The Android Open Source Project
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

#ifndef ART_LIBARTBASE_BASE_ZIP_ARCHIVE_H_
#define ART_LIBARTBASE_BASE_ZIP_ARCHIVE_H_

#include <stdint.h>

#include <memory>
#include <string>

#include "globals.h"
#include "mem_map.h"
#include "os.h"
#include "safe_map.h"
#include "unix_file/random_access_file.h"

// system/core/zip_archive definitions.
struct ZipArchive;
struct ZipEntry;
using ZipArchiveHandle = ZipArchive*;

namespace art {

class ZipArchive;
class MemMap;

class ZipEntry {
 public:
  // Extracts this entry to file.
  // Returns true on success, false on failure.
  bool ExtractToFile(File& file, /*out*/std::string* error_msg);
  // Extract this entry to anonymous memory (R/W).
  // Returns null on failure and sets error_msg.
  MemMap ExtractToMemMap(const char* zip_filename,
                         const char* entry_filename,
                         /*out*/std::string* error_msg);
  // Extracts this entry to memory. Stores `GetUncompressedSize()` bytes on success.
  // Returns true on success, false on failure.
  bool ExtractToMemory(/*out*/uint8_t* buffer, /*out*/std::string* error_msg);
  // Create a file-backed private (clean, R/W) memory mapping to this entry.
  // 'zip_filename' is used for diagnostics only,
  //   the original file that the ZipArchive was open with is used
  //   for the mapping.
  //
  // Will only succeed if the entry is stored uncompressed.
  // Returns invalid MemMap on failure and sets error_msg.
  MemMap MapDirectlyFromFile(const char* zip_filename, /*out*/std::string* error_msg);
  virtual ~ZipEntry();

  MemMap MapDirectlyOrExtract(const char* zip_filename,
                              const char* entry_filename,
                              std::string* error_msg,
                              size_t alignment);

  uint32_t GetUncompressedLength() const;
  uint32_t GetCrc32() const;

  bool IsUncompressed() const;
  bool IsAlignedTo(size_t alignment) const;
  off_t GetOffset() const;

 private:
  ZipEntry(ZipArchiveHandle handle,
           ::ZipEntry* zip_entry,
           const std::string& entry_name)
    : handle_(handle), zip_entry_(zip_entry), entry_name_(entry_name) {}

  ZipArchiveHandle handle_;
  ::ZipEntry* const zip_entry_;
  std::string const entry_name_;

  friend class ZipArchive;
  DISALLOW_COPY_AND_ASSIGN(ZipEntry);
};

class ZipArchive {
 public:
  // return new ZipArchive instance on success, null on error.
  static ZipArchive* Open(const char* filename, std::string* error_msg);
  static ZipArchive* OpenFromFd(int fd, const char* filename, std::string* error_msg);
  static ZipArchive* OpenFromOwnedFd(int fd, const char* filename, std::string* error_msg);
  static ZipArchive* OpenFromMemory(const uint8_t* data,
                                    size_t size,
                                    const char* filename,
                                    std::string* error_msg);

  ZipEntry* Find(const char* name, std::string* error_msg) const;

  // Same as Find, but doesn't return an error message if the entry is not found. The callers
  // should expect that the returned pointer is null while the error message is empty.
  ZipEntry* FindOrNull(const char* name, std::string* error_msg) const;

  ~ZipArchive();

 private:
  static ZipArchive* OpenFromFdInternal(int fd,
                                        bool assume_ownership,
                                        const char* filename,
                                        std::string* error_msg);

  explicit ZipArchive(ZipArchiveHandle handle) : handle_(handle) {}

  ZipEntry* FindImpl(const char* name, bool allow_entry_not_found, std::string* error_msg) const;

  friend class ZipEntry;

  ZipArchiveHandle handle_;

  DISALLOW_COPY_AND_ASSIGN(ZipArchive);
};

}  // namespace art

#endif  // ART_LIBARTBASE_BASE_ZIP_ARCHIVE_H_
