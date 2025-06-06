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

#include "instruction_simplifier.h"

#include "art_method-inl.h"
#include "class_linker-inl.h"
#include "class_root-inl.h"
#include "data_type-inl.h"
#include "driver/compiler_options.h"
#include "escape.h"
#include "intrinsic_objects.h"
#include "intrinsics.h"
#include "intrinsics_utils.h"
#include "mirror/class-inl.h"
#include "optimizing/data_type.h"
#include "optimizing/nodes.h"
#include "scoped_thread_state_change-inl.h"
#include "sharpening.h"
#include "string_builder_append.h"
#include "well_known_classes.h"

namespace art HIDDEN {

// Whether to run an exhaustive test of individual HInstructions cloning when each instruction
// is replaced with its copy if it is clonable.
static constexpr bool kTestInstructionClonerExhaustively = false;

class InstructionSimplifierVisitor final : public HGraphDelegateVisitor {
 public:
  InstructionSimplifierVisitor(HGraph* graph,
                               CodeGenerator* codegen,
                               OptimizingCompilerStats* stats,
                               bool be_loop_friendly)
      : HGraphDelegateVisitor(graph),
        codegen_(codegen),
        stats_(stats),
        be_loop_friendly_(be_loop_friendly) {}

  bool Run();

 private:
  void RecordSimplification() {
    simplification_occurred_ = true;
    simplifications_at_current_position_++;
    MaybeRecordStat(stats_, MethodCompilationStat::kInstructionSimplifications);
  }

  bool ReplaceRotateWithRor(HBinaryOperation* op, HUShr* ushr, HShl* shl);
  bool TryReplaceWithRotate(HBinaryOperation* instruction);
  bool TryReplaceWithRotateConstantPattern(HBinaryOperation* op, HUShr* ushr, HShl* shl);
  bool TryReplaceWithRotateRegisterNegPattern(HBinaryOperation* op, HUShr* ushr, HShl* shl);
  bool TryReplaceWithRotateRegisterSubPattern(HBinaryOperation* op, HUShr* ushr, HShl* shl);

  bool TryMoveNegOnInputsAfterBinop(HBinaryOperation* binop);
  // `op` should be either HOr or HAnd.
  // De Morgan's laws:
  // ~a & ~b = ~(a | b)  and  ~a | ~b = ~(a & b)
  bool TryDeMorganNegationFactoring(HBinaryOperation* op);
  bool TryHandleAssociativeAndCommutativeOperation(HBinaryOperation* instruction);
  bool TrySubtractionChainSimplification(HBinaryOperation* instruction);
  bool TryCombineVecMultiplyAccumulate(HVecMul* mul);
  void TryToReuseDiv(HRem* rem);

  void VisitShift(HBinaryOperation* shift);
  void VisitEqual(HEqual* equal) override;
  void VisitNotEqual(HNotEqual* equal) override;
  void VisitBooleanNot(HBooleanNot* bool_not) override;
  void VisitInstanceFieldSet(HInstanceFieldSet* equal) override;
  void VisitStaticFieldSet(HStaticFieldSet* equal) override;
  void VisitArraySet(HArraySet* equal) override;
  void VisitTypeConversion(HTypeConversion* instruction) override;
  void VisitNullCheck(HNullCheck* instruction) override;
  void VisitArrayLength(HArrayLength* instruction) override;
  void VisitCheckCast(HCheckCast* instruction) override;
  void VisitAbs(HAbs* instruction) override;
  void VisitAdd(HAdd* instruction) override;
  void VisitAnd(HAnd* instruction) override;
  void VisitCompare(HCompare* instruction) override;
  void VisitCondition(HCondition* instruction) override;
  void VisitGreaterThan(HGreaterThan* condition) override;
  void VisitGreaterThanOrEqual(HGreaterThanOrEqual* condition) override;
  void VisitLessThan(HLessThan* condition) override;
  void VisitLessThanOrEqual(HLessThanOrEqual* condition) override;
  void VisitBelow(HBelow* condition) override;
  void VisitBelowOrEqual(HBelowOrEqual* condition) override;
  void VisitAbove(HAbove* condition) override;
  void VisitAboveOrEqual(HAboveOrEqual* condition) override;
  void VisitDiv(HDiv* instruction) override;
  void VisitRem(HRem* instruction) override;
  void VisitMul(HMul* instruction) override;
  void VisitNeg(HNeg* instruction) override;
  void VisitNot(HNot* instruction) override;
  void VisitOr(HOr* instruction) override;
  void VisitShl(HShl* instruction) override;
  void VisitShr(HShr* instruction) override;
  void VisitSub(HSub* instruction) override;
  void VisitUShr(HUShr* instruction) override;
  void VisitXor(HXor* instruction) override;
  void VisitSelect(HSelect* select) override;
  void VisitIf(HIf* instruction) override;
  void VisitInstanceOf(HInstanceOf* instruction) override;
  void VisitInvoke(HInvoke* invoke) override;
  void VisitDeoptimize(HDeoptimize* deoptimize) override;
  void VisitVecMul(HVecMul* instruction) override;
  void SimplifyBoxUnbox(HInvoke* instruction, ArtField* field, DataType::Type type);
  void SimplifySystemArrayCopy(HInvoke* invoke);
  void SimplifyStringEquals(HInvoke* invoke);
  void SimplifyFP2Int(HInvoke* invoke);
  void SimplifyStringCharAt(HInvoke* invoke);
  void SimplifyStringLength(HInvoke* invoke);
  void SimplifyStringIndexOf(HInvoke* invoke);
  void SimplifyNPEOnArgN(HInvoke* invoke, size_t);
  void SimplifyReturnThis(HInvoke* invoke);
  void SimplifyAllocationIntrinsic(HInvoke* invoke);
  void SimplifyVarHandleIntrinsic(HInvoke* invoke);
  void SimplifyArrayBaseOffset(HInvoke* invoke);

  bool CanUseKnownImageVarHandle(HInvoke* invoke);
  static bool CanEnsureNotNullAt(HInstruction* input, HInstruction* at);

  // Returns an instruction with the opposite Boolean value from 'cond'.
  // The instruction is inserted into the graph, either in the entry block
  // (constant), or before the `cursor` (otherwise).
  HInstruction* InsertOppositeCondition(HInstruction* cond, HInstruction* cursor);

