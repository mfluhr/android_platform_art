/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "compiler_driver.h"

#include <unistd.h>

#ifndef __APPLE__
#include <malloc.h>  // For mallinfo
#endif

#include <string_view>
#include <vector>

#include <android-base/logging.h>
#include <android-base/strings.h>

#include "aot_class_linker.h"
#include "art_field-inl.h"
#include "art_method-inl.h"
#include "base/arena_allocator.h"
#include "base/array_ref.h"
#include "base/bit_vector.h"
#include "base/hash_set.h"
#include "base/logging.h"  // For VLOG
#include "base/pointer_size.h"
#include "base/stl_util.h"
#include "base/systrace.h"
#include "base/time_utils.h"
#include "base/timing_logger.h"
#include "class_linker-inl.h"
#include "class_root-inl.h"
#include "common_throws.h"
#include "compiled_method-inl.h"
#include "compiler.h"
#include "compiler_callbacks.h"
#include "compiler_driver-inl.h"
#include "dex/class_accessor-inl.h"
#include "dex/descriptors_names.h"
#include "dex/dex_file-inl.h"
#include "dex/dex_file_annotations.h"
#include "dex/dex_file_exception_helpers.h"
#include "dex/dex_instruction-inl.h"
#include "dex/verification_results.h"
#include "driver/compiler_options.h"
#include "driver/dex_compilation_unit.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/accounting/heap_bitmap.h"
#include "gc/space/image_space.h"
#include "gc/space/space.h"
#include "handle_scope-inl.h"
#include "intrinsics_enum.h"
#include "intrinsics_list.h"
#include "jni/jni_internal.h"
#include "linker/linker_patch.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mirror/dex_cache-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object-refvisitor-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/throwable.h"
#include "object_lock.h"
#include "profile/profile_compilation_info.h"
#include "runtime.h"
#include "runtime_intrinsics.h"
#include "scoped_thread_state_change-inl.h"
#include "thread.h"
#include "thread_list.h"
#include "thread_pool.h"
#include "trampolines/trampoline_compiler.h"
#include "utils/atomic_dex_ref_map-inl.h"
#include "utils/swap_space.h"
#include "vdex_file.h"
#include "verifier/class_verifier.h"
#include "verifier/verifier_deps.h"
#include "verifier/verifier_enums.h"
#include "well_known_classes-inl.h"

namespace art {

static constexpr bool kTimeCompileMethod = !kIsDebugBuild;

// Print additional info during profile guided compilation.
static constexpr bool kDebugProfileGuidedCompilation = false;

// Max encoded fields allowed for initializing app image. Hardcode the number for now
// because 5000 should be large enough.
static constexpr uint32_t kMaxEncodedFields = 5000;

static double Percentage(size_t x, size_t y) {
  return 100.0 * (static_cast<double>(x)) / (static_cast<double>(x + y));
}

static void DumpStat(size_t x, size_t y, const char* str) {
  if (x == 0 && y == 0) {
    return;
  }
  LOG(INFO) << Percentage(x, y) << "% of " << str << " for " << (x + y) << " cases";
}

class CompilerDriver::AOTCompilationStats {
 public:
  AOTCompilationStats()
      : stats_lock_("AOT compilation statistics lock") {}

  void Dump() {
    DumpStat(resolved_instance_fields_, unresolved_instance_fields_, "instance fields resolved");
    DumpStat(resolved_local_static_fields_ + resolved_static_fields_, unresolved_static_fields_,
             "static fields resolved");
    DumpStat(resolved_local_static_fields_, resolved_static_fields_ + unresolved_static_fields_,
             "static fields local to a class");
    DumpStat(safe_casts_, not_safe_casts_, "check-casts removed based on type information");
    // Note, the code below subtracts the stat value so that when added to the stat value we have
    // 100% of samples. TODO: clean this up.
    DumpStat(type_based_devirtualization_,
             resolved_methods_[kVirtual] + unresolved_methods_[kVirtual] +
             resolved_methods_[kInterface] + unresolved_methods_[kInterface] -
             type_based_devirtualization_,
             "virtual/interface calls made direct based on type information");

    const size_t total = std::accumulate(
        class_status_count_,
        class_status_count_ + static_cast<size_t>(ClassStatus::kLast) + 1,
        0u);
    for (size_t i = 0; i <= static_cast<size_t>(ClassStatus::kLast); ++i) {
      std::ostringstream oss;
      oss << "classes with status " << static_cast<ClassStatus>(i);
      DumpStat(class_status_count_[i], total - class_status_count_[i], oss.str().c_str());
    }

    for (size_t i = 0; i <= kMaxInvokeType; i++) {
      std::ostringstream oss;
      oss << static_cast<InvokeType>(i) << " methods were AOT resolved";
      DumpStat(resolved_methods_[i], unresolved_methods_[i], oss.str().c_str());
      if (virtual_made_direct_[i] > 0) {
        std::ostringstream oss2;
        oss2 << static_cast<InvokeType>(i) << " methods made direct";
        DumpStat(virtual_made_direct_[i],
                 resolved_methods_[i] + unresolved_methods_[i] - virtual_made_direct_[i],
                 oss2.str().c_str());
      }
      if (direct_calls_to_boot_[i] > 0) {
        std::ostringstream oss2;
        oss2 << static_cast<InvokeType>(i) << " method calls are direct into boot";
        DumpStat(direct_calls_to_boot_[i],
                 resolved_methods_[i] + unresolved_methods_[i] - direct_calls_to_boot_[i],
                 oss2.str().c_str());
      }
      if (direct_methods_to_boot_[i] > 0) {
        std::ostringstream oss2;
        oss2 << static_cast<InvokeType>(i) << " method calls have methods in boot";
        DumpStat(direct_methods_to_boot_[i],
                 resolved_methods_[i] + unresolved_methods_[i] - direct_methods_to_boot_[i],
                 oss2.str().c_str());
      }
    }
  }

// Allow lossy statistics in non-debug builds.
#ifndef NDEBUG
#define STATS_LOCK() MutexLock mu(Thread::Current(), stats_lock_)
#else
#define STATS_LOCK()
#endif

  void ResolvedInstanceField() REQUIRES(!stats_lock_) {
    STATS_LOCK();
    resolved_instance_fields_++;
  }

  void UnresolvedInstanceField() REQUIRES(!stats_lock_) {
    STATS_LOCK();
    unresolved_instance_fields_++;
  }

  void ResolvedLocalStaticField() REQUIRES(!stats_lock_) {
    STATS_LOCK();
    resolved_local_static_fields_++;
  }

  void ResolvedStaticField() REQUIRES(!stats_lock_) {
    STATS_LOCK();
    resolved_static_fields_++;
  }

  void UnresolvedStaticField() REQUIRES(!stats_lock_) {
    STATS_LOCK();
    unresolved_static_fields_++;
  }

  // Indicate that type information from the verifier led to devirtualization.
  void PreciseTypeDevirtualization() REQUIRES(!stats_lock_) {
    STATS_LOCK();
    type_based_devirtualization_++;
  }

  // A check-cast could be eliminated due to verifier type analysis.
  void SafeCast() REQUIRES(!stats_lock_) {
    STATS_LOCK();
    safe_casts_++;
  }

  // A check-cast couldn't be eliminated due to verifier type analysis.
  void NotASafeCast() REQUIRES(!stats_lock_) {
    STATS_LOCK();
    not_safe_casts_++;
  }

  // Register a class status.
  void AddClassStatus(ClassStatus status) REQUIRES(!stats_lock_) {
    STATS_LOCK();
    ++class_status_count_[static_cast<size_t>(status)];
  }

 private:
  Mutex stats_lock_;

  size_t resolved_instance_fields_ = 0u;
  size_t unresolved_instance_fields_ = 0u;

  size_t resolved_local_static_fields_ = 0u;
  size_t resolved_static_fields_ = 0u;
  size_t unresolved_static_fields_ = 0u;
  // Type based devirtualization for invoke interface and virtual.
  size_t type_based_devirtualization_ = 0u;

  size_t resolved_methods_[kMaxInvokeType + 1] = {};
  size_t unresolved_methods_[kMaxInvokeType + 1] = {};
  size_t virtual_made_direct_[kMaxInvokeType + 1] = {};
  size_t direct_calls_to_boot_[kMaxInvokeType + 1] = {};
  size_t direct_methods_to_boot_[kMaxInvokeType + 1] = {};

  size_t safe_casts_ = 0u;
  size_t not_safe_casts_ = 0u;

  size_t class_status_count_[static_cast<size_t>(ClassStatus::kLast) + 1] = {};

