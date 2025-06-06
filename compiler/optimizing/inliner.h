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

#ifndef ART_COMPILER_OPTIMIZING_INLINER_H_
#define ART_COMPILER_OPTIMIZING_INLINER_H_

#include "base/macros.h"
#include "dex/dex_file_types.h"
#include "dex/invoke_type.h"
#include "jit/profiling_info.h"
#include "optimization.h"
#include "profile/profile_compilation_info.h"

namespace art HIDDEN {

class CodeGenerator;
class DexCompilationUnit;
class HGraph;
class HInvoke;
class OptimizingCompilerStats;

class HInliner : public HOptimization {
 public:
  HInliner(HGraph* outer_graph,
           HGraph* outermost_graph,
           CodeGenerator* codegen,
           const DexCompilationUnit& outer_compilation_unit,
           const DexCompilationUnit& caller_compilation_unit,
           OptimizingCompilerStats* stats,
           size_t total_number_of_dex_registers,
           size_t total_number_of_instructions,
           HInliner* parent,
           HEnvironment* caller_environment,
           size_t depth,
           bool try_catch_inlining_allowed,
           const char* name = kInlinerPassName)
      : HOptimization(outer_graph, name, stats),
        outermost_graph_(outermost_graph),
        outer_compilation_unit_(outer_compilation_unit),
        caller_compilation_unit_(caller_compilation_unit),
        codegen_(codegen),
        total_number_of_dex_registers_(total_number_of_dex_registers),
        total_number_of_instructions_(total_number_of_instructions),
        parent_(parent),
        caller_environment_(caller_environment),
        depth_(depth),
        inlining_budget_(0),
        try_catch_inlining_allowed_(try_catch_inlining_allowed),
        run_extra_type_propagation_(false),
        inline_stats_(nullptr) {}

  bool Run() override;

  static constexpr const char* kInlinerPassName = "inliner";

  const HInliner* GetParent() const { return parent_; }
  const HEnvironment* GetCallerEnvironment() const { return caller_environment_; }

  const HGraph* GetOutermostGraph() const { return outermost_graph_; }
  const HGraph* GetGraph() const { return graph_; }

 private:
  enum InlineCacheType {
    kInlineCacheNoData = 0,
    kInlineCacheUninitialized = 1,
    kInlineCacheMonomorphic = 2,
    kInlineCachePolymorphic = 3,
    kInlineCacheMegamorphic = 4,
    kInlineCacheMissingTypes = 5
  };

  bool TryInline(HInvoke* invoke_instruction);

  // Try to inline `resolved_method` in place of `invoke_instruction`. `do_rtp` is whether
  // reference type propagation can run after the inlining. If the inlining is successful, this
  // method will replace and remove the `invoke_instruction`.
  bool TryInlineAndReplace(HInvoke* invoke_instruction,
                           ArtMethod* resolved_method,
                           ReferenceTypeInfo receiver_type,
                           bool do_rtp,
                           bool is_speculative)
    REQUIRES_SHARED(Locks::mutator_lock_);

  bool TryBuildAndInline(HInvoke* invoke_instruction,
                         ArtMethod* resolved_method,
                         ReferenceTypeInfo receiver_type,
                         HInstruction** return_replacement,
                         bool is_speculative)
    REQUIRES_SHARED(Locks::mutator_lock_);

  bool TryBuildAndInlineHelper(HInvoke* invoke_instruction,
                               ArtMethod* resolved_method,
                               ReferenceTypeInfo receiver_type,
                               HInstruction** return_replacement,
                               bool is_speculative)
    REQUIRES_SHARED(Locks::mutator_lock_);

  // Substitutes parameters in the callee graph with their values from the caller.
  void SubstituteArguments(HGraph* callee_graph,
                           HInvoke* invoke_instruction,
                           ReferenceTypeInfo receiver_type,
                           const DexCompilationUnit& dex_compilation_unit)
    REQUIRES_SHARED(Locks::mutator_lock_);

  // Run simple optimizations on `callee_graph`.
  void RunOptimizations(HGraph* callee_graph,
                        HEnvironment* caller_environment,
                        const dex::CodeItem* code_item,
                        const DexCompilationUnit& dex_compilation_unit,
                        bool try_catch_inlining_allowed_for_recursive_inline)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Try to recognize known simple patterns and replace invoke call with appropriate instructions.
  bool TryPatternSubstitution(HInvoke* invoke_instruction,
                              ArtMethod* method,
                              const CodeItemDataAccessor& accessor,
                              HInstruction** return_replacement)
    REQUIRES_SHARED(Locks::mutator_lock_);

