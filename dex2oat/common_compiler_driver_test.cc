/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include <sys/mman.h>

#include "common_compiler_driver_test.h"

#include "base/casts.h"
#include "base/timing_logger.h"
#include "dex/quick_compiler_callbacks.h"
#include "dex/verification_results.h"
#include "driver/compiler_driver.h"
#include "driver/compiler_options.h"
#include "utils/atomic_dex_ref_map-inl.h"

namespace art {

CommonCompilerDriverTest::CommonCompilerDriverTest() {}
CommonCompilerDriverTest::~CommonCompilerDriverTest() {}

void CommonCompilerDriverTest::CompileAll(jobject class_loader,
                                          const std::vector<const DexFile*>& dex_files,
                                          TimingLogger* timings) {
  TimingLogger::ScopedTiming t(__FUNCTION__, timings);
  SetDexFilesForOatFile(dex_files);

  compiler_driver_->InitializeThreadPools();

  compiler_driver_->PreCompile(class_loader,
                               dex_files,
                               timings,
                               &compiler_options_->image_classes_);

  compiler_driver_->CompileAll(class_loader, dex_files, timings);
  compiler_driver_->FreeThreadPools();
}

void CommonCompilerDriverTest::SetDexFilesForOatFile(const std::vector<const DexFile*>& dex_files) {
  compiler_options_->dex_files_for_oat_file_ = dex_files;
  compiler_driver_->compiled_classes_.AddDexFiles(dex_files);
}

void CommonCompilerDriverTest::ReserveImageSpace() {
  // Reserve where the image will be loaded up front so that other parts of test set up don't
  // accidentally end up colliding with the fixed memory address when we need to load the image.
  std::string error_msg;
  MemMap::Init();
  image_reservation_ = MemMap::MapAnonymous("image reservation",
                                            reinterpret_cast<uint8_t*>(ART_BASE_ADDRESS),
                                            static_cast<size_t>(120 * 1024 * 1024),  // 120MB
                                            PROT_NONE,
                                            false /* no need for 4gb flag with fixed mmap */,
                                            /*reuse=*/ false,
                                            /*reservation=*/ nullptr,
                                            &error_msg);
  CHECK(image_reservation_.IsValid()) << error_msg;
}

void CommonCompilerDriverTest::UnreserveImageSpace() {
  image_reservation_.Reset();
}

void CommonCompilerDriverTest::CreateCompilerDriver() {
  ApplyInstructionSet();

  compiler_options_->image_type_ = CompilerOptions::ImageType::kBootImage;
  compiler_options_->compile_pic_ = false;  // Non-PIC boot image is a test configuration.
  compiler_options_->SetCompilerFilter(GetCompilerFilter());
  compiler_options_->image_classes_.swap(*GetImageClasses());
  compiler_options_->profile_compilation_info_ = GetProfileCompilationInfo();
  compiler_driver_.reset(new CompilerDriver(compiler_options_.get(),
                                            verification_results_.get(),
                                            number_of_threads_,
                                            /* swap_fd= */ -1));
}

void CommonCompilerDriverTest::SetUpRuntimeOptions(RuntimeOptions* options) {
  CommonCompilerTest::SetUpRuntimeOptions(options);

  verification_results_.reset(new VerificationResults());
  QuickCompilerCallbacks* callbacks =
      new QuickCompilerCallbacks(CompilerCallbacks::CallbackMode::kCompileApp);
  callbacks->SetVerificationResults(verification_results_.get());
  callbacks_.reset(callbacks);
}

void CommonCompilerDriverTest::SetUp() {
  CommonCompilerTest::SetUp();

  CreateCompilerDriver();
}

void CommonCompilerDriverTest::TearDown() {
  image_reservation_.Reset();
  compiler_driver_.reset();
  verification_results_.reset();

  CommonCompilerTest::TearDown();
}

// Get the set of image classes given to the compiler options in CreateCompilerDriver().
std::unique_ptr<HashSet<std::string>> CommonCompilerDriverTest::GetImageClasses() {
  // Empty set: by default no classes are retained in the image.
  return std::make_unique<HashSet<std::string>>();
}

// Get ProfileCompilationInfo that should be passed to the driver.
ProfileCompilationInfo* CommonCompilerDriverTest::GetProfileCompilationInfo() {
  // Null, profile information will not be taken into account.
  return nullptr;
}

}  // namespace art
