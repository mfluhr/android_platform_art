/*
 * Copyright (C) 2016 The Android Open Source Project
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

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <iterator>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "android-base/logging.h"
#include "android-base/macros.h"
#include "android-base/result-gmock.h"
#include "android-base/result.h"
#include "android-base/stringprintf.h"
#include "arch/instruction_set_features.h"
#include "base/macros.h"
#include "base/mutex-inl.h"
#include "base/utils.h"
#include "base/zip_archive.h"
#include "common_runtime_test.h"
#include "dex/art_dex_file_loader.h"
#include "dex/base64_test_util.h"
#include "dex/bytecode_utils.h"
#include "dex/class_accessor-inl.h"
#include "dex/code_item_accessors-inl.h"
#include "dex/dex_file-inl.h"
#include "dex/dex_file_loader.h"
#include "dex2oat_environment_test.h"
#include "gc_root-inl.h"
#include "intern_table-inl.h"
#include "oat/elf_file.h"
#include "oat/elf_file_impl.h"
#include "oat/oat.h"
#include "oat/oat_file.h"
#include "profile/profile_compilation_info.h"
#include "vdex_file.h"
#include "ziparchive/zip_writer.h"

namespace art {

using ::android::base::Result;
using ::android::base::StringPrintf;
using ::android::base::testing::HasValue;
using ::android::base::testing::Ok;
using ::testing::AssertionFailure;
using ::testing::AssertionResult;
using ::testing::AssertionSuccess;
using ::testing::Ne;
using ::testing::Not;

class Dex2oatTest : public Dex2oatEnvironmentTest {
 public:
  enum class Status { kFailCompile, kFailOpenOat, kSuccess };

  void TearDown() override {
    Dex2oatEnvironmentTest::TearDown();

    output_ = "";
  }

 protected:
  Result<int> GenerateOdexForTestWithStatus(const std::vector<std::string>& dex_locations,
                                            const std::string& odex_location,
                                            CompilerFilter::Filter filter,
                                            const std::vector<std::string>& extra_args = {},
                                            bool use_fd = false) {
    std::unique_ptr<File> oat_file;
    std::vector<std::string> args;
    args.reserve(dex_locations.size() + extra_args.size() + 6);
    // Add dex file args.
    for (const std::string& dex_location : dex_locations) {
      args.push_back("--dex-file=" + dex_location);
    }
    if (use_fd) {
      oat_file.reset(OS::CreateEmptyFile(odex_location.c_str()));
      if (oat_file == nullptr) {
        return ErrnoErrorf("CreateEmptyFile failed on {}", odex_location);
      }
      args.push_back("--oat-fd=" + std::to_string(oat_file->Fd()));
      args.push_back("--oat-location=" + odex_location);
    } else {
      args.push_back("--oat-file=" + odex_location);
    }
    args.push_back("--compiler-filter=" + CompilerFilter::NameOfFilter(filter));
    args.push_back("--runtime-arg");
    args.push_back("-Xnorelocate");

    // Unless otherwise stated, use a small amount of threads, so that potential aborts are
    // shorter. This can be overridden with extra_args.
    args.push_back("-j4");

    args.insert(args.end(), extra_args.begin(), extra_args.end());

    int status = OR_RETURN(Dex2Oat(args, &output_));
    if (oat_file != nullptr) {
      int fc_errno = oat_file->FlushClose();
      if (fc_errno != 0) {
        return Errorf(
            "Could not flush and close oat file {}: {}", odex_location, strerror(-fc_errno));
      }
    }
    return status;
  }

  AssertionResult GenerateOdexForTest(const std::string& dex_location,
                                      const std::string& odex_location,
                                      CompilerFilter::Filter filter,
                                      const std::vector<std::string>& extra_args = {},
                                      Status expect_status = Status::kSuccess,
                                      bool use_fd = false,
                                      bool use_zip_fd = false) WARN_UNUSED {
    return GenerateOdexForTest(dex_location,
                               odex_location,
                               filter,
                               extra_args,
                               expect_status,
                               use_fd,
                               use_zip_fd,
                               [](const OatFile&) {});
  }

  bool test_accepts_odex_file_on_failure = false;

  template <typename T>
  AssertionResult GenerateOdexForTest(const std::string& dex_location,
                                      const std::string& odex_location,
                                      CompilerFilter::Filter filter,
                                      const std::vector<std::string>& extra_args,
                                      Status expect_status,
                                      bool use_fd,
                                      bool use_zip_fd,
                                      T check_oat) WARN_UNUSED {
    std::vector<std::string> dex_locations;
    if (use_zip_fd) {
      std::string loc_arg = "--zip-location=" + dex_location;
      CHECK(std::any_of(extra_args.begin(), extra_args.end(), [&](const std::string& s) {
        return s == loc_arg;
      }));
      CHECK(std::any_of(extra_args.begin(), extra_args.end(), [](const std::string& s) {
        return s.starts_with("--zip-fd=");
      }));
    } else {
      dex_locations.push_back(dex_location);
    }

    Result<int> status =
        GenerateOdexForTestWithStatus(dex_locations, odex_location, filter, extra_args, use_fd);

    bool success = status.ok() && status.value() == 0;
    if (expect_status != Status::kFailCompile) {
      if (!success) {
        return AssertionFailure() << "Failed to compile odex ("
                                  << (status.ok() ? StringPrintf("status=%d", status.value()) :
                                                    status.error().message())
                                  << "): " << output_;
      }

      // Verify the odex file was generated as expected.
      std::string error_msg;
      std::unique_ptr<OatFile> odex_file(OatFile::Open(/*zip_fd=*/-1,
                                                       odex_location,
                                                       odex_location,
                                                       /*executable=*/false,
                                                       /*low_4gb=*/false,
                                                       dex_location,
                                                       &error_msg));

      if (expect_status == Status::kFailOpenOat) {
        return (odex_file == nullptr) ?
                   AssertionSuccess() :
                   AssertionFailure() << "Unexpectedly was able to open odex file";
      }

      if (odex_file == nullptr) {
        return AssertionFailure() << "Could not open odex file: " << error_msg;
      }

      CheckFilter(filter, odex_file->GetCompilerFilter());
      check_oat(*(odex_file.get()));
    } else {
      if (success) {
        return AssertionFailure() << "Succeeded to compile odex: " << output_;
      }

      if (!test_accepts_odex_file_on_failure) {
        // Verify there's no loadable odex file.
        std::string error_msg;
        std::unique_ptr<OatFile> odex_file(OatFile::Open(/*zip_fd=*/-1,
                                                         odex_location,
                                                         odex_location,
                                                         /*executable=*/false,
                                                         /*low_4gb=*/false,
                                                         dex_location,
                                                         &error_msg));
        if (odex_file != nullptr) {
          return AssertionFailure() << "Could open odex file: " << error_msg;
        }
      }
    }
    return AssertionSuccess();
  }

  // Check the input compiler filter against the generated oat file's filter. May be overridden
  // in subclasses when equality is not expected.
  virtual void CheckFilter(CompilerFilter::Filter expected, CompilerFilter::Filter actual) {
    EXPECT_EQ(expected, actual);
  }

  std::string output_ = "";
};

// This test class provides an easy way to validate an expected filter which is different
// then the one pass to generate the odex file (compared to adding yet another argument
// to what's already huge test methods).
class Dex2oatWithExpectedFilterTest : public Dex2oatTest {
 protected:
  void CheckFilter([[maybe_unused]] CompilerFilter::Filter expected,
                   CompilerFilter::Filter actual) override {
    EXPECT_EQ(expected_filter_, actual);
  }

  CompilerFilter::Filter expected_filter_;
};

class Dex2oatSwapTest : public Dex2oatTest {
 protected:
  void RunTest(bool use_fd, bool expect_use, const std::vector<std::string>& extra_args = {}) {
    std::string dex_location = GetScratchDir() + "/Dex2OatSwapTest.jar";
    std::string odex_location = GetOdexDir() + "/Dex2OatSwapTest.odex";

    Copy(GetTestDexFileName(), dex_location);

    std::vector<std::string> copy(extra_args);

    std::unique_ptr<ScratchFile> sf;
    if (use_fd) {
      sf.reset(new ScratchFile());
      copy.push_back(android::base::StringPrintf("--swap-fd=%d", sf->GetFd()));
    } else {
      std::string swap_location = GetOdexDir() + "/Dex2OatSwapTest.odex.swap";
      copy.push_back("--swap-file=" + swap_location);
    }
    ASSERT_TRUE(GenerateOdexForTest(dex_location, odex_location, CompilerFilter::kSpeed, copy));

    CheckValidity();
    CheckResult(expect_use);
  }

  virtual std::string GetTestDexFileName() {
    return Dex2oatEnvironmentTest::GetTestDexFileName("VerifierDeps");
  }

  virtual void CheckResult(bool expect_use) {
    if (kIsTargetBuild) {
      CheckTargetResult(expect_use);
    } else {
      CheckHostResult(expect_use);
    }
  }

  virtual void CheckTargetResult([[maybe_unused]] bool expect_use) {
    // TODO: Ignore for now, as we won't capture any output (it goes to the logcat). We may do
    //       something for variants with file descriptor where we can control the lifetime of
    //       the swap file and thus take a look at it.
  }

  virtual void CheckHostResult(bool expect_use) {
    if (!kIsTargetBuild) {
      if (expect_use) {
        EXPECT_NE(output_.find("Large app, accepted running with swap."), std::string::npos)
            << output_;
      } else {
        EXPECT_EQ(output_.find("Large app, accepted running with swap."), std::string::npos)
            << output_;
      }
    }
  }

  // Check whether the dex2oat run was really successful.
  virtual void CheckValidity() {
    if (kIsTargetBuild) {
      CheckTargetValidity();
    } else {
      CheckHostValidity();
    }
  }

  virtual void CheckTargetValidity() {
    // TODO: Ignore for now, as we won't capture any output (it goes to the logcat). We may do
    //       something for variants with file descriptor where we can control the lifetime of
    //       the swap file and thus take a look at it.
  }

  // On the host, we can get the dex2oat output. Here, look for "dex2oat took."
  virtual void CheckHostValidity() {
    EXPECT_NE(output_.find("dex2oat took"), std::string::npos) << output_;
  }
};

TEST_F(Dex2oatSwapTest, DoNotUseSwapDefaultSingleSmall) {
  RunTest(/*use_fd=*/false, /*expect_use=*/false);
  RunTest(/*use_fd=*/true, /*expect_use=*/false);
}

TEST_F(Dex2oatSwapTest, DoNotUseSwapSingle) {
  RunTest(/*use_fd=*/false, /*expect_use=*/false, {"--swap-dex-size-threshold=0"});
  RunTest(/*use_fd=*/true, /*expect_use=*/false, {"--swap-dex-size-threshold=0"});
}

TEST_F(Dex2oatSwapTest, DoNotUseSwapSmall) {
  RunTest(/*use_fd=*/false, /*expect_use=*/false, {"--swap-dex-count-threshold=0"});
  RunTest(/*use_fd=*/true, /*expect_use=*/false, {"--swap-dex-count-threshold=0"});
}

TEST_F(Dex2oatSwapTest, DoUseSwapSingleSmall) {
  RunTest(/*use_fd=*/false,
          /*expect_use=*/true,
          {"--swap-dex-size-threshold=0", "--swap-dex-count-threshold=0"});
  RunTest(/*use_fd=*/true,
          /*expect_use=*/true,
          {"--swap-dex-size-threshold=0", "--swap-dex-count-threshold=0"});
}

