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

#include "logging.h"

#include <type_traits>

#include "android-base/logging.h"
#include "bit_utils.h"
#include "gtest/gtest.h"
#include "macros.h"
#include "runtime_debug.h"

namespace art {

[[noreturn]]
static void SimpleAborter(const char* msg) {
  LOG(FATAL_WITHOUT_ABORT) << msg;
  _exit(1);
}

class LoggingTest : public ::testing::Test {
 protected:
  LoggingTest() {
    // In our abort tests we really don't want the runtime to create a real dump.
    android::base::SetAborter(SimpleAborter);
  }
};

class TestClass {
 public:
  DECLARE_RUNTIME_DEBUG_FLAG(kFlag);
};
DEFINE_RUNTIME_DEBUG_FLAG(TestClass, kFlag);

TEST_F(LoggingTest, DECL_DEF) {
  SetRuntimeDebugFlagsEnabled(true);
  if (kIsDebugBuild) {
    EXPECT_TRUE(TestClass::kFlag);
  } else {
    // Runtime debug flags have a constant `false` value on non-debug builds.
    EXPECT_FALSE(TestClass::kFlag);
  }

  SetRuntimeDebugFlagsEnabled(false);
  EXPECT_FALSE(TestClass::kFlag);
}

}  // namespace art
