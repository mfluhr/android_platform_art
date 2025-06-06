/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "artd.h"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "aidl/com/android/server/art/ArtConstants.h"
#include "aidl/com/android/server/art/BnArtd.h"
#include "aidl/com/android/server/art/OutputArtifacts.h"
#include "android-base/collections.h"
#include "android-base/errors.h"
#include "android-base/file.h"
#include "android-base/logging.h"
#include "android-base/parseint.h"
#include "android-base/result-gmock.h"
#include "android-base/result.h"
#include "android-base/scopeguard.h"
#include "android-base/strings.h"
#include "android/binder_auto_utils.h"
#include "android/binder_status.h"
#include "base/array_ref.h"
#include "base/common_art_test.h"
#include "base/macros.h"
#include "base/pidfd.h"
#include "base/time_utils.h"
#include "exec_utils.h"
#include "file_utils.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "oat/oat_file.h"
#include "path_utils.h"
#include "profile/profile_compilation_info.cc"
#include "profman/profman_result.h"
#include "testing.h"
#include "tools/binder_utils.h"
#include "tools/system_properties.h"
#include "tools/testing.h"
#include "vdex_file.h"
#include "ziparchive/zip_writer.h"

extern char** environ;

namespace art {
namespace artd {
namespace {

using ::aidl::com::android::server::art::ArtConstants;
using ::aidl::com::android::server::art::ArtdDexoptResult;
using ::aidl::com::android::server::art::ArtifactsPath;
using ::aidl::com::android::server::art::CopyAndRewriteProfileResult;
using ::aidl::com::android::server::art::DexMetadataPath;
using ::aidl::com::android::server::art::DexoptOptions;
using ::aidl::com::android::server::art::FileVisibility;
using ::aidl::com::android::server::art::FsPermission;
using ::aidl::com::android::server::art::IArtdCancellationSignal;
using ::aidl::com::android::server::art::IArtdNotification;
using ::aidl::com::android::server::art::OutputArtifacts;
using ::aidl::com::android::server::art::OutputProfile;
using ::aidl::com::android::server::art::PriorityClass;
using ::aidl::com::android::server::art::ProfilePath;
using ::aidl::com::android::server::art::RuntimeArtifactsPath;
using ::aidl::com::android::server::art::SecureDexMetadataWithCompanionPaths;
using ::aidl::com::android::server::art::VdexPath;
using ::android::base::Append;
using ::android::base::Dirname;
using ::android::base::Error;
using ::android::base::make_scope_guard;
using ::android::base::ParseInt;
using ::android::base::ReadFdToString;
using ::android::base::ReadFileToString;
using ::android::base::Result;
using ::android::base::ScopeGuard;
using ::android::base::Split;
using ::android::base::WriteStringToFd;
using ::android::base::WriteStringToFile;
using ::android::base::testing::HasValue;
using ::art::tools::GetBin;
using ::art::tools::ScopedExec;
using ::testing::_;
using ::testing::AllOf;
using ::testing::AnyNumber;
using ::testing::AnyOf;
using ::testing::Contains;
using ::testing::ContainsRegex;
using ::testing::DoAll;
using ::testing::ElementsAre;
using ::testing::Field;
using ::testing::HasSubstr;
using ::testing::InSequence;
using ::testing::IsEmpty;
using ::testing::Matcher;
using ::testing::MockFunction;
using ::testing::NiceMock;
using ::testing::Not;
using ::testing::Property;
using ::testing::ResultOf;
using ::testing::Return;
using ::testing::SetArgPointee;
using ::testing::StartsWith;
using ::testing::StrEq;
using ::testing::UnorderedElementsAreArray;
using ::testing::WithArg;

using PermissionSettings = OutputArtifacts::PermissionSettings;
using PrimaryCurProfilePath = ProfilePath::PrimaryCurProfilePath;
using PrimaryRefProfilePath = ProfilePath::PrimaryRefProfilePath;
using TmpProfilePath = ProfilePath::TmpProfilePath;
using WritableProfilePath = ProfilePath::WritableProfilePath;

using std::literals::operator""s;  // NOLINT

// User build is missing the SELinux permission for the test process (run as `shell`) to reopen
// the memfd that it creates itself
// (https://cs.android.com/android/platform/superproject/main/+/main:system/sepolicy/private/shell.te;l=221;drc=3335a04676d400bda57d42d4af0ef4b1d311de21).
#define TEST_DISABLED_FOR_SHELL_WITHOUT_MEMFD_ACCESS() TEST_DISABLED_FOR_USER_BUILD()

ScopeGuard<std::function<void()>> ScopedSetLogger(android::base::LogFunction&& logger) {
  android::base::LogFunction old_logger = android::base::SetLogger(std::move(logger));
  return make_scope_guard([old_logger = std::move(old_logger)]() mutable {
    android::base::SetLogger(std::move(old_logger));
  });
}

void CheckContent(const std::string& path, const Matcher<std::string>& expected_content_matcher) {
  std::string actual_content;
  ASSERT_TRUE(ReadFileToString(path, &actual_content));
  EXPECT_THAT(actual_content, expected_content_matcher);
}

void CheckOtherReadable(const std::string& path, bool expected_value) {
  EXPECT_EQ((std::filesystem::status(path).permissions() & std::filesystem::perms::others_read) !=
                std::filesystem::perms::none,
            expected_value);
}

Result<std::vector<std::string>> GetFlagValues(ArrayRef<const std::string> args,
                                               std::string_view flag) {
  std::vector<std::string> values;
  for (const std::string& arg : args) {
    std::string_view value(arg);
    if (android::base::ConsumePrefix(&value, flag)) {
      values.emplace_back(value);
    }
  }
  if (values.empty()) {
    return Errorf("Flag '{}' not found", flag);
  }
  return values;
}

Result<std::string> GetFlagValue(ArrayRef<const std::string> args, std::string_view flag) {
  std::vector<std::string> flag_values = OR_RETURN(GetFlagValues(args, flag));
  if (flag_values.size() > 1) {
    return Errorf("Duplicate flag '{}'", flag);
  }
  return flag_values[0];
}

void WriteToFdFlagImpl(const std::vector<std::string>& args,
                       std::string_view flag,
                       std::string_view content,
                       bool assume_empty) {
  std::string value = OR_FAIL(GetFlagValue(ArrayRef<const std::string>(args), flag));
  ASSERT_NE(value, "");
  int fd;
  ASSERT_TRUE(ParseInt(value, &fd));
  if (assume_empty) {
    ASSERT_EQ(lseek(fd, /*offset=*/0, SEEK_CUR), 0);
  } else {
    ASSERT_EQ(ftruncate(fd, /*length=*/0), 0);
    ASSERT_EQ(lseek(fd, /*offset=*/0, SEEK_SET), 0);
  }
  ASSERT_TRUE(WriteStringToFd(content, fd));
}

// Writes `content` to the FD specified by the `flag`.
ACTION_P(WriteToFdFlag, flag, content) {
  WriteToFdFlagImpl(arg0, flag, content, /*assume_empty=*/true);
}

// Clears any existing content and writes `content` to the FD specified by the `flag`.
ACTION_P(ClearAndWriteToFdFlag, flag, content) {
  WriteToFdFlagImpl(arg0, flag, content, /*assume_empty=*/false);
}

// Matches a flag that starts with `flag` and whose value matches `matcher`.
MATCHER_P2(Flag, flag, matcher, "") {
  std::string_view value(arg);
  if (!android::base::ConsumePrefix(&value, flag)) {
    return false;
  }
  return ExplainMatchResult(matcher, std::string(value), result_listener);
}

// Matches a flag that starts with `flag` and whose value is a colon-separated list that matches
// `matcher`. The matcher acts on an `std::vector<std::string>` of the split list argument.
MATCHER_P2(ListFlag, flag, matcher, "") {
  return ExplainMatchResult(
      Flag(flag, ResultOf(std::bind(Split, std::placeholders::_1, ":"), matcher)),
      arg,
      result_listener);
}

// Matches an FD of a file whose path matches `matcher`.
MATCHER_P(FdOf, matcher, "") {
  std::string proc_path = ART_FORMAT("/proc/self/fd/{}", arg);
  char path[PATH_MAX];
  ssize_t len = readlink(proc_path.c_str(), path, sizeof(path));
  if (len < 0) {
    return false;
  }
  return ExplainMatchResult(matcher, std::string(path, static_cast<size_t>(len)), result_listener);
}

// Matches an FD of a file whose content matches `matcher`.
MATCHER_P(FdHasContent, matcher, "") {
  int fd;
  if (!ParseInt(arg, &fd)) {
    return false;
  }
  std::string actual_content;
  if (!ReadFdToString(fd, &actual_content)) {
    return false;
  }
  return ExplainMatchResult(matcher, actual_content, result_listener);
}

template <typename T, typename U>
Result<std::pair<ArrayRef<const T>, ArrayRef<const T>>> SplitBy(const std::vector<T>& list,
                                                                const U& separator) {
  auto it = std::find(list.begin(), list.end(), separator);
  if (it == list.end()) {
    return Errorf("'{}' not found", separator);
  }
  size_t pos = it - list.begin();
  return std::make_pair(ArrayRef<const T>(list).SubArray(0, pos),
                        ArrayRef<const T>(list).SubArray(pos + 1));
}

// Matches a container that, when split by `separator`, the first part matches `head_matcher`, and
// the second part matches `tail_matcher`.
MATCHER_P3(WhenSplitBy, separator, head_matcher, tail_matcher, "") {
  auto [head, tail] = OR_MISMATCH(SplitBy(arg, separator));
  return ExplainMatchResult(head_matcher, head, result_listener) &&
         ExplainMatchResult(tail_matcher, tail, result_listener);
}

MATCHER_P(HasKeepFdsForImpl, fd_flags, "") {
  auto [head, tail] = OR_MISMATCH(SplitBy(arg, "--"));
  std::string keep_fds_value = OR_MISMATCH(GetFlagValue(head, "--keep-fds="));
  std::vector<std::string> keep_fds = Split(keep_fds_value, ":");
  std::vector<std::string> fd_flag_values;
  for (std::string_view fd_flag : fd_flags) {
    for (const std::string& fd_flag_value : OR_MISMATCH(GetFlagValues(tail, fd_flag))) {
      for (std::string& fd : Split(fd_flag_value, ":")) {
        fd_flag_values.push_back(std::move(fd));
      }
    }
  }
  return ExplainMatchResult(UnorderedElementsAreArray(fd_flag_values), keep_fds, result_listener);
}

// Matches an argument list that has the "--keep-fds=" flag before "--", whose value is a
// semicolon-separated list that contains exactly the values of the given flags after "--".
//
// E.g., if the flags after "--" are "--foo=1", "--bar=2:3", "--baz=4", "--baz=5", and the matcher
// is `HasKeepFdsFor("--foo=", "--bar=", "--baz=")`, then it requires the "--keep-fds=" flag before
// "--" to contain exactly 1, 2, 3, 4, and 5.
template <typename... Args>
auto HasKeepFdsFor(Args&&... args) {
  std::vector<std::string_view> fd_flags;
  Append(fd_flags, std::forward<Args>(args)...);
  return HasKeepFdsForImpl(fd_flags);
}

class MockSystemProperties : public tools::SystemProperties {
 public:
  MOCK_METHOD(std::string, GetProperty, (const std::string& key), (const, override));
};

class MockExecUtils : public ExecUtils {
 public:
  // A workaround to avoid MOCK_METHOD on a method with an `std::string*` parameter, which will lead
  // to a conflict between gmock and android-base/logging.h (b/132668253).
  ExecResult ExecAndReturnResult(const std::vector<std::string>& arg_vector,
                                 int,
                                 const ExecCallbacks& callbacks,
                                 bool,
                                 ProcessStat* stat,
                                 std::string*) const override {
    Result<int> code = DoExecAndReturnCode(arg_vector, callbacks, stat);
    if (code.ok()) {
      return {.status = ExecResult::kExited, .exit_code = code.value()};
    } else {
      return {.status = ExecResult::kSignaled, .signal = SIGKILL};
    }
  }

  MOCK_METHOD(Result<int>,
              DoExecAndReturnCode,
              (const std::vector<std::string>& arg_vector,
               const ExecCallbacks& callbacks,
               ProcessStat* stat),
              (const));
};

class ArtdTest : public CommonArtTest {
 protected:
  void SetUp() override {
    CommonArtTest::SetUp();
    auto mock_props = std::make_unique<MockSystemProperties>();
    mock_props_ = mock_props.get();
    EXPECT_CALL(*mock_props_, GetProperty).Times(AnyNumber()).WillRepeatedly(Return(""));
    auto mock_exec_utils = std::make_unique<MockExecUtils>();
    mock_exec_utils_ = mock_exec_utils.get();
    artd_ = ndk::SharedRefBase::make<Artd>(Options(),
                                           std::move(mock_props),
                                           std::move(mock_exec_utils),
                                           mock_kill_.AsStdFunction(),
                                           mock_fstat_.AsStdFunction(),
                                           mock_poll_.AsStdFunction());
    scratch_dir_ = std::make_unique<ScratchDir>();
    scratch_path_ = scratch_dir_->GetPath();
    // Remove the trailing '/';
    scratch_path_.resize(scratch_path_.length() - 1);

    TestOnlySetListRootDir(scratch_path_);

    ON_CALL(mock_fstat_, Call).WillByDefault(fstat);

    // Use an arbitrary existing directory as ART root.
    art_root_ = scratch_path_ + "/com.android.art";
    std::filesystem::create_directories(art_root_);
    setenv("ANDROID_ART_ROOT", art_root_.c_str(), /*overwrite=*/1);

    // Use an arbitrary existing directory as Android data.
    android_data_ = scratch_path_ + "/data";
    std::filesystem::create_directories(android_data_);
    setenv("ANDROID_DATA", android_data_.c_str(), /*overwrite=*/1);

    // Use an arbitrary existing directory as Android expand.
    android_expand_ = scratch_path_ + "/mnt/expand";
    std::filesystem::create_directories(android_expand_);
    setenv("ANDROID_EXPAND", android_expand_.c_str(), /*overwrite=*/1);

    dex_file_ = scratch_path_ + "/a/b.apk";
    isa_ = "arm64";
    artifacts_path_ = ArtifactsPath{
        .dexPath = dex_file_,
        .isa = isa_,
        .isInDalvikCache = false,
    };
    struct stat st;
    ASSERT_EQ(stat(scratch_path_.c_str(), &st), 0);
    permission_settings_ = {
        .dirFsPermission =
            FsPermission{
                .uid = static_cast<int32_t>(st.st_uid),
                .gid = static_cast<int32_t>(st.st_gid),
                .isOtherReadable = true,
                .isOtherExecutable = true,
            },
        .fileFsPermission =
            FsPermission{
                .uid = static_cast<int32_t>(st.st_uid),
                .gid = static_cast<int32_t>(st.st_gid),
                .isOtherReadable = true,
            },
    };
    output_artifacts_ = OutputArtifacts{
        .artifactsPath = artifacts_path_,
        .permissionSettings = permission_settings_,
    };
    clc_1_ = GetTestDexFileName("Main");
    clc_2_ = GetTestDexFileName("Nested");
    class_loader_context_ = ART_FORMAT("PCL[{}:{}]", clc_1_, clc_2_);
    compiler_filter_ = "speed";
    tmp_profile_path_ =
        TmpProfilePath{.finalPath = PrimaryRefProfilePath{.packageName = "com.android.foo",
                                                          .profileName = "primary",
                                                          .isPreReboot = false},
                       .id = "12345"};
    profile_path_ = tmp_profile_path_;
    vdex_path_ = artifacts_path_;
    dm_path_ = DexMetadataPath{.dexPath = dex_file_};
    std::filesystem::create_directories(
        std::filesystem::path(OR_FATAL(BuildFinalProfilePath(tmp_profile_path_))).parent_path());

    sdm_sdc_paths_ = {
        .dexPath = dex_file_,
        .isa = isa_,
        .isInDalvikCache = false,
    };
  }

  void TearDown() override {
    scratch_dir_.reset();
    CommonArtTest::TearDown();
  }

  void RunDexopt(binder_exception_t expected_status = EX_NONE,
                 Matcher<ArtdDexoptResult> aidl_return_matcher = Field(&ArtdDexoptResult::cancelled,
                                                                       false),
                 std::shared_ptr<IArtdCancellationSignal> cancellation_signal = nullptr) {
    RunDexopt(Property(&ndk::ScopedAStatus::getExceptionCode, expected_status),
              std::move(aidl_return_matcher),
              std::move(cancellation_signal));
  }

  void RunDexopt(Matcher<ndk::ScopedAStatus> status_matcher,
                 Matcher<ArtdDexoptResult> aidl_return_matcher = Field(&ArtdDexoptResult::cancelled,
                                                                       false),
                 std::shared_ptr<IArtdCancellationSignal> cancellation_signal = nullptr) {
    InitFilesBeforeDexopt();
    if (cancellation_signal == nullptr) {
      ASSERT_TRUE(artd_->createCancellationSignal(&cancellation_signal).isOk());
    }
    ArtdDexoptResult aidl_return;
    ndk::ScopedAStatus status = artd_->dexopt(output_artifacts_,
                                              dex_file_,
                                              isa_,
                                              class_loader_context_,
                                              compiler_filter_,
                                              profile_path_,
                                              vdex_path_,
                                              dm_path_,
                                              priority_class_,
                                              dexopt_options_,
                                              cancellation_signal,
                                              &aidl_return);
    ASSERT_THAT(status, std::move(status_matcher)) << status.getMessage();
    if (status.isOk()) {
      ASSERT_THAT(aidl_return, std::move(aidl_return_matcher));
    }
  }

  template <bool kExpectOk>
  using RunCopyAndRewriteProfileResult = Result<
      std::pair<std::conditional_t<kExpectOk, CopyAndRewriteProfileResult, ndk::ScopedAStatus>,
                OutputProfile>>;

  // Runs `copyAndRewriteProfile` with `profile_path_` and `dex_file_`.
  template <bool kExpectOk = true>
  RunCopyAndRewriteProfileResult<kExpectOk> RunCopyAndRewriteProfile() {
    OutputProfile dst{.profilePath = tmp_profile_path_,
                      .fsPermission = FsPermission{.uid = -1, .gid = -1}};
    dst.profilePath.id = "";
    dst.profilePath.tmpPath = "";

    CopyAndRewriteProfileResult result;
    ndk::ScopedAStatus status =
        artd_->copyAndRewriteProfile(profile_path_.value(), &dst, dex_file_, &result);
    if constexpr (kExpectOk) {
      if (!status.isOk()) {
        return Error() << status.getMessage();
      }
      return std::make_pair(std::move(result), std::move(dst));
    } else {
      return std::make_pair(std::move(status), std::move(dst));
    }
  }

  // Runs `copyAndRewriteEmbeddedProfile` with `dex_file_`.
  template <bool kExpectOk = true>
  RunCopyAndRewriteProfileResult<kExpectOk> RunCopyAndRewriteEmbeddedProfile() {
    OutputProfile dst{.profilePath = tmp_profile_path_,
                      .fsPermission = FsPermission{.uid = -1, .gid = -1}};
    dst.profilePath.id = "";
    dst.profilePath.tmpPath = "";

    CopyAndRewriteProfileResult result;
    ndk::ScopedAStatus status = artd_->copyAndRewriteEmbeddedProfile(&dst, dex_file_, &result);
    if constexpr (kExpectOk) {
      if (!status.isOk()) {
        return Error() << status.getMessage();
      }
      return std::make_pair(std::move(result), std::move(dst));
    } else {
      return std::make_pair(std::move(status), std::move(dst));
    }
  }

  void CreateFile(const std::string& filename, const std::string& content = "") {
    std::filesystem::path path(filename);
    std::filesystem::create_directories(path.parent_path());
    ASSERT_TRUE(WriteStringToFile(content, filename));
  }