class Dex2oatSwapUseTest : public Dex2oatSwapTest {
 protected:
  void CheckHostResult(bool expect_use) override {
    if (!kIsTargetBuild) {
      if (expect_use) {
        EXPECT_NE(output_.find("Large app, accepted running with swap."), std::string::npos)
            << output_;
      } else {
        EXPECT_EQ(output_.find("Large app, accepted running with swap."), std::string::npos)
            << output_;
      }
    }
  }

  std::string GetTestDexFileName() override {
    // Use Statics as it has a handful of functions.
    return CommonRuntimeTest::GetTestDexFileName("Statics");
  }

  void GrabResult1() {
    if (!kIsTargetBuild) {
      native_alloc_1_ = ParseNativeAlloc();
      swap_1_ = ParseSwap(/*expected=*/false);
    } else {
      native_alloc_1_ = std::numeric_limits<size_t>::max();
      swap_1_ = 0;
    }
  }

  void GrabResult2() {
    if (!kIsTargetBuild) {
      native_alloc_2_ = ParseNativeAlloc();
      swap_2_ = ParseSwap(/*expected=*/true);
    } else {
      native_alloc_2_ = 0;
      swap_2_ = std::numeric_limits<size_t>::max();
    }
  }

 private:
  size_t ParseNativeAlloc() {
    std::regex native_alloc_regex("dex2oat took.*native alloc=[^ ]+ \\(([0-9]+)B\\)");
    std::smatch native_alloc_match;
    bool found = std::regex_search(output_, native_alloc_match, native_alloc_regex);
    if (!found) {
      EXPECT_TRUE(found);
      return 0;
    }
    if (native_alloc_match.size() != 2U) {
      EXPECT_EQ(native_alloc_match.size(), 2U);
      return 0;
    }

    std::istringstream stream(native_alloc_match[1].str());
    size_t value;
    stream >> value;

    return value;
  }

  size_t ParseSwap(bool expected) {
    std::regex swap_regex("dex2oat took[^\\n]+swap=[^ ]+ \\(([0-9]+)B\\)");
    std::smatch swap_match;
    bool found = std::regex_search(output_, swap_match, swap_regex);
    if (found != expected) {
      EXPECT_EQ(expected, found);
      return 0;
    }

    if (!found) {
      return 0;
    }

    if (swap_match.size() != 2U) {
      EXPECT_EQ(swap_match.size(), 2U);
      return 0;
    }

    std::istringstream stream(swap_match[1].str());
    size_t value;
    stream >> value;

    return value;
  }

 protected:
  size_t native_alloc_1_;
  size_t native_alloc_2_;

  size_t swap_1_;
  size_t swap_2_;
};

TEST_F(Dex2oatSwapUseTest, CheckSwapUsage) {
  // Native memory usage isn't correctly tracked when running under ASan.
  TEST_DISABLED_FOR_MEMORY_TOOL();

  // The `native_alloc_2_ >= native_alloc_1_` assertion below may not
  // hold true on some x86 or x86_64 systems; disable this test while we
  // investigate (b/29259363).
  TEST_DISABLED_FOR_X86();
  TEST_DISABLED_FOR_X86_64();

  RunTest(/*use_fd=*/false,
          /*expect_use=*/false);
  GrabResult1();
  std::string output_1 = output_;

  output_ = "";

  RunTest(/*use_fd=*/false,
          /*expect_use=*/true,
          {"--swap-dex-size-threshold=0", "--swap-dex-count-threshold=0"});
  GrabResult2();
  std::string output_2 = output_;

  if (native_alloc_2_ >= native_alloc_1_ || swap_1_ >= swap_2_) {
    EXPECT_LT(native_alloc_2_, native_alloc_1_);
    EXPECT_LT(swap_1_, swap_2_);

    LOG(ERROR) << output_1;
    LOG(ERROR) << output_2;
  }
}

class Dex2oatVeryLargeTest : public Dex2oatTest {
 protected:
  void CheckFilter([[maybe_unused]] CompilerFilter::Filter input,
                   [[maybe_unused]] CompilerFilter::Filter result) override {
    // Ignore, we'll do our own checks.
  }

  void RunTest(CompilerFilter::Filter filter,
               bool expect_large,
               bool expect_downgrade,
               const std::vector<std::string>& extra_args = {}) {
    RunTest(filter, filter, expect_large, expect_downgrade, extra_args);
  }

  void RunTest(CompilerFilter::Filter filter,
               CompilerFilter::Filter expected_filter,
               bool expect_large,
               bool expect_downgrade,
               const std::vector<std::string>& extra_args = {}) {
    std::string dex_location = GetScratchDir() + "/DexNoOat.jar";
    std::string odex_location = GetOdexDir() + "/DexOdexNoOat.odex";
    std::string app_image_file = GetScratchDir() + "/Test.art";

    Copy(GetDexSrc1(), dex_location);

    std::vector<std::string> new_args(extra_args);
    new_args.push_back("--app-image-file=" + app_image_file);
    ASSERT_TRUE(GenerateOdexForTest(dex_location, odex_location, filter, new_args));

    CheckValidity();
    CheckResult(dex_location,
                odex_location,
                app_image_file,
                expected_filter,
                expect_large,
                expect_downgrade);
  }

  void CheckResult(const std::string& dex_location,
                   const std::string& odex_location,
                   const std::string& app_image_file,
                   CompilerFilter::Filter expected_filter,
                   bool expect_large,
                   bool expect_downgrade) {
    if (expect_downgrade) {
      EXPECT_TRUE(expect_large);
    }
    // Host/target independent checks.
    std::string error_msg;
    std::unique_ptr<OatFile> odex_file(OatFile::Open(/*zip_fd=*/-1,
                                                     odex_location,
                                                     odex_location,
                                                     /*executable=*/false,
                                                     /*low_4gb=*/false,
                                                     dex_location,
                                                     &error_msg));
    ASSERT_TRUE(odex_file.get() != nullptr) << error_msg;
    EXPECT_GT(app_image_file.length(), 0u);
    std::unique_ptr<File> file(OS::OpenFileForReading(app_image_file.c_str()));
    if (expect_large) {
      // Note: we cannot check the following
      // EXPECT_FALSE(CompilerFilter::IsAotCompilationEnabled(odex_file->GetCompilerFilter()));
      // The reason is that the filter override currently happens when the dex files are
      // loaded in dex2oat, which is after the oat file has been started. Thus, the header
      // store cannot be changed, and the original filter is set in stone.

      for (const OatDexFile* oat_dex_file : odex_file->GetOatDexFiles()) {
        std::unique_ptr<const DexFile> dex_file = oat_dex_file->OpenDexFile(&error_msg);
        ASSERT_TRUE(dex_file != nullptr);
        uint32_t class_def_count = dex_file->NumClassDefs();
        ASSERT_LT(class_def_count, std::numeric_limits<uint16_t>::max());
        for (uint16_t class_def_index = 0; class_def_index < class_def_count; ++class_def_index) {
          OatFile::OatClass oat_class = oat_dex_file->GetOatClass(class_def_index);
          EXPECT_EQ(oat_class.GetType(), OatClassType::kNoneCompiled);
        }
      }

      // If the input filter was "below," it should have been used.
      EXPECT_EQ(odex_file->GetCompilerFilter(), expected_filter);

      // If expect large, make sure the app image isn't generated or is empty.
      if (file != nullptr) {
        EXPECT_EQ(file->GetLength(), 0u);
      }
    } else {
      EXPECT_EQ(odex_file->GetCompilerFilter(), expected_filter);
      ASSERT_TRUE(file != nullptr) << app_image_file;
      EXPECT_GT(file->GetLength(), 0u);
    }

    // Host/target dependent checks.
    if (kIsTargetBuild) {
      CheckTargetResult(expect_downgrade);
    } else {
      CheckHostResult(expect_downgrade);
    }
  }

  void CheckTargetResult([[maybe_unused]] bool expect_downgrade) {
    // TODO: Ignore for now. May do something for fd things.
  }

  void CheckHostResult(bool expect_downgrade) {
    if (!kIsTargetBuild) {
      if (expect_downgrade) {
        EXPECT_NE(output_.find("Very large app, downgrading to"), std::string::npos) << output_;
      } else {
        EXPECT_EQ(output_.find("Very large app, downgrading to"), std::string::npos) << output_;
      }
    }
  }

  // Check whether the dex2oat run was really successful.
  void CheckValidity() {
    if (kIsTargetBuild) {
      CheckTargetValidity();
    } else {
      CheckHostValidity();
    }
  }

  void CheckTargetValidity() {
    // TODO: Ignore for now.
  }

  // On the host, we can get the dex2oat output. Here, look for "dex2oat took."
  void CheckHostValidity() {
    EXPECT_NE(output_.find("dex2oat took"), std::string::npos) << output_;
  }
};

TEST_F(Dex2oatVeryLargeTest, DontUseVeryLarge) {
  RunTest(CompilerFilter::kAssumeVerified, false, false);
  RunTest(CompilerFilter::kSpeed, false, false);

  RunTest(CompilerFilter::kAssumeVerified, false, false, {"--very-large-app-threshold=10000000"});
  RunTest(CompilerFilter::kSpeed, false, false, {"--very-large-app-threshold=10000000"});
}

TEST_F(Dex2oatVeryLargeTest, UseVeryLarge) {
  RunTest(CompilerFilter::kAssumeVerified, true, false, {"--very-large-app-threshold=100"});
  RunTest(CompilerFilter::kSpeed, true, true, {"--very-large-app-threshold=100"});
}

// Regressin test for b/35665292.
TEST_F(Dex2oatVeryLargeTest, SpeedProfileNoProfile) {
  // Test that dex2oat doesn't crash with speed-profile but no input profile.
  RunTest(CompilerFilter::kSpeedProfile, CompilerFilter::kVerify, false, false);
}

class Dex2oatLayoutTest : public Dex2oatTest {
 protected:
  void CheckFilter([[maybe_unused]] CompilerFilter::Filter input,
                   [[maybe_unused]] CompilerFilter::Filter result) override {
    // Ignore, we'll do our own checks.
  }