  // Returns whether inlining is allowed based on ART semantics.
  bool IsInliningAllowed(art::ArtMethod* method, const CodeItemDataAccessor& accessor) const
    REQUIRES_SHARED(Locks::mutator_lock_);


  // Returns whether ART supports inlining this method.
  //
  // Some methods are not supported because they have features for which inlining
  // is not implemented. For example, we do not currently support inlining throw
  // instructions into a try block.
  bool IsInliningSupported(const HInvoke* invoke_instruction,
                           art::ArtMethod* method,
                           const CodeItemDataAccessor& accessor) const
    REQUIRES_SHARED(Locks::mutator_lock_);

  // Returns whether inlining is encouraged.
  //
  // For example, this checks whether the function has grown too large and
  // inlining should be prevented.
  bool IsInliningEncouraged(const HInvoke* invoke_instruction,
                            art::ArtMethod* method,
                            const CodeItemDataAccessor& accessor) const
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Inspects the body of a method (callee_graph) and returns whether it can be
  // inlined.
  //
  // This checks for instructions and constructs that we do not support
  // inlining, such as inlining a throw instruction into a try block.
  bool CanInlineBody(const HGraph* callee_graph,
                     HInvoke* invoke,
                     size_t* out_number_of_instructions,
                     bool is_speculative) const
    REQUIRES_SHARED(Locks::mutator_lock_);

  // Create a new HInstanceFieldGet.
  HInstanceFieldGet* CreateInstanceFieldGet(uint32_t field_index,
                                            ArtMethod* referrer,
                                            HInstruction* obj);
  // Create a new HInstanceFieldSet.
  HInstanceFieldSet* CreateInstanceFieldSet(uint32_t field_index,
                                            ArtMethod* referrer,
                                            HInstruction* obj,
                                            HInstruction* value,
                                            bool* is_final = nullptr);

  // Try inlining the invoke instruction using inline caches.
  bool TryInlineFromInlineCache(HInvoke* invoke_instruction)
    REQUIRES_SHARED(Locks::mutator_lock_);

  // Try inlining the invoke instruction using CHA.
  bool TryInlineFromCHA(HInvoke* invoke_instruction)
    REQUIRES_SHARED(Locks::mutator_lock_);

  // When we fail inlining `invoke_instruction`, we will try to devirtualize the
  // call.
  bool TryDevirtualize(HInvoke* invoke_instruction,
                       ArtMethod* method,
                       HInvoke** replacement)
    REQUIRES_SHARED(Locks::mutator_lock_);

  // Try getting the inline cache from JIT code cache.
  // Return true if the inline cache was successfully allocated and the
  // invoke info was found in the profile info.
  InlineCacheType GetInlineCacheJIT(
      HInvoke* invoke_instruction,
      /*out*/StackHandleScope<InlineCache::kIndividualCacheSize>* classes)
    REQUIRES_SHARED(Locks::mutator_lock_);

  // Try getting the inline cache from AOT offline profile.
  // Return true if the inline cache was successfully allocated and the
  // invoke info was found in the profile info.
  InlineCacheType GetInlineCacheAOT(
      HInvoke* invoke_instruction,
      /*out*/StackHandleScope<InlineCache::kIndividualCacheSize>* classes)
    REQUIRES_SHARED(Locks::mutator_lock_);

  // Compute the inline cache type.
  static InlineCacheType GetInlineCacheType(
      const StackHandleScope<InlineCache::kIndividualCacheSize>& classes)
    REQUIRES_SHARED(Locks::mutator_lock_);

  // Try to inline the target of a monomorphic call. If successful, the code
  // in the graph will look like:
  // if (receiver.getClass() != ic.GetMonomorphicType()) deopt
  // ... // inlined code
  bool TryInlineMonomorphicCall(HInvoke* invoke_instruction,
                                const StackHandleScope<InlineCache::kIndividualCacheSize>& classes)
    REQUIRES_SHARED(Locks::mutator_lock_);

  // Try to inline targets of a polymorphic call.
  bool TryInlinePolymorphicCall(HInvoke* invoke_instruction,
                                const StackHandleScope<InlineCache::kIndividualCacheSize>& classes)
    REQUIRES_SHARED(Locks::mutator_lock_);

  bool TryInlinePolymorphicCallToSameTarget(
      HInvoke* invoke_instruction,
      const StackHandleScope<InlineCache::kIndividualCacheSize>& classes)
    REQUIRES_SHARED(Locks::mutator_lock_);

  // Returns whether or not we should use only polymorphic inlining with no deoptimizations.
  bool UseOnlyPolymorphicInliningWithNoDeopt();