  void CreateZipWithSingleEntry(const std::string& filename,
                                const std::string& entry_name,
                                const std::string& content = "") {
    std::filesystem::path path(filename);
    std::filesystem::create_directories(path.parent_path());
    std::unique_ptr<File> file(OS::CreateEmptyFileWriteOnly(filename.c_str()));
    ASSERT_NE(file, nullptr) << strerror(errno);
    file->MarkUnchecked();  // `writer.Finish()` flushes the file and the destructor closes it.
    ZipWriter writer(fdopen(file->Fd(), "wb"));
    ASSERT_EQ(writer.StartEntry(entry_name, /*flags=*/0), 0);
    ASSERT_EQ(writer.WriteBytes(content.c_str(), content.size()), 0);
    ASSERT_EQ(writer.FinishEntry(), 0);
    ASSERT_EQ(writer.Finish(), 0);
  }

  std::shared_ptr<Artd> artd_;
  std::unique_ptr<ScratchDir> scratch_dir_;
  std::string scratch_path_;
  std::string art_root_;
  std::string android_data_;
  std::string android_expand_;
  MockFunction<android::base::LogFunction> mock_logger_;
  ScopedUnsetEnvironmentVariable art_root_env_ = ScopedUnsetEnvironmentVariable("ANDROID_ART_ROOT");
  ScopedUnsetEnvironmentVariable android_data_env_ = ScopedUnsetEnvironmentVariable("ANDROID_DATA");
  ScopedUnsetEnvironmentVariable android_expand_env_ =
      ScopedUnsetEnvironmentVariable("ANDROID_EXPAND");
  MockSystemProperties* mock_props_;
  MockExecUtils* mock_exec_utils_;
  MockFunction<KillFn> mock_kill_;
  MockFunction<FstatFn> mock_fstat_;
  MockFunction<PollFn> mock_poll_;

  std::string dex_file_;
  std::string isa_;
  ArtifactsPath artifacts_path_;
  PermissionSettings permission_settings_;
  OutputArtifacts output_artifacts_;
  std::string clc_1_;
  std::string clc_2_;
  std::optional<std::string> class_loader_context_;
  std::string compiler_filter_;
  std::optional<VdexPath> vdex_path_;
  std::optional<DexMetadataPath> dm_path_;
  PriorityClass priority_class_ = PriorityClass::BACKGROUND;
  DexoptOptions dexopt_options_;
  std::optional<ProfilePath> profile_path_;
  TmpProfilePath tmp_profile_path_;
  bool dex_file_other_readable_ = true;
  bool profile_other_readable_ = true;

  SecureDexMetadataWithCompanionPaths sdm_sdc_paths_;

