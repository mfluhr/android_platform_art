/*
 * Copyright (C) 2008 The Android Open Source Project
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

#include "dalvik_system_DexFile.h"

#include <sstream>

#include "android-base/file.h"
#include "android-base/stringprintf.h"

#include "base/casts.h"
#include "base/compiler_filter.h"
#include "base/file_utils.h"
#include "base/hiddenapi_domain.h"
#include "base/logging.h"
#include "base/os.h"
#include "base/stl_util.h"
#include "base/transform_iterator.h"
#include "base/utils.h"
#include "base/zip_archive.h"
#include "class_linker.h"
#include "class_loader_context.h"
#include "common_throws.h"
#include "dex/art_dex_file_loader.h"
#include "dex/descriptors_names.h"
#include "dex/dex_file-inl.h"
#include "dex/dex_file_loader.h"
#include "gc/space/image_space.h"
#include "handle_scope-inl.h"
#include "jit/debugger_interface.h"
#include "jni/jni_internal.h"
#include "mirror/class_loader.h"
#include "mirror/object-inl.h"
#include "mirror/string.h"
#include "native_util.h"
#include "nativehelper/jni_macros.h"
#include "nativehelper/scoped_local_ref.h"
#include "nativehelper/scoped_utf_chars.h"
#include "oat/oat_file.h"
#include "oat/oat_file_assistant.h"
#include "oat/oat_file_manager.h"
#include "runtime.h"
#include "scoped_thread_state_change-inl.h"
#include "string_array_utils.h"
#include "thread-current-inl.h"

#ifdef ART_TARGET_ANDROID
#include <android/api-level.h>
#include <sys/system_properties.h>
#endif  // ART_TARGET_ANDROID

namespace art HIDDEN {

// Should be the same as dalvik.system.DexFile.ENFORCE_READ_ONLY_JAVA_DCL
static constexpr uint64_t kEnforceReadOnlyJavaDcl = 218865702;

using android::base::StringPrintf;

static bool ConvertJavaArrayToDexFiles(
    JNIEnv* env,
    jobject arrayObject,
    /*out*/ std::vector<const DexFile*>& dex_files,
    /*out*/ const OatFile*& oat_file) {
  jarray array = reinterpret_cast<jarray>(arrayObject);

  jsize array_size = env->GetArrayLength(array);
  if (env->ExceptionCheck() == JNI_TRUE) {
    return false;
  }

  // TODO: Optimize. On 32bit we can use an int array.
  jboolean is_long_data_copied;
  jlong* long_data = env->GetLongArrayElements(reinterpret_cast<jlongArray>(array),
                                               &is_long_data_copied);
  if (env->ExceptionCheck() == JNI_TRUE) {
    return false;
  }

  oat_file = reinterpret_cast64<const OatFile*>(long_data[kOatFileIndex]);
  dex_files.reserve(array_size - 1);
  for (jsize i = kDexFileIndexStart; i < array_size; ++i) {
    dex_files.push_back(reinterpret_cast64<const DexFile*>(long_data[i]));
  }

  env->ReleaseLongArrayElements(reinterpret_cast<jlongArray>(array), long_data, JNI_ABORT);
  return env->ExceptionCheck() != JNI_TRUE;
}

static jlongArray ConvertDexFilesToJavaArray(JNIEnv* env,
                                             const OatFile* oat_file,
                                             std::vector<std::unique_ptr<const DexFile>>& vec) {
  // Add one for the oat file.
  jlongArray long_array = env->NewLongArray(static_cast<jsize>(kDexFileIndexStart + vec.size()));
  if (env->ExceptionCheck() == JNI_TRUE) {
    return nullptr;
  }

  jboolean is_long_data_copied;
  jlong* long_data = env->GetLongArrayElements(long_array, &is_long_data_copied);
  if (env->ExceptionCheck() == JNI_TRUE) {
    return nullptr;
  }

  long_data[kOatFileIndex] = reinterpret_cast64<jlong>(oat_file);
  for (size_t i = 0; i < vec.size(); ++i) {
    long_data[kDexFileIndexStart + i] = reinterpret_cast64<jlong>(vec[i].get());
  }

  env->ReleaseLongArrayElements(long_array, long_data, 0);
  if (env->ExceptionCheck() == JNI_TRUE) {
    return nullptr;
  }

  // Now release all the unique_ptrs.
  for (auto& dex_file : vec) {
    dex_file.release();  // NOLINT
  }

  return long_array;
}

// A smart pointer that provides read-only access to a Java string's UTF chars.
// Unlike libcore's NullableScopedUtfChars, this will *not* throw NullPointerException if
// passed a null jstring. The correct idiom is:
//
//   NullableScopedUtfChars name(env, javaName);
//   if (env->ExceptionCheck()) {
//       return null;
//   }
//   // ... use name.c_str()
//
// TODO: rewrite to get rid of this, or change ScopedUtfChars to offer this option.
class NullableScopedUtfChars {
 public:
  NullableScopedUtfChars(JNIEnv* env, jstring s) : mEnv(env), mString(s) {
    mUtfChars = (s != nullptr) ? env->GetStringUTFChars(s, nullptr) : nullptr;
  }

