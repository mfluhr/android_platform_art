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

#include "prepare_for_register_allocation.h"

#include "dex/dex_file_types.h"
#include "driver/compiler_options.h"
#include "jni/jni_internal.h"
#include "nodes.h"
#include "optimizing_compiler_stats.h"
#include "well_known_classes.h"

namespace art HIDDEN {

class PrepareForRegisterAllocationVisitor final : public HGraphDelegateVisitor {
 public:
  PrepareForRegisterAllocationVisitor(HGraph* graph,
                                      const CompilerOptions& compiler_options,
                                      OptimizingCompilerStats* stats)
      : HGraphDelegateVisitor(graph, stats),
        compiler_options_(compiler_options) {}

 private:
  void VisitCheckCast(HCheckCast* check_cast) override;
  void VisitInstanceOf(HInstanceOf* instance_of) override;
  void VisitNullCheck(HNullCheck* check) override;
  void VisitDivZeroCheck(HDivZeroCheck* check) override;
  void VisitBoundsCheck(HBoundsCheck* check) override;
  void VisitBoundType(HBoundType* bound_type) override;
  void VisitArraySet(HArraySet* instruction) override;
  void VisitClinitCheck(HClinitCheck* check) override;
  void VisitIf(HIf* if_instr) override;
  void VisitSelect(HSelect* select) override;
  void VisitConstructorFence(HConstructorFence* constructor_fence) override;
  void VisitInvokeStaticOrDirect(HInvokeStaticOrDirect* invoke) override;
  void VisitDeoptimize(HDeoptimize* deoptimize) override;
  void VisitTypeConversion(HTypeConversion* instruction) override;

  bool CanMoveClinitCheck(HInstruction* input, HInstruction* user) const;
  bool CanEmitConditionAt(HCondition* condition, HInstruction* user) const;
  void TryToMoveConditionToUser(HInstruction* maybe_condition, HInstruction* user);