 private:
  void InitFilesBeforeDexopt() {
    // Required files.
    CreateFile(dex_file_);
    std::filesystem::permissions(dex_file_,
                                 std::filesystem::perms::others_read,
                                 dex_file_other_readable_ ? std::filesystem::perm_options::add :
                                                            std::filesystem::perm_options::remove);

    // Optional files.
    if (vdex_path_.has_value()) {
      CreateFile(OR_FATAL(BuildVdexPath(vdex_path_.value())), "old_vdex");
    }
    if (dm_path_.has_value()) {
      CreateFile(OR_FATAL(BuildDexMetadataPath(dm_path_.value())));
    }
    if (profile_path_.has_value()) {
      std::string path = OR_FATAL(BuildProfileOrDmPath(profile_path_.value()));
      CreateFile(path);
      std::filesystem::permissions(path,
                                   std::filesystem::perms::others_read,
                                   profile_other_readable_ ? std::filesystem::perm_options::add :
                                                             std::filesystem::perm_options::remove);
    }

    // Files to be replaced.
    RawArtifactsPath artifacts_path = OR_FATAL(BuildArtifactsPath(artifacts_path_));
    CreateFile(artifacts_path.oat_path, "old_oat");
    CreateFile(artifacts_path.vdex_path, "old_vdex");
    CreateFile(artifacts_path.art_path, "old_art");
  }
};

TEST_F(ArtdTest, ConstantsAreInSync) {
  EXPECT_STREQ(ArtConstants::REASON_VDEX, kReasonVdex);
  EXPECT_STREQ(ArtConstants::DEX_METADATA_FILE_EXT, kDmExtension);
  EXPECT_STREQ(ArtConstants::SECURE_DEX_METADATA_FILE_EXT, kSdmExtension);
  EXPECT_STREQ(ArtConstants::DEX_METADATA_PROFILE_ENTRY,
               ProfileCompilationInfo::kDexMetadataProfileEntry);
  EXPECT_STREQ(ArtConstants::DEX_METADATA_VDEX_ENTRY, VdexFile::kVdexNameInDmFile);
}

TEST_F(ArtdTest, isAlive) {
  bool result = false;
  artd_->isAlive(&result);
  EXPECT_TRUE(result);
}

TEST_F(ArtdTest, deleteArtifacts) {
  std::string oat_dir = scratch_path_ + "/a/oat/arm64";
  std::filesystem::create_directories(oat_dir);
  ASSERT_TRUE(WriteStringToFile("abcd", oat_dir + "/b.odex"));  // 4 bytes.
  ASSERT_TRUE(WriteStringToFile("ab", oat_dir + "/b.vdex"));    // 2 bytes.
  ASSERT_TRUE(WriteStringToFile("a", oat_dir + "/b.art"));      // 1 byte.

  int64_t result = -1;
  EXPECT_TRUE(artd_->deleteArtifacts(artifacts_path_, &result).isOk());
  EXPECT_EQ(result, 4 + 2 + 1);

  EXPECT_FALSE(std::filesystem::exists(oat_dir + "/b.odex"));
  EXPECT_FALSE(std::filesystem::exists(oat_dir + "/b.vdex"));
  EXPECT_FALSE(std::filesystem::exists(oat_dir + "/b.art"));
}

TEST_F(ArtdTest, deleteArtifactsMissingFile) {
  // Missing VDEX file.
  std::string oat_dir = android_data_ + "/dalvik-cache/arm64";
  std::filesystem::create_directories(oat_dir);
  ASSERT_TRUE(WriteStringToFile("abcd", oat_dir + "/a@b.apk@classes.dex"));  // 4 bytes.
  ASSERT_TRUE(WriteStringToFile("a", oat_dir + "/a@b.apk@classes.art"));     // 1 byte.

  auto scoped_set_logger = ScopedSetLogger(mock_logger_.AsStdFunction());
  EXPECT_CALL(mock_logger_, Call(_, _, _, _, _, HasSubstr("Failed to get the file size"))).Times(0);

  int64_t result = -1;
  EXPECT_TRUE(artd_
                  ->deleteArtifacts(
                      ArtifactsPath{
                          .dexPath = "/a/b.apk",
                          .isa = "arm64",
                          .isInDalvikCache = true,
                      },
                      &result)
                  .isOk());
  EXPECT_EQ(result, 4 + 1);

  EXPECT_FALSE(std::filesystem::exists(oat_dir + "/a@b.apk@classes.dex"));
  EXPECT_FALSE(std::filesystem::exists(oat_dir + "/a@b.apk@classes.art"));
}

TEST_F(ArtdTest, deleteArtifactsNoFile) {
  auto scoped_set_logger = ScopedSetLogger(mock_logger_.AsStdFunction());
  EXPECT_CALL(mock_logger_, Call(_, _, _, _, _, HasSubstr("Failed to get the file size"))).Times(0);

  int64_t result = -1;
  EXPECT_TRUE(artd_->deleteArtifacts(artifacts_path_, &result).isOk());
  EXPECT_EQ(result, 0);
}

TEST_F(ArtdTest, deleteArtifactsPermissionDenied) {
  std::string oat_dir = scratch_path_ + "/a/oat/arm64";
  std::filesystem::create_directories(oat_dir);
  ASSERT_TRUE(WriteStringToFile("abcd", oat_dir + "/b.odex"));  // 4 bytes.
  ASSERT_TRUE(WriteStringToFile("ab", oat_dir + "/b.vdex"));    // 2 bytes.
  ASSERT_TRUE(WriteStringToFile("a", oat_dir + "/b.art"));      // 1 byte.

  auto scoped_set_logger = ScopedSetLogger(mock_logger_.AsStdFunction());
  EXPECT_CALL(mock_logger_, Call(_, _, _, _, _, HasSubstr("Failed to get the file size"))).Times(3);

  auto scoped_inaccessible = ScopedInaccessible(oat_dir);
  auto scoped_unroot = ScopedUnroot();

  int64_t result = -1;
  EXPECT_TRUE(artd_->deleteArtifacts(artifacts_path_, &result).isOk());
  EXPECT_EQ(result, 0);
}

TEST_F(ArtdTest, deleteArtifactsFileIsDir) {
  // VDEX file is a directory.
  std::string oat_dir = scratch_path_ + "/a/oat/arm64";
  std::filesystem::create_directories(oat_dir);
  std::filesystem::create_directories(oat_dir + "/b.vdex");
  ASSERT_TRUE(WriteStringToFile("abcd", oat_dir + "/b.odex"));  // 4 bytes.
  ASSERT_TRUE(WriteStringToFile("a", oat_dir + "/b.art"));      // 1 byte.

  auto scoped_set_logger = ScopedSetLogger(mock_logger_.AsStdFunction());
  EXPECT_CALL(mock_logger_,
              Call(_, _, _, _, _, ContainsRegex(R"re(Failed to get the file size.*b\.vdex)re")))
      .Times(1);

  int64_t result = -1;
  EXPECT_TRUE(artd_->deleteArtifacts(artifacts_path_, &result).isOk());
  EXPECT_EQ(result, 4 + 1);

  // The directory is kept because getting the file size failed.
  EXPECT_FALSE(std::filesystem::exists(oat_dir + "/b.odex"));
  EXPECT_TRUE(std::filesystem::exists(oat_dir + "/b.vdex"));
  EXPECT_FALSE(std::filesystem::exists(oat_dir + "/b.art"));
}

TEST_F(ArtdTest, maybeCreateSdc) {
  // Unable to create OatFileAssistantContext on host to get APEX versions.
  TEST_DISABLED_FOR_HOST();

  std::string sdm_file = OR_FAIL(BuildSdmPath(sdm_sdc_paths_));
  std::string sdc_file = OR_FAIL(BuildSdcPath(sdm_sdc_paths_));
  CreateFile(sdm_file);

  ASSERT_STATUS_OK(artd_->maybeCreateSdc(
      {.sdcPath = sdm_sdc_paths_, .permissionSettings = permission_settings_}));

  CheckContent(sdc_file, StartsWith("sdm-timestamp-ns="));
}

TEST_F(ArtdTest, maybeCreateSdcAlreadyCreated) {
  // Unable to create OatFileAssistantContext on host to get APEX versions.
  TEST_DISABLED_FOR_HOST();

  std::string sdm_file = OR_FAIL(BuildSdmPath(sdm_sdc_paths_));
  std::string sdc_file = OR_FAIL(BuildSdcPath(sdm_sdc_paths_));
  CreateFile(sdm_file);

  ASSERT_STATUS_OK(artd_->maybeCreateSdc(
      {.sdcPath = sdm_sdc_paths_, .permissionSettings = permission_settings_}));

  struct stat sdc_st;
  ASSERT_EQ(stat(sdc_file.c_str(), &sdc_st), 0);

  ASSERT_STATUS_OK(artd_->maybeCreateSdc(
      {.sdcPath = sdm_sdc_paths_, .permissionSettings = permission_settings_}));

  struct stat new_sdc_st;
  ASSERT_EQ(stat(sdc_file.c_str(), &new_sdc_st), 0);

  EXPECT_EQ(TimeSpecToNs(sdc_st.st_mtim), TimeSpecToNs(new_sdc_st.st_mtim));
}

TEST_F(ArtdTest, maybeCreateSdcOutdatedTimestamp) {
  // Unable to create OatFileAssistantContext on host to get APEX versions.
  TEST_DISABLED_FOR_HOST();

  std::string sdm_file = OR_FAIL(BuildSdmPath(sdm_sdc_paths_));
  std::string sdc_file = OR_FAIL(BuildSdcPath(sdm_sdc_paths_));
  CreateFile(sdm_file);

  ASSERT_STATUS_OK(artd_->maybeCreateSdc(
      {.sdcPath = sdm_sdc_paths_, .permissionSettings = permission_settings_}));

  struct stat sdc_st;
  ASSERT_EQ(stat(sdc_file.c_str(), &sdc_st), 0);

  // Simulate that the SDM file is updated.
  CreateFile(sdm_file);

  ASSERT_STATUS_OK(artd_->maybeCreateSdc(
      {.sdcPath = sdm_sdc_paths_, .permissionSettings = permission_settings_}));

  struct stat new_sdc_st;
  ASSERT_EQ(stat(sdc_file.c_str(), &new_sdc_st), 0);

  // The SDC file should be updated.
  EXPECT_LT(TimeSpecToNs(sdc_st.st_mtim), TimeSpecToNs(new_sdc_st.st_mtim));
}

TEST_F(ArtdTest, maybeCreateSdcNoSdm) {
  std::string sdc_file = OR_FAIL(BuildSdcPath(sdm_sdc_paths_));

  ASSERT_STATUS_OK(artd_->maybeCreateSdc(
      {.sdcPath = sdm_sdc_paths_, .permissionSettings = permission_settings_}));

  EXPECT_FALSE(std::filesystem::exists(sdc_file));
}

TEST_F(ArtdTest, dexopt) {
  dexopt_options_.generateAppImage = true;

  EXPECT_CALL(
      *mock_exec_utils_,
      DoExecAndReturnCode(
          AllOf(WhenSplitBy(
                    "--",
                    AllOf(Contains(art_root_ + "/bin/art_exec"), Contains("--drop-capabilities")),
                    AllOf(Contains(art_root_ + "/bin/dex2oat32"),
                          Contains(Flag("--zip-fd=", FdOf(dex_file_))),
                          Contains(Flag("--zip-location=", dex_file_)),
                          Contains(Flag("--oat-location=", scratch_path_ + "/a/oat/arm64/b.odex")),
                          Contains(Flag("--instruction-set=", "arm64")),
                          Contains(Flag("--compiler-filter=", "speed")),
                          Contains(Flag(
                              "--profile-file-fd=",
                              FdOf(android_data_ +
                                   "/misc/profiles/ref/com.android.foo/primary.prof.12345.tmp"))),
                          Contains(Flag("--input-vdex-fd=",
                                        FdOf(scratch_path_ + "/a/oat/arm64/b.vdex"))),
                          Contains(Flag("--dm-fd=", FdOf(scratch_path_ + "/a/b.dm"))))),
                HasKeepFdsFor("--zip-fd=",
                              "--profile-file-fd=",
                              "--input-vdex-fd=",
                              "--dm-fd=",
                              "--oat-fd=",
                              "--output-vdex-fd=",
                              "--app-image-fd=",
                              "--class-loader-context-fds=",
                              "--swap-fd=")),
          _,
          _))
      .WillOnce(DoAll(WithArg<0>(WriteToFdFlag("--oat-fd=", "oat")),
                      WithArg<0>(WriteToFdFlag("--output-vdex-fd=", "vdex")),
                      WithArg<0>(WriteToFdFlag("--app-image-fd=", "art")),
                      SetArgPointee<2>(ProcessStat{.wall_time_ms = 100, .cpu_time_ms = 400}),
                      Return(0)));
  RunDexopt(
      EX_NONE,
      AllOf(Field(&ArtdDexoptResult::cancelled, false),
            Field(&ArtdDexoptResult::wallTimeMs, 100),
            Field(&ArtdDexoptResult::cpuTimeMs, 400),
            Field(&ArtdDexoptResult::sizeBytes, strlen("art") + strlen("oat") + strlen("vdex")),
            Field(&ArtdDexoptResult::sizeBeforeBytes,
                  strlen("old_art") + strlen("old_oat") + strlen("old_vdex"))));

  CheckContent(scratch_path_ + "/a/oat/arm64/b.odex", "oat");
  CheckContent(scratch_path_ + "/a/oat/arm64/b.vdex", "vdex");
  CheckContent(scratch_path_ + "/a/oat/arm64/b.art", "art");
  CheckOtherReadable(scratch_path_ + "/a/oat/arm64/b.odex", true);
  CheckOtherReadable(scratch_path_ + "/a/oat/arm64/b.vdex", true);
  CheckOtherReadable(scratch_path_ + "/a/oat/arm64/b.art", true);
}

TEST_F(ArtdTest, dexoptClassLoaderContext) {
  EXPECT_CALL(
      *mock_exec_utils_,
      DoExecAndReturnCode(
          WhenSplitBy("--",
                      _,
                      AllOf(Contains(ListFlag("--class-loader-context-fds=",
                                              ElementsAre(FdOf(clc_1_), FdOf(clc_2_)))),
                            Contains(Flag("--class-loader-context=", class_loader_context_)),
                            Contains(Flag("--classpath-dir=", scratch_path_ + "/a")))),
          _,
          _))
      .WillOnce(Return(0));
  RunDexopt();
}

TEST_F(ArtdTest, dexoptClassLoaderContextNull) {
  class_loader_context_ = std::nullopt;

  EXPECT_CALL(
      *mock_exec_utils_,
      DoExecAndReturnCode(WhenSplitBy("--",
                                      _,
                                      AllOf(Not(Contains(Flag("--class-loader-context-fds=", _))),
                                            Not(Contains(Flag("--class-loader-context=", _))),
                                            Not(Contains(Flag("--classpath-dir=", _))))),
                          _,
                          _))
      .WillOnce(Return(0));
  RunDexopt();
}

TEST_F(ArtdTest, dexoptNoOptionalInputFiles) {
  profile_path_ = std::nullopt;
  vdex_path_ = std::nullopt;
  dm_path_ = std::nullopt;

  EXPECT_CALL(*mock_exec_utils_,
              DoExecAndReturnCode(WhenSplitBy("--",
                                              _,
                                              AllOf(Not(Contains(Flag("--profile-file-fd=", _))),
                                                    Not(Contains(Flag("--input-vdex-fd=", _))),
                                                    Not(Contains(Flag("--dm-fd=", _))))),
                                  _,
                                  _))
      .WillOnce(Return(0));
  RunDexopt();
}

TEST_F(ArtdTest, dexoptPriorityClassBoot) {
  priority_class_ = PriorityClass::BOOT;
  EXPECT_CALL(*mock_exec_utils_,
              DoExecAndReturnCode(WhenSplitBy("--",
                                              AllOf(Not(Contains(Flag("--set-task-profile=", _))),
                                                    Not(Contains(Flag("--set-priority=", _)))),
                                              _),
                                  _,
                                  _))
      .WillOnce(Return(0));
  RunDexopt();
}

TEST_F(ArtdTest, dexoptPriorityClassInteractive) {
  priority_class_ = PriorityClass::INTERACTIVE;
  EXPECT_CALL(*mock_exec_utils_,
              DoExecAndReturnCode(
                  WhenSplitBy("--",
                              AllOf(Contains(Flag("--set-task-profile=", "Dex2OatBootComplete")),
                                    Contains(Flag("--set-priority=", "background"))),
                              _),
                  _,
                  _))
      .WillOnce(Return(0));
  RunDexopt();
}

TEST_F(ArtdTest, dexoptPriorityClassInteractiveFast) {
  priority_class_ = PriorityClass::INTERACTIVE_FAST;
  EXPECT_CALL(*mock_exec_utils_,
              DoExecAndReturnCode(
                  WhenSplitBy("--",
                              AllOf(Contains(Flag("--set-task-profile=", "Dex2OatBootComplete")),
                                    Contains(Flag("--set-priority=", "background"))),
                              _),
                  _,
                  _))
      .WillOnce(Return(0));
  RunDexopt();
}

TEST_F(ArtdTest, dexoptPriorityClassBackground) {
  priority_class_ = PriorityClass::BACKGROUND;
  EXPECT_CALL(*mock_exec_utils_,
              DoExecAndReturnCode(
                  WhenSplitBy("--",
                              AllOf(Contains(Flag("--set-task-profile=", "Dex2OatBackground")),
                                    Contains(Flag("--set-priority=", "background"))),
                              _),
                  _,
                  _))
      .WillOnce(Return(0));
  RunDexopt();
}

TEST_F(ArtdTest, dexoptDexoptOptions) {
  dexopt_options_ = DexoptOptions{
      .compilationReason = "install",
      .targetSdkVersion = 123,
      .debuggable = false,
      .generateAppImage = false,
      .hiddenApiPolicyEnabled = false,
      .comments = "my-comments",
  };

  EXPECT_CALL(
      *mock_exec_utils_,
      DoExecAndReturnCode(WhenSplitBy("--",
                                      _,
                                      AllOf(Contains(Flag("--compilation-reason=", "install")),
                                            Contains(Flag("-Xtarget-sdk-version:", "123")),
                                            Not(Contains("--debuggable")),
                                            Not(Contains(Flag("--app-image-fd=", _))),
                                            Not(Contains(Flag("-Xhidden-api-policy:", _))),
                                            Contains(Flag("--comments=", "my-comments")))),
                          _,
                          _))
      .WillOnce(Return(0));

  // `sizeBeforeBytes` should include the size of the old ART file even if no new ART file is
  // generated.
  RunDexopt(EX_NONE,
            Field(&ArtdDexoptResult::sizeBeforeBytes,
                  strlen("old_art") + strlen("old_oat") + strlen("old_vdex")));
}

TEST_F(ArtdTest, dexoptDexoptOptions2) {
  dexopt_options_ = DexoptOptions{
      .compilationReason = "bg-dexopt",
      .targetSdkVersion = 456,
      .debuggable = true,
      .generateAppImage = true,
      .hiddenApiPolicyEnabled = true,
  };

  EXPECT_CALL(
      *mock_exec_utils_,
      DoExecAndReturnCode(WhenSplitBy("--",
                                      _,
                                      AllOf(Contains(Flag("--compilation-reason=", "bg-dexopt")),
                                            Contains(Flag("-Xtarget-sdk-version:", "456")),
                                            Contains("--debuggable"),
                                            Contains(Flag("--app-image-fd=", _)),
                                            Contains(Flag("-Xhidden-api-policy:", "enabled")))),
                          _,
                          _))
      .WillOnce(Return(0));

  RunDexopt();
}

TEST_F(ArtdTest, dexoptDefaultFlagsWhenNoSystemProps) {
  dexopt_options_.generateAppImage = true;

  EXPECT_CALL(*mock_exec_utils_,
              DoExecAndReturnCode(
                  WhenSplitBy("--",
                              _,
                              AllOf(Contains(Flag("--swap-fd=", FdOf(_))),
                                    Not(Contains(Flag("--instruction-set-features=", _))),
                                    Not(Contains(Flag("--instruction-set-variant=", _))),
                                    Not(Contains(Flag("--max-image-block-size=", _))),
                                    Not(Contains(Flag("--very-large-app-threshold=", _))),
                                    Not(Contains(Flag("--resolve-startup-const-strings=", _))),
                                    Not(Contains("--generate-debug-info")),
                                    Not(Contains("--generate-mini-debug-info")),
                                    Contains("-Xdeny-art-apex-data-files"),
                                    Not(Contains(Flag("--cpu-set=", _))),
                                    Not(Contains(Flag("-j", _))),
                                    Not(Contains(Flag("-Xms", _))),
                                    Not(Contains(Flag("-Xmx", _))),
                                    Not(Contains("--compile-individually")),
                                    Not(Contains(Flag("--image-format=", _))),
                                    Not(Contains("--force-jit-zygote")),
                                    Not(Contains(Flag("--boot-image=", _))))),
                  _,
                  _))
      .WillOnce(Return(0));
  RunDexopt();
}

TEST_F(ArtdTest, dexoptFlagsFromSystemProps) {
  dexopt_options_.generateAppImage = true;

  EXPECT_CALL(*mock_props_, GetProperty("dalvik.vm.dex2oat-swap")).WillOnce(Return("0"));
  EXPECT_CALL(*mock_props_, GetProperty("dalvik.vm.isa.arm64.features"))
      .WillOnce(Return("features"));
  EXPECT_CALL(*mock_props_, GetProperty("dalvik.vm.isa.arm64.variant")).WillOnce(Return("variant"));
  EXPECT_CALL(*mock_props_, GetProperty("dalvik.vm.dex2oat-max-image-block-size"))
      .WillOnce(Return("size"));
  EXPECT_CALL(*mock_props_, GetProperty("dalvik.vm.dex2oat-very-large"))
      .WillOnce(Return("threshold"));
  EXPECT_CALL(*mock_props_, GetProperty("dalvik.vm.dex2oat-resolve-startup-strings"))
      .WillOnce(Return("strings"));
  EXPECT_CALL(*mock_props_, GetProperty("debug.generate-debug-info")).WillOnce(Return("1"));
  EXPECT_CALL(*mock_props_, GetProperty("dalvik.vm.dex2oat-minidebuginfo")).WillOnce(Return("1"));
  EXPECT_CALL(*mock_props_, GetProperty("odsign.verification.success")).WillOnce(Return("1"));
  EXPECT_CALL(*mock_props_, GetProperty("dalvik.vm.dex2oat-Xms")).WillOnce(Return("xms"));
  EXPECT_CALL(*mock_props_, GetProperty("dalvik.vm.dex2oat-Xmx")).WillOnce(Return("xmx"));
  EXPECT_CALL(*mock_props_, GetProperty("ro.config.low_ram")).WillOnce(Return("1"));
  EXPECT_CALL(*mock_props_, GetProperty("dalvik.vm.appimageformat")).WillOnce(Return("imgfmt"));
  EXPECT_CALL(*mock_props_, GetProperty("dalvik.vm.boot-image")).WillOnce(Return("boot-image"));
  EXPECT_CALL(*mock_props_, GetProperty("dalvik.vm.dex2oat-flags"))
      .WillOnce(Return("--flag1 --flag2  --flag3"));

  EXPECT_CALL(*mock_exec_utils_,
              DoExecAndReturnCode(
                  WhenSplitBy("--",
                              _,
                              AllOf(Not(Contains(Flag("--swap-fd=", _))),
                                    Contains(Flag("--instruction-set-features=", "features")),
                                    Contains(Flag("--instruction-set-variant=", "variant")),
                                    Contains(Flag("--max-image-block-size=", "size")),
                                    Contains(Flag("--very-large-app-threshold=", "threshold")),
                                    Contains(Flag("--resolve-startup-const-strings=", "strings")),
                                    Contains("--generate-debug-info"),
                                    Contains("--generate-mini-debug-info"),
                                    Not(Contains("-Xdeny-art-apex-data-files")),
                                    Contains(Flag("-Xms", "xms")),
                                    Contains(Flag("-Xmx", "xmx")),
                                    Contains("--compile-individually"),
                                    Contains(Flag("--image-format=", "imgfmt")),
                                    Not(Contains("--force-jit-zygote")),
                                    Contains(Flag("--boot-image=", "boot-image")),
                                    Contains("--flag1"),
                                    Contains("--flag2"),
                                    Contains("--flag3"))),
                  _,
                  _))
      .WillOnce(Return(0));
  RunDexopt();
}

TEST_F(ArtdTest, dexoptFlagsForceJitZygote) {
  EXPECT_CALL(*mock_props_,
              GetProperty("persist.device_config.runtime_native_boot.profilebootclasspath"))
      .WillOnce(Return("true"));
  ON_CALL(*mock_props_, GetProperty("dalvik.vm.boot-image")).WillByDefault(Return("boot-image"));

  EXPECT_CALL(*mock_exec_utils_,
              DoExecAndReturnCode(WhenSplitBy("--",
                                              _,
                                              AllOf(Contains("--force-jit-zygote"),
                                                    Not(Contains(Flag("--boot-image=", _))))),
                                  _,
                                  _))
      .WillOnce(Return(0));
  RunDexopt();
}

static void SetDefaultResourceControlProps(MockSystemProperties* mock_props) {
  EXPECT_CALL(*mock_props, GetProperty("dalvik.vm.dex2oat-cpu-set")).WillRepeatedly(Return("0,2"));
  EXPECT_CALL(*mock_props, GetProperty("dalvik.vm.dex2oat-threads")).WillRepeatedly(Return("4"));
}

TEST_F(ArtdTest, dexoptDefaultResourceControlBoot) {
  SetDefaultResourceControlProps(mock_props_);

  // The default resource control properties don't apply to BOOT.
  EXPECT_CALL(
      *mock_exec_utils_,
      DoExecAndReturnCode(
          WhenSplitBy(
              "--", _, AllOf(Not(Contains(Flag("--cpu-set=", _))), Contains(Not(Flag("-j", _))))),
          _,
          _))
      .WillOnce(Return(0));
  priority_class_ = PriorityClass::BOOT;
  RunDexopt();
}

TEST_F(ArtdTest, dexoptDefaultResourceControlOther) {
  SetDefaultResourceControlProps(mock_props_);

  EXPECT_CALL(
      *mock_exec_utils_,
      DoExecAndReturnCode(
          WhenSplitBy(
              "--", _, AllOf(Contains(Flag("--cpu-set=", "0,2")), Contains(Flag("-j", "4")))),
          _,
          _))
      .Times(3)
      .WillRepeatedly(Return(0));
  priority_class_ = PriorityClass::INTERACTIVE_FAST;
  RunDexopt();
  priority_class_ = PriorityClass::INTERACTIVE;
  RunDexopt();
  priority_class_ = PriorityClass::BACKGROUND;
  RunDexopt();
}

static void SetAllResourceControlProps(MockSystemProperties* mock_props) {
  EXPECT_CALL(*mock_props, GetProperty("dalvik.vm.dex2oat-cpu-set")).WillRepeatedly(Return("0,2"));
  EXPECT_CALL(*mock_props, GetProperty("dalvik.vm.dex2oat-threads")).WillRepeatedly(Return("4"));
  EXPECT_CALL(*mock_props, GetProperty("dalvik.vm.boot-dex2oat-cpu-set"))
      .WillRepeatedly(Return("0,1,2,3"));
  EXPECT_CALL(*mock_props, GetProperty("dalvik.vm.boot-dex2oat-threads"))
      .WillRepeatedly(Return("8"));
  EXPECT_CALL(*mock_props, GetProperty("dalvik.vm.restore-dex2oat-cpu-set"))
      .WillRepeatedly(Return("0,2,3"));
  EXPECT_CALL(*mock_props, GetProperty("dalvik.vm.restore-dex2oat-threads"))
      .WillRepeatedly(Return("6"));
  EXPECT_CALL(*mock_props, GetProperty("dalvik.vm.background-dex2oat-cpu-set"))
      .WillRepeatedly(Return("0"));
  EXPECT_CALL(*mock_props, GetProperty("dalvik.vm.background-dex2oat-threads"))
      .WillRepeatedly(Return("2"));
}

TEST_F(ArtdTest, dexoptAllResourceControlBoot) {
  SetAllResourceControlProps(mock_props_);

  EXPECT_CALL(
      *mock_exec_utils_,
      DoExecAndReturnCode(
          WhenSplitBy(
              "--", _, AllOf(Contains(Flag("--cpu-set=", "0,1,2,3")), Contains(Flag("-j", "8")))),
          _,
          _))
      .WillOnce(Return(0));
  priority_class_ = PriorityClass::BOOT;
  RunDexopt();
}

TEST_F(ArtdTest, dexoptAllResourceControlInteractiveFast) {
  SetAllResourceControlProps(mock_props_);

  EXPECT_CALL(
      *mock_exec_utils_,
      DoExecAndReturnCode(
          WhenSplitBy(
              "--", _, AllOf(Contains(Flag("--cpu-set=", "0,2,3")), Contains(Flag("-j", "6")))),
          _,
          _))
      .WillOnce(Return(0));
  priority_class_ = PriorityClass::INTERACTIVE_FAST;
  RunDexopt();
}

TEST_F(ArtdTest, dexoptAllResourceControlInteractive) {
  SetAllResourceControlProps(mock_props_);

  // INTERACTIVE always uses the default resource control properties.
  EXPECT_CALL(
      *mock_exec_utils_,
      DoExecAndReturnCode(
          WhenSplitBy(
              "--", _, AllOf(Contains(Flag("--cpu-set=", "0,2")), Contains(Flag("-j", "4")))),
          _,
          _))
      .WillOnce(Return(0));
  priority_class_ = PriorityClass::INTERACTIVE;
  RunDexopt();
}

TEST_F(ArtdTest, dexoptAllResourceControlBackground) {
  SetAllResourceControlProps(mock_props_);

  EXPECT_CALL(
      *mock_exec_utils_,
      DoExecAndReturnCode(
          WhenSplitBy("--", _, AllOf(Contains(Flag("--cpu-set=", "0")), Contains(Flag("-j", "2")))),
          _,
          _))
      .WillOnce(Return(0));
  priority_class_ = PriorityClass::BACKGROUND;
  RunDexopt();
}

TEST_F(ArtdTest, dexoptTerminatedBySignal) {
  EXPECT_CALL(*mock_exec_utils_, DoExecAndReturnCode(_, _, _))
      .WillOnce(Return(Result<int>(Error())));
  RunDexopt(AllOf(Property(&ndk::ScopedAStatus::getExceptionCode, EX_SERVICE_SPECIFIC),
                  Property(&ndk::ScopedAStatus::getMessage,
                           HasSubstr(ART_FORMAT("[status={},exit_code=-1,signal={}]",
                                                static_cast<int>(ExecResult::kSignaled),
                                                SIGKILL)))));
}

TEST_F(ArtdTest, dexoptFailed) {
  dexopt_options_.generateAppImage = true;
  constexpr int kExitCode = 135;
  EXPECT_CALL(*mock_exec_utils_, DoExecAndReturnCode(_, _, _))
      .WillOnce(DoAll(WithArg<0>(WriteToFdFlag("--oat-fd=", "new_oat")),
                      WithArg<0>(WriteToFdFlag("--output-vdex-fd=", "new_vdex")),
                      WithArg<0>(WriteToFdFlag("--app-image-fd=", "new_art")),
                      Return(kExitCode)));
  RunDexopt(AllOf(Property(&ndk::ScopedAStatus::getExceptionCode, EX_SERVICE_SPECIFIC),
                  Property(&ndk::ScopedAStatus::getMessage,
                           HasSubstr(ART_FORMAT("[status={},exit_code={},signal=0]",
                                                static_cast<int>(ExecResult::kExited),
                                                kExitCode)))));

  CheckContent(scratch_path_ + "/a/oat/arm64/b.odex", "old_oat");
  CheckContent(scratch_path_ + "/a/oat/arm64/b.vdex", "old_vdex");
  CheckContent(scratch_path_ + "/a/oat/arm64/b.art", "old_art");
}

TEST_F(ArtdTest, dexoptFailedToCommit) {
  std::unique_ptr<ScopeGuard<std::function<void()>>> scoped_inaccessible;
  std::unique_ptr<ScopeGuard<std::function<void()>>> scoped_unroot;

  EXPECT_CALL(*mock_exec_utils_, DoExecAndReturnCode(_, _, _))
      .WillOnce(DoAll(WithArg<0>(WriteToFdFlag("--oat-fd=", "new_oat")),
                      WithArg<0>(WriteToFdFlag("--output-vdex-fd=", "new_vdex")),
                      [&](auto, auto, auto) {
                        scoped_inaccessible = std::make_unique<ScopeGuard<std::function<void()>>>(
                            ScopedInaccessible(scratch_path_ + "/a/oat/arm64"));
                        scoped_unroot =
                            std::make_unique<ScopeGuard<std::function<void()>>>(ScopedUnroot());
                        return 0;
                      }));

  RunDexopt(
      EX_SERVICE_SPECIFIC,
      AllOf(Field(&ArtdDexoptResult::sizeBytes, 0), Field(&ArtdDexoptResult::sizeBeforeBytes, 0)));
}

TEST_F(ArtdTest, dexoptCancelledBeforeDex2oat) {
  std::shared_ptr<IArtdCancellationSignal> cancellation_signal;
  ASSERT_TRUE(artd_->createCancellationSignal(&cancellation_signal).isOk());

  constexpr pid_t kPid = 123;

  EXPECT_CALL(*mock_exec_utils_, DoExecAndReturnCode(_, _, _))
      .WillOnce([&](auto, const ExecCallbacks& callbacks, auto) {
        callbacks.on_start(kPid);
        callbacks.on_end(kPid);
        return Error();
      });
  EXPECT_CALL(mock_kill_, Call(-kPid, SIGKILL));

  cancellation_signal->cancel();

  RunDexopt(EX_NONE, Field(&ArtdDexoptResult::cancelled, true), cancellation_signal);

  CheckContent(scratch_path_ + "/a/oat/arm64/b.odex", "old_oat");
  CheckContent(scratch_path_ + "/a/oat/arm64/b.vdex", "old_vdex");
  CheckContent(scratch_path_ + "/a/oat/arm64/b.art", "old_art");
}

TEST_F(ArtdTest, dexoptCancelledDuringDex2oat) {
  std::shared_ptr<IArtdCancellationSignal> cancellation_signal;
  ASSERT_TRUE(artd_->createCancellationSignal(&cancellation_signal).isOk());

  constexpr pid_t kPid = 123;
  constexpr std::chrono::duration<int> kTimeout = std::chrono::seconds(1);

  std::condition_variable process_started_cv, process_killed_cv;
  std::mutex mu;

  EXPECT_CALL(*mock_exec_utils_, DoExecAndReturnCode(_, _, _))
      .WillOnce([&](auto, const ExecCallbacks& callbacks, auto) {
        std::unique_lock<std::mutex> lock(mu);
        // Step 2.
        callbacks.on_start(kPid);
        process_started_cv.notify_one();
        EXPECT_EQ(process_killed_cv.wait_for(lock, kTimeout), std::cv_status::no_timeout);
        // Step 5.
        callbacks.on_end(kPid);
        return Error();
      });

  EXPECT_CALL(mock_kill_, Call(-kPid, SIGKILL)).WillOnce([&](auto, auto) {
    // Step 4.
    process_killed_cv.notify_one();
    return 0;
  });

  std::thread t;
  {
    std::unique_lock<std::mutex> lock(mu);
    // Step 1.
    t = std::thread([&] {
      RunDexopt(EX_NONE, Field(&ArtdDexoptResult::cancelled, true), cancellation_signal);
    });
    EXPECT_EQ(process_started_cv.wait_for(lock, kTimeout), std::cv_status::no_timeout);
    // Step 3.
    cancellation_signal->cancel();
  }

  t.join();

  // Step 6.
  CheckContent(scratch_path_ + "/a/oat/arm64/b.odex", "old_oat");
  CheckContent(scratch_path_ + "/a/oat/arm64/b.vdex", "old_vdex");
  CheckContent(scratch_path_ + "/a/oat/arm64/b.art", "old_art");
}

TEST_F(ArtdTest, dexoptCancelledAfterDex2oat) {
  std::shared_ptr<IArtdCancellationSignal> cancellation_signal;
  ASSERT_TRUE(artd_->createCancellationSignal(&cancellation_signal).isOk());

  constexpr pid_t kPid = 123;

  EXPECT_CALL(*mock_exec_utils_, DoExecAndReturnCode(_, _, _))
      .WillOnce(DoAll(WithArg<0>(WriteToFdFlag("--oat-fd=", "new_oat")),
                      WithArg<0>(WriteToFdFlag("--output-vdex-fd=", "new_vdex")),
                      [&](auto, const ExecCallbacks& callbacks, auto) {
                        callbacks.on_start(kPid);
                        callbacks.on_end(kPid);
                        return 0;
                      }));
  EXPECT_CALL(mock_kill_, Call).Times(0);

  RunDexopt(EX_NONE, Field(&ArtdDexoptResult::cancelled, false), cancellation_signal);

  // This signal should be ignored.
  cancellation_signal->cancel();

  CheckContent(scratch_path_ + "/a/oat/arm64/b.odex", "new_oat");
  CheckContent(scratch_path_ + "/a/oat/arm64/b.vdex", "new_vdex");
  EXPECT_FALSE(std::filesystem::exists(scratch_path_ + "/a/oat/arm64/b.art"));
}

TEST_F(ArtdTest, dexoptDexFileNotOtherReadable) {
  dex_file_other_readable_ = false;
  EXPECT_CALL(*mock_exec_utils_, DoExecAndReturnCode(_, _, _)).Times(0);
  RunDexopt(AllOf(Property(&ndk::ScopedAStatus::getExceptionCode, EX_SERVICE_SPECIFIC),
                  Property(&ndk::ScopedAStatus::getMessage,
                           HasSubstr("Outputs cannot be other-readable because the dex file"))));
}

TEST_F(ArtdTest, dexoptProfileNotOtherReadable) {
  profile_other_readable_ = false;
  EXPECT_CALL(*mock_exec_utils_, DoExecAndReturnCode(_, _, _)).Times(0);
  RunDexopt(AllOf(Property(&ndk::ScopedAStatus::getExceptionCode, EX_SERVICE_SPECIFIC),
                  Property(&ndk::ScopedAStatus::getMessage,
                           HasSubstr("Outputs cannot be other-readable because the profile"))));
}

TEST_F(ArtdTest, dexoptOutputNotOtherReadable) {
  output_artifacts_.permissionSettings.fileFsPermission.isOtherReadable = false;
  dex_file_other_readable_ = false;
  profile_other_readable_ = false;
  EXPECT_CALL(*mock_exec_utils_, DoExecAndReturnCode(_, _, _)).WillOnce(Return(0));
  RunDexopt();
  CheckOtherReadable(scratch_path_ + "/a/oat/arm64/b.odex", false);
  CheckOtherReadable(scratch_path_ + "/a/oat/arm64/b.vdex", false);
}

TEST_F(ArtdTest, dexoptUidMismatch) {
  output_artifacts_.permissionSettings.fileFsPermission.uid = 12345;
  output_artifacts_.permissionSettings.fileFsPermission.isOtherReadable = false;
  dex_file_other_readable_ = false;
  EXPECT_CALL(*mock_exec_utils_, DoExecAndReturnCode(_, _, _)).Times(0);
  RunDexopt(AllOf(Property(&ndk::ScopedAStatus::getExceptionCode, EX_SERVICE_SPECIFIC),
                  Property(&ndk::ScopedAStatus::getMessage,
                           HasSubstr("Outputs' owner doesn't match the dex file"))));
}

TEST_F(ArtdTest, dexoptGidMismatch) {
  output_artifacts_.permissionSettings.fileFsPermission.gid = 12345;
  output_artifacts_.permissionSettings.fileFsPermission.isOtherReadable = false;
  dex_file_other_readable_ = false;
  EXPECT_CALL(*mock_exec_utils_, DoExecAndReturnCode(_, _, _)).Times(0);
  RunDexopt(AllOf(Property(&ndk::ScopedAStatus::getExceptionCode, EX_SERVICE_SPECIFIC),
                  Property(&ndk::ScopedAStatus::getMessage,
                           HasSubstr("Outputs' owner doesn't match the dex file"))));
}

TEST_F(ArtdTest, dexoptGidMatchesUid) {
  output_artifacts_.permissionSettings.fileFsPermission = {
      .uid = 123, .gid = 123, .isOtherReadable = false};
  EXPECT_CALL(mock_fstat_, Call(_, _)).WillRepeatedly(fstat);  // For profile.
  EXPECT_CALL(mock_fstat_, Call(FdOf(dex_file_), _))
      .WillOnce(DoAll(SetArgPointee<1>((struct stat){
                          .st_mode = S_IRUSR | S_IRGRP, .st_uid = 123, .st_gid = 456}),
                      Return(0)));
  ON_CALL(*mock_exec_utils_, DoExecAndReturnCode(_, _, _)).WillByDefault(Return(0));
  // It's okay to fail on chown. This happens when the test is not run as root.
  RunDexopt(AnyOf(Property(&ndk::ScopedAStatus::getExceptionCode, EX_NONE),
                  AllOf(Property(&ndk::ScopedAStatus::getExceptionCode, EX_SERVICE_SPECIFIC),
                        Property(&ndk::ScopedAStatus::getMessage, HasSubstr("Failed to chown")))));
}

TEST_F(ArtdTest, dexoptGidMatchesGid) {
  output_artifacts_.permissionSettings.fileFsPermission = {
      .uid = 123, .gid = 456, .isOtherReadable = false};
  EXPECT_CALL(mock_fstat_, Call(_, _)).WillRepeatedly(fstat);  // For profile.
  EXPECT_CALL(mock_fstat_, Call(FdOf(dex_file_), _))
      .WillOnce(DoAll(SetArgPointee<1>((struct stat){
                          .st_mode = S_IRUSR | S_IRGRP, .st_uid = 123, .st_gid = 456}),
                      Return(0)));
  ON_CALL(*mock_exec_utils_, DoExecAndReturnCode(_, _, _)).WillByDefault(Return(0));
  // It's okay to fail on chown. This happens when the test is not run as root.
  RunDexopt(AnyOf(Property(&ndk::ScopedAStatus::getExceptionCode, EX_NONE),
                  AllOf(Property(&ndk::ScopedAStatus::getExceptionCode, EX_SERVICE_SPECIFIC),
                        Property(&ndk::ScopedAStatus::getMessage, HasSubstr("Failed to chown")))));
}

TEST_F(ArtdTest, dexoptUidGidChangeOk) {
  // The dex file is other-readable, so we don't check uid and gid.
  output_artifacts_.permissionSettings.fileFsPermission = {
      .uid = 12345, .gid = 12345, .isOtherReadable = false};
  ON_CALL(*mock_exec_utils_, DoExecAndReturnCode(_, _, _)).WillByDefault(Return(0));
  // It's okay to fail on chown. This happens when the test is not run as root.
  RunDexopt(AnyOf(Property(&ndk::ScopedAStatus::getExceptionCode, EX_NONE),
                  AllOf(Property(&ndk::ScopedAStatus::getExceptionCode, EX_SERVICE_SPECIFIC),
                        Property(&ndk::ScopedAStatus::getMessage, HasSubstr("Failed to chown")))));
}

TEST_F(ArtdTest, dexoptNoUidGidChange) {
  output_artifacts_.permissionSettings.fileFsPermission = {
      .uid = -1, .gid = -1, .isOtherReadable = false};
  dex_file_other_readable_ = false;
  EXPECT_CALL(*mock_exec_utils_, DoExecAndReturnCode(_, _, _)).WillOnce(Return(0));
  RunDexopt();
}

TEST_F(ArtdTest, isProfileUsable) {
  std::string profile_file = OR_FATAL(BuildProfileOrDmPath(profile_path_.value()));
  CreateFile(profile_file);
  CreateFile(dex_file_);

  EXPECT_CALL(
      *mock_exec_utils_,
      DoExecAndReturnCode(
          AllOf(WhenSplitBy(
                    "--",
                    AllOf(Contains(art_root_ + "/bin/art_exec"), Contains("--drop-capabilities")),
                    AllOf(Contains(art_root_ + "/bin/profman"),
                          Contains(Flag("--reference-profile-file-fd=", FdOf(profile_file))),
                          Contains(Flag("--apk-fd=", FdOf(dex_file_))))),
                HasKeepFdsFor("--reference-profile-file-fd=", "--apk-fd=")),
          _,
          _))
      .WillOnce(Return(ProfmanResult::kSkipCompilationSmallDelta));

  bool result;
  EXPECT_TRUE(artd_->isProfileUsable(profile_path_.value(), dex_file_, &result).isOk());
  EXPECT_TRUE(result);
}

TEST_F(ArtdTest, isProfileUsableFalse) {
  std::string profile_file = OR_FATAL(BuildProfileOrDmPath(profile_path_.value()));
  CreateFile(profile_file);
  CreateFile(dex_file_);

  EXPECT_CALL(*mock_exec_utils_, DoExecAndReturnCode(_, _, _))
      .WillOnce(Return(ProfmanResult::kSkipCompilationEmptyProfiles));

  bool result;
  EXPECT_TRUE(artd_->isProfileUsable(profile_path_.value(), dex_file_, &result).isOk());
  EXPECT_FALSE(result);
}

TEST_F(ArtdTest, isProfileUsableNotFound) {
  CreateFile(dex_file_);

  bool result;
  EXPECT_TRUE(artd_->isProfileUsable(profile_path_.value(), dex_file_, &result).isOk());
  EXPECT_FALSE(result);
}

TEST_F(ArtdTest, isProfileUsableFailed) {
  std::string profile_file = OR_FATAL(BuildProfileOrDmPath(profile_path_.value()));
  CreateFile(profile_file);
  CreateFile(dex_file_);

  EXPECT_CALL(*mock_exec_utils_, DoExecAndReturnCode(_, _, _)).WillOnce(Return(100));

  bool result;
  ndk::ScopedAStatus status = artd_->isProfileUsable(profile_path_.value(), dex_file_, &result);

  EXPECT_FALSE(status.isOk());
  EXPECT_EQ(status.getExceptionCode(), EX_SERVICE_SPECIFIC);
  EXPECT_THAT(status.getMessage(), HasSubstr("profman returned an unexpected code: 100"));
}

TEST_F(ArtdTest, copyAndRewriteProfileSuccess) {
  std::string src_file = OR_FATAL(BuildProfileOrDmPath(profile_path_.value()));
  CreateFile(src_file, "valid_profile");

  CreateFile(dex_file_);

  EXPECT_CALL(
      *mock_exec_utils_,
      DoExecAndReturnCode(
          AllOf(WhenSplitBy(
                    "--",
                    AllOf(Contains(art_root_ + "/bin/art_exec"), Contains("--drop-capabilities")),
                    AllOf(Contains(art_root_ + "/bin/profman"),
                          Contains("--copy-and-update-profile-key"),
                          Contains(Flag("--profile-file-fd=", FdOf(src_file))),
                          Contains(Flag("--apk-fd=", FdOf(dex_file_))))),
                HasKeepFdsFor("--profile-file-fd=", "--reference-profile-file-fd=", "--apk-fd=")),
          _,
          _))
      .WillOnce(DoAll(WithArg<0>(WriteToFdFlag("--reference-profile-file-fd=", "def")),
                      Return(ProfmanResult::kCopyAndUpdateSuccess)));

  auto [result, dst] = OR_FAIL(RunCopyAndRewriteProfile());

  EXPECT_EQ(result.status, CopyAndRewriteProfileResult::Status::SUCCESS);
  EXPECT_THAT(dst.profilePath.id, Not(IsEmpty()));
  std::string real_path = OR_FATAL(BuildTmpProfilePath(dst.profilePath));
  EXPECT_EQ(dst.profilePath.tmpPath, real_path);
  CheckContent(real_path, "def");
}

// The input is a plain profile file in the wrong format.
TEST_F(ArtdTest, copyAndRewriteProfileBadProfileWrongFormat) {
  std::string src_file = OR_FATAL(BuildProfileOrDmPath(profile_path_.value()));
  CreateFile(src_file, "wrong_format");

  CreateFile(dex_file_);

  EXPECT_CALL(*mock_exec_utils_, DoExecAndReturnCode(_, _, _))
      .WillOnce(Return(ProfmanResult::kCopyAndUpdateErrorFailedToLoadProfile));

  auto [result, dst] = OR_FAIL(RunCopyAndRewriteProfile());

  EXPECT_EQ(result.status, CopyAndRewriteProfileResult::Status::BAD_PROFILE);
  EXPECT_THAT(result.errorMsg,
              HasSubstr("The profile is in the wrong format or an I/O error has occurred"));
  EXPECT_THAT(dst.profilePath.id, IsEmpty());
  EXPECT_THAT(dst.profilePath.tmpPath, IsEmpty());
}

// The input is a plain profile file that doesn't match the APK.
TEST_F(ArtdTest, copyAndRewriteProfileBadProfileNoMatch) {
  std::string src_file = OR_FATAL(BuildProfileOrDmPath(profile_path_.value()));
  CreateFile(src_file, "no_match");

  CreateFile(dex_file_);

  EXPECT_CALL(*mock_exec_utils_, DoExecAndReturnCode(_, _, _))
      .WillOnce(Return(ProfmanResult::kCopyAndUpdateNoMatch));

  auto [result, dst] = OR_FAIL(RunCopyAndRewriteProfile());

  EXPECT_EQ(result.status, CopyAndRewriteProfileResult::Status::BAD_PROFILE);
  EXPECT_THAT(result.errorMsg, HasSubstr("The profile does not match the APK"));
  EXPECT_THAT(dst.profilePath.id, IsEmpty());
  EXPECT_THAT(dst.profilePath.tmpPath, IsEmpty());
}

// The input is a plain profile file that is empty.
TEST_F(ArtdTest, copyAndRewriteProfileNoProfileEmpty) {
  std::string src_file = OR_FATAL(BuildProfileOrDmPath(profile_path_.value()));
  CreateFile(src_file, "");

  CreateFile(dex_file_);

  EXPECT_CALL(*mock_exec_utils_, DoExecAndReturnCode(_, _, _))
      .WillOnce(Return(ProfmanResult::kCopyAndUpdateNoMatch));

  auto [result, dst] = OR_FAIL(RunCopyAndRewriteProfile());

  EXPECT_EQ(result.status, CopyAndRewriteProfileResult::Status::NO_PROFILE);
  EXPECT_THAT(dst.profilePath.id, IsEmpty());
  EXPECT_THAT(dst.profilePath.tmpPath, IsEmpty());
}

// The input does not exist.
TEST_F(ArtdTest, copyAndRewriteProfileNoProfileNoFile) {
  CreateFile(dex_file_);

  auto [result, dst] = OR_FAIL(RunCopyAndRewriteProfile());

  EXPECT_EQ(result.status, CopyAndRewriteProfileResult::Status::NO_PROFILE);
  EXPECT_THAT(dst.profilePath.id, IsEmpty());
  EXPECT_THAT(dst.profilePath.tmpPath, IsEmpty());
}

// The input is a dm file with a profile entry in the wrong format.
TEST_F(ArtdTest, copyAndRewriteProfileNoProfileDmWrongFormat) {
  std::string src_file = OR_FATAL(BuildProfileOrDmPath(profile_path_.value()));
  CreateZipWithSingleEntry(src_file, "primary.prof", "wrong_format");

  CreateFile(dex_file_);

  EXPECT_CALL(*mock_exec_utils_, DoExecAndReturnCode(_, _, _))
      .WillOnce(Return(ProfmanResult::kCopyAndUpdateErrorFailedToLoadProfile));

  auto [result, dst] = OR_FAIL(RunCopyAndRewriteProfile());

  EXPECT_EQ(result.status, CopyAndRewriteProfileResult::Status::BAD_PROFILE);
  EXPECT_THAT(result.errorMsg,
              HasSubstr("The profile is in the wrong format or an I/O error has occurred"));
  EXPECT_THAT(dst.profilePath.id, IsEmpty());
  EXPECT_THAT(dst.profilePath.tmpPath, IsEmpty());
}

// The input is a dm file with a profile entry that doesn't match the APK.
TEST_F(ArtdTest, copyAndRewriteProfileNoProfileDmNoMatch) {
  std::string src_file = OR_FATAL(BuildProfileOrDmPath(profile_path_.value()));
  CreateZipWithSingleEntry(src_file, "primary.prof", "no_match");

  CreateFile(dex_file_);

  EXPECT_CALL(*mock_exec_utils_, DoExecAndReturnCode(_, _, _))
      .WillOnce(Return(ProfmanResult::kCopyAndUpdateNoMatch));

  auto [result, dst] = OR_FAIL(RunCopyAndRewriteProfile());

  EXPECT_EQ(result.status, CopyAndRewriteProfileResult::Status::BAD_PROFILE);
  EXPECT_THAT(result.errorMsg, HasSubstr("The profile does not match the APK"));
  EXPECT_THAT(dst.profilePath.id, IsEmpty());
  EXPECT_THAT(dst.profilePath.tmpPath, IsEmpty());
}

// The input is a dm file with a profile entry that is empty.
TEST_F(ArtdTest, copyAndRewriteProfileNoProfileDmEmpty) {
  std::string src_file = OR_FATAL(BuildProfileOrDmPath(profile_path_.value()));
  CreateZipWithSingleEntry(src_file, "primary.prof");

  CreateFile(dex_file_);

  EXPECT_CALL(*mock_exec_utils_, DoExecAndReturnCode(_, _, _))
      .WillOnce(Return(ProfmanResult::kCopyAndUpdateNoMatch));

  auto [result, dst] = OR_FAIL(RunCopyAndRewriteProfile());

  EXPECT_EQ(result.status, CopyAndRewriteProfileResult::Status::NO_PROFILE);
  EXPECT_THAT(dst.profilePath.id, IsEmpty());
  EXPECT_THAT(dst.profilePath.tmpPath, IsEmpty());
}

// The input is a dm file without a profile entry.
TEST_F(ArtdTest, copyAndRewriteProfileNoProfileDmNoEntry) {
  std::string src_file = OR_FATAL(BuildProfileOrDmPath(profile_path_.value()));
  CreateZipWithSingleEntry(src_file, "primary.vdex");

  CreateFile(dex_file_);

  EXPECT_CALL(*mock_exec_utils_, DoExecAndReturnCode(_, _, _))
      .WillOnce(Return(ProfmanResult::kCopyAndUpdateNoMatch));

  auto [result, dst] = OR_FAIL(RunCopyAndRewriteProfile());

  EXPECT_EQ(result.status, CopyAndRewriteProfileResult::Status::NO_PROFILE);
  EXPECT_THAT(dst.profilePath.id, IsEmpty());
  EXPECT_THAT(dst.profilePath.tmpPath, IsEmpty());
}

TEST_F(ArtdTest, copyAndRewriteProfileException) {
  std::string src_file = OR_FATAL(BuildProfileOrDmPath(profile_path_.value()));
  CreateFile(src_file, "valid_profile");

  CreateFile(dex_file_);

  EXPECT_CALL(*mock_exec_utils_, DoExecAndReturnCode(_, _, _)).WillOnce(Return(100));

  auto [status, dst] = OR_FAIL(RunCopyAndRewriteProfile</*kExpectOk=*/false>());

  EXPECT_FALSE(status.isOk());
  EXPECT_EQ(status.getExceptionCode(), EX_SERVICE_SPECIFIC);
  EXPECT_THAT(status.getMessage(), HasSubstr("profman returned an unexpected code: 100"));
  EXPECT_THAT(dst.profilePath.id, IsEmpty());
  EXPECT_THAT(dst.profilePath.tmpPath, IsEmpty());
}

TEST_F(ArtdTest, copyAndRewriteEmbeddedProfileSuccess) {
  TEST_DISABLED_FOR_SHELL_WITHOUT_MEMFD_ACCESS();

  CreateZipWithSingleEntry(dex_file_, "assets/art-profile/baseline.prof", "valid_profile");

  EXPECT_CALL(
      *mock_exec_utils_,
      DoExecAndReturnCode(
          AllOf(WhenSplitBy(
                    "--",
                    AllOf(Contains(art_root_ + "/bin/art_exec"), Contains("--drop-capabilities")),
                    AllOf(Contains(art_root_ + "/bin/profman"),
                          Contains("--copy-and-update-profile-key"),
                          Contains(Flag("--profile-file-fd=", FdHasContent("valid_profile"))),
                          Contains(Flag("--apk-fd=", FdOf(dex_file_))))),
                HasKeepFdsFor("--profile-file-fd=", "--reference-profile-file-fd=", "--apk-fd=")),
          _,
          _))
      .WillOnce(DoAll(WithArg<0>(WriteToFdFlag("--reference-profile-file-fd=", "def")),
                      Return(ProfmanResult::kCopyAndUpdateSuccess)));

  auto [result, dst] = OR_FAIL(RunCopyAndRewriteEmbeddedProfile());

  EXPECT_EQ(result.status, CopyAndRewriteProfileResult::Status::SUCCESS);
  EXPECT_THAT(dst.profilePath.id, Not(IsEmpty()));
  std::string real_path = OR_FATAL(BuildTmpProfilePath(dst.profilePath));
  EXPECT_EQ(dst.profilePath.tmpPath, real_path);
  CheckContent(real_path, "def");
}

// The input is a plain dex file.
TEST_F(ArtdTest, copyAndRewriteEmbeddedProfileNoProfilePlainDex) {
  TEST_DISABLED_FOR_SHELL_WITHOUT_MEMFD_ACCESS();

  constexpr const char* kDexMagic = "dex\n";
  CreateFile(dex_file_, kDexMagic + "dex_code"s);

  auto [result, dst] = OR_FAIL(RunCopyAndRewriteEmbeddedProfile());

  EXPECT_EQ(result.status, CopyAndRewriteProfileResult::Status::NO_PROFILE);
  EXPECT_THAT(dst.profilePath.id, IsEmpty());
  EXPECT_THAT(dst.profilePath.tmpPath, IsEmpty());
}

// The input is neither a zip nor a plain dex file.
TEST_F(ArtdTest, copyAndRewriteEmbeddedProfileNotZipNotDex) {
  TEST_DISABLED_FOR_SHELL_WITHOUT_MEMFD_ACCESS();

  CreateFile(dex_file_, "wrong_format");

  auto [status, dst] = OR_FAIL(RunCopyAndRewriteEmbeddedProfile</*kExpectOk=*/false>());

  EXPECT_FALSE(status.isOk());
  EXPECT_EQ(status.getExceptionCode(), EX_SERVICE_SPECIFIC);
  EXPECT_THAT(status.getMessage(), HasSubstr("File is neither a zip file nor a plain dex file"));
  EXPECT_THAT(dst.profilePath.id, IsEmpty());
  EXPECT_THAT(dst.profilePath.tmpPath, IsEmpty());
}

// The input is a zip file without a profile entry.
TEST_F(ArtdTest, copyAndRewriteEmbeddedProfileNoProfileZipNoEntry) {
  TEST_DISABLED_FOR_SHELL_WITHOUT_MEMFD_ACCESS();

  CreateZipWithSingleEntry(dex_file_, "classes.dex", "dex_code");

  auto [result, dst] = OR_FAIL(RunCopyAndRewriteEmbeddedProfile());

  EXPECT_EQ(result.status, CopyAndRewriteProfileResult::Status::NO_PROFILE);
  EXPECT_THAT(dst.profilePath.id, IsEmpty());
  EXPECT_THAT(dst.profilePath.tmpPath, IsEmpty());
}

// The input is a zip file with a profile entry that doesn't match itself.
TEST_F(ArtdTest, copyAndRewriteEmbeddedProfileBadProfileNoMatch) {
  TEST_DISABLED_FOR_SHELL_WITHOUT_MEMFD_ACCESS();

  CreateZipWithSingleEntry(dex_file_, "assets/art-profile/baseline.prof", "no_match");

  EXPECT_CALL(*mock_exec_utils_, DoExecAndReturnCode(_, _, _))
      .WillOnce(Return(ProfmanResult::kCopyAndUpdateNoMatch));

  auto [result, dst] = OR_FAIL(RunCopyAndRewriteEmbeddedProfile());

  EXPECT_EQ(result.status, CopyAndRewriteProfileResult::Status::BAD_PROFILE);
  EXPECT_THAT(result.errorMsg, HasSubstr("The profile does not match the APK"));
  EXPECT_THAT(dst.profilePath.id, IsEmpty());
  EXPECT_THAT(dst.profilePath.tmpPath, IsEmpty());
}

TEST_F(ArtdTest, commitTmpProfile) {
  std::string tmp_profile_file = OR_FATAL(BuildTmpProfilePath(tmp_profile_path_));
  CreateFile(tmp_profile_file);

  EXPECT_TRUE(artd_->commitTmpProfile(tmp_profile_path_).isOk());

  EXPECT_FALSE(std::filesystem::exists(tmp_profile_file));
  EXPECT_TRUE(std::filesystem::exists(OR_FATAL(BuildFinalProfilePath(tmp_profile_path_))));
}

TEST_F(ArtdTest, commitTmpProfileFailed) {
  ndk::ScopedAStatus status = artd_->commitTmpProfile(tmp_profile_path_);

  EXPECT_FALSE(status.isOk());
  EXPECT_EQ(status.getExceptionCode(), EX_SERVICE_SPECIFIC);
  EXPECT_THAT(
      status.getMessage(),
      ContainsRegex(R"re(Failed to move .*primary\.prof\.12345\.tmp.* to .*primary\.prof)re"));

  EXPECT_FALSE(std::filesystem::exists(OR_FATAL(BuildFinalProfilePath(tmp_profile_path_))));
}

TEST_F(ArtdTest, deleteProfile) {
  std::string profile_file = OR_FATAL(BuildProfileOrDmPath(profile_path_.value()));
  CreateFile(profile_file);

  EXPECT_TRUE(artd_->deleteProfile(profile_path_.value()).isOk());

  EXPECT_FALSE(std::filesystem::exists(profile_file));
}

TEST_F(ArtdTest, deleteProfileDoesNotExist) {
  auto scoped_set_logger = ScopedSetLogger(mock_logger_.AsStdFunction());
  EXPECT_CALL(mock_logger_, Call).Times(0);

  EXPECT_TRUE(artd_->deleteProfile(profile_path_.value()).isOk());
}

TEST_F(ArtdTest, deleteProfileFailed) {
  auto scoped_set_logger = ScopedSetLogger(mock_logger_.AsStdFunction());
  EXPECT_CALL(
      mock_logger_,
      Call(_, _, _, _, _, ContainsRegex(R"re(Failed to remove .*primary\.prof\.12345\.tmp)re")));

  std::string profile_file = OR_FATAL(BuildProfileOrDmPath(profile_path_.value()));
  auto scoped_inaccessible = ScopedInaccessible(std::filesystem::path(profile_file).parent_path());
  auto scoped_unroot = ScopedUnroot();

  EXPECT_TRUE(artd_->deleteProfile(profile_path_.value()).isOk());
}

class ArtdGetVisibilityTest : public ArtdTest {
 protected:
  template <typename PathType>
  using Method = ndk::ScopedAStatus (Artd::*)(const PathType&, FileVisibility*);