  DISALLOW_COPY_AND_ASSIGN(AOTCompilationStats);
};

CompilerDriver::CompilerDriver(
    const CompilerOptions* compiler_options,
    const VerificationResults* verification_results,
    size_t thread_count,
    int swap_fd)
    : compiler_options_(compiler_options),
      verification_results_(verification_results),
      compiler_(),
      number_of_soft_verifier_failures_(0),
      had_hard_verifier_failure_(false),
      parallel_thread_count_(thread_count),
      stats_(new AOTCompilationStats),
      compiled_method_storage_(swap_fd),
      max_arena_alloc_(0) {
  DCHECK(compiler_options_ != nullptr);

  compiled_method_storage_.SetDedupeEnabled(compiler_options_->DeduplicateCode());
  compiler_.reset(Compiler::Create(*compiler_options, &compiled_method_storage_));
}

CompilerDriver::~CompilerDriver() {
  compiled_methods_.Visit(
      [this]([[maybe_unused]] const DexFileReference& ref, CompiledMethod* method) {
        if (method != nullptr) {
          CompiledMethod::ReleaseSwapAllocatedCompiledMethod(GetCompiledMethodStorage(), method);
        }
      });
}


#define CREATE_TRAMPOLINE(type, abi, offset)                                            \
    if (Is64BitInstructionSet(GetCompilerOptions().GetInstructionSet())) {              \
      return CreateTrampoline64(GetCompilerOptions().GetInstructionSet(),               \
                                abi,                                                    \
                                type ## _ENTRYPOINT_OFFSET(PointerSize::k64, offset));  \
    } else {                                                                            \
      return CreateTrampoline32(GetCompilerOptions().GetInstructionSet(),               \
                                abi,                                                    \
                                type ## _ENTRYPOINT_OFFSET(PointerSize::k32, offset));  \
    }

std::unique_ptr<const std::vector<uint8_t>> CompilerDriver::CreateJniDlsymLookupTrampoline() const {
  CREATE_TRAMPOLINE(JNI, kJniAbi, pDlsymLookup)
}

std::unique_ptr<const std::vector<uint8_t>>
CompilerDriver::CreateJniDlsymLookupCriticalTrampoline() const {
  // @CriticalNative calls do not have the `JNIEnv*` parameter, so this trampoline uses the
  // architecture-dependent access to `Thread*` using the managed code ABI, i.e. `kQuickAbi`.
  CREATE_TRAMPOLINE(JNI, kQuickAbi, pDlsymLookupCritical)
}

std::unique_ptr<const std::vector<uint8_t>> CompilerDriver::CreateQuickGenericJniTrampoline()
    const {
  CREATE_TRAMPOLINE(QUICK, kQuickAbi, pQuickGenericJniTrampoline)
}

std::unique_ptr<const std::vector<uint8_t>> CompilerDriver::CreateQuickImtConflictTrampoline()
    const {
  CREATE_TRAMPOLINE(QUICK, kQuickAbi, pQuickImtConflictTrampoline)
}

std::unique_ptr<const std::vector<uint8_t>> CompilerDriver::CreateQuickResolutionTrampoline()
    const {
  CREATE_TRAMPOLINE(QUICK, kQuickAbi, pQuickResolutionTrampoline)
}

std::unique_ptr<const std::vector<uint8_t>> CompilerDriver::CreateQuickToInterpreterBridge()
    const {
  CREATE_TRAMPOLINE(QUICK, kQuickAbi, pQuickToInterpreterBridge)
}

std::unique_ptr<const std::vector<uint8_t>> CompilerDriver::CreateNterpTrampoline()
    const {
  // We use QuickToInterpreterBridge to not waste one word in the Thread object.
  // The Nterp trampoline gets replaced with the nterp entrypoint when loading
  // an image.
  CREATE_TRAMPOLINE(QUICK, kQuickAbi, pQuickToInterpreterBridge)
}
#undef CREATE_TRAMPOLINE

void CompilerDriver::CompileAll(jobject class_loader,
                                const std::vector<const DexFile*>& dex_files,
                                TimingLogger* timings) {
  DCHECK(!Runtime::Current()->IsStarted());

  CheckThreadPools();

  // Compile:
  // 1) Compile all classes and methods enabled for compilation. May fall back to dex-to-dex
  //    compilation.
  if (GetCompilerOptions().IsAnyCompilationEnabled()) {
    Compile(class_loader, dex_files, timings);
  }
  if (GetCompilerOptions().GetDumpStats()) {
    stats_->Dump();
  }
}

// Does the runtime for the InstructionSet provide an implementation returned by
// GetQuickGenericJniStub allowing down calls that aren't compiled using a JNI compiler?
static bool InstructionSetHasGenericJniStub(InstructionSet isa) {
  switch (isa) {
    case InstructionSet::kArm:
    case InstructionSet::kArm64:
    case InstructionSet::kThumb2:
    case InstructionSet::kX86:
    case InstructionSet::kX86_64: return true;
    default: return false;
  }
}

template <typename CompileFn>
static void CompileMethodHarness(
    Thread* self,
    CompilerDriver* driver,
    const dex::CodeItem* code_item,
    uint32_t access_flags,
    uint16_t class_def_idx,
    uint32_t method_idx,
    Handle<mirror::ClassLoader> class_loader,
    const DexFile& dex_file,
    Handle<mirror::DexCache> dex_cache,
    CompileFn compile_fn) {
  DCHECK(driver != nullptr);
  CompiledMethod* compiled_method;
  uint64_t start_ns = kTimeCompileMethod ? NanoTime() : 0;
  MethodReference method_ref(&dex_file, method_idx);

  compiled_method = compile_fn(self,
                               driver,
                               code_item,
                               access_flags,
                               class_def_idx,
                               method_idx,
                               class_loader,
                               dex_file,
                               dex_cache);

  if (kTimeCompileMethod) {
    uint64_t duration_ns = NanoTime() - start_ns;
    if (duration_ns > MsToNs(driver->GetCompiler()->GetMaximumCompilationTimeBeforeWarning())) {
      LOG(WARNING) << "Compilation of " << dex_file.PrettyMethod(method_idx)
                   << " took " << PrettyDuration(duration_ns);
    }
  }

  if (compiled_method != nullptr) {
    driver->AddCompiledMethod(method_ref, compiled_method);
  }

  if (self->IsExceptionPending()) {
    ScopedObjectAccess soa(self);
    LOG(FATAL) << "Unexpected exception compiling: " << dex_file.PrettyMethod(method_idx) << "\n"
        << self->GetException()->Dump();
  }
}

// Checks whether profile guided compilation is enabled and if the method should be compiled
// according to the profile file.
static bool ShouldCompileBasedOnProfile(const CompilerOptions& compiler_options,
                                        ProfileCompilationInfo::ProfileIndexType profile_index,
                                        MethodReference method_ref) {
  if (profile_index == ProfileCompilationInfo::MaxProfileIndex()) {
    // No profile for this dex file. Check if we're actually compiling based on a profile.
    if (!CompilerFilter::DependsOnProfile(compiler_options.GetCompilerFilter())) {
      return true;
    }
    // Profile-based compilation without profile for this dex file. Do not compile the method.
    DCHECK(compiler_options.GetProfileCompilationInfo() == nullptr ||
           compiler_options.GetProfileCompilationInfo()->FindDexFile(*method_ref.dex_file) ==
               ProfileCompilationInfo::MaxProfileIndex());
    return false;
  } else {
    DCHECK(CompilerFilter::DependsOnProfile(compiler_options.GetCompilerFilter()));
    const ProfileCompilationInfo* profile_compilation_info =
        compiler_options.GetProfileCompilationInfo();
    DCHECK(profile_compilation_info != nullptr);

    bool result = profile_compilation_info->IsHotMethod(profile_index, method_ref.index);

    // On non-low RAM devices, compile startup methods to potentially speed up
    // startup.
    if (!result && !Runtime::Current()->GetHeap()->IsLowMemoryMode()) {
      result = profile_compilation_info->IsStartupMethod(profile_index, method_ref.index);
    }

    if (kDebugProfileGuidedCompilation) {
      LOG(INFO) << "[ProfileGuidedCompilation] "
          << (result ? "Compiled" : "Skipped") << " method:" << method_ref.PrettyMethod(true);
    }


    return result;
  }
}

static void CompileMethodQuick(
    Thread* self,
    CompilerDriver* driver,
    const dex::CodeItem* code_item,
    uint32_t access_flags,
    uint16_t class_def_idx,
    uint32_t method_idx,
    Handle<mirror::ClassLoader> class_loader,
    const DexFile& dex_file,
    Handle<mirror::DexCache> dex_cache,
    ProfileCompilationInfo::ProfileIndexType profile_index) {
  auto quick_fn = [profile_index]([[maybe_unused]] Thread* self,
                                  CompilerDriver* driver,
                                  const dex::CodeItem* code_item,
                                  uint32_t access_flags,
                                  uint16_t class_def_idx,
                                  uint32_t method_idx,
                                  Handle<mirror::ClassLoader> class_loader,
                                  const DexFile& dex_file,
                                  Handle<mirror::DexCache> dex_cache) {
    DCHECK(driver != nullptr);
    const VerificationResults* results = driver->GetVerificationResults();
    DCHECK(results != nullptr);
    MethodReference method_ref(&dex_file, method_idx);
    CompiledMethod* compiled_method = nullptr;
    if (results->IsUncompilableMethod(method_ref)) {
      return compiled_method;
    }

    if ((access_flags & kAccNative) != 0) {
      // Are we extracting only and have support for generic JNI down calls?
      const CompilerOptions& compiler_options = driver->GetCompilerOptions();
      if (!compiler_options.IsJniCompilationEnabled() &&
          InstructionSetHasGenericJniStub(compiler_options.GetInstructionSet())) {
        // Leaving this empty will trigger the generic JNI version
      } else {
        // Query any JNI optimization annotations such as @FastNative or @CriticalNative.
        access_flags |= annotations::GetNativeMethodAnnotationAccessFlags(
            dex_file, dex_file.GetClassDef(class_def_idx), method_idx);
        const void* boot_jni_stub = nullptr;
        if (!Runtime::Current()->GetHeap()->GetBootImageSpaces().empty()) {
          // Skip the compilation for native method if found an usable boot JNI stub.
          ClassLinker* const class_linker = Runtime::Current()->GetClassLinker();
          std::string_view shorty = dex_file.GetMethodShortyView(dex_file.GetMethodId(method_idx));
          boot_jni_stub = class_linker->FindBootJniStub(access_flags, shorty);
        }
        if (boot_jni_stub == nullptr) {
          compiled_method =
              driver->GetCompiler()->JniCompile(access_flags, method_idx, dex_file, dex_cache);
          CHECK(compiled_method != nullptr);
        }
      }
    } else if ((access_flags & kAccAbstract) != 0) {
      // Abstract methods don't have code.
    } else if (annotations::MethodIsNeverCompile(dex_file,
                                                 dex_file.GetClassDef(class_def_idx),
                                                 method_idx)) {
      // Method is annotated with @NeverCompile and should not be compiled.
    } else {
      const CompilerOptions& compiler_options = driver->GetCompilerOptions();
      // Don't compile class initializers unless kEverything.
      bool compile = (compiler_options.GetCompilerFilter() == CompilerFilter::kEverything) ||
         ((access_flags & kAccConstructor) == 0) || ((access_flags & kAccStatic) == 0);
      // Check if we should compile based on the profile.
      compile = compile && ShouldCompileBasedOnProfile(compiler_options, profile_index, method_ref);

      if (compile) {
        // NOTE: if compiler declines to compile this method, it will return null.
        compiled_method = driver->GetCompiler()->Compile(code_item,
                                                         access_flags,
                                                         class_def_idx,
                                                         method_idx,
                                                         class_loader,
                                                         dex_file,
                                                         dex_cache);
        ProfileMethodsCheck check_type = compiler_options.CheckProfiledMethodsCompiled();
        if (UNLIKELY(check_type != ProfileMethodsCheck::kNone)) {
          DCHECK(ShouldCompileBasedOnProfile(compiler_options, profile_index, method_ref));
          bool violation = (compiled_method == nullptr);
          if (violation) {
            std::ostringstream oss;
            oss << "Failed to compile "
                << method_ref.dex_file->PrettyMethod(method_ref.index)
                << "[" << method_ref.dex_file->GetLocation() << "]"
                << " as expected by profile";
            switch (check_type) {
              case ProfileMethodsCheck::kNone:
                break;
              case ProfileMethodsCheck::kLog:
                LOG(ERROR) << oss.str();
                break;
              case ProfileMethodsCheck::kAbort:
                LOG(FATAL_WITHOUT_ABORT) << oss.str();
                _exit(1);
            }
          }
        }
      }
    }
    return compiled_method;
  };
  CompileMethodHarness(self,
                       driver,
                       code_item,
                       access_flags,
                       class_def_idx,
                       method_idx,
                       class_loader,
                       dex_file,
                       dex_cache,
                       quick_fn);
}

void CompilerDriver::Resolve(jobject class_loader,
                             const std::vector<const DexFile*>& dex_files,
                             TimingLogger* timings) {
  TimingLogger::ScopedTiming t("Resolve Types", timings);
  // Resolution allocates classes and needs to run single-threaded to be deterministic.
  bool force_determinism = GetCompilerOptions().IsForceDeterminism();
  ThreadPool* resolve_thread_pool = force_determinism
                                     ? single_thread_pool_.get()
                                     : parallel_thread_pool_.get();
  size_t resolve_thread_count = force_determinism ? 1U : parallel_thread_count_;

  for (size_t i = 0; i != dex_files.size(); ++i) {
    const DexFile* dex_file = dex_files[i];
    CHECK(dex_file != nullptr);
    ResolveDexFile(class_loader,
                   *dex_file,
                   resolve_thread_pool,
                   resolve_thread_count,
                   timings);
  }
}

void CompilerDriver::ResolveConstStrings(const std::vector<const DexFile*>& dex_files,
                                         bool only_startup_strings,
                                         TimingLogger* timings) {
  TimingLogger::ScopedTiming t("Resolve const-string Strings", timings);
  const ProfileCompilationInfo* profile_compilation_info =
      GetCompilerOptions().GetProfileCompilationInfo();
  if (only_startup_strings && profile_compilation_info == nullptr) {
    // If there is no profile, don't resolve any strings. Resolving all of the strings in the image
    // will cause a bloated app image and slow down startup.
    return;
  }
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<1> hs(soa.Self());
  ClassLinker* const class_linker = Runtime::Current()->GetClassLinker();
  MutableHandle<mirror::DexCache> dex_cache(hs.NewHandle<mirror::DexCache>(nullptr));
  size_t num_instructions = 0u;

  for (const DexFile* dex_file : dex_files) {
    dex_cache.Assign(class_linker->FindDexCache(soa.Self(), *dex_file));

    ProfileCompilationInfo::ProfileIndexType profile_index =
        ProfileCompilationInfo::MaxProfileIndex();
    if (profile_compilation_info != nullptr) {
      profile_index = profile_compilation_info->FindDexFile(*dex_file);
      if (profile_index == ProfileCompilationInfo::MaxProfileIndex()) {
        // We have a `ProfileCompilationInfo` but no data for this dex file.
        // The code below would not find any method to process.
        continue;
      }
    }

    // TODO: Implement a profile-based filter for the boot image. See b/76145463.
    for (ClassAccessor accessor : dex_file->GetClasses()) {
      // Skip methods that failed to verify since they may contain invalid Dex code.
      if (GetClassStatus(ClassReference(dex_file, accessor.GetClassDefIndex())) <
          ClassStatus::kRetryVerificationAtRuntime) {
        continue;
      }

      for (const ClassAccessor::Method& method : accessor.GetMethods()) {
        if (profile_compilation_info != nullptr) {
          DCHECK_NE(profile_index, ProfileCompilationInfo::MaxProfileIndex());
          // There can be at most one class initializer in a class, so we shall not
          // call `ProfileCompilationInfo::ContainsClass()` more than once per class.
          constexpr uint32_t kMask = kAccConstructor | kAccStatic;
          const bool is_startup_clinit =
              (method.GetAccessFlags() & kMask) == kMask &&
              profile_compilation_info->ContainsClass(profile_index, accessor.GetClassIdx());

          if (!is_startup_clinit) {
            uint32_t method_index = method.GetIndex();
            bool process_method = only_startup_strings
                ? profile_compilation_info->IsStartupMethod(profile_index, method_index)
                : profile_compilation_info->IsMethodInProfile(profile_index, method_index);
            if (!process_method) {
              continue;
            }
          }
        }

        // Resolve const-strings in the code. Done to have deterministic allocation behavior. Right
        // now this is single-threaded for simplicity.
        // TODO: Collect the relevant string indices in parallel, then allocate them sequentially
        // in a stable order.
        for (const DexInstructionPcPair& inst : method.GetInstructions()) {
          switch (inst->Opcode()) {
            case Instruction::CONST_STRING:
            case Instruction::CONST_STRING_JUMBO: {
              dex::StringIndex string_index((inst->Opcode() == Instruction::CONST_STRING)
                  ? inst->VRegB_21c()
                  : inst->VRegB_31c());
              ObjPtr<mirror::String> string = class_linker->ResolveString(string_index, dex_cache);
              CHECK(string != nullptr) << "Could not allocate a string when forcing determinism";
              ++num_instructions;
              break;
            }

            default:
              break;
          }
        }
      }
    }
  }
  VLOG(compiler) << "Resolved " << num_instructions << " const string instructions";
}

// Initialize type check bit strings for check-cast and instance-of in the code. Done to have
// deterministic allocation behavior. Right now this is single-threaded for simplicity.
// TODO: Collect the relevant type indices in parallel, then process them sequentially in a
//       stable order.

static void InitializeTypeCheckBitstrings(CompilerDriver* driver,
                                          ClassLinker* class_linker,
                                          Handle<mirror::DexCache> dex_cache,
                                          const DexFile& dex_file,
                                          const ClassAccessor::Method& method)
      REQUIRES_SHARED(Locks::mutator_lock_) {
  for (const DexInstructionPcPair& inst : method.GetInstructions()) {
    switch (inst->Opcode()) {
      case Instruction::CHECK_CAST:
      case Instruction::INSTANCE_OF: {
        dex::TypeIndex type_index(
            (inst->Opcode() == Instruction::CHECK_CAST) ? inst->VRegB_21c() : inst->VRegC_22c());
        const char* descriptor = dex_file.GetTypeDescriptor(type_index);
        // We currently do not use the bitstring type check for array or final (including
        // primitive) classes. We may reconsider this in future if it's deemed to be beneficial.
        // And we cannot use it for classes outside the boot image as we do not know the runtime
        // value of their bitstring when compiling (it may not even get assigned at runtime).
        if (descriptor[0] == 'L' && driver->GetCompilerOptions().IsImageClass(descriptor)) {
          ObjPtr<mirror::Class> klass =
              class_linker->LookupResolvedType(type_index,
                                               dex_cache.Get(),
                                               /* class_loader= */ nullptr);
          CHECK(klass != nullptr) << descriptor << " should have been previously resolved.";
          // Now assign the bitstring if the class is not final. Keep this in sync with sharpening.
          if (!klass->IsFinal()) {
            MutexLock subtype_check_lock(Thread::Current(), *Locks::subtype_check_lock_);
            SubtypeCheck<ObjPtr<mirror::Class>>::EnsureAssigned(klass);
          }
        }
        break;
      }

      default:
        break;
    }
  }
}

static void InitializeTypeCheckBitstrings(CompilerDriver* driver,
                                          const std::vector<const DexFile*>& dex_files,
                                          TimingLogger* timings) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<1> hs(soa.Self());
  ClassLinker* const class_linker = Runtime::Current()->GetClassLinker();
  MutableHandle<mirror::DexCache> dex_cache(hs.NewHandle<mirror::DexCache>(nullptr));

  for (const DexFile* dex_file : dex_files) {
    dex_cache.Assign(class_linker->FindDexCache(soa.Self(), *dex_file));
    TimingLogger::ScopedTiming t("Initialize type check bitstrings", timings);

    for (ClassAccessor accessor : dex_file->GetClasses()) {
      // Direct and virtual methods.
      for (const ClassAccessor::Method& method : accessor.GetMethods()) {
        InitializeTypeCheckBitstrings(driver, class_linker, dex_cache, *dex_file, method);
      }
    }
  }
}

inline void CompilerDriver::CheckThreadPools() {
  DCHECK(parallel_thread_pool_ != nullptr);
  DCHECK(single_thread_pool_ != nullptr);
}

static void EnsureVerifiedOrVerifyAtRuntime(jobject jclass_loader,
                                            const std::vector<const DexFile*>& dex_files) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader>(jclass_loader)));
  MutableHandle<mirror::Class> cls(hs.NewHandle<mirror::Class>(nullptr));
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();

  for (const DexFile* dex_file : dex_files) {
    for (ClassAccessor accessor : dex_file->GetClasses()) {
      cls.Assign(
          class_linker->FindClass(soa.Self(), *dex_file, accessor.GetClassIdx(), class_loader));
      if (cls == nullptr) {
        soa.Self()->ClearException();
      } else if (&cls->GetDexFile() == dex_file) {
        DCHECK(cls->IsErroneous() ||
               cls->IsVerified() ||
               cls->ShouldVerifyAtRuntime() ||
               cls->IsVerifiedNeedsAccessChecks())
            << cls->PrettyClass()
            << " " << cls->GetStatus();
      }
    }
  }
}

void CompilerDriver::PrepareDexFilesForOatFile([[maybe_unused]] TimingLogger* timings) {
  compiled_classes_.AddDexFiles(GetCompilerOptions().GetDexFilesForOatFile());
}

class CreateConflictTablesVisitor : public ClassVisitor {
 public:
  explicit CreateConflictTablesVisitor(VariableSizedHandleScope& hs)
      : hs_(hs) {}