  CodeGenerator* codegen_;
  OptimizingCompilerStats* stats_;
  bool simplification_occurred_ = false;
  int simplifications_at_current_position_ = 0;
  // Prohibit optimizations which can affect HInductionVarAnalysis/HLoopOptimization
  // and prevent loop optimizations:
  //   true - avoid such optimizations.
  //   false - allow such optimizations.
  // Checked by the following optimizations:
  //   - TryToReuseDiv: simplification of Div+Rem into Div+Mul+Sub.
  bool be_loop_friendly_;
  // We ensure we do not loop infinitely. The value should not be too high, since that
  // would allow looping around the same basic block too many times. The value should
  // not be too low either, however, since we want to allow revisiting a basic block
  // with many statements and simplifications at least once.
  static constexpr int kMaxSamePositionSimplifications = 50;
};

bool InstructionSimplifier::Run() {
  if (kTestInstructionClonerExhaustively) {
    CloneAndReplaceInstructionVisitor visitor(graph_);
    visitor.VisitReversePostOrder();
  }

  bool be_loop_friendly = (use_all_optimizations_ == false);

  InstructionSimplifierVisitor visitor(graph_, codegen_, stats_, be_loop_friendly);
  return visitor.Run();
}

bool InstructionSimplifierVisitor::Run() {
  bool didSimplify = false;
  // Iterate in reverse post order to open up more simplifications to users
  // of instructions that got simplified.
  for (HBasicBlock* block : GetGraph()->GetReversePostOrder()) {
    // The simplification of an instruction to another instruction may yield
    // possibilities for other simplifications. So although we perform a reverse
    // post order visit, we sometimes need to revisit an instruction index.
    do {
      simplification_occurred_ = false;
      VisitNonPhiInstructions(block);
      if (simplification_occurred_) {
        didSimplify = true;
      }
    } while (simplification_occurred_ &&
             (simplifications_at_current_position_ < kMaxSamePositionSimplifications));
    simplifications_at_current_position_ = 0;
  }
  return didSimplify;
}

namespace {

bool AreAllBitsSet(HConstant* constant) {
  return Int64FromConstant(constant) == -1;
}

}  // namespace

// Returns true if the code was simplified to use only one negation operation
// after the binary operation instead of one on each of the inputs.
bool InstructionSimplifierVisitor::TryMoveNegOnInputsAfterBinop(HBinaryOperation* binop) {
  DCHECK(binop->IsAdd() || binop->IsSub());
  DCHECK(binop->GetLeft()->IsNeg() && binop->GetRight()->IsNeg());
  HNeg* left_neg = binop->GetLeft()->AsNeg();
  HNeg* right_neg = binop->GetRight()->AsNeg();
  if (!left_neg->HasOnlyOneNonEnvironmentUse() ||
      !right_neg->HasOnlyOneNonEnvironmentUse()) {
    return false;
  }
  // Replace code looking like
  //    NEG tmp1, a
  //    NEG tmp2, b
  //    ADD dst, tmp1, tmp2
  // with
  //    ADD tmp, a, b
  //    NEG dst, tmp
  // Note that we cannot optimize `(-a) + (-b)` to `-(a + b)` for floating-point.
  // When `a` is `-0.0` and `b` is `0.0`, the former expression yields `0.0`,
  // while the later yields `-0.0`.
  if (!DataType::IsIntegralType(binop->GetType())) {
    return false;
  }
  binop->ReplaceInput(left_neg->GetInput(), 0);
  binop->ReplaceInput(right_neg->GetInput(), 1);
  left_neg->GetBlock()->RemoveInstruction(left_neg);
  right_neg->GetBlock()->RemoveInstruction(right_neg);
  HNeg* neg = new (GetGraph()->GetAllocator()) HNeg(binop->GetType(), binop);
  binop->GetBlock()->InsertInstructionBefore(neg, binop->GetNext());
  binop->ReplaceWithExceptInReplacementAtIndex(neg, 0);
  RecordSimplification();
  return true;
}

bool InstructionSimplifierVisitor::TryDeMorganNegationFactoring(HBinaryOperation* op) {
  DCHECK(op->IsAnd() || op->IsOr()) << op->DebugName();
  DataType::Type type = op->GetType();
  HInstruction* left = op->GetLeft();
  HInstruction* right = op->GetRight();

  // We can apply De Morgan's laws if both inputs are Not's and are only used
  // by `op`.
  if (((left->IsNot() && right->IsNot()) ||
       (left->IsBooleanNot() && right->IsBooleanNot())) &&
      left->HasOnlyOneNonEnvironmentUse() &&
      right->HasOnlyOneNonEnvironmentUse()) {
    // Replace code looking like
    //    NOT nota, a
    //    NOT notb, b
    //    AND dst, nota, notb (respectively OR)
    // with
    //    OR or, a, b         (respectively AND)
    //    NOT dest, or
    HInstruction* src_left = left->InputAt(0);
    HInstruction* src_right = right->InputAt(0);
    uint32_t dex_pc = op->GetDexPc();

    // Remove the negations on the inputs.
    left->ReplaceWith(src_left);
    right->ReplaceWith(src_right);
    left->GetBlock()->RemoveInstruction(left);
    right->GetBlock()->RemoveInstruction(right);

    // Replace the `HAnd` or `HOr`.
    HBinaryOperation* hbin;
    if (op->IsAnd()) {
      hbin = new (GetGraph()->GetAllocator()) HOr(type, src_left, src_right, dex_pc);
    } else {
      hbin = new (GetGraph()->GetAllocator()) HAnd(type, src_left, src_right, dex_pc);
    }
    HInstruction* hnot;
    if (left->IsBooleanNot()) {
      hnot = new (GetGraph()->GetAllocator()) HBooleanNot(hbin, dex_pc);
    } else {
      hnot = new (GetGraph()->GetAllocator()) HNot(type, hbin, dex_pc);
    }

    op->GetBlock()->InsertInstructionBefore(hbin, op);
    op->GetBlock()->ReplaceAndRemoveInstructionWith(op, hnot);

    RecordSimplification();
    return true;
  }

  return false;
}

bool InstructionSimplifierVisitor::TryCombineVecMultiplyAccumulate(HVecMul* mul) {
  DataType::Type type = mul->GetPackedType();
  InstructionSet isa = codegen_->GetInstructionSet();
  switch (isa) {
    case InstructionSet::kArm64:
      if (!(type == DataType::Type::kUint8 ||
            type == DataType::Type::kInt8 ||
            type == DataType::Type::kUint16 ||
            type == DataType::Type::kInt16 ||
            type == DataType::Type::kInt32)) {
        return false;
      }
      break;
    default:
      return false;
  }

  ArenaAllocator* allocator = GetGraph()->GetAllocator();
  if (!mul->HasOnlyOneNonEnvironmentUse()) {
    return false;
  }
  HInstruction* binop = mul->GetUses().front().GetUser();
  if (!binop->IsVecAdd() && !binop->IsVecSub()) {
    return false;
  }

  // Replace code looking like
  //    VECMUL tmp, x, y
  //    VECADD/SUB dst, acc, tmp
  // with
  //    VECMULACC dst, acc, x, y
  // Note that we do not want to (unconditionally) perform the merge when the
  // multiplication has multiple uses and it can be merged in all of them.
  // Multiple uses could happen on the same control-flow path, and we would
  // then increase the amount of work. In the future we could try to evaluate
  // whether all uses are on different control-flow paths (using dominance and
  // reverse-dominance information) and only perform the merge when they are.
  HInstruction* accumulator = nullptr;
  HVecBinaryOperation* vec_binop = binop->AsVecBinaryOperation();
  HInstruction* binop_left = vec_binop->GetLeft();
  HInstruction* binop_right = vec_binop->GetRight();
  // This is always true since the `HVecMul` has only one use (which is checked above).
  DCHECK_NE(binop_left, binop_right);
  if (binop_right == mul) {
    accumulator = binop_left;
  } else {
    DCHECK_EQ(binop_left, mul);
    // Only addition is commutative.
    if (!binop->IsVecAdd()) {
      return false;
    }
    accumulator = binop_right;
  }

  DCHECK(accumulator != nullptr);
  HInstruction::InstructionKind kind =
      binop->IsVecAdd() ? HInstruction::kAdd : HInstruction::kSub;

  bool predicated_simd = vec_binop->IsPredicated();
  if (predicated_simd && !HVecOperation::HaveSamePredicate(vec_binop, mul)) {
    return false;
  }

  HVecMultiplyAccumulate* mulacc =
      new (allocator) HVecMultiplyAccumulate(allocator,
                                             kind,
                                             accumulator,
                                             mul->GetLeft(),
                                             mul->GetRight(),
                                             vec_binop->GetPackedType(),
                                             vec_binop->GetVectorLength(),
                                             vec_binop->GetDexPc());



  vec_binop->GetBlock()->ReplaceAndRemoveInstructionWith(vec_binop, mulacc);
  if (predicated_simd) {
    mulacc->SetGoverningPredicate(vec_binop->GetGoverningPredicate(),
                                  vec_binop->GetPredicationKind());
  }

  DCHECK(!mul->HasUses());
  mul->GetBlock()->RemoveInstruction(mul);
  return true;
}

// Replace code looking like (x << N >>> N or x << N >> N):
//    SHL tmp, x, N
//    USHR/SHR dst, tmp, N
// with the corresponding type conversion:
//    TypeConversion<Unsigned<T>/Signed<T>> dst, x
// if
//    SHL has only one non environment use
//    TypeOf(tmp) is not 64-bit type (they are not supported yet)
//    N % kBitsPerByte = 0
// where
//    T = SignedIntegralTypeFromSize(source_integral_size)
//    source_integral_size = ByteSize(tmp) - N / kBitsPerByte
//
//    We calculate source_integral_size from shift amount instead of
//    assuming that it is equal to ByteSize(x) to be able to optimize
//    cases like this:
//        int x = ...
//        int y = x << 24 >>> 24
//    that is equavalent to
//        int y = (unsigned byte) x
//    in this case:
//        N = 24
//        tmp = x << 24
//        source_integral_size is 1 (= 4 - 24 / 8) that corresponds to unsigned byte.
static bool TryReplaceShiftsByConstantWithTypeConversion(HBinaryOperation *instruction) {
  if (!instruction->IsUShr() && !instruction->IsShr()) {
    return false;
  }

  if (DataType::Is64BitType(instruction->GetResultType())) {
    return false;
  }

  HInstruction* shr_amount = instruction->GetRight();
  if (!shr_amount->IsIntConstant()) {
    return false;
  }

  int32_t shr_amount_cst = shr_amount->AsIntConstant()->GetValue();

  // We assume that shift amount simplification was applied first so it doesn't
  // exceed maximum distance that is kMaxIntShiftDistance as 64-bit shifts aren't
  // supported.
  DCHECK_LE(shr_amount_cst, kMaxIntShiftDistance);

  if ((shr_amount_cst % kBitsPerByte) != 0) {
    return false;
  }

  // Calculate size of the significant part of the input, e.g. a part that is not
  // discarded due to left shift.
  // Shift amount here should be less than size of right shift type.
  DCHECK_GT(DataType::Size(instruction->GetType()), shr_amount_cst / kBitsPerByte);
  size_t source_significant_part_size =
      DataType::Size(instruction->GetType()) - shr_amount_cst / kBitsPerByte;

  // Look for the smallest signed integer type that is suitable to store the
  // significant part of the input.
  DataType::Type source_integral_type =
      DataType::SignedIntegralTypeFromSize(source_significant_part_size);

  // If the size of the significant part of the input isn't equal to the size of the
  // found type, shifts cannot be replaced by type conversion.
  if (DataType::Size(source_integral_type) != source_significant_part_size) {
    return false;
  }

  HInstruction* shr_value = instruction->GetLeft();
  if (!shr_value->IsShl()) {
    return false;
  }

  HShl *shl = shr_value->AsShl();
  if (!shl->HasOnlyOneNonEnvironmentUse()) {
    return false;
  }

  // Constants are unique so we just compare pointer here.
  if (shl->GetRight() != shr_amount) {
    return false;
  }

  // Type of shift's value is always int so sign/zero extension only
  // depends on the type of the shift (shr/ushr).
  bool is_signed = instruction->IsShr();
  DataType::Type conv_type =
      is_signed ? source_integral_type : DataType::ToUnsigned(source_integral_type);

  DCHECK(DataType::IsTypeConversionImplicit(conv_type, instruction->GetResultType()));

  HInstruction* shl_value = shl->GetLeft();
  HBasicBlock *block = instruction->GetBlock();

  // We shouldn't introduce new implicit type conversions during simplification.
  if (DataType::IsTypeConversionImplicit(shl_value->GetType(), conv_type)) {
    instruction->ReplaceWith(shl_value);
    instruction->GetBlock()->RemoveInstruction(instruction);
  } else {
    HTypeConversion* new_conversion =
        new (block->GetGraph()->GetAllocator()) HTypeConversion(conv_type, shl_value);
    block->ReplaceAndRemoveInstructionWith(instruction, new_conversion);
  }

  shl->GetBlock()->RemoveInstruction(shl);

  return true;
}

void InstructionSimplifierVisitor::VisitShift(HBinaryOperation* instruction) {
  DCHECK(instruction->IsShl() || instruction->IsShr() || instruction->IsUShr());
  HInstruction* shift_amount = instruction->GetRight();
  HInstruction* value = instruction->GetLeft();

  int64_t implicit_mask = (value->GetType() == DataType::Type::kInt64)
      ? kMaxLongShiftDistance
      : kMaxIntShiftDistance;

  if (shift_amount->IsConstant()) {
    int64_t cst = Int64FromConstant(shift_amount->AsConstant());
    int64_t masked_cst = cst & implicit_mask;
    if (masked_cst == 0) {
      // Replace code looking like
      //    SHL dst, value, 0
      // with
      //    value
      instruction->ReplaceWith(value);
      instruction->GetBlock()->RemoveInstruction(instruction);
      RecordSimplification();
      return;
    } else if (masked_cst != cst) {
      // Replace code looking like
      //    SHL dst, value, cst
      // where cst exceeds maximum distance with the equivalent
      //    SHL dst, value, cst & implicit_mask
      // (as defined by shift semantics). This ensures other
      // optimizations do not need to special case for such situations.
      DCHECK_EQ(shift_amount->GetType(), DataType::Type::kInt32);
      instruction->ReplaceInput(GetGraph()->GetIntConstant(masked_cst), /* index= */ 1);
      RecordSimplification();
      return;
    }

    if (TryReplaceShiftsByConstantWithTypeConversion(instruction)) {
      RecordSimplification();
      return;
    }
  }

  // Shift operations implicitly mask the shift amount according to the type width. Get rid of
  // unnecessary And/Or/Xor/Add/Sub/TypeConversion operations on the shift amount that do not
  // affect the relevant bits.
  // Replace code looking like
  //    AND adjusted_shift, shift, <superset of implicit mask>
  //    [OR/XOR/ADD/SUB adjusted_shift, shift, <value not overlapping with implicit mask>]
  //    [<conversion-from-integral-non-64-bit-type> adjusted_shift, shift]
  //    SHL dst, value, adjusted_shift
  // with
  //    SHL dst, value, shift
  if (shift_amount->IsAnd() ||
      shift_amount->IsOr() ||
      shift_amount->IsXor() ||
      shift_amount->IsAdd() ||
      shift_amount->IsSub()) {
    int64_t required_result = shift_amount->IsAnd() ? implicit_mask : 0;
    HBinaryOperation* bin_op = shift_amount->AsBinaryOperation();
    HConstant* mask = bin_op->GetConstantRight();
    if (mask != nullptr && (Int64FromConstant(mask) & implicit_mask) == required_result) {
      instruction->ReplaceInput(bin_op->GetLeastConstantLeft(), 1);
      RecordSimplification();
      return;
    }
  } else if (shift_amount->IsTypeConversion()) {
    DCHECK_NE(shift_amount->GetType(), DataType::Type::kBool);  // We never convert to bool.
    DataType::Type source_type = shift_amount->InputAt(0)->GetType();
    // Non-integral and 64-bit source types require an explicit type conversion.
    if (DataType::IsIntegralType(source_type) && !DataType::Is64BitType(source_type)) {
      instruction->ReplaceInput(shift_amount->AsTypeConversion()->GetInput(), 1);
      RecordSimplification();
      return;
    }
  }
}

static bool IsSubRegBitsMinusOther(HSub* sub, size_t reg_bits, HInstruction* other) {
  return (sub->GetRight() == other &&
          sub->GetLeft()->IsConstant() &&
          (Int64FromConstant(sub->GetLeft()->AsConstant()) & (reg_bits - 1)) == 0);
}

bool InstructionSimplifierVisitor::ReplaceRotateWithRor(HBinaryOperation* op,
                                                        HUShr* ushr,
                                                        HShl* shl) {
  DCHECK(op->IsAdd() || op->IsXor() || op->IsOr()) << op->DebugName();
  HRor* ror =
      new (GetGraph()->GetAllocator()) HRor(ushr->GetType(), ushr->GetLeft(), ushr->GetRight());
  op->GetBlock()->ReplaceAndRemoveInstructionWith(op, ror);
  if (!ushr->HasUses()) {
    ushr->GetBlock()->RemoveInstruction(ushr);
  }
  if (!ushr->GetRight()->HasUses()) {
    ushr->GetRight()->GetBlock()->RemoveInstruction(ushr->GetRight());
  }
  if (!shl->HasUses()) {
    shl->GetBlock()->RemoveInstruction(shl);
  }
  if (!shl->GetRight()->HasUses()) {
    shl->GetRight()->GetBlock()->RemoveInstruction(shl->GetRight());
  }
  RecordSimplification();
  return true;
}

// Try to replace a binary operation flanked by one UShr and one Shl with a bitfield rotation.
bool InstructionSimplifierVisitor::TryReplaceWithRotate(HBinaryOperation* op) {
  DCHECK(op->IsAdd() || op->IsXor() || op->IsOr());
  HInstruction* left = op->GetLeft();
  HInstruction* right = op->GetRight();
  // If we have an UShr and a Shl (in either order).
  if ((left->IsUShr() && right->IsShl()) || (left->IsShl() && right->IsUShr())) {
    HUShr* ushr = left->IsUShr() ? left->AsUShr() : right->AsUShr();
    HShl* shl = left->IsShl() ? left->AsShl() : right->AsShl();
    DCHECK(DataType::IsIntOrLongType(ushr->GetType()));
    if (ushr->GetType() == shl->GetType() &&
        ushr->GetLeft() == shl->GetLeft()) {
      if (ushr->GetRight()->IsConstant() && shl->GetRight()->IsConstant()) {
        // Shift distances are both constant, try replacing with Ror if they
        // add up to the register size.
        return TryReplaceWithRotateConstantPattern(op, ushr, shl);
      } else if (ushr->GetRight()->IsSub() || shl->GetRight()->IsSub()) {
        // Shift distances are potentially of the form x and (reg_size - x).
        return TryReplaceWithRotateRegisterSubPattern(op, ushr, shl);
      } else if (ushr->GetRight()->IsNeg() || shl->GetRight()->IsNeg()) {
        // Shift distances are potentially of the form d and -d.
        return TryReplaceWithRotateRegisterNegPattern(op, ushr, shl);
      }
    }
  }
  return false;
}

// Try replacing code looking like (x >>> #rdist OP x << #ldist):
//    UShr dst, x,   #rdist
//    Shl  tmp, x,   #ldist
//    OP   dst, dst, tmp
// or like (x >>> #rdist OP x << #-ldist):
//    UShr dst, x,   #rdist
//    Shl  tmp, x,   #-ldist
//    OP   dst, dst, tmp
// with
//    Ror  dst, x,   #rdist
bool InstructionSimplifierVisitor::TryReplaceWithRotateConstantPattern(HBinaryOperation* op,
                                                                       HUShr* ushr,
                                                                       HShl* shl) {
  DCHECK(op->IsAdd() || op->IsXor() || op->IsOr());
  size_t reg_bits = DataType::Size(ushr->GetType()) * kBitsPerByte;
  size_t rdist = Int64FromConstant(ushr->GetRight()->AsConstant());
  size_t ldist = Int64FromConstant(shl->GetRight()->AsConstant());
  if (((ldist + rdist) & (reg_bits - 1)) == 0) {
    return ReplaceRotateWithRor(op, ushr, shl);
  }
  return false;
}

// Replace code looking like (x >>> -d OP x << d):
//    Neg  neg, d
//    UShr dst, x,   neg
//    Shl  tmp, x,   d
//    OP   dst, dst, tmp
// with
//    Neg  neg, d
//    Ror  dst, x,   neg
// *** OR ***
// Replace code looking like (x >>> d OP x << -d):
//    UShr dst, x,   d
//    Neg  neg, d
//    Shl  tmp, x,   neg
//    OP   dst, dst, tmp
// with
//    Ror  dst, x,   d
//
// Requires `d` to be non-zero for the HAdd and HXor case. If `d` is 0 the shifts and rotate are
// no-ops and the `OP` is never executed. This is fine for HOr since the result is the same, but the
// result is different for HAdd and HXor.
bool InstructionSimplifierVisitor::TryReplaceWithRotateRegisterNegPattern(HBinaryOperation* op,
                                                                          HUShr* ushr,
                                                                          HShl* shl) {
  DCHECK(op->IsAdd() || op->IsXor() || op->IsOr());
  DCHECK(ushr->GetRight()->IsNeg() || shl->GetRight()->IsNeg());
  bool neg_is_left = shl->GetRight()->IsNeg();
  HNeg* neg = neg_is_left ? shl->GetRight()->AsNeg() : ushr->GetRight()->AsNeg();
  HInstruction* value = neg->InputAt(0);

  // The shift distance being negated is the distance being shifted the other way.
  if (value != (neg_is_left ? ushr->GetRight() : shl->GetRight())) {
    return false;
  }

  const bool needs_non_zero_value = !op->IsOr();
  if (needs_non_zero_value) {
    if (!value->IsConstant() || value->AsConstant()->IsArithmeticZero()) {
      return false;
    }
  }
  return ReplaceRotateWithRor(op, ushr, shl);
}

// Try replacing code looking like (x >>> d OP x << (#bits - d)):
//    UShr dst, x,     d
//    Sub  ld,  #bits, d
//    Shl  tmp, x,     ld
//    OP   dst, dst,   tmp
// with
//    Ror  dst, x,     d
// *** OR ***
// Replace code looking like (x >>> (#bits - d) OP x << d):
//    Sub  rd,  #bits, d
//    UShr dst, x,     rd
//    Shl  tmp, x,     d
//    OP   dst, dst,   tmp
// with
//    Neg  neg, d
//    Ror  dst, x,     neg
bool InstructionSimplifierVisitor::TryReplaceWithRotateRegisterSubPattern(HBinaryOperation* op,
                                                                          HUShr* ushr,
                                                                          HShl* shl) {
  DCHECK(op->IsAdd() || op->IsXor() || op->IsOr());
  DCHECK(ushr->GetRight()->IsSub() || shl->GetRight()->IsSub());
  size_t reg_bits = DataType::Size(ushr->GetType()) * kBitsPerByte;
  HInstruction* shl_shift = shl->GetRight();
  HInstruction* ushr_shift = ushr->GetRight();
  if ((shl_shift->IsSub() && IsSubRegBitsMinusOther(shl_shift->AsSub(), reg_bits, ushr_shift)) ||
      (ushr_shift->IsSub() && IsSubRegBitsMinusOther(ushr_shift->AsSub(), reg_bits, shl_shift))) {
    return ReplaceRotateWithRor(op, ushr, shl);
  }
  return false;
}

void InstructionSimplifierVisitor::VisitNullCheck(HNullCheck* null_check) {
  HInstruction* obj = null_check->InputAt(0);
  // Note we don't do `CanEnsureNotNullAt` here. If we do that, we may get rid of a NullCheck but
  // what we should do instead is coalesce them. This is what GVN does, and so InstructionSimplifier
  // doesn't do this.
  if (!obj->CanBeNull()) {
    null_check->ReplaceWith(obj);
    null_check->GetBlock()->RemoveInstruction(null_check);
    if (stats_ != nullptr) {
      stats_->RecordStat(MethodCompilationStat::kRemovedNullCheck);
    }
  }
}

bool InstructionSimplifierVisitor::CanEnsureNotNullAt(HInstruction* input, HInstruction* at) {
  if (!input->CanBeNull()) {
    return true;
  }

  for (const HUseListNode<HInstruction*>& use : input->GetUses()) {
    HInstruction* user = use.GetUser();
    if (user->IsNullCheck() && user->StrictlyDominates(at)) {
      return true;
    }
  }

  return false;
}

// Returns whether doing a type test between the class of `object` against `klass` has
// a statically known outcome. The result of the test is stored in `outcome`.
static bool TypeCheckHasKnownOutcome(ReferenceTypeInfo class_rti,
                                     HInstruction* object,
                                     /*out*/bool* outcome) {
  DCHECK(!object->IsNullConstant()) << "Null constants should be special cased";
  ReferenceTypeInfo obj_rti = object->GetReferenceTypeInfo();
  ScopedObjectAccess soa(Thread::Current());
  if (!obj_rti.IsValid()) {
    // We run the simplifier before the reference type propagation so type info might not be
    // available.
    return false;
  }

  if (!class_rti.IsValid()) {
    // Happens when the loaded class is unresolved.
    if (obj_rti.IsExact()) {
      // outcome == 'true' && obj_rti is valid implies that class_rti is valid.
      // Since that's a contradiction we must not pass this check.
      *outcome = false;
      return true;
    } else {
      // We aren't able to say anything in particular since we don't know the
      // exact type of the object.
      return false;
    }
  }
  DCHECK(class_rti.IsExact());
  if (class_rti.IsSupertypeOf(obj_rti)) {
    *outcome = true;
    return true;
  } else if (obj_rti.IsExact()) {
    // The test failed at compile time so will also fail at runtime.
    *outcome = false;
    return true;
  } else if (!class_rti.IsInterface()
             && !obj_rti.IsInterface()
             && !obj_rti.IsSupertypeOf(class_rti)) {
    // Different type hierarchy. The test will fail.
    *outcome = false;
    return true;
  }
  return false;
}

void InstructionSimplifierVisitor::VisitCheckCast(HCheckCast* check_cast) {
  HInstruction* object = check_cast->InputAt(0);
  if (CanEnsureNotNullAt(object, check_cast)) {
    check_cast->ClearMustDoNullCheck();
  }

  if (object->IsNullConstant()) {
    check_cast->GetBlock()->RemoveInstruction(check_cast);
    MaybeRecordStat(stats_, MethodCompilationStat::kRemovedCheckedCast);
    return;
  }

  // Minor correctness check.
  DCHECK(check_cast->GetTargetClass()->StrictlyDominates(check_cast))
      << "Illegal graph!\n"
      << check_cast->DumpWithArgs();

  // Historical note: The `outcome` was initialized to please Valgrind - the compiler can reorder
  // the return value check with the `outcome` check, b/27651442.
  bool outcome = false;
  if (TypeCheckHasKnownOutcome(check_cast->GetTargetClassRTI(), object, &outcome)) {
    if (outcome) {
      check_cast->GetBlock()->RemoveInstruction(check_cast);
      MaybeRecordStat(stats_, MethodCompilationStat::kRemovedCheckedCast);
      if (check_cast->GetTypeCheckKind() != TypeCheckKind::kBitstringCheck) {
        HLoadClass* load_class = check_cast->GetTargetClass();
        if (!load_class->HasUses() && !load_class->NeedsAccessCheck()) {
          // We cannot rely on DCE to remove the class because the `HLoadClass` thinks it can throw.
          // However, here we know that it cannot because the checkcast was successful, hence
          // the class was already loaded.
          load_class->GetBlock()->RemoveInstruction(load_class);
        }
      }
    } else {
      // TODO Don't do anything for exceptional cases for now. Ideally we should
      // remove all instructions and blocks this instruction dominates and
      // replace it with a manual throw.
    }
  }
}

void InstructionSimplifierVisitor::VisitInstanceOf(HInstanceOf* instruction) {
  HInstruction* object = instruction->InputAt(0);

  bool can_be_null = true;
  if (CanEnsureNotNullAt(object, instruction)) {
    can_be_null = false;
    instruction->ClearMustDoNullCheck();
  }

  HGraph* graph = GetGraph();
  if (object->IsNullConstant()) {
    MaybeRecordStat(stats_, MethodCompilationStat::kRemovedInstanceOf);
    instruction->ReplaceWith(graph->GetIntConstant(0));
    instruction->GetBlock()->RemoveInstruction(instruction);
    RecordSimplification();
    return;
  }

  // Minor correctness check.
  DCHECK(instruction->GetTargetClass()->StrictlyDominates(instruction))
      << "Illegal graph!\n"
      << instruction->DumpWithArgs();

  // Historical note: The `outcome` was initialized to please Valgrind - the compiler can reorder
  // the return value check with the `outcome` check, b/27651442.
  bool outcome = false;
  if (TypeCheckHasKnownOutcome(instruction->GetTargetClassRTI(), object, &outcome)) {
    MaybeRecordStat(stats_, MethodCompilationStat::kRemovedInstanceOf);
    if (outcome && can_be_null) {
      // Type test will succeed, we just need a null test.
      HNotEqual* test = new (graph->GetAllocator()) HNotEqual(graph->GetNullConstant(), object);
      instruction->GetBlock()->InsertInstructionBefore(test, instruction);
      instruction->ReplaceWith(test);
    } else {
      // We've statically determined the result of the instanceof.
      instruction->ReplaceWith(graph->GetIntConstant(outcome));
    }
    RecordSimplification();
    instruction->GetBlock()->RemoveInstruction(instruction);
    if (outcome && instruction->GetTypeCheckKind() != TypeCheckKind::kBitstringCheck) {
      HLoadClass* load_class = instruction->GetTargetClass();
      if (!load_class->HasUses() && !load_class->NeedsAccessCheck()) {
        // We cannot rely on DCE to remove the class because the `HLoadClass`
        // thinks it can throw. However, here we know that it cannot because the
        // instanceof check was successful and we don't need to check the
        // access, hence the class was already loaded.
        load_class->GetBlock()->RemoveInstruction(load_class);
      }
    }
  }
}

void InstructionSimplifierVisitor::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  if ((instruction->GetValue()->GetType() == DataType::Type::kReference)
      && CanEnsureNotNullAt(instruction->GetValue(), instruction)) {
    instruction->ClearValueCanBeNull();
  }
}

void InstructionSimplifierVisitor::VisitStaticFieldSet(HStaticFieldSet* instruction) {
  if ((instruction->GetValue()->GetType() == DataType::Type::kReference)
      && CanEnsureNotNullAt(instruction->GetValue(), instruction)) {
    instruction->ClearValueCanBeNull();
  }
}

static IfCondition GetOppositeConditionForOperandSwap(IfCondition cond) {
  switch (cond) {
    case kCondEQ: return kCondEQ;
    case kCondNE: return kCondNE;
    case kCondLT: return kCondGT;
    case kCondLE: return kCondGE;
    case kCondGT: return kCondLT;
    case kCondGE: return kCondLE;
    case kCondB: return kCondA;
    case kCondBE: return kCondAE;
    case kCondA: return kCondB;
    case kCondAE: return kCondBE;
    default:
      LOG(FATAL) << "Unknown ConditionType " << cond;
      UNREACHABLE();
  }
}

HInstruction* InstructionSimplifierVisitor::InsertOppositeCondition(HInstruction* cond,
                                                                    HInstruction* cursor) {
  if (cond->IsCondition() &&
      !DataType::IsFloatingPointType(cond->InputAt(0)->GetType())) {
    // Can't reverse floating point conditions. We have to use `HBooleanNot` in that case.
    HInstruction* lhs = cond->InputAt(0);
    HInstruction* rhs = cond->InputAt(1);
    HInstruction* replacement =
        HCondition::Create(GetGraph(), cond->AsCondition()->GetOppositeCondition(), lhs, rhs);
    cursor->GetBlock()->InsertInstructionBefore(replacement, cursor);
    return replacement;
  } else if (cond->IsIntConstant()) {
    HIntConstant* int_const = cond->AsIntConstant();
    if (int_const->IsFalse()) {
      return GetGraph()->GetIntConstant(1);
    } else {
      DCHECK(int_const->IsTrue()) << int_const->GetValue();
      return GetGraph()->GetIntConstant(0);
    }
  } else {
    HInstruction* replacement = new (GetGraph()->GetAllocator()) HBooleanNot(cond);
    cursor->GetBlock()->InsertInstructionBefore(replacement, cursor);
    return replacement;
  }
}