  template <typename PathType>
  void TestGetVisibilityOtherReadable(Method<PathType> method,
                                      const PathType& input,
                                      const std::string& path) {
    CreateFile(path);
    std::filesystem::permissions(
        path, std::filesystem::perms::others_read, std::filesystem::perm_options::add);

    FileVisibility result;
    ASSERT_TRUE(((*artd_).*method)(input, &result).isOk());
    EXPECT_EQ(result, FileVisibility::OTHER_READABLE);
  }

  template <typename PathType>
  void TestGetVisibilityNotOtherReadable(Method<PathType> method,
                                         const PathType& input,
                                         const std::string& path) {
    CreateFile(path);
    std::filesystem::permissions(
        path, std::filesystem::perms::others_read, std::filesystem::perm_options::remove);

    FileVisibility result;
    ASSERT_TRUE(((*artd_).*method)(input, &result).isOk());
    EXPECT_EQ(result, FileVisibility::NOT_OTHER_READABLE);
  }

  template <typename PathType>
  void TestGetVisibilityNotFound(Method<PathType> method, const PathType& input) {
    FileVisibility result;
    ASSERT_TRUE(((*artd_).*method)(input, &result).isOk());
    EXPECT_EQ(result, FileVisibility::NOT_FOUND);
  }

  template <typename PathType>
  void TestGetVisibilityPermissionDenied(Method<PathType> method,
                                         const PathType& input,
                                         const std::string& path) {
    CreateFile(path);

    auto scoped_inaccessible = ScopedInaccessible(std::filesystem::path(path).parent_path());
    auto scoped_unroot = ScopedUnroot();

    FileVisibility result;
    ndk::ScopedAStatus status = ((*artd_).*method)(input, &result);
    EXPECT_FALSE(status.isOk());
    EXPECT_EQ(status.getExceptionCode(), EX_SERVICE_SPECIFIC);
    EXPECT_THAT(status.getMessage(), HasSubstr("Failed to get status of"));
  }
};

TEST_F(ArtdGetVisibilityTest, getProfileVisibilityOtherReadable) {
  TestGetVisibilityOtherReadable(&Artd::getProfileVisibility,
                                 profile_path_.value(),
                                 OR_FATAL(BuildProfileOrDmPath(profile_path_.value())));
}

TEST_F(ArtdGetVisibilityTest, getProfileVisibilityNotOtherReadable) {
  TestGetVisibilityNotOtherReadable(&Artd::getProfileVisibility,
                                    profile_path_.value(),
                                    OR_FATAL(BuildProfileOrDmPath(profile_path_.value())));
}

TEST_F(ArtdGetVisibilityTest, getProfileVisibilityNotFound) {
  TestGetVisibilityNotFound(&Artd::getProfileVisibility, profile_path_.value());
}

TEST_F(ArtdGetVisibilityTest, getProfileVisibilityPermissionDenied) {
  TestGetVisibilityPermissionDenied(&Artd::getProfileVisibility,
                                    profile_path_.value(),
                                    OR_FATAL(BuildProfileOrDmPath(profile_path_.value())));
}

TEST_F(ArtdGetVisibilityTest, getArtifactsVisibilityOtherReadable) {
  TestGetVisibilityOtherReadable(&Artd::getArtifactsVisibility,
                                 artifacts_path_,
                                 OR_FATAL(BuildArtifactsPath(artifacts_path_)).oat_path);
}

TEST_F(ArtdGetVisibilityTest, getArtifactsVisibilityNotOtherReadable) {
  TestGetVisibilityNotOtherReadable(&Artd::getArtifactsVisibility,
                                    artifacts_path_,
                                    OR_FATAL(BuildArtifactsPath(artifacts_path_)).oat_path);
}

TEST_F(ArtdGetVisibilityTest, getArtifactsVisibilityNotFound) {
  TestGetVisibilityNotFound(&Artd::getArtifactsVisibility, artifacts_path_);
}

TEST_F(ArtdGetVisibilityTest, getArtifactsVisibilityPermissionDenied) {
  TestGetVisibilityPermissionDenied(&Artd::getArtifactsVisibility,
                                    artifacts_path_,
                                    OR_FATAL(BuildArtifactsPath(artifacts_path_)).oat_path);
}

TEST_F(ArtdGetVisibilityTest, getDexFileVisibilityOtherReadable) {
  TestGetVisibilityOtherReadable(&Artd::getDexFileVisibility, dex_file_, dex_file_);
}

TEST_F(ArtdGetVisibilityTest, getDexFileVisibilityNotOtherReadable) {
  TestGetVisibilityNotOtherReadable(&Artd::getDexFileVisibility, dex_file_, dex_file_);
}

TEST_F(ArtdGetVisibilityTest, getDexFileVisibilityNotFound) {
  TestGetVisibilityNotFound(&Artd::getDexFileVisibility, dex_file_);
}

TEST_F(ArtdGetVisibilityTest, getDexFileVisibilityPermissionDenied) {
  TestGetVisibilityPermissionDenied(&Artd::getDexFileVisibility, dex_file_, dex_file_);
}

TEST_F(ArtdGetVisibilityTest, getDmFileVisibilityOtherReadable) {
  TestGetVisibilityOtherReadable(&Artd::getDmFileVisibility,
                                 dm_path_.value(),
                                 OR_FATAL(BuildDexMetadataPath(dm_path_.value())));
}

TEST_F(ArtdGetVisibilityTest, getDmFileVisibilityNotOtherReadable) {
  TestGetVisibilityNotOtherReadable(&Artd::getDmFileVisibility,
                                    dm_path_.value(),
                                    OR_FATAL(BuildDexMetadataPath(dm_path_.value())));
}

TEST_F(ArtdGetVisibilityTest, getDmFileVisibilityNotFound) {
  TestGetVisibilityNotFound(&Artd::getDmFileVisibility, dm_path_.value());
}

TEST_F(ArtdGetVisibilityTest, getDmFileVisibilityPermissionDenied) {
  TestGetVisibilityPermissionDenied(&Artd::getDmFileVisibility,
                                    dm_path_.value(),
                                    OR_FATAL(BuildDexMetadataPath(dm_path_.value())));
}

TEST_F(ArtdTest, mergeProfiles) {
  std::string reference_profile_file = OR_FATAL(BuildProfileOrDmPath(profile_path_.value()));
  CreateFile(reference_profile_file, "abc");

  // Doesn't exist.
  PrimaryCurProfilePath profile_0_path{
      .userId = 0, .packageName = "com.android.foo", .profileName = "primary"};
  std::string profile_0_file = OR_FATAL(BuildPrimaryCurProfilePath(profile_0_path));

  PrimaryCurProfilePath profile_1_path{
      .userId = 1, .packageName = "com.android.foo", .profileName = "primary"};
  std::string profile_1_file = OR_FATAL(BuildPrimaryCurProfilePath(profile_1_path));
  CreateFile(profile_1_file, "def");

  OutputProfile output_profile{.profilePath = tmp_profile_path_,
                               .fsPermission = FsPermission{.uid = -1, .gid = -1}};
  output_profile.profilePath.id = "";
  output_profile.profilePath.tmpPath = "";

  std::string dex_file_1 = scratch_path_ + "/a/b.apk";
  std::string dex_file_2 = scratch_path_ + "/a/c.apk";
  CreateFile(dex_file_1);
  CreateFile(dex_file_2);

  EXPECT_CALL(
      *mock_exec_utils_,
      DoExecAndReturnCode(
          AllOf(WhenSplitBy(
                    "--",
                    AllOf(Contains(art_root_ + "/bin/art_exec"), Contains("--drop-capabilities")),
                    AllOf(Contains(art_root_ + "/bin/profman"),
                          Not(Contains(Flag("--profile-file-fd=", FdOf(profile_0_file)))),
                          Contains(Flag("--profile-file-fd=", FdOf(profile_1_file))),
                          Contains(Flag("--reference-profile-file-fd=", FdHasContent("abc"))),
                          Contains(Flag("--apk-fd=", FdOf(dex_file_1))),
                          Contains(Flag("--apk-fd=", FdOf(dex_file_2))),
                          Not(Contains("--force-merge-and-analyze")),
                          Not(Contains("--boot-image-merge")))),
                HasKeepFdsFor("--profile-file-fd=", "--reference-profile-file-fd=", "--apk-fd=")),
          _,
          _))
      .WillOnce(DoAll(WithArg<0>(ClearAndWriteToFdFlag("--reference-profile-file-fd=", "merged")),
                      Return(ProfmanResult::kCompile)));

  bool result;
  EXPECT_TRUE(artd_
                  ->mergeProfiles({profile_0_path, profile_1_path},
                                  profile_path_,
                                  &output_profile,
                                  {dex_file_1, dex_file_2},
                                  /*in_options=*/{},
                                  &result)
                  .isOk());
  EXPECT_TRUE(result);
  EXPECT_THAT(output_profile.profilePath.id, Not(IsEmpty()));
  std::string real_path = OR_FATAL(BuildTmpProfilePath(output_profile.profilePath));
  EXPECT_EQ(output_profile.profilePath.tmpPath, real_path);
  CheckContent(real_path, "merged");
}

TEST_F(ArtdTest, mergeProfilesEmptyReferenceProfile) {
  PrimaryCurProfilePath profile_0_path{
      .userId = 0, .packageName = "com.android.foo", .profileName = "primary"};
  std::string profile_0_file = OR_FATAL(BuildPrimaryCurProfilePath(profile_0_path));
  CreateFile(profile_0_file, "def");

  OutputProfile output_profile{.profilePath = tmp_profile_path_,
                               .fsPermission = FsPermission{.uid = -1, .gid = -1}};
  output_profile.profilePath.id = "";
  output_profile.profilePath.tmpPath = "";

  CreateFile(dex_file_);

  EXPECT_CALL(
      *mock_exec_utils_,
      DoExecAndReturnCode(
          WhenSplitBy("--",
                      AllOf(Contains(art_root_ + "/bin/art_exec"), Contains("--drop-capabilities")),
                      AllOf(Contains(art_root_ + "/bin/profman"),
                            Contains(Flag("--profile-file-fd=", FdOf(profile_0_file))),
                            Contains(Flag("--reference-profile-file-fd=", FdHasContent(""))),
                            Contains(Flag("--apk-fd=", FdOf(dex_file_))))),
          _,
          _))
      .WillOnce(DoAll(WithArg<0>(WriteToFdFlag("--reference-profile-file-fd=", "merged")),
                      Return(ProfmanResult::kCompile)));

  bool result;
  EXPECT_TRUE(artd_
                  ->mergeProfiles({profile_0_path},
                                  std::nullopt,
                                  &output_profile,
                                  {dex_file_},
                                  /*in_options=*/{},
                                  &result)
                  .isOk());
  EXPECT_TRUE(result);
  EXPECT_THAT(output_profile.profilePath.id, Not(IsEmpty()));
  EXPECT_THAT(output_profile.profilePath.tmpPath, Not(IsEmpty()));
}

TEST_F(ArtdTest, mergeProfilesProfilesDontExist) {
  // Doesn't exist.
  PrimaryCurProfilePath profile_0_path{
      .userId = 0, .packageName = "com.android.foo", .profileName = "primary"};
  std::string profile_0_file = OR_FATAL(BuildPrimaryCurProfilePath(profile_0_path));

  // Doesn't exist.
  PrimaryCurProfilePath profile_1_path{
      .userId = 1, .packageName = "com.android.foo", .profileName = "primary"};
  std::string profile_1_file = OR_FATAL(BuildPrimaryCurProfilePath(profile_1_path));

  OutputProfile output_profile{.profilePath = tmp_profile_path_,
                               .fsPermission = FsPermission{.uid = -1, .gid = -1}};
  output_profile.profilePath.id = "";
  output_profile.profilePath.tmpPath = "";

  CreateFile(dex_file_);

  EXPECT_CALL(*mock_exec_utils_, DoExecAndReturnCode).Times(0);

  bool result;
  EXPECT_TRUE(artd_
                  ->mergeProfiles({profile_0_path},
                                  /*in_referenceProfile=*/std::nullopt,
                                  &output_profile,
                                  {dex_file_},
                                  /*in_options=*/{},
                                  &result)
                  .isOk());
  EXPECT_FALSE(result);
  EXPECT_THAT(output_profile.profilePath.id, IsEmpty());
  EXPECT_THAT(output_profile.profilePath.tmpPath, IsEmpty());
}

TEST_F(ArtdTest, mergeProfilesWithOptionsForceMerge) {
  PrimaryCurProfilePath profile_0_path{
      .userId = 0, .packageName = "com.android.foo", .profileName = "primary"};
  std::string profile_0_file = OR_FATAL(BuildPrimaryCurProfilePath(profile_0_path));
  CreateFile(profile_0_file, "def");

  OutputProfile output_profile{.profilePath = tmp_profile_path_,
                               .fsPermission = FsPermission{.uid = -1, .gid = -1}};
  output_profile.profilePath.id = "";
  output_profile.profilePath.tmpPath = "";

  CreateFile(dex_file_);

  EXPECT_CALL(*mock_exec_utils_,
              DoExecAndReturnCode(WhenSplitBy("--",
                                              _,
                                              AllOf(Contains("--force-merge-and-analyze"),
                                                    Contains("--boot-image-merge"))),
                                  _,
                                  _))
      .WillOnce(Return(ProfmanResult::kCompile));

  bool result;
  EXPECT_TRUE(artd_
                  ->mergeProfiles({profile_0_path},
                                  std::nullopt,
                                  &output_profile,
                                  {dex_file_},
                                  {.forceMerge = true, .forBootImage = true},
                                  &result)
                  .isOk());
  EXPECT_TRUE(result);
  EXPECT_THAT(output_profile.profilePath.id, Not(IsEmpty()));
  EXPECT_THAT(output_profile.profilePath.tmpPath, Not(IsEmpty()));
}

TEST_F(ArtdTest, mergeProfilesWithOptionsDumpOnly) {
  PrimaryCurProfilePath profile_0_path{
      .userId = 0, .packageName = "com.android.foo", .profileName = "primary"};
  std::string profile_0_file = OR_FATAL(BuildPrimaryCurProfilePath(profile_0_path));
  CreateFile(profile_0_file, "def");

  OutputProfile output_profile{.profilePath = tmp_profile_path_,
                               .fsPermission = FsPermission{.uid = -1, .gid = -1}};
  output_profile.profilePath.id = "";
  output_profile.profilePath.tmpPath = "";

  CreateFile(dex_file_);

  EXPECT_CALL(*mock_exec_utils_,
              DoExecAndReturnCode(
                  AllOf(WhenSplitBy("--",
                                    _,
                                    AllOf(Contains("--dump-only"),
                                          Not(Contains(Flag("--reference-profile-file-fd=", _))))),
                        HasKeepFdsFor("--profile-file-fd=", "--apk-fd=", "--dump-output-to-fd=")),
                  _,
                  _))
      .WillOnce(DoAll(WithArg<0>(WriteToFdFlag("--dump-output-to-fd=", "dump")),
                      Return(ProfmanResult::kSuccess)));

  bool result;
  EXPECT_TRUE(artd_
                  ->mergeProfiles({profile_0_path},
                                  std::nullopt,
                                  &output_profile,
                                  {dex_file_},
                                  {.dumpOnly = true},
                                  &result)
                  .isOk());
  EXPECT_TRUE(result);
  EXPECT_THAT(output_profile.profilePath.id, Not(IsEmpty()));
  CheckContent(output_profile.profilePath.tmpPath, "dump");
}

TEST_F(ArtdTest, mergeProfilesWithOptionsDumpClassesAndMethods) {
  PrimaryCurProfilePath profile_0_path{
      .userId = 0, .packageName = "com.android.foo", .profileName = "primary"};
  std::string profile_0_file = OR_FATAL(BuildPrimaryCurProfilePath(profile_0_path));
  CreateFile(profile_0_file, "def");

  OutputProfile output_profile{.profilePath = tmp_profile_path_,
                               .fsPermission = FsPermission{.uid = -1, .gid = -1}};
  output_profile.profilePath.id = "";
  output_profile.profilePath.tmpPath = "";

  CreateFile(dex_file_);

  EXPECT_CALL(*mock_exec_utils_,
              DoExecAndReturnCode(
                  WhenSplitBy("--",
                              _,
                              AllOf(Contains("--dump-classes-and-methods"),
                                    Not(Contains(Flag("--reference-profile-file-fd=", _))))),
                  _,
                  _))
      .WillOnce(DoAll(WithArg<0>(WriteToFdFlag("--dump-output-to-fd=", "dump")),
                      Return(ProfmanResult::kSuccess)));

  bool result;
  EXPECT_TRUE(artd_
                  ->mergeProfiles({profile_0_path},
                                  std::nullopt,
                                  &output_profile,
                                  {dex_file_},
                                  {.dumpClassesAndMethods = true},
                                  &result)
                  .isOk());
  EXPECT_TRUE(result);
  EXPECT_THAT(output_profile.profilePath.id, Not(IsEmpty()));
  CheckContent(output_profile.profilePath.tmpPath, "dump");
}

static std::string EncodeLocationForDalvikCache(const std::string& location) {
  std::string encoded = location.substr(/*pos=*/1);  // Remove the leading '/';
  std::replace(encoded.begin(), encoded.end(), '/', '@');
  return encoded;
}

class ArtdCleanupTest : public ArtdTest {
 protected:
  void SetUpForCleanup() {
    // Unmanaged files.
    CreateGcKeptFile(android_data_ + "/user_de/0/com.android.foo/1.odex");
    CreateGcKeptFile(android_data_ + "/user_de/0/com.android.foo/1.arm64.sdm");
    CreateGcKeptFile(android_data_ + "/user_de/0/com.android.foo/oat/1.odex");
    CreateGcKeptFile(android_data_ + "/user_de/0/com.android.foo/oat/1.txt");
    CreateGcKeptFile(android_data_ + "/user_de/0/com.android.foo/oat/arm64/1.txt");
    CreateGcKeptFile(android_data_ + "/user_de/0/com.android.foo/oat/arm64/1.tmp");
    CreateGcKeptFile(android_data_ + "/user_de/0/com.android.foo/oat/arm64/1.sdc");

    // Files to keep.
    CreateGcKeptFile(android_data_ + "/misc/profiles/cur/1/com.android.foo/primary.prof");
    CreateGcKeptFile(android_data_ + "/misc/profiles/cur/3/com.android.foo/primary.prof");
    CreateGcKeptFile(android_data_ + "/dalvik-cache/arm64/system@app@Foo@Foo.apk@classes.dex");
    CreateGcKeptFile(android_data_ + "/dalvik-cache/arm64/system@app@Foo@Foo.apk@classes.vdex");
    CreateGcKeptFile(android_data_ + "/dalvik-cache/arm64/system@app@Foo@Foo.apk@classes.art");
    CreateGcKeptFile(android_data_ + "/user_de/0/com.android.foo/aaa/oat/arm64/1.vdex");
    CreateGcKeptFile(
        android_expand_ +
        "/123456-7890/app/~~nkfeankfna==/com.android.bar-jfoeaofiew==/oat/arm64/base.odex");
    CreateGcKeptFile(
        android_expand_ +
        "/123456-7890/app/~~nkfeankfna==/com.android.bar-jfoeaofiew==/oat/arm64/base.vdex");
    CreateGcKeptFile(
        android_expand_ +
        "/123456-7890/app/~~nkfeankfna==/com.android.bar-jfoeaofiew==/oat/arm64/base.art");
    CreateGcKeptFile(android_data_ + "/user_de/0/com.android.foo/aaa/oat/arm64/2.odex");
    CreateGcKeptFile(android_data_ + "/user_de/0/com.android.foo/aaa/oat/arm64/2.vdex");
    CreateGcKeptFile(android_data_ + "/user_de/0/com.android.foo/aaa/oat/arm64/2.art");
    CreateGcKeptFile(android_data_ + "/user_de/0/com.android.foo/cache/oat_primary/arm64/base.art");
    CreateGcKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
    CreateGcKeptFile(android_data_ + "/user/1/com.android.foo/cache/oat_primary/arm64/base.art");
    CreateGcKeptFile(android_expand_ +
                     "/123456-7890/user/1/com.android.foo/cache/oat_primary/arm64/base.art");
    CreateGcKeptFile(android_data_ +
                     "/user/0/com.android.foo/cache/not_oat_dir/oat_primary/arm64/base.art");
    CreateGcKeptFile(android_data_ +
                     "/app/~~fadsfgadg==/com.android.baz-fadsfgadg==/base.arm64.sdm");
    CreateGcKeptFile(android_data_ +
                     "/app/~~fadsfgadg==/com.android.baz-fadsfgadg==/oat/arm64/base.sdc");
    CreateGcKeptFile(android_data_ +
                     "/app/~~jhrwafasr==/com.android.qux-bredcweff==/base.arm64.sdm");
    CreateGcKeptFile(android_data_ + "/dalvik-cache/arm64/" +
                     EncodeLocationForDalvikCache(android_data_) +
                     "@app@~~jhrwafasr==@com.android.qux-bredcweff==@base.apk@classes.sdc");

    // Files to remove.
    CreateGcRemovedFile(android_data_ + "/misc/profiles/ref/com.android.foo/primary.prof");
    CreateGcRemovedFile(android_data_ + "/misc/profiles/cur/2/com.android.foo/primary.prof");
    CreateGcRemovedFile(android_data_ + "/misc/profiles/cur/3/com.android.bar/primary.prof");
    CreateGcRemovedFile(android_data_ + "/dalvik-cache/arm64/extra.odex");
    CreateGcRemovedFile(android_data_ + "/dalvik-cache/arm64/system@app@Bar@Bar.apk@classes.dex");
    CreateGcRemovedFile(android_data_ + "/dalvik-cache/arm64/system@app@Bar@Bar.apk@classes.vdex");
    CreateGcRemovedFile(android_data_ + "/dalvik-cache/arm64/system@app@Bar@Bar.apk@classes.art");
    CreateGcRemovedFile(
        android_expand_ +
        "/123456-7890/app/~~daewfweaf==/com.android.foo-fjuwidhia==/oat/arm64/base.odex");
    CreateGcRemovedFile(
        android_expand_ +
        "/123456-7890/app/~~daewfweaf==/com.android.foo-fjuwidhia==/oat/arm64/base.vdex");
    CreateGcRemovedFile(
        android_expand_ +
        "/123456-7890/app/~~daewfweaf==/com.android.foo-fjuwidhia==/oat/arm64/base.art");
    CreateGcRemovedFile(android_data_ + "/user_de/0/com.android.foo/oat/1.prof");
    CreateGcRemovedFile(android_data_ + "/user_de/0/com.android.foo/oat/1.prof.123456.tmp");
    CreateGcRemovedFile(android_data_ + "/user_de/0/com.android.foo/oat/arm64/1.odex");
    CreateGcRemovedFile(android_data_ + "/user_de/0/com.android.foo/oat/arm64/1.vdex");
    CreateGcRemovedFile(android_data_ + "/user_de/0/com.android.foo/oat/arm64/1.art");
    CreateGcRemovedFile(android_data_ + "/user_de/0/com.android.foo/oat/arm64/1.odex.123456.tmp");
    CreateGcRemovedFile(android_data_ + "/user_de/0/com.android.foo/oat/arm64/2.odex.123456.tmp");
    CreateGcRemovedFile(android_data_ + "/user_de/0/com.android.foo/aaa/oat/arm64/1.odex");
    CreateGcRemovedFile(android_data_ + "/user_de/0/com.android.foo/aaa/oat/arm64/1.art");
    CreateGcRemovedFile(android_data_ +
                        "/user_de/0/com.android.foo/aaa/oat/arm64/1.vdex.123456.tmp");
    CreateGcRemovedFile(android_data_ + "/user_de/0/com.android.foo/aaa/bbb/oat/arm64/1.odex");
    CreateGcRemovedFile(android_data_ + "/user_de/0/com.android.foo/aaa/bbb/oat/arm64/1.vdex");
    CreateGcRemovedFile(android_data_ + "/user_de/0/com.android.foo/aaa/bbb/oat/arm64/1.art");
    CreateGcRemovedFile(android_data_ +
                        "/user_de/0/com.android.foo/aaa/bbb/oat/arm64/1.art.123456.tmp");
    CreateGcRemovedFile(android_data_ + "/user_de/0/com.android.bar/aaa/oat/arm64/1.vdex");
    CreateGcRemovedFile(android_data_ +
                        "/user/0/com.android.different_package/cache/oat_primary/arm64/base.art");
    CreateGcRemovedFile(android_data_ +
                        "/user/0/com.android.foo/cache/oat_primary/arm64/different_dex.art");
    CreateGcRemovedFile(android_data_ +
                        "/user/0/com.android.foo/cache/oat_primary/different_isa/base.art");
    CreateGcRemovedFile(android_data_ +
                        "/app/~~fadsfgadg==/com.android.baz-fadsfgadg==/different_dex.arm64.sdm");
    CreateGcRemovedFile(
        android_data_ +
        "/app/~~fadsfgadg==/com.android.baz-fadsfgadg==/oat/arm64/different_dex.sdc");
    CreateGcRemovedFile(android_data_ +
                        "/app/~~fadsfgadg==/com.android.baz-fadsfgadg==/base.different_isa.sdm");
    CreateGcRemovedFile(
        android_data_ +
        "/app/~~fadsfgadg==/com.android.baz-fadsfgadg==/oat/different_isa/base.sdc");
    CreateGcRemovedFile(android_data_ +
                        "/app/~~jhrwafasr==/com.android.qux-bredcweff==/different_dex.arm64.sdm");
    CreateGcRemovedFile(
        android_data_ + "/dalvik-cache/arm64/" + EncodeLocationForDalvikCache(android_data_) +
        "@app@~~jhrwafasr==@com.android.qux-bredcweff==@different_dex.apk@classes.sdc");
    CreateGcRemovedFile(android_data_ +
                        "/app/~~jhrwafasr==/com.android.qux-bredcweff==/base.different_isa.sdm");
    CreateGcRemovedFile(android_data_ + "/dalvik-cache/different_isa/" +
                        EncodeLocationForDalvikCache(android_data_) +
                        "@app@~~jhrwafasr==@com.android.qux-bredcweff==@base.apk@classes.sdc");
  }