  ~NullableScopedUtfChars() {
    if (mUtfChars) {
      mEnv->ReleaseStringUTFChars(mString, mUtfChars);
    }
  }

  const char* c_str() const {
    return mUtfChars;
  }

  size_t size() const {
    return strlen(mUtfChars);
  }

  // Element access.
  const char& operator[](size_t n) const {
    return mUtfChars[n];
  }

 private:
  JNIEnv* mEnv;
  jstring mString;
  const char* mUtfChars;

  // Disallow copy and assignment.
  NullableScopedUtfChars(const NullableScopedUtfChars&);
  void operator=(const NullableScopedUtfChars&);
};

static jobject CreateCookieFromOatFileManagerResult(
    JNIEnv* env,
    std::vector<std::unique_ptr<const DexFile>>& dex_files,
    const OatFile* oat_file,
    const std::vector<std::string>& error_msgs) {
  ClassLinker* linker = Runtime::Current()->GetClassLinker();
  if (dex_files.empty()) {
    ScopedObjectAccess soa(env);
    CHECK(!error_msgs.empty());
    // The most important message is at the end. So set up nesting by going forward, which will
    // wrap the existing exception as a cause for the following one.
    auto it = error_msgs.begin();
    auto itEnd = error_msgs.end();
    for ( ; it != itEnd; ++it) {
      ThrowWrappedIOException("%s", it->c_str());
    }
    return nullptr;
  }

  jlongArray array = ConvertDexFilesToJavaArray(env, oat_file, dex_files);
  if (array == nullptr) {
    ScopedObjectAccess soa(env);
    for (auto& dex_file : dex_files) {
      if (linker->IsDexFileRegistered(soa.Self(), *dex_file)) {
        dex_file.release();  // NOLINT
      }
    }
  }
  return array;
}

static MemMap AllocateDexMemoryMap(JNIEnv* env, jint start, jint end) {
  if (end <= start) {
    ScopedObjectAccess soa(env);
    ThrowWrappedIOException("Bad range");
    return MemMap::Invalid();
  }

  std::string error_message;
  size_t length = static_cast<size_t>(end - start);
  MemMap dex_mem_map = MemMap::MapAnonymous("DEX data",
                                            length,
                                            PROT_READ | PROT_WRITE,
                                            /*low_4gb=*/ false,
                                            &error_message);
  if (!dex_mem_map.IsValid()) {
    ScopedObjectAccess soa(env);
    ThrowWrappedIOException("%s", error_message.c_str());
    return MemMap::Invalid();
  }
  return dex_mem_map;
}

struct ScopedIntArrayAccessor {
 public:
  ScopedIntArrayAccessor(JNIEnv* env, jintArray arr) : env_(env), array_(arr) {
    elements_ = env_->GetIntArrayElements(array_, /* isCopy= */ nullptr);
    CHECK(elements_ != nullptr);
  }

  ~ScopedIntArrayAccessor() {
    env_->ReleaseIntArrayElements(array_, elements_, JNI_ABORT);
  }

  jint Get(jsize index) const { return elements_[index]; }

 private:
  JNIEnv* env_;
  jintArray array_;
  jint* elements_;
};

static jobject DexFile_openInMemoryDexFilesNative(JNIEnv* env,
                                                  jclass,
                                                  jobjectArray buffers,
                                                  jobjectArray arrays,
                                                  jintArray jstarts,
                                                  jintArray jends,
                                                  jobject class_loader,
                                                  jobjectArray dex_elements) {
  jsize buffers_length = env->GetArrayLength(buffers);
  CHECK_EQ(buffers_length, env->GetArrayLength(arrays));
  CHECK_EQ(buffers_length, env->GetArrayLength(jstarts));
  CHECK_EQ(buffers_length, env->GetArrayLength(jends));

  ScopedIntArrayAccessor starts(env, jstarts);
  ScopedIntArrayAccessor ends(env, jends);

  // Allocate memory for dex files and copy data from ByteBuffers.
  std::vector<MemMap> dex_mem_maps;
  dex_mem_maps.reserve(buffers_length);
  for (jsize i = 0; i < buffers_length; ++i) {
    jobject buffer = env->GetObjectArrayElement(buffers, i);
    jbyteArray array = reinterpret_cast<jbyteArray>(env->GetObjectArrayElement(arrays, i));
    jint start = starts.Get(i);
    jint end = ends.Get(i);

    MemMap dex_data = AllocateDexMemoryMap(env, start, end);
    if (!dex_data.IsValid()) {
      DCHECK(Thread::Current()->IsExceptionPending());
      return nullptr;
    }

    if (array == nullptr) {
      // Direct ByteBuffer
      uint8_t* base_address = reinterpret_cast<uint8_t*>(env->GetDirectBufferAddress(buffer));
      if (base_address == nullptr) {
        ScopedObjectAccess soa(env);
        ThrowWrappedIOException("dexFileBuffer not direct");
        return nullptr;
      }
      size_t length = static_cast<size_t>(end - start);
      memcpy(dex_data.Begin(), base_address + start, length);
    } else {
      // ByteBuffer backed by a byte array
      jbyte* destination = reinterpret_cast<jbyte*>(dex_data.Begin());
      env->GetByteArrayRegion(array, start, end - start, destination);
    }

    dex_mem_maps.push_back(std::move(dex_data));
  }

  // Hand MemMaps over to OatFileManager to open the dex files and potentially
  // create a backing OatFile instance from an anonymous vdex.
  std::vector<std::string> error_msgs;
  const OatFile* oat_file = nullptr;
  std::vector<std::unique_ptr<const DexFile>> dex_files =
      Runtime::Current()->GetOatFileManager().OpenDexFilesFromOat(std::move(dex_mem_maps),
                                                                  class_loader,
                                                                  dex_elements,
                                                                  /*out*/ &oat_file,
                                                                  /*out*/ &error_msgs);
  return CreateCookieFromOatFileManagerResult(env, dex_files, oat_file, error_msgs);
}