void InstructionSimplifierVisitor::VisitEqual(HEqual* equal) {
  HInstruction* input_const = equal->GetConstantRight();
  if (input_const != nullptr) {
    HInstruction* input_value = equal->GetLeastConstantLeft();
    if ((input_value->GetType() == DataType::Type::kBool) && input_const->IsIntConstant()) {
      HBasicBlock* block = equal->GetBlock();
      // We are comparing the boolean to a constant which is of type int and can
      // be any constant.
      if (input_const->AsIntConstant()->IsTrue()) {
        // Replace (bool_value == true) with bool_value
        equal->ReplaceWith(input_value);
        block->RemoveInstruction(equal);
        RecordSimplification();
      } else if (input_const->AsIntConstant()->IsFalse()) {
        // Replace (bool_value == false) with !bool_value
        equal->ReplaceWith(InsertOppositeCondition(input_value, equal));
        block->RemoveInstruction(equal);
        RecordSimplification();
      } else {
        // Replace (bool_value == integer_not_zero_nor_one_constant) with false
        equal->ReplaceWith(GetGraph()->GetIntConstant(0));
        block->RemoveInstruction(equal);
        RecordSimplification();
      }
    } else {
      VisitCondition(equal);
    }
  } else {
    VisitCondition(equal);
  }
}

void InstructionSimplifierVisitor::VisitNotEqual(HNotEqual* not_equal) {
  HInstruction* input_const = not_equal->GetConstantRight();
  if (input_const != nullptr) {
    HInstruction* input_value = not_equal->GetLeastConstantLeft();
    if ((input_value->GetType() == DataType::Type::kBool) && input_const->IsIntConstant()) {
      HBasicBlock* block = not_equal->GetBlock();
      // We are comparing the boolean to a constant which is of type int and can
      // be any constant.
      if (input_const->AsIntConstant()->IsTrue()) {
        // Replace (bool_value != true) with !bool_value
        not_equal->ReplaceWith(InsertOppositeCondition(input_value, not_equal));
        block->RemoveInstruction(not_equal);
        RecordSimplification();
      } else if (input_const->AsIntConstant()->IsFalse()) {
        // Replace (bool_value != false) with bool_value
        not_equal->ReplaceWith(input_value);
        block->RemoveInstruction(not_equal);
        RecordSimplification();
      } else {
        // Replace (bool_value != integer_not_zero_nor_one_constant) with true
        not_equal->ReplaceWith(GetGraph()->GetIntConstant(1));
        block->RemoveInstruction(not_equal);
        RecordSimplification();
      }
    } else {
      VisitCondition(not_equal);
    }
  } else {
    VisitCondition(not_equal);
  }
}

void InstructionSimplifierVisitor::VisitBooleanNot(HBooleanNot* bool_not) {
  HInstruction* input = bool_not->InputAt(0);
  HInstruction* replace_with = nullptr;

  if (input->IsIntConstant()) {
    // Replace !(true/false) with false/true.
    if (input->AsIntConstant()->IsTrue()) {
      replace_with = GetGraph()->GetIntConstant(0);
    } else {
      DCHECK(input->AsIntConstant()->IsFalse()) << input->AsIntConstant()->GetValue();
      replace_with = GetGraph()->GetIntConstant(1);
    }
  } else if (input->IsBooleanNot()) {
    // Replace (!(!bool_value)) with bool_value.
    replace_with = input->InputAt(0);
  } else if (input->IsCondition() &&
             // Don't change FP compares. The definition of compares involving
             // NaNs forces the compares to be done as written by the user.
             !DataType::IsFloatingPointType(input->InputAt(0)->GetType())) {
    // Replace condition with its opposite.
    replace_with = InsertOppositeCondition(input->AsCondition(), bool_not);
  }

  if (replace_with != nullptr) {
    bool_not->ReplaceWith(replace_with);
    bool_not->GetBlock()->RemoveInstruction(bool_not);
    RecordSimplification();
  }
}

// Constructs a new ABS(x) node in the HIR.
static HInstruction* NewIntegralAbs(ArenaAllocator* allocator,
                                    HInstruction* x,
                                    HInstruction* cursor) {
  DataType::Type type = DataType::Kind(x->GetType());
  DCHECK(type == DataType::Type::kInt32 || type == DataType::Type::kInt64);
  HAbs* abs = new (allocator) HAbs(type, x, cursor->GetDexPc());
  cursor->GetBlock()->InsertInstructionBefore(abs, cursor);
  return abs;
}

// Constructs a new MIN/MAX(x, y) node in the HIR.
static HInstruction* NewIntegralMinMax(ArenaAllocator* allocator,
                                       HInstruction* x,
                                       HInstruction* y,
                                       HInstruction* cursor,
                                       bool is_min) {
  DataType::Type type = DataType::Kind(x->GetType());
  DCHECK(type == DataType::Type::kInt32 || type == DataType::Type::kInt64);
  HBinaryOperation* minmax = nullptr;
  if (is_min) {
    minmax = new (allocator) HMin(type, x, y, cursor->GetDexPc());
  } else {
    minmax = new (allocator) HMax(type, x, y, cursor->GetDexPc());
  }
  cursor->GetBlock()->InsertInstructionBefore(minmax, cursor);
  return minmax;
}

// Returns true if operands a and b consists of widening type conversions
// (either explicit or implicit) to the given to_type.
static bool AreLowerPrecisionArgs(DataType::Type to_type, HInstruction* a, HInstruction* b) {
  if (a->IsTypeConversion() && a->GetType() == to_type) {
    a = a->InputAt(0);
  }
  if (b->IsTypeConversion() && b->GetType() == to_type) {
    b = b->InputAt(0);
  }
  DataType::Type type1 = a->GetType();
  DataType::Type type2 = b->GetType();
  return (type1 == DataType::Type::kUint8  && type2 == DataType::Type::kUint8) ||
         (type1 == DataType::Type::kInt8   && type2 == DataType::Type::kInt8) ||
         (type1 == DataType::Type::kInt16  && type2 == DataType::Type::kInt16) ||
         (type1 == DataType::Type::kUint16 && type2 == DataType::Type::kUint16) ||
         (type1 == DataType::Type::kInt32  && type2 == DataType::Type::kInt32 &&
          to_type == DataType::Type::kInt64);
}

// Returns an acceptable substitution for "a" on the select
// construct "a <cmp> b ? c : .."  during MIN/MAX recognition.
static HInstruction* AllowInMinMax(IfCondition cmp,
                                   HInstruction* a,
                                   HInstruction* b,
                                   HInstruction* c) {
  int64_t value = 0;
  if (IsInt64AndGet(b, /*out*/ &value) &&
      (((cmp == kCondLT || cmp == kCondLE) && c->IsMax()) ||
       ((cmp == kCondGT || cmp == kCondGE) && c->IsMin()))) {
    HConstant* other = c->AsBinaryOperation()->GetConstantRight();
    if (other != nullptr && a == c->AsBinaryOperation()->GetLeastConstantLeft()) {
      int64_t other_value = Int64FromConstant(other);
      bool is_max = (cmp == kCondLT || cmp == kCondLE);
      // Allow the max for a <  100 ? max(a, -100) : ..
      //    or the min for a > -100 ? min(a,  100) : ..
      if (is_max ? (value >= other_value) : (value <= other_value)) {
        return c;
      }
    }
  }
  return nullptr;
}

void InstructionSimplifierVisitor::VisitSelect(HSelect* select) {
  HInstruction* replace_with = nullptr;
  HInstruction* condition = select->GetCondition();
  HInstruction* true_value = select->GetTrueValue();
  HInstruction* false_value = select->GetFalseValue();

  if (condition->IsBooleanNot()) {
    // Change ((!cond) ? x : y) to (cond ? y : x).
    condition = condition->InputAt(0);
    std::swap(true_value, false_value);
    select->ReplaceInput(false_value, 0);
    select->ReplaceInput(true_value, 1);
    select->ReplaceInput(condition, 2);
    RecordSimplification();
  }

  if (true_value == false_value) {
    // Replace (cond ? x : x) with (x).
    replace_with = true_value;
  } else if (condition->IsIntConstant()) {
    if (condition->AsIntConstant()->IsTrue()) {
      // Replace (true ? x : y) with (x).
      replace_with = true_value;
    } else {
      // Replace (false ? x : y) with (y).
      DCHECK(condition->AsIntConstant()->IsFalse()) << condition->AsIntConstant()->GetValue();
      replace_with = false_value;
    }
  } else if (true_value->IsIntConstant() && false_value->IsIntConstant()) {
    if (true_value->AsIntConstant()->IsTrue() && false_value->AsIntConstant()->IsFalse()) {
      // Replace (cond ? true : false) with (cond).
      replace_with = condition;
    } else if (true_value->AsIntConstant()->IsFalse() && false_value->AsIntConstant()->IsTrue()) {
      // Replace (cond ? false : true) with (!cond).
      replace_with = InsertOppositeCondition(condition, select);
    }
  } else if (condition->IsCondition()) {
    IfCondition cmp = condition->AsCondition()->GetCondition();
    HInstruction* a = condition->InputAt(0);
    HInstruction* b = condition->InputAt(1);
    DataType::Type t_type = true_value->GetType();
    DataType::Type f_type = false_value->GetType();
    if (DataType::IsIntegralType(t_type) && DataType::Kind(t_type) == DataType::Kind(f_type)) {
      if (cmp == kCondEQ || cmp == kCondNE) {
        // Turns
        // * Select[a, b, EQ(a,b)] / Select[a, b, EQ(b,a)] into a
        // * Select[a, b, NE(a,b)] / Select[a, b, NE(b,a)] into b
        // Note that the order in EQ/NE is irrelevant.
        if ((a == true_value && b == false_value) || (a == false_value && b == true_value)) {
          replace_with = cmp == kCondEQ ? false_value : true_value;
        }
      } else {
        // Test if both values are compatible integral types (resulting MIN/MAX/ABS
        // type will be int or long, like the condition). Replacements are general,
        // but assume conditions prefer constants on the right.

        // Allow a <  100 ? max(a, -100) : ..
        //    or a > -100 ? min(a,  100) : ..
        // to use min/max instead of a to detect nested min/max expressions.
        HInstruction* new_a = AllowInMinMax(cmp, a, b, true_value);
        if (new_a != nullptr) {
          a = new_a;
        }
        // Try to replace typical integral MIN/MAX/ABS constructs.
        if ((cmp == kCondLT || cmp == kCondLE || cmp == kCondGT || cmp == kCondGE) &&
            ((a == true_value && b == false_value) || (b == true_value && a == false_value))) {
          // Found a < b ? a : b (MIN) or a < b ? b : a (MAX)
          //    or a > b ? a : b (MAX) or a > b ? b : a (MIN).
          bool is_min = (cmp == kCondLT || cmp == kCondLE) == (a == true_value);
          replace_with = NewIntegralMinMax(GetGraph()->GetAllocator(), a, b, select, is_min);
        } else if (((cmp == kCondLT || cmp == kCondLE) && true_value->IsNeg()) ||
                   ((cmp == kCondGT || cmp == kCondGE) && false_value->IsNeg())) {
          bool negLeft = (cmp == kCondLT || cmp == kCondLE);
          HInstruction* the_negated = negLeft ? true_value->InputAt(0) : false_value->InputAt(0);
          HInstruction* not_negated = negLeft ? false_value : true_value;
          if (a == the_negated && a == not_negated && IsInt64Value(b, 0)) {
            // Found a < 0 ? -a :  a
            //    or a > 0 ?  a : -a
            // which can be replaced by ABS(a).
            replace_with = NewIntegralAbs(GetGraph()->GetAllocator(), a, select);
          }
        } else if (true_value->IsSub() && false_value->IsSub()) {
          HInstruction* true_sub1 = true_value->InputAt(0);
          HInstruction* true_sub2 = true_value->InputAt(1);
          HInstruction* false_sub1 = false_value->InputAt(0);
          HInstruction* false_sub2 = false_value->InputAt(1);
          if ((((cmp == kCondGT || cmp == kCondGE) &&
                (a == true_sub1 && b == true_sub2 && a == false_sub2 && b == false_sub1)) ||
               ((cmp == kCondLT || cmp == kCondLE) &&
                (a == true_sub2 && b == true_sub1 && a == false_sub1 && b == false_sub2))) &&
              AreLowerPrecisionArgs(t_type, a, b)) {
            // Found a > b ? a - b  : b - a
            //    or a < b ? b - a  : a - b
            // which can be replaced by ABS(a - b) for lower precision operands a, b.
            replace_with = NewIntegralAbs(GetGraph()->GetAllocator(), true_value, select);
          }
        }
      }
    }
  }

  if (replace_with != nullptr) {
    select->ReplaceWith(replace_with);
    select->GetBlock()->RemoveInstruction(select);
    RecordSimplification();
  }
}

void InstructionSimplifierVisitor::VisitIf(HIf* instruction) {
  HInstruction* condition = instruction->InputAt(0);
  if (condition->IsBooleanNot()) {
    // Swap successors if input is negated.
    instruction->ReplaceInput(condition->InputAt(0), 0);
    instruction->GetBlock()->SwapSuccessors();
    RecordSimplification();
  }
}

// TODO(solanes): This optimization should be in ConstantFolding since we are folding to a constant.
// However, we get code size regressions when we do that since we sometimes have a NullCheck between
// HArrayLength and IsNewArray, and said NullCheck is eliminated in InstructionSimplifier. If we run
// ConstantFolding and InstructionSimplifier in lockstep this wouldn't be an issue.
void InstructionSimplifierVisitor::VisitArrayLength(HArrayLength* instruction) {
  HInstruction* input = instruction->InputAt(0);
  // If the array is a NewArray with constant size, replace the array length
  // with the constant instruction. This helps the bounds check elimination phase.
  if (input->IsNewArray()) {
    input = input->AsNewArray()->GetLength();
    if (input->IsIntConstant()) {
      instruction->ReplaceWith(input);
    }
  }
}

void InstructionSimplifierVisitor::VisitArraySet(HArraySet* instruction) {
  HInstruction* value = instruction->GetValue();
  if (value->GetType() != DataType::Type::kReference) {
    return;
  }

  if (CanEnsureNotNullAt(value, instruction)) {
    instruction->ClearValueCanBeNull();
  }

  if (value->IsArrayGet()) {
    if (value->AsArrayGet()->GetArray() == instruction->GetArray()) {
      // If the code is just swapping elements in the array, no need for a type check.
      instruction->ClearTypeCheck();
      return;
    }
  }

  if (value->IsNullConstant()) {
    instruction->ClearTypeCheck();
    return;
  }

  ScopedObjectAccess soa(Thread::Current());
  ReferenceTypeInfo array_rti = instruction->GetArray()->GetReferenceTypeInfo();
  ReferenceTypeInfo value_rti = value->GetReferenceTypeInfo();
  if (!array_rti.IsValid()) {
    return;
  }

  if (value_rti.IsValid() && array_rti.CanArrayHold(value_rti)) {
    instruction->ClearTypeCheck();
    return;
  }

  if (array_rti.IsObjectArray()) {
    if (array_rti.IsExact()) {
      instruction->ClearTypeCheck();
      return;
    }
    instruction->SetStaticTypeOfArrayIsObjectArray();
  }
}

static bool IsTypeConversionLossless(DataType::Type input_type, DataType::Type result_type) {
  // Make sure all implicit conversions have been simplified and no new ones have been introduced.
  DCHECK(!DataType::IsTypeConversionImplicit(input_type, result_type))
      << input_type << "," << result_type;
  // The conversion to a larger type is loss-less with the exception of two cases,
  //   - conversion to the unsigned type Uint16, where we may lose some bits, and
  //   - conversion from float to long, the only FP to integral conversion with smaller FP type.
  // For integral to FP conversions this holds because the FP mantissa is large enough.
  // Note: The size check excludes Uint8 as the result type.
  return DataType::Size(result_type) > DataType::Size(input_type) &&
      result_type != DataType::Type::kUint16 &&
      !(result_type == DataType::Type::kInt64 && input_type == DataType::Type::kFloat32);
}

static bool CanRemoveRedundantAnd(HConstant* and_right,
                                  HConstant* shr_right,
                                  DataType::Type result_type) {
  int64_t and_cst = Int64FromConstant(and_right);
  int64_t shr_cst = Int64FromConstant(shr_right);

  // In the following sequence A is the input value, D is the result:
  // B := A & x
  // C := B >> r
  // D := TypeConv(n-bit type) C

  // The value of D is entirely dependent on the bits [n-1:0] of C, which in turn are dependent
  // on bits [r+n-1:r] of B.
  // Therefore, if the AND does not change bits [r+n-1:r] of A then it will not affect D.
  // This can be checked by ensuring that bits [r+n-1:r] of the AND Constant are 1.

  // For example: return (byte) ((value & 0xff00) >> 8)
  //              return (byte) ((value & 0xff000000) >> 31)

  // The mask sets bits [r+n-1:r] to 1, and all others to 0.
  int64_t mask = DataType::MaxValueOfIntegralType(DataType::ToUnsigned(result_type)) << shr_cst;

  // If the result of a bitwise AND between the mask and the AND constant is the original mask, then
  // the AND does not change bits [r+n-1:r], meaning that it is redundant and can be removed.
  return ((and_cst & mask) == mask);
}

static inline bool TryReplaceFieldOrArrayGetType(HInstruction* maybe_get, DataType::Type new_type) {
  if (maybe_get->IsInstanceFieldGet()) {
    maybe_get->AsInstanceFieldGet()->SetType(new_type);
    return true;
  } else if (maybe_get->IsStaticFieldGet()) {
    maybe_get->AsStaticFieldGet()->SetType(new_type);
    return true;
  } else if (maybe_get->IsArrayGet() && !maybe_get->AsArrayGet()->IsStringCharAt()) {
    maybe_get->AsArrayGet()->SetType(new_type);
    return true;
  } else {
    return false;
  }
}

// The type conversion is only used for storing into a field/element of the
// same/narrower size.
static bool IsTypeConversionForStoringIntoNoWiderFieldOnly(HTypeConversion* type_conversion) {
  if (type_conversion->HasEnvironmentUses()) {
    return false;
  }
  DataType::Type input_type = type_conversion->GetInputType();
  DataType::Type result_type = type_conversion->GetResultType();
  if (!DataType::IsIntegralType(input_type) ||
      !DataType::IsIntegralType(result_type) ||
      input_type == DataType::Type::kInt64 ||
      result_type == DataType::Type::kInt64) {
    // Type conversion is needed if non-integer types are involved, or 64-bit
    // types are involved, which may use different number of registers.
    return false;
  }
  if (DataType::Size(input_type) >= DataType::Size(result_type)) {
    // Type conversion is not necessary when storing to a field/element of the
    // same/smaller size.
  } else {
    // We do not handle this case here.
    return false;
  }

  // Check if the converted value is only used for storing into heap.
  for (const HUseListNode<HInstruction*>& use : type_conversion->GetUses()) {
    HInstruction* instruction = use.GetUser();
    if (instruction->IsInstanceFieldSet() &&
        instruction->AsInstanceFieldSet()->GetFieldType() == result_type) {
      DCHECK_EQ(instruction->AsInstanceFieldSet()->GetValue(), type_conversion);
      continue;
    }
    if (instruction->IsStaticFieldSet() &&
        instruction->AsStaticFieldSet()->GetFieldType() == result_type) {
      DCHECK_EQ(instruction->AsStaticFieldSet()->GetValue(), type_conversion);
      continue;
    }
    if (instruction->IsArraySet() &&
        instruction->AsArraySet()->GetComponentType() == result_type &&
        // not index use.
        instruction->AsArraySet()->GetIndex() != type_conversion) {
      DCHECK_EQ(instruction->AsArraySet()->GetValue(), type_conversion);
      continue;
    }
    // The use is not as a store value, or the field/element type is not the
    // same as the result_type, keep the type conversion.
    return false;
  }
  // Codegen automatically handles the type conversion during the store.
  return true;
}