  void CreateGcRemovedFile(const std::string& path) {
    CreateFile(path);
    gc_removed_files_.push_back(path);
  }

  void CreateGcKeptFile(const std::string& path) {
    CreateFile(path);
    gc_kept_files_.push_back(path);
  }

  void RunCleanup(bool keepPreRebootStagedFiles) {
    int64_t aidl_return;
    ASSERT_STATUS_OK(artd_->cleanup(
        {
            PrimaryCurProfilePath{
                .userId = 1, .packageName = "com.android.foo", .profileName = "primary"},
            PrimaryCurProfilePath{
                .userId = 3, .packageName = "com.android.foo", .profileName = "primary"},
        },
        {
            ArtifactsPath{
                .dexPath = "/system/app/Foo/Foo.apk", .isa = "arm64", .isInDalvikCache = true},
            ArtifactsPath{
                .dexPath = android_expand_ +
                           "/123456-7890/app/~~nkfeankfna==/com.android.bar-jfoeaofiew==/base.apk",
                .isa = "arm64",
                .isInDalvikCache = false},
            ArtifactsPath{.dexPath = android_data_ + "/user_de/0/com.android.foo/aaa/2.apk",
                          .isa = "arm64",
                          .isInDalvikCache = false},
        },
        {
            VdexPath{
                ArtifactsPath{.dexPath = android_data_ + "/user_de/0/com.android.foo/aaa/1.apk",
                              .isa = "arm64",
                              .isInDalvikCache = false}},
        },
        {
            SecureDexMetadataWithCompanionPaths{
                .dexPath =
                    android_data_ + "/app/~~fadsfgadg==/com.android.baz-fadsfgadg==/base.apk",
                .isa = "arm64",
                .isInDalvikCache = false},
            SecureDexMetadataWithCompanionPaths{
                .dexPath =
                    android_data_ + "/app/~~jhrwafasr==/com.android.qux-bredcweff==/base.apk",
                .isa = "arm64",
                .isInDalvikCache = true},
        },
        {
            RuntimeArtifactsPath{
                .packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "arm64"},
        },
        keepPreRebootStagedFiles,
        &aidl_return));
  }