#ifdef ART_TARGET_ANDROID
static bool isReadOnlyJavaDclEnforced(JNIEnv* env) {
  static bool is_at_least_u = [] {
    const int api_level = android_get_device_api_level();
    if (api_level > __ANDROID_API_T__) {
      return true;
    } else if (api_level == __ANDROID_API_T__) {
      // Check if running U preview
      char value[92] = {0};
      if (__system_property_get("ro.build.version.preview_sdk", value) >= 0 && atoi(value) > 0) {
        return true;
      }
    }
    return false;
  }();
  if (is_at_least_u) {
    // The reason why we are calling the AppCompat framework through JVM
    // instead of directly using the CompatFramework C++ API is because feature
    // overrides only apply to the Java API.
    // CtsLibcoreTestCases is part of mainline modules, which requires the same test
    // to run on older Android versions; the target SDK of CtsLibcoreTestCases is locked
    // to the lowest supported API level (at the time of writing, it's API 31).
    // We would need to be able to manually enable the compat change in CTS tests.
    ScopedLocalRef<jclass> compat(env, env->FindClass("android/compat/Compatibility"));
    jmethodID mId = env->GetStaticMethodID(compat.get(), "isChangeEnabled", "(J)Z");
    return env->CallStaticBooleanMethod(compat.get(), mId, kEnforceReadOnlyJavaDcl) == JNI_TRUE;
  } else {
    return false;
  }
}
#else   // ART_TARGET_ANDROID
constexpr static bool isReadOnlyJavaDclEnforced(JNIEnv*) {
  (void)kEnforceReadOnlyJavaDcl;
  return false;
}
#endif  // ART_TARGET_ANDROID

static bool isReadOnlyJavaDclChecked() {
  if (!kIsTargetAndroid) {
    return false;
  }
  const int uid = getuid();
  // The following UIDs are exempted:
  // * Root (0): root processes always have write access to files.
  // * System (1000): /data/app/**.apk are owned by AID_SYSTEM;
  //   loading installed APKs in system_server is allowed.
  // * Shell (2000): directly calling dalvikvm/app_process in ADB shell
  //   to run JARs with CLI is allowed.
  return uid != 0 && uid != 1000 && uid != 2000;
}

// TODO(calin): clean up the unused parameters (here and in libcore).
static jobject DexFile_openDexFileNative(JNIEnv* env,
                                         jclass,
                                         jstring javaSourceName,
                                         [[maybe_unused]] jstring javaOutputName,
                                         [[maybe_unused]] jint flags,
                                         jobject class_loader,
                                         jobjectArray dex_elements) {
  ScopedUtfChars sourceName(env, javaSourceName);
  if (sourceName.c_str() == nullptr) {
    return nullptr;
  }

  if (isReadOnlyJavaDclChecked() && access(sourceName.c_str(), W_OK) == 0) {
    LOG(ERROR) << "Attempt to load writable dex file: " << sourceName.c_str();
    if (isReadOnlyJavaDclEnforced(env)) {
      ScopedLocalRef<jclass> se(env, env->FindClass("java/lang/SecurityException"));
      std::string message(
          StringPrintf("Writable dex file '%s' is not allowed.", sourceName.c_str()));
      env->ThrowNew(se.get(), message.c_str());
      return nullptr;
    }
  }

  std::vector<std::string> error_msgs;
  const OatFile* oat_file = nullptr;
  std::vector<std::unique_ptr<const DexFile>> dex_files =
      Runtime::Current()->GetOatFileManager().OpenDexFilesFromOat(sourceName.c_str(),
                                                                  class_loader,
                                                                  dex_elements,
                                                                  /*out*/ &oat_file,
                                                                  /*out*/ &error_msgs);
  return CreateCookieFromOatFileManagerResult(env, dex_files, oat_file, error_msgs);
}

static void DexFile_verifyInBackgroundNative(JNIEnv* env,
                                             jclass,
                                             jobject cookie,
                                             jobject class_loader) {
  CHECK(cookie != nullptr);
  CHECK(class_loader != nullptr);

  // Extract list of dex files from the cookie.
  std::vector<const DexFile*> dex_files;
  const OatFile* oat_file;
  if (!ConvertJavaArrayToDexFiles(env, cookie, dex_files, oat_file)) {
    Thread::Current()->AssertPendingException();
    return;
  }
  CHECK(oat_file == nullptr) << "Called verifyInBackground on a dex file backed by oat";

  // Hand over to OatFileManager to spawn a verification thread.
  Runtime::Current()->GetOatFileManager().RunBackgroundVerification(
      dex_files,
      class_loader);
}