void InstructionSimplifierVisitor::VisitTypeConversion(HTypeConversion* instruction) {
  HInstruction* input = instruction->GetInput();
  DataType::Type input_type = input->GetType();
  DataType::Type result_type = instruction->GetResultType();
  if (instruction->IsImplicitConversion()) {
    instruction->ReplaceWith(input);
    instruction->GetBlock()->RemoveInstruction(instruction);
    RecordSimplification();
    return;
  }

  if (input->IsTypeConversion()) {
    HTypeConversion* input_conversion = input->AsTypeConversion();
    HInstruction* original_input = input_conversion->GetInput();
    DataType::Type original_type = original_input->GetType();

    // When the first conversion is lossless, a direct conversion from the original type
    // to the final type yields the same result, even for a lossy second conversion, for
    // example float->double->int or int->double->float.
    bool is_first_conversion_lossless = IsTypeConversionLossless(original_type, input_type);

    // For integral conversions, see if the first conversion loses only bits that the second
    // doesn't need, i.e. the final type is no wider than the intermediate. If so, direct
    // conversion yields the same result, for example long->int->short or int->char->short.
    bool integral_conversions_with_non_widening_second =
        DataType::IsIntegralType(input_type) &&
        DataType::IsIntegralType(original_type) &&
        DataType::IsIntegralType(result_type) &&
        DataType::Size(result_type) <= DataType::Size(input_type);

    if (is_first_conversion_lossless || integral_conversions_with_non_widening_second) {
      // If the merged conversion is implicit, do the simplification unconditionally.
      if (DataType::IsTypeConversionImplicit(original_type, result_type)) {
        instruction->ReplaceWith(original_input);
        instruction->GetBlock()->RemoveInstruction(instruction);
        if (!input_conversion->HasUses()) {
          // Don't wait for DCE.
          input_conversion->GetBlock()->RemoveInstruction(input_conversion);
        }
        RecordSimplification();
        return;
      }
      // Otherwise simplify only if the first conversion has no other use.
      if (input_conversion->HasOnlyOneNonEnvironmentUse()) {
        input_conversion->ReplaceWith(original_input);
        input_conversion->GetBlock()->RemoveInstruction(input_conversion);
        RecordSimplification();
        return;
      }
    }
  } else if (input->IsShr() && DataType::IsIntegralType(result_type) &&
            // Optimization only applies to lossy Type Conversions.
            !IsTypeConversionLossless(input_type, result_type)) {
    DCHECK(DataType::IsIntegralType(input_type));
    HShr* shr_op = input->AsShr();
    HConstant* shr_right = shr_op->GetConstantRight();
    HInstruction* shr_left = shr_op->GetLeastConstantLeft();
    if (shr_right != nullptr && shr_left->IsAnd()) {
      // Optimization needs AND -> SHR -> TypeConversion pattern.
      HAnd* and_op = shr_left->AsAnd();
      HConstant* and_right = and_op->GetConstantRight();
      HInstruction* and_left = and_op->GetLeastConstantLeft();
      if (and_right != nullptr &&
          !DataType::IsUnsignedType(and_left->GetType()) &&
          !DataType::IsUnsignedType(result_type) &&
          !DataType::IsUnsignedType(and_right->GetType()) &&
          (DataType::Size(and_left->GetType()) < 8) &&
          (DataType::Size(result_type) == 1)) {
        // TODO: Support Unsigned Types.
        // TODO: Support Long Types.
        // TODO: Support result types other than byte.
        if (and_op->HasOnlyOneNonEnvironmentUse() &&
            CanRemoveRedundantAnd(and_right, shr_right, result_type)) {
          and_op->ReplaceWith(and_left);
          and_op->GetBlock()->RemoveInstruction(and_op);
          RecordSimplification();
          return;
        }
      }
    }
  } else if (input->IsAnd() && DataType::IsIntegralType(result_type)) {
    DCHECK(DataType::IsIntegralType(input_type));
    HAnd* input_and = input->AsAnd();
    HConstant* constant = input_and->GetConstantRight();
    if (constant != nullptr) {
      int64_t value = Int64FromConstant(constant);
      DCHECK_NE(value, -1);  // "& -1" would have been optimized away in VisitAnd().
      size_t trailing_ones = CTZ(~static_cast<uint64_t>(value));
      if (trailing_ones >= kBitsPerByte * DataType::Size(result_type)) {
        // The `HAnd` is useless, for example in `(byte) (x & 0xff)`, get rid of it.
        HInstruction* original_input = input_and->GetLeastConstantLeft();
        if (DataType::IsTypeConversionImplicit(original_input->GetType(), result_type)) {
          instruction->ReplaceWith(original_input);
          instruction->GetBlock()->RemoveInstruction(instruction);
          RecordSimplification();
          return;
        } else if (input->HasOnlyOneNonEnvironmentUse()) {
          input_and->ReplaceWith(original_input);
          input_and->GetBlock()->RemoveInstruction(input_and);
          RecordSimplification();
          return;
        }
      }
    }
  } else if (input->HasOnlyOneNonEnvironmentUse() &&
             ((input_type == DataType::Type::kInt8 && result_type == DataType::Type::kUint8) ||
              (input_type == DataType::Type::kUint8 && result_type == DataType::Type::kInt8) ||
              (input_type == DataType::Type::kInt16 && result_type == DataType::Type::kUint16) ||
              (input_type == DataType::Type::kUint16 && result_type == DataType::Type::kInt16))) {
    // Try to modify the type of the load to `result_type` and remove the explicit type conversion.
    if (TryReplaceFieldOrArrayGetType(input, result_type)) {
      instruction->ReplaceWith(input);
      instruction->GetBlock()->RemoveInstruction(instruction);
      RecordSimplification();
      return;
    }
  }

  if (IsTypeConversionForStoringIntoNoWiderFieldOnly(instruction)) {
    instruction->ReplaceWith(input);
    instruction->GetBlock()->RemoveInstruction(instruction);
    RecordSimplification();
    return;
  }
}

void InstructionSimplifierVisitor::VisitAbs(HAbs* instruction) {
  HInstruction* input = instruction->GetInput();
  if (DataType::IsZeroExtension(input->GetType(), instruction->GetResultType())) {
    // Zero extension from narrow to wide can never set sign bit in the wider
    // operand, making the subsequent Abs redundant (e.g., abs(b & 0xff) for byte b).
    instruction->ReplaceWith(input);
    instruction->GetBlock()->RemoveInstruction(instruction);
    RecordSimplification();
  }
}

void InstructionSimplifierVisitor::VisitAdd(HAdd* instruction) {
  HConstant* input_cst = instruction->GetConstantRight();
  HInstruction* input_other = instruction->GetLeastConstantLeft();
  bool integral_type = DataType::IsIntegralType(instruction->GetType());
  if ((input_cst != nullptr) && input_cst->IsArithmeticZero()) {
    // Replace code looking like
    //    ADD dst, src, 0
    // with
    //    src
    // Note that we cannot optimize `x + 0.0` to `x` for floating-point. When
    // `x` is `-0.0`, the former expression yields `0.0`, while the later
    // yields `-0.0`.
    if (integral_type) {
      instruction->ReplaceWith(input_other);
      instruction->GetBlock()->RemoveInstruction(instruction);
      RecordSimplification();
      return;
    }
  }

  HInstruction* left = instruction->GetLeft();
  HInstruction* right = instruction->GetRight();
  bool left_is_neg = left->IsNeg();
  bool right_is_neg = right->IsNeg();

  if (left_is_neg && right_is_neg) {
    if (TryMoveNegOnInputsAfterBinop(instruction)) {
      return;
    }
  }

  if (left_is_neg != right_is_neg) {
    HNeg* neg = left_is_neg ? left->AsNeg() : right->AsNeg();
    if (neg->HasOnlyOneNonEnvironmentUse()) {
      // Replace code looking like
      //    NEG tmp, b
      //    ADD dst, a, tmp
      // with
      //    SUB dst, a, b
      // We do not perform the optimization if the input negation has environment
      // uses or multiple non-environment uses as it could lead to worse code. In
      // particular, we do not want the live range of `b` to be extended if we are
      // not sure the initial 'NEG' instruction can be removed.
      HInstruction* other = left_is_neg ? right : left;
      HSub* sub =
          new(GetGraph()->GetAllocator()) HSub(instruction->GetType(), other, neg->GetInput());
      instruction->GetBlock()->ReplaceAndRemoveInstructionWith(instruction, sub);
      RecordSimplification();
      neg->GetBlock()->RemoveInstruction(neg);
      return;
    }
  }

  if (TryReplaceWithRotate(instruction)) {
    return;
  }

  // TryHandleAssociativeAndCommutativeOperation() does not remove its input,
  // so no need to return.
  TryHandleAssociativeAndCommutativeOperation(instruction);

  if ((left->IsSub() || right->IsSub()) &&
      TrySubtractionChainSimplification(instruction)) {
    return;
  }

  if (integral_type) {
    // Replace code patterns looking like
    //    SUB dst1, x, y        SUB dst1, x, y
    //    ADD dst2, dst1, y     ADD dst2, y, dst1
    // with
    //    SUB dst1, x, y
    // ADD instruction is not needed in this case, we may use
    // one of inputs of SUB instead.
    if (left->IsSub() && left->InputAt(1) == right) {
      instruction->ReplaceWith(left->InputAt(0));
      RecordSimplification();
      instruction->GetBlock()->RemoveInstruction(instruction);
      return;
    } else if (right->IsSub() && right->InputAt(1) == left) {
      instruction->ReplaceWith(right->InputAt(0));
      RecordSimplification();
      instruction->GetBlock()->RemoveInstruction(instruction);
      return;
    }
  }
}

void InstructionSimplifierVisitor::VisitAnd(HAnd* instruction) {
  DCHECK(DataType::IsIntegralType(instruction->GetType()));
  HConstant* input_cst = instruction->GetConstantRight();
  HInstruction* input_other = instruction->GetLeastConstantLeft();

  if (input_cst != nullptr) {
    int64_t value = Int64FromConstant(input_cst);
    if (value == -1 ||
        // Similar cases under zero extension.
        (DataType::IsUnsignedType(input_other->GetType()) &&
         ((DataType::MaxValueOfIntegralType(input_other->GetType()) & ~value) == 0))) {
      // Replace code looking like
      //    AND dst, src, 0xFFF...FF
      // with
      //    src
      instruction->ReplaceWith(input_other);
      instruction->GetBlock()->RemoveInstruction(instruction);
      RecordSimplification();
      return;
    }
    if (input_other->IsTypeConversion() &&
        input_other->GetType() == DataType::Type::kInt64 &&
        DataType::IsIntegralType(input_other->InputAt(0)->GetType()) &&
        IsInt<32>(value) &&
        input_other->HasOnlyOneNonEnvironmentUse()) {
      // The AND can be reordered before the TypeConversion. Replace
      //   LongConstant cst, <32-bit-constant-sign-extended-to-64-bits>
      //   TypeConversion<Int64> tmp, src
      //   AND dst, tmp, cst
      // with
      //   IntConstant cst, <32-bit-constant>
      //   AND tmp, src, cst
      //   TypeConversion<Int64> dst, tmp
      // This helps 32-bit targets and does not hurt 64-bit targets.
      // This also simplifies detection of other patterns, such as Uint8 loads.
      HInstruction* new_and_input = input_other->InputAt(0);
      // Implicit conversion Int64->Int64 would have been removed previously.
      DCHECK_NE(new_and_input->GetType(), DataType::Type::kInt64);
      HConstant* new_const = GetGraph()->GetConstant(DataType::Type::kInt32, value);
      HAnd* new_and =
          new (GetGraph()->GetAllocator()) HAnd(DataType::Type::kInt32, new_and_input, new_const);
      instruction->GetBlock()->InsertInstructionBefore(new_and, instruction);
      HTypeConversion* new_conversion =
          new (GetGraph()->GetAllocator()) HTypeConversion(DataType::Type::kInt64, new_and);
      instruction->GetBlock()->ReplaceAndRemoveInstructionWith(instruction, new_conversion);
      input_other->GetBlock()->RemoveInstruction(input_other);
      RecordSimplification();
      // Try to process the new And now, do not wait for the next round of simplifications.
      instruction = new_and;
      input_other = new_and_input;
    }
    // Eliminate And from UShr+And if the And-mask contains all the bits that
    // can be non-zero after UShr. Transform Shr+And to UShr if the And-mask
    // precisely clears the shifted-in sign bits.
    if ((input_other->IsUShr() || input_other->IsShr()) && input_other->InputAt(1)->IsConstant()) {
      size_t reg_bits = (instruction->GetResultType() == DataType::Type::kInt64) ? 64 : 32;
      size_t shift = Int64FromConstant(input_other->InputAt(1)->AsConstant()) & (reg_bits - 1);
      size_t num_tail_bits_set = CTZ(value + 1);
      if ((num_tail_bits_set >= reg_bits - shift) && input_other->IsUShr()) {
        // This AND clears only bits known to be clear, for example "(x >>> 24) & 0xff".
        instruction->ReplaceWith(input_other);
        instruction->GetBlock()->RemoveInstruction(instruction);
        RecordSimplification();
        return;
      }  else if ((num_tail_bits_set == reg_bits - shift) && IsPowerOfTwo(value + 1) &&
          input_other->HasOnlyOneNonEnvironmentUse()) {
        DCHECK(input_other->IsShr());  // For UShr, we would have taken the branch above.
        // Replace SHR+AND with USHR, for example "(x >> 24) & 0xff" -> "x >>> 24".
        HUShr* ushr = new (GetGraph()->GetAllocator()) HUShr(instruction->GetType(),
                                                             input_other->InputAt(0),
                                                             input_other->InputAt(1),
                                                             input_other->GetDexPc());
        instruction->GetBlock()->ReplaceAndRemoveInstructionWith(instruction, ushr);
        input_other->GetBlock()->RemoveInstruction(input_other);
        RecordSimplification();
        return;
      }
    }
    if ((value == 0xff || value == 0xffff) && instruction->GetType() != DataType::Type::kInt64) {
      // Transform AND to a type conversion to Uint8/Uint16. If `input_other` is a field
      // or array Get with only a single use, short-circuit the subsequent simplification
      // of the Get+TypeConversion and change the Get's type to `new_type` instead.
      DataType::Type new_type = (value == 0xff) ? DataType::Type::kUint8 : DataType::Type::kUint16;
      DataType::Type find_type = (value == 0xff) ? DataType::Type::kInt8 : DataType::Type::kInt16;
      if (input_other->GetType() == find_type &&
          input_other->HasOnlyOneNonEnvironmentUse() &&
          TryReplaceFieldOrArrayGetType(input_other, new_type)) {
        instruction->ReplaceWith(input_other);
        instruction->GetBlock()->RemoveInstruction(instruction);
      } else if (DataType::IsTypeConversionImplicit(input_other->GetType(), new_type)) {
        instruction->ReplaceWith(input_other);
        instruction->GetBlock()->RemoveInstruction(instruction);
      } else {
        HTypeConversion* type_conversion = new (GetGraph()->GetAllocator()) HTypeConversion(
            new_type, input_other, instruction->GetDexPc());
        instruction->GetBlock()->ReplaceAndRemoveInstructionWith(instruction, type_conversion);
      }
      RecordSimplification();
      return;
    }
  }

  // We assume that GVN has run before, so we only perform a pointer comparison.
  // If for some reason the values are equal but the pointers are different, we
  // are still correct and only miss an optimization opportunity.
  if (instruction->GetLeft() == instruction->GetRight()) {
    // Replace code looking like
    //    AND dst, src, src
    // with
    //    src
    instruction->ReplaceWith(instruction->GetLeft());
    instruction->GetBlock()->RemoveInstruction(instruction);
    RecordSimplification();
    return;
  }

  if (TryDeMorganNegationFactoring(instruction)) {
    return;
  }

  // TryHandleAssociativeAndCommutativeOperation() does not remove its input,
  // so no need to return.
  TryHandleAssociativeAndCommutativeOperation(instruction);
}

void InstructionSimplifierVisitor::VisitGreaterThan(HGreaterThan* condition) {
  VisitCondition(condition);
}

void InstructionSimplifierVisitor::VisitGreaterThanOrEqual(HGreaterThanOrEqual* condition) {
  VisitCondition(condition);
}

void InstructionSimplifierVisitor::VisitLessThan(HLessThan* condition) {
  VisitCondition(condition);
}

void InstructionSimplifierVisitor::VisitLessThanOrEqual(HLessThanOrEqual* condition) {
  VisitCondition(condition);
}

void InstructionSimplifierVisitor::VisitBelow(HBelow* condition) {
  VisitCondition(condition);
}

void InstructionSimplifierVisitor::VisitBelowOrEqual(HBelowOrEqual* condition) {
  VisitCondition(condition);
}

void InstructionSimplifierVisitor::VisitAbove(HAbove* condition) {
  VisitCondition(condition);
}

void InstructionSimplifierVisitor::VisitAboveOrEqual(HAboveOrEqual* condition) {
  VisitCondition(condition);
}

// Recognize the following pattern:
// obj.getClass() ==/!= Foo.class
// And replace it with a constant value if the type of `obj` is statically known.
static bool RecognizeAndSimplifyClassCheck(HCondition* condition) {
  HInstruction* input_one = condition->InputAt(0);
  HInstruction* input_two = condition->InputAt(1);
  HLoadClass* load_class = input_one->IsLoadClass()
      ? input_one->AsLoadClass()
      : input_two->AsLoadClassOrNull();
  if (load_class == nullptr) {
    return false;
  }

  ReferenceTypeInfo class_rti = load_class->GetLoadedClassRTI();
  if (!class_rti.IsValid()) {
    // Unresolved class.
    return false;
  }

  HInstanceFieldGet* field_get = (load_class == input_one)
      ? input_two->AsInstanceFieldGetOrNull()
      : input_one->AsInstanceFieldGetOrNull();
  if (field_get == nullptr) {
    return false;
  }

  HInstruction* receiver = field_get->InputAt(0);
  ReferenceTypeInfo receiver_type = receiver->GetReferenceTypeInfo();
  if (!receiver_type.IsExact()) {
    return false;
  }

  {
    ScopedObjectAccess soa(Thread::Current());
    ArtField* field = WellKnownClasses::java_lang_Object_shadowKlass;
    if (field_get->GetFieldInfo().GetField() != field) {
      return false;
    }

    // We can replace the compare.
    int value = 0;
    if (receiver_type.IsEqual(class_rti)) {
      value = condition->IsEqual() ? 1 : 0;
    } else {
      value = condition->IsNotEqual() ? 1 : 0;
    }
    condition->ReplaceWith(condition->GetBlock()->GetGraph()->GetIntConstant(value));
    return true;
  }
}