  bool operator()(ObjPtr<mirror::Class> klass) override
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (Runtime::Current()->GetHeap()->ObjectIsInBootImageSpace(klass)) {
      return true;
    }
    // Collect handles since there may be thread suspension in future EnsureInitialized.
    to_visit_.push_back(hs_.NewHandle(klass));
    return true;
  }

  void FillAllIMTAndConflictTables() REQUIRES_SHARED(Locks::mutator_lock_) {
    ScopedAssertNoThreadSuspension ants(__FUNCTION__);
    for (Handle<mirror::Class> c : to_visit_) {
      // Create the conflict tables.
      FillIMTAndConflictTables(c.Get());
    }
  }

 private:
  void FillIMTAndConflictTables(ObjPtr<mirror::Class> klass)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (!klass->ShouldHaveImt()) {
      return;
    }
    if (visited_classes_.find(klass.Ptr()) != visited_classes_.end()) {
      return;
    }
    if (klass->HasSuperClass()) {
      FillIMTAndConflictTables(klass->GetSuperClass());
    }
    if (!klass->IsTemp()) {
      Runtime::Current()->GetClassLinker()->FillIMTAndConflictTables(klass);
    }
    visited_classes_.insert(klass.Ptr());
  }

  VariableSizedHandleScope& hs_;
  std::vector<Handle<mirror::Class>> to_visit_;
  HashSet<mirror::Class*> visited_classes_;
};

void CompilerDriver::PreCompile(jobject class_loader,
                                const std::vector<const DexFile*>& dex_files,
                                TimingLogger* timings,
                                /*inout*/ HashSet<std::string>* image_classes) {
  CheckThreadPools();

  VLOG(compiler) << "Before precompile " << GetMemoryUsageString(false);

  // Precompile:
  // 1) Load image classes.
  // 2) Resolve all classes.
  // 3) For deterministic boot image, resolve strings for const-string instructions.
  // 4) Attempt to verify all classes.
  // 5) Attempt to initialize image classes, and trivially initialized classes.
  // 6) Update the set of image classes.
  // 7) For deterministic boot image, initialize bitstrings for type checking.

  LoadImageClasses(timings, class_loader, image_classes);
  VLOG(compiler) << "LoadImageClasses: " << GetMemoryUsageString(false);

  if (compiler_options_->AssumeClassesAreVerified()) {
    VLOG(compiler) << "Verify none mode specified, skipping verification.";
    SetVerified(class_loader, dex_files, timings);
  } else {
    DCHECK(compiler_options_->IsVerificationEnabled());

    if (compiler_options_->IsAnyCompilationEnabled()) {
      // Avoid adding the dex files in the case where we aren't going to add compiled methods.
      // This reduces RAM usage for this case.
      for (const DexFile* dex_file : dex_files) {
        // Can be already inserted. This happens for gtests.
        if (!compiled_methods_.HaveDexFile(dex_file)) {
          compiled_methods_.AddDexFile(dex_file);
        }
      }
    }

    // Resolve eagerly for compilations always, and for verifications only if we are running with
    // multiple threads.
    const bool should_resolve_eagerly =
        compiler_options_->IsAnyCompilationEnabled() ||
        (!GetCompilerOptions().IsForceDeterminism() && parallel_thread_count_ > 1);
    if (should_resolve_eagerly) {
      Resolve(class_loader, dex_files, timings);
      VLOG(compiler) << "Resolve: " << GetMemoryUsageString(false);
    }

    Verify(class_loader, dex_files, timings);
    VLOG(compiler) << "Verify: " << GetMemoryUsageString(false);

    if (GetCompilerOptions().IsForceDeterminism() &&
        (GetCompilerOptions().IsBootImage() || GetCompilerOptions().IsBootImageExtension())) {
      // Resolve strings from const-string. Do this now to have a deterministic image.
      ResolveConstStrings(dex_files, /*only_startup_strings=*/ false, timings);
      VLOG(compiler) << "Resolve const-strings: " << GetMemoryUsageString(false);
    } else if (GetCompilerOptions().ResolveStartupConstStrings()) {
      ResolveConstStrings(dex_files, /*only_startup_strings=*/ true, timings);
    }

    if (had_hard_verifier_failure_ && GetCompilerOptions().AbortOnHardVerifierFailure()) {
      // Avoid dumping threads. Even if we shut down the thread pools, there will still be three
      // instances of this thread's stack.
      LOG(FATAL_WITHOUT_ABORT) << "Had a hard failure verifying all classes, and was asked to abort "
                               << "in such situations. Please check the log.";
      _exit(1);
    } else if (number_of_soft_verifier_failures_ > 0 &&
               GetCompilerOptions().AbortOnSoftVerifierFailure()) {
      LOG(FATAL_WITHOUT_ABORT) << "Had " << number_of_soft_verifier_failures_ << " soft failure(s) "
                               << "verifying all classes, and was asked to abort in such situations. "
                               << "Please check the log.";
      _exit(1);
    }

    if (GetCompilerOptions().IsAppImage() && had_hard_verifier_failure_) {
      // Prune erroneous classes and classes that depend on them.
      UpdateImageClasses(timings, image_classes);
      VLOG(compiler) << "verify/UpdateImageClasses: " << GetMemoryUsageString(false);
    }
  }

  if (GetCompilerOptions().IsGeneratingImage()) {
    // We can only initialize classes when their verification bit is set.
    if (compiler_options_->AssumeClassesAreVerified() ||
        compiler_options_->IsVerificationEnabled()) {
      if (kIsDebugBuild) {
        EnsureVerifiedOrVerifyAtRuntime(class_loader, dex_files);
      }
      InitializeClasses(class_loader, dex_files, timings);
      VLOG(compiler) << "InitializeClasses: " << GetMemoryUsageString(false);
    }
    {
      // Create conflict tables, as the runtime expects boot image classes to
      // always have their conflict tables filled.
      ScopedObjectAccess soa(Thread::Current());
      VariableSizedHandleScope hs(soa.Self());
      CreateConflictTablesVisitor visitor(hs);
      Runtime::Current()->GetClassLinker()->VisitClassesWithoutClassesLock(&visitor);
      visitor.FillAllIMTAndConflictTables();
    }

    if (GetCompilerOptions().IsBootImage() || GetCompilerOptions().IsBootImageExtension()) {
      UpdateImageClasses(timings, image_classes);
      VLOG(compiler) << "UpdateImageClasses: " << GetMemoryUsageString(false);
    }

    if (kBitstringSubtypeCheckEnabled &&
        GetCompilerOptions().IsForceDeterminism() && GetCompilerOptions().IsBootImage()) {
      // Initialize type check bit string used by check-cast and instanceof.
      // Do this now to have a deterministic image.
      // Note: This is done after UpdateImageClasses() at it relies on the image
      // classes to be final.
      InitializeTypeCheckBitstrings(this, dex_files, timings);
    }
  }
}

class ResolveCatchBlockExceptionsClassVisitor : public ClassVisitor {
 public:
  explicit ResolveCatchBlockExceptionsClassVisitor(Thread* self)
      : hs_(self),
        dex_file_records_(),
        unprocessed_classes_(),
        exception_types_to_resolve_(),
        boot_images_start_(Runtime::Current()->GetHeap()->GetBootImagesStartAddress()),
        boot_images_size_(Runtime::Current()->GetHeap()->GetBootImagesSize()) {}

  bool operator()(ObjPtr<mirror::Class> c) override REQUIRES_SHARED(Locks::mutator_lock_) {
    // Filter out classes from boot images we're compiling against.
    // These have been processed when we compiled those boot images.
    if (reinterpret_cast32<uint32_t>(c.Ptr()) - boot_images_start_ < boot_images_size_) {
      DCHECK(Runtime::Current()->GetHeap()->ObjectIsInBootImageSpace(c));
      return true;
    }
    // Filter out classes without methods.
    // These include primitive types and array types which have no dex file.
    if (c->GetMethodsPtr() == nullptr) {
      return true;
    }
    auto it = dex_file_records_.find(&c->GetDexFile());
    if (it != dex_file_records_.end()) {
      DexFileRecord& record = it->second;
      DCHECK_EQ(c->GetDexCache(), record.GetDexCache().Get());
      DCHECK_EQ(c->GetClassLoader(), record.GetClassLoader().Get());
      if (record.IsProcessedClass(c)) {
        return true;
      }
    }
    unprocessed_classes_.push_back(c);
    return true;
  }

  void FindAndResolveExceptionTypes(Thread* self, ClassLinker* class_linker)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    // If we try to resolve any exception types, we need to repeat the process.
    // Even if we failed to resolve an exception type, we could have resolved its supertype
    // or some implemented interfaces as a side-effect (the exception type could implement
    // another unresolved interface) and we need to visit methods of such new resolved
    // classes as they shall be recorded as image classes.
    while (FindExceptionTypesToResolve(class_linker)) {
      ResolveExceptionTypes(self, class_linker);
    }
  }

 private:
  class DexFileRecord {
   public:
    DexFileRecord(Handle<mirror::DexCache> dex_cache, Handle<mirror::ClassLoader> class_loader)
        REQUIRES_SHARED(Locks::mutator_lock_)
        : dex_cache_(dex_cache),
          class_loader_(class_loader),
          processed_classes_(/*start_bits=*/ dex_cache->GetDexFile()->NumClassDefs(),
                             /*expandable=*/ false,
                             Allocator::GetCallocAllocator()),
          processed_exception_types_(/*start_bits=*/ dex_cache->GetDexFile()->NumTypeIds(),
                                     /*expandable=*/ false,
                                     Allocator::GetCallocAllocator()) {}

    Handle<mirror::DexCache> GetDexCache() {
      return dex_cache_;
    }

    Handle<mirror::ClassLoader> GetClassLoader() {
      return class_loader_;
    }

    bool IsProcessedClass(ObjPtr<mirror::Class> c) REQUIRES_SHARED(Locks::mutator_lock_) {
      DCHECK_LT(c->GetDexClassDefIndex(), dex_cache_->GetDexFile()->NumClassDefs());
      return processed_classes_.IsBitSet(c->GetDexClassDefIndex());
    }

    void MarkProcessedClass(ObjPtr<mirror::Class> c) REQUIRES_SHARED(Locks::mutator_lock_) {
      DCHECK_LT(c->GetDexClassDefIndex(), dex_cache_->GetDexFile()->NumClassDefs());
      processed_classes_.SetBit(c->GetDexClassDefIndex());
    }

    bool IsProcessedExceptionType(dex::TypeIndex type_idx) REQUIRES_SHARED(Locks::mutator_lock_) {
      DCHECK_LT(type_idx.index_, dex_cache_->GetDexFile()->NumTypeIds());
      return processed_exception_types_.IsBitSet(type_idx.index_);
    }

    void MarkProcessedExceptionType(dex::TypeIndex type_idx) REQUIRES_SHARED(Locks::mutator_lock_) {
      DCHECK_LT(type_idx.index_, dex_cache_->GetDexFile()->NumTypeIds());
      processed_exception_types_.SetBit(type_idx.index_);
    }

   private:
    Handle<mirror::DexCache> dex_cache_;
    Handle<mirror::ClassLoader> class_loader_;
    BitVector processed_classes_;
    BitVector processed_exception_types_;
  };

  struct ExceptionTypeReference {
    dex::TypeIndex exception_type_idx;
    Handle<mirror::DexCache> dex_cache;
    Handle<mirror::ClassLoader> class_loader;
  };

  bool FindExceptionTypesToResolve(ClassLinker* class_linker)
      REQUIRES_SHARED(Locks::mutator_lock_);

  void ResolveExceptionTypes(Thread* self, ClassLinker* class_linker)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK(!exception_types_to_resolve_.empty());
    for (auto [exception_type_idx, dex_cache, class_loader] : exception_types_to_resolve_) {
      ObjPtr<mirror::Class> exception_class =
          class_linker->ResolveType(exception_type_idx, dex_cache, class_loader);
      if (exception_class == nullptr) {
        VLOG(compiler) << "Failed to resolve exception class "
            << dex_cache->GetDexFile()->GetTypeDescriptorView(exception_type_idx);
        self->ClearException();
      } else {
        DCHECK(GetClassRoot<mirror::Throwable>(class_linker)->IsAssignableFrom(exception_class));
      }
    }
    exception_types_to_resolve_.clear();
  }

  VariableSizedHandleScope hs_;
  SafeMap<const DexFile*, DexFileRecord> dex_file_records_;
  std::vector<ObjPtr<mirror::Class>> unprocessed_classes_;
  std::vector<ExceptionTypeReference> exception_types_to_resolve_;
  const uint32_t boot_images_start_;
  const uint32_t boot_images_size_;
};

bool ResolveCatchBlockExceptionsClassVisitor::FindExceptionTypesToResolve(
    ClassLinker* class_linker) {
  // Thread suspension is not allowed while the `ResolveCatchBlockExceptionsClassVisitor`
  // is using a `std::vector<ObjPtr<mirror::Class>>`.
  ScopedAssertNoThreadSuspension ants(__FUNCTION__);
  DCHECK(unprocessed_classes_.empty());
  class_linker->VisitClasses(this);
  if (unprocessed_classes_.empty()) {
    return false;
  }

  DCHECK(exception_types_to_resolve_.empty());
  const PointerSize pointer_size = class_linker->GetImagePointerSize();
  for (ObjPtr<mirror::Class> klass : unprocessed_classes_) {
    const DexFile* dex_file = &klass->GetDexFile();
    DexFileRecord& record = dex_file_records_.GetOrCreate(
        dex_file,
        // NO_THREAD_SAFETY_ANALYSIS: Called from unannotated `SafeMap<>::GetOrCreate()`.
        [&]() NO_THREAD_SAFETY_ANALYSIS {
          return DexFileRecord(hs_.NewHandle(klass->GetDexCache()),
                               hs_.NewHandle(klass->GetClassLoader()));
        });
    DCHECK_EQ(klass->GetDexCache(), record.GetDexCache().Get());
    DCHECK_EQ(klass->GetClassLoader(), record.GetClassLoader().Get());
    DCHECK(!record.IsProcessedClass(klass));
    record.MarkProcessedClass(klass);
    for (ArtMethod& method : klass->GetDeclaredMethods(pointer_size)) {
      if (method.GetCodeItem() == nullptr) {
        continue;  // native or abstract method
      }
      CodeItemDataAccessor accessor(method.DexInstructionData());
      if (accessor.TriesSize() == 0) {
        continue;  // nothing to process
      }
      const uint8_t* handlers_ptr = accessor.GetCatchHandlerData();
      size_t num_encoded_catch_handlers = DecodeUnsignedLeb128(&handlers_ptr);
      for (size_t i = 0; i < num_encoded_catch_handlers; i++) {
        CatchHandlerIterator iterator(handlers_ptr);
        for (; iterator.HasNext(); iterator.Next()) {
          dex::TypeIndex exception_type_idx = iterator.GetHandlerTypeIndex();
          if (exception_type_idx.IsValid() &&
              !record.IsProcessedExceptionType(exception_type_idx)) {
            record.MarkProcessedExceptionType(exception_type_idx);
            // Add to set of types to resolve if not resolved yet.
            ObjPtr<mirror::Class> type = class_linker->LookupResolvedType(
                exception_type_idx, record.GetDexCache().Get(), record.GetClassLoader().Get());
            if (type == nullptr) {
              exception_types_to_resolve_.push_back(
                  {exception_type_idx, record.GetDexCache(), record.GetClassLoader()});
            }
          }
        }
        handlers_ptr = iterator.EndDataPointer();
      }
    }
  }
  unprocessed_classes_.clear();
  return !exception_types_to_resolve_.empty();
}