  // Emits a profile with a single dex file with the given location and classes ranging
  // from `class_offset` to `class_offset + num_classes`.
  void GenerateProfile(const std::string& test_profile,
                       const std::string& dex_location,
                       size_t num_classes,
                       size_t class_offset = 0) {
    const char* location = dex_location.c_str();
    std::string error_msg;
    std::vector<std::unique_ptr<const DexFile>> dex_files;
    ArtDexFileLoader dex_file_loader(location);
    ASSERT_TRUE(dex_file_loader.Open(
        /*verify=*/true, /*verify_checksum=*/true, &error_msg, &dex_files));
    EXPECT_EQ(dex_files.size(), 1U);
    std::unique_ptr<const DexFile>& dex_file = dex_files[0];

    int profile_test_fd =
        open(test_profile.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
    CHECK_GE(profile_test_fd, 0);

    ProfileCompilationInfo info;
    std::vector<dex::TypeIndex> classes;
    for (size_t i = 0; i < num_classes; ++i) {
      classes.push_back(dex::TypeIndex(class_offset + 1 + i));
    }
    info.AddClassesForDex(dex_file.get(), classes.begin(), classes.end());
    bool result = info.Save(profile_test_fd);
    close(profile_test_fd);
    ASSERT_TRUE(result);
  }

  // Compiles a dex file with profiles.
  void CompileProfileOdex(const std::string& dex_location,
                          const std::string& odex_location,
                          const std::string& app_image_file_name,
                          bool use_fd,
                          const std::vector<std::string>& profile_locations,
                          const std::vector<std::string>& extra_args = {},
                          Status expect_status = Status::kSuccess) {
    std::vector<std::string> copy(extra_args);
    for (const std::string& profile_location : profile_locations) {
      copy.push_back("--profile-file=" + profile_location);
    }
    std::unique_ptr<File> app_image_file;
    if (!app_image_file_name.empty()) {
      if (use_fd) {
        app_image_file.reset(OS::CreateEmptyFile(app_image_file_name.c_str()));
        copy.push_back("--app-image-fd=" + std::to_string(app_image_file->Fd()));
      } else {
        copy.push_back("--app-image-file=" + app_image_file_name);
      }
    }
    ASSERT_TRUE(GenerateOdexForTest(
        dex_location, odex_location, CompilerFilter::kSpeedProfile, copy, expect_status, use_fd));
    if (app_image_file != nullptr) {
      ASSERT_EQ(app_image_file->FlushCloseOrErase(), 0) << "Could not flush and close art file";
    }
  }

  // Same as above, but generates the profile internally with classes ranging from 0 to
  // `num_profile_classes`.
  void CompileProfileOdex(const std::string& dex_location,
                          const std::string& odex_location,
                          const std::string& app_image_file_name,
                          bool use_fd,
                          size_t num_profile_classes,
                          const std::vector<std::string>& extra_args = {},
                          Status expect_status = Status::kSuccess) {
    const std::string profile_location = GetScratchDir() + "/primary.prof";
    GenerateProfile(profile_location, dex_location, num_profile_classes);
    CompileProfileOdex(dex_location,
                       odex_location,
                       app_image_file_name,
                       use_fd,
                       {profile_location},
                       extra_args,
                       expect_status);
  }

  uint32_t GetImageObjectSectionSize(const std::string& image_file_name) {
    EXPECT_FALSE(image_file_name.empty());
    std::unique_ptr<File> file(OS::OpenFileForReading(image_file_name.c_str()));
    CHECK(file != nullptr);
    ImageHeader image_header;
    const bool success = file->ReadFully(&image_header, sizeof(image_header));
    CHECK(success);
    CHECK(image_header.IsValid());
    ReaderMutexLock mu(Thread::Current(), *Locks::mutator_lock_);
    return image_header.GetObjectsSection().Size();
  }

  void RunTest(bool app_image) {
    std::string dex_location = GetScratchDir() + "/DexNoOat.jar";
    std::string odex_location = GetOdexDir() + "/DexOdexNoOat.odex";
    std::string app_image_file = app_image ? (GetOdexDir() + "/DexOdexNoOat.art") : "";
    Copy(GetDexSrc2(), dex_location);

    uint32_t image_file_empty_profile = 0;
    if (app_image) {
      CompileProfileOdex(dex_location,
                         odex_location,
                         app_image_file,
                         /*use_fd=*/false,
                         /*num_profile_classes=*/0);
      CheckValidity();
      // Don't check the result since CheckResult relies on the class being in the profile.
      image_file_empty_profile = GetImageObjectSectionSize(app_image_file);
      EXPECT_GT(image_file_empty_profile, 0u);
      CheckCompilerFilter(dex_location, odex_location, CompilerFilter::Filter::kVerify);
    }

    // Small profile.
    CompileProfileOdex(dex_location,
                       odex_location,
                       app_image_file,
                       /*use_fd=*/false,
                       /*num_profile_classes=*/1);
    CheckValidity();
    CheckResult(dex_location, odex_location, app_image_file);
    CheckCompilerFilter(dex_location, odex_location, CompilerFilter::Filter::kSpeedProfile);

    if (app_image) {
      // Test that the profile made a difference by adding more classes.
      const uint32_t image_file_small_profile = GetImageObjectSectionSize(app_image_file);
      ASSERT_LT(image_file_empty_profile, image_file_small_profile);
    }
  }

  void CheckCompilerFilter(const std::string& dex_location,
                           const std::string& odex_location,
                           CompilerFilter::Filter expected_filter) {
    std::string error_msg;
    std::unique_ptr<OatFile> odex_file(OatFile::Open(/*zip_fd=*/-1,
                                                     odex_location,
                                                     odex_location,
                                                     /*executable=*/false,
                                                     /*low_4gb=*/false,
                                                     dex_location,
                                                     &error_msg));
    EXPECT_EQ(odex_file->GetCompilerFilter(), expected_filter);
  }

  void RunTestVDex() {
    std::string dex_location = GetScratchDir() + "/DexNoOat.jar";
    std::string odex_location = GetOdexDir() + "/DexOdexNoOat.odex";
    std::string vdex_location = GetOdexDir() + "/DexOdexNoOat.vdex";
    std::string app_image_file_name = GetOdexDir() + "/DexOdexNoOat.art";
    Copy(GetDexSrc2(), dex_location);

    std::unique_ptr<File> vdex_file1(OS::CreateEmptyFile(vdex_location.c_str()));
    CHECK(vdex_file1 != nullptr) << vdex_location;
    ScratchFile vdex_file2;
    {
      std::string input_vdex = "--input-vdex-fd=-1";
      std::string output_vdex = StringPrintf("--output-vdex-fd=%d", vdex_file1->Fd());
      CompileProfileOdex(dex_location,
                         odex_location,
                         app_image_file_name,
                         /*use_fd=*/true,
                         /*num_profile_classes=*/1,
                         {input_vdex, output_vdex});
      EXPECT_GT(vdex_file1->GetLength(), 0u);
    }
    {
      // Test that vdex and dexlayout fail gracefully.
      std::string input_vdex = StringPrintf("--input-vdex-fd=%d", vdex_file1->Fd());
      std::string output_vdex = StringPrintf("--output-vdex-fd=%d", vdex_file2.GetFd());
      CompileProfileOdex(dex_location,
                         odex_location,
                         app_image_file_name,
                         /*use_fd=*/true,
                         /*num_profile_classes=*/1,
                         {input_vdex, output_vdex},
                         /*expect_status=*/Status::kSuccess);
      EXPECT_GT(vdex_file2.GetFile()->GetLength(), 0u);
    }
    ASSERT_EQ(vdex_file1->FlushCloseOrErase(), 0) << "Could not flush and close vdex file";
    CheckValidity();
  }

  void CheckResult(const std::string& dex_location,
                   const std::string& odex_location,
                   const std::string& app_image_file_name) {
    // Host/target independent checks.
    std::string error_msg;
    std::unique_ptr<OatFile> odex_file(OatFile::Open(/*zip_fd=*/-1,
                                                     odex_location,
                                                     odex_location,
                                                     /*executable=*/false,
                                                     /*low_4gb=*/false,
                                                     dex_location,
                                                     &error_msg));
    ASSERT_TRUE(odex_file.get() != nullptr) << error_msg;

    const char* location = dex_location.c_str();
    std::vector<std::unique_ptr<const DexFile>> dex_files;
    ArtDexFileLoader dex_file_loader(location);
    ASSERT_TRUE(dex_file_loader.Open(
        /*verify=*/true, /*verify_checksum=*/true, &error_msg, &dex_files));
    EXPECT_EQ(dex_files.size(), 1U);
    std::unique_ptr<const DexFile>& old_dex_file = dex_files[0];

    for (const OatDexFile* oat_dex_file : odex_file->GetOatDexFiles()) {
      std::unique_ptr<const DexFile> new_dex_file = oat_dex_file->OpenDexFile(&error_msg);
      ASSERT_TRUE(new_dex_file != nullptr);
      uint32_t class_def_count = new_dex_file->NumClassDefs();
      ASSERT_LT(class_def_count, std::numeric_limits<uint16_t>::max());
      ASSERT_GE(class_def_count, 2U);

      // Make sure the indexes stay the same.
      std::string old_class0 = old_dex_file->PrettyType(old_dex_file->GetClassDef(0).class_idx_);
      std::string old_class1 = old_dex_file->PrettyType(old_dex_file->GetClassDef(1).class_idx_);
      std::string new_class0 = new_dex_file->PrettyType(new_dex_file->GetClassDef(0).class_idx_);
      std::string new_class1 = new_dex_file->PrettyType(new_dex_file->GetClassDef(1).class_idx_);
      EXPECT_EQ(old_class0, new_class0);
      EXPECT_EQ(old_class1, new_class1);
    }

    EXPECT_EQ(odex_file->GetCompilerFilter(), CompilerFilter::kSpeedProfile);

    if (!app_image_file_name.empty()) {
      // Go peek at the image header to make sure it was large enough to contain the class.
      std::unique_ptr<File> file(OS::OpenFileForReading(app_image_file_name.c_str()));
      ImageHeader image_header;
      bool success = file->ReadFully(&image_header, sizeof(image_header));
      ASSERT_TRUE(success);
      ASSERT_TRUE(image_header.IsValid());
      EXPECT_GT(image_header.GetObjectsSection().Size(), 0u);
    }
  }

  // Check whether the dex2oat run was really successful.
  void CheckValidity() {
    if (kIsTargetBuild) {
      CheckTargetValidity();
    } else {
      CheckHostValidity();
    }
  }

  void CheckTargetValidity() {
    // TODO: Ignore for now.
  }

  // On the host, we can get the dex2oat output. Here, look for "dex2oat took."
  void CheckHostValidity() {
    EXPECT_NE(output_.find("dex2oat took"), std::string::npos) << output_;
  }
};

TEST_F(Dex2oatLayoutTest, TestLayout) { RunTest(/*app_image=*/false); }

TEST_F(Dex2oatLayoutTest, TestLayoutAppImage) { RunTest(/*app_image=*/true); }

TEST_F(Dex2oatLayoutTest, TestLayoutAppImageMissingBootImage) {
  std::string dex_location = GetScratchDir() + "/DexNoOat.jar";
  std::string odex_location = GetOdexDir() + "/DexOdexNoOat.odex";
  std::string app_image_file = GetOdexDir() + "/DexOdexNoOat.art";
  Copy(GetDexSrc2(), dex_location);

  CompileProfileOdex(dex_location,
                     odex_location,
                     app_image_file,
                     /*use_fd=*/false,
                     /*num_profile_classes=*/1,
                     /*extra_args=*/{"--boot-image=/nonx/boot.art"},
                     /*expect_status=*/Status::kSuccess);

  // Verify the odex file does not require an image.
  std::string error_msg;
  std::unique_ptr<OatFile> odex_file(OatFile::Open(/*zip_fd=*/-1,
                                                   odex_location,
                                                   odex_location,
                                                   /*executable=*/false,
                                                   /*low_4gb=*/false,
                                                   dex_location,
                                                   &error_msg));
  ASSERT_TRUE(odex_file != nullptr) << "Could not open odex file: " << error_msg;

  CheckFilter(CompilerFilter::kSpeedProfile, odex_file->GetCompilerFilter());
  ASSERT_FALSE(odex_file->GetOatHeader().RequiresImage());
}

TEST_F(Dex2oatLayoutTest, TestLayoutMultipleProfiles) {
  std::string dex_location = GetScratchDir() + "/Dex.jar";
  std::string odex_location = GetOdexDir() + "/Dex.odex";
  std::string app_image_file = GetOdexDir() + "/Dex.art";
  Copy(GetDexSrc2(), dex_location);

  const std::string profile1_location = GetScratchDir() + "/primary.prof";
  GenerateProfile(profile1_location, dex_location, /*num_classes=*/1, /*class_offset=*/0);
  CompileProfileOdex(dex_location,
                     odex_location,
                     app_image_file,
                     /*use_fd=*/false,
                     {profile1_location});
  uint32_t image_file_size_profile1 = GetImageObjectSectionSize(app_image_file);

  const std::string profile2_location = GetScratchDir() + "/secondary.prof";
  GenerateProfile(profile2_location, dex_location, /*num_classes=*/1, /*class_offset=*/1);
  CompileProfileOdex(dex_location,
                     odex_location,
                     app_image_file,
                     /*use_fd=*/false,
                     {profile2_location});
  uint32_t image_file_size_profile2 = GetImageObjectSectionSize(app_image_file);

  CompileProfileOdex(dex_location,
                     odex_location,
                     app_image_file,
                     /*use_fd=*/false,
                     {profile1_location, profile2_location});
  uint32_t image_file_size_multiple_profiles = GetImageObjectSectionSize(app_image_file);

  CheckCompilerFilter(dex_location, odex_location, CompilerFilter::Filter::kSpeedProfile);

  // The image file generated with multiple profiles should be larger than any image file generated
  // with each profile.
  ASSERT_GT(image_file_size_multiple_profiles, image_file_size_profile1);
  ASSERT_GT(image_file_size_multiple_profiles, image_file_size_profile2);
}

TEST_F(Dex2oatLayoutTest, TestLayoutMultipleProfilesChecksumMismatch) {
  std::string dex_location = GetScratchDir() + "/Dex.jar";

  // Create two profiles whose dex locations are the same but checksums are different.
  Copy(GetDexSrc1(), dex_location);
  const std::string profile_old = GetScratchDir() + "/profile_old.prof";
  GenerateProfile(profile_old, dex_location, /*num_classes=*/1, /*class_offset=*/0);

  Copy(GetDexSrc2(), dex_location);
  const std::string profile_new = GetScratchDir() + "/profile_new.prof";
  GenerateProfile(profile_new, dex_location, /*num_classes=*/1, /*class_offset=*/0);

  // Create an empty profile for reference.
  const std::string profile_empty = GetScratchDir() + "/profile_empty.prof";
  GenerateProfile(profile_empty, dex_location, /*num_classes=*/0, /*class_offset=*/0);

  std::string odex_location = GetOdexDir() + "/Dex.odex";
  std::string app_image_file = GetOdexDir() + "/Dex.art";

  // This should produce a normal image because only `profile_new` is used and it has the right
  // checksum.
  CompileProfileOdex(dex_location,
                     odex_location,
                     app_image_file,
                     /*use_fd=*/false,
                     {profile_new, profile_old});
  uint32_t image_size_right_checksum = GetImageObjectSectionSize(app_image_file);

  // This should produce an empty image because only `profile_old` is used and it has the wrong
  // checksum. Note that dex2oat does not abort compilation when the profile verification fails
  // (b/62602192, b/65260586).
  CompileProfileOdex(dex_location,
                     odex_location,
                     app_image_file,
                     /*use_fd=*/false,
                     {profile_old, profile_new});
  uint32_t image_size_wrong_checksum = GetImageObjectSectionSize(app_image_file);

  // Create an empty image using an empty profile for reference.
  CompileProfileOdex(dex_location,
                     odex_location,
                     app_image_file,
                     /*use_fd=*/false,
                     {profile_empty});
  uint32_t image_size_empty = GetImageObjectSectionSize(app_image_file);

  EXPECT_GT(image_size_right_checksum, image_size_empty);
  EXPECT_EQ(image_size_wrong_checksum, image_size_empty);
}

TEST_F(Dex2oatLayoutTest, TestVdexLayout) { RunTestVDex(); }

class Dex2oatWatchdogTest : public Dex2oatTest {
 protected:
  void RunTest(Status expect_status, const std::vector<std::string>& extra_args = {}) {
    std::string dex_location = GetScratchDir() + "/Dex2OatSwapTest.jar";
    std::string odex_location = GetOdexDir() + "/Dex2OatSwapTest.odex";

    Copy(GetTestDexFileName(), dex_location);

    std::vector<std::string> copy(extra_args);

    std::string swap_location = GetOdexDir() + "/Dex2OatSwapTest.odex.swap";
    copy.push_back("--swap-file=" + swap_location);
    copy.push_back("-j512");  // Excessive idle threads just slow down dex2oat.
    ASSERT_TRUE(GenerateOdexForTest(
        dex_location, odex_location, CompilerFilter::kSpeed, copy, expect_status));
  }