static HInstruction* CreateUnsignedConditionReplacement(ArenaAllocator* allocator,
                                                        HCondition* cond,
                                                        HCompare* compare) {
  DCHECK(cond->InputAt(1)->IsIntConstant());
  DCHECK_EQ(cond->InputAt(1)->AsIntConstant()->GetValue(), 0);
  DCHECK(cond->InputAt(0) == compare);

  HBasicBlock* block = cond->GetBlock();
  HInstruction* lhs = compare->InputAt(0);
  HInstruction* rhs = compare->InputAt(1);

  switch (cond->GetKind()) {
    case HInstruction::kLessThan:
      return new (allocator) HBelow(lhs, rhs, cond->GetDexPc());
    case HInstruction::kLessThanOrEqual:
      return new (allocator) HBelowOrEqual(lhs, rhs, cond->GetDexPc());
    case HInstruction::kGreaterThan:
      return new (allocator) HAbove(lhs, rhs, cond->GetDexPc());
    case HInstruction::kGreaterThanOrEqual:
      return new (allocator) HAboveOrEqual(lhs, rhs, cond->GetDexPc());
    case HInstruction::kBelow:
      // Below(Compare(x, y), 0) always False since
      //   unsigned(-1) < 0 -> False
      //   0 < 0 -> False
      //   1 < 0 -> False
      return block->GetGraph()->GetConstant(DataType::Type::kBool, 0);
    case HInstruction::kBelowOrEqual:
      // BelowOrEqual(Compare(x, y), 0) transforms into Equal(x, y)
      //    unsigned(-1) <= 0 -> False
      //    0 <= 0 -> True
      //    1 <= 0 -> False
      return new (allocator) HEqual(lhs, rhs, cond->GetDexPc());
    case HInstruction::kAbove:
      // Above(Compare(x, y), 0) transforms into NotEqual(x, y)
      //    unsigned(-1) > 0 -> True
      //    0 > 0 -> False
      //    1 > 0 -> True
      return new (allocator) HNotEqual(lhs, rhs, cond->GetDexPc());
    case HInstruction::kAboveOrEqual:
      // AboveOrEqual(Compare(x, y), 0) always True since
      //   unsigned(-1) >= 0 -> True
      //   0 >= 0 -> True
      //   1 >= 0 -> True
      return block->GetGraph()->GetConstant(DataType::Type::kBool, 1);
    default:
      LOG(FATAL) << "Unknown ConditionType " << cond->GetKind();
      UNREACHABLE();
  }
}

void InstructionSimplifierVisitor::VisitCondition(HCondition* condition) {
  if (condition->IsEqual() || condition->IsNotEqual()) {
    if (RecognizeAndSimplifyClassCheck(condition)) {
      return;
    }
  }

  // Reverse condition if left is constant. Our code generators prefer constant
  // on the right hand side.
  HBasicBlock* block = condition->GetBlock();
  HInstruction* left = condition->GetLeft();
  HInstruction* right = condition->GetRight();
  if (left->IsConstant() && !right->IsConstant()) {
    IfCondition new_cond = GetOppositeConditionForOperandSwap(condition->GetCondition());
    HCondition* replacement = HCondition::Create(GetGraph(), new_cond, right, left);
    block->ReplaceAndRemoveInstructionWith(condition, replacement);
    // If it is a FP condition, we must set the opposite bias.
    if (condition->IsLtBias()) {
      replacement->SetBias(ComparisonBias::kGtBias);
    } else if (condition->IsGtBias()) {
      replacement->SetBias(ComparisonBias::kLtBias);
    }
    RecordSimplification();
    condition = replacement;
    std::swap(left, right);
  }

  // Try to fold an HCompare into this HCondition.

  // We can only replace an HCondition which compares a Compare to 0.
  // Both 'dx' and 'jack' generate a compare to 0 when compiling a
  // condition with a long, float or double comparison as input.
  if (!left->IsCompare() || !right->IsConstant() || right->AsIntConstant()->GetValue() != 0) {
    // Conversion is not possible.
    return;
  }

  // Is the Compare only used for this purpose?
  if (!left->GetUses().HasExactlyOneElement()) {
    // Someone else also wants the result of the compare.
    return;
  }

  if (!left->GetEnvUses().empty()) {
    // There is a reference to the compare result in an environment. Do we really need it?
    if (GetGraph()->IsDebuggable()) {
      return;
    }

    // We have to ensure that there are no deopt points in the sequence.
    if (left->HasAnyEnvironmentUseBefore(condition)) {
      return;
    }
  }

  // Clean up any environment uses from the HCompare, if any.
  left->RemoveEnvironmentUsers();

  // We have decided to fold the HCompare into the HCondition. Transfer the information.
  if (DataType::IsUnsignedType(left->AsCompare()->GetComparisonType()) &&
      !condition->IsEqual() &&
      !condition->IsNotEqual()) {
    DCHECK_EQ(condition->GetBias(), ComparisonBias::kNoBias);
    HInstruction* replacement = CreateUnsignedConditionReplacement(
        block->GetGraph()->GetAllocator(), condition, left->AsCompare());

    if (replacement->IsConstant()) {
      condition->ReplaceWith(replacement);
      block->RemoveInstruction(condition);
    } else {
      block->ReplaceAndRemoveInstructionWith(condition, replacement);
    }
  } else {
    condition->SetBias(left->AsCompare()->GetBias());

    // Replace the operands of the HCondition.
    condition->ReplaceInput(left->InputAt(0), 0);
    condition->ReplaceInput(left->InputAt(1), 1);
  }

  // Remove the HCompare.
  left->GetBlock()->RemoveInstruction(left);

  RecordSimplification();
}

static HInstruction* CheckSignedToUnsignedCompareConversion(HInstruction* operand,
                                                            HCompare* compare) {
  // Check if operand looks like `ADD op, MIN_INTEGRAL`
  if (operand->IsConstant()) {
    // CONSTANT #x -> CONSTANT #(x - MIN_INTEGRAL)
    HConstant* constant = operand->AsConstant();
    if (constant->IsIntConstant()) {
      HIntConstant* int_constant = constant->AsIntConstant();
      int32_t old_value = int_constant->GetValue();
      int32_t new_value = old_value - std::numeric_limits<int32_t>::min();
      return operand->GetBlock()->GetGraph()->GetIntConstant(new_value);
    } else if (constant->IsLongConstant()) {
      HLongConstant* long_constant = constant->AsLongConstant();
      int64_t old_value = long_constant->GetValue();
      int64_t new_value = old_value - std::numeric_limits<int64_t>::min();
      return operand->GetBlock()->GetGraph()->GetLongConstant(new_value);
    } else {
      return nullptr;
    }
  }

  if (!operand->IsAdd() && !operand->IsXor()) {
    return nullptr;
  }

  if (!operand->GetEnvUses().empty()) {
    // There is a reference to the compare result in an environment. Do we really need it?
    if (operand->GetBlock()->GetGraph()->IsDebuggable()) {
      return nullptr;
    }

    // We have to ensure that there are no deopt points in the sequence.
    if (operand->HasAnyEnvironmentUseBefore(compare)) {
      return nullptr;
    }
  }

  HBinaryOperation* additive_operand = operand->AsBinaryOperation();

  HInstruction* left = additive_operand->GetLeft();
  HInstruction* right = additive_operand->GetRight();

  HConstant* constant = nullptr;
  HInstruction* value = nullptr;

  if (left->IsConstant() && !right->IsConstant()) {
    constant = left->AsConstant();
    value = right;
  } else if (!left->IsConstant() && right->IsConstant()) {
    value = left;
    constant = right->AsConstant();
  } else {
    return nullptr;
  }

  if (constant->IsIntConstant()) {
    HIntConstant* int_constant = constant->AsIntConstant();
    if (int_constant->GetValue() != std::numeric_limits<int32_t>::min()) {
      return nullptr;
    }
  } else if (constant->IsLongConstant()) {
    HLongConstant* long_constant = constant->AsLongConstant();
    if (long_constant->GetValue() != std::numeric_limits<int64_t>::min()) {
      return nullptr;
    }
  } else {
    return nullptr;
  }

  return value;
}

static DataType::Type GetOpositeSignType(DataType::Type type) {
  return DataType::IsUnsignedType(type) ? DataType::ToSigned(type) : DataType::ToUnsigned(type);
}

void InstructionSimplifierVisitor::VisitCompare(HCompare* compare) {
  // Transform signed compare into unsigned if possible
  // Replace code looking like
  //    ADD normalizedLeft, left, MIN_INTEGRAL
  //    ADD normalizedRight, right, MIN_INTEGRAL
  //    COMPARE normalizedLeft, normalizedRight, sign
  // with
  //    COMPARE left, right, !sign

  if (!DataType::IsIntegralType(compare->GetComparisonType())) {
    return;
  }

  HInstruction* compare_left = compare->GetLeft();
  HInstruction* compare_right = compare->GetRight();

  if (compare_left->IsConstant() && compare_right->IsConstant()) {
    // Do not simplify, let it be folded.
    return;
  }

  HInstruction* left = CheckSignedToUnsignedCompareConversion(compare_left, compare);
  if (left == nullptr) {
    return;
  }

  HInstruction* right = CheckSignedToUnsignedCompareConversion(compare_right, compare);
  if (right == nullptr) {
    return;
  }

  compare->SetComparisonType(GetOpositeSignType(compare->GetComparisonType()));
  compare->ReplaceInput(left, 0);
  compare->ReplaceInput(right, 1);

  RecordSimplification();

  if (compare_left->GetUses().empty()) {
    compare_left->RemoveEnvironmentUsers();
    compare_left->GetBlock()->RemoveInstruction(compare_left);
  }

  if (compare_right->GetUses().empty()) {
    compare_right->RemoveEnvironmentUsers();
    compare_right->GetBlock()->RemoveInstruction(compare_right);
  }
}

// Return whether x / divisor == x * (1.0f / divisor), for every float x.
static constexpr bool CanDivideByReciprocalMultiplyFloat(int32_t divisor) {
  // True, if the most significant bits of divisor are 0.
  return ((divisor & 0x7fffff) == 0);
}

// Return whether x / divisor == x * (1.0 / divisor), for every double x.
static constexpr bool CanDivideByReciprocalMultiplyDouble(int64_t divisor) {
  // True, if the most significant bits of divisor are 0.
  return ((divisor & ((UINT64_C(1) << 52) - 1)) == 0);
}

void InstructionSimplifierVisitor::VisitDiv(HDiv* instruction) {
  HConstant* input_cst = instruction->GetConstantRight();
  HInstruction* input_other = instruction->GetLeastConstantLeft();
  DataType::Type type = instruction->GetType();

  if ((input_cst != nullptr) && input_cst->IsOne()) {
    // Replace code looking like
    //    DIV dst, src, 1
    // with
    //    src
    instruction->ReplaceWith(input_other);
    instruction->GetBlock()->RemoveInstruction(instruction);
    RecordSimplification();
    return;
  }

  if ((input_cst != nullptr) && input_cst->IsMinusOne()) {
    // Replace code looking like
    //    DIV dst, src, -1
    // with
    //    NEG dst, src
    instruction->GetBlock()->ReplaceAndRemoveInstructionWith(
        instruction, new (GetGraph()->GetAllocator()) HNeg(type, input_other));
    RecordSimplification();
    return;
  }

  if ((input_cst != nullptr) && DataType::IsFloatingPointType(type)) {
    // Try replacing code looking like
    //    DIV dst, src, constant
    // with
    //    MUL dst, src, 1 / constant
    HConstant* reciprocal = nullptr;
    if (type == DataType::Type::kFloat64) {
      double value = input_cst->AsDoubleConstant()->GetValue();
      if (CanDivideByReciprocalMultiplyDouble(bit_cast<int64_t, double>(value))) {
        reciprocal = GetGraph()->GetDoubleConstant(1.0 / value);
      }
    } else {
      DCHECK_EQ(type, DataType::Type::kFloat32);
      float value = input_cst->AsFloatConstant()->GetValue();
      if (CanDivideByReciprocalMultiplyFloat(bit_cast<int32_t, float>(value))) {
        reciprocal = GetGraph()->GetFloatConstant(1.0f / value);
      }
    }

    if (reciprocal != nullptr) {
      instruction->GetBlock()->ReplaceAndRemoveInstructionWith(
          instruction, new (GetGraph()->GetAllocator()) HMul(type, input_other, reciprocal));
      RecordSimplification();
      return;
    }
  }
}


// Search HDiv having the specified dividend and divisor which is in the specified basic block.
// Return nullptr if nothing has been found.
static HDiv* FindDivWithInputsInBasicBlock(HInstruction* dividend,
                                           HInstruction* divisor,
                                           HBasicBlock* basic_block) {
  for (const HUseListNode<HInstruction*>& use : dividend->GetUses()) {
    HInstruction* user = use.GetUser();
    if (user->GetBlock() == basic_block &&
        user->IsDiv() &&
        user->InputAt(0) == dividend &&
        user->InputAt(1) == divisor) {
      return user->AsDiv();
    }
  }
  return nullptr;
}

// If there is Div with the same inputs as Rem and in the same basic block, it can be reused.
// Rem is replaced with Mul+Sub which use the found Div.
void InstructionSimplifierVisitor::TryToReuseDiv(HRem* rem) {
  // As the optimization replaces Rem with Mul+Sub they prevent some loop optimizations
  // if the Rem is in a loop.
  // Check if it is allowed to optimize such Rems.
  if (rem->IsInLoop() && be_loop_friendly_) {
    return;
  }
  DataType::Type type = rem->GetResultType();
  if (!DataType::IsIntOrLongType(type)) {
    return;
  }

  HBasicBlock* basic_block = rem->GetBlock();
  HInstruction* dividend = rem->GetLeft();
  HInstruction* divisor = rem->GetRight();

  if (divisor->IsConstant()) {
    HConstant* input_cst = rem->GetConstantRight();
    DCHECK(input_cst->IsIntConstant() || input_cst->IsLongConstant());
    int64_t cst_value = Int64FromConstant(input_cst);
    if (cst_value == std::numeric_limits<int64_t>::min() || IsPowerOfTwo(std::abs(cst_value))) {
      // Such cases are usually handled in the code generator because they don't need Div at all.
      return;
    }
  }

  HDiv* quotient = FindDivWithInputsInBasicBlock(dividend, divisor, basic_block);
  if (quotient == nullptr) {
    return;
  }
  if (!quotient->StrictlyDominates(rem)) {
    quotient->MoveBefore(rem);
  }

  ArenaAllocator* allocator = GetGraph()->GetAllocator();
  HInstruction* mul = new (allocator) HMul(type, quotient, divisor);
  basic_block->InsertInstructionBefore(mul, rem);
  HInstruction* sub = new (allocator) HSub(type, dividend, mul);
  basic_block->InsertInstructionBefore(sub, rem);
  rem->ReplaceWith(sub);
  basic_block->RemoveInstruction(rem);
  RecordSimplification();
}

void InstructionSimplifierVisitor::VisitRem(HRem* rem) {
  TryToReuseDiv(rem);
}

void InstructionSimplifierVisitor::VisitMul(HMul* instruction) {
  HConstant* input_cst = instruction->GetConstantRight();
  HInstruction* input_other = instruction->GetLeastConstantLeft();
  DataType::Type type = instruction->GetType();
  HBasicBlock* block = instruction->GetBlock();
  ArenaAllocator* allocator = GetGraph()->GetAllocator();

  if (input_cst == nullptr) {
    return;
  }

  if (input_cst->IsOne()) {
    // Replace code looking like
    //    MUL dst, src, 1
    // with
    //    src
    instruction->ReplaceWith(input_other);
    instruction->GetBlock()->RemoveInstruction(instruction);
    RecordSimplification();
    return;
  }

  if (input_cst->IsMinusOne() &&
      (DataType::IsFloatingPointType(type) || DataType::IsIntOrLongType(type))) {
    // Replace code looking like
    //    MUL dst, src, -1
    // with
    //    NEG dst, src
    HNeg* neg = new (allocator) HNeg(type, input_other);
    block->ReplaceAndRemoveInstructionWith(instruction, neg);
    RecordSimplification();
    return;
  }

  if (DataType::IsFloatingPointType(type) &&
      ((input_cst->IsFloatConstant() && input_cst->AsFloatConstant()->GetValue() == 2.0f) ||
       (input_cst->IsDoubleConstant() && input_cst->AsDoubleConstant()->GetValue() == 2.0))) {
    // Replace code looking like
    //    FP_MUL dst, src, 2.0
    // with
    //    FP_ADD dst, src, src
    // The 'int' and 'long' cases are handled below.
    block->ReplaceAndRemoveInstructionWith(instruction,
                                           new (allocator) HAdd(type, input_other, input_other));
    RecordSimplification();
    return;
  }

  if (DataType::IsIntOrLongType(type)) {
    int64_t factor = Int64FromConstant(input_cst);
    // Even though constant propagation also takes care of the zero case, other
    // optimizations can lead to having a zero multiplication.
    if (factor == 0) {
      // Replace code looking like
      //    MUL dst, src, 0
      // with
      //    0
      instruction->ReplaceWith(input_cst);
      instruction->GetBlock()->RemoveInstruction(instruction);
      RecordSimplification();
      return;
    } else if (IsPowerOfTwo(factor)) {
      // Replace code looking like
      //    MUL dst, src, pow_of_2
      // with
      //    SHL dst, src, log2(pow_of_2)
      HIntConstant* shift = GetGraph()->GetIntConstant(WhichPowerOf2(factor));
      HShl* shl = new (allocator) HShl(type, input_other, shift);
      block->ReplaceAndRemoveInstructionWith(instruction, shl);
      RecordSimplification();
      return;
    } else if (IsPowerOfTwo(factor - 1)) {
      // Transform code looking like
      //    MUL dst, src, (2^n + 1)
      // into
      //    SHL tmp, src, n
      //    ADD dst, src, tmp
      HShl* shl = new (allocator) HShl(type,
                                       input_other,
                                       GetGraph()->GetIntConstant(WhichPowerOf2(factor - 1)));
      HAdd* add = new (allocator) HAdd(type, input_other, shl);

      block->InsertInstructionBefore(shl, instruction);
      block->ReplaceAndRemoveInstructionWith(instruction, add);
      RecordSimplification();
      return;
    } else if (IsPowerOfTwo(factor + 1)) {
      // Transform code looking like
      //    MUL dst, src, (2^n - 1)
      // into
      //    SHL tmp, src, n
      //    SUB dst, tmp, src
      HShl* shl = new (allocator) HShl(type,
                                       input_other,
                                       GetGraph()->GetIntConstant(WhichPowerOf2(factor + 1)));
      HSub* sub = new (allocator) HSub(type, shl, input_other);

      block->InsertInstructionBefore(shl, instruction);
      block->ReplaceAndRemoveInstructionWith(instruction, sub);
      RecordSimplification();
      return;
    }
  }

  // TryHandleAssociativeAndCommutativeOperation() does not remove its input,
  // so no need to return.
  TryHandleAssociativeAndCommutativeOperation(instruction);
}

void InstructionSimplifierVisitor::VisitNeg(HNeg* instruction) {
  HInstruction* input = instruction->GetInput();
  if (input->IsNeg()) {
    // Replace code looking like
    //    NEG tmp, src
    //    NEG dst, tmp
    // with
    //    src
    HNeg* previous_neg = input->AsNeg();
    instruction->ReplaceWith(previous_neg->GetInput());
    instruction->GetBlock()->RemoveInstruction(instruction);
    // We perform the optimization even if the input negation has environment
    // uses since it allows removing the current instruction. But we only delete
    // the input negation only if it is does not have any uses left.
    if (!previous_neg->HasUses()) {
      previous_neg->GetBlock()->RemoveInstruction(previous_neg);
    }
    RecordSimplification();
    return;
  }

  if (input->IsSub() && input->HasOnlyOneNonEnvironmentUse() &&
      !DataType::IsFloatingPointType(input->GetType())) {
    // Replace code looking like
    //    SUB tmp, a, b
    //    NEG dst, tmp
    // with
    //    SUB dst, b, a
    // We do not perform the optimization if the input subtraction has
    // environment uses or multiple non-environment uses as it could lead to
    // worse code. In particular, we do not want the live ranges of `a` and `b`
    // to be extended if we are not sure the initial 'SUB' instruction can be
    // removed.
    // We do not perform optimization for fp because we could lose the sign of zero.
    HSub* sub = input->AsSub();
    HSub* new_sub = new (GetGraph()->GetAllocator()) HSub(
        instruction->GetType(), sub->GetRight(), sub->GetLeft());
    instruction->GetBlock()->ReplaceAndRemoveInstructionWith(instruction, new_sub);
    if (!sub->HasUses()) {
      sub->GetBlock()->RemoveInstruction(sub);
    }
    RecordSimplification();
  }
}

