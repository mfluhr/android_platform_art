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

#ifndef ART_COMPILER_OPTIMIZING_OPTIMIZING_COMPILER_STATS_H_
#define ART_COMPILER_OPTIMIZING_OPTIMIZING_COMPILER_STATS_H_

#include <atomic>
#include <iomanip>
#include <string>
#include <type_traits>

#include <android-base/logging.h>

#include "base/atomic.h"
#include "base/globals.h"
#include "base/macros.h"

namespace art HIDDEN {

enum class MethodCompilationStat {
  kAttemptBytecodeCompilation = 0,
  kAttemptIntrinsicCompilation,
  kCompiledNativeStub,
  kCompiledIntrinsic,
  kCompiledBytecode,
  kCHAInline,
  kInlinedInvoke,
  kInlinedLastInvoke,
  kReplacedInvokeWithSimplePattern,
  kInstructionSimplifications,
  kInstructionSimplificationsArch,
  kUnresolvedMethod,
  kUnresolvedField,
  kUnresolvedFieldNotAFastAccess,
  kRemovedCheckedCast,
  kRemovedDeadInstruction,
  kRemovedDeadPhi,
  kRemovedTry,
  kRemovedNullCheck,
  kRemovedVolatileLoad,
  kRemovedVolatileStore,
  kRemovedMonitorOp,
  kNotCompiledSkipped,
  kNotCompiledInvalidBytecode,
  kNotCompiledThrowCatchLoop,
  kNotCompiledAmbiguousArrayOp,
  kNotCompiledHugeMethod,
  kNotCompiledMalformedOpcode,
  kNotCompiledNoCodegen,
  kNotCompiledPathological,
  kNotCompiledSpaceFilter,
  kNotCompiledUnhandledInstruction,
  kNotCompiledUnsupportedIsa,
  kNotCompiledInliningIrreducibleLoop,
  kNotCompiledIrreducibleLoopAndStringInit,
  kNotCompiledPhiEquivalentInOsr,
  kNotCompiledFrameTooBig,
  kInlinedMonomorphicCall,
  kInlinedPolymorphicCall,
  kMonomorphicCall,
  kPolymorphicCall,
  kMegamorphicCall,
  kBooleanSimplified,
  kIntrinsicRecognized,
  kLoopInvariantMoved,
  kLoopVectorized,
  kLoopVectorizedIdiom,
  kRemovedInstanceOf,
  kPropagatedIfValue,
  kInlinedInvokeVirtualOrInterface,
  kInlinedLastInvokeVirtualOrInterface,
  kImplicitNullCheckGenerated,
  kExplicitNullCheckGenerated,
  kControlFlowSelectGenerated,
  kControlFlowDiamondRemoved,
  kSimplifyIf,
  kSimplifyIfAddedPhi,
  kSimplifyThrowingInvoke,
  kInstructionSunk,
  kNotInlinedUnresolvedEntrypoint,
  kNotInlinedBss,
  kNotInlinedDexCacheInaccessibleToCaller,
  kNotInlinedDexCacheClinitCheck,
  kNotInlinedStackMaps,
  kNotInlinedEnvironmentBudget,
  kNotInlinedInstructionBudget,
  kNotInlinedLoopWithoutExit,
  kNotInlinedIrreducibleLoopCallee,
  kNotInlinedIrreducibleLoopCaller,
  kNotInlinedAlwaysThrows,
  kNotInlinedInfiniteLoop,
  kNotInlinedTryCatchCallee,
  kNotInlinedTryCatchDisabled,
  kNotInlinedRegisterAllocator,
  kNotInlinedCannotBuild,
  kNotInlinedNeverInlineAnnotation,
  kNotInlinedNotCompilable,
  kNotInlinedNotVerified,
  kNotInlinedCodeItem,
  kNotInlinedEndsWithThrow,
  kNotInlinedWont,
  kNotInlinedRecursiveBudget,
  kNotInlinedPolymorphicRecursiveBudget,
  kNotInlinedProxy,
  kNotInlinedUnresolved,
  kNotInlinedPolymorphic,
  kNotInlinedCustom,
  kNotVarAnalyzedPathological,
  kTryInline,
  kConstructorFenceGeneratedNew,
  kConstructorFenceGeneratedFinal,
  kConstructorFenceRemovedLSE,
  kConstructorFenceRemovedPFRA,
  kConstructorFenceRemovedCFRE,
  kPossibleWriteBarrier,
  kRemovedWriteBarrier,
  kBitstringTypeCheck,
  kJitOutOfMemoryForCommit,
  kFullLSEAllocationRemoved,
  kFullLSEPossible,
  kDevirtualized,
  kLastStat
};
std::ostream& operator<<(std::ostream& os, MethodCompilationStat rhs);

class OptimizingCompilerStats {
 public:
  OptimizingCompilerStats() {
    // The std::atomic<> default constructor leaves values uninitialized, so initialize them now.
    Reset();
  }

