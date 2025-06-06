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

#include "dalvik_system_VMDebug.h"

#include <string.h>
#include <unistd.h>

#include <sstream>

#include "base/file_utils.h"
#include "base/histogram-inl.h"
#include "base/time_utils.h"
#include "class_linker.h"
#include "class_root-inl.h"
#include "common_throws.h"
#include "debugger.h"
#include "dex/class_accessor-inl.h"
#include "dex/descriptors_names.h"
#include "gc/space/bump_pointer_space.h"
#include "gc/space/dlmalloc_space.h"
#include "gc/space/large_object_space.h"
#include "gc/space/space-inl.h"
#include "gc/space/zygote_space.h"
#include "handle_scope-inl.h"
#include "hprof/hprof.h"
#include "jni/java_vm_ext.h"
#include "jni/jni_internal.h"
#include "mirror/array-alloc-inl.h"
#include "mirror/array-inl.h"
#include "mirror/class.h"
#include "mirror/executable-inl.h"
#include "mirror/object_array-alloc-inl.h"
#include "native_util.h"
#include "nativehelper/jni_macros.h"
#include "nativehelper/scoped_local_ref.h"
#include "nativehelper/scoped_utf_chars.h"
#include "nativehelper/utils.h"
#include "oat/oat_quick_method_header.h"
#include "scoped_fast_native_object_access-inl.h"
#include "string_array_utils.h"
#include "thread-inl.h"
#include "trace.h"
#include "trace_profile.h"