  void Verify() {
    for (const std::string& path : gc_removed_files_) {
      EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
    }

    for (const std::string& path : gc_kept_files_) {
      EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
    }
  }

 private:
  std::vector<std::string> gc_removed_files_;
  std::vector<std::string> gc_kept_files_;
};

TEST_F(ArtdCleanupTest, cleanupKeepingPreRebootStagedFiles) {
  SetUpForCleanup();
  CreateGcKeptFile(
      android_expand_ +
      "/123456-7890/app/~~nkfeankfna==/com.android.bar-jfoeaofiew==/oat/arm64/base.odex.staged");
  CreateGcKeptFile(android_data_ + "/user_de/0/com.android.foo/aaa/oat/arm64/2.odex.staged");

  ASSERT_NO_FATAL_FAILURE(RunCleanup(/*keepPreRebootStagedFiles=*/true));
  Verify();
}

TEST_F(ArtdCleanupTest, cleanupRemovingPreRebootStagedFiles) {
  SetUpForCleanup();
  CreateGcRemovedFile(
      android_expand_ +
      "/123456-7890/app/~~nkfeankfna==/com.android.bar-jfoeaofiew==/oat/arm64/base.odex.staged");
  CreateGcRemovedFile(android_data_ + "/user_de/0/com.android.foo/aaa/oat/arm64/2.odex.staged");

  ASSERT_NO_FATAL_FAILURE(RunCleanup(/*keepPreRebootStagedFiles=*/false));
  Verify();
}

TEST_F(ArtdCleanupTest, cleanUpPreRebootStagedFiles) {
  // Unmanaged file.
  CreateGcKeptFile(android_data_ + "/user_de/0/com.android.foo/1.odex.staged");

  // Not Pre-reboot staged files.
  CreateGcKeptFile(android_data_ + "/misc/profiles/ref/com.android.foo/primary.prof");
  CreateGcKeptFile(
      android_expand_ +
      "/123456-7890/app/~~nkfeankfna==/com.android.bar-jfoeaofiew==/oat/arm64/base.odex");
  CreateGcKeptFile(android_data_ + "/user_de/0/com.android.foo/aaa/oat/arm64/2.odex");

  // Pre-reboot staged files.
  CreateGcRemovedFile(android_data_ + "/misc/profiles/ref/com.android.foo/primary.prof.staged");
  CreateGcRemovedFile(
      android_expand_ +
      "/123456-7890/app/~~nkfeankfna==/com.android.bar-jfoeaofiew==/oat/arm64/base.odex.staged");
  CreateGcRemovedFile(android_data_ + "/user_de/0/com.android.foo/aaa/oat/arm64/2.odex.staged");

  ASSERT_STATUS_OK(artd_->cleanUpPreRebootStagedFiles());
  Verify();
}

TEST_F(ArtdTest, isInDalvikCache) {
  TEST_DISABLED_FOR_HOST();

  auto is_in_dalvik_cache = [this](const std::string& dex_file) -> Result<bool> {
    bool result;
    ndk::ScopedAStatus status = artd_->isInDalvikCache(dex_file, &result);
    if (!status.isOk()) {
      return Error() << status.getMessage();
    }
    return result;
  };

  EXPECT_THAT(is_in_dalvik_cache("/system/app/base.apk"), HasValue(true));
  EXPECT_THAT(is_in_dalvik_cache("/system_ext/app/base.apk"), HasValue(true));
  EXPECT_THAT(is_in_dalvik_cache("/vendor/app/base.apk"), HasValue(true));
  EXPECT_THAT(is_in_dalvik_cache("/product/app/base.apk"), HasValue(true));
  EXPECT_THAT(is_in_dalvik_cache("/data/app/base.apk"), HasValue(false));

  // Test a path where we don't expect to find packages. The method should still work.
  EXPECT_THAT(is_in_dalvik_cache("/foo"), HasValue(true));
}

TEST_F(ArtdTest, deleteSdmSdcFiles) {
  CreateFile(scratch_path_ + "/a/b.arm64.sdm", "**");     // 2 bytes.
  CreateFile(scratch_path_ + "/a/oat/arm64/b.sdc", "*");  // 1 byte.

  int64_t result = -1;
  ASSERT_STATUS_OK(artd_->deleteSdmSdcFiles(
      {.dexPath = scratch_path_ + "/a/b.apk", .isa = "arm64", .isInDalvikCache = false}, &result));
  EXPECT_EQ(result, 2 + 1);

  EXPECT_FALSE(std::filesystem::exists(scratch_path_ + "/a/b.arm64.sdm"));
  EXPECT_FALSE(std::filesystem::exists(scratch_path_ + "/a/oat/arm64/b.sdc"));
}

TEST_F(ArtdTest, deleteRuntimeArtifacts) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;

  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };

  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };

