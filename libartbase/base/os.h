/*
 * Copyright (C) 2009 The Android Open Source Project
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

#ifndef ART_LIBARTBASE_BASE_OS_H_
#define ART_LIBARTBASE_BASE_OS_H_

#include <stdint.h>
#include <sys/types.h>

#include <cstddef>
#include <memory>
#include <string>

#include "unix_file/fd_file.h"

namespace art {

using File = ::unix_file::FdFile;

struct FileWithRange {
  std::unique_ptr<File> file;
  off_t start;
  size_t length;

  static FileWithRange Invalid();
};

// Interface to the underlying OS platform.
class OS {
 public:
  // Open an existing file with read only access.
  static File* OpenFileForReading(const char* name);

  // Open an existing file with read/write access.
  static File* OpenFileReadWrite(const char* name);

  // Create an empty file with read/write access. This is a *new* file, that is, if the file
  // already exists, it is *not* overwritten, but unlinked, and a new inode will be used.
  static File* CreateEmptyFile(const char* name);

  // Create an empty file with write access. This is a *new* file, that is, if the file
  // already exists, it is *not* overwritten, but unlinked, and a new inode will be used.
  static File* CreateEmptyFileWriteOnly(const char* name);

  // Open a file with the specified open(2) flags.
  static File* OpenFileWithFlags(const char* name, int flags, bool auto_flush = true);

  // Check if a file exists.
  static bool FileExists(const char* name, bool check_file_type = true);

  // Check if a directory exists.
  static bool DirectoryExists(const char* name);

  // Get the size of a file (or -1 if it does not exist).
  static int64_t GetFileSizeBytes(const char* name);

  // Open an existing file or an entry in a zip file with read only access.
  // `name_and_zip_entry` should be either a path to an existing file, or a path to a zip file and
  // the name of the zip entry, separated by `zip_separator`.
  // `alignment` is the expected alignment of the specified zip entry, in bytes. Only applicable if
  // `name_and_zip_entry` points to a zip entry.
  // Returns `file` being the file at the specified path and the range being the entire range of the
  // file, if `name_and_zip_entry` points to a file. Returns `file` being the zip file and the range
  // being the range of the zip entry, if `name_and_zip_entry` points to a zip entry. Returns `file`
  // being nullptr on failure.
  static FileWithRange OpenFileDirectlyOrFromZip(const std::string& name_and_zip_entry,
                                                 const char* zip_separator,
                                                 size_t alignment,
                                                 std::string* error_msg);
};

}  // namespace art

#endif  // ART_LIBARTBASE_BASE_OS_H_