static jboolean DexFile_closeDexFile(JNIEnv* env, jclass, jobject cookie) {
  std::vector<const DexFile*> dex_files;
  const OatFile* oat_file;
  if (!ConvertJavaArrayToDexFiles(env, cookie, dex_files, oat_file)) {
    Thread::Current()->AssertPendingException();
    return JNI_FALSE;
  }
  Runtime* const runtime = Runtime::Current();
  bool all_deleted = true;
  // We need to clear the caches since they may contain pointers to the dex instructions.
  // Different dex file can be loaded at the same memory location later by chance.
  Thread::ClearAllInterpreterCaches();
  {
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Object> dex_files_object = soa.Decode<mirror::Object>(cookie);
    ObjPtr<mirror::LongArray> long_dex_files = dex_files_object->AsLongArray();
    // Delete dex files associated with this dalvik.system.DexFile since there should not be running
    // code using it. dex_files is a vector due to multidex.
    ClassLinker* const class_linker = runtime->GetClassLinker();
    int32_t i = kDexFileIndexStart;  // Oat file is at index 0.
    for (const DexFile* dex_file : dex_files) {
      if (dex_file != nullptr) {
        RemoveNativeDebugInfoForDex(soa.Self(), dex_file);
        // Only delete the dex file if the dex cache is not found to prevent runtime crashes
        // if there are calls to DexFile.close while the ART DexFile is still in use.
        if (!class_linker->IsDexFileRegistered(soa.Self(), *dex_file)) {
          // Clear the element in the array so that we can call close again.
          long_dex_files->Set(i, 0);
          class_linker->RemoveDexFromCaches(*dex_file);
          delete dex_file;
        } else {
          all_deleted = false;
        }
      }
      ++i;
    }
  }

  // oat_file can be null if we are running without dex2oat.
  if (all_deleted && oat_file != nullptr) {
    // If all of the dex files are no longer in use we can unmap the corresponding oat file.
    VLOG(class_linker) << "Unregistering " << oat_file;
    runtime->GetOatFileManager().UnRegisterAndDeleteOatFile(oat_file);
  }
  return all_deleted ? JNI_TRUE : JNI_FALSE;
}

static jclass DexFile_defineClassNative(JNIEnv* env,
                                        jclass,
                                        jstring javaName,
                                        jobject javaLoader,
                                        jobject cookie,
                                        jobject dexFile) {
  std::vector<const DexFile*> dex_files;
  const OatFile* oat_file;
  if (!ConvertJavaArrayToDexFiles(env, cookie, /*out*/ dex_files, /*out*/ oat_file)) {
    VLOG(class_linker) << "Failed to find dex_file";
    DCHECK(env->ExceptionCheck());
    return nullptr;
  }

  ScopedUtfChars class_name(env, javaName);
  if (class_name.c_str() == nullptr) {
    VLOG(class_linker) << "Failed to find class_name";
    return nullptr;
  }
  const std::string descriptor = DotToDescriptor(class_name);
  const size_t hash = ComputeModifiedUtf8Hash(descriptor);
  for (auto& dex_file : dex_files) {
    const dex::ClassDef* dex_class_def = OatDexFile::FindClassDef(*dex_file, descriptor, hash);
    if (dex_class_def != nullptr) {
      ScopedObjectAccess soa(env);
      ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
      StackHandleScope<1> hs(soa.Self());
      Handle<mirror::ClassLoader> class_loader(
          hs.NewHandle(soa.Decode<mirror::ClassLoader>(javaLoader)));
      ObjPtr<mirror::DexCache> dex_cache =
          class_linker->RegisterDexFile(*dex_file, class_loader.Get());
      if (dex_cache == nullptr) {
        // OOME or InternalError (dexFile already registered with a different class loader).
        soa.Self()->AssertPendingException();
        return nullptr;
      }
      ObjPtr<mirror::Class> result = class_linker->DefineClass(soa.Self(),
                                                               descriptor.c_str(),
                                                               descriptor.length(),
                                                               hash,
                                                               class_loader,
                                                               *dex_file,
                                                               *dex_class_def);
      // Add the used dex file. This only required for the DexFile.loadClass API since normal
      // class loaders already keep their dex files live.
      class_linker->InsertDexFileInToClassLoader(soa.Decode<mirror::Object>(dexFile),
                                                 class_loader.Get());
      if (result != nullptr) {
        VLOG(class_linker) << "DexFile_defineClassNative returning " << result
                           << " for " << class_name.c_str();
        return soa.AddLocalReference<jclass>(result);
      }
    }
  }
  VLOG(class_linker) << "Failed to find dex_class_def " << class_name.c_str();
  return nullptr;
}

// Needed as a compare functor for sets of const char
struct CharPointerComparator {
  bool operator()(const char *str1, const char *str2) const {
    return strcmp(str1, str2) < 0;
  }
};