namespace art HIDDEN {

static jobjectArray VMDebug_getVmFeatureList(JNIEnv* env, jclass) {
  ScopedObjectAccess soa(Thread::ForEnv(env));
  return soa.AddLocalReference<jobjectArray>(
      CreateStringArray(soa.Self(),
                        {
                            "method-trace-profiling",
                            "method-trace-profiling-streaming",
                            "method-sample-profiling",
                            "hprof-heap-dump",
                            "hprof-heap-dump-streaming",
                            "app_info",
                        }));
}

static void VMDebug_startAllocCounting(JNIEnv*, jclass) {
  Runtime::Current()->SetStatsEnabled(true);
}

static void VMDebug_stopAllocCounting(JNIEnv*, jclass) {
  Runtime::Current()->SetStatsEnabled(false);
}

static jint VMDebug_getAllocCount(JNIEnv*, jclass, jint kind) {
  return static_cast<jint>(Runtime::Current()->GetStat(kind));
}

static void VMDebug_resetAllocCount(JNIEnv*, jclass, jint kinds) {
  Runtime::Current()->ResetStats(kinds);
}

static void VMDebug_startMethodTracingDdmsImpl(JNIEnv*, jclass, jint bufferSize, jint flags,
                                               jboolean samplingEnabled, jint intervalUs) {
  Trace::StartDDMS(bufferSize,
                   flags,
                   samplingEnabled ? Trace::TraceMode::kSampling : Trace::TraceMode::kMethodTracing,
                   intervalUs);
}

static void VMDebug_startMethodTracingFd(JNIEnv* env,
                                         jclass,
                                         [[maybe_unused]] jstring javaTraceFilename,
                                         jint javaFd,
                                         jint bufferSize,
                                         jint flags,
                                         jboolean samplingEnabled,
                                         jint intervalUs,
                                         jboolean streamingOutput) {
  int originalFd = javaFd;
  if (originalFd < 0) {
    ScopedObjectAccess soa(env);
    soa.Self()->ThrowNewExceptionF("Ljava/lang/RuntimeException;",
                                   "Trace fd is invalid: %d",
                                   originalFd);
    return;
  }

  int fd = DupCloexec(originalFd);
  if (fd < 0) {
    ScopedObjectAccess soa(env);
    soa.Self()->ThrowNewExceptionF("Ljava/lang/RuntimeException;",
                                   "dup(%d) failed: %s",
                                   originalFd,
                                   strerror(errno));
    return;
  }

  // Ignore the traceFilename.
  TraceOutputMode outputMode =
      streamingOutput ? TraceOutputMode::kStreaming : TraceOutputMode::kFile;
  Trace::Start(fd,
               bufferSize,
               flags,
               outputMode,
               samplingEnabled ? Trace::TraceMode::kSampling : Trace::TraceMode::kMethodTracing,
               intervalUs);
}

static void VMDebug_startMethodTracingFilename(JNIEnv* env, jclass, jstring javaTraceFilename,
                                               jint bufferSize, jint flags,
                                               jboolean samplingEnabled, jint intervalUs) {
  ScopedUtfChars traceFilename(env, javaTraceFilename);
  if (traceFilename.c_str() == nullptr) {
    return;
  }
  Trace::Start(traceFilename.c_str(),
               bufferSize,
               flags,
               TraceOutputMode::kFile,
               samplingEnabled ? Trace::TraceMode::kSampling : Trace::TraceMode::kMethodTracing,
               intervalUs);
}

static jint VMDebug_getMethodTracingMode(JNIEnv*, jclass) {
  return Trace::GetMethodTracingMode();
}

static void VMDebug_stopMethodTracing(JNIEnv*, jclass) {
  Trace::Stop();
}

static void VMDebug_stopLowOverheadTraceImpl(JNIEnv*, jclass) {
  TraceProfiler::Stop();
}

static void VMDebug_dumpLowOverheadTraceImpl(JNIEnv* env, jclass, jstring javaProfileFileName) {
  ScopedUtfChars profileFileName(env, javaProfileFileName);
  if (profileFileName.c_str() == nullptr) {
    LOG(ERROR) << "Filename not provided, ignoring the request to dump low-overhead trace";
    return;
  }
  TraceProfiler::Dump(profileFileName.c_str());
}

static void VMDebug_dumpLowOverheadTraceFdImpl(JNIEnv*, jclass, jint originalFd) {
  if (originalFd < 0) {
    LOG(ERROR) << "Invalid file descriptor, ignoring the request to dump low-overhead trace";
    return;
  }

  // Set the O_CLOEXEC flag atomically here, so the file gets closed when a new process is forked.
  int fd = DupCloexec(originalFd);
  if (fd < 0) {
    LOG(ERROR)
        << "Unable to dup the file descriptor, ignoring the request to dump low-overhead trace";
    return;
  }

  TraceProfiler::Dump(fd);
}

static void VMDebug_startLowOverheadTraceForAllMethodsImpl(JNIEnv*, jclass) {
  TraceProfiler::Start();
}

static void VMDebug_startLowOverheadTraceForLongRunningMethodsImpl(JNIEnv*,
                                                                   jclass,
                                                                   jlong traceDuration) {
  TraceProfiler::StartTraceLongRunningMethods(traceDuration);
}

static jboolean VMDebug_isDebuggerConnected(JNIEnv*, jclass) {
  // This function will be replaced by the debugger when it's connected. See
  // external/oj-libjdwp/src/share/vmDebug.c for implementation when debugger is connected.
  return false;
}

static jboolean VMDebug_isDebuggingEnabled(JNIEnv* env, jclass) {
  ScopedObjectAccess soa(env);
  return Runtime::Current()->GetRuntimeCallbacks()->IsDebuggerConfigured();
}

static jlong VMDebug_lastDebuggerActivity(JNIEnv*, jclass) {
  // This function will be replaced by the debugger when it's connected. See
  // external/oj-libjdwp/src/share/vmDebug.c for implementation when debugger is connected.
  return -1;
}

static void VMDebug_suspendAllAndSendVmStart(JNIEnv*, jclass)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  // This function will be replaced by the debugger when it's connected. See
  // external/oj-libjdwp/src/share/vmDebug.c for implementation when debugger is connected.
  ThrowRuntimeException("ART's suspendAllAndSendVmStart is not implemented");
}