void InstructionSimplifierVisitor::VisitNot(HNot* instruction) {
  HInstruction* input = instruction->GetInput();
  if (input->IsNot()) {
    // Replace code looking like
    //    NOT tmp, src
    //    NOT dst, tmp
    // with
    //    src
    // We perform the optimization even if the input negation has environment
    // uses since it allows removing the current instruction. But we only delete
    // the input negation only if it is does not have any uses left.
    HNot* previous_not = input->AsNot();
    instruction->ReplaceWith(previous_not->GetInput());
    instruction->GetBlock()->RemoveInstruction(instruction);
    if (!previous_not->HasUses()) {
      previous_not->GetBlock()->RemoveInstruction(previous_not);
    }
    RecordSimplification();
  }
}

void InstructionSimplifierVisitor::VisitOr(HOr* instruction) {
  HConstant* input_cst = instruction->GetConstantRight();
  HInstruction* input_other = instruction->GetLeastConstantLeft();

  if ((input_cst != nullptr) && input_cst->IsZeroBitPattern()) {
    // Replace code looking like
    //    OR dst, src, 0
    // with
    //    src
    instruction->ReplaceWith(input_other);
    instruction->GetBlock()->RemoveInstruction(instruction);
    RecordSimplification();
    return;
  }

  // We assume that GVN has run before, so we only perform a pointer comparison.
  // If for some reason the values are equal but the pointers are different, we
  // are still correct and only miss an optimization opportunity.
  if (instruction->GetLeft() == instruction->GetRight()) {
    // Replace code looking like
    //    OR dst, src, src
    // with
    //    src
    instruction->ReplaceWith(instruction->GetLeft());
    instruction->GetBlock()->RemoveInstruction(instruction);
    RecordSimplification();
    return;
  }

  if (TryDeMorganNegationFactoring(instruction)) return;

  if (TryReplaceWithRotate(instruction)) {
    return;
  }

  // TryHandleAssociativeAndCommutativeOperation() does not remove its input,
  // so no need to return.
  TryHandleAssociativeAndCommutativeOperation(instruction);
}

void InstructionSimplifierVisitor::VisitShl(HShl* instruction) {
  VisitShift(instruction);
}

void InstructionSimplifierVisitor::VisitShr(HShr* instruction) {
  VisitShift(instruction);
}

void InstructionSimplifierVisitor::VisitSub(HSub* instruction) {
  HConstant* input_cst = instruction->GetConstantRight();
  HInstruction* input_other = instruction->GetLeastConstantLeft();

  DataType::Type type = instruction->GetType();
  if (DataType::IsFloatingPointType(type)) {
    return;
  }

  if ((input_cst != nullptr) && input_cst->IsArithmeticZero()) {
    // Replace code looking like
    //    SUB dst, src, 0
    // with
    //    src
    // Note that we cannot optimize `x - 0.0` to `x` for floating-point. When
    // `x` is `-0.0`, the former expression yields `0.0`, while the later
    // yields `-0.0`.
    instruction->ReplaceWith(input_other);
    instruction->GetBlock()->RemoveInstruction(instruction);
    RecordSimplification();
    return;
  }

  HBasicBlock* block = instruction->GetBlock();
  ArenaAllocator* allocator = GetGraph()->GetAllocator();

  HInstruction* left = instruction->GetLeft();
  HInstruction* right = instruction->GetRight();
  if (left->IsConstant()) {
    if (Int64FromConstant(left->AsConstant()) == 0) {
      // Replace code looking like
      //    SUB dst, 0, src
      // with
      //    NEG dst, src
      // Note that we cannot optimize `0.0 - x` to `-x` for floating-point. When
      // `x` is `0.0`, the former expression yields `0.0`, while the later
      // yields `-0.0`.
      HNeg* neg = new (allocator) HNeg(type, right);
      block->ReplaceAndRemoveInstructionWith(instruction, neg);
      RecordSimplification();
      return;
    }
  }

  if (left->IsNeg() && right->IsNeg()) {
    if (TryMoveNegOnInputsAfterBinop(instruction)) {
      return;
    }
  }

  if (right->IsNeg() && right->HasOnlyOneNonEnvironmentUse()) {
    // Replace code looking like
    //    NEG tmp, b
    //    SUB dst, a, tmp
    // with
    //    ADD dst, a, b
    HAdd* add = new(GetGraph()->GetAllocator()) HAdd(type, left, right->AsNeg()->GetInput());
    instruction->GetBlock()->ReplaceAndRemoveInstructionWith(instruction, add);
    RecordSimplification();
    right->GetBlock()->RemoveInstruction(right);
    return;
  }

  if (left->IsNeg() && left->HasOnlyOneNonEnvironmentUse()) {
    // Replace code looking like
    //    NEG tmp, a
    //    SUB dst, tmp, b
    // with
    //    ADD tmp, a, b
    //    NEG dst, tmp
    // The second version is not intrinsically better, but enables more
    // transformations.
    HAdd* add = new(GetGraph()->GetAllocator()) HAdd(type, left->AsNeg()->GetInput(), right);
    instruction->GetBlock()->InsertInstructionBefore(add, instruction);
    HNeg* neg = new (GetGraph()->GetAllocator()) HNeg(instruction->GetType(), add);
    instruction->GetBlock()->InsertInstructionBefore(neg, instruction);
    instruction->ReplaceWith(neg);
    instruction->GetBlock()->RemoveInstruction(instruction);
    RecordSimplification();
    left->GetBlock()->RemoveInstruction(left);
    return;
  }

  if (TrySubtractionChainSimplification(instruction)) {
    return;
  }

  if (left->IsAdd()) {
    // Cases (x + y) - y = x, and (x + y) - x = y.
    // Replace code patterns looking like
    //    ADD dst1, x, y        ADD dst1, x, y
    //    SUB dst2, dst1, y     SUB dst2, dst1, x
    // with
    //    ADD dst1, x, y
    // SUB instruction is not needed in this case, we may use
    // one of inputs of ADD instead.
    // It is applicable to integral types only.
    HAdd* add = left->AsAdd();
    DCHECK(DataType::IsIntegralType(type));
    if (add->GetRight() == right) {
      instruction->ReplaceWith(add->GetLeft());
      RecordSimplification();
      instruction->GetBlock()->RemoveInstruction(instruction);
      return;
    } else if (add->GetLeft() == right) {
      instruction->ReplaceWith(add->GetRight());
      RecordSimplification();
      instruction->GetBlock()->RemoveInstruction(instruction);
      return;
    }
  } else if (right->IsAdd()) {
    // Cases y - (x + y) = -x, and  x - (x + y) = -y.
    // Replace code patterns looking like
    //    ADD dst1, x, y        ADD dst1, x, y
    //    SUB dst2, y, dst1     SUB dst2, x, dst1
    // with
    //    ADD dst1, x, y        ADD dst1, x, y
    //    NEG x                 NEG y
    // SUB instruction is not needed in this case, we may use
    // one of inputs of ADD instead with a NEG.
    // It is applicable to integral types only.
    HAdd* add = right->AsAdd();
    DCHECK(DataType::IsIntegralType(type));
    if (add->GetRight() == left) {
      HNeg* neg = new (GetGraph()->GetAllocator()) HNeg(add->GetType(), add->GetLeft());
      instruction->GetBlock()->ReplaceAndRemoveInstructionWith(instruction, neg);
      RecordSimplification();
      return;
    } else if (add->GetLeft() == left) {
      HNeg* neg = new (GetGraph()->GetAllocator()) HNeg(add->GetType(), add->GetRight());
      instruction->GetBlock()->ReplaceAndRemoveInstructionWith(instruction, neg);
      RecordSimplification();
      return;
    }
  } else if (left->IsSub()) {
    // Case (x - y) - x = -y.
    // Replace code patterns looking like
    //    SUB dst1, x, y
    //    SUB dst2, dst1, x
    // with
    //    SUB dst1, x, y
    //    NEG y
    // The second SUB is not needed in this case, we may use the second input of the first SUB
    // instead with a NEG.
    // It is applicable to integral types only.
    HSub* sub = left->AsSub();
    DCHECK(DataType::IsIntegralType(type));
    if (sub->GetLeft() == right) {
      HNeg* neg = new (GetGraph()->GetAllocator()) HNeg(sub->GetType(), sub->GetRight());
      instruction->GetBlock()->ReplaceAndRemoveInstructionWith(instruction, neg);
      RecordSimplification();
      return;
    }
  } else if (right->IsSub()) {
    // Case x - (x - y) = y.
    // Replace code patterns looking like
    //    SUB dst1, x, y
    //    SUB dst2, x, dst1
    // with
    //    SUB dst1, x, y
    // The second SUB is not needed in this case, we may use the second input of the first SUB.
    // It is applicable to integral types only.
    HSub* sub = right->AsSub();
    DCHECK(DataType::IsIntegralType(type));
    if (sub->GetLeft() == left) {
      instruction->ReplaceWith(sub->GetRight());
      RecordSimplification();
      instruction->GetBlock()->RemoveInstruction(instruction);
      return;
    }
  }
}

void InstructionSimplifierVisitor::VisitUShr(HUShr* instruction) {
  VisitShift(instruction);
}

void InstructionSimplifierVisitor::VisitXor(HXor* instruction) {
  HConstant* input_cst = instruction->GetConstantRight();
  HInstruction* input_other = instruction->GetLeastConstantLeft();

  if ((input_cst != nullptr) && input_cst->IsZeroBitPattern()) {
    // Replace code looking like
    //    XOR dst, src, 0
    // with
    //    src
    instruction->ReplaceWith(input_other);
    instruction->GetBlock()->RemoveInstruction(instruction);
    RecordSimplification();
    return;
  }

  if ((input_cst != nullptr) && input_cst->IsOne()
      && input_other->GetType() == DataType::Type::kBool) {
    // Replace code looking like
    //    XOR dst, src, 1
    // with
    //    BOOLEAN_NOT dst, src
    HBooleanNot* boolean_not = new (GetGraph()->GetAllocator()) HBooleanNot(input_other);
    instruction->GetBlock()->ReplaceAndRemoveInstructionWith(instruction, boolean_not);
    RecordSimplification();
    return;
  }

  if ((input_cst != nullptr) && AreAllBitsSet(input_cst)) {
    // Replace code looking like
    //    XOR dst, src, 0xFFF...FF
    // with
    //    NOT dst, src
    HNot* bitwise_not = new (GetGraph()->GetAllocator()) HNot(instruction->GetType(), input_other);
    instruction->GetBlock()->ReplaceAndRemoveInstructionWith(instruction, bitwise_not);
    RecordSimplification();
    return;
  }

  HInstruction* left = instruction->GetLeft();
  HInstruction* right = instruction->GetRight();
  if (((left->IsNot() && right->IsNot()) ||
       (left->IsBooleanNot() && right->IsBooleanNot())) &&
      left->HasOnlyOneNonEnvironmentUse() &&
      right->HasOnlyOneNonEnvironmentUse()) {
    // Replace code looking like
    //    NOT nota, a
    //    NOT notb, b
    //    XOR dst, nota, notb
    // with
    //    XOR dst, a, b
    instruction->ReplaceInput(left->InputAt(0), 0);
    instruction->ReplaceInput(right->InputAt(0), 1);
    left->GetBlock()->RemoveInstruction(left);
    right->GetBlock()->RemoveInstruction(right);
    RecordSimplification();
    return;
  }

  if (TryReplaceWithRotate(instruction)) {
    return;
  }

  // TryHandleAssociativeAndCommutativeOperation() does not remove its input,
  // so no need to return.
  TryHandleAssociativeAndCommutativeOperation(instruction);
}

void InstructionSimplifierVisitor::SimplifyBoxUnbox(
    HInvoke* instruction, ArtField* field, DataType::Type type) {
  DCHECK(instruction->GetIntrinsic() == Intrinsics::kByteValueOf ||
         instruction->GetIntrinsic() == Intrinsics::kShortValueOf ||
         instruction->GetIntrinsic() == Intrinsics::kCharacterValueOf ||
         instruction->GetIntrinsic() == Intrinsics::kIntegerValueOf);
  const HUseList<HInstruction*>& uses = instruction->GetUses();
  for (auto it = uses.begin(), end = uses.end(); it != end;) {
    HInstruction* user = it->GetUser();
    ++it;  // Increment the iterator before we potentially remove the node from the list.
    if (user->IsInstanceFieldGet() &&
        user->AsInstanceFieldGet()->GetFieldInfo().GetField() == field &&
        // Note: Due to other simplifications, we may have an `HInstanceFieldGet` with
        // a different type (Int8 vs. Uint8, Int16 vs. Uint16) for the same field.
        // Do not optimize that case for now. (We would need to insert a `HTypeConversion`.)
        user->GetType() == type) {
      user->ReplaceWith(instruction->InputAt(0));
      RecordSimplification();
      // Do not remove `user` while we're iterating over the block's instructions. Let DCE do it.
    }
  }
}

void InstructionSimplifierVisitor::SimplifyStringEquals(HInvoke* instruction) {
  HInstruction* argument = instruction->InputAt(1);
  HInstruction* receiver = instruction->InputAt(0);
  if (receiver == argument) {
    // Because String.equals is an instance call, the receiver is
    // a null check if we don't know it's null. The argument however, will
    // be the actual object. So we cannot end up in a situation where both
    // are equal but could be null.
    DCHECK(CanEnsureNotNullAt(argument, instruction));
    instruction->ReplaceWith(GetGraph()->GetIntConstant(1));
    instruction->GetBlock()->RemoveInstruction(instruction);
  } else {
    StringEqualsOptimizations optimizations(instruction);
    if (CanEnsureNotNullAt(argument, instruction)) {
      optimizations.SetArgumentNotNull();
    }
    ScopedObjectAccess soa(Thread::Current());
    ReferenceTypeInfo argument_rti = argument->GetReferenceTypeInfo();
    if (argument_rti.IsValid() && argument_rti.IsStringClass()) {
      optimizations.SetArgumentIsString();
    }
  }
}

static bool IsArrayLengthOf(HInstruction* potential_length, HInstruction* potential_array) {
  if (potential_length->IsArrayLength()) {
    return potential_length->InputAt(0) == potential_array;
  }

  if (potential_array->IsNewArray()) {
    return potential_array->AsNewArray()->GetLength() == potential_length;
  }

  return false;
}

void InstructionSimplifierVisitor::SimplifySystemArrayCopy(HInvoke* instruction) {
  HInstruction* source = instruction->InputAt(0);
  HInstruction* source_pos = instruction->InputAt(1);
  HInstruction* destination = instruction->InputAt(2);
  HInstruction* destination_pos = instruction->InputAt(3);
  HInstruction* count = instruction->InputAt(4);
  SystemArrayCopyOptimizations optimizations(instruction);
  if (CanEnsureNotNullAt(source, instruction)) {
    optimizations.SetSourceIsNotNull();
  }
  if (CanEnsureNotNullAt(destination, instruction)) {
    optimizations.SetDestinationIsNotNull();
  }
  if (destination == source) {
    optimizations.SetDestinationIsSource();
  }

  if (source_pos == destination_pos) {
    optimizations.SetSourcePositionIsDestinationPosition();
  }

  if (IsArrayLengthOf(count, source)) {
    optimizations.SetCountIsSourceLength();
  }

  if (IsArrayLengthOf(count, destination)) {
    optimizations.SetCountIsDestinationLength();
  }

  {
    ScopedObjectAccess soa(Thread::Current());
    DataType::Type source_component_type = DataType::Type::kVoid;
    DataType::Type destination_component_type = DataType::Type::kVoid;
    ReferenceTypeInfo destination_rti = destination->GetReferenceTypeInfo();
    if (destination_rti.IsValid()) {
      if (destination_rti.IsObjectArray()) {
        if (destination_rti.IsExact()) {
          optimizations.SetDoesNotNeedTypeCheck();
        }
        optimizations.SetDestinationIsTypedObjectArray();
      }
      if (destination_rti.IsPrimitiveArrayClass()) {
        destination_component_type = DataTypeFromPrimitive(
            destination_rti.GetTypeHandle()->GetComponentType()->GetPrimitiveType());
        optimizations.SetDestinationIsPrimitiveArray();
      } else if (destination_rti.IsNonPrimitiveArrayClass()) {
        optimizations.SetDestinationIsNonPrimitiveArray();
      }
    }
    ReferenceTypeInfo source_rti = source->GetReferenceTypeInfo();
    if (source_rti.IsValid()) {
      if (destination_rti.IsValid() && destination_rti.CanArrayHoldValuesOf(source_rti)) {
        optimizations.SetDoesNotNeedTypeCheck();
      }
      if (source_rti.IsPrimitiveArrayClass()) {
        optimizations.SetSourceIsPrimitiveArray();
        source_component_type = DataTypeFromPrimitive(
            source_rti.GetTypeHandle()->GetComponentType()->GetPrimitiveType());
      } else if (source_rti.IsNonPrimitiveArrayClass()) {
        optimizations.SetSourceIsNonPrimitiveArray();
      }
    }
    // For primitive arrays, use their optimized ArtMethod implementations.
    if ((source_component_type != DataType::Type::kVoid) &&
        (source_component_type == destination_component_type)) {
      ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
      PointerSize image_size = class_linker->GetImagePointerSize();
      HInvokeStaticOrDirect* invoke = instruction->AsInvokeStaticOrDirect();
      ObjPtr<mirror::Class> system = invoke->GetResolvedMethod()->GetDeclaringClass();
      ArtMethod* method = nullptr;
      switch (source_component_type) {
        case DataType::Type::kBool:
          method = system->FindClassMethod("arraycopy", "([ZI[ZII)V", image_size);
          break;
        case DataType::Type::kInt8:
          method = system->FindClassMethod("arraycopy", "([BI[BII)V", image_size);
          break;
        case DataType::Type::kUint16:
          method = system->FindClassMethod("arraycopy", "([CI[CII)V", image_size);
          break;
        case DataType::Type::kInt16:
          method = system->FindClassMethod("arraycopy", "([SI[SII)V", image_size);
          break;
        case DataType::Type::kInt32:
          method = system->FindClassMethod("arraycopy", "([II[III)V", image_size);
          break;
        case DataType::Type::kFloat32:
          method = system->FindClassMethod("arraycopy", "([FI[FII)V", image_size);
          break;
        case DataType::Type::kInt64:
          method = system->FindClassMethod("arraycopy", "([JI[JII)V", image_size);
          break;
        case DataType::Type::kFloat64:
          method = system->FindClassMethod("arraycopy", "([DI[DII)V", image_size);
          break;
        default:
          LOG(FATAL) << "Unreachable";
      }
      DCHECK(method != nullptr);
      DCHECK(method->IsStatic());
      DCHECK(method->GetDeclaringClass() == system);
      invoke->SetResolvedMethod(method, !codegen_->GetGraph()->IsDebuggable());
      // Sharpen the new invoke. Note that we do not update the dex method index of
      // the invoke, as we would need to look it up in the current dex file, and it
      // is unlikely that it exists. The most usual situation for such typed
      // arraycopy methods is a direct pointer to the boot image.
      invoke->SetDispatchInfo(HSharpening::SharpenLoadMethod(
          method,
          /* has_method_id= */ true,
          /* for_interface_call= */ false,
          codegen_));
    }
  }
}