// Note: this can be an expensive call, as we sort out duplicates in MultiDex files.
static jobjectArray DexFile_getClassNameList(JNIEnv* env, jclass, jobject cookie) {
  const OatFile* oat_file = nullptr;
  std::vector<const DexFile*> dex_files;
  if (!ConvertJavaArrayToDexFiles(env, cookie, /*out */ dex_files, /* out */ oat_file)) {
    DCHECK(env->ExceptionCheck());
    return nullptr;
  }

  // Push all class descriptors into a set. Use set instead of unordered_set as we want to
  // retrieve all in the end.
  std::set<const char*, CharPointerComparator> descriptors;
  for (auto& dex_file : dex_files) {
    for (size_t i = 0; i < dex_file->NumClassDefs(); ++i) {
      const dex::ClassDef& class_def = dex_file->GetClassDef(i);
      const char* descriptor = dex_file->GetClassDescriptor(class_def);
      descriptors.insert(descriptor);
    }
  }

  // Now create output array and copy the set into it.
  ScopedObjectAccess soa(Thread::ForEnv(env));
  auto descriptor_to_dot = [](const char* descriptor) { return DescriptorToDot(descriptor); };
  return soa.AddLocalReference<jobjectArray>(CreateStringArray(
      soa.Self(),
      descriptors.size(),
      MakeTransformRange(descriptors, descriptor_to_dot)));
}

static jint GetDexOptNeeded(JNIEnv* env,
                            const char* filename,
                            const char* instruction_set,
                            const char* compiler_filter_name,
                            const char* class_loader_context,
                            bool profile_changed,
                            bool downgrade) {
  if ((filename == nullptr) || !OS::FileExists(filename)) {
    LOG(ERROR) << "DexFile_getDexOptNeeded file '" << filename << "' does not exist";
    ScopedLocalRef<jclass> fnfe(env, env->FindClass("java/io/FileNotFoundException"));
    const char* message = (filename == nullptr) ? "<empty file name>" : filename;
    env->ThrowNew(fnfe.get(), message);
    return -1;
  }

  const InstructionSet target_instruction_set = GetInstructionSetFromString(instruction_set);
  if (target_instruction_set == InstructionSet::kNone) {
    ScopedLocalRef<jclass> iae(env, env->FindClass("java/lang/IllegalArgumentException"));
    std::string message(StringPrintf("Instruction set %s is invalid.", instruction_set));
    env->ThrowNew(iae.get(), message.c_str());
    return -1;
  }

  CompilerFilter::Filter filter;
  if (!CompilerFilter::ParseCompilerFilter(compiler_filter_name, &filter)) {
    ScopedLocalRef<jclass> iae(env, env->FindClass("java/lang/IllegalArgumentException"));
    std::string message(StringPrintf("Compiler filter %s is invalid.", compiler_filter_name));
    env->ThrowNew(iae.get(), message.c_str());
    return -1;
  }

  std::unique_ptr<ClassLoaderContext> context = nullptr;
  if (class_loader_context != nullptr) {
    context = ClassLoaderContext::Create(class_loader_context);

    if (context == nullptr) {
      ScopedLocalRef<jclass> iae(env, env->FindClass("java/lang/IllegalArgumentException"));
      std::string message(StringPrintf("Class loader context '%s' is invalid.",
                                       class_loader_context));
      env->ThrowNew(iae.get(), message.c_str());
      return -1;
    }
    std::vector<int> context_fds;
    context->OpenDexFiles(android::base::Dirname(filename),
                          context_fds,
                          /*only_read_checksums*/ true);
  }

  // TODO: Verify the dex location is well formed, and throw an IOException if
  // not?

  OatFileAssistant oat_file_assistant(filename,
                                      target_instruction_set,
                                      context.get(),
                                      /* load_executable= */ false);

  // Always treat elements of the bootclasspath as up-to-date.
  if (oat_file_assistant.IsInBootClassPath()) {
    return OatFileAssistant::kNoDexOptNeeded;
  }

  return oat_file_assistant.GetDexOptNeeded(filter,
                                            profile_changed,
                                            downgrade);
}

// Return an array specifying the optimization status of the given file.
// The array specification is [compiler_filter, compiler_reason].
static jobjectArray DexFile_getDexFileOptimizationStatus(JNIEnv* env,
                                                         jclass,
                                                         jstring javaFilename,
                                                         jstring javaInstructionSet) {
  ScopedUtfChars filename(env, javaFilename);
  if (env->ExceptionCheck()) {
    return nullptr;
  }

  ScopedUtfChars instruction_set(env, javaInstructionSet);
  if (env->ExceptionCheck()) {
    return nullptr;
  }

  const InstructionSet target_instruction_set = GetInstructionSetFromString(
      instruction_set.c_str());
  if (target_instruction_set == InstructionSet::kNone) {
    ScopedLocalRef<jclass> iae(env, env->FindClass("java/lang/IllegalArgumentException"));
    std::string message(StringPrintf("Instruction set %s is invalid.", instruction_set.c_str()));
    env->ThrowNew(iae.get(), message.c_str());
    return nullptr;
  }

  std::string compilation_filter;
  std::string compilation_reason;
  OatFileAssistant::GetOptimizationStatus(
      filename.c_str(), target_instruction_set, &compilation_filter, &compilation_reason);

  ScopedObjectAccess soa(Thread::ForEnv(env));
  return soa.AddLocalReference<jobjectArray>(CreateStringArray(soa.Self(), {
      compilation_filter.c_str(),
      compilation_reason.c_str()
  }));
}