static void VMDebug_printLoadedClasses(JNIEnv* env, jclass, jint flags) {
  class DumpClassVisitor : public ClassVisitor {
   public:
    explicit DumpClassVisitor(int dump_flags) : flags_(dump_flags) {}

    bool operator()(ObjPtr<mirror::Class> klass) override REQUIRES_SHARED(Locks::mutator_lock_) {
      klass->DumpClass(LOG_STREAM(ERROR), flags_);
      return true;
    }

   private:
    const int flags_;
  };
  DumpClassVisitor visitor(flags);

  ScopedFastNativeObjectAccess soa(env);
  return Runtime::Current()->GetClassLinker()->VisitClasses(&visitor);
}

static jint VMDebug_getLoadedClassCount(JNIEnv* env, jclass) {
  ScopedFastNativeObjectAccess soa(env);
  return Runtime::Current()->GetClassLinker()->NumLoadedClasses();
}

/*
 * Returns the thread-specific CPU-time clock value for the current thread,
 * or -1 if the feature isn't supported.
 */
static jlong VMDebug_threadCpuTimeNanos(JNIEnv*, jclass) {
  return ThreadCpuNanoTime();
}

/*
 * static void dumpHprofData(String fileName, FileDescriptor fd)
 *
 * Cause "hprof" data to be dumped.  We can throw an IOException if an
 * error occurs during file handling.
 */
static void VMDebug_dumpHprofData(JNIEnv* env, jclass, jstring javaFilename, jint javaFd) {
  // Only one of these may be null.
  if (javaFilename == nullptr && javaFd < 0) {
    ScopedObjectAccess soa(env);
    ThrowNullPointerException("fileName == null && fd == null");
    return;
  }

  std::string filename;
  if (javaFilename != nullptr) {
    ScopedUtfChars chars(env, javaFilename);
    if (env->ExceptionCheck()) {
      return;
    }
    filename = chars.c_str();
  } else {
    filename = "[fd]";
  }

  int fd = javaFd;

  hprof::DumpHeap(filename.c_str(), fd, false);
}

static void VMDebug_dumpHprofDataDdms(JNIEnv*, jclass) {
  hprof::DumpHeap("[DDMS]", -1, true);
}

static void VMDebug_dumpReferenceTables(JNIEnv* env, jclass) {
  ScopedObjectAccess soa(env);
  LOG(INFO) << "--- reference table dump ---";

  soa.Env()->DumpReferenceTables(LOG_STREAM(INFO));
  soa.Vm()->DumpReferenceTables(LOG_STREAM(INFO));

  LOG(INFO) << "---";
}

static jlong VMDebug_countInstancesOfClass(JNIEnv* env,
                                           jclass,
                                           jclass javaClass,
                                           jboolean countAssignable) {
  ScopedObjectAccess soa(env);
  gc::Heap* const heap = Runtime::Current()->GetHeap();
  // Caller's responsibility to do GC if desired.
  ObjPtr<mirror::Class> c = soa.Decode<mirror::Class>(javaClass);
  if (c == nullptr) {
    return 0;
  }
  VariableSizedHandleScope hs(soa.Self());
  std::vector<Handle<mirror::Class>> classes {hs.NewHandle(c)};
  uint64_t count = 0;
  heap->CountInstances(classes, countAssignable, &count);
  return count;
}