static inline bool CanIncludeInCurrentImage(ObjPtr<mirror::Class> klass)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  DCHECK(klass != nullptr);
  gc::Heap* heap = Runtime::Current()->GetHeap();
  if (heap->GetBootImageSpaces().empty()) {
    return true;  // We can include any class when compiling the primary boot image.
  }
  if (heap->ObjectIsInBootImageSpace(klass)) {
    return false;  // Already included in the boot image we're compiling against.
  }
  return AotClassLinker::CanReferenceInBootImageExtensionOrAppImage(klass, heap);
}

class RecordImageClassesVisitor : public ClassVisitor {
 public:
  explicit RecordImageClassesVisitor(HashSet<std::string>* image_classes)
      : image_classes_(image_classes) {}

  bool operator()(ObjPtr<mirror::Class> klass) override REQUIRES_SHARED(Locks::mutator_lock_) {
    bool resolved = klass->IsResolved();
    DCHECK(resolved || klass->IsErroneousUnresolved());
    bool can_include_in_image = LIKELY(resolved) && CanIncludeInCurrentImage(klass);
    std::string temp;
    std::string_view descriptor(klass->GetDescriptor(&temp));
    if (can_include_in_image) {
      image_classes_->insert(std::string(descriptor));  // Does nothing if already present.
    } else {
      auto it = image_classes_->find(descriptor);
      if (it != image_classes_->end()) {
        VLOG(compiler) << "Removing " << (resolved ? "unsuitable" : "unresolved")
            << " class from image classes: " << descriptor;
        image_classes_->erase(it);
      }
    }
    return true;
  }

 private:
  HashSet<std::string>* const image_classes_;
};

// Verify that classes which contain intrinsics methods are in the list of image classes.
static void VerifyClassesContainingIntrinsicsAreImageClasses(HashSet<std::string>* image_classes) {
#define CHECK_INTRINSIC_OWNER_CLASS(_, __, ___, ____, _____, ClassName, ______, _______) \
  CHECK(image_classes->find(std::string_view(ClassName)) != image_classes->end());

  ART_INTRINSICS_LIST(CHECK_INTRINSIC_OWNER_CLASS)
#undef CHECK_INTRINSIC_OWNER_CLASS
}

// We need to put classes required by app class loaders to the boot image,
// otherwise we would not be able to store app class loaders in app images.
static void AddClassLoaderClasses(/* out */ HashSet<std::string>* image_classes) {
  ScopedObjectAccess soa(Thread::Current());
  // Well known classes have been loaded and shall be added to image classes
  // by the `RecordImageClassesVisitor`. However, there are fields with array
  // types which we need to add to the image classes explicitly.
  ArtField* class_loader_array_fields[] = {
      WellKnownClasses::dalvik_system_BaseDexClassLoader_sharedLibraryLoaders,
      // BaseDexClassLoader.sharedLibraryLoadersAfter has the same array type as above.
      WellKnownClasses::dalvik_system_DexPathList_dexElements,
  };
  for (ArtField* field : class_loader_array_fields) {
    const char* field_type_descriptor = field->GetTypeDescriptor();
    DCHECK_EQ(field_type_descriptor[0], '[');
    image_classes->insert(field_type_descriptor);
  }
}

static void VerifyClassLoaderClassesAreImageClasses(/* out */ HashSet<std::string>* image_classes) {
  ScopedObjectAccess soa(Thread::Current());
  ScopedAssertNoThreadSuspension sants(__FUNCTION__);
  ObjPtr<mirror::Class> class_loader_classes[] = {
      WellKnownClasses::dalvik_system_BaseDexClassLoader.Get(),
      WellKnownClasses::dalvik_system_DelegateLastClassLoader.Get(),
      WellKnownClasses::dalvik_system_DexClassLoader.Get(),
      WellKnownClasses::dalvik_system_DexFile.Get(),
      WellKnownClasses::dalvik_system_DexPathList.Get(),
      WellKnownClasses::dalvik_system_DexPathList__Element.Get(),
      WellKnownClasses::dalvik_system_InMemoryDexClassLoader.Get(),
      WellKnownClasses::dalvik_system_PathClassLoader.Get(),
      WellKnownClasses::java_lang_BootClassLoader.Get(),
      WellKnownClasses::java_lang_ClassLoader.Get(),
  };
  for (ObjPtr<mirror::Class> klass : class_loader_classes) {
    std::string temp;
    std::string_view descriptor = klass->GetDescriptor(&temp);
    CHECK(image_classes->find(descriptor) != image_classes->end());
  }
  ArtField* class_loader_fields[] = {
      WellKnownClasses::dalvik_system_BaseDexClassLoader_pathList,
      WellKnownClasses::dalvik_system_BaseDexClassLoader_sharedLibraryLoaders,
      WellKnownClasses::dalvik_system_BaseDexClassLoader_sharedLibraryLoadersAfter,
      WellKnownClasses::dalvik_system_DexFile_cookie,
      WellKnownClasses::dalvik_system_DexFile_fileName,
      WellKnownClasses::dalvik_system_DexPathList_dexElements,
      WellKnownClasses::dalvik_system_DexPathList__Element_dexFile,
      WellKnownClasses::java_lang_ClassLoader_parent,
  };
  for (ArtField* field : class_loader_fields) {
    std::string_view field_type_descriptor = field->GetTypeDescriptor();
    CHECK(image_classes->find(field_type_descriptor) != image_classes->end());
  }
}

// Make a list of descriptors for classes to include in the image
void CompilerDriver::LoadImageClasses(TimingLogger* timings,
                                      jobject class_loader,
                                      /*inout*/ HashSet<std::string>* image_classes) {
  CHECK(timings != nullptr);
  if (!GetCompilerOptions().IsGeneratingImage()) {
    return;
  }

  TimingLogger::ScopedTiming t("LoadImageClasses", timings);

  if (GetCompilerOptions().IsBootImage()) {
    // Image classes of intrinsics are loaded and shall be added
    // to image classes by the `RecordImageClassesVisitor`.
    // Add classes needed for storing class loaders in app images.
    AddClassLoaderClasses(image_classes);
  }

  // Make a first pass to load all classes explicitly listed in the profile.
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);
  StackHandleScope<2u> hs(self);
  Handle<mirror::ClassLoader> loader = hs.NewHandle(soa.Decode<mirror::ClassLoader>(class_loader));
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  CHECK(image_classes != nullptr);
  for (auto it = image_classes->begin(), end = image_classes->end(); it != end;) {
    const std::string& descriptor(*it);
    ObjPtr<mirror::Class> klass =
        class_linker->FindClass(self, descriptor.c_str(), descriptor.length(), loader);
    if (klass == nullptr) {
      VLOG(compiler) << "Failed to find class " << descriptor;
      it = image_classes->erase(it);  // May cause some descriptors to be revisited.
      self->ClearException();
    } else {
      ++it;
    }
  }

  // Resolve exception classes referenced by the loaded classes. The catch logic assumes
  // exceptions are resolved by the verifier when there is a catch block in an interested method.
  // Do this here so that exception classes appear to have been specified image classes.
  ResolveCatchBlockExceptionsClassVisitor resolve_exception_classes_visitor(self);
  resolve_exception_classes_visitor.FindAndResolveExceptionTypes(self, class_linker);

  // We walk the roots looking for classes so that we'll pick up the
  // above classes plus any classes they depend on such super
  // classes, interfaces, and the required ClassLinker roots.
  RecordImageClassesVisitor visitor(image_classes);
  class_linker->VisitClasses(&visitor);

  if (kIsDebugBuild && GetCompilerOptions().IsBootImage()) {
    VerifyClassesContainingIntrinsicsAreImageClasses(image_classes);
    VerifyClassLoaderClassesAreImageClasses(image_classes);
  }

  if (GetCompilerOptions().IsBootImage()) {
    CHECK(!image_classes->empty());
  }
}

static void MaybeAddToImageClasses(Thread* self,
                                   ObjPtr<mirror::Class> klass,
                                   HashSet<std::string>* image_classes)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  DCHECK_EQ(self, Thread::Current());
  DCHECK(klass->IsResolved());
  Runtime* runtime = Runtime::Current();
  gc::Heap* heap = runtime->GetHeap();
  if (heap->ObjectIsInBootImageSpace(klass)) {
    // We're compiling a boot image extension and the class is already
    // in the boot image we're compiling against.
    return;
  }
  const PointerSize pointer_size = runtime->GetClassLinker()->GetImagePointerSize();
  std::string temp;
  while (!klass->IsObjectClass()) {
    const char* descriptor = klass->GetDescriptor(&temp);
    if (image_classes->find(std::string_view(descriptor)) != image_classes->end()) {
      break;  // Previously inserted.
    }
    image_classes->insert(descriptor);
    VLOG(compiler) << "Adding " << descriptor << " to image classes";
    for (size_t i = 0, num_interfaces = klass->NumDirectInterfaces(); i != num_interfaces; ++i) {
      ObjPtr<mirror::Class> interface = klass->GetDirectInterface(i);
      DCHECK(interface != nullptr);
      MaybeAddToImageClasses(self, interface, image_classes);
    }
    for (auto& m : klass->GetVirtualMethods(pointer_size)) {
      MaybeAddToImageClasses(self, m.GetDeclaringClass(), image_classes);
    }
    if (klass->IsArrayClass()) {
      MaybeAddToImageClasses(self, klass->GetComponentType(), image_classes);
    }
    klass = klass->GetSuperClass();
  }
}

// Keeps all the data for the update together. Also doubles as the reference visitor.
// Note: we can use object pointers because we suspend all threads.
class ClinitImageUpdate {
 public:
  ClinitImageUpdate(HashSet<std::string>* image_class_descriptors,
                    Thread* self) REQUIRES_SHARED(Locks::mutator_lock_)
      : hs_(self),
        image_class_descriptors_(image_class_descriptors),
        self_(self) {
    CHECK(image_class_descriptors != nullptr);

    // Make sure nobody interferes with us.
    old_cause_ = self->StartAssertNoThreadSuspension("Boot image closure");
  }

  ~ClinitImageUpdate() {
    // Allow others to suspend again.
    self_->EndAssertNoThreadSuspension(old_cause_);
  }

  // Visitor for VisitReferences.
  void operator()(ObjPtr<mirror::Object> object,
                  MemberOffset field_offset,
                  [[maybe_unused]] bool is_static) const REQUIRES_SHARED(Locks::mutator_lock_) {
    mirror::Object* ref = object->GetFieldObject<mirror::Object>(field_offset);
    if (ref != nullptr) {
      VisitClinitClassesObject(ref);
    }
  }

  // java.lang.ref.Reference visitor for VisitReferences.
  void operator()([[maybe_unused]] ObjPtr<mirror::Class> klass,
                  [[maybe_unused]] ObjPtr<mirror::Reference> ref) const {}

  // Ignore class native roots.
  void VisitRootIfNonNull(
      [[maybe_unused]] mirror::CompressedReference<mirror::Object>* root) const {}
  void VisitRoot([[maybe_unused]] mirror::CompressedReference<mirror::Object>* root) const {}

  void Walk() REQUIRES_SHARED(Locks::mutator_lock_) {
    // Find all the already-marked classes.
    WriterMutexLock mu(self_, *Locks::heap_bitmap_lock_);
    FindImageClassesVisitor visitor(this);
    Runtime::Current()->GetClassLinker()->VisitClasses(&visitor);

    // Use the initial classes as roots for a search.
    for (Handle<mirror::Class> klass_root : image_classes_) {
      VisitClinitClassesObject(klass_root.Get());
    }
    ScopedAssertNoThreadSuspension ants(__FUNCTION__);
    for (Handle<mirror::Class> h_klass : to_insert_) {
      MaybeAddToImageClasses(self_, h_klass.Get(), image_class_descriptors_);
    }
  }

 private:
  class FindImageClassesVisitor : public ClassVisitor {
   public:
    explicit FindImageClassesVisitor(ClinitImageUpdate* data)
        : data_(data) {}

    bool operator()(ObjPtr<mirror::Class> klass) override REQUIRES_SHARED(Locks::mutator_lock_) {
      bool resolved = klass->IsResolved();
      DCHECK(resolved || klass->IsErroneousUnresolved());
      bool can_include_in_image =
          LIKELY(resolved) && LIKELY(!klass->IsErroneous()) && CanIncludeInCurrentImage(klass);
      std::string temp;
      std::string_view descriptor(klass->GetDescriptor(&temp));
      auto it = data_->image_class_descriptors_->find(descriptor);
      if (it != data_->image_class_descriptors_->end()) {
        if (can_include_in_image) {
          data_->image_classes_.push_back(data_->hs_.NewHandle(klass));
        } else {
          VLOG(compiler) << "Removing " << (resolved ? "unsuitable" : "unresolved")
              << " class from image classes: " << descriptor;
          data_->image_class_descriptors_->erase(it);
        }
      } else if (can_include_in_image) {
        // Check whether the class is initialized and has a clinit or static fields.
        // Such classes must be kept too.
        if (klass->IsInitialized() && !klass->IsArrayClass()) {
          PointerSize pointer_size = Runtime::Current()->GetClassLinker()->GetImagePointerSize();
          if (klass->FindClassInitializer(pointer_size) != nullptr || klass->HasStaticFields()) {
            DCHECK(!Runtime::Current()->GetHeap()->ObjectIsInBootImageSpace(klass->GetDexCache()))
                << klass->PrettyDescriptor();
            data_->image_classes_.push_back(data_->hs_.NewHandle(klass));
          }
        }
      }
      return true;
    }

   private:
    ClinitImageUpdate* const data_;
  };

  void VisitClinitClassesObject(mirror::Object* object) const
      REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK(object != nullptr);
    if (marked_objects_.find(object) != marked_objects_.end()) {
      // Already processed.
      return;
    }

    // Mark it.
    marked_objects_.insert(object);

    if (object->IsClass()) {
      // Add to the TODO list since MaybeAddToImageClasses may cause thread suspension. Thread
      // suspensionb is not safe to do in VisitObjects or VisitReferences.
      to_insert_.push_back(hs_.NewHandle(object->AsClass()));
    } else {
      // Else visit the object's class.
      VisitClinitClassesObject(object->GetClass());
    }

    // If it is not a DexCache, visit all references.
    if (!object->IsDexCache()) {
      object->VisitReferences(*this, *this);
    }
  }

  mutable VariableSizedHandleScope hs_;
  mutable std::vector<Handle<mirror::Class>> to_insert_;
  mutable HashSet<mirror::Object*> marked_objects_;
  HashSet<std::string>* const image_class_descriptors_;
  std::vector<Handle<mirror::Class>> image_classes_;
  Thread* const self_;
  const char* old_cause_;

  DISALLOW_COPY_AND_ASSIGN(ClinitImageUpdate);
};

void CompilerDriver::UpdateImageClasses(TimingLogger* timings,
                                        /*inout*/ HashSet<std::string>* image_classes) {
  DCHECK(GetCompilerOptions().IsGeneratingImage());
  TimingLogger::ScopedTiming t("UpdateImageClasses", timings);

  // Suspend all threads.
  ScopedSuspendAll ssa(__FUNCTION__);

  ClinitImageUpdate update(image_classes, Thread::Current());

  // Do the marking.
  update.Walk();
}