static jint DexFile_getDexOptNeeded(JNIEnv* env,
                                    jclass,
                                    jstring javaFilename,
                                    jstring javaInstructionSet,
                                    jstring javaTargetCompilerFilter,
                                    jstring javaClassLoaderContext,
                                    jboolean newProfile,
                                    jboolean downgrade) {
  ScopedUtfChars filename(env, javaFilename);
  if (env->ExceptionCheck()) {
    return -1;
  }

  ScopedUtfChars instruction_set(env, javaInstructionSet);
  if (env->ExceptionCheck()) {
    return -1;
  }

  ScopedUtfChars target_compiler_filter(env, javaTargetCompilerFilter);
  if (env->ExceptionCheck()) {
    return -1;
  }

  NullableScopedUtfChars class_loader_context(env, javaClassLoaderContext);
  if (env->ExceptionCheck()) {
    return -1;
  }

  return GetDexOptNeeded(env,
                         filename.c_str(),
                         instruction_set.c_str(),
                         target_compiler_filter.c_str(),
                         class_loader_context.c_str(),
                         newProfile == JNI_TRUE,
                         downgrade == JNI_TRUE);
}

// public API
static jboolean DexFile_isDexOptNeeded(JNIEnv* env, jclass, jstring javaFilename) {
  ScopedUtfChars filename_utf(env, javaFilename);
  if (env->ExceptionCheck()) {
    return JNI_FALSE;
  }

  const char* filename = filename_utf.c_str();
  if ((filename == nullptr) || !OS::FileExists(filename)) {
    LOG(ERROR) << "DexFile_isDexOptNeeded file '" << filename << "' does not exist";
    ScopedLocalRef<jclass> fnfe(env, env->FindClass("java/io/FileNotFoundException"));
    const char* message = (filename == nullptr) ? "<empty file name>" : filename;
    env->ThrowNew(fnfe.get(), message);
    return JNI_FALSE;
  }

  OatFileAssistant oat_file_assistant(filename,
                                      kRuntimeISA,
                                      /* context= */ nullptr,
                                      /* load_executable= */ false);
  return oat_file_assistant.IsUpToDate() ? JNI_FALSE : JNI_TRUE;
}

static jboolean DexFile_isValidCompilerFilter(JNIEnv* env,
                                              [[maybe_unused]] jclass javaDexFileClass,
                                              jstring javaCompilerFilter) {
  ScopedUtfChars compiler_filter(env, javaCompilerFilter);
  if (env->ExceptionCheck()) {
    return -1;
  }

  CompilerFilter::Filter filter;
  return CompilerFilter::ParseCompilerFilter(compiler_filter.c_str(), &filter)
      ? JNI_TRUE : JNI_FALSE;
}

static jboolean DexFile_isProfileGuidedCompilerFilter(JNIEnv* env,
                                                      [[maybe_unused]] jclass javaDexFileClass,
                                                      jstring javaCompilerFilter) {
  ScopedUtfChars compiler_filter(env, javaCompilerFilter);
  if (env->ExceptionCheck()) {
    return -1;
  }

  CompilerFilter::Filter filter;
  if (!CompilerFilter::ParseCompilerFilter(compiler_filter.c_str(), &filter)) {
    return JNI_FALSE;
  }
  return CompilerFilter::DependsOnProfile(filter) ? JNI_TRUE : JNI_FALSE;
}

static jboolean DexFile_isVerifiedCompilerFilter(JNIEnv* env,
                                                 [[maybe_unused]] jclass javaDexFileClass,
                                                 jstring javaCompilerFilter) {
  ScopedUtfChars compiler_filter(env, javaCompilerFilter);
  if (env->ExceptionCheck()) {
    return -1;
  }

  CompilerFilter::Filter filter;
  if (!CompilerFilter::ParseCompilerFilter(compiler_filter.c_str(), &filter)) {
    return JNI_FALSE;
  }
  return CompilerFilter::IsVerificationEnabled(filter) ? JNI_TRUE : JNI_FALSE;
}

static jboolean DexFile_isOptimizedCompilerFilter(JNIEnv* env,
                                                  [[maybe_unused]] jclass javaDexFileClass,
                                                  jstring javaCompilerFilter) {
  ScopedUtfChars compiler_filter(env, javaCompilerFilter);
  if (env->ExceptionCheck()) {
    return -1;
  }

  CompilerFilter::Filter filter;
  if (!CompilerFilter::ParseCompilerFilter(compiler_filter.c_str(), &filter)) {
    return JNI_FALSE;
  }
  return CompilerFilter::IsAotCompilationEnabled(filter) ? JNI_TRUE : JNI_FALSE;
}