  std::string GetTestDexFileName() { return GetDexSrc1(); }
};

TEST_F(Dex2oatWatchdogTest, TestWatchdogOK) {
  // Check with default.
  RunTest(/*expect_status=*/Status::kSuccess);

  // Check with ten minutes.
  RunTest(/*expect_status=*/Status::kSuccess, {"--watchdog-timeout=600000"});
}

TEST_F(Dex2oatWatchdogTest, TestWatchdogTrigger) {
  // This test is frequently interrupted by signal_dumper on host (x86);
  // disable it while we investigate (b/121352534).
  TEST_DISABLED_FOR_X86();

  // The watchdog is independent of dex2oat and will not delete intermediates. It is possible
  // that the compilation succeeds and the file is completely written by the time the watchdog
  // kills dex2oat (but the dex2oat threads must have been scheduled pretty badly).
  test_accepts_odex_file_on_failure = true;

  // Check with ten milliseconds.
  RunTest(/*expect_status=*/Status::kFailCompile, {"--watchdog-timeout=10"});
}

class Dex2oatClassLoaderContextTest : public Dex2oatTest {
 protected:
  void RunTest(const char* class_loader_context,
               const char* expected_classpath_key,
               Status expect_status,
               bool use_second_source = false,
               bool generate_image = false) {
    std::string dex_location = GetUsedDexLocation();
    std::string odex_location = GetUsedOatLocation();

    Copy(use_second_source ? GetDexSrc2() : GetDexSrc1(), dex_location);

    std::vector<std::string> extra_args;
    if (class_loader_context != nullptr) {
      extra_args.push_back(std::string("--class-loader-context=") + class_loader_context);
    }
    if (generate_image) {
      extra_args.push_back(std::string("--app-image-file=") + GetUsedImageLocation());
    }
    auto check_oat = [expected_classpath_key](const OatFile& oat_file) {
      ASSERT_TRUE(expected_classpath_key != nullptr);
      const char* classpath = oat_file.GetOatHeader().GetStoreValueByKey(OatHeader::kClassPathKey);
      ASSERT_TRUE(classpath != nullptr);
      ASSERT_STREQ(expected_classpath_key, classpath);
    };

    ASSERT_TRUE(GenerateOdexForTest(dex_location,
                                    odex_location,
                                    CompilerFilter::kVerify,
                                    extra_args,
                                    expect_status,
                                    /*use_fd=*/false,
                                    /*use_zip_fd=*/false,
                                    check_oat));
  }

  std::string GetUsedDexLocation() { return GetScratchDir() + "/Context.jar"; }

  std::string GetUsedOatLocation() { return GetOdexDir() + "/Context.odex"; }

  std::string GetUsedImageLocation() { return GetOdexDir() + "/Context.art"; }