void CompilerDriver::ProcessedInstanceField(bool resolved) {
  if (!resolved) {
    stats_->UnresolvedInstanceField();
  } else {
    stats_->ResolvedInstanceField();
  }
}

void CompilerDriver::ProcessedStaticField(bool resolved, bool local) {
  if (!resolved) {
    stats_->UnresolvedStaticField();
  } else if (local) {
    stats_->ResolvedLocalStaticField();
  } else {
    stats_->ResolvedStaticField();
  }
}

ArtField* CompilerDriver::ComputeInstanceFieldInfo(uint32_t field_idx,
                                                   const DexCompilationUnit* mUnit,
                                                   bool is_put,
                                                   const ScopedObjectAccess& soa) {
  // Try to resolve the field and compiling method's class.
  ArtField* resolved_field;
  ObjPtr<mirror::Class> referrer_class;
  Handle<mirror::DexCache> dex_cache(mUnit->GetDexCache());
  {
    Handle<mirror::ClassLoader> class_loader = mUnit->GetClassLoader();
    resolved_field = ResolveField(soa, dex_cache, class_loader, field_idx, /* is_static= */ false);
    referrer_class = resolved_field != nullptr
        ? ResolveCompilingMethodsClass(soa, dex_cache, class_loader, mUnit) : nullptr;
  }
  bool can_link = false;
  if (resolved_field != nullptr && referrer_class != nullptr) {
    std::pair<bool, bool> fast_path = IsFastInstanceField(
        dex_cache.Get(), referrer_class, resolved_field, field_idx);
    can_link = is_put ? fast_path.second : fast_path.first;
  }
  ProcessedInstanceField(can_link);
  return can_link ? resolved_field : nullptr;
}

bool CompilerDriver::ComputeInstanceFieldInfo(uint32_t field_idx, const DexCompilationUnit* mUnit,
                                              bool is_put, MemberOffset* field_offset,
                                              bool* is_volatile) {
  ScopedObjectAccess soa(Thread::Current());
  ArtField* resolved_field = ComputeInstanceFieldInfo(field_idx, mUnit, is_put, soa);

  if (resolved_field == nullptr) {
    // Conservative defaults.
    *is_volatile = true;
    *field_offset = MemberOffset(static_cast<size_t>(-1));
    return false;
  } else {
    *is_volatile = resolved_field->IsVolatile();
    *field_offset = resolved_field->GetOffset();
    return true;
  }
}

class CompilationVisitor {
 public:
  virtual ~CompilationVisitor() {}
  virtual void Visit(size_t index) = 0;
};

class ParallelCompilationManager {
 public:
  ParallelCompilationManager(ClassLinker* class_linker,
                             jobject class_loader,
                             CompilerDriver* compiler,
                             const DexFile* dex_file,
                             ThreadPool* thread_pool)
    : index_(0),
      class_linker_(class_linker),
      class_loader_(class_loader),
      compiler_(compiler),
      dex_file_(dex_file),
      thread_pool_(thread_pool) {}

  ClassLinker* GetClassLinker() const {
    CHECK(class_linker_ != nullptr);
    return class_linker_;
  }

  jobject GetClassLoader() const {
    return class_loader_;
  }

  CompilerDriver* GetCompiler() const {
    CHECK(compiler_ != nullptr);
    return compiler_;
  }

  const DexFile* GetDexFile() const {
    CHECK(dex_file_ != nullptr);
    return dex_file_;
  }

  void ForAll(size_t begin, size_t end, CompilationVisitor* visitor, size_t work_units)
      REQUIRES(!*Locks::mutator_lock_) {
    ForAllLambda(begin, end, [visitor](size_t index) { visitor->Visit(index); }, work_units);
  }

  template <typename Fn>
  void ForAllLambda(size_t begin, size_t end, Fn fn, size_t work_units)
      REQUIRES(!*Locks::mutator_lock_) {
    Thread* self = Thread::Current();
    self->AssertNoPendingException();
    CHECK_GT(work_units, 0U);

    index_.store(begin, std::memory_order_relaxed);
    for (size_t i = 0; i < work_units; ++i) {
      thread_pool_->AddTask(self, new ForAllClosureLambda<Fn>(this, end, fn));
    }
    thread_pool_->StartWorkers(self);

    // Ensure we're suspended while we're blocked waiting for the other threads to finish (worker
    // thread destructor's called below perform join).
    CHECK_NE(self->GetState(), ThreadState::kRunnable);

    // Wait for all the worker threads to finish.
    thread_pool_->Wait(self, true, false);

    // And stop the workers accepting jobs.
    thread_pool_->StopWorkers(self);
  }

  size_t NextIndex() {
    return index_.fetch_add(1, std::memory_order_seq_cst);
  }

 private:
  template <typename Fn>
  class ForAllClosureLambda : public Task {
   public:
    ForAllClosureLambda(ParallelCompilationManager* manager, size_t end, Fn fn)
        : manager_(manager),
          end_(end),
          fn_(fn) {}

    void Run(Thread* self) override {
      while (true) {
        const size_t index = manager_->NextIndex();
        if (UNLIKELY(index >= end_)) {
          break;
        }
        fn_(index);
        self->AssertNoPendingException();
      }
    }

    void Finalize() override {
      delete this;
    }

   private:
    ParallelCompilationManager* const manager_;
    const size_t end_;
    Fn fn_;
  };

  AtomicInteger index_;
  ClassLinker* const class_linker_;
  const jobject class_loader_;
  CompilerDriver* const compiler_;
  const DexFile* const dex_file_;
  ThreadPool* const thread_pool_;

  DISALLOW_COPY_AND_ASSIGN(ParallelCompilationManager);
};

// A fast version of SkipClass above if the class pointer is available
// that avoids the expensive FindInClassPath search.
static bool SkipClass(jobject class_loader, const DexFile& dex_file, ObjPtr<mirror::Class> klass)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  DCHECK(klass != nullptr);
  const DexFile& original_dex_file = klass->GetDexFile();
  if (&dex_file != &original_dex_file) {
    if (class_loader == nullptr) {
      LOG(WARNING) << "Skipping class " << klass->PrettyDescriptor() << " from "
                   << dex_file.GetLocation() << " previously found in "
                   << original_dex_file.GetLocation();
    }
    return true;
  }
  return false;
}

static void DCheckResolveException(mirror::Throwable* exception)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (!kIsDebugBuild) {
    return;
  }
  std::string temp;
  const char* descriptor = exception->GetClass()->GetDescriptor(&temp);
  const char* expected_exceptions[] = {
      "Ljava/lang/ClassFormatError;",
      "Ljava/lang/ClassCircularityError;",
      "Ljava/lang/IllegalAccessError;",
      "Ljava/lang/IncompatibleClassChangeError;",
      "Ljava/lang/InstantiationError;",
      "Ljava/lang/LinkageError;",
      "Ljava/lang/NoClassDefFoundError;",
      "Ljava/lang/VerifyError;",
  };
  bool found = false;
  for (size_t i = 0; (found == false) && (i < arraysize(expected_exceptions)); ++i) {
    if (strcmp(descriptor, expected_exceptions[i]) == 0) {
      found = true;
    }
  }
  if (!found) {
    LOG(FATAL) << "Unexpected exception " << exception->Dump();
  }
}

template <bool kApp>
class ResolveTypeVisitor : public CompilationVisitor {
 public:
  explicit ResolveTypeVisitor(const ParallelCompilationManager* manager) : manager_(manager) {
  }
  void Visit(size_t index) override REQUIRES(!Locks::mutator_lock_) {
    const DexFile& dex_file = *manager_->GetDexFile();
    // For boot images we resolve all referenced types, such as arrays,
    // whereas for applications just those with classdefs.
    dex::TypeIndex type_idx = kApp ? dex_file.GetClassDef(index).class_idx_ : dex::TypeIndex(index);
    ClassLinker* class_linker = manager_->GetClassLinker();
    ScopedObjectAccess soa(Thread::Current());
    StackHandleScope<kApp ? 4u : 2u> hs(soa.Self());
    Handle<mirror::ClassLoader> class_loader(
        hs.NewHandle(soa.Decode<mirror::ClassLoader>(manager_->GetClassLoader())));
    // TODO: Fix tests that require `RegisterDexFile()` and use `FindDexCache()` in all cases.
    Handle<mirror::DexCache> dex_cache = hs.NewHandle(
        kApp ? class_linker->FindDexCache(soa.Self(), dex_file)
             : class_linker->RegisterDexFile(dex_file, class_loader.Get()));
    DCHECK(dex_cache != nullptr);

    // Resolve the class.
    ObjPtr<mirror::Class> klass = class_linker->ResolveType(type_idx, dex_cache, class_loader);
    if (klass == nullptr) {
      mirror::Throwable* exception = soa.Self()->GetException();
      DCHECK(exception != nullptr);
      VLOG(compiler) << "Exception during type resolution: " << exception->Dump();
      if (exception->GetClass() == WellKnownClasses::java_lang_OutOfMemoryError.Get()) {
        // There's little point continuing compilation if the heap is exhausted.
        // Trying to do so would also introduce non-deterministic compilation results.
        LOG(FATAL) << "Out of memory during type resolution for compilation";
      }
      DCheckResolveException(exception);
      soa.Self()->ClearException();
    } else {
      if (kApp && manager_->GetCompiler()->GetCompilerOptions().IsCheckLinkageConditions()) {
        Handle<mirror::Class> hklass = hs.NewHandle(klass);
        bool is_fatal = manager_->GetCompiler()->GetCompilerOptions().IsCrashOnLinkageViolation();
        Handle<mirror::ClassLoader> defining_class_loader = hs.NewHandle(hklass->GetClassLoader());
        if (defining_class_loader.Get() != class_loader.Get()) {
          // Redefinition via different ClassLoaders.
          // This OptStat stuff is to enable logging from the APK scanner.
          if (is_fatal)
            LOG(FATAL) << "OptStat#" << hklass->PrettyClassAndClassLoader() << ": 1";
          else
            LOG(ERROR)
                << "LINKAGE VIOLATION: "
                << hklass->PrettyClassAndClassLoader()
                << " was redefined";
        }
        // Check that the current class is not a subclass of java.lang.ClassLoader.
        if (!hklass->IsInterface() &&
            hklass->IsSubClass(GetClassRoot<mirror::ClassLoader>(class_linker))) {
          // Subclassing of java.lang.ClassLoader.
          // This OptStat stuff is to enable logging from the APK scanner.
          if (is_fatal) {
            LOG(FATAL) << "OptStat#" << hklass->PrettyClassAndClassLoader() << ": 1";
          } else {
            LOG(ERROR)
                << "LINKAGE VIOLATION: "
                << hklass->PrettyClassAndClassLoader()
                << " is a subclass of java.lang.ClassLoader";
          }
        }
        CHECK(hklass->IsResolved()) << hklass->PrettyClass();
      }
    }
  }

 private:
  const ParallelCompilationManager* const manager_;
};

void CompilerDriver::ResolveDexFile(jobject class_loader,
                                    const DexFile& dex_file,
                                    ThreadPool* thread_pool,
                                    size_t thread_count,
                                    TimingLogger* timings) {
  ScopedTrace trace(__FUNCTION__);
  TimingLogger::ScopedTiming t("Resolve Dex File", timings);
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();

  // TODO: we could resolve strings here, although the string table is largely filled with class
  //       and method names.

  ParallelCompilationManager context(class_linker, class_loader, this, &dex_file, thread_pool);
  // For boot images we resolve all referenced types, such as arrays,
  // whereas for applications just those with classdefs.
  if (GetCompilerOptions().IsBootImage() || GetCompilerOptions().IsBootImageExtension()) {
    ResolveTypeVisitor</*kApp=*/ false> visitor(&context);
    context.ForAll(0, dex_file.NumTypeIds(), &visitor, thread_count);
  } else {
    ResolveTypeVisitor</*kApp=*/ true> visitor(&context);
    context.ForAll(0, dex_file.NumClassDefs(), &visitor, thread_count);
  }
}

void CompilerDriver::SetVerified(jobject class_loader,
                                 const std::vector<const DexFile*>& dex_files,
                                 TimingLogger* timings) {
  // This can be run in parallel.
  for (const DexFile* dex_file : dex_files) {
    CHECK(dex_file != nullptr);
    SetVerifiedDexFile(class_loader,
                       *dex_file,
                       parallel_thread_pool_.get(),
                       parallel_thread_count_,
                       timings);
  }
}

static void LoadAndUpdateStatus(const ClassAccessor& accessor,
                                ClassStatus status,
                                Handle<mirror::ClassLoader> class_loader,
                                Thread* self)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  StackHandleScope<1> hs(self);
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Handle<mirror::Class> cls(hs.NewHandle<mirror::Class>(
      class_linker->FindClass(self, accessor.GetDexFile(), accessor.GetClassIdx(), class_loader)));
  if (cls != nullptr) {
    // Check that the class is resolved with the current dex file. We might get
    // a boot image class, or a class in a different dex file for multidex, and
    // we should not update the status in that case.
    if (&cls->GetDexFile() == &accessor.GetDexFile()) {
      VLOG(compiler) << "Updating class status of " << accessor.GetDescriptorView()
                     << " to " << status;
      ObjectLock<mirror::Class> lock(self, cls);
      mirror::Class::SetStatus(cls, status, self);
    }
  } else {
    DCHECK(self->IsExceptionPending());
    self->ClearException();
  }
}

bool CompilerDriver::FastVerify(jobject jclass_loader,
                                const std::vector<const DexFile*>& dex_files,
                                TimingLogger* timings) {
  CompilerCallbacks* callbacks = Runtime::Current()->GetCompilerCallbacks();
  verifier::VerifierDeps* verifier_deps = callbacks->GetVerifierDeps();
  // If there exist VerifierDeps that aren't the ones we just created to output, use them to verify.
  if (verifier_deps == nullptr || verifier_deps->OutputOnly()) {
    return false;
  }
  TimingLogger::ScopedTiming t("Fast Verify", timings);

  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader>(jclass_loader)));
  std::string error_msg;

  verifier_deps->ValidateDependenciesAndUpdateStatus(
      soa.Self(),
      class_loader,
      dex_files);

  bool compiler_only_verifies =
      !GetCompilerOptions().IsAnyCompilationEnabled() &&
      !GetCompilerOptions().IsGeneratingImage();

  const bool is_generating_image = GetCompilerOptions().IsGeneratingImage();

  // We successfully validated the dependencies, now update class status
  // of verified classes. Note that the dependencies also record which classes
  // could not be fully verified; we could try again, but that would hurt verification
  // time. So instead we assume these classes still need to be verified at
  // runtime.
  for (const DexFile* dex_file : dex_files) {
    // Fetch the list of verified classes.
    const std::vector<bool>& verified_classes = verifier_deps->GetVerifiedClasses(*dex_file);
    DCHECK_EQ(verified_classes.size(), dex_file->NumClassDefs());
    for (ClassAccessor accessor : dex_file->GetClasses()) {
      ClassStatus status = verified_classes[accessor.GetClassDefIndex()]
          ? ClassStatus::kVerifiedNeedsAccessChecks
          : ClassStatus::kRetryVerificationAtRuntime;
      if (compiler_only_verifies) {
        // Just update the compiled_classes_ map. The compiler doesn't need to resolve
        // the type.
        ClassReference ref(dex_file, accessor.GetClassDefIndex());
        const ClassStatus existing = ClassStatus::kNotReady;
        // Note: when dex files are compiled inidividually, the class may have
        // been verified in a previous stage. This means this insertion can
        // fail, but that's OK.
        compiled_classes_.Insert(ref, existing, status);
      } else {
        if (is_generating_image &&
            status == ClassStatus::kVerifiedNeedsAccessChecks &&
            GetCompilerOptions().IsImageClass(accessor.GetDescriptor())) {
          // If the class will be in the image, we can rely on the ArtMethods
          // telling that they need access checks.
          VLOG(compiler) << "Promoting "
                         << accessor.GetDescriptorView()
                         << " from needs access checks to verified given it is an image class";
          status = ClassStatus::kVerified;
        }
        // Update the class status, so later compilation stages know they don't need to verify
        // the class.
        LoadAndUpdateStatus(accessor, status, class_loader, soa.Self());
      }

      // Vdex marks class as unverified for two reasons only:
      // 1. It has a hard failure, or
      // 2. One of its method needs lock counting.
      //
      // The optimizing compiler expects a method to not have a hard failure before
      // compiling it, so for simplicity just disable any compilation of methods
      // of these classes.
      if (status == ClassStatus::kRetryVerificationAtRuntime) {
        ClassReference ref(dex_file, accessor.GetClassDefIndex());
        callbacks->AddUncompilableClass(ref);
      }
    }
  }
  return true;
}