  void RecordStat(MethodCompilationStat stat, uint32_t count = 1) {
    size_t stat_index = static_cast<size_t>(stat);
    DCHECK_LT(stat_index, arraysize(compile_stats_));
    compile_stats_[stat_index] += count;
  }

  uint32_t GetStat(MethodCompilationStat stat) const {
    size_t stat_index = static_cast<size_t>(stat);
    DCHECK_LT(stat_index, arraysize(compile_stats_));
    return compile_stats_[stat_index];
  }

  void Log() const {
    uint32_t compiled_intrinsics = GetStat(MethodCompilationStat::kCompiledIntrinsic);
    uint32_t compiled_native_stubs = GetStat(MethodCompilationStat::kCompiledNativeStub);
    uint32_t bytecode_attempts =
        GetStat(MethodCompilationStat::kAttemptBytecodeCompilation);
    if (compiled_intrinsics == 0u && compiled_native_stubs == 0u && bytecode_attempts == 0u) {
      LOG(INFO) << "Did not compile any method.";
    } else {
      uint32_t compiled_bytecode_methods =
          GetStat(MethodCompilationStat::kCompiledBytecode);
      // Successful intrinsic compilation preempts other compilation attempts but failed intrinsic
      // compilation shall still count towards bytecode or native stub compilation attempts.
      uint32_t num_compilation_attempts =
          compiled_intrinsics + compiled_native_stubs + bytecode_attempts;
      uint32_t num_successful_compilations =
          compiled_intrinsics + compiled_native_stubs + compiled_bytecode_methods;
      float compiled_percent = num_successful_compilations * 100.0f / num_compilation_attempts;
      LOG(INFO) << "Attempted compilation of "
          << num_compilation_attempts << " methods: " << std::fixed << std::setprecision(2)
          << compiled_percent << "% (" << num_successful_compilations << ") compiled.";

      for (size_t i = 0; i < arraysize(compile_stats_); ++i) {
        if (compile_stats_[i] != 0) {
          LOG(INFO) << "OptStat#" << static_cast<MethodCompilationStat>(i) << ": "
              << compile_stats_[i];
        }
      }
    }
  }

  void AddTo(OptimizingCompilerStats* other_stats) {
    for (size_t i = 0; i != arraysize(compile_stats_); ++i) {
      uint32_t count = compile_stats_[i];
      if (count != 0) {
        other_stats->RecordStat(static_cast<MethodCompilationStat>(i), count);
      }
    }
  }

  void Reset() {
    for (std::atomic<uint32_t>& stat : compile_stats_) {
      stat = 0u;
    }
  }

 private:
  std::atomic<uint32_t> compile_stats_[static_cast<size_t>(MethodCompilationStat::kLastStat)];

  DISALLOW_COPY_AND_ASSIGN(OptimizingCompilerStats);
};

inline void MaybeRecordStat(OptimizingCompilerStats* compiler_stats,
                            MethodCompilationStat stat,
                            uint32_t count = 1) {
  if (compiler_stats != nullptr) {
    compiler_stats->RecordStat(stat, count);
  }
}

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_OPTIMIZING_COMPILER_STATS_H_