void InstructionSimplifierVisitor::SimplifyFP2Int(HInvoke* invoke) {
  DCHECK(invoke->IsInvokeStaticOrDirect());
  uint32_t dex_pc = invoke->GetDexPc();
  HInstruction* x = invoke->InputAt(0);
  DataType::Type type = x->GetType();
  // Set proper bit pattern for NaN and replace intrinsic with raw version.
  HInstruction* nan;
  if (type == DataType::Type::kFloat64) {
    nan = GetGraph()->GetLongConstant(0x7ff8000000000000L);
    invoke->SetIntrinsic(Intrinsics::kDoubleDoubleToRawLongBits,
                         kNeedsEnvironment,
                         kNoSideEffects,
                         kNoThrow);
  } else {
    DCHECK_EQ(type, DataType::Type::kFloat32);
    nan = GetGraph()->GetIntConstant(0x7fc00000);
    invoke->SetIntrinsic(Intrinsics::kFloatFloatToRawIntBits,
                         kNeedsEnvironment,
                         kNoSideEffects,
                         kNoThrow);
  }
  // Test IsNaN(x), which is the same as x != x.
  HCondition* condition = new (GetGraph()->GetAllocator()) HNotEqual(x, x, dex_pc);
  condition->SetBias(ComparisonBias::kLtBias);
  invoke->GetBlock()->InsertInstructionBefore(condition, invoke->GetNext());
  // Select between the two.
  HInstruction* select = new (GetGraph()->GetAllocator()) HSelect(condition, nan, invoke, dex_pc);
  invoke->GetBlock()->InsertInstructionBefore(select, condition->GetNext());
  invoke->ReplaceWithExceptInReplacementAtIndex(select, 0);  // false at index 0
}

void InstructionSimplifierVisitor::SimplifyStringCharAt(HInvoke* invoke) {
  HInstruction* str = invoke->InputAt(0);
  HInstruction* index = invoke->InputAt(1);
  uint32_t dex_pc = invoke->GetDexPc();
  ArenaAllocator* allocator = GetGraph()->GetAllocator();
  // We treat String as an array to allow DCE and BCE to seamlessly work on strings,
  // so create the HArrayLength, HBoundsCheck and HArrayGet.
  HArrayLength* length = new (allocator) HArrayLength(str, dex_pc, /* is_string_length= */ true);
  invoke->GetBlock()->InsertInstructionBefore(length, invoke);
  HBoundsCheck* bounds_check = new (allocator) HBoundsCheck(
      index, length, dex_pc, /* is_string_char_at= */ true);
  invoke->GetBlock()->InsertInstructionBefore(bounds_check, invoke);
  HArrayGet* array_get = new (allocator) HArrayGet(str,
                                                   bounds_check,
                                                   DataType::Type::kUint16,
                                                   SideEffects::None(),  // Strings are immutable.
                                                   dex_pc,
                                                   /* is_string_char_at= */ true);
  invoke->GetBlock()->ReplaceAndRemoveInstructionWith(invoke, array_get);
  bounds_check->CopyEnvironmentFrom(invoke->GetEnvironment());
  GetGraph()->SetHasBoundsChecks(true);
}

void InstructionSimplifierVisitor::SimplifyStringLength(HInvoke* invoke) {
  HInstruction* str = invoke->InputAt(0);
  uint32_t dex_pc = invoke->GetDexPc();
  // We treat String as an array to allow DCE and BCE to seamlessly work on strings,
  // so create the HArrayLength.
  HArrayLength* length =
      new (GetGraph()->GetAllocator()) HArrayLength(str, dex_pc, /* is_string_length= */ true);
  invoke->GetBlock()->ReplaceAndRemoveInstructionWith(invoke, length);
}

void InstructionSimplifierVisitor::SimplifyStringIndexOf(HInvoke* invoke) {
  DCHECK(invoke->GetIntrinsic() == Intrinsics::kStringIndexOf ||
         invoke->GetIntrinsic() == Intrinsics::kStringIndexOfAfter);
  if (invoke->InputAt(0)->IsLoadString()) {
    HLoadString* load_string = invoke->InputAt(0)->AsLoadString();
    const DexFile& dex_file = load_string->GetDexFile();
    uint32_t utf16_length;
    const char* data =
        dex_file.GetStringDataAndUtf16Length(load_string->GetStringIndex(), &utf16_length);
    if (utf16_length == 0) {
      invoke->ReplaceWith(GetGraph()->GetIntConstant(-1));
      invoke->GetBlock()->RemoveInstruction(invoke);
      RecordSimplification();
      return;
    }
    if (utf16_length == 1 && invoke->GetIntrinsic() == Intrinsics::kStringIndexOf) {
      // Simplify to HSelect(HEquals(., load_string.charAt(0)), 0, -1).
      // If the sought character is supplementary, this gives the correct result, i.e. -1.
      uint32_t c = GetUtf16FromUtf8(&data);
      DCHECK_EQ(GetTrailingUtf16Char(c), 0u);
      DCHECK_EQ(GetLeadingUtf16Char(c), c);
      uint32_t dex_pc = invoke->GetDexPc();
      ArenaAllocator* allocator = GetGraph()->GetAllocator();
      HEqual* equal =
          new (allocator) HEqual(invoke->InputAt(1), GetGraph()->GetIntConstant(c), dex_pc);
      invoke->GetBlock()->InsertInstructionBefore(equal, invoke);
      HSelect* result = new (allocator) HSelect(equal,
                                                GetGraph()->GetIntConstant(0),
                                                GetGraph()->GetIntConstant(-1),
                                                dex_pc);
      invoke->GetBlock()->ReplaceAndRemoveInstructionWith(invoke, result);
      RecordSimplification();
      return;
    }
  }
}

// This method should only be used on intrinsics whose sole way of throwing an
// exception is raising a NPE when the nth argument is null. If that argument
// is provably non-null, we can clear the flag.
void InstructionSimplifierVisitor::SimplifyNPEOnArgN(HInvoke* invoke, size_t n) {
  HInstruction* arg = invoke->InputAt(n);
  if (invoke->CanThrow() && !arg->CanBeNull()) {
    invoke->SetCanThrow(false);
  }
}

// Methods that return "this" can replace the returned value with the receiver.
void InstructionSimplifierVisitor::SimplifyReturnThis(HInvoke* invoke) {
  if (invoke->HasUses()) {
    HInstruction* receiver = invoke->InputAt(0);
    invoke->ReplaceWith(receiver);
    RecordSimplification();
  }
}

// Helper method for StringBuffer escape analysis.
static bool NoEscapeForStringBufferReference(HInstruction* reference, HInstruction* user) {
  if (user->IsInvoke()) {
    switch (user->AsInvoke()->GetIntrinsic()) {
      case Intrinsics::kStringBufferLength:
      case Intrinsics::kStringBufferToString:
        DCHECK_EQ(user->InputAt(0), reference);
        return true;
      case Intrinsics::kStringBufferAppend:
        // Returns "this", so only okay if no further uses.
        DCHECK_EQ(user->InputAt(0), reference);
        DCHECK_NE(user->InputAt(1), reference);
        return !user->HasUses();
      default:
        break;
    }
  }

  if (user->IsInvokeStaticOrDirect()) {
    // Any constructor on StringBuffer is okay.
    return user->AsInvokeStaticOrDirect()->GetResolvedMethod() != nullptr &&
           user->AsInvokeStaticOrDirect()->GetResolvedMethod()->IsConstructor() &&
           user->InputAt(0) == reference;
  }

  return false;
}

static bool TryReplaceStringBuilderAppend(CodeGenerator* codegen, HInvoke* invoke) {
  DCHECK_EQ(invoke->GetIntrinsic(), Intrinsics::kStringBuilderToString);
  if (invoke->CanThrowIntoCatchBlock()) {
    return false;
  }

  HBasicBlock* block = invoke->GetBlock();
  HInstruction* sb = invoke->InputAt(0);

  // We support only a new StringBuilder, otherwise we cannot ensure that
  // the StringBuilder data does not need to be populated for other users.
  if (!sb->IsNewInstance()) {
    return false;
  }

  // For now, we support only single-block recognition.
  // (Ternary operators feeding the append could be implemented.)
  for (const HUseListNode<HInstruction*>& use : sb->GetUses()) {
    if (use.GetUser()->GetBlock() != block) {
      return false;
    }
    // The append pattern uses the StringBuilder only as the first argument.
    if (use.GetIndex() != 0u) {
      return false;
    }
  }

  // Collect args and check for unexpected uses.
  // We expect one call to a constructor with no arguments, one constructor fence (unless
  // eliminated), some number of append calls and one call to StringBuilder.toString().
  bool seen_constructor = false;
  bool seen_constructor_fence = false;
  bool seen_to_string = false;
  uint32_t format = 0u;
  uint32_t num_args = 0u;
  bool has_fp_args = false;
  HInstruction* args[StringBuilderAppend::kMaxArgs];  // Added in reverse order.
  for (HBackwardInstructionIterator iter(block->GetInstructions()); !iter.Done(); iter.Advance()) {
    HInstruction* user = iter.Current();
    // Instructions of interest apply to `sb`, skip those that do not involve `sb`.
    if (user->InputCount() == 0u || user->InputAt(0u) != sb) {
      continue;
    }
    // We visit the uses in reverse order, so the StringBuilder.toString() must come first.
    if (!seen_to_string) {
      if (user == invoke) {
        seen_to_string = true;
        continue;
      } else {
        return false;
      }
    }

    // Pattern match seeing arguments, then constructor, then constructor fence.
    if (user->IsInvokeStaticOrDirect() &&
        user->AsInvokeStaticOrDirect()->GetResolvedMethod() != nullptr &&
        user->AsInvokeStaticOrDirect()->GetResolvedMethod()->IsConstructor() &&
        user->AsInvokeStaticOrDirect()->GetNumberOfArguments() == 1u) {
      // After arguments, we should see the constructor.
      // We accept only the constructor with no extra arguments.
      DCHECK(!seen_constructor);
      DCHECK(!seen_constructor_fence);
      seen_constructor = true;
    } else if (user->IsInvoke()) {
      // The arguments.
      HInvoke* as_invoke = user->AsInvoke();
      DCHECK(!seen_constructor);
      DCHECK(!seen_constructor_fence);
      StringBuilderAppend::Argument arg;
      switch (as_invoke->GetIntrinsic()) {
        case Intrinsics::kStringBuilderAppendObject:
          // TODO: Unimplemented, needs to call String.valueOf().
          return false;
        case Intrinsics::kStringBuilderAppendString:
          arg = StringBuilderAppend::Argument::kString;
          break;
        case Intrinsics::kStringBuilderAppendCharArray:
          // TODO: Unimplemented, StringBuilder.append(char[]) can throw NPE and we would
          // not have the correct stack trace for it.
          return false;
        case Intrinsics::kStringBuilderAppendBoolean:
          arg = StringBuilderAppend::Argument::kBoolean;
          break;
        case Intrinsics::kStringBuilderAppendChar:
          arg = StringBuilderAppend::Argument::kChar;
          break;
        case Intrinsics::kStringBuilderAppendInt:
          arg = StringBuilderAppend::Argument::kInt;
          break;
        case Intrinsics::kStringBuilderAppendLong:
          arg = StringBuilderAppend::Argument::kLong;
          break;
        case Intrinsics::kStringBuilderAppendFloat:
          arg = StringBuilderAppend::Argument::kFloat;
          has_fp_args = true;
          break;
        case Intrinsics::kStringBuilderAppendDouble:
          arg = StringBuilderAppend::Argument::kDouble;
          has_fp_args = true;
          break;
        case Intrinsics::kStringBuilderAppendCharSequence: {
          ReferenceTypeInfo rti = as_invoke->InputAt(1)->GetReferenceTypeInfo();
          if (!rti.IsValid()) {
            return false;
          }
          ScopedObjectAccess soa(Thread::Current());
          Handle<mirror::Class> input_type = rti.GetTypeHandle();
          DCHECK(input_type != nullptr);
          if (input_type.Get() == GetClassRoot<mirror::String>()) {
            arg = StringBuilderAppend::Argument::kString;
          } else {
            // TODO: Check and implement for StringBuilder. We could find the StringBuilder's
            // internal char[] inconsistent with the length, or the string compression
            // of the result could be compromised with a concurrent modification, and
            // we would need to throw appropriate exceptions.
            return false;
          }
          break;
        }
        default: {
          return false;
        }
      }
      // Uses of the append return value should have been replaced with the first input.
      DCHECK(!as_invoke->HasUses());
      DCHECK(!as_invoke->HasEnvironmentUses());
      if (num_args == StringBuilderAppend::kMaxArgs) {
        return false;
      }
      format = (format << StringBuilderAppend::kBitsPerArg) | static_cast<uint32_t>(arg);
      args[num_args] = as_invoke->InputAt(1u);
      ++num_args;
    } else if (user->IsConstructorFence()) {
      // The last use we see is the constructor fence.
      DCHECK(seen_constructor);
      DCHECK(!seen_constructor_fence);
      seen_constructor_fence = true;
    } else {
      return false;
    }
  }

  if (num_args == 0u) {
    return false;
  }

  // Check environment uses.
  for (const HUseListNode<HEnvironment*>& use : sb->GetEnvUses()) {
    HInstruction* holder = use.GetUser()->GetHolder();
    if (holder->GetBlock() != block) {
      return false;
    }
    // Accept only calls on the StringBuilder (which shall all be removed).
    // TODO: Carve-out for const-string? Or rely on environment pruning (to be implemented)?
    if (holder->InputCount() == 0 || holder->InputAt(0) != sb) {
      return false;
    }
  }

  // Calculate outgoing vregs, including padding for 64-bit arg alignment.
  const PointerSize pointer_size = InstructionSetPointerSize(codegen->GetInstructionSet());
  const size_t method_vregs = static_cast<size_t>(pointer_size) / kVRegSize;
  uint32_t number_of_out_vregs = method_vregs;  // For correct alignment padding; subtracted below.
  for (uint32_t f = format; f != 0u; f >>= StringBuilderAppend::kBitsPerArg) {
    auto a = enum_cast<StringBuilderAppend::Argument>(f & StringBuilderAppend::kArgMask);
    if (a == StringBuilderAppend::Argument::kLong || a == StringBuilderAppend::Argument::kDouble) {
      number_of_out_vregs += /* alignment */ ((number_of_out_vregs) & 1u) + /* vregs */ 2u;
    } else {
      number_of_out_vregs += /* vregs */ 1u;
    }
  }
  number_of_out_vregs -= method_vregs;

  // Create replacement instruction.
  HIntConstant* fmt = block->GetGraph()->GetIntConstant(static_cast<int32_t>(format));
  ArenaAllocator* allocator = block->GetGraph()->GetAllocator();
  HStringBuilderAppend* append = new (allocator) HStringBuilderAppend(
      fmt, num_args, number_of_out_vregs, has_fp_args, allocator, invoke->GetDexPc());
  append->SetReferenceTypeInfoIfValid(invoke->GetReferenceTypeInfo());
  for (size_t i = 0; i != num_args; ++i) {
    append->SetArgumentAt(i, args[num_args - 1u - i]);
  }
  block->InsertInstructionBefore(append, invoke);
  DCHECK(!invoke->CanBeNull());
  DCHECK(!append->CanBeNull());
  invoke->ReplaceWith(append);
  // Copy environment, except for the StringBuilder uses.
  for (HEnvironment* env = invoke->GetEnvironment(); env != nullptr; env = env->GetParent()) {
    for (size_t i = 0, size = env->Size(); i != size; ++i) {
      if (env->GetInstructionAt(i) == sb) {
        env->RemoveAsUserOfInput(i);
        env->SetRawEnvAt(i, /*instruction=*/ nullptr);
      }
    }
  }
  append->CopyEnvironmentFrom(invoke->GetEnvironment());
  // Remove the old instruction.
  block->RemoveInstruction(invoke);
  // Remove the StringBuilder's uses and StringBuilder.
  while (sb->HasNonEnvironmentUses()) {
    block->RemoveInstruction(sb->GetUses().front().GetUser());
  }
  DCHECK(!sb->HasEnvironmentUses());
  block->RemoveInstruction(sb);
  return true;
}

// Certain allocation intrinsics are not removed by dead code elimination
// because of potentially throwing an OOM exception or other side effects.
// This method removes such intrinsics when special circumstances allow.
void InstructionSimplifierVisitor::SimplifyAllocationIntrinsic(HInvoke* invoke) {
  if (!invoke->HasUses()) {
    // Instruction has no uses. If unsynchronized, we can remove right away, safely ignoring
    // the potential OOM of course. Otherwise, we must ensure the receiver object of this
    // call does not escape since only thread-local synchronization may be removed.
    bool is_synchronized = invoke->GetIntrinsic() == Intrinsics::kStringBufferToString;
    HInstruction* receiver = invoke->InputAt(0);
    if (!is_synchronized || DoesNotEscape(receiver, NoEscapeForStringBufferReference)) {
      invoke->GetBlock()->RemoveInstruction(invoke);
      RecordSimplification();
    }
  } else if (invoke->GetIntrinsic() == Intrinsics::kStringBuilderToString &&
             TryReplaceStringBuilderAppend(codegen_, invoke)) {
    RecordSimplification();
  }
}

void InstructionSimplifierVisitor::SimplifyVarHandleIntrinsic(HInvoke* invoke) {
  DCHECK(invoke->IsInvokePolymorphic());
  VarHandleOptimizations optimizations(invoke);

  if (optimizations.GetDoNotIntrinsify()) {
    // Preceding static checks disabled intrinsic, so no need to analyze further.
    return;
  }

  size_t expected_coordinates_count = GetExpectedVarHandleCoordinatesCount(invoke);
  if (expected_coordinates_count != 0u) {
    HInstruction* object = invoke->InputAt(1);
    // The following has been ensured by static checks in the instruction builder.
    DCHECK(object->GetType() == DataType::Type::kReference);
    // Re-check for null constant, as this might have changed after the inliner.
    if (object->IsNullConstant()) {
      optimizations.SetDoNotIntrinsify();
      return;
    }
    // Test whether we can avoid the null check on the object.
    if (CanEnsureNotNullAt(object, invoke)) {
      optimizations.SetSkipObjectNullCheck();
    }
  }

  if (CanUseKnownImageVarHandle(invoke)) {
    optimizations.SetUseKnownImageVarHandle();
  }
}