  const char* kEmptyClassPathKey = "PCL[]";
};

TEST_F(Dex2oatClassLoaderContextTest, InvalidContext) {
  RunTest("Invalid[]", /*expected_classpath_key=*/nullptr, /*expect_status=*/Status::kFailCompile);
}

TEST_F(Dex2oatClassLoaderContextTest, EmptyContext) {
  RunTest("PCL[]", kEmptyClassPathKey, /*expect_status=*/Status::kSuccess);
}

TEST_F(Dex2oatClassLoaderContextTest, ContextWithTheSourceDexFiles) {
  std::string context = "PCL[" + GetUsedDexLocation() + "]";
  RunTest(context.c_str(), kEmptyClassPathKey, /*expect_status=*/Status::kSuccess);
}

TEST_F(Dex2oatClassLoaderContextTest, ContextWithOtherDexFiles) {
  std::vector<std::unique_ptr<const DexFile>> dex_files = OpenTestDexFiles("Nested");

  uint32_t expected_checksum = DexFileLoader::GetMultiDexChecksum(dex_files);

  std::string context = "PCL[" + dex_files[0]->GetLocation() + "]";
  std::string expected_classpath_key =
      "PCL[" + dex_files[0]->GetLocation() + "*" + std::to_string(expected_checksum) + "]";
  RunTest(context.c_str(), expected_classpath_key.c_str(), /*expect_status=*/Status::kSuccess);
}

TEST_F(Dex2oatClassLoaderContextTest, ContextWithResourceOnlyDexFiles) {
  std::string resource_only_classpath = GetScratchDir() + "/resource_only_classpath.jar";
  Copy(GetResourceOnlySrc1(), resource_only_classpath);

  std::string context = "PCL[" + resource_only_classpath + "]";
  // Expect an empty context because resource only dex files cannot be open.
  RunTest(context.c_str(), kEmptyClassPathKey, /*expect_status=*/Status::kSuccess);
}

TEST_F(Dex2oatClassLoaderContextTest, ContextWithNotExistentDexFiles) {
  std::string context = "PCL[does_not_exists.dex]";
  // Expect an empty context because stripped dex files cannot be open.
  RunTest(context.c_str(), kEmptyClassPathKey, /*expect_status=*/Status::kSuccess);
}

TEST_F(Dex2oatClassLoaderContextTest, ChainContext) {
  std::vector<std::unique_ptr<const DexFile>> dex_files1 = OpenTestDexFiles("Nested");
  std::vector<std::unique_ptr<const DexFile>> dex_files2 = OpenTestDexFiles("MultiDex");

  std::string context =
      "PCL[" + GetTestDexFileName("Nested") + "];" + "DLC[" + GetTestDexFileName("MultiDex") + "]";
  std::string expected_classpath_key = "PCL[" + CreateClassPathWithChecksums(dex_files1) + "];" +
                                       "DLC[" + CreateClassPathWithChecksums(dex_files2) + "]";

  RunTest(context.c_str(), expected_classpath_key.c_str(), /*expect_status=*/Status::kSuccess);
}

TEST_F(Dex2oatClassLoaderContextTest, ContextWithSharedLibrary) {
  std::vector<std::unique_ptr<const DexFile>> dex_files1 = OpenTestDexFiles("Nested");
  std::vector<std::unique_ptr<const DexFile>> dex_files2 = OpenTestDexFiles("MultiDex");

  std::string context =
      "PCL[" + GetTestDexFileName("Nested") + "]" + "{PCL[" + GetTestDexFileName("MultiDex") + "]}";
  std::string expected_classpath_key = "PCL[" + CreateClassPathWithChecksums(dex_files1) + "]" +
                                       "{PCL[" + CreateClassPathWithChecksums(dex_files2) + "]}";
  RunTest(context.c_str(), expected_classpath_key.c_str(), /*expect_status=*/Status::kSuccess);
}

TEST_F(Dex2oatClassLoaderContextTest, ContextWithSharedLibraryAndImage) {
  std::vector<std::unique_ptr<const DexFile>> dex_files1 = OpenTestDexFiles("Nested");
  std::vector<std::unique_ptr<const DexFile>> dex_files2 = OpenTestDexFiles("MultiDex");

  std::string context =
      "PCL[" + GetTestDexFileName("Nested") + "]" + "{PCL[" + GetTestDexFileName("MultiDex") + "]}";
  std::string expected_classpath_key = "PCL[" + CreateClassPathWithChecksums(dex_files1) + "]" +
                                       "{PCL[" + CreateClassPathWithChecksums(dex_files2) + "]}";
  RunTest(context.c_str(),
          expected_classpath_key.c_str(),
          /*expect_status=*/Status::kSuccess,
          /*use_second_source=*/false,
          /*generate_image=*/true);
}

TEST_F(Dex2oatClassLoaderContextTest, ContextWithSameSharedLibrariesAndImage) {
  std::vector<std::unique_ptr<const DexFile>> dex_files1 = OpenTestDexFiles("Nested");
  std::vector<std::unique_ptr<const DexFile>> dex_files2 = OpenTestDexFiles("MultiDex");

  std::string context = "PCL[" + GetTestDexFileName("Nested") + "]" + "{PCL[" +
                        GetTestDexFileName("MultiDex") + "]" + "#PCL[" +
                        GetTestDexFileName("MultiDex") + "]}";
  std::string expected_classpath_key = "PCL[" + CreateClassPathWithChecksums(dex_files1) + "]" +
                                       "{PCL[" + CreateClassPathWithChecksums(dex_files2) + "]" +
                                       "#PCL[" + CreateClassPathWithChecksums(dex_files2) + "]}";
  RunTest(context.c_str(),
          expected_classpath_key.c_str(),
          /*expect_status=*/Status::kSuccess,
          /*use_second_source=*/false,
          /*generate_image=*/true);
}

TEST_F(Dex2oatClassLoaderContextTest, ContextWithSharedLibrariesDependenciesAndImage) {
  std::vector<std::unique_ptr<const DexFile>> dex_files1 = OpenTestDexFiles("Nested");
  std::vector<std::unique_ptr<const DexFile>> dex_files2 = OpenTestDexFiles("MultiDex");

  std::string context = "PCL[" + GetTestDexFileName("Nested") + "]" + "{PCL[" +
                        GetTestDexFileName("MultiDex") + "]" + "{PCL[" +
                        GetTestDexFileName("Nested") + "]}}";
  std::string expected_classpath_key = "PCL[" + CreateClassPathWithChecksums(dex_files1) + "]" +
                                       "{PCL[" + CreateClassPathWithChecksums(dex_files2) + "]" +
                                       "{PCL[" + CreateClassPathWithChecksums(dex_files1) + "]}}";
  RunTest(context.c_str(),
          expected_classpath_key.c_str(),
          /*expect_status=*/Status::kSuccess,
          /*use_second_source=*/false,
          /*generate_image=*/true);
}

class Dex2oatDeterminism : public Dex2oatTest {};

TEST_F(Dex2oatDeterminism, UnloadCompile) {
  Runtime* const runtime = Runtime::Current();
  std::string out_dir = GetScratchDir();
  const std::string base_oat_name = out_dir + "/base.oat";
  const std::string base_vdex_name = out_dir + "/base.vdex";
  const std::string unload_oat_name = out_dir + "/unload.oat";
  const std::string unload_vdex_name = out_dir + "/unload.vdex";
  const std::string no_unload_oat_name = out_dir + "/nounload.oat";
  const std::string no_unload_vdex_name = out_dir + "/nounload.vdex";
  const std::vector<gc::space::ImageSpace*>& spaces = runtime->GetHeap()->GetBootImageSpaces();
  ASSERT_GT(spaces.size(), 0u);
  const std::string image_location = spaces[0]->GetImageLocation();
  // Without passing in an app image, it will unload in between compilations.
  ASSERT_THAT(GenerateOdexForTestWithStatus(GetLibCoreDexFileNames(),
                                            base_oat_name,
                                            CompilerFilter::Filter::kVerify,
                                            {"--force-determinism", "--avoid-storing-invocation"}),
              HasValue(0));
  Copy(base_oat_name, unload_oat_name);
  Copy(base_vdex_name, unload_vdex_name);
  std::unique_ptr<File> unload_oat(OS::OpenFileForReading(unload_oat_name.c_str()));
  std::unique_ptr<File> unload_vdex(OS::OpenFileForReading(unload_vdex_name.c_str()));
  ASSERT_TRUE(unload_oat != nullptr);
  ASSERT_TRUE(unload_vdex != nullptr);
  EXPECT_GT(unload_oat->GetLength(), 0u);
  EXPECT_GT(unload_vdex->GetLength(), 0u);
  // Regenerate with an app image to disable the dex2oat unloading and verify that the output is
  // the same.
  ASSERT_THAT(GenerateOdexForTestWithStatus(
                  GetLibCoreDexFileNames(),
                  base_oat_name,
                  CompilerFilter::Filter::kVerify,
                  {"--force-determinism", "--avoid-storing-invocation", "--compile-individually"}),
              HasValue(0));
  Copy(base_oat_name, no_unload_oat_name);
  Copy(base_vdex_name, no_unload_vdex_name);
  std::unique_ptr<File> no_unload_oat(OS::OpenFileForReading(no_unload_oat_name.c_str()));
  std::unique_ptr<File> no_unload_vdex(OS::OpenFileForReading(no_unload_vdex_name.c_str()));
  ASSERT_TRUE(no_unload_oat != nullptr);
  ASSERT_TRUE(no_unload_vdex != nullptr);
  EXPECT_GT(no_unload_oat->GetLength(), 0u);
  EXPECT_GT(no_unload_vdex->GetLength(), 0u);
  // Verify that both of the files are the same (odex and vdex).
  EXPECT_EQ(unload_oat->GetLength(), no_unload_oat->GetLength());
  EXPECT_EQ(unload_vdex->GetLength(), no_unload_vdex->GetLength());
  EXPECT_EQ(unload_oat->Compare(no_unload_oat.get()), 0)
      << unload_oat_name << " " << no_unload_oat_name;
  EXPECT_EQ(unload_vdex->Compare(no_unload_vdex.get()), 0)
      << unload_vdex_name << " " << no_unload_vdex_name;
}

class Dex2oatVerifierAbort : public Dex2oatTest {};

TEST_F(Dex2oatVerifierAbort, HardFail) {
  // Use VerifierDeps as it has hard-failing classes.
  std::unique_ptr<const DexFile> dex(OpenTestDexFile("VerifierDeps"));
  std::string out_dir = GetScratchDir();
  const std::string base_oat_name = out_dir + "/base.oat";

  EXPECT_THAT(GenerateOdexForTestWithStatus({dex->GetLocation()},
                                            base_oat_name,
                                            CompilerFilter::Filter::kVerify,
                                            {"--abort-on-hard-verifier-error"}),
              HasValue(Ne(0)));

  EXPECT_THAT(GenerateOdexForTestWithStatus({dex->GetLocation()},
                                            base_oat_name,
                                            CompilerFilter::Filter::kVerify,
                                            {"--no-abort-on-hard-verifier-error"}),
              HasValue(0));
}

class Dex2oatDedupeCode : public Dex2oatTest {};

TEST_F(Dex2oatDedupeCode, DedupeTest) {
  // Use MyClassNatives. It has lots of native methods that will produce deduplicate-able code.
  std::unique_ptr<const DexFile> dex(OpenTestDexFile("MyClassNatives"));
  std::string out_dir = GetScratchDir();
  const std::string base_oat_name = out_dir + "/base.oat";
  size_t no_dedupe_size = 0;
  ASSERT_TRUE(
      GenerateOdexForTest(dex->GetLocation(),
                          base_oat_name,
                          CompilerFilter::Filter::kSpeed,
                          {"--deduplicate-code=false"},
                          /*expect_status=*/Status::kSuccess,
                          /*use_fd=*/false,
                          /*use_zip_fd=*/false,
                          [&no_dedupe_size](const OatFile& o) { no_dedupe_size = o.Size(); }));

  size_t dedupe_size = 0;
  ASSERT_TRUE(GenerateOdexForTest(dex->GetLocation(),
                                  base_oat_name,
                                  CompilerFilter::Filter::kSpeed,
                                  {"--deduplicate-code=true"},
                                  /*expect_status=*/Status::kSuccess,
                                  /*use_fd=*/false,
                                  /*use_zip_fd=*/false,
                                  [&dedupe_size](const OatFile& o) { dedupe_size = o.Size(); }));

  EXPECT_LT(dedupe_size, no_dedupe_size);
}

TEST_F(Dex2oatTest, UncompressedTest) {
  std::unique_ptr<const DexFile> dex(OpenTestDexFile("MainUncompressedAligned"));
  std::string out_dir = GetScratchDir();
  const std::string base_oat_name = out_dir + "/base.oat";
  ASSERT_TRUE(GenerateOdexForTest(dex->GetLocation(),
                                  base_oat_name,
                                  CompilerFilter::Filter::kVerify,
                                  {},
                                  /*expect_status=*/Status::kSuccess,
                                  /*use_fd=*/false,
                                  /*use_zip_fd=*/false,
                                  [](const OatFile& o) { CHECK(!o.ContainsDexCode()); }));
}

TEST_F(Dex2oatTest, MissingBootImageTest) {
  std::string out_dir = GetScratchDir();
  const std::string base_oat_name = out_dir + "/base.oat";
  // The compilation should succeed even without the boot image.
  ASSERT_TRUE(GenerateOdexForTest(
      {GetTestDexFileName("MainUncompressedAligned")},
      base_oat_name,
      CompilerFilter::Filter::kVerify,
      // Note: Extra options go last and the second `--boot-image` option overrides the first.
      {"--boot-image=/nonx/boot.art"}));
}

TEST_F(Dex2oatTest, EmptyUncompressedDexTest) {
  std::string out_dir = GetScratchDir();
  const std::string base_oat_name = out_dir + "/base.oat";
  // Expect to fail with code 1 and not SIGSEGV or SIGABRT.
  EXPECT_THAT(GenerateOdexForTestWithStatus({GetTestDexFileName("MainEmptyUncompressed")},
                                            base_oat_name,
                                            CompilerFilter::Filter::kVerify,
                                            /*extra_args*/ {},
                                            /*use_fd*/ false),
              HasValue(1));
}

TEST_F(Dex2oatTest, EmptyUncompressedAlignedDexTest) {
  std::string out_dir = GetScratchDir();
  const std::string base_oat_name = out_dir + "/base.oat";
  // Expect to fail with code 1 and not SIGSEGV or SIGABRT.
  EXPECT_THAT(GenerateOdexForTestWithStatus({GetTestDexFileName("MainEmptyUncompressedAligned")},
                                            base_oat_name,
                                            CompilerFilter::Filter::kVerify,
                                            /*extra_args*/ {},
                                            /*use_fd*/ false),
              HasValue(1));
}

TEST_F(Dex2oatTest, StderrLoggerOutput) {
  std::string dex_location = GetScratchDir() + "/Dex2OatStderrLoggerTest.jar";
  std::string odex_location = GetOdexDir() + "/Dex2OatStderrLoggerTest.odex";

  // Test file doesn't matter.
  Copy(GetDexSrc1(), dex_location);

  ASSERT_TRUE(GenerateOdexForTest(dex_location,
                                  odex_location,
                                  CompilerFilter::kVerify,
                                  {"--runtime-arg", "-Xuse-stderr-logger"},
                                  /*expect_status=*/Status::kSuccess));
  // Look for some random part of dex2oat logging. With the stderr logger this should be captured,
  // even on device.
  EXPECT_NE(std::string::npos, output_.find("dex2oat took"));
}

TEST_F(Dex2oatTest, VerifyCompilationReason) {
  std::string dex_location = GetScratchDir() + "/Dex2OatCompilationReason.jar";
  std::string odex_location = GetOdexDir() + "/Dex2OatCompilationReason.odex";

  // Test file doesn't matter.
  Copy(GetDexSrc1(), dex_location);

  ASSERT_TRUE(GenerateOdexForTest(dex_location,
                                  odex_location,
                                  CompilerFilter::kVerify,
                                  {"--compilation-reason=install"},
                                  /*expect_status=*/Status::kSuccess));
  std::string error_msg;
  std::unique_ptr<OatFile> odex_file(OatFile::Open(/*zip_fd=*/-1,
                                                   odex_location,
                                                   odex_location,
                                                   /*executable=*/false,
                                                   /*low_4gb=*/false,
                                                   dex_location,
                                                   &error_msg));
  ASSERT_TRUE(odex_file != nullptr);
  ASSERT_STREQ("install", odex_file->GetCompilationReason());
}

TEST_F(Dex2oatTest, VerifyNoCompilationReason) {
  std::string dex_location = GetScratchDir() + "/Dex2OatNoCompilationReason.jar";
  std::string odex_location = GetOdexDir() + "/Dex2OatNoCompilationReason.odex";

  // Test file doesn't matter.
  Copy(GetDexSrc1(), dex_location);

  ASSERT_TRUE(GenerateOdexForTest(dex_location,
                                  odex_location,
                                  CompilerFilter::kVerify,
                                  /*extra_args=*/{},
                                  /*expect_status=*/Status::kSuccess));
  std::string error_msg;
  std::unique_ptr<OatFile> odex_file(OatFile::Open(/*zip_fd=*/-1,
                                                   odex_location,
                                                   odex_location,
                                                   /*executable=*/false,
                                                   /*low_4gb=*/false,
                                                   dex_location,
                                                   &error_msg));
  ASSERT_TRUE(odex_file != nullptr);
  ASSERT_EQ(nullptr, odex_file->GetCompilationReason());
}

TEST_F(Dex2oatTest, DontExtract) {
  std::unique_ptr<const DexFile> dex(OpenTestDexFile("ManyMethods"));
  std::string error_msg;
  const std::string out_dir = GetScratchDir();
  const std::string dex_location = dex->GetLocation();
  const std::string odex_location = out_dir + "/base.oat";
  const std::string vdex_location = out_dir + "/base.vdex";
  ASSERT_TRUE(GenerateOdexForTest(dex_location,
                                  odex_location,
                                  CompilerFilter::Filter::kVerify,
                                  {"--copy-dex-files=false"},
                                  /*expect_status=*/Status::kSuccess,
                                  /*use_fd=*/false,
                                  /*use_zip_fd=*/false,
                                  [](const OatFile&) {}));
  {
    // Check the vdex doesn't have dex.
    std::unique_ptr<VdexFile> vdex(VdexFile::Open(vdex_location,
                                                  /*low_4gb=*/false,
                                                  &error_msg));
    ASSERT_TRUE(vdex != nullptr);
    EXPECT_FALSE(vdex->HasDexSection()) << output_;
  }
  std::unique_ptr<OatFile> odex_file(OatFile::Open(/*zip_fd=*/-1,
                                                   odex_location,
                                                   odex_location,
                                                   /*executable=*/false,
                                                   /*low_4gb=*/false,
                                                   dex_location,
                                                   &error_msg));
  ASSERT_TRUE(odex_file != nullptr) << dex_location;
  std::vector<const OatDexFile*> oat_dex_files = odex_file->GetOatDexFiles();
  ASSERT_EQ(oat_dex_files.size(), 1u);
  // Verify that the oat file can still open the dex files.
  for (const OatDexFile* oat_dex : oat_dex_files) {
    std::unique_ptr<const DexFile> dex_file(oat_dex->OpenDexFile(&error_msg));
    ASSERT_TRUE(dex_file != nullptr) << error_msg;
  }
  // Create a dm file and use it to verify.
  // Add produced artifacts to a zip file that doesn't contain the classes.dex.
  ScratchFile dm_file;
  {
    std::unique_ptr<File> vdex_file(OS::OpenFileForReading(vdex_location.c_str()));
    ASSERT_TRUE(vdex_file != nullptr);
    ASSERT_GT(vdex_file->GetLength(), 0u);
    FILE* file = fdopen(DupCloexec(dm_file.GetFd()), "w+b");
    ZipWriter writer(file);
    auto write_all_bytes = [&](File* file) {
      std::unique_ptr<uint8_t[]> bytes(new uint8_t[file->GetLength()]);
      ASSERT_TRUE(file->ReadFully(&bytes[0], file->GetLength()));
      ASSERT_GE(writer.WriteBytes(&bytes[0], file->GetLength()), 0);
    };
    // Add vdex to zip.
    writer.StartEntry(VdexFile::kVdexNameInDmFile, ZipWriter::kCompress);
    write_all_bytes(vdex_file.get());
    writer.FinishEntry();
    writer.Finish();
    ASSERT_EQ(dm_file.GetFile()->Flush(), 0);
  }

  auto generate_and_check = [&](CompilerFilter::Filter filter) {
    output_.clear();
    ASSERT_TRUE(GenerateOdexForTest(dex_location,
                                    odex_location,
                                    filter,
                                    {"--dump-timings",
                                     "--dm-file=" + dm_file.GetFilename(),
                                     // Pass -Xuse-stderr-logger have dex2oat output in output_ on
                                     // target.
                                     "--runtime-arg",
                                     "-Xuse-stderr-logger"},
                                    /*expect_status=*/Status::kSuccess,
                                    /*use_fd=*/false,
                                    /*use_zip_fd=*/false,
                                    [](const OatFile& o) { CHECK(o.ContainsDexCode()); }));
    // Check the output for "Fast verify", this is printed from --dump-timings.
    std::istringstream iss(output_);
    std::string line;
    bool found_fast_verify = false;
    const std::string kFastVerifyString = "Fast Verify";
    while (std::getline(iss, line) && !found_fast_verify) {
      found_fast_verify = found_fast_verify || line.find(kFastVerifyString) != std::string::npos;
    }
    EXPECT_TRUE(found_fast_verify) << "Expected to find " << kFastVerifyString << "\n" << output_;
  };

  // Use verify compiler filter to check that FastVerify works for that filter too.
  generate_and_check(CompilerFilter::Filter::kVerify);
}

// Test that compact dex generation with invalid dex files doesn't crash dex2oat. b/75970654
TEST_F(Dex2oatTest, CompactDexInvalidSource) {
  ScratchFile invalid_dex;
  {
    FILE* file = fdopen(DupCloexec(invalid_dex.GetFd()), "w+b");
    ZipWriter writer(file);
    writer.StartEntry("classes.dex", ZipWriter::kAlign32);
    DexFile::Header header = {};
    StandardDexFile::WriteMagic(header.magic_.data());
    StandardDexFile::WriteCurrentVersion(header.magic_.data());
    header.file_size_ = 4 * KB;
    header.data_size_ = 4 * KB;
    header.data_off_ = 10 * MB;
    header.map_off_ = 10 * MB;
    header.class_defs_off_ = 10 * MB;
    header.class_defs_size_ = 10000;
    ASSERT_GE(writer.WriteBytes(&header, sizeof(header)), 0);
    writer.FinishEntry();
    writer.Finish();
    ASSERT_EQ(invalid_dex.GetFile()->Flush(), 0);
  }
  const std::string& dex_location = invalid_dex.GetFilename();
  const std::string odex_location = GetOdexDir() + "/output.odex";
  EXPECT_THAT(GenerateOdexForTestWithStatus(
                  {dex_location}, odex_location, CompilerFilter::kVerify, /*extra_args*/ {}),
              HasValue(Ne(0)))
      << " " << output_;
}

// Retain the header magic for the now removed compact dex files.
class LegacyCompactDexFile : public DexFile {
 public:
  static constexpr uint8_t kDexMagic[kDexMagicSize] = { 'c', 'd', 'e', 'x' };
  static constexpr uint8_t kDexMagicVersion[] = {'0', '0', '1', '\0'};

