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

#include <dlfcn.h>

#include "base/sdk_version.h"
#include "dex/art_dex_file_loader.h"
#include "hidden_api.h"
#include "jni.h"
#include "runtime.h"
#include "ti-agent/scoped_utf_chars.h"

#ifdef ART_TARGET_ANDROID
#include "nativeloader/dlext_namespaces.h"
#endif

namespace art {
namespace Test674HiddenApi {

// Should be the same as dalvik.system.VMRuntime.PREVENT_META_REFLECTION_BLOCKLIST_ACCESS
static constexpr uint64_t kPreventMetaReflectionBlocklistAccess = 142365358;

std::vector<std::vector<std::unique_ptr<const DexFile>>> opened_dex_files;

// The JNI entrypoints below end up in libarttest(d).so, while the test makes
// copies of libarttest(d)_external.so and loads them instead. Those libs depend
// on libarttest(d).so, so its exported symbols become visible directly in them.
// Hence we don't need to create wrappers for the JNI methods in
// libarttest(d)_external.so.

extern "C" JNIEXPORT void JNICALL
Java_Main_addDefaultNamespaceLibsLinkToSystemLinkerNamespace(JNIEnv*, jclass) {
#ifdef ART_TARGET_ANDROID
  const char* links = getenv("NATIVELOADER_DEFAULT_NAMESPACE_LIBS");
  if (links == nullptr || *links == 0) {
    LOG(FATAL) << "Expected NATIVELOADER_DEFAULT_NAMESPACE_LIBS to be set";
  }
  struct android_namespace_t* system_ns = android_get_exported_namespace("system");
  if (system_ns == nullptr) {
    LOG(FATAL) << "Failed to retrieve system namespace";
  }
  if (!android_link_namespaces(system_ns, nullptr, links)) {
    LOG(FATAL) << "Error adding linker namespace link from system to default for " << links << ": "
               << dlerror();
  }
#endif
}

extern "C" JNIEXPORT void JNICALL Java_Main_init(JNIEnv*, jclass) {
  Runtime* runtime = Runtime::Current();
  runtime->SetHiddenApiEnforcementPolicy(hiddenapi::EnforcementPolicy::kEnabled);
  runtime->SetCorePlatformApiEnforcementPolicy(hiddenapi::EnforcementPolicy::kEnabled);
  runtime->SetTargetSdkVersion(
      static_cast<uint32_t>(hiddenapi::ApiList::MaxTargetO().GetMaxAllowedSdkVersion()));
  runtime->SetDedupeHiddenApiWarnings(false);
}

extern "C" JNIEXPORT void JNICALL Java_Main_setDexDomain(
    JNIEnv*, jclass, jint int_index, jboolean is_core_platform) {
  size_t index = static_cast<size_t>(int_index);
  CHECK_LT(index, opened_dex_files.size());
  for (std::unique_ptr<const DexFile>& dex_file : opened_dex_files[index]) {
    const_cast<DexFile*>(dex_file.get())->SetHiddenapiDomain(
        (is_core_platform == JNI_FALSE) ? hiddenapi::Domain::kPlatform
                                        : hiddenapi::Domain::kCorePlatform);
  }
}

extern "C" JNIEXPORT jint JNICALL Java_Main_appendToBootClassLoader(
    JNIEnv* env, jclass klass, jstring jpath, jboolean is_core_platform) {
  ScopedUtfChars utf(env, jpath);
  const char* path = utf.c_str();
  CHECK(path != nullptr);

  const size_t index = opened_dex_files.size();
  const jint int_index = static_cast<jint>(index);
  opened_dex_files.push_back(std::vector<std::unique_ptr<const DexFile>>());

  DexFileLoader dex_loader(path);
  std::string error_msg;

  if (!dex_loader.Open(/* verify */ false,
                       /* verify_checksum */ true,
                       &error_msg,
                       &opened_dex_files[index])) {
    LOG(FATAL) << "Could not open " << path << " for boot classpath extension: " << error_msg;
    UNREACHABLE();
  }

  Java_Main_setDexDomain(env, klass, int_index, is_core_platform);

  Runtime::Current()->AppendToBootClassPath(path, path, opened_dex_files[index]);

  return int_index;
}

extern "C" JNIEXPORT void JNICALL Java_Main_setSdkAll(JNIEnv*, jclass, jboolean value) {
  std::vector<std::string> exemptions;
  if (value != JNI_FALSE) {
    exemptions.push_back("L");
  }
  Runtime::Current()->SetHiddenApiExemptions(exemptions);
}

static jobject NewInstance(JNIEnv* env, jclass klass) {
  jmethodID constructor = env->GetMethodID(klass, "<init>", "()V");
  if (constructor == nullptr) {
    return nullptr;
  }
  return env->NewObject(klass, constructor);
}

extern "C" JNIEXPORT jboolean JNICALL Java_JNI_canDiscoverField(
    JNIEnv* env, jclass, jclass klass, jstring name, jboolean is_static) {
  ScopedUtfChars utf_name(env, name);
  jfieldID field = is_static ? env->GetStaticFieldID(klass, utf_name.c_str(), "I")
                             : env->GetFieldID(klass, utf_name.c_str(), "I");
  if (field == nullptr) {
    env->ExceptionClear();
    return JNI_FALSE;
  }

  return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL Java_JNI_canGetField(
    JNIEnv* env, jclass, jclass klass, jstring name, jboolean is_static) {
  ScopedUtfChars utf_name(env, name);
  jfieldID field = is_static ? env->GetStaticFieldID(klass, utf_name.c_str(), "I")
                             : env->GetFieldID(klass, utf_name.c_str(), "I");
  if (field == nullptr) {
    env->ExceptionClear();
    return JNI_FALSE;
  }
  if (is_static) {
    env->GetStaticIntField(klass, field);
  } else {
    jobject obj = NewInstance(env, klass);
    if (obj == nullptr) {
      env->ExceptionDescribe();
      env->ExceptionClear();
      return JNI_FALSE;
    }
    env->GetIntField(obj, field);
  }

  if (env->ExceptionOccurred()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    return JNI_FALSE;
  }

  return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL Java_JNI_canSetField(
    JNIEnv* env, jclass, jclass klass, jstring name, jboolean is_static) {
  ScopedUtfChars utf_name(env, name);
  jfieldID field = is_static ? env->GetStaticFieldID(klass, utf_name.c_str(), "I")
                             : env->GetFieldID(klass, utf_name.c_str(), "I");
  if (field == nullptr) {
    env->ExceptionClear();
    return JNI_FALSE;
  }
  if (is_static) {
    env->SetStaticIntField(klass, field, 42);
  } else {
    jobject obj = NewInstance(env, klass);
    if (obj == nullptr) {
      env->ExceptionDescribe();
      env->ExceptionClear();
      return JNI_FALSE;
    }
    env->SetIntField(obj, field, 42);
  }

  if (env->ExceptionOccurred()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    return JNI_FALSE;
  }

  return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL Java_JNI_canDiscoverMethod(
    JNIEnv* env, jclass, jclass klass, jstring name, jboolean is_static) {
  ScopedUtfChars utf_name(env, name);
  jmethodID method = is_static ? env->GetStaticMethodID(klass, utf_name.c_str(), "()I")
                               : env->GetMethodID(klass, utf_name.c_str(), "()I");
  if (method == nullptr) {
    env->ExceptionClear();
    return JNI_FALSE;
  }

  return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL Java_JNI_canInvokeMethodA(
    JNIEnv* env, jclass, jclass klass, jstring name, jboolean is_static) {
  ScopedUtfChars utf_name(env, name);
  jmethodID method = is_static ? env->GetStaticMethodID(klass, utf_name.c_str(), "()I")
                               : env->GetMethodID(klass, utf_name.c_str(), "()I");
  if (method == nullptr) {
    env->ExceptionClear();
    return JNI_FALSE;
  }

  if (is_static) {
    env->CallStaticIntMethodA(klass, method, nullptr);
  } else {
    jobject obj = NewInstance(env, klass);
    if (obj == nullptr) {
      env->ExceptionDescribe();
      env->ExceptionClear();
      return JNI_FALSE;
    }
    env->CallIntMethodA(obj, method, nullptr);
  }

  if (env->ExceptionOccurred()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    return JNI_FALSE;
  }

  return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL Java_JNI_canInvokeMethodV(
    JNIEnv* env, jclass, jclass klass, jstring name, jboolean is_static) {
  ScopedUtfChars utf_name(env, name);
  jmethodID method = is_static ? env->GetStaticMethodID(klass, utf_name.c_str(), "()I")
                               : env->GetMethodID(klass, utf_name.c_str(), "()I");
  if (method == nullptr) {
    env->ExceptionClear();
    return JNI_FALSE;
  }

  if (is_static) {
    env->CallStaticIntMethod(klass, method);
  } else {
    jobject obj = NewInstance(env, klass);
    if (obj == nullptr) {
      env->ExceptionDescribe();
      env->ExceptionClear();
      return JNI_FALSE;
    }
    env->CallIntMethod(obj, method);
  }

  if (env->ExceptionOccurred()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    return JNI_FALSE;
  }

  return JNI_TRUE;
}

static constexpr size_t kConstructorSignatureLength = 5;  // e.g. (IZ)V
static constexpr size_t kNumConstructorArgs = kConstructorSignatureLength - 3;

extern "C" JNIEXPORT jboolean JNICALL Java_JNI_canDiscoverConstructor(
    JNIEnv* env, jclass, jclass klass, jstring args) {
  ScopedUtfChars utf_args(env, args);
  jmethodID constructor = env->GetMethodID(klass, "<init>", utf_args.c_str());
  if (constructor == nullptr) {
    env->ExceptionClear();
    return JNI_FALSE;
  }

  return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL Java_JNI_canInvokeConstructorA(
    JNIEnv* env, jclass, jclass klass, jstring args) {
  ScopedUtfChars utf_args(env, args);
  jmethodID constructor = env->GetMethodID(klass, "<init>", utf_args.c_str());
  if (constructor == nullptr) {
    env->ExceptionClear();
    return JNI_FALSE;
  }

  // CheckJNI won't allow out-of-range values, so just zero everything.
  CHECK_EQ(strlen(utf_args.c_str()), kConstructorSignatureLength);
  size_t initargs_size = sizeof(jvalue) * kNumConstructorArgs;
  jvalue *initargs = reinterpret_cast<jvalue*>(alloca(initargs_size));
  memset(initargs, 0, initargs_size);

  env->NewObjectA(klass, constructor, initargs);
  if (env->ExceptionOccurred()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    return JNI_FALSE;
  }

  return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL Java_JNI_canInvokeConstructorV(
    JNIEnv* env, jclass, jclass klass, jstring args) {
  ScopedUtfChars utf_args(env, args);
  jmethodID constructor = env->GetMethodID(klass, "<init>", utf_args.c_str());
  if (constructor == nullptr) {
    env->ExceptionClear();
    return JNI_FALSE;
  }

  // CheckJNI won't allow out-of-range values, so just zero everything.
  CHECK_EQ(strlen(utf_args.c_str()), kConstructorSignatureLength);
  size_t initargs_size = sizeof(jvalue) * kNumConstructorArgs;
  jvalue *initargs = reinterpret_cast<jvalue*>(alloca(initargs_size));
  memset(initargs, 0, initargs_size);

  static_assert(kNumConstructorArgs == 2, "Change the varargs below if you change the constant");
  env->NewObject(klass, constructor, initargs[0], initargs[1]);
  if (env->ExceptionOccurred()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    return JNI_FALSE;
  }

  return JNI_TRUE;
}

extern "C" JNIEXPORT jint JNICALL Java_Reflection_getHiddenApiAccessFlags(JNIEnv*, jclass) {
  return static_cast<jint>(kAccHiddenapiBits);
}

extern "C" JNIEXPORT void JNICALL Java_Reflection_setHiddenApiCheckHardening(JNIEnv*, jclass,
    jboolean value) {
  CompatFramework& compat_framework = Runtime::Current()->GetCompatFramework();
  std::set<uint64_t> disabled_changes = compat_framework.GetDisabledCompatChanges();
  if (value == JNI_TRUE) {
    // If hidden api check hardening is enabled, remove it from the set of disabled changes.
    disabled_changes.erase(kPreventMetaReflectionBlocklistAccess);
  } else {
    // If hidden api check hardening is disabled, add it to the set of disabled changes.
    disabled_changes.insert(kPreventMetaReflectionBlocklistAccess);
  }
  compat_framework.SetDisabledCompatChanges(disabled_changes);
}

}  // namespace Test674HiddenApi
}  // namespace art
