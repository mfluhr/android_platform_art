/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "NativeBridge8IdentifyTrampolines_lib.h"
#include "NativeBridgeTest.h"

namespace android {

TEST_F(NativeBridgeTest, V8_IdentifyTrampolines) {
  // Init
  ASSERT_TRUE(LoadNativeBridge(kNativeBridgeLibrary8, nullptr));
  ASSERT_TRUE(NativeBridgeAvailable());
  ASSERT_TRUE(PreInitializeNativeBridge(AppDataDir(), "isa"));
  ASSERT_TRUE(NativeBridgeAvailable());
  ASSERT_TRUE(InitializeNativeBridge(nullptr, nullptr));
  ASSERT_TRUE(NativeBridgeAvailable());

  ASSERT_EQ(NativeBridgeGetVersion(), 8U);

  const void* ptr = reinterpret_cast<void*>(NativeBridgeGetVersion);

  UNUSED(NativeBridgeIsNativeBridgeFunctionPointer(ptr));
  ASSERT_TRUE(IsNativeBridgeFunctionPointerCalledFor(ptr));
}

}  // namespace android