  CreateKeptFile(android_data_ +
                 "/user/0/com.android.different_package/cache/oat_primary/arm64/base.art");
  CreateKeptFile(android_data_ +
                 "/user/0/com.android.foo/cache/oat_primary/arm64/different_dex.art");
  CreateKeptFile(android_data_ +
                 "/user/0/com.android.foo/cache/oat_primary/different_isa/base.art");
  CreateKeptFile(android_data_ +
                 "/user/0/com.android.foo/cache/not_oat_dir/oat_primary/arm64/base.art");

  CreateRemovedFile(android_data_ + "/user_de/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/1/com.android.foo/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_expand_ +
                    "/123456-7890/user/1/com.android.foo/cache/oat_primary/arm64/base.art");

  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts(
              {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "arm64"},
              &aidl_return)
          .isOk());

  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }

  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}

TEST_F(ArtdTest, deleteRuntimeArtifactsAndroidDataNotExist) {
  // Will be cleaned up by `android_data_env_`
  setenv("ANDROID_DATA", "/non-existing", /*replace=*/1);

  auto scoped_set_logger = ScopedSetLogger(mock_logger_.AsStdFunction());
  EXPECT_CALL(mock_logger_,
              Call(_, _, _, _, _, HasSubstr("Failed to find directory /non-existing")));

  int64_t aidl_return;
  ASSERT_TRUE(
      artd_
          ->deleteRuntimeArtifacts(
              {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "arm64"},
              &aidl_return)
          .isOk());

  EXPECT_EQ(aidl_return, 0);
}

// Verifies that `deleteRuntimeArtifacts` doesn't treat "*" as a wildcard. It should either treat it
// as a normal character in the path or reject it. The caller is never supposed to use a wildcard.
TEST_F(ArtdTest, deleteRuntimeArtifactsSpecialChars) {
  std::vector<std::string> removed_files;
  std::vector<std::string> kept_files;

  auto CreateRemovedFile = [&](const std::string& path) {
    CreateFile(path);
    removed_files.push_back(path);
  };

  auto CreateKeptFile = [&](const std::string& path) {
    CreateFile(path);
    kept_files.push_back(path);
  };

  CreateKeptFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art");

  CreateRemovedFile(android_data_ + "/user/0/*/cache/oat_primary/arm64/base.art");
  CreateRemovedFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/*.art");

  int64_t aidl_return;
  ASSERT_STATUS_OK(artd_->deleteRuntimeArtifacts(
      {.packageName = "*", .dexPath = "/a/b/base.apk", .isa = "arm64"}, &aidl_return));
  ASSERT_STATUS_OK(artd_->deleteRuntimeArtifacts(
      {.packageName = "com.android.foo", .dexPath = "/a/b/*.apk", .isa = "arm64"}, &aidl_return));
  ASSERT_FALSE(artd_
                   ->deleteRuntimeArtifacts(
                       {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "*"},
                       &aidl_return)
                   .isOk());

  for (const std::string& path : removed_files) {
    EXPECT_FALSE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be removed", path);
  }

  for (const std::string& path : kept_files) {
    EXPECT_TRUE(std::filesystem::exists(path)) << ART_FORMAT("'{}' should be kept", path);
  }
}

TEST_F(ArtdTest, getArtifactsSize) {
  std::string oat_dir = scratch_path_ + "/a/oat/arm64";
  CreateFile(oat_dir + "/b.odex", std::string(1, '*'));
  CreateFile(oat_dir + "/b.vdex", std::string(2, '*'));
  CreateFile(oat_dir + "/b.art", std::string(4, '*'));

  // Irrelevant.
  CreateFile(oat_dir + "/c.vdex", std::string(8, '*'));

  int64_t aidl_return = -1;
  ASSERT_TRUE(
      artd_
          ->getArtifactsSize(
              {.dexPath = scratch_path_ + "/a/b.apk", .isa = "arm64", .isInDalvikCache = false},
              &aidl_return)
          .isOk());
  EXPECT_EQ(aidl_return, 1 + 2 + 4);
}

TEST_F(ArtdTest, getVdexFileSize) {
  std::string oat_dir = scratch_path_ + "/a/oat/arm64";
  CreateFile(oat_dir + "/b.vdex", std::string(1, '*'));

  // Irrelevant.
  CreateFile(oat_dir + "/b.odex", std::string(2, '*'));
  CreateFile(oat_dir + "/b.art", std::string(4, '*'));
  CreateFile(oat_dir + "/c.vdex", std::string(8, '*'));

  int64_t aidl_return = -1;
  ASSERT_TRUE(artd_
                  ->getVdexFileSize(ArtifactsPath{.dexPath = scratch_path_ + "/a/b.apk",
                                                  .isa = "arm64",
                                                  .isInDalvikCache = false},
                                    &aidl_return)
                  .isOk());
  EXPECT_EQ(aidl_return, 1);
}

TEST_F(ArtdTest, getSdmFileSize) {
  CreateFile(scratch_path_ + "/a/b.arm64.sdm", std::string(1, '*'));

  int64_t aidl_return = -1;
  ASSERT_TRUE(
      artd_
          ->getSdmFileSize(
              {.dexPath = scratch_path_ + "/a/b.apk", .isa = "arm64", .isInDalvikCache = false},
              &aidl_return)
          .isOk());
  EXPECT_EQ(aidl_return, 1);
}

TEST_F(ArtdTest, getRuntimeArtifactsSize) {
  CreateFile(android_data_ + "/user_de/0/com.android.foo/cache/oat_primary/arm64/base.art",
             std::string(1, '*'));
  CreateFile(android_data_ + "/user/0/com.android.foo/cache/oat_primary/arm64/base.art",
             std::string(2, '*'));
  CreateFile(android_data_ + "/user/1/com.android.foo/cache/oat_primary/arm64/base.art",
             std::string(4, '*'));
  CreateFile(
      android_expand_ + "/123456-7890/user/1/com.android.foo/cache/oat_primary/arm64/base.art",
      std::string(8, '*'));

  // Irrelevant.
  CreateFile(android_expand_ + "/user/0/com.android.foo/cache/oat_primary/arm64/different_dex.art",
             std::string(16, '*'));

  int64_t aidl_return = -1;
  ASSERT_TRUE(
      artd_
          ->getRuntimeArtifactsSize(
              {.packageName = "com.android.foo", .dexPath = "/a/b/base.apk", .isa = "arm64"},
              &aidl_return)
          .isOk());

  EXPECT_EQ(aidl_return, 1 + 2 + 4 + 8);
}

TEST_F(ArtdTest, getProfileSize) {
  CreateFile(android_data_ + "/misc/profiles/cur/0/com.android.foo/primary.prof",
             std::string(1, '*'));

  // Irrelevant.
  CreateFile(android_data_ + "/misc/profiles/cur/0/com.android.foo/split_0.split.prof",
             std::string(2, '*'));
  CreateFile(android_data_ + "/misc/profiles/cur/0/com.android.bar/primary.prof",
             std::string(4, '*'));
  CreateFile(android_data_ + "/misc/profiles/ref/com.android.foo/primary.prof",
             std::string(8, '*'));

  int64_t aidl_return = -1;
  ASSERT_TRUE(artd_
                  ->getProfileSize(
                      PrimaryCurProfilePath{
                          .userId = 0, .packageName = "com.android.foo", .profileName = "primary"},
                      &aidl_return)
                  .isOk());
  EXPECT_EQ(aidl_return, 1);
}

class ArtdProfileSaveNotificationTest : public ArtdTest {
 protected:
  void SetUp() override {
    ArtdTest::SetUp();

    std::vector<std::string> args{GetBin("sleep"), "10"};
    std::tie(pid_, scope_guard_) = ScopedExec(args, /*wait=*/false);
    notification_file_ = OR_FAIL(BuildPrimaryCurProfilePath(profile_path_));
    std::filesystem::create_directories(Dirname(notification_file_));
  }

  const PrimaryCurProfilePath profile_path_{
      .userId = 0,
      .packageName = "com.android.foo",
      .profileName = "primary",
  };
  std::string notification_file_;
  int pid_;
  std::unique_ptr<ScopeGuard<std::function<void()>>> scope_guard_;
};

TEST_F(ArtdProfileSaveNotificationTest, initAndWaitSuccess) {
  // Use a condvar to sequence the NewFile::CommitOrAbandon calls.
  constexpr std::chrono::duration<int> kTimeout = std::chrono::seconds(1);
  std::condition_variable wait_started_cv;
  std::mutex mu;

  EXPECT_CALL(mock_poll_, Call)
      .Times(2)
      .WillRepeatedly(DoAll(
          [&](auto, auto, auto) {
            // Step 3, 5.
            std::unique_lock<std::mutex> lock(mu);
            wait_started_cv.notify_one();
          },
          poll));

  std::shared_ptr<IArtdNotification> notification;
  ASSERT_STATUS_OK(artd_->initProfileSaveNotification(profile_path_, pid_, &notification));

  std::unique_lock<std::mutex> lock(mu);

  // Step 1.
  std::thread t([&] {
    // Step 2.
    bool aidl_return;
    ASSERT_STATUS_OK(notification->wait(/*in_timeoutMs=*/1000, &aidl_return));
    // Step 7.
    EXPECT_TRUE(aidl_return);
  });
  wait_started_cv.wait_for(lock, kTimeout);

  // Step 4.
  std::unique_ptr<NewFile> unrelated_file = OR_FAIL(NewFile::Create(
      Dirname(notification_file_) + "/unrelated.prof", FsPermission{.uid = -1, .gid = -1}));
  OR_FAIL(unrelated_file->CommitOrAbandon());
  wait_started_cv.wait_for(lock, kTimeout);

  // Step 6.
  std::unique_ptr<NewFile> file =
      OR_FAIL(NewFile::Create(notification_file_, FsPermission{.uid = -1, .gid = -1}));
  OR_FAIL(file->CommitOrAbandon());

  t.join();
}

TEST_F(ArtdProfileSaveNotificationTest, initAndWaitProcessGone) {
  EXPECT_CALL(mock_poll_, Call).WillOnce(poll);

  std::shared_ptr<IArtdNotification> notification;
  ASSERT_STATUS_OK(artd_->initProfileSaveNotification(profile_path_, pid_, &notification));

  std::thread t([&] {
    bool aidl_return;
    ASSERT_STATUS_OK(notification->wait(/*in_timeoutMs=*/1000, &aidl_return));
    EXPECT_TRUE(aidl_return);
  });

  kill(pid_, SIGKILL);

  t.join();
}

TEST_F(ArtdProfileSaveNotificationTest, initAndWaitTimeout) {
  EXPECT_CALL(mock_poll_, Call).WillOnce(poll).WillOnce(Return(0));

  std::shared_ptr<IArtdNotification> notification;
  ASSERT_STATUS_OK(artd_->initProfileSaveNotification(profile_path_, pid_, &notification));

  std::unique_ptr<NewFile> unrelated_file = OR_FAIL(NewFile::Create(
      Dirname(notification_file_) + "/unrelated.prof", FsPermission{.uid = -1, .gid = -1}));
  OR_FAIL(unrelated_file->CommitOrAbandon());

  bool aidl_return;
  ASSERT_STATUS_OK(notification->wait(/*in_timeoutMs=*/1000, &aidl_return));
  EXPECT_FALSE(aidl_return);
}

TEST_F(ArtdProfileSaveNotificationTest, initProcessGone) {
  // Kill the process before pidfd_open.
  scope_guard_.reset();

  EXPECT_CALL(mock_poll_, Call).Times(0);

  std::shared_ptr<IArtdNotification> notification;
  ASSERT_STATUS_OK(artd_->initProfileSaveNotification(profile_path_, pid_, &notification));

  bool aidl_return;
  ASSERT_STATUS_OK(notification->wait(/*in_timeoutMs=*/1000, &aidl_return));
  EXPECT_TRUE(aidl_return);
}

TEST_F(ArtdTest, commitPreRebootStagedFiles) {
  CreateFile(android_data_ + "/dalvik-cache/arm64/system@app@Foo@Foo.apk@classes.dex.staged",
             "new_odex_1");
  CreateFile(android_data_ + "/dalvik-cache/arm64/system@app@Foo@Foo.apk@classes.vdex.staged",
             "new_vdex_1");
  CreateFile(android_data_ + "/dalvik-cache/arm64/system@app@Foo@Foo.apk@classes.art.staged",
             "new_art_1");

  CreateFile(android_data_ + "/dalvik-cache/arm64/system@app@Foo@Foo.apk@classes.dex",
             "old_odex_1");
  CreateFile(android_data_ + "/dalvik-cache/arm64/system@app@Foo@Foo.apk@classes.vdex",
             "old_vdex_1");
  CreateFile(android_data_ + "/dalvik-cache/arm64/system@app@Foo@Foo.apk@classes.art", "old_art_1");

  CreateFile(android_data_ + "/app/com.android.foo/oat/arm64/base.odex", "old_odex_2");
  CreateFile(android_data_ + "/app/com.android.foo/oat/arm64/base.vdex", "old_vdex_2");
  CreateFile(android_data_ + "/app/com.android.foo/oat/arm64/base.art", "old_art_2");

  CreateFile(android_data_ + "/app/com.android.foo/oat/arm64/base.odex.staged", "new_odex_2");
  CreateFile(android_data_ + "/app/com.android.foo/oat/arm64/base.vdex.staged", "new_vdex_2");

  CreateFile(android_data_ + "/app/com.android.foo/oat/arm/base.odex", "old_odex_3");
  CreateFile(android_data_ + "/app/com.android.foo/oat/arm/base.vdex", "old_vdex_3");
  CreateFile(android_data_ + "/app/com.android.foo/oat/arm/base.art", "old_art_3");

  CreateFile(android_data_ + "/misc/profiles/ref/com.android.foo/primary.prof", "old_prof_1");
  CreateFile(android_data_ + "/misc/profiles/ref/com.android.foo/primary.prof.staged",
             "new_prof_1");

  CreateFile(android_data_ + "/misc/profiles/ref/com.android.bar/primary.prof", "old_prof_2");

  bool aidl_return;
  ASSERT_STATUS_OK(artd_->commitPreRebootStagedFiles(
      {
          // Has all new files. All old files should be replaced.
          ArtifactsPath{
              .dexPath = "/system/app/Foo/Foo.apk", .isa = "arm64", .isInDalvikCache = true},
          // Has new files but not ".art" file. Old ".odex" and ".vdex" files should be replaced,
          // and old ".art" file should be removed.
          ArtifactsPath{.dexPath = android_data_ + "/app/com.android.foo/base.apk",
                        .isa = "arm64",
                        .isInDalvikCache = false},
          // Has no new file. All old files should be kept.
          ArtifactsPath{.dexPath = android_data_ + "/app/com.android.foo/base.apk",
                        .isa = "arm",
                        .isInDalvikCache = false},
      },
      {
          // Has new file.
          PrimaryRefProfilePath{.packageName = "com.android.foo", .profileName = "primary"},
          // Has no new file.
          PrimaryRefProfilePath{.packageName = "com.android.bar", .profileName = "primary"},
      },
      &aidl_return));
  EXPECT_TRUE(aidl_return);

  CheckContent(android_data_ + "/dalvik-cache/arm64/system@app@Foo@Foo.apk@classes.dex",
               "new_odex_1");
  CheckContent(android_data_ + "/dalvik-cache/arm64/system@app@Foo@Foo.apk@classes.vdex",
               "new_vdex_1");
  CheckContent(android_data_ + "/dalvik-cache/arm64/system@app@Foo@Foo.apk@classes.art",
               "new_art_1");

  CreateFile(android_data_ + "/app/com.android.foo/oat/arm64/base.odex", "new_odex_2");
  CreateFile(android_data_ + "/app/com.android.foo/oat/arm64/base.vdex", "new_vdex_2");
  EXPECT_FALSE(std::filesystem::exists(android_data_ + "/app/com.android.foo/oat/arm64/base.art"));

  CheckContent(android_data_ + "/app/com.android.foo/oat/arm/base.odex", "old_odex_3");
  CheckContent(android_data_ + "/app/com.android.foo/oat/arm/base.vdex", "old_vdex_3");
  CheckContent(android_data_ + "/app/com.android.foo/oat/arm/base.art", "old_art_3");

  CheckContent(android_data_ + "/misc/profiles/ref/com.android.foo/primary.prof", "new_prof_1");

  CheckContent(android_data_ + "/misc/profiles/ref/com.android.bar/primary.prof", "old_prof_2");

  // All staged files are gone.
  EXPECT_FALSE(std::filesystem::exists(
      android_data_ + "/dalvik-cache/arm64/system@app@Foo@Foo.apk@classes.dex.staged"));
  EXPECT_FALSE(std::filesystem::exists(
      android_data_ + "/dalvik-cache/arm64/system@app@Foo@Foo.apk@classes.vdex.staged"));
  EXPECT_FALSE(std::filesystem::exists(
      android_data_ + "/dalvik-cache/arm64/system@app@Foo@Foo.apk@classes.art.staged"));
  EXPECT_FALSE(
      std::filesystem::exists(android_data_ + "/app/com.android.foo/oat/arm64/base.odex.staged"));
  EXPECT_FALSE(
      std::filesystem::exists(android_data_ + "/app/com.android.foo/oat/arm64/base.vdex.staged"));
  EXPECT_FALSE(std::filesystem::exists(android_data_ +
                                       "/misc/profiles/ref/com.android.foo/primary.prof.staged"));
}

TEST_F(ArtdTest, commitPreRebootStagedFilesNoNewFile) {
  bool aidl_return;
  ASSERT_STATUS_OK(artd_->commitPreRebootStagedFiles(
      {
          ArtifactsPath{.dexPath = android_data_ + "/app/com.android.foo/base.apk",
                        .isa = "arm",
                        .isInDalvikCache = false},
      },
      {},
      &aidl_return));
  EXPECT_FALSE(aidl_return);
}

TEST_F(ArtdTest, checkPreRebootSystemRequirements) {
  EXPECT_CALL(*mock_props_, GetProperty("ro.build.version.release")).WillRepeatedly(Return("15"));
  std::string chroot_dir = scratch_path_ + "/chroot";
  bool aidl_return;

  constexpr const char* kTemplate = R"(
    # Comment.
    unrelated.system.property=abc

    ro.build.version.release={}
  )";

  CreateFile(chroot_dir + "/system/build.prop", ART_FORMAT(kTemplate, 15));
  ASSERT_STATUS_OK(artd_->checkPreRebootSystemRequirements(chroot_dir, &aidl_return));
  EXPECT_TRUE(aidl_return);

  CreateFile(chroot_dir + "/system/build.prop", ART_FORMAT(kTemplate, 16));
  ASSERT_STATUS_OK(artd_->checkPreRebootSystemRequirements(chroot_dir, &aidl_return));
  EXPECT_TRUE(aidl_return);

  CreateFile(chroot_dir + "/system/build.prop", ART_FORMAT(kTemplate, 17));
  ASSERT_STATUS_OK(artd_->checkPreRebootSystemRequirements(chroot_dir, &aidl_return));
  EXPECT_FALSE(aidl_return);
}

TEST_F(ArtdTest, BuildSystemProperties) {
  constexpr const char* kContent = R"(
    # Comment.
    property.foo=123
    property.foo?=456
    property.bar?=000
    property.bar=789
    property.baz?=111
    import /vendor/my_import.prop ro.*
    import=222
  )";

  CreateFile(scratch_path_ + "/build.prop", kContent);
  BuildSystemProperties props =
      OR_FAIL(BuildSystemProperties::Create(scratch_path_ + "/build.prop"));
  EXPECT_EQ(props.GetOrEmpty("property.foo"), "123");
  EXPECT_EQ(props.GetOrEmpty("property.bar"), "789");
  EXPECT_EQ(props.GetOrEmpty("property.baz"), "111");
  EXPECT_EQ(props.GetOrEmpty("import"), "222");
}