  static void WriteMagic(uint8_t* magic) {
    std::copy_n(kDexMagic, kDexMagicSize, magic);
  }

  static void WriteCurrentVersion(uint8_t* magic) {
    std::copy_n(kDexMagicVersion, kDexVersionLen, magic + kDexMagicSize);
  }
};

// Test that dex2oat with a legacy CompactDex file in the APK fails.
TEST_F(Dex2oatTest, CompactDexInZip) {
  LegacyCompactDexFile::Header header = {};
  LegacyCompactDexFile::WriteMagic(header.magic_.data());
  LegacyCompactDexFile::WriteCurrentVersion(header.magic_.data());
  header.file_size_ = sizeof(LegacyCompactDexFile::Header);
  header.map_off_ = 10 * MB;
  header.class_defs_off_ = 10 * MB;
  header.class_defs_size_ = 10000;
  // Create a zip containing the invalid dex.
  ScratchFile invalid_dex_zip;
  {
    FILE* file = fdopen(DupCloexec(invalid_dex_zip.GetFd()), "w+b");
    ZipWriter writer(file);
    writer.StartEntry("classes.dex", ZipWriter::kCompress);
    ASSERT_GE(writer.WriteBytes(&header, sizeof(header)), 0);
    writer.FinishEntry();
    writer.Finish();
    ASSERT_EQ(invalid_dex_zip.GetFile()->Flush(), 0);
  }
  // Create the dex file directly.
  ScratchFile invalid_dex;
  {
    ASSERT_GE(invalid_dex.GetFile()->WriteFully(&header, sizeof(header)), 0);
    ASSERT_EQ(invalid_dex.GetFile()->Flush(), 0);
  }

  EXPECT_THAT(GenerateOdexForTestWithStatus({invalid_dex_zip.GetFilename()},
                                            GetOdexDir() + "/output_apk.odex",
                                            CompilerFilter::kVerify,
                                            /*extra_args*/ {}),
              HasValue(Ne(0)))
      << " " << output_;

  EXPECT_THAT(GenerateOdexForTestWithStatus({invalid_dex.GetFilename()},
                                            GetOdexDir() + "/output.odex",
                                            CompilerFilter::kVerify,
                                            /*extra_args*/ {}),
              HasValue(Ne(0)))
      << " " << output_;
}

TEST_F(Dex2oatWithExpectedFilterTest, AppImageNoProfile) {
  // Set the expected filter.
  expected_filter_ = CompilerFilter::Filter::kVerify;

  ScratchFile app_image_file;
  const std::string out_dir = GetScratchDir();
  const std::string odex_location = out_dir + "/base.odex";
  ASSERT_TRUE(GenerateOdexForTest(GetTestDexFileName("ManyMethods"),
                                  odex_location,
                                  CompilerFilter::Filter::kSpeedProfile,
                                  {"--app-image-fd=" + std::to_string(app_image_file.GetFd())},
                                  /*expect_status=*/Status::kSuccess,
                                  /*use_fd=*/false,
                                  /*use_zip_fd=*/false,
                                  [](const OatFile&) {}));
  // Open our generated oat file.
  std::string error_msg;
  std::unique_ptr<OatFile> odex_file(OatFile::Open(/*zip_fd=*/-1,
                                                   odex_location,
                                                   odex_location,
                                                   /*executable=*/false,
                                                   /*low_4gb=*/false,
                                                   &error_msg));
  ASSERT_TRUE(odex_file != nullptr);
  ImageHeader header = {};
  ASSERT_TRUE(app_image_file.GetFile()->PreadFully(reinterpret_cast<void*>(&header),
                                                   sizeof(header),
                                                   /*offset*/ 0u))
      << app_image_file.GetFile()->GetLength();
  EXPECT_GT(header.GetImageSection(ImageHeader::kSectionObjects).Size(), 0u);
  EXPECT_EQ(header.GetImageSection(ImageHeader::kSectionArtMethods).Size(), 0u);
  EXPECT_EQ(header.GetImageSection(ImageHeader::kSectionArtFields).Size(), 0u);
}

TEST_F(Dex2oatTest, ZipFd) {
  std::string zip_location = GetTestDexFileName("MainUncompressedAligned");
  std::unique_ptr<File> dex_file(OS::OpenFileForReading(zip_location.c_str()));
  std::vector<std::string> extra_args{
      StringPrintf("--zip-fd=%d", dex_file->Fd()),
      "--zip-location=" + zip_location,
  };
  std::string out_dir = GetScratchDir();
  const std::string base_oat_name = out_dir + "/base.oat";
  ASSERT_TRUE(GenerateOdexForTest(zip_location,
                                  base_oat_name,
                                  CompilerFilter::Filter::kVerify,
                                  extra_args,
                                  /*expect_status=*/Status::kSuccess,
                                  /*use_fd=*/false,
                                  /*use_zip_fd=*/true));
}

TEST_F(Dex2oatWithExpectedFilterTest, AppImageEmptyDex) {
  // Set the expected filter.
  expected_filter_ = CompilerFilter::Filter::kVerify;

  // Create a profile with the startup method marked.
  ScratchFile profile_file;
  ScratchFile temp_dex;
  const std::string& dex_location = temp_dex.GetFilename();
  std::vector<uint16_t> methods;
  std::vector<dex::TypeIndex> classes;
  {
    MutateDexFile(temp_dex.GetFile(), GetTestDexFileName("StringLiterals"), [&](DexFile* dex) {
      // Modify the header to make the dex file valid but empty.
      DexFile::Header* header = const_cast<DexFile::Header*>(&dex->GetHeader());
      header->string_ids_size_ = 0;
      header->string_ids_off_ = 0;
      header->type_ids_size_ = 0;
      header->type_ids_off_ = 0;
      header->proto_ids_size_ = 0;
      header->proto_ids_off_ = 0;
      header->field_ids_size_ = 0;
      header->field_ids_off_ = 0;
      header->method_ids_size_ = 0;
      header->method_ids_off_ = 0;
      header->class_defs_size_ = 0;
      header->class_defs_off_ = 0;
      ASSERT_GT(header->file_size_,
                sizeof(*header) + sizeof(dex::MapList) + sizeof(dex::MapItem) * 2);
      // Move map list to be right after the header.
      header->map_off_ = header->header_size_;
      dex::MapList* map_list = const_cast<dex::MapList*>(dex->GetMapList());
      map_list->list_[0].type_ = DexFile::kDexTypeHeaderItem;
      map_list->list_[0].size_ = 1u;
      map_list->list_[0].offset_ = 0u;
      map_list->list_[1].type_ = DexFile::kDexTypeMapList;
      map_list->list_[1].size_ = 1u;
      map_list->list_[1].offset_ = header->map_off_;
      map_list->size_ = 2;
      header->data_off_ = header->map_off_;
      header->data_size_ = map_list->Size();
      header->SetDexContainer(0, header->file_size_);
    });
  }
  std::unique_ptr<const DexFile> dex_file(OpenDexFile(temp_dex.GetFilename().c_str()));
  const std::string out_dir = GetScratchDir();
  const std::string odex_location = out_dir + "/base.odex";
  const std::string app_image_location = out_dir + "/base.art";
  ASSERT_TRUE(GenerateOdexForTest(dex_location,
                                  odex_location,
                                  CompilerFilter::Filter::kSpeedProfile,
                                  {"--app-image-file=" + app_image_location,
                                   "--resolve-startup-const-strings=true",
                                   "--profile-file=" + profile_file.GetFilename()},
                                  /*expect_status=*/Status::kSuccess,
                                  /*use_fd=*/false,
                                  /*use_zip_fd=*/false,
                                  [](const OatFile&) {}));
  // Open our generated oat file.
  std::string error_msg;
  std::unique_ptr<OatFile> odex_file(OatFile::Open(/*zip_fd=*/-1,
                                                   odex_location,
                                                   odex_location,
                                                   /*executable=*/false,
                                                   /*low_4gb=*/false,
                                                   &error_msg));
  ASSERT_TRUE(odex_file != nullptr);
}

TEST_F(Dex2oatWithExpectedFilterTest, AppImageNonexistentDex) {
  const std::string out_dir = GetScratchDir();
  // Test that dex2oat does not crash trying to compile app image with zero DEX files.
  ASSERT_TRUE(GenerateOdexForTest(
      out_dir + "/base.apk",
      out_dir + "/base.odex",
      CompilerFilter::Filter::kSpeedProfile,
      {"--dex-file=nonexistent.apk", "--app-image-file=" + out_dir + "/base.art"},
      /*expect_status=*/Status::kFailOpenOat,
      /*use_fd=*/false,
      /*use_zip_fd=*/false,
      [](const OatFile&) {}));
}

TEST_F(Dex2oatTest, DexFileFd) {
  std::string error_msg;
  std::string zip_location = GetTestDexFileName("Main");
  std::unique_ptr<File> zip_file(OS::OpenFileForReading(zip_location.c_str()));
  ASSERT_NE(-1, zip_file->Fd());

  std::unique_ptr<ZipArchive> zip_archive(
      ZipArchive::OpenFromFd(zip_file->Release(), zip_location.c_str(), &error_msg));
  ASSERT_TRUE(zip_archive != nullptr);

  std::string entry_name = DexFileLoader::GetMultiDexClassesDexName(0);
  std::unique_ptr<ZipEntry> entry(zip_archive->Find(entry_name.c_str(), &error_msg));
  ASSERT_TRUE(entry != nullptr);

  ScratchFile dex_file;
  const std::string& dex_location = dex_file.GetFilename();
  const std::string base_oat_name = GetScratchDir() + "/base.oat";

  bool success = entry->ExtractToFile(*(dex_file.GetFile()), &error_msg);
  ASSERT_TRUE(success);
  ASSERT_EQ(0, lseek(dex_file.GetFd(), 0, SEEK_SET));

  std::vector<std::string> extra_args{
      StringPrintf("--zip-fd=%d", dex_file.GetFd()),
      "--zip-location=" + dex_location,
  };
  ASSERT_TRUE(GenerateOdexForTest(dex_location,
                                  base_oat_name,
                                  CompilerFilter::Filter::kVerify,
                                  extra_args,
                                  /*expect_status=*/Status::kSuccess,
                                  /*use_fd=*/false,
                                  /*use_zip_fd=*/true));
}

TEST_F(Dex2oatTest, DontCopyPlainDex) {
  std::unique_ptr<const DexFile> dex(OpenTestDexFile("VerifierDepsMulti"));
  std::string error_msg;
  const std::string out_dir = GetScratchDir();
  const std::string dex_location = dex->GetLocation();
  const std::string odex_location = out_dir + "/base.oat";
  const std::string vdex_location = out_dir + "/base.vdex";
  ASSERT_TRUE(GenerateOdexForTest(dex_location,
                                  odex_location,
                                  CompilerFilter::Filter::kVerify,
                                  /*extra_args=*/{},
                                  /*expect_status=*/Status::kSuccess,
                                  /*use_fd=*/false,
                                  /*use_zip_fd=*/false,
                                  [](const OatFile&) {}));

  // Check that the vdex doesn't have dex code.
  std::unique_ptr<VdexFile> vdex(VdexFile::Open(vdex_location,
                                                /*low_4gb=*/false,
                                                &error_msg));
  ASSERT_TRUE(vdex != nullptr);
  EXPECT_FALSE(vdex->HasDexSection()) << output_;
}

TEST_F(Dex2oatTest, AppImageResolveStrings) {
  using Hotness = ProfileCompilationInfo::MethodHotness;
  // Create a profile with the startup method marked.
  ScratchFile profile_file;
  ScratchFile temp_dex;
  const std::string& dex_location = temp_dex.GetFilename();
  std::vector<uint16_t> methods;
  std::vector<dex::TypeIndex> classes;
  {
    MutateDexFile(
        temp_dex.GetFile(), GetTestDexFileName("StringLiterals"), [&](DexFile* dex) {
          bool mutated_successfully = false;
          // Change the dex instructions to make an opcode that spans past the end of the code item.
          for (ClassAccessor accessor : dex->GetClasses()) {
            if (accessor.GetDescriptorView() == "LStringLiterals$StartupClass;") {
              classes.push_back(accessor.GetClassIdx());
            }
            for (const ClassAccessor::Method& method : accessor.GetMethods()) {
              std::string method_name(dex->GetMethodName(dex->GetMethodId(method.GetIndex())));
              CodeItemInstructionAccessor instructions = method.GetInstructions();
              if (method_name == "startUpMethod2") {
                // Make an instruction that runs past the end of the code item and verify that it
                // doesn't cause dex2oat to crash.
                ASSERT_TRUE(instructions.begin() != instructions.end());
                DexInstructionIterator last_instruction = instructions.begin();
                for (auto dex_it = instructions.begin(); dex_it != instructions.end(); ++dex_it) {
                  last_instruction = dex_it;
                }
                ASSERT_EQ(last_instruction->SizeInCodeUnits(), 1u);
                // Set the opcode to something that will go past the end of the code item.
                const_cast<Instruction&>(last_instruction.Inst())
                    .SetOpcode(Instruction::CONST_STRING_JUMBO);
                mutated_successfully = true;
                methods.push_back(method.GetIndex());
                mutated_successfully = true;
              } else if (method_name == "startUpMethod") {
                methods.push_back(method.GetIndex());
              }
            }
          }
          CHECK(mutated_successfully)
              << "Failed to find candidate code item with only one code unit in last instruction.";
        });
  }
  std::unique_ptr<const DexFile> dex_file(OpenDexFile(temp_dex.GetFilename().c_str()));
  {
    ASSERT_GT(classes.size(), 0u);
    ASSERT_GT(methods.size(), 0u);
    // Here, we build the profile from the method lists.
    ProfileCompilationInfo info;
    info.AddClassesForDex(dex_file.get(), classes.begin(), classes.end());
    info.AddMethodsForDex(Hotness::kFlagStartup, dex_file.get(), methods.begin(), methods.end());
    // Save the profile since we want to use it with dex2oat to produce an oat file.
    ASSERT_TRUE(info.Save(profile_file.GetFd()));
  }
  const std::string out_dir = GetScratchDir();
  const std::string odex_location = out_dir + "/base.odex";
  const std::string app_image_location = out_dir + "/base.art";
  ASSERT_TRUE(GenerateOdexForTest(dex_location,
                                  odex_location,
                                  CompilerFilter::Filter::kSpeedProfile,
                                  {"--app-image-file=" + app_image_location,
                                   "--resolve-startup-const-strings=true",
                                   "--profile-file=" + profile_file.GetFilename()},
                                  /*expect_status=*/Status::kSuccess,
                                  /*use_fd=*/false,
                                  /*use_zip_fd=*/false,
                                  [](const OatFile&) {}));
  // Open our generated oat file.
  std::string error_msg;
  std::unique_ptr<OatFile> odex_file(OatFile::Open(/*zip_fd=*/-1,
                                                   odex_location,
                                                   odex_location,
                                                   /*executable=*/false,
                                                   /*low_4gb=*/false,
                                                   &error_msg));
  ASSERT_TRUE(odex_file != nullptr);
  // Check the strings in the app image intern table only contain the "startup" strigs.
  {
    std::unique_ptr<gc::space::ImageSpace> space = gc::space::ImageSpace::CreateFromAppImage(
        app_image_location.c_str(), odex_file.get(), &error_msg);
    ASSERT_TRUE(space != nullptr) << error_msg;
    ScopedObjectAccess soa(Thread::Current());
    std::set<std::string> seen;
    InternTable intern_table;
    intern_table.AddImageStringsToTable(
        space.get(), [&](InternTable::UnorderedSet& interns) REQUIRES_SHARED(Locks::mutator_lock_) {
          for (const GcRoot<mirror::String>& str : interns) {
            seen.insert(str.Read()->ToModifiedUtf8());
          }
        });
    // Normal methods
    EXPECT_TRUE(seen.find("Loading ") != seen.end());
    EXPECT_TRUE(seen.find("Starting up") != seen.end());
    EXPECT_TRUE(seen.find("abcd.apk") != seen.end());
    EXPECT_TRUE(seen.find("Unexpected error") == seen.end());
    EXPECT_TRUE(seen.find("Shutting down!") == seen.end());
    // Classes initializers
    EXPECT_TRUE(seen.find("Startup init") != seen.end());
    EXPECT_TRUE(seen.find("Other class init") == seen.end());

    // Verify what strings are marked as boot image.
    std::set<std::string> boot_image_strings;
    std::set<std::string> app_image_strings;

    MutexLock mu(Thread::Current(), *Locks::intern_table_lock_);
    intern_table.VisitInterns(
        [&](const GcRoot<mirror::String>& root) REQUIRES_SHARED(Locks::mutator_lock_) {
          boot_image_strings.insert(root.Read()->ToModifiedUtf8());
        },
        /*visit_boot_images=*/true,
        /*visit_non_boot_images=*/false);
    intern_table.VisitInterns(
        [&](const GcRoot<mirror::String>& root) REQUIRES_SHARED(Locks::mutator_lock_) {
          app_image_strings.insert(root.Read()->ToModifiedUtf8());
        },
        /*visit_boot_images=*/false,
        /*visit_non_boot_images=*/true);
    EXPECT_EQ(boot_image_strings.size(), 0u);
    EXPECT_TRUE(app_image_strings == seen);
  }
}

TEST_F(Dex2oatClassLoaderContextTest, StoredClassLoaderContext) {
  std::vector<std::unique_ptr<const DexFile>> dex_files = OpenTestDexFiles("MultiDex");
  const std::string out_dir = GetScratchDir();
  const std::string odex_location = out_dir + "/base.odex";
  const std::string valid_context = "PCL[" + dex_files[0]->GetLocation() + "]";
  const std::string stored_context = "PCL[/system/not_real_lib.jar]";
  uint32_t checksum = DexFileLoader::GetMultiDexChecksum(dex_files);
  std::string expected_stored_context =
      "PCL[/system/not_real_lib.jar*" + std::to_string(checksum) + "]";
  // The class path should not be valid and should fail being stored.
  EXPECT_TRUE(GenerateOdexForTest(GetTestDexFileName("ManyMethods"),
                                  odex_location,
                                  CompilerFilter::Filter::kVerify,
                                  {"--class-loader-context=" + stored_context},
                                  /*expect_status=*/Status::kSuccess,
                                  /*use_fd=*/false,
                                  /*use_zip_fd=*/false,
                                  [&](const OatFile& oat_file) {
                                    EXPECT_NE(oat_file.GetClassLoaderContext(), stored_context)
                                        << output_;
                                    EXPECT_NE(oat_file.GetClassLoaderContext(), valid_context)
                                        << output_;
                                  }));
  // The stored context should match what we expect even though it's invalid.
  EXPECT_TRUE(GenerateOdexForTest(
      GetTestDexFileName("ManyMethods"),
      odex_location,
      CompilerFilter::Filter::kVerify,
      {"--class-loader-context=" + valid_context,
       "--stored-class-loader-context=" + stored_context},
      /*expect_status=*/Status::kSuccess,
      /*use_fd=*/false,
      /*use_zip_fd=*/false,
      [&](const OatFile& oat_file) {
        EXPECT_EQ(oat_file.GetClassLoaderContext(), expected_stored_context) << output_;
      }));
}

class Dex2oatISAFeaturesRuntimeDetectionTest : public Dex2oatTest {
 protected:
  void RunTest(const std::vector<std::string>& extra_args = {}) {
    std::string dex_location = GetScratchDir() + "/Dex2OatSwapTest.jar";
    std::string odex_location = GetOdexDir() + "/Dex2OatSwapTest.odex";

    Copy(GetTestDexFileName(), dex_location);

    ASSERT_TRUE(
        GenerateOdexForTest(dex_location, odex_location, CompilerFilter::kSpeed, extra_args));
  }