static jobject VMDebug_getExecutableMethodFileOffsetsNative(JNIEnv* env,
                                                            jclass,
                                                            jobject javaExecutable) {
  ScopedObjectAccess soa(env);
  ObjPtr<mirror::Executable> m = soa.Decode<mirror::Executable>(javaExecutable);
  if (m == nullptr) {
    soa.Self()->ThrowNewExceptionF("Ljava/lang/RuntimeException;",
                                   "Could not find mirror::Executable for supplied jobject");
    return nullptr;
  }

  ObjPtr<mirror::Class> c = m->GetDeclaringClass();
  if (c == nullptr) {
    soa.Self()->ThrowNewExceptionF("Ljava/lang/RuntimeException;",
                                   "Could not find mirror::Class for supplied jobject");
    return nullptr;
  }

  ArtMethod* art_method = m->GetArtMethod();
  auto oat_method_quick_code =
      reinterpret_cast<const uint8_t*>(art_method->GetOatMethodQuickCode(kRuntimePointerSize));

  if (oat_method_quick_code == nullptr) {
    LOG(ERROR) << "No OatMethodQuickCode for method " << art_method->PrettyMethod();
    return nullptr;
  }

  const OatDexFile* oat_dex_file = c->GetDexFile().GetOatDexFile();
  if (oat_dex_file == nullptr) {
    soa.Self()->ThrowNewExceptionF("Ljava/lang/RuntimeException;", "Could not find oat_dex_file");
    return nullptr;
  }

  const OatFile* oat_file = oat_dex_file->GetOatFile();
  if (oat_file == nullptr) {
    soa.Self()->ThrowNewExceptionF("Ljava/lang/RuntimeException;", "Could not find oat_file");
    return nullptr;
  }

  std::string error_msg;
  const uint8_t* elf_begin = oat_file->ComputeElfBegin(&error_msg);
  if (elf_begin == nullptr) {
    soa.Self()->ThrowNewExceptionF(
        "Ljava/lang/RuntimeException;", "Could not find elf_begin: %s", error_msg.c_str());
    return nullptr;
  }

  size_t adjusted_offset = oat_method_quick_code - elf_begin;

  ScopedLocalRef<jstring> odex_path = CREATE_UTF_OR_RETURN(env, oat_file->GetLocation());
  auto odex_offset = reinterpret_cast64<jlong>(elf_begin);
  auto method_offset = static_cast<jlong>(adjusted_offset);

  ScopedLocalRef<jclass> clazz(env,
                               env->FindClass("dalvik/system/VMDebug$ExecutableMethodFileOffsets"));
  if (clazz == nullptr) {
    soa.Self()->ThrowNewExceptionF(
        "Ljava/lang/RuntimeException;",
        "Could not find dalvik/system/VMDebug$ExecutableMethodFileOffsets");
    return nullptr;
  }

  jmethodID constructor_id = env->GetMethodID(clazz.get(), "<init>", "(Ljava/lang/String;JJ)V");
  return env->NewObject(clazz.get(), constructor_id, odex_path.get(), odex_offset, method_offset);
}

static jlongArray VMDebug_countInstancesOfClasses(JNIEnv* env,
                                                  jclass,
                                                  jobjectArray javaClasses,
                                                  jboolean countAssignable) {
  ScopedObjectAccess soa(env);
  gc::Heap* const heap = Runtime::Current()->GetHeap();
  // Caller's responsibility to do GC if desired.
  ObjPtr<mirror::ObjectArray<mirror::Class>> decoded_classes =
      soa.Decode<mirror::ObjectArray<mirror::Class>>(javaClasses);
  if (decoded_classes == nullptr) {
    return nullptr;
  }
  VariableSizedHandleScope hs(soa.Self());
  std::vector<Handle<mirror::Class>> classes;
  for (size_t i = 0, count = decoded_classes->GetLength(); i < count; ++i) {
    classes.push_back(hs.NewHandle(decoded_classes->Get(i)));
  }
  std::vector<uint64_t> counts(classes.size(), 0u);
  // Heap::CountInstances can handle null and will put 0 for these classes.
  heap->CountInstances(classes, countAssignable, &counts[0]);
  ObjPtr<mirror::LongArray> long_counts = mirror::LongArray::Alloc(soa.Self(), counts.size());
  if (long_counts == nullptr) {
    soa.Self()->AssertPendingOOMException();
    return nullptr;
  }
  for (size_t i = 0; i < counts.size(); ++i) {
    long_counts->Set(i, counts[i]);
  }
  return soa.AddLocalReference<jlongArray>(long_counts);
}

