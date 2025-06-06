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

#include "jni.h"

#include <iostream>

namespace art {

// The JNI entrypoint below ends up in libarttest(d).so, while the test loads
// libarttest(d)_external.so instead. That lib depends on libarttest(d).so, so
// its exported symbols become visible directly in it. Hence we don't need to
// create a wrapper for the JNI method in libarttest(d)_external.so.

// Native method annotated with `SampleAnnotation` in Java source.
extern "C" JNIEXPORT void JNICALL Java_Test_nativeMethodWithAnnotation(JNIEnv*, jclass) {
  std::cout << "Java_Test_nativeMethodWithAnnotation" << std::endl;
}

}  // namespace art