void CompilerDriver::Verify(jobject jclass_loader,
                            const std::vector<const DexFile*>& dex_files,
                            TimingLogger* timings) {
  if (FastVerify(jclass_loader, dex_files, timings)) {
    return;
  }

  // If there is no existing `verifier_deps` (because of non-existing vdex), or
  // the existing `verifier_deps` is not valid anymore, create a new one. The
  // verifier will need it to record the new dependencies. Then dex2oat can update
  // the vdex file with these new dependencies.
  // Dex2oat creates the verifier deps.
  // Create the main VerifierDeps, and set it to this thread.
  verifier::VerifierDeps* main_verifier_deps =
      Runtime::Current()->GetCompilerCallbacks()->GetVerifierDeps();
  // Verifier deps can be null when unit testing.
  if (main_verifier_deps != nullptr) {
    Thread::Current()->SetVerifierDeps(main_verifier_deps);
    // Create per-thread VerifierDeps to avoid contention on the main one.
    // We will merge them after verification.
    for (ThreadPoolWorker* worker : parallel_thread_pool_->GetWorkers()) {
      worker->GetThread()->SetVerifierDeps(
          new verifier::VerifierDeps(GetCompilerOptions().GetDexFilesForOatFile()));
    }
  }

  {
    TimingLogger::ScopedTiming t("Verify Classes", timings);
    // Verification updates VerifierDeps and needs to run single-threaded to be deterministic.
    bool force_determinism = GetCompilerOptions().IsForceDeterminism();
    ThreadPool* verify_thread_pool =
        force_determinism ? single_thread_pool_.get() : parallel_thread_pool_.get();
    size_t verify_thread_count = force_determinism ? 1U : parallel_thread_count_;
    for (const DexFile* dex_file : dex_files) {
      CHECK(dex_file != nullptr);
      VerifyDexFile(jclass_loader,
                    *dex_file,
                    verify_thread_pool,
                    verify_thread_count,
                    timings);
    }
  }

  if (main_verifier_deps != nullptr) {
    // Merge all VerifierDeps into the main one.
    for (ThreadPoolWorker* worker : parallel_thread_pool_->GetWorkers()) {
      std::unique_ptr<verifier::VerifierDeps> thread_deps(worker->GetThread()->GetVerifierDeps());
      worker->GetThread()->SetVerifierDeps(nullptr);  // We just took ownership.
      main_verifier_deps->MergeWith(std::move(thread_deps),
                                    GetCompilerOptions().GetDexFilesForOatFile());
    }
    Thread::Current()->SetVerifierDeps(nullptr);
  }
}

class VerifyClassVisitor : public CompilationVisitor {
 public:
  VerifyClassVisitor(const ParallelCompilationManager* manager, verifier::HardFailLogMode log_level)
     : manager_(manager),
       log_level_(log_level),
       sdk_version_(Runtime::Current()->GetTargetSdkVersion()) {}

  void Visit(size_t class_def_index) REQUIRES(!Locks::mutator_lock_) override {
    ScopedTrace trace(__FUNCTION__);
    ScopedObjectAccess soa(Thread::Current());
    const DexFile& dex_file = *manager_->GetDexFile();
    const dex::ClassDef& class_def = dex_file.GetClassDef(class_def_index);
    ClassLinker* class_linker = manager_->GetClassLinker();
    jobject jclass_loader = manager_->GetClassLoader();
    StackHandleScope<3> hs(soa.Self());
    Handle<mirror::ClassLoader> class_loader(
        hs.NewHandle(soa.Decode<mirror::ClassLoader>(jclass_loader)));
    Handle<mirror::Class> klass = hs.NewHandle(
        class_linker->FindClass(soa.Self(), dex_file, class_def.class_idx_, class_loader));
    ClassReference ref(manager_->GetDexFile(), class_def_index);
    verifier::FailureKind failure_kind;
    if (klass == nullptr) {
      CHECK(soa.Self()->IsExceptionPending());
      soa.Self()->ClearException();

      /*
       * At compile time, we can still structurally verify the class even if FindClass fails.
       * This is to ensure the class is structurally sound for compilation. An unsound class
       * will be rejected by the verifier and later skipped during compilation in the compiler.
       */
      Handle<mirror::DexCache> dex_cache(hs.NewHandle(class_linker->FindDexCache(
          soa.Self(), dex_file)));
      std::string error_msg;
      failure_kind =
          verifier::ClassVerifier::VerifyClass(soa.Self(),
                                               soa.Self()->GetVerifierDeps(),
                                               &dex_file,
                                               klass,
                                               dex_cache,
                                               class_loader,
                                               class_def,
                                               Runtime::Current()->GetCompilerCallbacks(),
                                               log_level_,
                                               sdk_version_,
                                               &error_msg);
      switch (failure_kind) {
        case verifier::FailureKind::kHardFailure: {
          manager_->GetCompiler()->SetHadHardVerifierFailure();
          break;
        }
        case verifier::FailureKind::kSoftFailure: {
          manager_->GetCompiler()->AddSoftVerifierFailure();
          break;
        }
        case verifier::FailureKind::kTypeChecksFailure: {
          // Don't record anything, we will do the type checks from the vdex
          // file at runtime.
          break;
        }
        case verifier::FailureKind::kAccessChecksFailure: {
          manager_->GetCompiler()->RecordClassStatus(ref, ClassStatus::kVerifiedNeedsAccessChecks);
          break;
        }
        case verifier::FailureKind::kNoFailure: {
          manager_->GetCompiler()->RecordClassStatus(ref, ClassStatus::kVerified);
          break;
        }
      }
    } else if (SkipClass(jclass_loader, dex_file, klass.Get())) {
      // Skip a duplicate class (as the resolved class is from another, earlier dex file).
      return;  // Do not update state.
    } else {
      CHECK(klass->IsResolved()) << klass->PrettyClass();
      failure_kind = class_linker->VerifyClass(soa.Self(),
                                               soa.Self()->GetVerifierDeps(),
                                               klass,
                                               log_level_);

      DCHECK_EQ(klass->IsErroneous(), failure_kind == verifier::FailureKind::kHardFailure);
      if (failure_kind == verifier::FailureKind::kHardFailure) {
        // ClassLinker::VerifyClass throws, which isn't useful in the compiler.
        CHECK(soa.Self()->IsExceptionPending());
        soa.Self()->ClearException();
        manager_->GetCompiler()->SetHadHardVerifierFailure();
      } else if (failure_kind == verifier::FailureKind::kSoftFailure) {
        manager_->GetCompiler()->AddSoftVerifierFailure();
      }

      CHECK(klass->ShouldVerifyAtRuntime() ||
            klass->IsVerifiedNeedsAccessChecks() ||
            klass->IsVerified() ||
            klass->IsErroneous())
          << klass->PrettyDescriptor() << ": state=" << klass->GetStatus();

      // Class has a meaningful status for the compiler now, record it.
      ClassStatus status = klass->GetStatus();
      if (status == ClassStatus::kInitialized) {
        // Initialized classes shall be visibly initialized when loaded from the image.
        status = ClassStatus::kVisiblyInitialized;
      }
      manager_->GetCompiler()->RecordClassStatus(ref, status);

      // It is *very* problematic if there are resolution errors in the boot classpath.
      //
      // It is also bad if classes fail verification. For example, we rely on things working
      // OK without verification when the decryption dialog is brought up. It is thus highly
      // recommended to compile the boot classpath with
      //   --abort-on-hard-verifier-error --abort-on-soft-verifier-error
      // which is the default build system configuration.
      if (kIsDebugBuild) {
        if (manager_->GetCompiler()->GetCompilerOptions().IsBootImage() ||
            manager_->GetCompiler()->GetCompilerOptions().IsBootImageExtension()) {
          if (!klass->IsResolved() || klass->IsErroneous()) {
            LOG(FATAL) << "Boot classpath class " << klass->PrettyClass()
                       << " failed to resolve/is erroneous: state= " << klass->GetStatus();
            UNREACHABLE();
          }
        }
        if (klass->IsVerified()) {
          DCHECK_EQ(failure_kind, verifier::FailureKind::kNoFailure);
        } else if (klass->IsVerifiedNeedsAccessChecks()) {
          DCHECK_EQ(failure_kind, verifier::FailureKind::kAccessChecksFailure);
        } else if (klass->ShouldVerifyAtRuntime()) {
          DCHECK_NE(failure_kind, verifier::FailureKind::kHardFailure);
          // This could either be due to:
          // - kTypeChecksFailure, or
          // - kSoftFailure, or
          // - the superclass or interfaces not being verified.
        } else {
          DCHECK_EQ(failure_kind, verifier::FailureKind::kHardFailure);
        }
      }
    }
    verifier::VerifierDeps::MaybeRecordVerificationStatus(soa.Self()->GetVerifierDeps(),
                                                          dex_file,
                                                          class_def,
                                                          failure_kind);
    soa.Self()->AssertNoPendingException();
  }

 private:
  const ParallelCompilationManager* const manager_;
  const verifier::HardFailLogMode log_level_;
  const uint32_t sdk_version_;
};

void CompilerDriver::VerifyDexFile(jobject class_loader,
                                   const DexFile& dex_file,
                                   ThreadPool* thread_pool,
                                   size_t thread_count,
                                   TimingLogger* timings) {
  TimingLogger::ScopedTiming t("Verify Dex File", timings);
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  ParallelCompilationManager context(class_linker, class_loader, this, &dex_file, thread_pool);
  bool abort_on_verifier_failures = GetCompilerOptions().AbortOnHardVerifierFailure()
                                    || GetCompilerOptions().AbortOnSoftVerifierFailure();
  verifier::HardFailLogMode log_level = abort_on_verifier_failures
                              ? verifier::HardFailLogMode::kLogInternalFatal
                              : verifier::HardFailLogMode::kLogWarning;
  VerifyClassVisitor visitor(&context, log_level);
  context.ForAll(0, dex_file.NumClassDefs(), &visitor, thread_count);

  // Make initialized classes visibly initialized.
  class_linker->MakeInitializedClassesVisiblyInitialized(Thread::Current(), /*wait=*/ true);
}

class SetVerifiedClassVisitor : public CompilationVisitor {
 public:
  explicit SetVerifiedClassVisitor(const ParallelCompilationManager* manager) : manager_(manager) {}

  void Visit(size_t class_def_index) REQUIRES(!Locks::mutator_lock_) override {
    ScopedTrace trace(__FUNCTION__);
    ScopedObjectAccess soa(Thread::Current());
    const DexFile& dex_file = *manager_->GetDexFile();
    const dex::ClassDef& class_def = dex_file.GetClassDef(class_def_index);
    ClassLinker* class_linker = manager_->GetClassLinker();
    jobject jclass_loader = manager_->GetClassLoader();
    StackHandleScope<3> hs(soa.Self());
    Handle<mirror::ClassLoader> class_loader(
        hs.NewHandle(soa.Decode<mirror::ClassLoader>(jclass_loader)));
    Handle<mirror::Class> klass = hs.NewHandle(
        class_linker->FindClass(soa.Self(), dex_file, class_def.class_idx_, class_loader));
    // Class might have failed resolution. Then don't set it to verified.
    if (klass != nullptr) {
      // Only do this if the class is resolved. If even resolution fails, quickening will go very,
      // very wrong.
      if (klass->IsResolved() && !klass->IsErroneousResolved()) {
        if (klass->GetStatus() < ClassStatus::kVerified) {
          ObjectLock<mirror::Class> lock(soa.Self(), klass);
          // Set class status to verified.
          mirror::Class::SetStatus(klass, ClassStatus::kVerified, soa.Self());
          // Mark methods as pre-verified. If we don't do this, the interpreter will run with
          // access checks.
          InstructionSet instruction_set =
              manager_->GetCompiler()->GetCompilerOptions().GetInstructionSet();
          klass->SetSkipAccessChecksFlagOnAllMethods(GetInstructionSetPointerSize(instruction_set));
        }
        // Record the final class status if necessary.
        ClassReference ref(manager_->GetDexFile(), class_def_index);
        manager_->GetCompiler()->RecordClassStatus(ref, klass->GetStatus());
      }
    } else {
      Thread* self = soa.Self();
      DCHECK(self->IsExceptionPending());
      self->ClearException();
    }
  }

 private:
  const ParallelCompilationManager* const manager_;
};

void CompilerDriver::SetVerifiedDexFile(jobject class_loader,
                                        const DexFile& dex_file,
                                        ThreadPool* thread_pool,
                                        size_t thread_count,
                                        TimingLogger* timings) {
  TimingLogger::ScopedTiming t("Set Verified Dex File", timings);
  if (!compiled_classes_.HaveDexFile(&dex_file)) {
    compiled_classes_.AddDexFile(&dex_file);
  }
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  ParallelCompilationManager context(class_linker, class_loader, this, &dex_file, thread_pool);
  SetVerifiedClassVisitor visitor(&context);
  context.ForAll(0, dex_file.NumClassDefs(), &visitor, thread_count);
}

class InitializeClassVisitor : public CompilationVisitor {
 public:
  explicit InitializeClassVisitor(const ParallelCompilationManager* manager) : manager_(manager) {}

  void Visit(size_t class_def_index) override {
    ScopedTrace trace(__FUNCTION__);
    jobject jclass_loader = manager_->GetClassLoader();
    const DexFile& dex_file = *manager_->GetDexFile();
    const dex::ClassDef& class_def = dex_file.GetClassDef(class_def_index);

    ScopedObjectAccess soa(Thread::Current());
    StackHandleScope<3> hs(soa.Self());
    Handle<mirror::ClassLoader> class_loader(
        hs.NewHandle(soa.Decode<mirror::ClassLoader>(jclass_loader)));
    Handle<mirror::Class> klass = hs.NewHandle(manager_->GetClassLinker()->FindClass(
        soa.Self(), dex_file, class_def.class_idx_, class_loader));

    if (klass != nullptr) {
      if (!SkipClass(manager_->GetClassLoader(), dex_file, klass.Get())) {
        TryInitializeClass(soa.Self(), klass, class_loader);
      }
      manager_->GetCompiler()->stats_->AddClassStatus(klass->GetStatus());
    }
    // Clear any class not found or verification exceptions.
    soa.Self()->ClearException();
  }