static jboolean DexFile_isReadOnlyJavaDclEnforced(JNIEnv* env,
                                                  [[maybe_unused]] jclass javaDexFileClass) {
  return (isReadOnlyJavaDclChecked() && isReadOnlyJavaDclEnforced(env)) ? JNI_TRUE : JNI_FALSE;
}

static jstring DexFile_getNonProfileGuidedCompilerFilter(JNIEnv* env,
                                                         [[maybe_unused]] jclass javaDexFileClass,
                                                         jstring javaCompilerFilter) {
  ScopedUtfChars compiler_filter(env, javaCompilerFilter);
  if (env->ExceptionCheck()) {
    return nullptr;
  }

  CompilerFilter::Filter filter;
  if (!CompilerFilter::ParseCompilerFilter(compiler_filter.c_str(), &filter)) {
    return javaCompilerFilter;
  }

  CompilerFilter::Filter new_filter = CompilerFilter::GetNonProfileDependentFilterFrom(filter);

  // Filter stayed the same, return input.
  if (filter == new_filter) {
    return javaCompilerFilter;
  }

  // Create a new string object and return.
  std::string new_filter_str = CompilerFilter::NameOfFilter(new_filter);
  return env->NewStringUTF(new_filter_str.c_str());
}

static jstring DexFile_getSafeModeCompilerFilter(JNIEnv* env,
                                                 [[maybe_unused]] jclass javaDexFileClass,
                                                 jstring javaCompilerFilter) {
  ScopedUtfChars compiler_filter(env, javaCompilerFilter);
  if (env->ExceptionCheck()) {
    return nullptr;
  }

  CompilerFilter::Filter filter;
  if (!CompilerFilter::ParseCompilerFilter(compiler_filter.c_str(), &filter)) {
    return javaCompilerFilter;
  }

  CompilerFilter::Filter new_filter = CompilerFilter::GetSafeModeFilterFrom(filter);

  // Filter stayed the same, return input.
  if (filter == new_filter) {
    return javaCompilerFilter;
  }

  // Create a new string object and return.
  std::string new_filter_str = CompilerFilter::NameOfFilter(new_filter);
  return env->NewStringUTF(new_filter_str.c_str());
}

static jboolean DexFile_isBackedByOatFile(JNIEnv* env, jclass, jobject cookie) {
  const OatFile* oat_file = nullptr;
  std::vector<const DexFile*> dex_files;
  if (!ConvertJavaArrayToDexFiles(env, cookie, /*out */ dex_files, /* out */ oat_file)) {
    DCHECK(env->ExceptionCheck());
    return false;
  }
  return oat_file != nullptr;
}

static jobjectArray DexFile_getDexFileOutputPaths(JNIEnv* env,
                                            jclass,
                                            jstring javaFilename,
                                            jstring javaInstructionSet) {
  ScopedUtfChars filename(env, javaFilename);
  if (env->ExceptionCheck()) {
    return nullptr;
  }

  ScopedUtfChars instruction_set(env, javaInstructionSet);
  if (env->ExceptionCheck()) {
    return nullptr;
  }

  const InstructionSet target_instruction_set = GetInstructionSetFromString(
      instruction_set.c_str());
  if (target_instruction_set == InstructionSet::kNone) {
    ScopedLocalRef<jclass> iae(env, env->FindClass("java/lang/IllegalArgumentException"));
    std::string message(StringPrintf("Instruction set %s is invalid.", instruction_set.c_str()));
    env->ThrowNew(iae.get(), message.c_str());
    return nullptr;
  }

  std::string oat_filename;
  std::string vdex_filename;
  // Check if the file is in the boot classpath by looking at image spaces which
  // have oat files.
  bool is_vdex_only = false;
  for (gc::space::ImageSpace* space : Runtime::Current()->GetHeap()->GetBootImageSpaces()) {
    const OatFile* oat_file = space->GetOatFile();
    if (oat_file != nullptr) {
      const std::vector<const OatDexFile*>& oat_dex_files = oat_file->GetOatDexFiles();
      for (const OatDexFile* oat_dex_file : oat_dex_files) {
        if (DexFileLoader::GetBaseLocation(oat_dex_file->GetDexFileLocation()) ==
                filename.c_str()) {
          oat_filename = oat_file->GetLocation();
          is_vdex_only = oat_file->IsBackedByVdexOnly();
          break;
        }
      }
      if (!oat_filename.empty()) {
        break;
      }
    }
  }

  // If we did not find a boot classpath oat file, lookup the oat file for an app.
  if (oat_filename.empty()) {
    OatFileAssistant oat_file_assistant(filename.c_str(),
                                        target_instruction_set,
                                        /* context= */ nullptr,
                                        /* load_executable= */ false);

    std::unique_ptr<OatFile> best_oat_file = oat_file_assistant.GetBestOatFile();
    if (best_oat_file == nullptr) {
      return nullptr;
    }

    oat_filename = best_oat_file->GetLocation();
    is_vdex_only = best_oat_file->IsBackedByVdexOnly();
  }

  const char* filenames[] = { oat_filename.c_str(), nullptr };
  ArrayRef<const char* const> used_filenames(filenames, 1u);
  if (!is_vdex_only) {
    vdex_filename = GetVdexFilename(oat_filename);
    filenames[1] = vdex_filename.c_str();
    used_filenames = ArrayRef<const char* const>(filenames, 2u);
  }
  ScopedObjectAccess soa(Thread::ForEnv(env));
  return soa.AddLocalReference<jobjectArray>(CreateStringArray(soa.Self(), used_filenames));
}