// The runtime stat names for VMDebug.getRuntimeStat().
enum class VMDebugRuntimeStatId {
  kArtGcGcCount = 0,
  kArtGcGcTime,
  kArtGcBytesAllocated,
  kArtGcBytesFreed,
  kArtGcBlockingGcCount,
  kArtGcBlockingGcTime,
  kArtGcGcCountRateHistogram,
  kArtGcBlockingGcCountRateHistogram,
  kArtGcObjectsAllocated,
  kArtGcTotalTimeWaitingForGc,
  kArtGcPreOomeGcCount,
  kNumRuntimeStats,
};

static jstring VMDebug_getRuntimeStatInternal(JNIEnv* env, jclass, jint statId) {
  gc::Heap* heap = Runtime::Current()->GetHeap();
  switch (static_cast<VMDebugRuntimeStatId>(statId)) {
    case VMDebugRuntimeStatId::kArtGcGcCount: {
      std::string output = std::to_string(heap->GetGcCount());
      return env->NewStringUTF(output.c_str());
    }
    case VMDebugRuntimeStatId::kArtGcGcTime: {
      std::string output = std::to_string(NsToMs(heap->GetGcTime()));
      return env->NewStringUTF(output.c_str());
    }
    case VMDebugRuntimeStatId::kArtGcBytesAllocated: {
      std::string output = std::to_string(heap->GetBytesAllocatedEver());
      return env->NewStringUTF(output.c_str());
    }
    case VMDebugRuntimeStatId::kArtGcBytesFreed: {
      std::string output = std::to_string(heap->GetBytesFreedEver());
      return env->NewStringUTF(output.c_str());
    }
    case VMDebugRuntimeStatId::kArtGcBlockingGcCount: {
      std::string output = std::to_string(heap->GetBlockingGcCount());
      return env->NewStringUTF(output.c_str());
    }
    case VMDebugRuntimeStatId::kArtGcBlockingGcTime: {
      std::string output = std::to_string(NsToMs(heap->GetBlockingGcTime()));
      return env->NewStringUTF(output.c_str());
    }
    case VMDebugRuntimeStatId::kArtGcGcCountRateHistogram: {
      std::ostringstream output;
      heap->DumpGcCountRateHistogram(output);
      return env->NewStringUTF(output.str().c_str());
    }
    case VMDebugRuntimeStatId::kArtGcBlockingGcCountRateHistogram: {
      std::ostringstream output;
      heap->DumpBlockingGcCountRateHistogram(output);
      return env->NewStringUTF(output.str().c_str());
    }
    case VMDebugRuntimeStatId::kArtGcObjectsAllocated: {
      std::string output = std::to_string(heap->GetObjectsAllocated());
      return env->NewStringUTF(output.c_str());
    }
    case VMDebugRuntimeStatId::kArtGcTotalTimeWaitingForGc: {
      std::string output = std::to_string(heap->GetTotalTimeWaitingForGC());
      return env->NewStringUTF(output.c_str());
    }
    case VMDebugRuntimeStatId::kArtGcPreOomeGcCount: {
      std::string output = std::to_string(heap->GetPreOomeGcCount());
      return env->NewStringUTF(output.c_str());
    }
    default:
      return nullptr;
  }
}

static bool SetRuntimeStatValue(Thread* self,
                                Handle<mirror::ObjectArray<mirror::String>> array,
                                VMDebugRuntimeStatId id,
                                const std::string& value) REQUIRES_SHARED(Locks::mutator_lock_) {
  ObjPtr<mirror::String> ovalue = mirror::String::AllocFromModifiedUtf8(self, value.c_str());
  if (ovalue == nullptr) {
    DCHECK(self->IsExceptionPending());
    return false;
  }
  // We're initializing a newly allocated array object, so we do not need to record that under
  // a transaction. If the transaction is aborted, the whole object shall be unreachable.
  array->SetWithoutChecks</*kTransactionActive=*/ false, /*kCheckTransaction=*/ false>(
      static_cast<int32_t>(id), ovalue);
  return true;
}

