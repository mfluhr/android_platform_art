/*
 * Copyright (C) 2014 The Android Open Source Project
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

#define LOG_TAG "nativebridge"

#include "nativebridge/native_bridge.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>

#include <android-base/macros.h>
#include <log/log.h>

#ifdef ART_TARGET_ANDROID
#include "nativeloader/dlext_namespaces.h"
#endif

namespace android {

#ifdef __APPLE__
template <typename T>
void UNUSED(const T&) {}
#endif

extern "C" {

void* OpenSystemLibrary(const char* path, int flags) {
#ifdef ART_TARGET_ANDROID
  // The system namespace is called "default" for binaries in /system and
  // "system" for those in the Runtime APEX. Try "system" first since
  // "default" always exists.
  // TODO(b/185587109): Get rid of this error prone logic.
  android_namespace_t* system_ns = android_get_exported_namespace("system");
  if (system_ns == nullptr) {
    system_ns = android_get_exported_namespace("default");
    LOG_ALWAYS_FATAL_IF(system_ns == nullptr,
                        "Failed to get system namespace for loading %s", path);
  }
  const android_dlextinfo dlextinfo = {
      .flags = ANDROID_DLEXT_USE_NAMESPACE,
      .library_namespace = system_ns,
  };
  return android_dlopen_ext(path, flags, &dlextinfo);
#else
  return dlopen(path, flags);
#endif
}

// Environment values required by the apps running with native bridge.
struct NativeBridgeRuntimeValues {
    const char* os_arch;
    const char* cpu_abi;
    const char* cpu_abi2;
    const char* *supported_abis;
    int32_t abi_count;
};

// The symbol name exposed by native-bridge with the type of NativeBridgeCallbacks.
static constexpr const char* kNativeBridgeInterfaceSymbol = "NativeBridgeItf";

enum class NativeBridgeState {
  kNotSetup,                        // Initial state.
  kOpened,                          // After successful dlopen.
  kPreInitialized,                  // After successful pre-initialization.
  kInitialized,                     // After successful initialization.
  kClosed                           // Closed or errors.
};

static constexpr const char* kNotSetupString = "kNotSetup";
static constexpr const char* kOpenedString = "kOpened";
static constexpr const char* kPreInitializedString = "kPreInitialized";
static constexpr const char* kInitializedString = "kInitialized";
static constexpr const char* kClosedString = "kClosed";

static const char* GetNativeBridgeStateString(NativeBridgeState state) {
  switch (state) {
    case NativeBridgeState::kNotSetup:
      return kNotSetupString;

    case NativeBridgeState::kOpened:
      return kOpenedString;

    case NativeBridgeState::kPreInitialized:
      return kPreInitializedString;

    case NativeBridgeState::kInitialized:
      return kInitializedString;

    case NativeBridgeState::kClosed:
      return kClosedString;
  }
}

// Current state of the native bridge.
static NativeBridgeState state = NativeBridgeState::kNotSetup;

// The version of NativeBridge implementation.
// Different Nativebridge interface needs the service of different version of
// Nativebridge implementation.
// Used by isCompatibleWith() which is introduced in v2.
enum NativeBridgeImplementationVersion {
  // first version, not used.
  DEFAULT_VERSION = 1,
  // The version which signal semantic is introduced.
  SIGNAL_VERSION = 2,
  // The version which namespace semantic is introduced.
  NAMESPACE_VERSION = 3,
  // The version with vendor namespaces
  VENDOR_NAMESPACE_VERSION = 4,
  // The version with runtime namespaces
  RUNTIME_NAMESPACE_VERSION = 5,
  // The version with pre-zygote-fork hook to support app-zygotes.
  PRE_ZYGOTE_FORK_VERSION = 6,
  // The version with critical_native support
  CRITICAL_NATIVE_SUPPORT_VERSION = 7,
  // The version with native bridge detection fallback for function pointers
  IDENTIFY_NATIVELY_BRIDGED_FUNCTION_POINTERS_VERSION = 8,
};

// Whether we had an error at some point.
static bool had_error = false;

// Handle of the loaded library.
static void* native_bridge_handle = nullptr;
// Pointer to the callbacks. Available as soon as LoadNativeBridge succeeds, but only initialized
// later.
static const NativeBridgeCallbacks* callbacks = nullptr;
// Callbacks provided by the environment to the bridge. Passed to LoadNativeBridge.
static const NativeBridgeRuntimeCallbacks* runtime_callbacks = nullptr;

// The app's code cache directory.
static char* app_code_cache_dir = nullptr;

// Code cache directory (relative to the application private directory)
// Ideally we'd like to call into framework to retrieve this name. However that's considered an
// implementation detail and will require either hacks or consistent refactorings. We compromise
// and hard code the directory name again here.
static constexpr const char* kCodeCacheDir = "code_cache";

// Characters allowed in a native bridge filename. The first character must
// be in [a-zA-Z] (expected 'l' for "libx"). The rest must be in [a-zA-Z0-9._-].
static bool CharacterAllowed(char c, bool first) {
  if (first) {
    return ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z');
  } else {
    return ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') || ('0' <= c && c <= '9') ||
           (c == '.') || (c == '_') || (c == '-');
  }
}

static void ReleaseAppCodeCacheDir() {
  if (app_code_cache_dir != nullptr) {
    delete[] app_code_cache_dir;
    app_code_cache_dir = nullptr;
  }
}

// We only allow simple names for the library. It is supposed to be a file in
// /system/lib or /vendor/lib. Only allow a small range of characters, that is
// names consisting of [a-zA-Z0-9._-] and starting with [a-zA-Z].
bool NativeBridgeNameAcceptable(const char* nb_library_filename) {
  const char* ptr = nb_library_filename;
  if (*ptr == 0) {
    // Emptry string. Allowed, means no native bridge.
    return true;
  } else {
    // First character must be [a-zA-Z].
    if (!CharacterAllowed(*ptr, true))  {
      // Found an invalid fist character, don't accept.
      ALOGE("Native bridge library %s has been rejected for first character %c",
            nb_library_filename,
            *ptr);
      return false;
    } else {
      // For the rest, be more liberal.
      ptr++;
      while (*ptr != 0) {
        if (!CharacterAllowed(*ptr, false)) {
          // Found an invalid character, don't accept.
          ALOGE("Native bridge library %s has been rejected for %c", nb_library_filename, *ptr);
          return false;
        }
        ptr++;
      }
    }
    return true;
  }
}

// The policy of invoking Nativebridge changed in v3 with/without namespace.
// Suggest Nativebridge implementation not maintain backward-compatible.
static bool isCompatibleWith(const uint32_t version) {
  // Libnativebridge is now designed to be forward-compatible. So only "0" is an unsupported
  // version.
  if (callbacks == nullptr || callbacks->version == 0 || version == 0) {
    return false;
  }

  // If this is a v2+ bridge, it may not be forwards- or backwards-compatible. Check.
  if (callbacks->version >= SIGNAL_VERSION) {
    return callbacks->isCompatibleWith(version);
  }

  return true;
}

static void CloseNativeBridge(bool with_error) {
  state = NativeBridgeState::kClosed;
  had_error |= with_error;
  ReleaseAppCodeCacheDir();
}

bool LoadNativeBridge(const char* nb_library_filename,
                      const NativeBridgeRuntimeCallbacks* runtime_cbs) {
  // We expect only one place that calls LoadNativeBridge: Runtime::Init. At that point we are not
  // multi-threaded, so we do not need locking here.

  if (state != NativeBridgeState::kNotSetup) {
    // Setup has been called before. Ignore this call.
    if (nb_library_filename != nullptr) {  // Avoids some log-spam for dalvikvm.
      ALOGW("Called LoadNativeBridge for an already set up native bridge. State is %s.",
            GetNativeBridgeStateString(state));
    }
    // Note: counts as an error, even though the bridge may be functional.
    had_error = true;
    return false;
  }

  if (nb_library_filename == nullptr || *nb_library_filename == 0) {
    CloseNativeBridge(false);
    return false;
  } else {
    if (!NativeBridgeNameAcceptable(nb_library_filename)) {
      CloseNativeBridge(true);
    } else {
      // Try to open the library. We assume this library is provided by the
      // platform rather than the ART APEX itself, so use the system namespace
      // to avoid requiring a static linker config link to it from the
      // com_android_art namespace.
      void* handle = OpenSystemLibrary(nb_library_filename, RTLD_LAZY);

      if (handle != nullptr) {
        callbacks = reinterpret_cast<NativeBridgeCallbacks*>(dlsym(handle,
                                                                   kNativeBridgeInterfaceSymbol));
        if (callbacks != nullptr) {
          if (isCompatibleWith(NAMESPACE_VERSION)) {
            // Store the handle for later.
            native_bridge_handle = handle;
          } else {
            ALOGW("Unsupported native bridge API in %s (is version %d not compatible with %d)",
                  nb_library_filename, callbacks->version, NAMESPACE_VERSION);
            callbacks = nullptr;
            dlclose(handle);
          }
        } else {
          dlclose(handle);
          ALOGW("Unsupported native bridge API in %s: %s not found",
                nb_library_filename, kNativeBridgeInterfaceSymbol);
        }
      } else {
        ALOGW("Failed to load native bridge implementation: %s", dlerror());
      }

      // Two failure conditions: could not find library (dlopen failed), or could not find native
      // bridge interface (dlsym failed). Both are an error and close the native bridge.
      if (callbacks == nullptr) {
        CloseNativeBridge(true);
      } else {
        runtime_callbacks = runtime_cbs;
        state = NativeBridgeState::kOpened;
      }
    }
    return state == NativeBridgeState::kOpened;
  }
}

bool NeedsNativeBridge(const char* instruction_set) {
  if (instruction_set == nullptr) {
    ALOGE("Null instruction set in NeedsNativeBridge.");
    return false;
  }
  return strncmp(instruction_set, ABI_STRING, strlen(ABI_STRING) + 1) != 0;
}

#ifndef __APPLE__
static bool MountCpuinfo(const char* cpuinfo_path) {
  // If the file does not exist, the mount command will fail,
  // so we save the extra file existence check.
  if (TEMP_FAILURE_RETRY(mount(cpuinfo_path,        // Source.
                               "/proc/cpuinfo",     // Target.
                               nullptr,             // FS type.
                               MS_BIND,             // Mount flags: bind mount.
                               nullptr)) == -1) {   // "Data."
    ALOGW("Failed to bind-mount %s as /proc/cpuinfo: %s", cpuinfo_path, strerror(errno));
    return false;
  }
  return true;
}
#endif

static void MountCpuinfoForInstructionSet(const char* instruction_set) {
  if (instruction_set == nullptr) {
    return;
  }

  size_t isa_len = strlen(instruction_set);
  if (isa_len > 10) {
    // 10 is a loose upper bound on the currently known instruction sets (a tight bound is 7 for
    // x86_64 [including the trailing \0]). This is so we don't have to change here if there will
    // be another instruction set in the future.
    ALOGW("Instruction set %s is malformed, must be less than or equal to 10 characters.",
          instruction_set);
    return;
  }

#if defined(__APPLE__)
  ALOGW("Mac OS does not support bind-mounting. Host simulation of native bridge impossible.");

#elif !defined(__ANDROID__)
  // To be able to test on the host, we hardwire a relative path.
  MountCpuinfo("./cpuinfo");

#else  // __ANDROID__
  char cpuinfo_path[1024];

  // Bind-mount /system/etc/cpuinfo.<isa>.txt to /proc/cpuinfo.
  snprintf(cpuinfo_path, sizeof(cpuinfo_path), "/system/etc/cpuinfo.%s.txt", instruction_set);
  if (MountCpuinfo(cpuinfo_path)) {
    return;
  }

  // Bind-mount /system/lib{,64}/<isa>/cpuinfo to /proc/cpuinfo.
  // TODO(b/179753190): remove when all implementations migrate to system/etc!
#ifdef __LP64__
  snprintf(cpuinfo_path, sizeof(cpuinfo_path), "/system/lib64/%s/cpuinfo", instruction_set);
#else
  snprintf(cpuinfo_path, sizeof(cpuinfo_path), "/system/lib/%s/cpuinfo", instruction_set);
#endif  // __LP64__
  MountCpuinfo(cpuinfo_path);

#endif
}

bool PreInitializeNativeBridge(const char* app_data_dir_in, const char* instruction_set) {
  if (state != NativeBridgeState::kOpened) {
    ALOGE("Invalid state: native bridge is expected to be opened.");
    CloseNativeBridge(true);
    return false;
  }

  if (app_data_dir_in != nullptr) {
    // Create the path to the application code cache directory.
    // The memory will be release after Initialization or when the native bridge is closed.
    const size_t len = strlen(app_data_dir_in) + strlen(kCodeCacheDir) + 2;  // '\0' + '/'
    app_code_cache_dir = new char[len];
    snprintf(app_code_cache_dir, len, "%s/%s", app_data_dir_in, kCodeCacheDir);
  } else {
    ALOGW("Application private directory isn't available.");
    app_code_cache_dir = nullptr;
  }

  // Mount cpuinfo that corresponds to the instruction set.
  // Failure is not fatal.
  MountCpuinfoForInstructionSet(instruction_set);

  state = NativeBridgeState::kPreInitialized;
  return true;
}

void PreZygoteForkNativeBridge() {
  if (NativeBridgeInitialized()) {
    if (isCompatibleWith(PRE_ZYGOTE_FORK_VERSION)) {
      return callbacks->preZygoteFork();
    } else {
      ALOGE("not compatible with version %d, preZygoteFork() isn't invoked",
            PRE_ZYGOTE_FORK_VERSION);
    }
  }
}

static void SetCpuAbi(JNIEnv* env, jclass build_class, const char* field, const char* value) {
  if (value != nullptr) {
    jfieldID field_id = env->GetStaticFieldID(build_class, field, "Ljava/lang/String;");
    if (field_id == nullptr) {
      env->ExceptionClear();
      ALOGW("Could not find %s field.", field);
      return;
    }

    jstring str = env->NewStringUTF(value);
    if (str == nullptr) {
      env->ExceptionClear();
      ALOGW("Could not create string %s.", value);
      return;
    }

    env->SetStaticObjectField(build_class, field_id, str);
  }
}

// Set up the environment for the bridged app.
static void SetupEnvironment(const NativeBridgeCallbacks* cbs, JNIEnv* env, const char* isa) {
  // Need a JNIEnv* to do anything.
  if (env == nullptr) {
    ALOGW("No JNIEnv* to set up app environment.");
    return;
  }

  // Query the bridge for environment values.
  const struct NativeBridgeRuntimeValues* env_values = cbs->getAppEnv(isa);
  if (env_values == nullptr) {
    return;
  }

  // Keep the JNIEnv clean.
  jint success = env->PushLocalFrame(16);  // That should be small and large enough.
  if (success < 0) {
    // Out of memory, really borked.
    ALOGW("Out of memory while setting up app environment.");
    env->ExceptionClear();
    return;
  }

  // Reset CPU_ABI & CPU_ABI2 to values required by the apps running with native bridge.
  if (env_values->cpu_abi != nullptr || env_values->cpu_abi2 != nullptr ||
      env_values->abi_count >= 0) {
    jclass bclass_id = env->FindClass("android/os/Build");
    if (bclass_id != nullptr) {
      SetCpuAbi(env, bclass_id, "CPU_ABI", env_values->cpu_abi);
      SetCpuAbi(env, bclass_id, "CPU_ABI2", env_values->cpu_abi2);
    } else {
      // For example in a host test environment.
      env->ExceptionClear();
      ALOGW("Could not find Build class.");
    }
  }

  if (env_values->os_arch != nullptr) {
    jclass sclass_id = env->FindClass("java/lang/System");
    if (sclass_id != nullptr) {
      jmethodID set_prop_id = env->GetStaticMethodID(sclass_id, "setUnchangeableSystemProperty",
          "(Ljava/lang/String;Ljava/lang/String;)V");
      if (set_prop_id != nullptr) {
        // Init os.arch to the value reqired by the apps running with native bridge.
        env->CallStaticVoidMethod(sclass_id, set_prop_id, env->NewStringUTF("os.arch"),
            env->NewStringUTF(env_values->os_arch));
      } else {
        env->ExceptionClear();
        ALOGW("Could not find System#setUnchangeableSystemProperty.");
      }
    } else {
      env->ExceptionClear();
      ALOGW("Could not find System class.");
    }
  }

  // Make it pristine again.
  env->PopLocalFrame(nullptr);
}

bool InitializeNativeBridge(JNIEnv* env, const char* instruction_set) {
  // We expect only one place that calls InitializeNativeBridge: Runtime::DidForkFromZygote. At that
  // point we are not multi-threaded, so we do not need locking here.

  if (state == NativeBridgeState::kPreInitialized) {
    if (app_code_cache_dir != nullptr) {
      // Check for code cache: if it doesn't exist try to create it.
      struct stat st;
      if (stat(app_code_cache_dir, &st) == -1) {
        if (errno == ENOENT) {
          if (mkdir(app_code_cache_dir, S_IRWXU | S_IRWXG | S_IXOTH) == -1) {
            ALOGW("Cannot create code cache directory %s: %s.",
                  app_code_cache_dir, strerror(errno));
            ReleaseAppCodeCacheDir();
          }
        } else {
          ALOGW("Cannot stat code cache directory %s: %s.",
                app_code_cache_dir, strerror(errno));
          ReleaseAppCodeCacheDir();
        }
      } else if (!S_ISDIR(st.st_mode)) {
        ALOGW("Code cache is not a directory %s.", app_code_cache_dir);
        ReleaseAppCodeCacheDir();
      }
    }

    // If we're still PreInitialized (didn't fail the code cache checks) try to initialize.
    if (state == NativeBridgeState::kPreInitialized) {
      if (callbacks->initialize(runtime_callbacks, app_code_cache_dir, instruction_set)) {
        SetupEnvironment(callbacks, env, instruction_set);
        state = NativeBridgeState::kInitialized;
        // We no longer need the code cache path, release the memory.
        ReleaseAppCodeCacheDir();
      } else {
        // Unload the library.
        dlclose(native_bridge_handle);
        CloseNativeBridge(true);
      }
    }
  } else {
    CloseNativeBridge(true);
  }

  return state == NativeBridgeState::kInitialized;
}

void UnloadNativeBridge() {
  // We expect only one place that calls UnloadNativeBridge: Runtime::DidForkFromZygote. At that
  // point we are not multi-threaded, so we do not need locking here.

  switch (state) {
    case NativeBridgeState::kOpened:
    case NativeBridgeState::kPreInitialized:
    case NativeBridgeState::kInitialized:
      // Unload.
      dlclose(native_bridge_handle);
      CloseNativeBridge(false);
      break;

    case NativeBridgeState::kNotSetup:
      // Not even set up. Error.
      CloseNativeBridge(true);
      break;

    case NativeBridgeState::kClosed:
      // Ignore.
      break;
  }
}

bool NativeBridgeError() {
  return had_error;
}

bool NativeBridgeAvailable() {
  return state == NativeBridgeState::kOpened
      || state == NativeBridgeState::kPreInitialized
      || state == NativeBridgeState::kInitialized;
}

bool NativeBridgeInitialized() {
  // Calls of this are supposed to happen in a state where the native bridge is stable, i.e., after
  // Runtime::DidForkFromZygote. In that case we do not need a lock.
  return state == NativeBridgeState::kInitialized;
}

void* NativeBridgeLoadLibrary(const char* libpath, int flag) {
  if (NativeBridgeInitialized()) {
    return callbacks->loadLibrary(libpath, flag);
  }
  return nullptr;
}

void* NativeBridgeGetTrampoline(void* handle, const char* name, const char* shorty,
                                uint32_t len) {
  return NativeBridgeGetTrampoline2(handle, name, shorty, len, kJNICallTypeRegular);
}

void* NativeBridgeGetTrampoline2(
    void* handle, const char* name, const char* shorty, uint32_t len, JNICallType jni_call_type) {
  if (!NativeBridgeInitialized()) {
    return nullptr;
  }

  // For version 1 isCompatibleWith is always true, even though the extensions
  // are not supported, so we need to handle it separately.
  if (callbacks != nullptr && callbacks->version == DEFAULT_VERSION) {
    return callbacks->getTrampoline(handle, name, shorty, len);
  }

  if (isCompatibleWith(CRITICAL_NATIVE_SUPPORT_VERSION)) {
    return callbacks->getTrampolineWithJNICallType(handle, name, shorty, len, jni_call_type);
  }

  return callbacks->getTrampoline(handle, name, shorty, len);
}

void* NativeBridgeGetTrampolineForFunctionPointer(const void* method,
                                                  const char* shorty,
                                                  uint32_t len,
                                                  JNICallType jni_call_type) {
  if (!NativeBridgeInitialized()) {
    return nullptr;
  }

  if (isCompatibleWith(CRITICAL_NATIVE_SUPPORT_VERSION)) {
    return callbacks->getTrampolineForFunctionPointer(method, shorty, len, jni_call_type);
  } else {
    ALOGE("not compatible with version %d, getTrampolineFnPtrWithJNICallType() isn't invoked",
          CRITICAL_NATIVE_SUPPORT_VERSION);
    return nullptr;
  }
}

bool NativeBridgeIsSupported(const char* libpath) {
  if (NativeBridgeInitialized()) {
    return callbacks->isSupported(libpath);
  }
  return false;
}

uint32_t NativeBridgeGetVersion() {
  if (NativeBridgeAvailable()) {
    return callbacks->version;
  }
  return 0;
}

NativeBridgeSignalHandlerFn NativeBridgeGetSignalHandler(int signal) {
  if (NativeBridgeInitialized()) {
    if (isCompatibleWith(SIGNAL_VERSION)) {
      return callbacks->getSignalHandler(signal);
    } else {
      ALOGE("not compatible with version %d, cannot get signal handler", SIGNAL_VERSION);
    }
  }
  return nullptr;
}

int NativeBridgeUnloadLibrary(void* handle) {
  if (NativeBridgeInitialized()) {
    if (isCompatibleWith(NAMESPACE_VERSION)) {
      return callbacks->unloadLibrary(handle);
    } else {
      ALOGE("not compatible with version %d, cannot unload library", NAMESPACE_VERSION);
    }
  }
  return -1;
}

const char* NativeBridgeGetError() {
  if (NativeBridgeInitialized()) {
    if (isCompatibleWith(NAMESPACE_VERSION)) {
      return callbacks->getError();
    } else {
      return "native bridge implementation is not compatible with version 3, cannot get message";
    }
  }
  return "native bridge is not initialized";
}

bool NativeBridgeIsPathSupported(const char* path) {
  if (NativeBridgeInitialized()) {
    if (isCompatibleWith(NAMESPACE_VERSION)) {
      return callbacks->isPathSupported(path);
    } else {
      ALOGE("not compatible with version %d, cannot check via library path", NAMESPACE_VERSION);
    }
  }
  return false;
}

native_bridge_namespace_t* NativeBridgeCreateNamespace(const char* name,
                                                       const char* ld_library_path,
                                                       const char* default_library_path,
                                                       uint64_t type,
                                                       const char* permitted_when_isolated_path,
                                                       native_bridge_namespace_t* parent_ns) {
  if (NativeBridgeInitialized()) {
    if (isCompatibleWith(NAMESPACE_VERSION)) {
      return callbacks->createNamespace(name,
                                        ld_library_path,
                                        default_library_path,
                                        type,
                                        permitted_when_isolated_path,
                                        parent_ns);
    } else {
      ALOGE("not compatible with version %d, cannot create namespace %s", NAMESPACE_VERSION, name);
    }
  }

  return nullptr;
}

bool NativeBridgeLinkNamespaces(native_bridge_namespace_t* from, native_bridge_namespace_t* to,
                                const char* shared_libs_sonames) {
  if (NativeBridgeInitialized()) {
    if (isCompatibleWith(NAMESPACE_VERSION)) {
      return callbacks->linkNamespaces(from, to, shared_libs_sonames);
    } else {
      ALOGE("not compatible with version %d, cannot init namespace", NAMESPACE_VERSION);
    }
  }

  return false;
}

native_bridge_namespace_t* NativeBridgeGetExportedNamespace(const char* name) {
  if (!NativeBridgeInitialized()) {
    return nullptr;
  }

  if (isCompatibleWith(RUNTIME_NAMESPACE_VERSION)) {
    return callbacks->getExportedNamespace(name);
  }

  // sphal is vendor namespace name -> use v4 callback in the case NB callbacks
  // are not compatible with v5
  if (isCompatibleWith(VENDOR_NAMESPACE_VERSION) && name != nullptr && strcmp("sphal", name) == 0) {
    return callbacks->getVendorNamespace();
  }

  return nullptr;
}

void* NativeBridgeLoadLibraryExt(const char* libpath, int flag, native_bridge_namespace_t* ns) {
  if (NativeBridgeInitialized()) {
    if (isCompatibleWith(NAMESPACE_VERSION)) {
      return callbacks->loadLibraryExt(libpath, flag, ns);
    } else {
      ALOGE("not compatible with version %d, cannot load library in namespace", NAMESPACE_VERSION);
    }
  }
  return nullptr;
}

bool NativeBridgeIsNativeBridgeFunctionPointer(const void* method) {
  if (NativeBridgeInitialized()) {
    if (isCompatibleWith(IDENTIFY_NATIVELY_BRIDGED_FUNCTION_POINTERS_VERSION)) {
      return callbacks->isNativeBridgeFunctionPointer(method);
    } else {
      ALOGW("not compatible with version %d, unable to call isNativeBridgeFunctionPointer",
            IDENTIFY_NATIVELY_BRIDGED_FUNCTION_POINTERS_VERSION);
    }
  }
  return false;
}

}  // extern "C"

}  // namespace android
