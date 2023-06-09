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

#include <sys/time.h>
#include <sys/resource.h>

#include "base/macros.h"
#include "base/utils.h"
#include "jni.h"

extern "C" JNIEXPORT jint JNICALL Java_Main_getThreadPlatformPriority(
    [[maybe_unused]] JNIEnv* env,
    [[maybe_unused]] jclass clazz) {
  return getpriority(PRIO_PROCESS, art::GetTid());
}
