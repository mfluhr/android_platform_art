/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "oatdump_test.h"

namespace art {

TEST_F(OatDumpTest, TestAppWithBootImage) {
  TEST_DISABLED_FOR_RISCV64();
  ASSERT_TRUE(GenerateAppOdexFile(Flavor::kDynamic, {"--runtime-arg", "-Xmx64M"}));
  ASSERT_TRUE(Exec(Flavor::kDynamic, kModeOatWithBootImage, {}, kListAndCode));
}
TEST_F(OatDumpTest, TestAppWithBootImageStatic) {
  TEST_DISABLED_FOR_RISCV64();
  TEST_DISABLED_FOR_NON_STATIC_HOST_BUILDS();
  ASSERT_TRUE(GenerateAppOdexFile(Flavor::kStatic, {"--runtime-arg", "-Xmx64M"}));
  ASSERT_TRUE(Exec(Flavor::kStatic, kModeOatWithBootImage, {}, kListAndCode));
}

TEST_F(OatDumpTest, TestAppImageWithBootImage) {
  TEST_DISABLED_FOR_RISCV64();
  TEST_DISABLED_WITHOUT_BAKER_READ_BARRIERS();  // GC bug, b/126305867
  const std::string app_image_arg = "--app-image-file=" + GetAppImageName();
  ASSERT_TRUE(GenerateAppOdexFile(Flavor::kDynamic, {"--runtime-arg", "-Xmx64M", app_image_arg}));
  ASSERT_TRUE(Exec(Flavor::kDynamic, kModeAppImage, {}, kListAndCode));
}
TEST_F(OatDumpTest, TestAppImageWithBootImageStatic) {
  TEST_DISABLED_FOR_RISCV64();
  TEST_DISABLED_WITHOUT_BAKER_READ_BARRIERS();  // GC bug, b/126305867
  TEST_DISABLED_FOR_NON_STATIC_HOST_BUILDS();
  const std::string app_image_arg = "--app-image-file=" + GetAppImageName();
  ASSERT_TRUE(GenerateAppOdexFile(Flavor::kStatic, {"--runtime-arg", "-Xmx64M", app_image_arg}));
  ASSERT_TRUE(Exec(Flavor::kStatic, kModeAppImage, {}, kListAndCode));
}

TEST_F(OatDumpTest, TestAppImageInvalidPath) {
  TEST_DISABLED_FOR_RISCV64();
  TEST_DISABLED_WITHOUT_BAKER_READ_BARRIERS();  // GC bug, b/126305867
  TEST_DISABLED_FOR_NON_STATIC_HOST_BUILDS();
  const std::string app_image_arg = "--app-image-file=" + GetAppImageName();
  ASSERT_TRUE(GenerateAppOdexFile(Flavor::kStatic, {"--runtime-arg", "-Xmx64M", app_image_arg}));
  SetAppImageName("missing_app_image.art");
  ASSERT_TRUE(Exec(Flavor::kStatic, kModeAppImage, {}, kListAndCode, /*expect_failure=*/true));
}

}  // namespace art