static jobjectArray VMDebug_getRuntimeStatsInternal(JNIEnv* env, jclass) {
  Thread* self = Thread::ForEnv(env);
  ScopedObjectAccess soa(self);
  StackHandleScope<1u> hs(self);
  int32_t size = enum_cast<int32_t>(VMDebugRuntimeStatId::kNumRuntimeStats);
  Handle<mirror::ObjectArray<mirror::String>> array = hs.NewHandle(
      mirror::ObjectArray<mirror::String>::Alloc(
          self, GetClassRoot<mirror::ObjectArray<mirror::String>>(), size));
  if (array == nullptr) {
    DCHECK(self->IsExceptionPending());
    return nullptr;
  }
  gc::Heap* heap = Runtime::Current()->GetHeap();
  if (!SetRuntimeStatValue(self,
                           array,
                           VMDebugRuntimeStatId::kArtGcGcCount,
                           std::to_string(heap->GetGcCount()))) {
    return nullptr;
  }
  if (!SetRuntimeStatValue(self,
                           array,
                           VMDebugRuntimeStatId::kArtGcGcTime,
                           std::to_string(NsToMs(heap->GetGcTime())))) {
    return nullptr;
  }
  if (!SetRuntimeStatValue(self,
                           array,
                           VMDebugRuntimeStatId::kArtGcBytesAllocated,
                           std::to_string(heap->GetBytesAllocatedEver()))) {
    return nullptr;
  }
  if (!SetRuntimeStatValue(self,
                           array,
                           VMDebugRuntimeStatId::kArtGcBytesFreed,
                           std::to_string(heap->GetBytesFreedEver()))) {
    return nullptr;
  }
  if (!SetRuntimeStatValue(self,
                           array,
                           VMDebugRuntimeStatId::kArtGcBlockingGcCount,
                           std::to_string(heap->GetBlockingGcCount()))) {
    return nullptr;
  }
  if (!SetRuntimeStatValue(self,
                           array,
                           VMDebugRuntimeStatId::kArtGcBlockingGcTime,
                           std::to_string(NsToMs(heap->GetBlockingGcTime())))) {
    return nullptr;
  }
  {
    std::ostringstream output;
    heap->DumpGcCountRateHistogram(output);
    if (!SetRuntimeStatValue(self,
                             array,
                             VMDebugRuntimeStatId::kArtGcGcCountRateHistogram,
                             output.str())) {
      return nullptr;
    }
  }
  {
    std::ostringstream output;
    heap->DumpBlockingGcCountRateHistogram(output);
    if (!SetRuntimeStatValue(self,
                             array,
                             VMDebugRuntimeStatId::kArtGcBlockingGcCountRateHistogram,
                             output.str())) {
      return nullptr;
    }
  }
  return soa.AddLocalReference<jobjectArray>(array.Get());
}

static void VMDebug_nativeAttachAgent(JNIEnv* env, jclass, jstring agent, jobject classloader) {
  if (agent == nullptr) {
    ScopedObjectAccess soa(env);
    ThrowNullPointerException("agent is null");
    return;
  }

  if (!Dbg::IsJdwpAllowed()) {
    ScopedObjectAccess soa(env);
    ThrowSecurityException("Can't attach agent, process is not debuggable.");
    return;
  }

  std::string filename;
  {
    ScopedUtfChars chars(env, agent);
    if (env->ExceptionCheck()) {
      return;
    }
    filename = chars.c_str();
  }

  Runtime::Current()->AttachAgent(env, filename, classloader);
}

