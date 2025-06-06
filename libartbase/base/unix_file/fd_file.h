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

#ifndef ART_LIBARTBASE_BASE_UNIX_FILE_FD_FILE_H_
#define ART_LIBARTBASE_BASE_UNIX_FILE_FD_FILE_H_

#include <fcntl.h>

#include <string>

#include "base/macros.h"
#include "random_access_file.h"

namespace unix_file {

// If true, check whether Flush and Close are called before destruction.
static constexpr bool kCheckSafeUsage = true;

// Used to work around kernel bugs.
bool AllowSparseFiles();

// A RandomAccessFile implementation backed by a file descriptor.
//
// Not thread safe.
class FdFile : public RandomAccessFile {
 public:
  static constexpr int kInvalidFd = -1;

  FdFile() = default;
  // Creates an FdFile using the given file descriptor.
  // Takes ownership of the file descriptor.
  FdFile(int fd, bool check_usage);
  FdFile(int fd, const std::string& path, bool check_usage);
  FdFile(int fd, const std::string& path, bool check_usage, bool read_only_mode);

  FdFile(const std::string& path, int flags, bool check_usage)
      : FdFile(path, flags, 0640, check_usage) {}
  FdFile(const std::string& path, int flags, mode_t mode, bool check_usage);

  // Move constructor.
  FdFile(FdFile&& other) noexcept;

  // Move assignment operator.
  FdFile& operator=(FdFile&& other) noexcept;

  // Release the file descriptor. This will make further accesses to this FdFile invalid. Disables
  // all further state checking.
  int Release();

  void Reset(int fd, bool check_usage);

  // Destroys an FdFile, closing the file descriptor if Close hasn't already
  // been called. (If you care about the return value of Close, call it
  // yourself; this is meant to handle failure cases and read-only accesses.
  // Note though that calling Close and checking its return value is still no
  // guarantee that data actually made it to stable storage.)
  virtual ~FdFile();

  // RandomAccessFile API.
  int Close() override WARN_UNUSED;
  int64_t Read(char* buf, int64_t byte_count, int64_t offset) const override WARN_UNUSED;
  int SetLength(int64_t new_length) override WARN_UNUSED;
  int64_t GetLength() const override;
  int64_t Write(const char* buf, int64_t byte_count, int64_t offset) override WARN_UNUSED;

  int Flush() override WARN_UNUSED { return Flush(/*flush_metadata=*/false); }
  int Flush(bool flush_metadata) WARN_UNUSED;

  // Short for SetLength(0); Flush(); Close();
  // If the file was opened with a path name and unlink = true, also calls Unlink() on the path.
  // Note that it is the the caller's responsibility to avoid races.
  bool Erase(bool unlink = false);

  // Call unlink(), though only if FilePathMatchesFd() returns true.
  bool Unlink();

  // Try to Flush(), then try to Close(); If either fails, call Erase().
  int FlushCloseOrErase() WARN_UNUSED;

  // Try to Flush and Close(). Attempts both, but returns the first error.
  int FlushClose() WARN_UNUSED;

  // Bonus API.
  int Fd() const;
  bool ReadOnlyMode() const;
  bool CheckUsage() const;

  // Check whether the underlying file descriptor refers to an open file.
  bool IsOpened() const;

  // Check whether the numeric value of the underlying file descriptor is valid (Fd() != -1).
  bool IsValid() const { return fd_ != kInvalidFd; }

  const std::string& GetPath() const {
    return file_path_;
  }
  bool ReadFully(void* buffer, size_t byte_count) WARN_UNUSED;
  bool PreadFully(void* buffer, size_t byte_count, size_t offset) WARN_UNUSED;
  bool WriteFully(const void* buffer, size_t byte_count) WARN_UNUSED;
  bool PwriteFully(const void* buffer, size_t byte_count, size_t offset) WARN_UNUSED;

  // Change the file path, though only if FilePathMatchesFd() returns true.
  //
  // If a file at new_path already exists, it will be replaced.
  // On Linux, the rename syscall will fail unless the source and destination are on the same
  // mounted filesystem.
  // This function is not expected to modify the file data itself, instead it modifies the inodes of
  // the source and destination directories, and therefore the function flushes those file
  // descriptors following the rename.
  bool Rename(const std::string& new_path);
  // Copy data from another file.
  // On Linux, we only support copies that will append regions to the file, and we require the file
  // offset of the output file descriptor to be aligned with the filesystem blocksize (see comments
  // in implementation).
  bool Copy(FdFile* input_file, int64_t offset, int64_t size);
  // Clears the file content and resets the file offset to 0.
  // Returns true upon success, false otherwise.
  bool ClearContent();
  // Resets the file offset to the beginning of the file.
  bool ResetOffset();

