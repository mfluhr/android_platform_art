/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include <string>
#include <vector>

#include "android-base/result-gmock.h"
#include "android-base/result.h"
#include "common_runtime_test.h"
#include "dex2oat_environment_test.h"
#include "vdex_file.h"
#include "verifier/verifier_deps.h"

namespace art {

using ::android::base::Result;
using ::android::base::testing::HasValue;
using verifier::VerifierDeps;

class Dex2oatVdexTest : public Dex2oatEnvironmentTest {
 public:
  void TearDown() override {
    Dex2oatEnvironmentTest::TearDown();

    output_ = "";
    opened_vdex_files_.clear();
  }

 protected:
  Result<bool> RunDex2oat(const std::string& dex_location,
                          const std::string& odex_location,
                          const std::string* public_sdk,
                          bool copy_dex_files = false,
                          const std::vector<std::string>& extra_args = {}) {
    std::vector<std::string> args;
    args.push_back("--dex-file=" + dex_location);
    args.push_back("--oat-file=" + odex_location);
    if (public_sdk != nullptr) {
      args.push_back("--public-sdk=" + *public_sdk);
    }
    args.push_back("--compiler-filter=" +
                   CompilerFilter::NameOfFilter(CompilerFilter::Filter::kVerify));
    args.push_back("--runtime-arg");
    args.push_back("-Xnorelocate");
    if (!copy_dex_files) {
      args.push_back("--copy-dex-files=false");
    }
    args.push_back("--runtime-arg");
    args.push_back("-verbose:verifier,compiler");
    // Use a single thread to facilitate debugging. We only compile tiny dex files.
    args.push_back("-j1");

    args.insert(args.end(), extra_args.begin(), extra_args.end());

    int status = OR_RETURN(Dex2Oat(args, &output_));
    return status == 0;
  }

  Result<std::unique_ptr<VerifierDeps>> GetVerifierDeps(const std::string& vdex_location,
                                                        const DexFile* dex_file) {
    // Verify the vdex file content: only the classes using public APIs should be verified.
    std::string error_msg;
    std::unique_ptr<VdexFile> vdex(VdexFile::Open(vdex_location,
                                                  /*low_4gb=*/false,
                                                  &error_msg));
    // Check the vdex doesn't have dex.
    if (vdex->HasDexSection()) {
      return Errorf("The vdex {} should not contain dex code", vdex_location);
    }

    // Verify the deps.
    VdexFile::VdexFileHeader vdex_header = vdex->GetVdexFileHeader();
    if (!vdex_header.IsValid()) {
      return Errorf("Invalid vdex header in {}", vdex_location);
    }

    std::vector<const DexFile*> dex_files;
    dex_files.push_back(dex_file);
    std::unique_ptr<VerifierDeps> deps(new VerifierDeps(dex_files, /*output_only=*/false));

    if (!deps->ParseStoredData(dex_files, vdex->GetVerifierDepsData())) {
      return Errorf("{}", error_msg);
    }

    opened_vdex_files_.push_back(std::move(vdex));
    return deps;
  }

  uint16_t GetClassDefIndex(const std::string& cls, const DexFile& dex_file) {
    const dex::TypeId* type_id = dex_file.FindTypeId(cls.c_str());
    DCHECK(type_id != nullptr);
    dex::TypeIndex type_idx = dex_file.GetIndexForTypeId(*type_id);
    const dex::ClassDef* class_def = dex_file.FindClassDef(type_idx);
    DCHECK(class_def != nullptr);
    return dex_file.GetIndexForClassDef(*class_def);
  }

  bool HasVerifiedClass(const std::unique_ptr<VerifierDeps>& deps,
                        const std::string& cls,
                        const DexFile& dex_file) {
    uint16_t class_def_idx = GetClassDefIndex(cls, dex_file);
    return deps->GetVerifiedClasses(dex_file)[class_def_idx];
  }

  std::string GetFilename(const std::unique_ptr<const DexFile>& dex_file) {
    const std::string& str = dex_file->GetLocation();
    size_t idx = str.rfind('/');
    if (idx == std::string::npos) {
      return str;
    }
    return str.substr(idx + 1);
  }

  std::string GetOdex(const std::unique_ptr<const DexFile>& dex_file,
                      const std::string& suffix = "") {
    return GetScratchDir() + "/" + GetFilename(dex_file) + suffix + ".odex";
  }