static void VMDebug_allowHiddenApiReflectionFrom(JNIEnv* env, jclass, jclass j_caller) {
  Runtime* runtime = Runtime::Current();
  ScopedObjectAccess soa(env);

  if (!runtime->IsJavaDebuggableAtInit()) {
    ThrowSecurityException("Can't exempt class, process is not debuggable.");
    return;
  }

  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::Class> h_caller(hs.NewHandle(soa.Decode<mirror::Class>(j_caller)));
  if (h_caller.IsNull()) {
    ThrowNullPointerException("argument is null");
    return;
  }

  h_caller->SetSkipHiddenApiChecks();
}

static void VMDebug_setAllocTrackerStackDepth(JNIEnv* env, jclass, jint stack_depth) {
  Runtime* runtime = Runtime::Current();
  if (stack_depth < 0 ||
      static_cast<size_t>(stack_depth) > gc::AllocRecordObjectMap::kMaxSupportedStackDepth) {
    ScopedObjectAccess soa(env);
    soa.Self()->ThrowNewExceptionF("Ljava/lang/RuntimeException;",
                                   "Stack depth is invalid: %d",
                                   stack_depth);
  } else {
    runtime->GetHeap()->SetAllocTrackerStackDepth(static_cast<size_t>(stack_depth));
  }
}

static void VMDebug_setCurrentProcessName(JNIEnv* env, jclass, jstring process_name) {
  ScopedObjectAccess soa(env);

  // Android application ID naming convention states:
  // "The name can contain uppercase or lowercase letters, numbers, and underscores ('_')"
  // This is fine to convert to std::string
  const char* c_process_name = env->GetStringUTFChars(process_name, NULL);
  Runtime::Current()->GetRuntimeCallbacks()->SetCurrentProcessName(std::string(c_process_name));
  env->ReleaseStringUTFChars(process_name, c_process_name);
}

static void VMDebug_addApplication(JNIEnv* env, jclass, jstring package_name) {
  ScopedObjectAccess soa(env);

  // Android application ID naming convention states:
  // "The name can contain uppercase or lowercase letters, numbers, and underscores ('_')"
  // This is fine to convert to std::string
  const char* c_package_name = env->GetStringUTFChars(package_name, NULL);
  Runtime::Current()->GetRuntimeCallbacks()->AddApplication(std::string(c_package_name));
  env->ReleaseStringUTFChars(package_name, c_package_name);
}

static void VMDebug_removeApplication(JNIEnv* env, jclass, jstring package_name) {
  ScopedObjectAccess soa(env);

  // Android application ID naming convention states:
  // "The name can contain uppercase or lowercase letters, numbers, and underscores ('_')"
  // This is fine to convert to std::string
  const char* c_package_name = env->GetStringUTFChars(package_name, NULL);
  Runtime::Current()->GetRuntimeCallbacks()->RemoveApplication(std::string(c_package_name));
  env->ReleaseStringUTFChars(package_name, c_package_name);
}

static void VMDebug_setWaitingForDebugger(JNIEnv* env, jclass, jboolean waiting) {
  ScopedObjectAccess soa(env);
  Runtime::Current()->GetRuntimeCallbacks()->SetWaitingForDebugger(waiting);
}

static void VMDebug_setUserId(JNIEnv* env, jclass, jint user_id) {
  ScopedObjectAccess soa(env);
  Runtime::Current()->GetRuntimeCallbacks()->SetUserId(user_id);
}