class ArtdPreRebootTest : public ArtdTest {
 protected:
  void SetUp() override {
    ArtdTest::SetUp();

    pre_reboot_tmp_dir_ = scratch_path_ + "/artd_tmp";
    std::filesystem::create_directories(pre_reboot_tmp_dir_);
    init_environ_rc_path_ = scratch_path_ + "/init.environ.rc";

    auto mock_props = std::make_unique<NiceMock<MockSystemProperties>>();
    mock_props_ = mock_props.get();
    ON_CALL(*mock_props_, GetProperty).WillByDefault(Return(""));
    auto mock_exec_utils = std::make_unique<MockExecUtils>();
    mock_exec_utils_ = mock_exec_utils.get();
    auto mock_pre_reboot_build_props = std::make_unique<NiceMock<MockSystemProperties>>();
    mock_pre_reboot_build_props_ = mock_pre_reboot_build_props.get();

    ON_CALL(*mock_pre_reboot_build_props_, GetProperty).WillByDefault(Return(""));
    ON_CALL(*mock_pre_reboot_build_props_, GetProperty("ro.build.version.sdk"))
        .WillByDefault(Return("35"));
    ON_CALL(*mock_pre_reboot_build_props_, GetProperty("ro.build.version.codename"))
        .WillByDefault(Return("Baklava"));
    ON_CALL(*mock_pre_reboot_build_props_, GetProperty("ro.build.version.known_codenames"))
        .WillByDefault(Return("VanillaIceCream,Baklava"));

    artd_ = ndk::SharedRefBase::make<Artd>(Options{.is_pre_reboot = true},
                                           std::move(mock_props),
                                           std::move(mock_exec_utils),
                                           mock_kill_.AsStdFunction(),
                                           mock_fstat_.AsStdFunction(),
                                           mock_poll_.AsStdFunction(),
                                           mock_mount_.AsStdFunction(),
                                           mock_restorecon_.AsStdFunction(),
                                           pre_reboot_tmp_dir_,
                                           init_environ_rc_path_,
                                           std::move(mock_pre_reboot_build_props));

    ON_CALL(mock_restorecon_, Call).WillByDefault(Return(Result<void>()));

    constexpr const char* kInitEnvironRcTmpl = R"(
      on early-init
          export ANDROID_ART_ROOT {}
          export ANDROID_DATA {}
    )";
    ASSERT_TRUE(WriteStringToFile(ART_FORMAT(kInitEnvironRcTmpl, art_root_, android_data_),
                                  init_environ_rc_path_));

    tmp_profile_path_.finalPath.get<WritableProfilePath::forPrimary>().isPreReboot = true;
    output_artifacts_.artifactsPath.isPreReboot = true;
  }

  std::string pre_reboot_tmp_dir_;
  std::string init_environ_rc_path_;
  MockFunction<int(const char*, const char*, const char*, uint32_t, const void*)> mock_mount_;
  MockFunction<Result<void>(const std::string&,
                            const std::optional<OutputArtifacts::PermissionSettings::SeContext>&,
                            bool)>
      mock_restorecon_;
  MockSystemProperties* mock_pre_reboot_build_props_;
};

TEST_F(ArtdPreRebootTest, preRebootInit) {
  // Color the env vars to make sure that the expected values are not from the parent process but
  // from "/init.environ.rc".
  ASSERT_EQ(setenv("ANDROID_ART_ROOT", "old_value", /*replace=*/1), 0);
  ASSERT_EQ(setenv("ANDROID_DATA", "old_value", /*replace=*/1), 0);
  ASSERT_EQ(setenv("BOOTCLASSPATH", "old_value", /*replace=*/1), 0);

  // Add an env var that doesn't get overridden, to check that it gets removed.
  ASSERT_EQ(setenv("FOO", "old_value", /*replace=*/1), 0);

  InSequence seq;

  EXPECT_CALL(*mock_exec_utils_,
              DoExecAndReturnCode(
                  AllOf(WhenSplitBy("--",
                                    AllOf(Contains(art_root_ + "/bin/art_exec"),
                                          Contains("--drop-capabilities")),
                                    AllOf(Contains("/apex/com.android.sdkext/bin/derive_classpath"),
                                          Contains(Flag("--override-device-sdk-version=", "35")),
                                          Contains(Flag("--override-device-codename=", "Baklava")),
                                          Contains(Flag("--override-device-known-codenames=",
                                                        "VanillaIceCream,Baklava")))),
                        HasKeepFdsFor("/proc/self/fd/")),
                  _,
                  _))
      .WillOnce(DoAll(WithArg<0>(WriteToFdFlag("/proc/self/fd/", "export BOOTCLASSPATH /foo:/bar")),
                      Return(0)));

  EXPECT_CALL(mock_mount_,
              Call(StrEq(pre_reboot_tmp_dir_ + "/art_apex_data"),
                   StrEq("/data/misc/apexdata/com.android.art"),
                   /*fs_type=*/nullptr,
                   MS_BIND | MS_PRIVATE,
                   /*data=*/nullptr))
      .WillOnce(Return(0));

  EXPECT_CALL(mock_mount_,
              Call(StrEq(pre_reboot_tmp_dir_ + "/odrefresh"),
                   StrEq("/data/misc/odrefresh"),
                   /*fs_type=*/nullptr,
                   MS_BIND | MS_PRIVATE,
                   /*data=*/nullptr))
      .WillOnce(Return(0));

  EXPECT_CALL(*mock_exec_utils_,
              DoExecAndReturnCode(WhenSplitBy("--",
                                              AllOf(Contains(art_root_ + "/bin/art_exec"),
                                                    Contains("--drop-capabilities")),
                                              AllOf(Contains(art_root_ + "/bin/odrefresh"),
                                                    Contains("--only-boot-images"),
                                                    Contains("--compile"))),
                                  _,
                                  _))
      .WillOnce(Return(0));

  std::shared_ptr<IArtdCancellationSignal> cancellation_signal;
  ASSERT_STATUS_OK(artd_->createCancellationSignal(&cancellation_signal));

  bool aidl_return;
  ASSERT_STATUS_OK(artd_->preRebootInit(cancellation_signal, &aidl_return));
  EXPECT_TRUE(aidl_return);

  auto env_var_count = []() {
    int count = 0;
    for (char** it = environ; *it != nullptr; it++) {
      count++;
    }
    return count;
  };

  EXPECT_EQ(getenv("ANDROID_ART_ROOT"), art_root_);
  EXPECT_EQ(getenv("ANDROID_DATA"), android_data_);
  EXPECT_STREQ(getenv("BOOTCLASSPATH"), "/foo:/bar");
  EXPECT_EQ(env_var_count(), 3);
  EXPECT_TRUE(std::filesystem::exists(pre_reboot_tmp_dir_ + "/preparation_done"));

  // Color the env vars again to simulate that artd died and restarted.
  ASSERT_EQ(setenv("ANDROID_ART_ROOT", "old_value", /*replace=*/1), 0);
  ASSERT_EQ(setenv("ANDROID_DATA", "old_value", /*replace=*/1), 0);
  ASSERT_EQ(setenv("BOOTCLASSPATH", "old_value", /*replace=*/1), 0);

  // Calling again will not involve `mount`, `derive_classpath`, or `odrefresh` but only restore env
  // vars.
  ASSERT_STATUS_OK(artd_->preRebootInit(/*in_cancellationSignal=*/nullptr, &aidl_return));
  EXPECT_TRUE(aidl_return);
  EXPECT_EQ(getenv("ANDROID_ART_ROOT"), art_root_);
  EXPECT_EQ(getenv("ANDROID_DATA"), android_data_);
  EXPECT_STREQ(getenv("BOOTCLASSPATH"), "/foo:/bar");
  EXPECT_EQ(env_var_count(), 3);
}

TEST_F(ArtdPreRebootTest, preRebootInitFailed) {
  EXPECT_CALL(*mock_exec_utils_,
              DoExecAndReturnCode(Contains("/apex/com.android.sdkext/bin/derive_classpath"), _, _))
      .WillOnce(DoAll(WithArg<0>(WriteToFdFlag("/proc/self/fd/", "export BOOTCLASSPATH /foo:/bar")),
                      Return(0)));

  EXPECT_CALL(mock_mount_, Call).Times(2).WillRepeatedly(Return(0));

  EXPECT_CALL(*mock_exec_utils_, DoExecAndReturnCode(Contains(art_root_ + "/bin/odrefresh"), _, _))
      .WillOnce(Return(1));

  std::shared_ptr<IArtdCancellationSignal> cancellation_signal;
  ASSERT_STATUS_OK(artd_->createCancellationSignal(&cancellation_signal));

  bool aidl_return;
  ndk::ScopedAStatus status = artd_->preRebootInit(cancellation_signal, &aidl_return);
  EXPECT_FALSE(status.isOk());
  EXPECT_EQ(status.getExceptionCode(), EX_SERVICE_SPECIFIC);
  EXPECT_STREQ(status.getMessage(), "odrefresh returned an unexpected code: 1");
}

TEST_F(ArtdPreRebootTest, preRebootInitNoRetry) {
  // Simulate that a previous attempt failed halfway.
  ASSERT_TRUE(WriteStringToFile("", pre_reboot_tmp_dir_ + "/classpath.txt"));

  bool aidl_return;
  ndk::ScopedAStatus status = artd_->preRebootInit(/*in_cancellationSignal=*/nullptr, &aidl_return);
  EXPECT_FALSE(status.isOk());
  EXPECT_EQ(status.getExceptionCode(), EX_ILLEGAL_STATE);
  EXPECT_STREQ(
      status.getMessage(),
      "preRebootInit must not be concurrently called or retried after cancellation or failure");
}

TEST_F(ArtdPreRebootTest, preRebootInitCancelled) {
  EXPECT_CALL(*mock_exec_utils_,
              DoExecAndReturnCode(Contains("/apex/com.android.sdkext/bin/derive_classpath"), _, _))
      .WillOnce(DoAll(WithArg<0>(WriteToFdFlag("/proc/self/fd/", "export BOOTCLASSPATH /foo:/bar")),
                      Return(0)));

  EXPECT_CALL(mock_mount_, Call).Times(2).WillRepeatedly(Return(0));

  std::shared_ptr<IArtdCancellationSignal> cancellation_signal;
  ASSERT_STATUS_OK(artd_->createCancellationSignal(&cancellation_signal));

  constexpr pid_t kPid = 123;
  constexpr std::chrono::duration<int> kTimeout = std::chrono::seconds(1);

  std::condition_variable process_started_cv, process_killed_cv;
  std::mutex mu;

  EXPECT_CALL(*mock_exec_utils_, DoExecAndReturnCode(Contains(art_root_ + "/bin/odrefresh"), _, _))
      .WillOnce([&](auto, const ExecCallbacks& callbacks, auto) {
        std::unique_lock<std::mutex> lock(mu);
        // Step 2.
        callbacks.on_start(kPid);
        process_started_cv.notify_one();
        EXPECT_EQ(process_killed_cv.wait_for(lock, kTimeout), std::cv_status::no_timeout);
        // Step 5.
        callbacks.on_end(kPid);
        return Error();
      });

  EXPECT_CALL(mock_kill_, Call(-kPid, SIGKILL)).WillOnce([&](auto, auto) {
    // Step 4.
    process_killed_cv.notify_one();
    return 0;
  });

  std::thread t;
  bool aidl_return;
  {
    std::unique_lock<std::mutex> lock(mu);
    // Step 1.
    t = std::thread(
        [&] { ASSERT_STATUS_OK(artd_->preRebootInit(cancellation_signal, &aidl_return)); });
    EXPECT_EQ(process_started_cv.wait_for(lock, kTimeout), std::cv_status::no_timeout);
    // Step 3.
    cancellation_signal->cancel();
  }

  t.join();

  // Step 6.
  EXPECT_FALSE(aidl_return);
}

TEST_F(ArtdPreRebootTest, dexopt) {
  std::string profile_file = OR_FATAL(BuildProfileOrDmPath(profile_path_.value()));

  dexopt_options_.generateAppImage = true;

  EXPECT_CALL(
      *mock_exec_utils_,
      DoExecAndReturnCode(
          WhenSplitBy("--", _, Contains(Flag("--profile-file-fd=", FdOf(profile_file)))), _, _))
      .WillOnce(DoAll(WithArg<0>(WriteToFdFlag("--oat-fd=", "oat")),
                      WithArg<0>(WriteToFdFlag("--output-vdex-fd=", "vdex")),
                      WithArg<0>(WriteToFdFlag("--app-image-fd=", "art")),
                      Return(0)));
  RunDexopt();

  CheckContent(scratch_path_ + "/a/oat/arm64/b.odex.staged", "oat");
  CheckContent(scratch_path_ + "/a/oat/arm64/b.vdex.staged", "vdex");
  CheckContent(scratch_path_ + "/a/oat/arm64/b.art.staged", "art");
}

TEST_F(ArtdPreRebootTest, dexoptPreRebootProfile) {
  profile_path_->get<ProfilePath::tmpProfilePath>()
      .finalPath.get<WritableProfilePath::forPrimary>()
      .isPreReboot = true;
  std::string profile_file = OR_FATAL(BuildProfileOrDmPath(profile_path_.value()));

  dexopt_options_.generateAppImage = true;

  EXPECT_CALL(
      *mock_exec_utils_,
      DoExecAndReturnCode(
          WhenSplitBy("--", _, Contains(Flag("--profile-file-fd=", FdOf(profile_file)))), _, _))
      .WillOnce(DoAll(WithArg<0>(WriteToFdFlag("--oat-fd=", "oat")),
                      WithArg<0>(WriteToFdFlag("--output-vdex-fd=", "vdex")),
                      WithArg<0>(WriteToFdFlag("--app-image-fd=", "art")),
                      Return(0)));
  RunDexopt();

  CheckContent(scratch_path_ + "/a/oat/arm64/b.odex.staged", "oat");
  CheckContent(scratch_path_ + "/a/oat/arm64/b.vdex.staged", "vdex");
  CheckContent(scratch_path_ + "/a/oat/arm64/b.art.staged", "art");
}

TEST_F(ArtdPreRebootTest, copyAndRewriteProfile) {
  std::string src_file = OR_FATAL(BuildProfileOrDmPath(profile_path_.value()));
  CreateFile(src_file, "valid_profile");

  CreateFile(dex_file_);

  EXPECT_CALL(*mock_exec_utils_, DoExecAndReturnCode)
      .WillOnce(DoAll(WithArg<0>(WriteToFdFlag("--reference-profile-file-fd=", "def")),
                      Return(ProfmanResult::kCopyAndUpdateSuccess)));

  auto [result, dst] = OR_FAIL(RunCopyAndRewriteProfile());

  EXPECT_EQ(result.status, CopyAndRewriteProfileResult::Status::SUCCESS);
  EXPECT_THAT(dst.profilePath.tmpPath, ContainsRegex(R"re(/primary\.prof\.staged\.\w+\.tmp$)re"));
  CheckContent(dst.profilePath.tmpPath, "def");
}

TEST_F(ArtdPreRebootTest, copyAndRewriteEmbeddedProfile) {
  TEST_DISABLED_FOR_SHELL_WITHOUT_MEMFD_ACCESS();

  CreateZipWithSingleEntry(dex_file_, "assets/art-profile/baseline.prof", "valid_profile");

  EXPECT_CALL(*mock_exec_utils_, DoExecAndReturnCode)
      .WillOnce(DoAll(WithArg<0>(WriteToFdFlag("--reference-profile-file-fd=", "def")),
                      Return(ProfmanResult::kCopyAndUpdateSuccess)));

  auto [result, dst] = OR_FAIL(RunCopyAndRewriteEmbeddedProfile());

  EXPECT_EQ(result.status, CopyAndRewriteProfileResult::Status::SUCCESS);
  EXPECT_THAT(dst.profilePath.tmpPath, ContainsRegex(R"re(/primary\.prof\.staged\.\w+\.tmp$)re"));
  CheckContent(dst.profilePath.tmpPath, "def");
}

TEST_F(ArtdPreRebootTest, mergeProfiles) {
  std::string reference_profile_file = OR_FATAL(BuildProfileOrDmPath(profile_path_.value()));
  CreateFile(reference_profile_file, "abc");

  PrimaryCurProfilePath profile_1_path{
      .userId = 1, .packageName = "com.android.foo", .profileName = "primary"};
  std::string profile_1_file = OR_FATAL(BuildPrimaryCurProfilePath(profile_1_path));
  CreateFile(profile_1_file, "def");

  OutputProfile output_profile{.profilePath = tmp_profile_path_,
                               .fsPermission = FsPermission{.uid = -1, .gid = -1}};
  output_profile.profilePath.id = "";
  output_profile.profilePath.tmpPath = "";

  std::string dex_file_1 = scratch_path_ + "/a/b.apk";
  CreateFile(dex_file_1);

  EXPECT_CALL(
      *mock_exec_utils_,
      DoExecAndReturnCode(
          WhenSplitBy("--",
                      _,
                      AllOf(Contains(Flag("--reference-profile-file-fd=", FdHasContent("abc"))),
                            Contains(Flag("--profile-file-fd=", FdHasContent("def"))))),
          _,
          _))
      .WillOnce(DoAll(WithArg<0>(ClearAndWriteToFdFlag("--reference-profile-file-fd=", "merged")),
                      Return(ProfmanResult::kCompile)));

  bool result;
  ASSERT_STATUS_OK(artd_->mergeProfiles({profile_1_path},
                                        profile_path_,
                                        &output_profile,
                                        {dex_file_1},
                                        /*in_options=*/{},
                                        &result));
  EXPECT_TRUE(result);
  EXPECT_THAT(output_profile.profilePath.tmpPath,
              ContainsRegex(R"re(/primary\.prof\.staged\.\w+\.tmp$)re"));
  CheckContent(output_profile.profilePath.tmpPath, "merged");
}

TEST_F(ArtdPreRebootTest, mergeProfilesPreRebootReference) {
  profile_path_->get<ProfilePath::tmpProfilePath>()
      .finalPath.get<WritableProfilePath::forPrimary>()
      .isPreReboot = true;
  std::string reference_profile_file = OR_FATAL(BuildProfileOrDmPath(profile_path_.value()));
  CreateFile(reference_profile_file, "abc");

  PrimaryCurProfilePath profile_1_path{
      .userId = 1, .packageName = "com.android.foo", .profileName = "primary"};
  std::string profile_1_file = OR_FATAL(BuildPrimaryCurProfilePath(profile_1_path));
  CreateFile(profile_1_file, "def");

  OutputProfile output_profile{.profilePath = tmp_profile_path_,
                               .fsPermission = FsPermission{.uid = -1, .gid = -1}};
  output_profile.profilePath.id = "";
  output_profile.profilePath.tmpPath = "";

  std::string dex_file_1 = scratch_path_ + "/a/b.apk";
  CreateFile(dex_file_1);

  EXPECT_CALL(
      *mock_exec_utils_,
      DoExecAndReturnCode(
          WhenSplitBy("--",
                      _,
                      AllOf(Contains(Flag("--reference-profile-file-fd=", FdHasContent("abc"))),
                            Contains(Flag("--profile-file-fd=", FdHasContent("def"))))),
          _,
          _))
      .WillOnce(DoAll(WithArg<0>(ClearAndWriteToFdFlag("--reference-profile-file-fd=", "merged")),
                      Return(ProfmanResult::kCompile)));

  bool result;
  ASSERT_STATUS_OK(artd_->mergeProfiles({profile_1_path},
                                        profile_path_,
                                        &output_profile,
                                        {dex_file_1},
                                        /*in_options=*/{},
                                        &result));
  EXPECT_TRUE(result);
  EXPECT_THAT(output_profile.profilePath.tmpPath,
              ContainsRegex(R"re(/primary\.prof\.staged\.\w+\.tmp$)re"));
  CheckContent(output_profile.profilePath.tmpPath, "merged");
}

}  // namespace
}  // namespace artd
}  // namespace art