  // A helper function for initializing klass.
  void TryInitializeClass(Thread* self,
                          Handle<mirror::Class> klass,
                          Handle<mirror::ClassLoader>& class_loader)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    const DexFile& dex_file = klass->GetDexFile();
    const dex::ClassDef* class_def = klass->GetClassDef();
    const dex::TypeId& class_type_id = dex_file.GetTypeId(class_def->class_idx_);
    const char* descriptor = dex_file.GetStringData(class_type_id.descriptor_idx_);
    StackHandleScope<3> hs(self);
    AotClassLinker* const class_linker = down_cast<AotClassLinker*>(manager_->GetClassLinker());
    Runtime* const runtime = Runtime::Current();
    const CompilerOptions& compiler_options = manager_->GetCompiler()->GetCompilerOptions();
    const bool is_boot_image = compiler_options.IsBootImage();
    const bool is_boot_image_extension = compiler_options.IsBootImageExtension();
    const bool is_app_image = compiler_options.IsAppImage();

    // For boot image extension, do not initialize classes defined
    // in dex files belonging to the boot image we're compiling against.
    if (is_boot_image_extension &&
        runtime->GetHeap()->ObjectIsInBootImageSpace(klass->GetDexCache())) {
      // Also return early and don't store the class status in the recorded class status.
      return;
    }
    // Do not initialize classes in boot space when compiling app (with or without image).
    if ((!is_boot_image && !is_boot_image_extension) && klass->IsBootStrapClassLoaded()) {
      // Also return early and don't store the class status in the recorded class status.
      return;
    }

    ClassStatus old_status = klass->GetStatus();
    // Only try to initialize classes that were successfully verified.
    if (klass->IsVerified()) {
      // Attempt to initialize the class but bail if we either need to initialize the super-class
      // or static fields.
      class_linker->EnsureInitialized(self, klass, false, false);
      DCHECK(!self->IsExceptionPending());
      old_status = klass->GetStatus();
      if (!klass->IsInitialized()) {
        // We don't want non-trivial class initialization occurring on multiple threads due to
        // deadlock problems. For example, a parent class is initialized (holding its lock) that
        // refers to a sub-class in its static/class initializer causing it to try to acquire the
        // sub-class' lock. While on a second thread the sub-class is initialized (holding its lock)
        // after first initializing its parents, whose locks are acquired. This leads to a
        // parent-to-child and a child-to-parent lock ordering and consequent potential deadlock.
        // We need to use an ObjectLock due to potential suspension in the interpreting code. Rather
        // than use a special Object for the purpose we use the Class of java.lang.Class.
        Handle<mirror::Class> h_klass(hs.NewHandle(klass->GetClass()));
        ObjectLock<mirror::Class> lock(self, h_klass);
        // Attempt to initialize allowing initialization of parent classes but still not static
        // fields.
        // Initialize dependencies first only for app or boot image extension,
        // to make TryInitializeClass() recursive.
        bool try_initialize_with_superclasses =
            is_boot_image ? true : InitializeDependencies(klass, class_loader, self);
        if (try_initialize_with_superclasses) {
          class_linker->EnsureInitialized(self, klass, false, true);
          DCHECK(!self->IsExceptionPending());
        }
        // Otherwise it's in app image or boot image extension but superclasses
        // cannot be initialized, no need to proceed.
        old_status = klass->GetStatus();

        ClassAccessor accessor(klass->GetDexFile(), klass->GetDexClassDefIndex());
        bool too_many_encoded_fields = (!is_boot_image && !is_boot_image_extension) &&
            accessor.NumStaticFields() > kMaxEncodedFields;

        bool have_profile = (compiler_options.GetProfileCompilationInfo() != nullptr) &&
            !compiler_options.GetProfileCompilationInfo()->IsEmpty();
        // If the class was not initialized, we can proceed to see if we can initialize static
        // fields. Limit the max number of encoded fields.
        if (!klass->IsInitialized() &&
            (is_app_image || is_boot_image || is_boot_image_extension) &&
            try_initialize_with_superclasses && !too_many_encoded_fields &&
            compiler_options.IsImageClass(descriptor) &&
            // TODO(b/274077782): remove this test.
            (have_profile || !is_boot_image_extension)) {
          bool can_init_static_fields = false;
          if (is_boot_image || is_boot_image_extension) {
            // We need to initialize static fields, we only do this for image classes that aren't
            // marked with the $NoPreloadHolder (which implies this should not be initialized
            // early).
            can_init_static_fields = !std::string_view(descriptor).ends_with("$NoPreloadHolder;");
          } else {
            CHECK(is_app_image);
            // The boot image case doesn't need to recursively initialize the dependencies with
            // special logic since the class linker already does this.
            // Optimization will be disabled in debuggable build, because in debuggable mode we
            // want the <clinit> behavior to be observable for the debugger, so we don't do the
            // <clinit> at compile time.
            can_init_static_fields =
                ClassLinker::kAppImageMayContainStrings &&
                !self->IsExceptionPending() &&
                !compiler_options.GetDebuggable() &&
                (compiler_options.InitializeAppImageClasses() ||
                 NoClinitInDependency(klass, self, &class_loader));
            // TODO The checking for clinit can be removed since it's already
            // checked when init superclass. Currently keep it because it contains
            // processing of intern strings. Will be removed later when intern strings
            // and clinit are both initialized.
          }

          if (can_init_static_fields) {
            VLOG(compiler) << "Initializing: " << descriptor;
            // TODO multithreading support. We should ensure the current compilation thread has
            // exclusive access to the runtime and the transaction. To achieve this, we could use
            // a ReaderWriterMutex but we're holding the mutator lock so we fail the check of mutex
            // validity in Thread::AssertThreadSuspensionIsAllowable.

            // Resolve and initialize the exception type before enabling the transaction in case
            // the transaction aborts and cannot resolve the type.
            // TransactionAbortError is not initialized ant not in boot image, needed only by
            // compiler and will be pruned by ImageWriter.
            Handle<mirror::Class> exception_class =
                hs.NewHandle(class_linker->FindSystemClass(self, kTransactionAbortErrorDescriptor));
            bool exception_initialized =
                class_linker->EnsureInitialized(self, exception_class, true, true);
            DCHECK(exception_initialized);

            // Run the class initializer in transaction mode.
            class_linker->EnterTransactionMode(is_app_image, klass.Get());

            bool success = class_linker->EnsureInitialized(self, klass, true, true);
            // TODO we detach transaction from runtime to indicate we quit the transactional
            // mode which prevents the GC from visiting objects modified during the transaction.
            // Ensure GC is not run so don't access freed objects when aborting transaction.

            {
              ScopedAssertNoThreadSuspension ants("Transaction end");

              if (success) {
                class_linker->ExitTransactionMode();
                DCHECK(!runtime->IsActiveTransaction());

                if (is_boot_image || is_boot_image_extension) {
                  // For boot image and boot image extension, we want to put the updated
                  // status in the oat class. This is not the case for app image as we
                  // want to keep the ability to load the oat file without the app image.
                  old_status = klass->GetStatus();
                }
              } else {
                CHECK(self->IsExceptionPending());
                mirror::Throwable* exception = self->GetException();
                VLOG(compiler) << "Initialization of " << descriptor << " aborted because of "
                               << exception->Dump();
                std::ostream* file_log = manager_->GetCompiler()->
                    GetCompilerOptions().GetInitFailureOutput();
                if (file_log != nullptr) {
                  *file_log << descriptor << "\n";
                  *file_log << exception->Dump() << "\n";
                }
                self->ClearException();
                class_linker->RollbackAllTransactions();
                CHECK_EQ(old_status, klass->GetStatus()) << "Previous class status not restored";
              }
            }

            if (!success && (is_boot_image || is_boot_image_extension)) {
              // On failure, still intern strings of static fields and seen in <clinit>, as these
              // will be created in the zygote. This is separated from the transaction code just
              // above as we will allocate strings, so must be allowed to suspend.
              // We only need to intern strings for boot image and boot image extension
              // because classes that failed to be initialized will not appear in app image.
              if (&klass->GetDexFile() == manager_->GetDexFile()) {
                InternStrings(klass, class_loader);
              } else {
                DCHECK(!is_boot_image) << "Boot image must have equal dex files";
              }
            }
          }
        }
        // Clear exception in case EnsureInitialized has caused one in the code above.
        // It's OK to clear the exception here since the compiler is supposed to be fault
        // tolerant and will silently not initialize classes that have exceptions.
        self->ClearException();

        // If the class still isn't initialized, at least try some checks that initialization
        // would do so they can be skipped at runtime.
        if (!klass->IsInitialized() && class_linker->ValidateSuperClassDescriptors(klass)) {
          old_status = ClassStatus::kSuperclassValidated;
        } else {
          self->ClearException();
        }
        self->AssertNoPendingException();
      }
    }
    if (old_status == ClassStatus::kInitialized) {
      // Initialized classes shall be visibly initialized when loaded from the image.
      old_status = ClassStatus::kVisiblyInitialized;
    }
    // Record the final class status if necessary.
    ClassReference ref(&dex_file, klass->GetDexClassDefIndex());
    // Back up the status before doing initialization for static encoded fields,
    // because the static encoded branch wants to keep the status to uninitialized.
    manager_->GetCompiler()->RecordClassStatus(ref, old_status);

    if (kIsDebugBuild) {
      // Make sure the class initialization did not leave any local references.
      self->GetJniEnv()->AssertLocalsEmpty();
    }

    if (!klass->IsInitialized() &&
        (is_boot_image || is_boot_image_extension) &&
        !compiler_options.IsPreloadedClass(PrettyDescriptor(descriptor))) {
      klass->SetInBootImageAndNotInPreloadedClasses();
    }

    if (compiler_options.CompileArtTest()) {
      // For stress testing and unit-testing the clinit check in compiled code feature.
      if (kIsDebugBuild || std::string_view(descriptor).ends_with("$NoPreloadHolder;")) {
        klass->SetInBootImageAndNotInPreloadedClasses();
      }
    }
  }

 private:
  void InternStrings(Handle<mirror::Class> klass, Handle<mirror::ClassLoader> class_loader)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK(manager_->GetCompiler()->GetCompilerOptions().IsBootImage() ||
           manager_->GetCompiler()->GetCompilerOptions().IsBootImageExtension());
    DCHECK(klass->IsVerified());
    DCHECK(!klass->IsInitialized());

    StackHandleScope<1> hs(Thread::Current());
    Handle<mirror::DexCache> dex_cache = hs.NewHandle(klass->GetDexCache());
    const dex::ClassDef* class_def = klass->GetClassDef();
    ClassLinker* class_linker = manager_->GetClassLinker();

    // Check encoded final field values for strings and intern.
    annotations::RuntimeEncodedStaticFieldValueIterator value_it(dex_cache,
                                                                 class_loader,
                                                                 manager_->GetClassLinker(),
                                                                 *class_def);
    for ( ; value_it.HasNext(); value_it.Next()) {
      if (value_it.GetValueType() == annotations::RuntimeEncodedStaticFieldValueIterator::kString) {
        // Resolve the string. This will intern the string.
        art::ObjPtr<mirror::String> resolved = class_linker->ResolveString(
            dex::StringIndex(value_it.GetJavaValue().i), dex_cache);
        CHECK(resolved != nullptr);
      }
    }

    // Intern strings seen in <clinit>.
    ArtMethod* clinit = klass->FindClassInitializer(class_linker->GetImagePointerSize());
    if (clinit != nullptr) {
      for (const DexInstructionPcPair& inst : clinit->DexInstructions()) {
        if (inst->Opcode() == Instruction::CONST_STRING) {
          ObjPtr<mirror::String> s = class_linker->ResolveString(
              dex::StringIndex(inst->VRegB_21c()), dex_cache);
          CHECK(s != nullptr);
        } else if (inst->Opcode() == Instruction::CONST_STRING_JUMBO) {
          ObjPtr<mirror::String> s = class_linker->ResolveString(
              dex::StringIndex(inst->VRegB_31c()), dex_cache);
          CHECK(s != nullptr);
        }
      }
    }
  }

  bool ResolveTypesOfMethods(Thread* self, ArtMethod* m)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    // Return value of ResolveReturnType() is discarded because resolve will be done internally.
    ObjPtr<mirror::Class> rtn_type = m->ResolveReturnType();
    if (rtn_type == nullptr) {
      self->ClearException();
      return false;
    }
    const dex::TypeList* types = m->GetParameterTypeList();
    if (types != nullptr) {
      for (uint32_t i = 0; i < types->Size(); ++i) {
        dex::TypeIndex param_type_idx = types->GetTypeItem(i).type_idx_;
        ObjPtr<mirror::Class> param_type = m->ResolveClassFromTypeIndex(param_type_idx);
        if (param_type == nullptr) {
          self->ClearException();
          return false;
        }
      }
    }
    return true;
  }

  // Pre resolve types mentioned in all method signatures before start a transaction
  // since ResolveType doesn't work in transaction mode.
  bool PreResolveTypes(Thread* self, const Handle<mirror::Class>& klass)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    PointerSize pointer_size = manager_->GetClassLinker()->GetImagePointerSize();
    for (ArtMethod& m : klass->GetMethods(pointer_size)) {
      if (!ResolveTypesOfMethods(self, &m)) {
        return false;
      }
    }
    if (klass->IsInterface()) {
      return true;
    } else if (klass->HasSuperClass()) {
      StackHandleScope<1> hs(self);
      MutableHandle<mirror::Class> super_klass(hs.NewHandle<mirror::Class>(klass->GetSuperClass()));
      for (int i = super_klass->GetVTableLength() - 1; i >= 0; --i) {
        ArtMethod* m = klass->GetVTableEntry(i, pointer_size);
        ArtMethod* super_m = super_klass->GetVTableEntry(i, pointer_size);
        if (!ResolveTypesOfMethods(self, m) || !ResolveTypesOfMethods(self, super_m)) {
          return false;
        }
      }
      for (int32_t i = 0; i < klass->GetIfTableCount(); ++i) {
        super_klass.Assign(klass->GetIfTable()->GetInterface(i));
        if (klass->GetClassLoader() != super_klass->GetClassLoader()) {
          uint32_t num_methods = super_klass->NumVirtualMethods();
          for (uint32_t j = 0; j < num_methods; ++j) {
            ArtMethod* m = klass->GetIfTable()->GetMethodArray(i)->GetElementPtrSize<ArtMethod*>(
                j, pointer_size);
            ArtMethod* super_m = super_klass->GetVirtualMethod(j, pointer_size);
            if (!ResolveTypesOfMethods(self, m) || !ResolveTypesOfMethods(self, super_m)) {
              return false;
            }
          }
        }
      }
    }
    return true;
  }

  // Initialize the klass's dependencies recursively before initializing itself.
  // Checking for interfaces is also necessary since interfaces that contain
  // default methods must be initialized before the class.
  bool InitializeDependencies(const Handle<mirror::Class>& klass,
                              Handle<mirror::ClassLoader> class_loader,
                              Thread* self)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (klass->HasSuperClass()) {
      StackHandleScope<1> hs(self);
      Handle<mirror::Class> super_class = hs.NewHandle(klass->GetSuperClass());
      if (!super_class->IsInitialized()) {
        this->TryInitializeClass(self, super_class, class_loader);
        if (!super_class->IsInitialized()) {
          return false;
        }
      }
    }

    if (!klass->IsInterface()) {
      size_t num_interfaces = klass->GetIfTableCount();
      for (size_t i = 0; i < num_interfaces; ++i) {
        StackHandleScope<1> hs(self);
        Handle<mirror::Class> iface = hs.NewHandle(klass->GetIfTable()->GetInterface(i));
        if (iface->HasDefaultMethods() && !iface->IsInitialized()) {
          TryInitializeClass(self, iface, class_loader);
          if (!iface->IsInitialized()) {
            return false;
          }
        }
      }
    }

    return PreResolveTypes(self, klass);
  }

  // In this phase the classes containing class initializers are ignored. Make sure no
  // clinit appears in klass's super class chain and interfaces.
  bool NoClinitInDependency(const Handle<mirror::Class>& klass,
                            Thread* self,
                            Handle<mirror::ClassLoader>* class_loader)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    ArtMethod* clinit =
        klass->FindClassInitializer(manager_->GetClassLinker()->GetImagePointerSize());
    if (clinit != nullptr) {
      VLOG(compiler) << klass->PrettyClass() << ' ' << clinit->PrettyMethod(true);
      return false;
    }
    if (klass->HasSuperClass()) {
      ObjPtr<mirror::Class> super_class = klass->GetSuperClass();
      StackHandleScope<1> hs(self);
      Handle<mirror::Class> handle_scope_super(hs.NewHandle(super_class));
      if (!NoClinitInDependency(handle_scope_super, self, class_loader)) {
        return false;
      }
    }

    uint32_t num_if = klass->NumDirectInterfaces();
    for (size_t i = 0; i < num_if; i++) {
      ObjPtr<mirror::Class> interface = klass->GetDirectInterface(i);
      DCHECK(interface != nullptr);
      StackHandleScope<1> hs(self);
      Handle<mirror::Class> handle_interface(hs.NewHandle(interface));
      if (!NoClinitInDependency(handle_interface, self, class_loader)) {
        return false;
      }
    }

    return true;
  }

  const ParallelCompilationManager* const manager_;
};