  // This enum is public so that we can define the << operator over it.
  enum class GuardState {
    kBase,           // Base, file has not been flushed or closed.
    kFlushed,        // File has been flushed, but not closed.
    kClosed,         // File has been flushed and closed.
    kNoCheck         // Do not check for the current file instance.
  };

  // WARNING: Only use this when you know what you're doing!
  void MarkUnchecked();

  // Compare against another file. Returns 0 if the files are equivalent, otherwise returns -1 or 1
  // depending on if the lengths are different. If the lengths are the same, the function returns
  // the difference of the first byte that differs.
  int Compare(FdFile* other);

  // Check that `fd` has a valid value (!= kInvalidFd) and refers to an open file.
  // On Windows, this call only checks that the value of `fd` is valid .
  static bool IsOpenFd(int fd);

 protected:
  // If the guard state indicates checking (!=kNoCheck), go to the target state `target`. Print the
  // given warning if the current state is or exceeds warn_threshold.
  void moveTo(GuardState target, GuardState warn_threshold, const char* warning);

  // If the guard state indicates checking (<kNoCheck), and is below the target state `target`, go
  // to `target`. If the current state is higher (excluding kNoCheck) than the target state, print
  // the warning.
  void moveUp(GuardState target, const char* warning);

  // Forcefully sets the state to the given one. This can overwrite kNoCheck.
  void resetGuard(GuardState new_state) {
    if (kCheckSafeUsage) {
      guard_state_ = new_state;
    }
  }

  GuardState guard_state_ = GuardState::kClosed;

  // Opens file `file_path` using `flags` and `mode`.
  bool Open(const std::string& file_path, int flags);
  bool Open(const std::string& file_path, int flags, mode_t mode);

 private:
  template <bool kUseOffset>
  bool WriteFullyGeneric(const void* buffer, size_t byte_count, size_t offset);

  // The file path we hold for the file descriptor may be invalid, or may not even exist (e.g. if
  // the FdFile wasn't initialised with a path). This helper function checks if calling open() on
  // the file path (if it is set) returns the expected up-to-date file descriptor. This is still
  // racy, though, and it is up to the caller to ensure correctness in a multi-process setup.
  bool FilePathMatchesFd();

#ifdef __linux__
  // Sparse copy of 'size' bytes from an input file, starting at 'off'. Both this file's offset and
  // the input file's offset will be incremented by 'size' bytes.
  //
  // Note: in order to preserve the same sparsity, the input and output files must be on mounted
  // filesystems that use the same blocksize, and the offsets used for the copy must be aligned to
  // it. Otherwise, the copied region's sparsity within the output file may differ from its original
  // sparsity in the input file.
  bool UserspaceSparseCopy(const FdFile* input_file, off_t off, size_t size, size_t fs_blocksize);

  // Write 'size' bytes from 'data' to the file if any are non-zero. Otherwise, just update the file
  // offset and skip the write. For efficiency, the function expects a vector of zeroed uint8_t
  // values to check the data array against. This vector 'zeroes' must have length greater than or
  // equal to 'size'.
  //
  // As filesystems which support sparse files only allocate physical space to blocks that have been
  // written, any whole filesystem blocks in the output file which are skipped in this way will save
  // storage space. Subsequent reads of bytes in non-allocated blocks will simply return zeros
  // without accessing the underlying storage.
  bool SparseWrite(const uint8_t* data,
                   size_t size,
                   const std::vector<uint8_t>& zeroes);
#endif

  void Destroy();  // For ~FdFile and operator=(&&).

  int fd_ = kInvalidFd;
  std::string file_path_;
  bool read_only_mode_ = false;

  DISALLOW_COPY_AND_ASSIGN(FdFile);
};

std::ostream& operator<<(std::ostream& os, FdFile::GuardState kind);

}  // namespace unix_file

#endif  // ART_LIBARTBASE_BASE_UNIX_FILE_FD_FILE_H_