  const CompilerOptions& compiler_options_;
};

bool PrepareForRegisterAllocation::Run() {
  PrepareForRegisterAllocationVisitor visitor(graph_, compiler_options_, stats_);
  // Order does not matter.
  for (HBasicBlock* block : graph_->GetReversePostOrder()) {
    // No need to visit the phis.
    for (HInstructionIteratorHandleChanges inst_it(block->GetInstructions()); !inst_it.Done();
         inst_it.Advance()) {
      inst_it.Current()->Accept(&visitor);
    }
  }
  return true;
}

void PrepareForRegisterAllocationVisitor::VisitCheckCast(HCheckCast* check_cast) {
  // Record only those bitstring type checks that make it to the codegen stage.
  if (check_cast->GetTypeCheckKind() == TypeCheckKind::kBitstringCheck) {
    MaybeRecordStat(stats_, MethodCompilationStat::kBitstringTypeCheck);
  }
}

void PrepareForRegisterAllocationVisitor::VisitInstanceOf(HInstanceOf* instance_of) {
  // Record only those bitstring type checks that make it to the codegen stage.
  if (instance_of->GetTypeCheckKind() == TypeCheckKind::kBitstringCheck) {
    MaybeRecordStat(stats_, MethodCompilationStat::kBitstringTypeCheck);
  }
}

void PrepareForRegisterAllocationVisitor::VisitNullCheck(HNullCheck* check) {
  check->ReplaceWith(check->InputAt(0));
  if (compiler_options_.GetImplicitNullChecks()) {
    HInstruction* next = check->GetNext();

    // The `PrepareForRegisterAllocation` pass removes `HBoundType` from the graph,
    // so do it ourselves now to not prevent optimizations.
    while (next->IsBoundType()) {
      next = next->GetNext();
      VisitBoundType(next->GetPrevious()->AsBoundType());
    }
    if (next->CanDoImplicitNullCheckOn(check->InputAt(0))) {
      check->MarkEmittedAtUseSite();
    }
  }
}

void PrepareForRegisterAllocationVisitor::VisitDivZeroCheck(HDivZeroCheck* check) {
  check->ReplaceWith(check->InputAt(0));
}

void PrepareForRegisterAllocationVisitor::VisitDeoptimize(HDeoptimize* deoptimize) {
  if (deoptimize->GuardsAnInput()) {
    // Replace the uses with the actual guarded instruction.
    deoptimize->ReplaceWith(deoptimize->GuardedInput());
    deoptimize->RemoveGuard();
  }
  TryToMoveConditionToUser(deoptimize->InputAt(0), deoptimize);
}

void PrepareForRegisterAllocationVisitor::VisitBoundsCheck(HBoundsCheck* check) {
  check->ReplaceWith(check->InputAt(0));
  if (check->IsStringCharAt()) {
    // Add a fake environment for String.charAt() inline info as we want the exception
    // to appear as being thrown from there. Skip if we're compiling String.charAt() itself.
    ArtMethod* char_at_method = WellKnownClasses::java_lang_String_charAt;
    if (GetGraph()->GetArtMethod() != char_at_method) {
      ArenaAllocator* allocator = GetGraph()->GetAllocator();
      HEnvironment* environment = HEnvironment::Create(allocator,
                                                       /* number_of_vregs= */ 0u,
                                                       char_at_method,
                                                       /* dex_pc= */ dex::kDexNoIndex,
                                                       check);
      check->InsertRawEnvironment(environment);
    }
  }
}

void PrepareForRegisterAllocationVisitor::VisitBoundType(HBoundType* bound_type) {
  bound_type->ReplaceWith(bound_type->InputAt(0));
  bound_type->GetBlock()->RemoveInstruction(bound_type);
}

void PrepareForRegisterAllocationVisitor::VisitArraySet(HArraySet* instruction) {
  HInstruction* value = instruction->GetValue();
  // PrepareForRegisterAllocationVisitor::VisitBoundType may have replaced a
  // BoundType (as value input of this ArraySet) with a NullConstant.
  // If so, this ArraySet no longer needs a type check.
  if (value->IsNullConstant()) {
    DCHECK_EQ(value->GetType(), DataType::Type::kReference);
    if (instruction->NeedsTypeCheck()) {
      instruction->ClearTypeCheck();
    }
  }
}

void PrepareForRegisterAllocationVisitor::VisitClinitCheck(HClinitCheck* check) {
  // Try to find a static invoke or a new-instance from which this check originated.
  HInstruction* implicit_clinit = nullptr;
  for (const HUseListNode<HInstruction*>& use : check->GetUses()) {
    HInstruction* user = use.GetUser();
    if ((user->IsInvokeStaticOrDirect() || user->IsNewInstance()) &&
        CanMoveClinitCheck(check, user)) {
      implicit_clinit = user;
      if (user->IsInvokeStaticOrDirect()) {
        DCHECK(user->AsInvokeStaticOrDirect()->IsStaticWithExplicitClinitCheck());
        user->AsInvokeStaticOrDirect()->RemoveExplicitClinitCheck(
            HInvokeStaticOrDirect::ClinitCheckRequirement::kImplicit);
      } else {
        DCHECK(user->IsNewInstance());
        // We delegate the initialization duty to the allocation.
        if (user->AsNewInstance()->GetEntrypoint() == kQuickAllocObjectInitialized) {
          user->AsNewInstance()->SetEntrypoint(kQuickAllocObjectResolved);
        }
      }
      break;
    }
  }
  // If we found a static invoke or new-instance for merging, remove the check
  // from dominated static invokes.
  if (implicit_clinit != nullptr) {
    const HUseList<HInstruction*>& uses = check->GetUses();
    for (auto it = uses.begin(), end = uses.end(); it != end; /* ++it below */) {
      HInstruction* user = it->GetUser();
      // All other uses must be dominated.
      DCHECK(implicit_clinit->StrictlyDominates(user) || (implicit_clinit == user));
      ++it;  // Advance before we remove the node, reference to the next node is preserved.
      if (user->IsInvokeStaticOrDirect()) {
        user->AsInvokeStaticOrDirect()->RemoveExplicitClinitCheck(
            HInvokeStaticOrDirect::ClinitCheckRequirement::kNone);
      }
    }
  }

  HLoadClass* load_class = check->GetLoadClass();
  bool can_merge_with_load_class = CanMoveClinitCheck(load_class, check);

  check->ReplaceWith(load_class);

  if (implicit_clinit != nullptr) {
    // Remove the check from the graph. It has been merged into the invoke or new-instance.
    check->GetBlock()->RemoveInstruction(check);
    // Check if we can merge the load class as well, or whether the LoadClass is now dead.
    if ((can_merge_with_load_class || !load_class->CanThrow()) && !load_class->HasUses()) {
      load_class->GetBlock()->RemoveInstruction(load_class);
    }
  } else if (can_merge_with_load_class &&
             load_class->GetLoadKind() != HLoadClass::LoadKind::kRuntimeCall) {
    // Pass the initialization duty to the `HLoadClass` instruction,
    // and remove the instruction from the graph.
    DCHECK(load_class->HasEnvironment());
    load_class->SetMustGenerateClinitCheck(true);
    check->GetBlock()->RemoveInstruction(check);
  }
}

// Determine if moving `condition` to `user` would observably extend the lifetime of a reference.
// By "observably" we understand that the reference would need to be visible to the GC for longer.
// We're not concerned with the lifetime for the purposes of register allocation here.
static bool ConditionMoveWouldExtendReferenceLifetime(HCondition* condition, HInstruction* user) {
  HInstruction* lhs = condition->InputAt(0);
  if (lhs->GetType() != DataType::Type::kReference) {
    return false;
  }
  HInstruction* rhs = condition->InputAt(1);
  DCHECK_EQ(rhs->GetType(), DataType::Type::kReference);
  if (lhs->IsNullConstant() && rhs->IsNullConstant()) {
    return false;
  }
  // Check if the last instruction with environment before `user` has all non-null
  // inputs in the environment. If so, we would not be extending the lifetime.
  HInstruction* instruction_with_env = user->GetPrevious();
  while (instruction_with_env != nullptr &&
         instruction_with_env != condition &&
         instruction_with_env->GetEnvironment() == nullptr) {
    DCHECK(!instruction_with_env->GetSideEffects().Includes(SideEffects::CanTriggerGC()));
    instruction_with_env = instruction_with_env->GetPrevious();
  }
  if (instruction_with_env == nullptr) {
    // No env use in the user's block. Do not search other blocks. Conservatively assume that
    // moving the `condition` to the `user` would indeed extend the lifetime of a reference.
    return true;
  }
  if (instruction_with_env == condition) {
    // There is no instruction with an environment between `condition` and `user`, so moving
    // the condition before the user shall not observably extend the lifetime of the reference.
    return false;
  }
  DCHECK(instruction_with_env->HasEnvironment());
  auto env_inputs = instruction_with_env->GetEnvironment()->GetEnvInputs();
  auto extends_lifetime = [&](HInstruction* instruction) {
    return !instruction->IsNullConstant() &&
           std::find(env_inputs.begin(), env_inputs.end(), instruction) == env_inputs.end();
  };
  return extends_lifetime(lhs) || extends_lifetime(rhs);
}

bool PrepareForRegisterAllocationVisitor::CanEmitConditionAt(HCondition* condition,
                                                             HInstruction* user) const {
  DCHECK(user->IsIf() || user->IsDeoptimize() || user->IsSelect());

  if (GetGraph()->IsCompilingBaseline() && compiler_options_.ProfileBranches()) {
    // To do branch profiling, we cannot emit conditions at use site.
    return false;
  }

  // Move only a single-user `HCondition` to the `user`.
  if (!condition->HasOnlyOneNonEnvironmentUse()) {
    return false;
  }
  DCHECK(condition->GetUses().front().GetUser() == user);

  if (condition->GetNext() != user) {
    // Avoid moving across blocks if the graph has any irreducible loops.
    if (condition->GetBlock() != user->GetBlock() && GetGraph()->HasIrreducibleLoops()) {
      return false;
    }
    // Avoid extending the lifetime of references by moving the condition.
    if (ConditionMoveWouldExtendReferenceLifetime(condition, user)) {
      return false;
    }
  }

  return true;
}

void PrepareForRegisterAllocationVisitor::TryToMoveConditionToUser(HInstruction* maybe_condition,
                                                                   HInstruction* user) {
  DCHECK(user->IsIf() || user->IsDeoptimize() || user->IsSelect());
  if (maybe_condition->IsCondition() && CanEmitConditionAt(maybe_condition->AsCondition(), user)) {
    if (maybe_condition->GetNext() != user) {
      maybe_condition->MoveBefore(user);
#ifdef ART_ENABLE_CODEGEN_x86
      for (HInstruction* input : maybe_condition->GetInputs()) {
        if (input->IsEmittedAtUseSite()) {
          DCHECK(input->IsX86LoadFromConstantTable());
          input->MoveBefore(maybe_condition);
          HInstruction* inputs_input = input->InputAt(0);
          DCHECK(inputs_input->IsX86ComputeBaseMethodAddress());
          if (inputs_input->HasOnlyOneNonEnvironmentUse()) {
            inputs_input->MoveBefore(input);
          }
        }
      }
#else  // ART_ENABLE_CODEGEN_x86
      if (kIsDebugBuild) {
        for (HInstruction* input : maybe_condition->GetInputs()) {
          CHECK(!input->IsEmittedAtUseSite()) << input->DebugName() << "#" << input->GetId();
        }
      }
#endif
    }
    maybe_condition->MarkEmittedAtUseSite();
  }
}

void PrepareForRegisterAllocationVisitor::VisitIf(HIf* if_instr) {
  TryToMoveConditionToUser(if_instr->InputAt(0), if_instr);
}

void PrepareForRegisterAllocationVisitor::VisitSelect(HSelect* select) {
  TryToMoveConditionToUser(select->GetCondition(), select);
}

void PrepareForRegisterAllocationVisitor::VisitConstructorFence(
    HConstructorFence* constructor_fence) {
  // Trivially remove redundant HConstructorFence when it immediately follows an HNewInstance
  // to an uninitialized class. In this special case, the art_quick_alloc_object_resolved
  // will already have the 'dmb' which is strictly stronger than an HConstructorFence.
  //
  // The instruction builder always emits "x = HNewInstance; HConstructorFence(x)" so this
  // is effectively pattern-matching that particular case and undoing the redundancy the builder
  // had introduced.
  //
  // TODO: Move this to a separate pass.
  HInstruction* allocation_inst = constructor_fence->GetAssociatedAllocation();
  if (allocation_inst != nullptr && allocation_inst->IsNewInstance()) {
    HNewInstance* new_inst = allocation_inst->AsNewInstance();
    // This relies on the entrypoint already being set to the more optimized version;
    // as that happens in this pass, this redundancy removal also cannot happen any earlier.
    if (new_inst != nullptr && new_inst->GetEntrypoint() == kQuickAllocObjectResolved) {
      // If this was done in an earlier pass, we would want to match that `previous` was an input
      // to the `constructor_fence`. However, since this pass removes the inputs to the fence,
      // we can ignore the inputs and just remove the instruction from its block.
      DCHECK_EQ(1u, constructor_fence->InputCount());
      // TODO: GetAssociatedAllocation should not care about multiple inputs
      // if we are in prepare_for_register_allocation pass only.
      constructor_fence->GetBlock()->RemoveInstruction(constructor_fence);
      MaybeRecordStat(stats_,
                      MethodCompilationStat::kConstructorFenceRemovedPFRA);
      return;
    }

    // HNewArray does not need this check because the art_quick_alloc_array does not itself
    // have a dmb in any normal situation (i.e. the array class is never exactly in the
    // "resolved" state). If the array class is not yet loaded, it will always go from
    // Unloaded->Initialized state.
  }

  // Remove all the inputs to the constructor fence;
  // they aren't used by the InstructionCodeGenerator and this lets us avoid creating a
  // LocationSummary in the LocationsBuilder.
  constructor_fence->RemoveAllInputs();
}

void PrepareForRegisterAllocationVisitor::VisitInvokeStaticOrDirect(
    HInvokeStaticOrDirect* invoke) {
  if (invoke->IsStaticWithExplicitClinitCheck()) {
    HInstruction* last_input = invoke->GetInputs().back();
    DCHECK(last_input->IsLoadClass())
        << "Last input is not HLoadClass. It is " << last_input->DebugName();

    // Detach the explicit class initialization check from the invoke.
    // Keeping track of the initializing instruction is no longer required
    // at this stage (i.e., after inlining has been performed).
    invoke->RemoveExplicitClinitCheck(HInvokeStaticOrDirect::ClinitCheckRequirement::kNone);

    // Merging with load class should have happened in VisitClinitCheck().
    DCHECK(!CanMoveClinitCheck(last_input, invoke));
  }
}

bool PrepareForRegisterAllocationVisitor::CanMoveClinitCheck(HInstruction* input,
                                                             HInstruction* user) const {
  // Determine if input and user come from the same dex instruction, so that we can move
  // the clinit check responsibility from one to the other, i.e. from HClinitCheck (user)
  // to HLoadClass (input), or from HClinitCheck (input) to HInvokeStaticOrDirect (user),
  // or from HLoadClass (input) to HNewInstance (user).

  // Start with a quick dex pc check.
  if (user->GetDexPc() != input->GetDexPc()) {
    return false;
  }

  if (user->IsNewInstance() && user->AsNewInstance()->IsPartialMaterialization()) {
    return false;
  }

  // Now do a thorough environment check that this is really coming from the same instruction in
  // the same inlined graph. Unfortunately, we have to go through the whole environment chain.
  HEnvironment* user_environment = user->GetEnvironment();
  HEnvironment* input_environment = input->GetEnvironment();
  while (user_environment != nullptr || input_environment != nullptr) {
    if (user_environment == nullptr || input_environment == nullptr) {
      // Different environment chain length. This happens when a method is called
      // once directly and once indirectly through another inlined method.
      return false;
    }
    if (user_environment->GetDexPc() != input_environment->GetDexPc() ||
        user_environment->GetMethod() != input_environment->GetMethod()) {
      return false;
    }
    user_environment = user_environment->GetParent();
    input_environment = input_environment->GetParent();
  }

  // Check for code motion taking the input to a different block.
  if (user->GetBlock() != input->GetBlock()) {
    return false;
  }

  // If there's a instruction between them that can throw or it has side effects, we cannot move the
  // responsibility.
  for (HInstruction* between = input->GetNext(); between != user; between = between->GetNext()) {
    DCHECK(between != nullptr) << " User must be after input in the same block. input: " << *input
                               << ", user: " << *user;
    if (between->CanThrow() || between->HasSideEffects()) {
      return false;
    }
  }

  return true;
}

void PrepareForRegisterAllocationVisitor::VisitTypeConversion(HTypeConversion* instruction) {
  // For simplicity, our code generators don't handle implicit type conversion, so ensure
  // there are none before hitting codegen.
  if (instruction->IsImplicitConversion()) {
    instruction->ReplaceWith(instruction->GetInput());
    instruction->GetBlock()->RemoveInstruction(instruction);
  }
}

}  // namespace art