static jlong DexFile_getStaticSizeOfDexFile(JNIEnv* env, jclass, jobject cookie) {
  const OatFile* oat_file = nullptr;
  std::vector<const DexFile*> dex_files;
  if (!ConvertJavaArrayToDexFiles(env, cookie, /*out */ dex_files, /* out */ oat_file)) {
    DCHECK(env->ExceptionCheck());
    return 0;
  }

  uint64_t file_size = 0;
  for (auto& dex_file : dex_files) {
    if (dex_file) {
      file_size += dex_file->GetHeader().file_size_;
    }
  }
  return static_cast<jlong>(file_size);
}

static void DexFile_setTrusted(JNIEnv* env, jclass, jobject j_cookie) {
  Runtime* runtime = Runtime::Current();
  ScopedObjectAccess soa(env);

  // Currently only allow this for debuggable apps.
  if (!runtime->IsJavaDebuggableAtInit()) {
    ThrowSecurityException("Can't exempt class, process is not debuggable.");
    return;
  }

  std::vector<const DexFile*> dex_files;
  const OatFile* oat_file;
  if (!ConvertJavaArrayToDexFiles(env, j_cookie, dex_files, oat_file)) {
    Thread::Current()->AssertPendingException();
    return;
  }

  // Assign core platform domain as the dex files are allowed to access all the other domains.
  for (const DexFile* dex_file : dex_files) {
    const_cast<DexFile*>(dex_file)->SetHiddenapiDomain(hiddenapi::Domain::kCorePlatform);
  }
}

static JNINativeMethod gMethods[] = {
    NATIVE_METHOD(DexFile, closeDexFile, "(Ljava/lang/Object;)Z"),
    NATIVE_METHOD(DexFile,
                  defineClassNative,
                  "(Ljava/lang/String;"
                  "Ljava/lang/ClassLoader;"
                  "Ljava/lang/Object;"
                  "Ldalvik/system/DexFile;"
                  ")Ljava/lang/Class;"),
    NATIVE_METHOD(DexFile, getClassNameList, "(Ljava/lang/Object;)[Ljava/lang/String;"),
    NATIVE_METHOD(DexFile, isDexOptNeeded, "(Ljava/lang/String;)Z"),
    NATIVE_METHOD(DexFile,
                  getDexOptNeeded,
                  "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;ZZ)I"),
    NATIVE_METHOD(DexFile,
                  openDexFileNative,
                  "(Ljava/lang/String;"
                  "Ljava/lang/String;"
                  "I"
                  "Ljava/lang/ClassLoader;"
                  "[Ldalvik/system/DexPathList$Element;"
                  ")Ljava/lang/Object;"),
    NATIVE_METHOD(DexFile,
                  openInMemoryDexFilesNative,
                  "([Ljava/nio/ByteBuffer;"
                  "[[B"
                  "[I"
                  "[I"
                  "Ljava/lang/ClassLoader;"
                  "[Ldalvik/system/DexPathList$Element;"
                  ")Ljava/lang/Object;"),
    NATIVE_METHOD(DexFile,
                  verifyInBackgroundNative,
                  "(Ljava/lang/Object;"
                  "Ljava/lang/ClassLoader;"
                  ")V"),
    NATIVE_METHOD(DexFile, isValidCompilerFilter, "(Ljava/lang/String;)Z"),
    NATIVE_METHOD(DexFile, isProfileGuidedCompilerFilter, "(Ljava/lang/String;)Z"),
    NATIVE_METHOD(DexFile, isVerifiedCompilerFilter, "(Ljava/lang/String;)Z"),
    NATIVE_METHOD(DexFile, isOptimizedCompilerFilter, "(Ljava/lang/String;)Z"),
    NATIVE_METHOD(DexFile, isReadOnlyJavaDclEnforced, "()Z"),
    NATIVE_METHOD(
        DexFile, getNonProfileGuidedCompilerFilter, "(Ljava/lang/String;)Ljava/lang/String;"),
    NATIVE_METHOD(DexFile, getSafeModeCompilerFilter, "(Ljava/lang/String;)Ljava/lang/String;"),
    NATIVE_METHOD(DexFile, isBackedByOatFile, "(Ljava/lang/Object;)Z"),
    NATIVE_METHOD(DexFile,
                  getDexFileOutputPaths,
                  "(Ljava/lang/String;Ljava/lang/String;)[Ljava/lang/String;"),
    NATIVE_METHOD(DexFile, getStaticSizeOfDexFile, "(Ljava/lang/Object;)J"),
    NATIVE_METHOD(DexFile,
                  getDexFileOptimizationStatus,
                  "(Ljava/lang/String;Ljava/lang/String;)[Ljava/lang/String;"),
    NATIVE_METHOD(DexFile, setTrusted, "(Ljava/lang/Object;)V")};

void register_dalvik_system_DexFile(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("dalvik/system/DexFile");
}

}  // namespace art