static JNINativeMethod gMethods[] = {
    NATIVE_METHOD(VMDebug, countInstancesOfClass, "(Ljava/lang/Class;Z)J"),
    NATIVE_METHOD(VMDebug, countInstancesOfClasses, "([Ljava/lang/Class;Z)[J"),
    NATIVE_METHOD(VMDebug, dumpHprofData, "(Ljava/lang/String;I)V"),
    NATIVE_METHOD(VMDebug, dumpHprofDataDdms, "()V"),
    NATIVE_METHOD(VMDebug, dumpReferenceTables, "()V"),
    NATIVE_METHOD(VMDebug, getAllocCount, "(I)I"),
    FAST_NATIVE_METHOD(VMDebug, getLoadedClassCount, "()I"),
    NATIVE_METHOD(VMDebug, getVmFeatureList, "()[Ljava/lang/String;"),
    FAST_NATIVE_METHOD(VMDebug, isDebuggerConnected, "()Z"),
    FAST_NATIVE_METHOD(VMDebug, isDebuggingEnabled, "()Z"),
    NATIVE_METHOD(VMDebug, suspendAllAndSendVmStart, "()V"),
    NATIVE_METHOD(VMDebug, getMethodTracingMode, "()I"),
    FAST_NATIVE_METHOD(VMDebug, lastDebuggerActivity, "()J"),
    FAST_NATIVE_METHOD(VMDebug, printLoadedClasses, "(I)V"),
    NATIVE_METHOD(VMDebug, resetAllocCount, "(I)V"),
    NATIVE_METHOD(VMDebug, startAllocCounting, "()V"),
    NATIVE_METHOD(VMDebug, startMethodTracingDdmsImpl, "(IIZI)V"),
    NATIVE_METHOD(VMDebug, startMethodTracingFd, "(Ljava/lang/String;IIIZIZ)V"),
    NATIVE_METHOD(VMDebug, startMethodTracingFilename, "(Ljava/lang/String;IIZI)V"),
    NATIVE_METHOD(VMDebug, stopAllocCounting, "()V"),
    NATIVE_METHOD(VMDebug, stopMethodTracing, "()V"),
    FAST_NATIVE_METHOD(VMDebug, threadCpuTimeNanos, "()J"),
    NATIVE_METHOD(VMDebug, getRuntimeStatInternal, "(I)Ljava/lang/String;"),
    NATIVE_METHOD(VMDebug, getRuntimeStatsInternal, "()[Ljava/lang/String;"),
    NATIVE_METHOD(VMDebug, nativeAttachAgent, "(Ljava/lang/String;Ljava/lang/ClassLoader;)V"),
    NATIVE_METHOD(VMDebug, allowHiddenApiReflectionFrom, "(Ljava/lang/Class;)V"),
    NATIVE_METHOD(VMDebug, setAllocTrackerStackDepth, "(I)V"),
    NATIVE_METHOD(VMDebug, setCurrentProcessName, "(Ljava/lang/String;)V"),
    NATIVE_METHOD(VMDebug, setWaitingForDebugger, "(Z)V"),
    NATIVE_METHOD(VMDebug, addApplication, "(Ljava/lang/String;)V"),
    NATIVE_METHOD(VMDebug, removeApplication, "(Ljava/lang/String;)V"),
    NATIVE_METHOD(VMDebug, setUserId, "(I)V"),
    NATIVE_METHOD(VMDebug, startLowOverheadTraceForAllMethodsImpl, "()V"),
    NATIVE_METHOD(VMDebug, startLowOverheadTraceForLongRunningMethodsImpl, "(J)V"),
    NATIVE_METHOD(VMDebug, stopLowOverheadTraceImpl, "()V"),
    NATIVE_METHOD(VMDebug, dumpLowOverheadTraceImpl, "(Ljava/lang/String;)V"),
    NATIVE_METHOD(VMDebug, dumpLowOverheadTraceFdImpl, "(I)V"),
    NATIVE_METHOD(
        VMDebug,
        getExecutableMethodFileOffsetsNative,
        "(Ljava/lang/reflect/Executable;)Ldalvik/system/VMDebug$ExecutableMethodFileOffsets;"),
};

void register_dalvik_system_VMDebug(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("dalvik/system/VMDebug");
}

}  // namespace art