  std::string GetTestDexFileName() { return GetDexSrc1(); }
};

TEST_F(Dex2oatISAFeaturesRuntimeDetectionTest, TestCurrentRuntimeFeaturesAsDex2OatArguments) {
  std::vector<std::string> argv;
  Runtime::Current()->AddCurrentRuntimeFeaturesAsDex2OatArguments(&argv);
  auto option_pos =
      std::find(std::begin(argv), std::end(argv), "--instruction-set-features=runtime");
  if (InstructionSetFeatures::IsRuntimeDetectionSupported()) {
    EXPECT_TRUE(kIsTargetBuild);
    EXPECT_NE(option_pos, std::end(argv));
  } else {
    EXPECT_EQ(option_pos, std::end(argv));
  }

  RunTest();
}

class LinkageTest : public Dex2oatTest {};

TEST_F(LinkageTest, LinkageEnabled) {
  TEST_DISABLED_FOR_TARGET();
  std::unique_ptr<const DexFile> dex(OpenTestDexFile("LinkageTest"));
  std::string out_dir = GetScratchDir();
  const std::string base_oat_name = out_dir + "/base.oat";
  EXPECT_THAT(
      GenerateOdexForTestWithStatus({dex->GetLocation()},
                                    base_oat_name,
                                    CompilerFilter::Filter::kSpeed,
                                    {"--check-linkage-conditions", "--crash-on-linkage-violation"}),
      Not(Ok()));

  EXPECT_THAT(GenerateOdexForTestWithStatus({dex->GetLocation()},
                                            base_oat_name,
                                            CompilerFilter::Filter::kSpeed,
                                            {"--check-linkage-conditions"}),
              HasValue(0));
}

// Regression test for bug 179221298.
TEST_F(Dex2oatTest, LoadOutOfDateOatFile) {
  std::unique_ptr<const DexFile> dex(OpenTestDexFile("ManyMethods"));
  std::string out_dir = GetScratchDir();
  const std::string base_oat_name = out_dir + "/base.oat";
  ASSERT_TRUE(GenerateOdexForTest(dex->GetLocation(),
                                  base_oat_name,
                                  CompilerFilter::Filter::kSpeed,
                                  {"--deduplicate-code=false"},
                                  /*expect_status=*/Status::kSuccess,
                                  /*use_fd=*/false,
                                  /*use_zip_fd=*/false));

  // Check that we can open the oat file as executable.
  {
    std::string error_msg;
    std::unique_ptr<OatFile> odex_file(OatFile::Open(/*zip_fd=*/-1,
                                                     base_oat_name,
                                                     base_oat_name,
                                                     /*executable=*/true,
                                                     /*low_4gb=*/false,
                                                     dex->GetLocation(),
                                                     &error_msg));
    ASSERT_TRUE(odex_file != nullptr) << error_msg;
  }

  // Rewrite the oat file with wrong version and bogus contents.
  {
    std::unique_ptr<File> file(OS::OpenFileReadWrite(base_oat_name.c_str()));
    ASSERT_TRUE(file != nullptr);
    // Retrieve the offset and size of the embedded oat file.
    size_t oatdata_offset;
    size_t oatdata_size;
    {
      std::string error_msg;
      std::unique_ptr<ElfFile> elf_file(ElfFile::Open(file.get(),
                                                      /*low_4gb=*/false,
                                                      &error_msg));
      ASSERT_TRUE(elf_file != nullptr) << error_msg;
      ASSERT_TRUE(elf_file->Load(/*executable=*/false,
                                 /*low_4gb=*/false,
                                 /*reservation=*/nullptr,
                                 &error_msg))
          << error_msg;
      const uint8_t* base_address = elf_file->GetBaseAddress();
      const uint8_t* oatdata = elf_file->FindDynamicSymbolAddress("oatdata");
      ASSERT_TRUE(oatdata != nullptr);
      ASSERT_TRUE(oatdata > base_address);
      // Note: We're assuming here that the virtual address offset is the same
      // as file offset. This is currently true for all oat files we generate.
      oatdata_offset = static_cast<size_t>(oatdata - base_address);
      const uint8_t* oatlastword = elf_file->FindDynamicSymbolAddress("oatlastword");
      ASSERT_TRUE(oatlastword != nullptr);
      ASSERT_TRUE(oatlastword > oatdata);
      oatdata_size = oatlastword - oatdata;
    }

    // Check that we have the right `oatdata_offset`.
    int64_t length = file->GetLength();
    ASSERT_GE(length, static_cast<ssize_t>(oatdata_offset + sizeof(OatHeader)));
    alignas(OatHeader) uint8_t header_data[sizeof(OatHeader)];
    ASSERT_TRUE(file->PreadFully(header_data, sizeof(header_data), oatdata_offset));
    const OatHeader& header = reinterpret_cast<const OatHeader&>(header_data);
    ASSERT_TRUE(header.IsValid()) << header.GetValidationErrorMessage();

    // Overwrite all oat data from version onwards with bytes with value 4.
    // (0x04040404 is not a valid version, we're using three decimal digits and '\0'.)
    //
    // We previously tried to find the value for key "debuggable" (bug 179221298)
    // in the key-value store before checking the oat header. This test tries to
    // ensure that such early processing of the key-value store shall crash.
    // Reading 0x04040404 as the size of the key-value store yields a bit over
    // 64MiB which should hopefully include some unmapped memory beyond the end
    // of the loaded oat file. Overwriting the whole embedded oat file ensures
    // that we do not match the key within the oat file but we could still
    // accidentally match it in the additional sections of the elf file, so this
    // approach could fail to catch similar issues. At the time of writing, this
    // test crashed when run without the fix on 64-bit host (but not 32-bit).
    static constexpr size_t kVersionOffset = sizeof(OatHeader::kOatMagic);
    static_assert(kVersionOffset < sizeof(OatHeader));
    std::vector<uint8_t> data(oatdata_size - kVersionOffset, 4u);
    ASSERT_TRUE(file->PwriteFully(data.data(), data.size(), oatdata_offset + kVersionOffset));
    UNUSED(oatdata_size);
    CHECK_EQ(file->FlushClose(), 0) << "Could not flush and close oat file";
  }

  // Check that we reject the oat file without crashing.
  {
    std::string error_msg;
    std::unique_ptr<OatFile> odex_file(OatFile::Open(/*zip_fd=*/-1,
                                                     base_oat_name,
                                                     base_oat_name,
                                                     /*executable=*/true,
                                                     /*low_4gb=*/false,
                                                     dex->GetLocation(),
                                                     &error_msg));
    ASSERT_FALSE(odex_file != nullptr);
  }
}

}  // namespace art