  // Try CHA-based devirtualization to change virtual method calls into
  // direct calls.
  // Returns the actual method that resolved_method can be devirtualized to.
  ArtMethod* FindMethodFromCHA(ArtMethod* resolved_method)
    REQUIRES_SHARED(Locks::mutator_lock_);

  // Add a CHA guard for a CHA-based devirtualized call. A CHA guard checks a
  // should_deoptimize flag and if it's true, does deoptimization.
  void AddCHAGuard(HInstruction* invoke_instruction,
                   uint32_t dex_pc,
                   HInstruction* cursor,
                   HBasicBlock* bb_cursor);

  HInstanceFieldGet* BuildGetReceiverClass(HInstruction* receiver, uint32_t dex_pc) const
    REQUIRES_SHARED(Locks::mutator_lock_);

  void MaybeRunReferenceTypePropagation(HInstruction* replacement,
                                        HInvoke* invoke_instruction)
    REQUIRES_SHARED(Locks::mutator_lock_);

  void FixUpReturnReferenceType(ArtMethod* resolved_method, HInstruction* return_replacement)
    REQUIRES_SHARED(Locks::mutator_lock_);

  bool ArgumentTypesMoreSpecific(HInvoke* invoke_instruction, ArtMethod* resolved_method)
    REQUIRES_SHARED(Locks::mutator_lock_);

  bool ReturnTypeMoreSpecific(HInstruction* return_replacement, HInvoke* invoke_instruction)
    REQUIRES_SHARED(Locks::mutator_lock_);

  // Add a type guard on the given `receiver`. This will add to the graph:
  // i0 = HFieldGet(receiver, klass)
  // i1 = HLoadClass(class_index, is_referrer)
  // i2 = HNotEqual(i0, i1)
  //
  // And if `with_deoptimization` is true:
  // HDeoptimize(i2)
  //
  // The method returns the `HNotEqual`, that will be used for polymorphic inlining.
  HInstruction* AddTypeGuard(HInstruction* receiver,
                             HInstruction* cursor,
                             HBasicBlock* bb_cursor,
                             dex::TypeIndex class_index,
                             Handle<mirror::Class> klass,
                             HInstruction* invoke_instruction,
                             bool with_deoptimization)
    REQUIRES_SHARED(Locks::mutator_lock_);

  /*
   * Ad-hoc implementation for implementing a diamond pattern in the graph for
   * polymorphic inlining:
   * 1) `compare` becomes the input of the new `HIf`.
   * 2) Everything up until `invoke_instruction` is in the then branch (could
   *    contain multiple blocks).
   * 3) `invoke_instruction` is moved to the otherwise block.
   * 4) If `return_replacement` is not null, the merge block will have
   *    a phi whose inputs are `return_replacement` and `invoke_instruction`.
   *
   * Before:
   *             Block1
   *             compare
   *              ...
   *         invoke_instruction
   *
   * After:
   *            Block1
   *            compare
   *              if
   *          /        \
   *         /          \
   *   Then block    Otherwise block
   *      ...       invoke_instruction
   *       \              /
   *        \            /
   *          Merge block
   *  phi(return_replacement, invoke_instruction)
   */
  void CreateDiamondPatternForPolymorphicInline(HInstruction* compare,
                                                HInstruction* return_replacement,
                                                HInstruction* invoke_instruction);

  // Update the inlining budget based on `total_number_of_instructions_`.
  void UpdateInliningBudget();

  // Count the number of calls of `method` being inlined recursively.
  size_t CountRecursiveCallsOf(ArtMethod* method) const;

  // Pretty-print for spaces during logging.
  std::string DepthString(int line) const;

  HGraph* const outermost_graph_;
  const DexCompilationUnit& outer_compilation_unit_;
  const DexCompilationUnit& caller_compilation_unit_;
  CodeGenerator* const codegen_;
  const size_t total_number_of_dex_registers_;
  size_t total_number_of_instructions_;

  // The 'parent' inliner, that means the inlining optimization that requested
  // `graph_` to be inlined.
  const HInliner* const parent_;
  const HEnvironment* const caller_environment_;
  const size_t depth_;

  // The budget left for inlining, in number of instructions.
  size_t inlining_budget_;

  // States if we are allowing try catch inlining to occur at this particular instance of inlining.
  bool try_catch_inlining_allowed_;

  // True if we need to run type propagation to type guards we inserted.
  bool run_extra_type_propagation_;

  // Used to record stats about optimizations on the inlined graph.
  // If the inlining is successful, these stats are merged to the caller graph's stats.
  OptimizingCompilerStats* inline_stats_;

  DISALLOW_COPY_AND_ASSIGN(HInliner);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_INLINER_H_