void CompilerDriver::InitializeClasses(jobject jni_class_loader,
                                       const DexFile& dex_file,
                                       TimingLogger* timings) {
  TimingLogger::ScopedTiming t("Initialize Classes Dex File", timings);

  // Initialization allocates objects and needs to run single-threaded to be deterministic.
  bool force_determinism = GetCompilerOptions().IsForceDeterminism();
  ThreadPool* init_thread_pool = force_determinism
                                     ? single_thread_pool_.get()
                                     : parallel_thread_pool_.get();
  size_t init_thread_count = force_determinism ? 1U : parallel_thread_count_;

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  ParallelCompilationManager context(
      class_linker, jni_class_loader, this, &dex_file, init_thread_pool);

  if (GetCompilerOptions().IsBootImage() ||
      GetCompilerOptions().IsBootImageExtension() ||
      GetCompilerOptions().IsAppImage()) {
    // Set the concurrency thread to 1 to support initialization for images since transaction
    // doesn't support multithreading now.
    // TODO: remove this when transactional mode supports multithreading.
    init_thread_count = 1U;
  }
  InitializeClassVisitor visitor(&context);
  context.ForAll(0, dex_file.NumClassDefs(), &visitor, init_thread_count);

  // Make initialized classes visibly initialized.
  class_linker->MakeInitializedClassesVisiblyInitialized(Thread::Current(), /*wait=*/ true);
}

void CompilerDriver::InitializeClasses(jobject class_loader,
                                       const std::vector<const DexFile*>& dex_files,
                                       TimingLogger* timings) {
  TimingLogger::ScopedTiming t("Initialize Classes", timings);
  for (const DexFile* dex_file : dex_files) {
    CHECK(dex_file != nullptr);
    InitializeClasses(class_loader, *dex_file, timings);
  }
  if (GetCompilerOptions().IsBootImage() || GetCompilerOptions().IsBootImageExtension()) {
    // Prune garbage objects created during aborted transactions.
    Runtime::Current()->GetHeap()->CollectGarbage(/* clear_soft_references= */ true);
  }
}

template <typename CompileFn>
static void CompileDexFile(CompilerDriver* driver,
                           jobject class_loader,
                           const DexFile& dex_file,
                           ThreadPool* thread_pool,
                           size_t thread_count,
                           TimingLogger* timings,
                           const char* timing_name,
                           CompileFn compile_fn) {
  TimingLogger::ScopedTiming t(timing_name, timings);
  ParallelCompilationManager context(Runtime::Current()->GetClassLinker(),
                                     class_loader,
                                     driver,
                                     &dex_file,
                                     thread_pool);
  const CompilerOptions& compiler_options = driver->GetCompilerOptions();
  bool have_profile = (compiler_options.GetProfileCompilationInfo() != nullptr);
  bool use_profile = CompilerFilter::DependsOnProfile(compiler_options.GetCompilerFilter());
  ProfileCompilationInfo::ProfileIndexType profile_index = (have_profile && use_profile)
      ? compiler_options.GetProfileCompilationInfo()->FindDexFile(dex_file)
      : ProfileCompilationInfo::MaxProfileIndex();

  auto compile = [&context, &compile_fn, profile_index](size_t class_def_index) {
    const DexFile& dex_file = *context.GetDexFile();
    SCOPED_TRACE << "compile " << dex_file.GetLocation() << "@" << class_def_index;
    ClassLinker* class_linker = context.GetClassLinker();
    jobject jclass_loader = context.GetClassLoader();
    ClassReference ref(&dex_file, class_def_index);
    const dex::ClassDef& class_def = dex_file.GetClassDef(class_def_index);
    ClassAccessor accessor(dex_file, class_def_index);
    CompilerDriver* const driver = context.GetCompiler();
    // Skip compiling classes with generic verifier failures since they will still fail at runtime
    DCHECK(driver->GetVerificationResults() != nullptr);
    if (driver->GetVerificationResults()->IsClassRejected(ref)) {
      return;
    }
    // Use a scoped object access to perform to the quick SkipClass check.
    ScopedObjectAccess soa(Thread::Current());
    StackHandleScope<3> hs(soa.Self());
    Handle<mirror::ClassLoader> class_loader(
        hs.NewHandle(soa.Decode<mirror::ClassLoader>(jclass_loader)));
    Handle<mirror::Class> klass = hs.NewHandle(
        class_linker->FindClass(soa.Self(), dex_file, class_def.class_idx_, class_loader));
    Handle<mirror::DexCache> dex_cache;
    if (klass == nullptr) {
      soa.Self()->AssertPendingException();
      soa.Self()->ClearException();
      dex_cache = hs.NewHandle(class_linker->FindDexCache(soa.Self(), dex_file));
    } else if (SkipClass(jclass_loader, dex_file, klass.Get())) {
      // Skip a duplicate class (as the resolved class is from another, earlier dex file).
      return;  // Do not update state.
    } else {
      dex_cache = hs.NewHandle(klass->GetDexCache());
    }

    // Avoid suspension if there are no methods to compile.
    if (accessor.NumDirectMethods() + accessor.NumVirtualMethods() == 0) {
      return;
    }

    // Go to native so that we don't block GC during compilation.
    ScopedThreadSuspension sts(soa.Self(), ThreadState::kNative);

    // Compile direct and virtual methods.
    int64_t previous_method_idx = -1;
    for (const ClassAccessor::Method& method : accessor.GetMethods()) {
      const uint32_t method_idx = method.GetIndex();
      if (method_idx == previous_method_idx) {
        // smali can create dex files with two encoded_methods sharing the same method_idx
        // http://code.google.com/p/smali/issues/detail?id=119
        continue;
      }
      previous_method_idx = method_idx;
      compile_fn(soa.Self(),
                 driver,
                 method.GetCodeItem(),
                 method.GetAccessFlags(),
                 class_def_index,
                 method_idx,
                 class_loader,
                 dex_file,
                 dex_cache,
                 profile_index);
    }
  };
  context.ForAllLambda(0, dex_file.NumClassDefs(), compile, thread_count);
}

void CompilerDriver::Compile(jobject class_loader,
                             const std::vector<const DexFile*>& dex_files,
                             TimingLogger* timings) {
  TimingLogger::ScopedTiming t("Compile Methods", timings);
  if (kDebugProfileGuidedCompilation) {
    const ProfileCompilationInfo* profile_compilation_info =
        GetCompilerOptions().GetProfileCompilationInfo();
    LOG(INFO) << "[ProfileGuidedCompilation] " <<
        ((profile_compilation_info == nullptr)
            ? "null"
            : profile_compilation_info->DumpInfo(dex_files));
  }

  for (const DexFile* dex_file : dex_files) {
    CHECK(dex_file != nullptr);
    CompileDexFile(this,
                   class_loader,
                   *dex_file,
                   parallel_thread_pool_.get(),
                   parallel_thread_count_,
                   timings,
                   "Compile Dex File Quick",
                   CompileMethodQuick);
    const ArenaPool* const arena_pool = Runtime::Current()->GetArenaPool();
    const size_t arena_alloc = arena_pool->GetBytesAllocated();
    max_arena_alloc_ = std::max(arena_alloc, max_arena_alloc_);
    Runtime::Current()->ReclaimArenaPoolMemory();
  }

  VLOG(compiler) << "Compile: " << GetMemoryUsageString(false);
}

void CompilerDriver::AddCompiledMethod(const MethodReference& method_ref,
                                       CompiledMethod* const compiled_method) {
  DCHECK(GetCompiledMethod(method_ref) == nullptr) << method_ref.PrettyMethod();
  MethodTable::InsertResult result = compiled_methods_.Insert(method_ref,
                                                              /*expected*/ nullptr,
                                                              compiled_method);
  CHECK(result == MethodTable::kInsertResultSuccess);
  DCHECK(GetCompiledMethod(method_ref) != nullptr) << method_ref.PrettyMethod();
}

CompiledMethod* CompilerDriver::RemoveCompiledMethod(const MethodReference& method_ref) {
  CompiledMethod* ret = nullptr;
  CHECK(compiled_methods_.Remove(method_ref, &ret));
  return ret;
}

bool CompilerDriver::GetCompiledClass(const ClassReference& ref, ClassStatus* status) const {
  DCHECK(status != nullptr);
  // The table doesn't know if something wasn't inserted. For this case it will return
  // ClassStatus::kNotReady. To handle this, just assume anything we didn't try to verify
  // is not compiled.
  if (!compiled_classes_.Get(ref, status) ||
      *status < ClassStatus::kRetryVerificationAtRuntime) {
    return false;
  }
  return true;
}

ClassStatus CompilerDriver::GetClassStatus(const ClassReference& ref) const {
  ClassStatus status = ClassStatus::kNotReady;
  if (!GetCompiledClass(ref, &status)) {
    classpath_classes_.Get(ref, &status);
  }
  return status;
}

void CompilerDriver::RecordClassStatus(const ClassReference& ref, ClassStatus status) {
  switch (status) {
    case ClassStatus::kErrorResolved:
    case ClassStatus::kErrorUnresolved:
    case ClassStatus::kNotReady:
    case ClassStatus::kResolved:
    case ClassStatus::kRetryVerificationAtRuntime:
    case ClassStatus::kVerifiedNeedsAccessChecks:
    case ClassStatus::kVerified:
    case ClassStatus::kSuperclassValidated:
    case ClassStatus::kVisiblyInitialized:
      break;  // Expected states.
    default:
      LOG(FATAL) << "Unexpected class status for class "
          << PrettyDescriptor(
              ref.dex_file->GetClassDescriptor(ref.dex_file->GetClassDef(ref.index)))
          << " of " << status;
  }

  ClassStateTable::InsertResult result;
  ClassStateTable* table = &compiled_classes_;
  do {
    ClassStatus existing = ClassStatus::kNotReady;
    if (!table->Get(ref, &existing)) {
      // A classpath class.
      if (kIsDebugBuild) {
        // Check to make sure it's not a dex file for an oat file we are compiling since these
        // should always succeed. These do not include classes in for used libraries.
        for (const DexFile* dex_file : GetCompilerOptions().GetDexFilesForOatFile()) {
          CHECK_NE(ref.dex_file, dex_file) << ref.dex_file->GetLocation();
        }
      }
      if (!classpath_classes_.HaveDexFile(ref.dex_file)) {
        // Boot classpath dex file.
        return;
      }
      table = &classpath_classes_;
      table->Get(ref, &existing);
    }
    if (existing >= status) {
      // Existing status is already better than we expect, break.
      break;
    }
    // Update the status if we now have a greater one. This happens with vdex,
    // which records a class is verified, but does not resolve it.
    result = table->Insert(ref, existing, status);
    CHECK(result != ClassStateTable::kInsertResultInvalidDexFile) << ref.dex_file->GetLocation();
  } while (result != ClassStateTable::kInsertResultSuccess);
}

CompiledMethod* CompilerDriver::GetCompiledMethod(MethodReference ref) const {
  CompiledMethod* compiled_method = nullptr;
  compiled_methods_.Get(ref, &compiled_method);
  return compiled_method;
}

std::string CompilerDriver::GetMemoryUsageString(bool extended) const {
  std::ostringstream oss;
  const gc::Heap* const heap = Runtime::Current()->GetHeap();
  const size_t java_alloc = heap->GetBytesAllocated();
  oss << "arena alloc=" << PrettySize(max_arena_alloc_) << " (" << max_arena_alloc_ << "B)";
  oss << " java alloc=" << PrettySize(java_alloc) << " (" << java_alloc << "B)";
#if defined(__BIONIC__) || defined(__GLIBC__) || defined(ANDROID_HOST_MUSL)
  const struct mallinfo info = mallinfo();
  const size_t allocated_space = static_cast<size_t>(info.uordblks);
  const size_t free_space = static_cast<size_t>(info.fordblks);
  oss << " native alloc=" << PrettySize(allocated_space) << " (" << allocated_space << "B)"
      << " free=" << PrettySize(free_space) << " (" << free_space << "B)";
#endif
  compiled_method_storage_.DumpMemoryUsage(oss, extended);
  return oss.str();
}

void CompilerDriver::InitializeThreadPools() {
  size_t parallel_count = parallel_thread_count_ > 0 ? parallel_thread_count_ - 1 : 0;
  parallel_thread_pool_.reset(
      ThreadPool::Create("Compiler driver thread pool", parallel_count));
  single_thread_pool_.reset(ThreadPool::Create("Single-threaded Compiler driver thread pool", 0));
}

void CompilerDriver::FreeThreadPools() {
  parallel_thread_pool_.reset();
  single_thread_pool_.reset();
}

void CompilerDriver::SetClasspathDexFiles(const std::vector<const DexFile*>& dex_files) {
  classpath_classes_.AddDexFiles(dex_files);
}

}  // namespace art