bool InstructionSimplifierVisitor::CanUseKnownImageVarHandle(HInvoke* invoke) {
  // If the `VarHandle` comes from a static final field of an initialized class in an image
  // (boot image or app image), we can do the checks at compile time. We do this optimization
  // only for AOT and only for field handles when we can avoid all checks. This avoids the
  // possibility of the code concurrently messing with the `VarHandle` using reflection,
  // we simply perform the operation with the `VarHandle` as seen at compile time.
  // TODO: Extend this to arrays to support the `AtomicIntegerArray` class.
  const CompilerOptions& compiler_options = codegen_->GetCompilerOptions();
  if (!compiler_options.IsAotCompiler()) {
    return false;
  }
  size_t expected_coordinates_count = GetExpectedVarHandleCoordinatesCount(invoke);
  if (expected_coordinates_count == 2u) {
    return false;
  }
  HInstruction* var_handle_instruction = invoke->InputAt(0);
  if (var_handle_instruction->IsNullCheck()) {
    var_handle_instruction = var_handle_instruction->InputAt(0);
  }
  if (!var_handle_instruction->IsStaticFieldGet()) {
    return false;
  }
  ArtField* field = var_handle_instruction->AsStaticFieldGet()->GetFieldInfo().GetField();
  DCHECK(field->IsStatic());
  if (!field->IsFinal()) {
    return false;
  }
  ScopedObjectAccess soa(Thread::Current());
  ObjPtr<mirror::Class> declaring_class = field->GetDeclaringClass();
  if (!declaring_class->IsVisiblyInitialized()) {
    // During AOT compilation, dex2oat ensures that initialized classes are visibly initialized.
    DCHECK(!declaring_class->IsInitialized());
    return false;
  }
  HInstruction* load_class = var_handle_instruction->InputAt(0);
  if (kIsDebugBuild) {
    bool is_in_image = false;
    if (Runtime::Current()->GetHeap()->ObjectIsInBootImageSpace(declaring_class)) {
      is_in_image = true;
    } else if (compiler_options.IsGeneratingImage()) {
      std::string storage;
      const char* descriptor = declaring_class->GetDescriptor(&storage);
      is_in_image = compiler_options.IsImageClass(descriptor);
    }
    CHECK_EQ(is_in_image, load_class->IsLoadClass() && load_class->AsLoadClass()->IsInImage());
  }
  if (!load_class->IsLoadClass() || !load_class->AsLoadClass()->IsInImage()) {
    return false;
  }

  // Get the `VarHandle` object and check its class.
  ObjPtr<mirror::Class> expected_var_handle_class;
  switch (expected_coordinates_count) {
    case 0:
      expected_var_handle_class = GetClassRoot<mirror::StaticFieldVarHandle>();
      break;
    default:
      DCHECK_EQ(expected_coordinates_count, 1u);
      expected_var_handle_class = GetClassRoot<mirror::FieldVarHandle>();
      break;
  }
  ObjPtr<mirror::Object> var_handle_object = field->GetObject(declaring_class);
  if (var_handle_object == nullptr || var_handle_object->GetClass() != expected_var_handle_class) {
    return false;
  }
  ObjPtr<mirror::VarHandle> var_handle = ObjPtr<mirror::VarHandle>::DownCast(var_handle_object);

  // Check access mode.
  mirror::VarHandle::AccessMode access_mode =
      mirror::VarHandle::GetAccessModeByIntrinsic(invoke->GetIntrinsic());
  if (!var_handle->IsAccessModeSupported(access_mode)) {
    return false;
  }

  // Check argument types.
  ObjPtr<mirror::Class> var_type = var_handle->GetVarType();
  mirror::VarHandle::AccessModeTemplate access_mode_template =
      mirror::VarHandle::GetAccessModeTemplate(access_mode);
  // Note: The data type of input arguments does not need to match the type from shorty
  // due to implicit conversions or avoiding unnecessary conversions before narrow stores.
  DataType::Type type = (access_mode_template == mirror::VarHandle::AccessModeTemplate::kGet)
      ? invoke->GetType()
      : GetDataTypeFromShorty(invoke, invoke->GetNumberOfArguments() - 1u);
  if (type != DataTypeFromPrimitive(var_type->GetPrimitiveType())) {
    return false;
  }
  if (type == DataType::Type::kReference) {
    uint32_t arguments_start = /* VarHandle object */ 1u + expected_coordinates_count;
    uint32_t number_of_arguments = invoke->GetNumberOfArguments();
    for (size_t arg_index = arguments_start; arg_index != number_of_arguments; ++arg_index) {
      HInstruction* arg = invoke->InputAt(arg_index);
      DCHECK_EQ(arg->GetType(), DataType::Type::kReference);
      if (!arg->IsNullConstant()) {
        ReferenceTypeInfo arg_type_info = arg->GetReferenceTypeInfo();
        if (!arg_type_info.IsValid() ||
            !var_type->IsAssignableFrom(arg_type_info.GetTypeHandle().Get())) {
          return false;
        }
      }
    }
  }

  // Check the first coordinate.
  if (expected_coordinates_count != 0u) {
    ObjPtr<mirror::Class> coordinate0_type = var_handle->GetCoordinateType0();
    DCHECK(coordinate0_type != nullptr);
    ReferenceTypeInfo object_type_info = invoke->InputAt(1)->GetReferenceTypeInfo();
    if (!object_type_info.IsValid() ||
        !coordinate0_type->IsAssignableFrom(object_type_info.GetTypeHandle().Get())) {
      return false;
    }
  }

  // All required checks passed.
  return true;
}

void InstructionSimplifierVisitor::VisitInvoke(HInvoke* instruction) {
  switch (instruction->GetIntrinsic()) {
#define SIMPLIFY_BOX_UNBOX(name, low, high, type, start_index) \
    case Intrinsics::k ## name ## ValueOf: \
      SimplifyBoxUnbox(instruction, WellKnownClasses::java_lang_##name##_value, type); \
      break;
    BOXED_TYPES(SIMPLIFY_BOX_UNBOX)
#undef SIMPLIFY_BOX_UNBOX
    case Intrinsics::kStringEquals:
      SimplifyStringEquals(instruction);
      break;
    case Intrinsics::kSystemArrayCopy:
      SimplifySystemArrayCopy(instruction);
      break;
    case Intrinsics::kFloatFloatToIntBits:
    case Intrinsics::kDoubleDoubleToLongBits:
      SimplifyFP2Int(instruction);
      break;
    case Intrinsics::kStringCharAt:
      // Instruction builder creates intermediate representation directly
      // but the inliner can sharpen CharSequence.charAt() to String.charAt().
      SimplifyStringCharAt(instruction);
      break;
    case Intrinsics::kStringLength:
      // Instruction builder creates intermediate representation directly
      // but the inliner can sharpen CharSequence.length() to String.length().
      SimplifyStringLength(instruction);
      break;
    case Intrinsics::kStringIndexOf:
    case Intrinsics::kStringIndexOfAfter:
      SimplifyStringIndexOf(instruction);
      break;
    case Intrinsics::kStringStringIndexOf:
    case Intrinsics::kStringStringIndexOfAfter:
      SimplifyNPEOnArgN(instruction, 1);  // 0th has own NullCheck
      break;
    case Intrinsics::kStringBufferAppend:
    case Intrinsics::kStringBuilderAppendObject:
    case Intrinsics::kStringBuilderAppendString:
    case Intrinsics::kStringBuilderAppendCharSequence:
    case Intrinsics::kStringBuilderAppendCharArray:
    case Intrinsics::kStringBuilderAppendBoolean:
    case Intrinsics::kStringBuilderAppendChar:
    case Intrinsics::kStringBuilderAppendInt:
    case Intrinsics::kStringBuilderAppendLong:
    case Intrinsics::kStringBuilderAppendFloat:
    case Intrinsics::kStringBuilderAppendDouble:
      SimplifyReturnThis(instruction);
      break;
    case Intrinsics::kStringBufferToString:
    case Intrinsics::kStringBuilderToString:
      SimplifyAllocationIntrinsic(instruction);
      break;
    case Intrinsics::kVarHandleCompareAndExchange:
    case Intrinsics::kVarHandleCompareAndExchangeAcquire:
    case Intrinsics::kVarHandleCompareAndExchangeRelease:
    case Intrinsics::kVarHandleCompareAndSet:
    case Intrinsics::kVarHandleGet:
    case Intrinsics::kVarHandleGetAcquire:
    case Intrinsics::kVarHandleGetAndAdd:
    case Intrinsics::kVarHandleGetAndAddAcquire:
    case Intrinsics::kVarHandleGetAndAddRelease:
    case Intrinsics::kVarHandleGetAndBitwiseAnd:
    case Intrinsics::kVarHandleGetAndBitwiseAndAcquire:
    case Intrinsics::kVarHandleGetAndBitwiseAndRelease:
    case Intrinsics::kVarHandleGetAndBitwiseOr:
    case Intrinsics::kVarHandleGetAndBitwiseOrAcquire:
    case Intrinsics::kVarHandleGetAndBitwiseOrRelease:
    case Intrinsics::kVarHandleGetAndBitwiseXor:
    case Intrinsics::kVarHandleGetAndBitwiseXorAcquire:
    case Intrinsics::kVarHandleGetAndBitwiseXorRelease:
    case Intrinsics::kVarHandleGetAndSet:
    case Intrinsics::kVarHandleGetAndSetAcquire:
    case Intrinsics::kVarHandleGetAndSetRelease:
    case Intrinsics::kVarHandleGetOpaque:
    case Intrinsics::kVarHandleGetVolatile:
    case Intrinsics::kVarHandleSet:
    case Intrinsics::kVarHandleSetOpaque:
    case Intrinsics::kVarHandleSetRelease:
    case Intrinsics::kVarHandleSetVolatile:
    case Intrinsics::kVarHandleWeakCompareAndSet:
    case Intrinsics::kVarHandleWeakCompareAndSetAcquire:
    case Intrinsics::kVarHandleWeakCompareAndSetPlain:
    case Intrinsics::kVarHandleWeakCompareAndSetRelease:
      SimplifyVarHandleIntrinsic(instruction);
      break;
    case Intrinsics::kUnsafeArrayBaseOffset:
    case Intrinsics::kJdkUnsafeArrayBaseOffset:
      SimplifyArrayBaseOffset(instruction);
      break;
    default:
      break;
  }
}

void InstructionSimplifierVisitor::SimplifyArrayBaseOffset(HInvoke* invoke) {
  if (!invoke->InputAt(1)->IsLoadClass()) {
    return;
  }
  HLoadClass* load_class = invoke->InputAt(1)->AsLoadClass();
  ReferenceTypeInfo info = load_class->GetLoadedClassRTI();
  if (!info.IsValid()) {
    return;
  }
  ScopedObjectAccess soa(Thread::Current());
  ObjPtr<mirror::Class> cls = info.GetTypeHandle()->GetComponentType();
  if (cls == nullptr) {
    return;
  }
  uint32_t base_offset =
      mirror::Array::DataOffset(Primitive::ComponentSize(cls->GetPrimitiveType())).Int32Value();
  invoke->ReplaceWith(GetGraph()->GetIntConstant(base_offset));
  RecordSimplification();
  return;
}

void InstructionSimplifierVisitor::VisitDeoptimize(HDeoptimize* deoptimize) {
  HInstruction* cond = deoptimize->InputAt(0);
  if (cond->IsConstant()) {
    if (cond->AsIntConstant()->IsFalse()) {
      // Never deopt: instruction can be removed.
      if (deoptimize->GuardsAnInput()) {
        deoptimize->ReplaceWith(deoptimize->GuardedInput());
      }
      deoptimize->GetBlock()->RemoveInstruction(deoptimize);
    } else {
      // Always deopt.
    }
  }
}

// Replace code looking like
//    OP y, x, const1
//    OP z, y, const2
// with
//    OP z, x, const3
// where OP is both an associative and a commutative operation.
bool InstructionSimplifierVisitor::TryHandleAssociativeAndCommutativeOperation(
    HBinaryOperation* instruction) {
  DCHECK(instruction->IsCommutative());

  if (!DataType::IsIntegralType(instruction->GetType())) {
    return false;
  }

  HInstruction* left = instruction->GetLeft();
  HInstruction* right = instruction->GetRight();
  // Variable names as described above.
  HConstant* const2;
  HBinaryOperation* y;

  if (instruction->GetKind() == left->GetKind() && right->IsConstant()) {
    const2 = right->AsConstant();
    y = left->AsBinaryOperation();
  } else if (left->IsConstant() && instruction->GetKind() == right->GetKind()) {
    const2 = left->AsConstant();
    y = right->AsBinaryOperation();
  } else {
    // The node does not match the pattern.
    return false;
  }

  // If `y` has more than one use, we do not perform the optimization
  // because it might increase code size (e.g. if the new constant is
  // no longer encodable as an immediate operand in the target ISA).
  if (!y->HasOnlyOneNonEnvironmentUse()) {
    return false;
  }

  // GetConstantRight() can return both left and right constants
  // for commutative operations.
  HConstant* const1 = y->GetConstantRight();
  if (const1 == nullptr) {
    return false;
  }

  instruction->ReplaceInput(const1, 0);
  instruction->ReplaceInput(const2, 1);
  HConstant* const3 = instruction->TryStaticEvaluation();
  DCHECK(const3 != nullptr);
  instruction->ReplaceInput(y->GetLeastConstantLeft(), 0);
  instruction->ReplaceInput(const3, 1);
  RecordSimplification();
  return true;
}

static HBinaryOperation* AsAddOrSubOrNull(HInstruction* binop) {
  return (binop->IsAdd() || binop->IsSub()) ? binop->AsBinaryOperation() : nullptr;
}

// Helper function that performs addition statically, considering the result type.
static int64_t ComputeAddition(DataType::Type type, int64_t x, int64_t y) {
  // Use the Compute() method for consistency with TryStaticEvaluation().
  if (type == DataType::Type::kInt32) {
    return HAdd::Compute<int32_t>(x, y);
  } else {
    DCHECK_EQ(type, DataType::Type::kInt64);
    return HAdd::Compute<int64_t>(x, y);
  }
}

// Helper function that handles the child classes of HConstant
// and returns an integer with the appropriate sign.
static int64_t GetValue(HConstant* constant, bool is_negated) {
  int64_t ret = Int64FromConstant(constant);
  return is_negated ? -ret : ret;
}

// Replace code looking like
//    OP1 y, x, const1
//    OP2 z, y, const2
// with
//    OP3 z, x, const3
// where OPx is either ADD or SUB, and at least one of OP{1,2} is SUB.
bool InstructionSimplifierVisitor::TrySubtractionChainSimplification(
    HBinaryOperation* instruction) {
  DCHECK(instruction->IsAdd() || instruction->IsSub()) << instruction->DebugName();

  DataType::Type type = instruction->GetType();
  if (!DataType::IsIntegralType(type)) {
    return false;
  }

  HInstruction* left = instruction->GetLeft();
  HInstruction* right = instruction->GetRight();
  // Variable names as described above.
  HConstant* const2 = right->IsConstant() ? right->AsConstant() : left->AsConstantOrNull();
  if (const2 == nullptr) {
    return false;
  }

  HBinaryOperation* y = (AsAddOrSubOrNull(left) != nullptr)
      ? left->AsBinaryOperation()
      : AsAddOrSubOrNull(right);
  // If y has more than one use, we do not perform the optimization because
  // it might increase code size (e.g. if the new constant is no longer
  // encodable as an immediate operand in the target ISA).
  if ((y == nullptr) || !y->HasOnlyOneNonEnvironmentUse()) {
    return false;
  }

  left = y->GetLeft();
  HConstant* const1 = left->IsConstant() ? left->AsConstant() : y->GetRight()->AsConstantOrNull();
  if (const1 == nullptr) {
    return false;
  }

  HInstruction* x = (const1 == left) ? y->GetRight() : left;
  // If both inputs are constants, let the constant folding pass deal with it.
  if (x->IsConstant()) {
    return false;
  }

  bool is_const2_negated = (const2 == right) && instruction->IsSub();
  int64_t const2_val = GetValue(const2, is_const2_negated);
  bool is_y_negated = (y == right) && instruction->IsSub();
  right = y->GetRight();
  bool is_const1_negated = is_y_negated ^ ((const1 == right) && y->IsSub());
  int64_t const1_val = GetValue(const1, is_const1_negated);
  bool is_x_negated = is_y_negated ^ ((x == right) && y->IsSub());
  int64_t const3_val = ComputeAddition(type, const1_val, const2_val);
  HBasicBlock* block = instruction->GetBlock();
  HConstant* const3 = GetGraph()->GetConstant(type, const3_val);
  ArenaAllocator* allocator = GetGraph()->GetAllocator();
  HInstruction* z;

  if (is_x_negated) {
    z = new (allocator) HSub(type, const3, x, instruction->GetDexPc());
  } else {
    z = new (allocator) HAdd(type, x, const3, instruction->GetDexPc());
  }

  block->ReplaceAndRemoveInstructionWith(instruction, z);
  RecordSimplification();
  return true;
}

void InstructionSimplifierVisitor::VisitVecMul(HVecMul* instruction) {
  if (TryCombineVecMultiplyAccumulate(instruction)) {
    RecordSimplification();
  }
}

bool TryMergeNegatedInput(HBinaryOperation* op) {
  DCHECK(op->IsAnd() || op->IsOr() || op->IsXor()) << op->DebugName();
  HInstruction* left = op->GetLeft();
  HInstruction* right = op->GetRight();

  // Only consider the case where there is exactly one Not, with 2 Not's De
  // Morgan's laws should be applied instead.
  if (left->IsNot() ^ right->IsNot()) {
    HInstruction* hnot = (left->IsNot() ? left : right);
    HInstruction* hother = (left->IsNot() ? right : left);

    // Only do the simplification if the Not has only one use and can thus be
    // safely removed. Even though ARM64 negated bitwise operations do not have
    // an immediate variant (only register), we still do the simplification when
    // `hother` is a constant, because it removes an instruction if the constant
    // cannot be encoded as an immediate:
    //   mov r0, #large_constant
    //   neg r2, r1
    //   and r0, r0, r2
    // becomes:
    //   mov r0, #large_constant
    //   bic r0, r0, r1
    if (hnot->HasOnlyOneNonEnvironmentUse()) {
      // Replace code looking like
      //    NOT tmp, mask
      //    AND dst, src, tmp   (respectively ORR, EOR)
      // with
      //    BIC dst, src, mask  (respectively ORN, EON)
      HInstruction* src = hnot->AsNot()->GetInput();

      HBitwiseNegatedRight* neg_op = new (hnot->GetBlock()->GetGraph()->GetAllocator())
          HBitwiseNegatedRight(op->GetType(), op->GetKind(), hother, src, op->GetDexPc());

      op->GetBlock()->ReplaceAndRemoveInstructionWith(op, neg_op);
      hnot->GetBlock()->RemoveInstruction(hnot);
      return true;
    }
  }

  return false;
}

bool TryMergeWithAnd(HSub* instruction) {
  HAnd* and_instr = instruction->GetRight()->AsAndOrNull();
  if (and_instr == nullptr) {
    return false;
  }

  HInstruction* value = instruction->GetLeft();

  HInstruction* left = and_instr->GetLeft();
  const bool left_is_equal = left == value;
  HInstruction* right = and_instr->GetRight();
  const bool right_is_equal = right == value;
  if (!left_is_equal && !right_is_equal) {
    return false;
  }

  HBitwiseNegatedRight* bnr = new (instruction->GetBlock()->GetGraph()->GetAllocator())
      HBitwiseNegatedRight(instruction->GetType(),
                           HInstruction::InstructionKind::kAnd,
                           value,
                           left_is_equal ? right : left,
                           instruction->GetDexPc());
  instruction->GetBlock()->ReplaceAndRemoveInstructionWith(instruction, bnr);
  // Since we don't run DCE after this phase, try to manually remove the And instruction.
  if (!and_instr->HasUses()) {
    and_instr->GetBlock()->RemoveInstruction(and_instr);
  }
  return true;
}

}  // namespace art