  std::string GetVdex(const std::unique_ptr<const DexFile>& dex_file,
                      const std::string& suffix = "") {
    return GetScratchDir() + "/" + GetFilename(dex_file) + suffix + ".vdex";
  }

  std::string output_;
  std::vector<std::unique_ptr<VdexFile>> opened_vdex_files_;
};

// Validates verification against public API stubs:
// - create a vdex file contraints by a predefined list of public API (passed as separate dex)
// - compile with the above vdex file as input to validate the compilation flow
TEST_F(Dex2oatVdexTest, VerifyPublicSdkStubs) {
  // Dex2oatVdexTestDex is the subject app using normal APIs found in the boot classpath.
  std::unique_ptr<const DexFile> dex_file(OpenTestDexFile("Dex2oatVdexTestDex"));
  // Dex2oatVdexPublicSdkDex serves as the public API-stubs, restricting what can be verified.
  const std::string api_dex_location = GetTestDexFileName("Dex2oatVdexPublicSdkDex");

  // Compile the subject app using the predefined API-stubs
  ASSERT_THAT(RunDex2oat(dex_file->GetLocation(), GetOdex(dex_file), &api_dex_location),
              HasValue(true));

  std::unique_ptr<VerifierDeps> deps =
      OR_ASSERT_FAIL(GetVerifierDeps(GetVdex(dex_file), dex_file.get()));

  // Verify public API usage. The classes should be verified.
  ASSERT_TRUE(HasVerifiedClass(deps, "LAccessPublicCtor;", *dex_file));
  ASSERT_TRUE(HasVerifiedClass(deps, "LAccessPublicMethod;", *dex_file));
  ASSERT_TRUE(HasVerifiedClass(deps, "LAccessPublicMethodFromParent;", *dex_file));
  ASSERT_TRUE(HasVerifiedClass(deps, "LAccessPublicStaticMethod;", *dex_file));
  ASSERT_TRUE(HasVerifiedClass(deps, "LAccessPublicStaticField;", *dex_file));

  // Verify NON public API usage. The classes should be verified, but will run
  // with access checks.
  ASSERT_TRUE(HasVerifiedClass(deps, "LAccessNonPublicCtor;", *dex_file));
  ASSERT_TRUE(HasVerifiedClass(deps, "LAccessNonPublicMethod;", *dex_file));
  ASSERT_TRUE(HasVerifiedClass(deps, "LAccessNonPublicMethodFromParent;", *dex_file));
  ASSERT_TRUE(HasVerifiedClass(deps, "LAccessNonPublicStaticMethod;", *dex_file));
  ASSERT_TRUE(HasVerifiedClass(deps, "LAccessNonPublicStaticField;", *dex_file));

  // Compile again without public API stubs but with the previously generated vdex.
  // This simulates a normal install where the apk has its code pre-verified.
  // The results should be the same.

  std::string dm_file = GetScratchDir() + "/base.dm";
  CreateDexMetadata(GetVdex(dex_file), dm_file);
  std::vector<std::string> extra_args;
  extra_args.push_back("--dm-file=" + dm_file);
  output_ = "";
  ASSERT_THAT(RunDex2oat(dex_file->GetLocation(), GetOdex(dex_file), nullptr, false, extra_args),
              HasValue(true));

  std::unique_ptr<VerifierDeps> deps2 =
      OR_ASSERT_FAIL(GetVerifierDeps(GetVdex(dex_file), dex_file.get()));

  ASSERT_TRUE(HasVerifiedClass(deps2, "LAccessPublicCtor;", *dex_file));
  ASSERT_TRUE(HasVerifiedClass(deps2, "LAccessPublicMethod;", *dex_file));
  ASSERT_TRUE(HasVerifiedClass(deps2, "LAccessPublicMethodFromParent;", *dex_file));
  ASSERT_TRUE(HasVerifiedClass(deps2, "LAccessPublicStaticMethod;", *dex_file));
  ASSERT_TRUE(HasVerifiedClass(deps2, "LAccessPublicStaticField;", *dex_file));

  ASSERT_TRUE(HasVerifiedClass(deps2, "LAccessNonPublicCtor;", *dex_file)) << output_;
  ASSERT_TRUE(HasVerifiedClass(deps2, "LAccessNonPublicMethod;", *dex_file));
  ASSERT_TRUE(HasVerifiedClass(deps2, "LAccessNonPublicMethodFromParent;", *dex_file));
  ASSERT_TRUE(HasVerifiedClass(deps2, "LAccessNonPublicStaticMethod;", *dex_file));
  ASSERT_TRUE(HasVerifiedClass(deps2, "LAccessNonPublicStaticField;", *dex_file));
}

// Check that if the input dm does contain dex files then the compilation fails
TEST_F(Dex2oatVdexTest, VerifyPublicSdkStubsWithDexFiles) {
  // Dex2oatVdexTestDex is the subject app using normal APIs found in the boot classpath.
  std::unique_ptr<const DexFile> dex_file(OpenTestDexFile("Dex2oatVdexTestDex"));

  // Compile the subject app using the predefined API-stubs
  ASSERT_THAT(RunDex2oat(dex_file->GetLocation(),
                         GetOdex(dex_file),
                         /*public_sdk=*/nullptr,
                         /*copy_dex_files=*/true),
              HasValue(true));

  // Create the .dm file with the output.
  std::string dm_file = GetScratchDir() + "/base.dm";
  CreateDexMetadata(GetVdex(dex_file), dm_file);
  std::vector<std::string> extra_args;
  extra_args.push_back("--dm-file=" + dm_file);

  // Recompile again with the .dm file which contains a vdex with code.
  // The compilation will pass, but dex2oat will not use the vdex file.
  ASSERT_THAT(RunDex2oat(dex_file->GetLocation(),
                         GetOdex(dex_file, "v2"),
                         /*public_sdk=*/nullptr,
                         /*copy_dex_files=*/true,
                         extra_args),
              HasValue(true));
}

// Check that corrupt vdex files from .dm archives are ignored.
TEST_F(Dex2oatVdexTest, VerifyCorruptVdexFile) {
  // Dex2oatVdexTestDex is the subject app using normal APIs found in the boot classpath.
  std::unique_ptr<const DexFile> dex_file(OpenTestDexFile("Dex2oatVdexTestDex"));

  // Create the .dm file with the output.
  // Instead passing the vdex files, pass the actual dex file. This will simulate a vdex corruption.
  // The compiler should ignore it.
  std::string dm_file = GetScratchDir() + "/base.dm";
  CreateDexMetadata(dex_file->GetLocation(), dm_file);
  std::vector<std::string> extra_args;
  extra_args.push_back("--dm-file=" + dm_file);

  // Compile the dex file. Despite having a corrupt input .vdex, we should not crash.
  ASSERT_THAT(RunDex2oat(dex_file->GetLocation(),
                         GetOdex(dex_file),
                         /*public_sdk=*/nullptr,
                         /*copy_dex_files=*/true,
                         extra_args),
              HasValue(true))
      << output_;
}

// Check that if the input dm a vdex with mismatching checksums the compilation fails
TEST_F(Dex2oatVdexTest, VerifyInputDmWithMismatchedChecksums) {
  // Generate a vdex file for Dex2oatVdexTestDex.
  std::unique_ptr<const DexFile> dex_file(OpenTestDexFile("Dex2oatVdexTestDex"));

  ASSERT_THAT(RunDex2oat(dex_file->GetLocation(),
                         GetOdex(dex_file),
                         /*public_sdk=*/nullptr,
                         /*copy_dex_files=*/false),
              HasValue(true));

  // Create the .dm file with the output.
  std::string dm_file = GetScratchDir() + "/base.dm";
  CreateDexMetadata(GetVdex(dex_file), dm_file);
  std::vector<std::string> extra_args;
  extra_args.push_back("--dm-file=" + dm_file);

  // Try to compile Main using an input dm which contains the vdex for
  // Dex2oatVdexTestDex. It should fail.
  std::unique_ptr<const DexFile> dex_file2(OpenTestDexFile("Main"));
  ASSERT_THAT(RunDex2oat(dex_file2->GetLocation(),
                         GetOdex(dex_file2, "v2"),
                         /*public_sdk=*/nullptr,
                         /*copy_dex_files=*/false,
                         extra_args),
              HasValue(false))
      << output_;
}

}  // namespace art
