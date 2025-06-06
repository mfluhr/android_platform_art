/*
 * Copyright (C) 2017 The Android Open Source Project
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

#ifndef ART_RUNTIME_DEXOPT_TEST_H_
#define ART_RUNTIME_DEXOPT_TEST_H_

#include <string>
#include <vector>

#include "base/macros.h"
#include "dex2oat_environment_test.h"

namespace art HIDDEN {

class DexoptTest : public Dex2oatEnvironmentTest {
 public:
  void SetUp() override;

  void PreRuntimeCreate() override;

  void PostRuntimeCreate() override;

  std::string GenerateAlternateImage(const std::string& scratch_dir);

  // Generate an oat file for the purposes of test.
  // The oat file will be generated for dex_location in the given oat_location
  // with the following configuration:
  //   filter - controls the compilation filter
  //   with_alternate_image - if true, the oat file will be generated with an
  //      image checksum different than the current image checksum.
  void GenerateOatForTest(const std::string& dex_location,
                          const std::string& oat_location,
                          CompilerFilter::Filter filter,
                          bool with_alternate_image,
                          const char* compilation_reason = nullptr,
                          const std::vector<std::string>& extra_args = {});

  // Generate an odex file for the purposes of test.
  void GenerateOdexForTest(const std::string& dex_location,
                           const std::string& odex_location,
                           CompilerFilter::Filter filter,
                           const char* compilation_reason = nullptr,
                          const std::vector<std::string>& extra_args = {});

  // Generate an oat file for the given dex location in its oat location (under
  // the dalvik cache).
  void GenerateOatForTest(const char* dex_location,
                          CompilerFilter::Filter filter,
                          bool with_alternate_image);

  // Generate a standard oat file in the oat location.
  void GenerateOatForTest(const char* dex_location, CompilerFilter::Filter filter);

  // Generate sdm and dm files for the purposes of test.
  // If `include_app_image` is true, generates an app image and includes it in the sdm file.
  void GenerateSdmDmForTest(const std::string& dex_location,
                            const std::string& sdm_location,
                            const std::string& dm_location,
                            CompilerFilter::Filter filter,
                            bool include_app_image,
                            const char* compilation_reason = nullptr,
                            const std::vector<std::string>& extra_args = {});

  bool Dex2Oat(const std::vector<std::string>& args, std::string* error_msg);

 private:
  // Reserve memory around where the image will be loaded so other memory
  // won't conflict when it comes time to load the image.
  // This can be called with an already loaded image to reserve the space
  // around it.
  void ReserveImageSpace();

  // Reserve a chunk of memory for the image space in the given range.
  // Only has effect for chunks with a positive number of bytes.
  void ReserveImageSpaceChunk(uintptr_t start, uintptr_t end);

  // Unreserve any memory reserved by ReserveImageSpace. This should be called
  // before the image is loaded.
  void UnreserveImageSpace();

  std::vector<MemMap> image_reservation_;
};

}  // namespace art

#endif  // ART_RUNTIME_DEXOPT_TEST_H_
