/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef ART_RUNTIME_INTERPRETER_INTERPRETER_SWITCH_IMPL_INL_H_
#define ART_RUNTIME_INTERPRETER_INTERPRETER_SWITCH_IMPL_INL_H_

#include "interpreter_switch_impl.h"

#include "base/globals.h"
#include "base/memory_tool.h"
#include "base/pointer_size.h"
#include "base/quasi_atomic.h"
#include "common_throws.h"
#include "dex/dex_file_types.h"
#include "dex/dex_instruction_list.h"
#include "experimental_flags.h"
#include "handle_scope.h"
#include "interpreter_common.h"
#include "interpreter/shadow_frame.h"
#include "jit/jit-inl.h"
#include "jvalue-inl.h"
#include "mirror/string-alloc-inl.h"
#include "mirror/throwable.h"
#include "monitor.h"
#include "nth_caller_visitor.h"
#include "safe_math.h"
#include "shadow_frame-inl.h"
#include "thread.h"
#include "verifier/method_verifier.h"

namespace art HIDDEN {
namespace interpreter {

// We declare the helpers classes for transaction checks here but they shall be defined
// only when compiling the transactional and non-transactional interpreter.
class ActiveTransactionChecker;  // For transactional interpreter.
class InactiveTransactionChecker;  // For non-transactional interpreter.

// We declare the helpers classes for instrumentation handling here but they shall be defined
// only when compiling the transactional and non-transactional interpreter.
class ActiveInstrumentationHandler;  // For non-transactional interpreter.
class InactiveInstrumentationHandler;  // For transactional interpreter.

// Handles iget-XXX and sget-XXX instructions.
// Returns true on success, otherwise throws an exception and returns false.
template<FindFieldType find_type,
         Primitive::Type field_type,
         bool transaction_active = false>
ALWAYS_INLINE bool DoFieldGet(Thread* self,
                              ShadowFrame& shadow_frame,
                              const Instruction* inst,
                              uint16_t inst_data,
                              const instrumentation::Instrumentation* instrumentation)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  using InstrumentationHandler = typename std::conditional_t<
      transaction_active, InactiveInstrumentationHandler, ActiveInstrumentationHandler>;
  bool should_report = InstrumentationHandler::HasFieldReadListeners(instrumentation);
  const bool is_static = (find_type == StaticObjectRead) || (find_type == StaticPrimitiveRead);
  ArtField* field = nullptr;
  MemberOffset offset(0u);
  bool is_volatile;
  GetFieldInfo(self,
               shadow_frame,
               reinterpret_cast<const uint16_t*>(inst),
               is_static,
               /*resolve_field_type=*/ false,
               &field,
               &is_volatile,
               &offset);
  if (self->IsExceptionPending()) {
    return false;
  }

  ObjPtr<mirror::Object> obj;
  if (is_static) {
    obj = field->GetDeclaringClass();
    using TransactionChecker = typename std::conditional_t<
        transaction_active, ActiveTransactionChecker, InactiveTransactionChecker>;
    if (TransactionChecker::ReadConstraint(self, obj)) {
      return false;
    }
  } else {
    obj = shadow_frame.GetVRegReference(inst->VRegB_22c(inst_data));
    if (should_report || obj == nullptr) {
      field = ResolveFieldWithAccessChecks(self,
                                           Runtime::Current()->GetClassLinker(),
                                           inst->VRegC_22c(),
                                           shadow_frame.GetMethod(),
                                           /* is_static= */ false,
                                           /* is_put= */ false,
                                           /* resolve_field_type= */ false);
      if (obj == nullptr) {
        ThrowNullPointerExceptionForFieldAccess(
            field, shadow_frame.GetMethod(), /* is_read= */ true);
        return false;
      }
      // Reload in case suspension happened during field resolution.
      obj = shadow_frame.GetVRegReference(inst->VRegB_22c(inst_data));
    }
  }

  uint32_t vregA = is_static ? inst->VRegA_21c(inst_data) : inst->VRegA_22c(inst_data);
  JValue result;
  if (should_report) {
    DCHECK(field != nullptr);
    if (UNLIKELY(!DoFieldGetCommon<field_type>(self, shadow_frame, obj, field, &result))) {
      // Instrumentation threw an error!
      CHECK(self->IsExceptionPending());
      return false;
    }
  }

#define FIELD_GET(prim, type, jtype, vreg)                                      \
  case Primitive::kPrim ##prim:                                                 \
    shadow_frame.SetVReg ##vreg(vregA,                                          \
        should_report ? result.Get ##jtype()                                    \
                      : is_volatile ? obj->GetField ## type ## Volatile(offset) \
                                    : obj->GetField ##type(offset));            \
    break;

  switch (field_type) {
    FIELD_GET(Boolean, Boolean, Z, )
    FIELD_GET(Byte, Byte, B, )
    FIELD_GET(Char, Char, C, )
    FIELD_GET(Short, Short, S, )
    FIELD_GET(Int, 32, I, )
    FIELD_GET(Long, 64, J, Long)
#undef FIELD_GET
    case Primitive::kPrimNot:
      shadow_frame.SetVRegReference(
          vregA,
          should_report ? result.GetL()
                        : is_volatile ? obj->GetFieldObjectVolatile<mirror::Object>(offset)
                                      : obj->GetFieldObject<mirror::Object>(offset));
      break;
    default:
      LOG(FATAL) << "Unreachable: " << field_type;
      UNREACHABLE();
  }
  return true;
}

// Handles iput-XXX and sput-XXX instructions.
// Returns true on success, otherwise throws an exception and returns false.
template<FindFieldType find_type, Primitive::Type field_type, bool transaction_active>
ALWAYS_INLINE bool DoFieldPut(Thread* self,
                              ShadowFrame& shadow_frame,
                              const Instruction* inst,
                              uint16_t inst_data,
                              const instrumentation::Instrumentation* instrumentation)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  using InstrumentationHandler = typename std::conditional_t<
      transaction_active, InactiveInstrumentationHandler, ActiveInstrumentationHandler>;
  bool should_report = InstrumentationHandler::HasFieldWriteListeners(instrumentation);
  bool is_static = (find_type == StaticObjectWrite) || (find_type == StaticPrimitiveWrite);
  uint32_t vregA = is_static ? inst->VRegA_21c(inst_data) : inst->VRegA_22c(inst_data);
  bool resolve_field_type = (shadow_frame.GetVRegReference(vregA) != nullptr);
  ArtField* field = nullptr;
  MemberOffset offset(0u);
  bool is_volatile;
  GetFieldInfo(self,
               shadow_frame,
               reinterpret_cast<const uint16_t*>(inst),
               is_static,
               resolve_field_type,
               &field,
               &is_volatile,
               &offset);
  if (self->IsExceptionPending()) {
    return false;
  }

  ObjPtr<mirror::Object> obj;
  if (is_static) {
    obj = field->GetDeclaringClass();
  } else {
    obj = shadow_frame.GetVRegReference(inst->VRegB_22c(inst_data));
    if (should_report || obj == nullptr) {
      field = ResolveFieldWithAccessChecks(self,
                                           Runtime::Current()->GetClassLinker(),
                                           inst->VRegC_22c(),
                                           shadow_frame.GetMethod(),
                                           /* is_static= */ false,
                                           /* is_put= */ true,
                                           resolve_field_type);
      if (UNLIKELY(obj == nullptr)) {
        ThrowNullPointerExceptionForFieldAccess(
            field, shadow_frame.GetMethod(), /* is_read= */ false);
        return false;
      }
      // Reload in case suspension happened during field resolution.
      obj = shadow_frame.GetVRegReference(inst->VRegB_22c(inst_data));
    }
  }
  using TransactionChecker = typename std::conditional_t<
      transaction_active, ActiveTransactionChecker, InactiveTransactionChecker>;
  if (TransactionChecker::WriteConstraint(self, obj)) {
    return false;
  }

  JValue value = GetFieldValue<field_type>(shadow_frame, vregA);

  if (field_type == Primitive::kPrimNot &&
      TransactionChecker::WriteValueConstraint(self, value.GetL())) {
    return false;
  }
  if (should_report) {
    return DoFieldPutCommon<field_type, transaction_active>(self,
                                                            shadow_frame,
                                                            obj,
                                                            field,
                                                            value);
  }
#define FIELD_SET(prim, type, jtype) \
  case Primitive::kPrim ## prim: \
    if (is_volatile) { \
      obj->SetField ## type ## Volatile<transaction_active>(offset, value.Get ## jtype()); \
    } else { \
      obj->SetField ## type<transaction_active>(offset, value.Get ## jtype()); \
    } \
    break;

  switch (field_type) {
    FIELD_SET(Boolean, Boolean, Z)
    FIELD_SET(Byte, Byte, B)
    FIELD_SET(Char, Char, C)
    FIELD_SET(Short, Short, S)
    FIELD_SET(Int, 32, I)
    FIELD_SET(Long, 64, J)
    FIELD_SET(Not, Object, L)
    case Primitive::kPrimVoid: {
      LOG(FATAL) << "Unreachable " << field_type;
      break;
    }
  }
#undef FIELD_SET

  if (transaction_active) {
    if (UNLIKELY(self->IsExceptionPending())) {
      return false;
    }
  }
  return true;
}

// Short-lived helper class which executes single DEX bytecode.  It is inlined by compiler.
// Any relevant execution information is stored in the fields - it should be kept to minimum.
// All instance functions must be inlined so that the fields can be stored in registers.
//
// The function names must match the names from dex_instruction_list.h and have no arguments.
// Return value: The handlers must return false if the instruction throws or returns (exits).
//
template<bool transaction_active, Instruction::Format kFormat>
class InstructionHandler {
 public:
  using InstrumentationHandler = typename std::conditional_t<
      transaction_active, InactiveInstrumentationHandler, ActiveInstrumentationHandler>;
  using TransactionChecker = typename std::conditional_t<
      transaction_active, ActiveTransactionChecker, InactiveTransactionChecker>;

#define HANDLER_ATTRIBUTES ALWAYS_INLINE FLATTEN WARN_UNUSED REQUIRES_SHARED(Locks::mutator_lock_)

  HANDLER_ATTRIBUTES bool CheckTransactionAbort() {
    if (TransactionChecker::IsTransactionAborted()) {
      // Transaction abort cannot be caught by catch handlers.
      // Preserve the abort exception while doing non-standard return.
      StackHandleScope<1u> hs(Self());
      Handle<mirror::Throwable> abort_exception = hs.NewHandle(Self()->GetException());
      DCHECK(abort_exception != nullptr);
      DCHECK(abort_exception->GetClass()->DescriptorEquals(kTransactionAbortErrorDescriptor));
      Self()->ClearException();
      PerformNonStandardReturn(Self(), shadow_frame_, ctx_->result, Instrumentation());
      Self()->SetException(abort_exception.Get());
      ExitInterpreterLoop();
      return false;
    }
    return true;
  }

  HANDLER_ATTRIBUTES bool CheckForceReturn() {
    if (InstrumentationHandler::GetForcePopFrame(shadow_frame_)) {
      DCHECK(Runtime::Current()->AreNonStandardExitsEnabled());
      PerformNonStandardReturn(Self(), shadow_frame_, ctx_->result, Instrumentation());
      ExitInterpreterLoop();
      return false;
    }
    return true;
  }

  HANDLER_ATTRIBUTES bool HandlePendingException() {
    DCHECK(Self()->IsExceptionPending());
    Self()->AllowThreadSuspension();
    if (!CheckTransactionAbort()) {
      return false;
    }
    if (!CheckForceReturn()) {
      return false;
    }
    bool skip_event = shadow_frame_.GetSkipNextExceptionEvent();
    shadow_frame_.SetSkipNextExceptionEvent(false);
    if (!MoveToExceptionHandler(Self(),
                                shadow_frame_,
                                /* skip_listeners= */ skip_event,
                                /* skip_throw_listener= */ skip_event)) {
      // Structured locking is to be enforced for abnormal termination, too.
      DoMonitorCheckOnExit(Self(), &shadow_frame_);
      ctx_->result = JValue(); /* Handled in caller. */
      ExitInterpreterLoop();
      return false;  // Return to caller.
    }
    if (!CheckForceReturn()) {
      return false;
    }
    int32_t displacement =
        static_cast<int32_t>(shadow_frame_.GetDexPC()) - static_cast<int32_t>(dex_pc_);
    SetNextInstruction(inst_->RelativeAt(displacement));
    return true;
  }

  HANDLER_ATTRIBUTES bool PossiblyHandlePendingExceptionOnInvoke(bool is_exception_pending) {
    if (UNLIKELY(shadow_frame_.GetForceRetryInstruction())) {
      /* Don't need to do anything except clear the flag and exception. We leave the */
      /* instruction the same so it will be re-executed on the next go-around.       */
      DCHECK(inst_->IsInvoke());
      shadow_frame_.SetForceRetryInstruction(false);
      if (UNLIKELY(is_exception_pending)) {
        DCHECK(Self()->IsExceptionPending());
        if (kIsDebugBuild) {
          LOG(WARNING) << "Suppressing exception for instruction-retry: "
                       << Self()->GetException()->Dump();
        }
        Self()->ClearException();
      }
      SetNextInstruction(inst_);
    } else if (UNLIKELY(is_exception_pending)) {
      /* Should have succeeded. */
      DCHECK(!shadow_frame_.GetForceRetryInstruction());
      return false;  // Pending exception.
    }
    return true;
  }

  // Code to run before each dex instruction.
  HANDLER_ATTRIBUTES bool Preamble() {
    /* We need to put this before & after the instrumentation to avoid having to put in a */
    /* post-script macro.                                                                 */
    if (!CheckForceReturn()) {
      return false;
    }
    if (UNLIKELY(InstrumentationHandler::NeedsDexPcEvents(shadow_frame_))) {
      uint8_t opcode = inst_->Opcode(inst_data_);
      bool is_move_result_object = (opcode == Instruction::MOVE_RESULT_OBJECT);
      JValue* save_ref = is_move_result_object ? &ctx_->result_register : nullptr;
      if (UNLIKELY(!InstrumentationHandler::DoDexPcMoveEvent(Self(),
                                                             Accessor(),
                                                             shadow_frame_,
                                                             DexPC(),
                                                             Instrumentation(),
                                                             save_ref))) {
        DCHECK(Self()->IsExceptionPending());
        // Do not raise exception event if it is caused by other instrumentation event.
        shadow_frame_.SetSkipNextExceptionEvent(true);
        return false;  // Pending exception.
      }
      if (!CheckForceReturn()) {
        return false;
      }
    }

    // Call any exception handled event handlers after the dex pc move event.
    // The order is important to see a consistent behaviour in the debuggers.
    // See b/333446719 for more discussion.
    if (UNLIKELY(shadow_frame_.GetNotifyExceptionHandledEvent())) {
      shadow_frame_.SetNotifyExceptionHandledEvent(/*enable=*/ false);
      bool is_move_exception = (inst_->Opcode(inst_data_) == Instruction::MOVE_EXCEPTION);

      if (!InstrumentationHandler::ExceptionHandledEvent(
              Self(), is_move_exception, Instrumentation())) {
        DCHECK(Self()->IsExceptionPending());
        // TODO(375373721): We need to set SetSkipNextExceptionEvent here since the exception was
        // thrown by an instrumentation handler.
        return false;  // Pending exception.
      }

      if (!CheckForceReturn()) {
        return false;
      }
    }
    return true;
  }

  HANDLER_ATTRIBUTES bool HandleReturn(JValue result) {
    Self()->AllowThreadSuspension();
    if (!DoMonitorCheckOnExit(Self(), &shadow_frame_)) {
      return false;
    }
    if (UNLIKELY(InstrumentationHandler::NeedsMethodExitEvent(Instrumentation()) &&
                 !InstrumentationHandler::SendMethodExitEvents(Self(),
                                                               Instrumentation(),
                                                               shadow_frame_,
                                                               shadow_frame_.GetMethod(),
                                                               result))) {
      DCHECK(Self()->IsExceptionPending());
      // Do not raise exception event if it is caused by other instrumentation event.
      shadow_frame_.SetSkipNextExceptionEvent(true);
      return false;  // Pending exception.
    }
    ctx_->result = result;
    ExitInterpreterLoop();
    return false;
  }

  HANDLER_ATTRIBUTES bool HandleBranch(int32_t offset) {
    if (UNLIKELY(Self()->ObserveAsyncException())) {
      return false;  // Pending exception.
    }
    if (UNLIKELY(InstrumentationHandler::HasBranchListeners(Instrumentation()))) {
      InstrumentationHandler::Branch(
          Self(), shadow_frame_.GetMethod(), DexPC(), offset, Instrumentation());
    }
    if (!transaction_active) {
      // TODO: Do OSR only on back-edges and check if OSR code is ready here.
      JValue result;
      if (jit::Jit::MaybeDoOnStackReplacement(Self(),
                                              shadow_frame_.GetMethod(),
                                              DexPC(),
                                              offset,
                                              &result)) {
        ctx_->result = result;
        ExitInterpreterLoop();
        return false;
      }
    }
    SetNextInstruction(inst_->RelativeAt(offset));
    if (offset <= 0) {  // Back-edge.
      // Hotness update.
      jit::Jit* jit = Runtime::Current()->GetJit();
      if (jit != nullptr) {
        jit->AddSamples(Self(), shadow_frame_.GetMethod());
      }
      // Record new dex pc early to have consistent suspend point at loop header.
      shadow_frame_.SetDexPC(next_->GetDexPc(Insns()));
      Self()->AllowThreadSuspension();
    }
    return true;
  }

  HANDLER_ATTRIBUTES bool HandleIf(bool cond, int32_t offset) {
    return HandleBranch(cond ? offset : Instruction::SizeInCodeUnits(kFormat));
  }

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfloat-equal"

  template<typename T>
  HANDLER_ATTRIBUTES bool HandleCmpl(T val1, T val2) {
    int32_t result;
    if (val1 > val2) {
      result = 1;
    } else if (val1 == val2) {
      result = 0;
    } else {
      result = -1;
    }
    SetVReg(A(), result);
    return true;
  }

  // Returns the same result as the function above. It only differs for NaN values.
  template<typename T>
  HANDLER_ATTRIBUTES bool HandleCmpg(T val1, T val2) {
    int32_t result;
    if (val1 < val2) {
      result = -1;
    } else if (val1 == val2) {
      result = 0;
    } else {
      result = 1;
    }
    SetVReg(A(), result);
    return true;
  }

#pragma clang diagnostic pop

  HANDLER_ATTRIBUTES bool HandleConstString() {
    ObjPtr<mirror::String> s = ResolveString(Self(), shadow_frame_, dex::StringIndex(B()));
    if (UNLIKELY(s == nullptr)) {
      return false;  // Pending exception.
    }
    SetVRegReference(A(), s);
    return true;
  }

  template<typename ArrayType, typename SetVRegFn>
  HANDLER_ATTRIBUTES bool HandleAGet(SetVRegFn setVReg) {
    ObjPtr<mirror::Object> a = GetVRegReference(B());
    if (UNLIKELY(a == nullptr)) {
      ThrowNullPointerExceptionFromInterpreter();
      return false;  // Pending exception.
    }
    int32_t index = GetVReg(C());
    ObjPtr<ArrayType> array = ObjPtr<ArrayType>::DownCast(a);
    if (UNLIKELY(!array->CheckIsValidIndex(index))) {
      return false;  // Pending exception.
    }
    (this->*setVReg)(A(), array->GetWithoutChecks(index));
    return true;
  }

  template<typename ArrayType, typename T>
  HANDLER_ATTRIBUTES bool HandleAPut(T value) {
    ObjPtr<mirror::Object> a = GetVRegReference(B());
    if (UNLIKELY(a == nullptr)) {
      ThrowNullPointerExceptionFromInterpreter();
      return false;  // Pending exception.
    }
    int32_t index = GetVReg(C());
    ObjPtr<ArrayType> array = ObjPtr<ArrayType>::DownCast(a);
    if (UNLIKELY(!array->CheckIsValidIndex(index))) {
      return false;  // Pending exception.
    }
    if (TransactionChecker::WriteConstraint(Self(), array)) {
      return false;
    }
    array->template SetWithoutChecks<transaction_active>(index, value);
    return true;
  }

  template<FindFieldType find_type, Primitive::Type field_type>
  HANDLER_ATTRIBUTES bool HandleGet() {
    return DoFieldGet<find_type, field_type, transaction_active>(
        Self(), shadow_frame_, inst_, inst_data_, Instrumentation());
  }

  template<FindFieldType find_type, Primitive::Type field_type>
  HANDLER_ATTRIBUTES bool HandlePut() {
    return DoFieldPut<find_type, field_type, transaction_active>(
        Self(), shadow_frame_, inst_, inst_data_, Instrumentation());
  }

  template<InvokeType type, bool is_range>
  HANDLER_ATTRIBUTES bool HandleInvoke() {
    bool success = DoInvoke<type, is_range>(
        Self(), shadow_frame_, inst_, inst_data_, ResultRegister());
    return PossiblyHandlePendingExceptionOnInvoke(!success);
  }

  HANDLER_ATTRIBUTES bool HandleUnused() {
    UnexpectedOpcode(inst_, shadow_frame_);
    return true;
  }

  HANDLER_ATTRIBUTES bool NOP() {
    return true;
  }

  HANDLER_ATTRIBUTES bool MOVE() {
    SetVReg(A(), GetVReg(B()));
    return true;
  }

  HANDLER_ATTRIBUTES bool MOVE_FROM16() {
    SetVReg(A(), GetVReg(B()));
    return true;
  }

  HANDLER_ATTRIBUTES bool MOVE_16() {
    SetVReg(A(), GetVReg(B()));
    return true;
  }

  HANDLER_ATTRIBUTES bool MOVE_WIDE() {
    SetVRegLong(A(), GetVRegLong(B()));
    return true;
  }

  HANDLER_ATTRIBUTES bool MOVE_WIDE_FROM16() {
    SetVRegLong(A(), GetVRegLong(B()));
    return true;
  }

  HANDLER_ATTRIBUTES bool MOVE_WIDE_16() {
    SetVRegLong(A(), GetVRegLong(B()));
    return true;
  }

  HANDLER_ATTRIBUTES bool MOVE_OBJECT() {
    SetVRegReference(A(), GetVRegReference(B()));
    return true;
  }

  HANDLER_ATTRIBUTES bool MOVE_OBJECT_FROM16() {
    SetVRegReference(A(), GetVRegReference(B()));
    return true;
  }

  HANDLER_ATTRIBUTES bool MOVE_OBJECT_16() {
    SetVRegReference(A(), GetVRegReference(B()));
    return true;
  }

  HANDLER_ATTRIBUTES bool MOVE_RESULT() {
    SetVReg(A(), ResultRegister()->GetI());
    return true;
  }

  HANDLER_ATTRIBUTES bool MOVE_RESULT_WIDE() {
    SetVRegLong(A(), ResultRegister()->GetJ());
    return true;
  }

  HANDLER_ATTRIBUTES bool MOVE_RESULT_OBJECT() {
    SetVRegReference(A(), ResultRegister()->GetL());
    return true;
  }

  HANDLER_ATTRIBUTES bool MOVE_EXCEPTION() {
    ObjPtr<mirror::Throwable> exception = Self()->GetException();
    DCHECK(exception != nullptr) << "No pending exception on MOVE_EXCEPTION instruction";
    SetVRegReference(A(), exception);
    Self()->ClearException();
    return true;
  }

  HANDLER_ATTRIBUTES bool RETURN_VOID() {
    QuasiAtomic::ThreadFenceForConstructor();
    JValue result;
    return HandleReturn(result);
  }

  HANDLER_ATTRIBUTES bool RETURN() {
    JValue result;
    result.SetJ(0);
    result.SetI(GetVReg(A()));
    return HandleReturn(result);
  }

  HANDLER_ATTRIBUTES bool RETURN_WIDE() {
    JValue result;
    result.SetJ(GetVRegLong(A()));
    return HandleReturn(result);
  }

  HANDLER_ATTRIBUTES bool RETURN_OBJECT() {
    JValue result;
    Self()->AllowThreadSuspension();
    if (!DoMonitorCheckOnExit(Self(), &shadow_frame_)) {
      return false;
    }
    const size_t ref_idx = A();
    ObjPtr<mirror::Object> obj_result = GetVRegReference(ref_idx);
    if (obj_result != nullptr && UNLIKELY(DoAssignabilityChecks())) {
      ObjPtr<mirror::Class> return_type = shadow_frame_.GetMethod()->ResolveReturnType();
      // Re-load since it might have moved.
      obj_result = GetVRegReference(ref_idx);
      if (return_type == nullptr) {
        // Return the pending exception.
        return false;  // Pending exception.
      }
      if (!obj_result->VerifierInstanceOf(return_type)) {
        CHECK_LE(Runtime::Current()->GetTargetSdkVersion(), 29u);
        // This should never happen.
        std::string temp1, temp2;
        Self()->ThrowNewExceptionF("Ljava/lang/InternalError;",
                                   "Returning '%s' that is not instance of return type '%s'",
                                   obj_result->GetClass()->GetDescriptor(&temp1),
                                   return_type->GetDescriptor(&temp2));
        return false;  // Pending exception.
      }
    }
    result.SetL(obj_result);
    if (UNLIKELY(InstrumentationHandler::NeedsMethodExitEvent(Instrumentation()))) {
      StackHandleScope<1> hs(Self());
      MutableHandle<mirror::Object> h_result(hs.NewHandle(obj_result));
      if (!InstrumentationHandler::SendMethodExitEvents(Self(),
                                                        Instrumentation(),
                                                        shadow_frame_,
                                                        shadow_frame_.GetMethod(),
                                                        h_result)) {
        DCHECK(Self()->IsExceptionPending());
        // Do not raise exception event if it is caused by other instrumentation event.
        shadow_frame_.SetSkipNextExceptionEvent(true);
        return false;  // Pending exception.
      }
      // Re-load since it might have moved or been replaced during the MethodExitEvent.
      result.SetL(h_result.Get());
    }
    ctx_->result = result;
    ExitInterpreterLoop();
    return false;
  }

  HANDLER_ATTRIBUTES bool CONST_4() {
    SetVReg(A(), B());
    return true;
  }

  HANDLER_ATTRIBUTES bool CONST_16() {
    SetVReg(A(), B());
    return true;
  }

  HANDLER_ATTRIBUTES bool CONST() {
    SetVReg(A(), B());
    return true;
  }

  HANDLER_ATTRIBUTES bool CONST_HIGH16() {
    SetVReg(A(), static_cast<int32_t>(B() << 16));
    return true;
  }

  HANDLER_ATTRIBUTES bool CONST_WIDE_16() {
    SetVRegLong(A(), B());
    return true;
  }

  HANDLER_ATTRIBUTES bool CONST_WIDE_32() {
    SetVRegLong(A(), B());
    return true;
  }

  HANDLER_ATTRIBUTES bool CONST_WIDE() {
    SetVRegLong(A(), inst_->WideVRegB());
    return true;
  }

  HANDLER_ATTRIBUTES bool CONST_WIDE_HIGH16() {
    SetVRegLong(A(), static_cast<uint64_t>(B()) << 48);
    return true;
  }

  HANDLER_ATTRIBUTES bool CONST_STRING() {
    return HandleConstString();
  }

  HANDLER_ATTRIBUTES bool CONST_STRING_JUMBO() {
    return HandleConstString();
  }

  HANDLER_ATTRIBUTES bool CONST_CLASS() {
    ObjPtr<mirror::Class> c =
        ResolveVerifyAndClinit(dex::TypeIndex(B()),
                               shadow_frame_.GetMethod(),
                               Self(),
                               false,
                               !shadow_frame_.GetMethod()->SkipAccessChecks());
    if (UNLIKELY(c == nullptr)) {
      return false;  // Pending exception.
    }
    SetVRegReference(A(), c);
    return true;
  }

  HANDLER_ATTRIBUTES bool CONST_METHOD_HANDLE() {
    ClassLinker* cl = Runtime::Current()->GetClassLinker();
    ObjPtr<mirror::MethodHandle> mh = cl->ResolveMethodHandle(Self(),
                                                              B(),
                                                              shadow_frame_.GetMethod());
    if (UNLIKELY(mh == nullptr)) {
      return false;  // Pending exception.
    }
    SetVRegReference(A(), mh);
    return true;
  }

  HANDLER_ATTRIBUTES bool CONST_METHOD_TYPE() {
    ClassLinker* cl = Runtime::Current()->GetClassLinker();
    ObjPtr<mirror::MethodType> mt = cl->ResolveMethodType(Self(),
                                                          dex::ProtoIndex(B()),
                                                          shadow_frame_.GetMethod());
    if (UNLIKELY(mt == nullptr)) {
      return false;  // Pending exception.
    }
    SetVRegReference(A(), mt);
    return true;
  }

  HANDLER_ATTRIBUTES bool MONITOR_ENTER() {
    if (UNLIKELY(Self()->ObserveAsyncException())) {
      return false;  // Pending exception.
    }
    ObjPtr<mirror::Object> obj = GetVRegReference(A());
    if (UNLIKELY(obj == nullptr)) {
      ThrowNullPointerExceptionFromInterpreter();
      return false;  // Pending exception.
    }
    DoMonitorEnter(Self(), &shadow_frame_, obj);
    return !Self()->IsExceptionPending();
  }

  HANDLER_ATTRIBUTES bool MONITOR_EXIT() {
    if (UNLIKELY(Self()->ObserveAsyncException())) {
      return false;  // Pending exception.
    }
    ObjPtr<mirror::Object> obj = GetVRegReference(A());
    if (UNLIKELY(obj == nullptr)) {
      ThrowNullPointerExceptionFromInterpreter();
      return false;  // Pending exception.
    }
    DoMonitorExit(Self(), &shadow_frame_, obj);
    return !Self()->IsExceptionPending();
  }

  HANDLER_ATTRIBUTES bool CHECK_CAST() {
    ObjPtr<mirror::Class> c =
        ResolveVerifyAndClinit(dex::TypeIndex(B()),
                               shadow_frame_.GetMethod(),
                               Self(),
                               false,
                               !shadow_frame_.GetMethod()->SkipAccessChecks());
    if (UNLIKELY(c == nullptr)) {
      return false;  // Pending exception.
    }
    ObjPtr<mirror::Object> obj = GetVRegReference(A());
    if (UNLIKELY(obj != nullptr && !obj->InstanceOf(c))) {
      ThrowClassCastException(c, obj->GetClass());
      return false;  // Pending exception.
    }
    return true;
  }

  HANDLER_ATTRIBUTES bool INSTANCE_OF() {
    ObjPtr<mirror::Class> c =
        ResolveVerifyAndClinit(dex::TypeIndex(C()),
                               shadow_frame_.GetMethod(),
                               Self(),
                               false,
                               !shadow_frame_.GetMethod()->SkipAccessChecks());
    if (UNLIKELY(c == nullptr)) {
      return false;  // Pending exception.
    }
    ObjPtr<mirror::Object> obj = GetVRegReference(B());
    SetVReg(A(), (obj != nullptr && obj->InstanceOf(c)) ? 1 : 0);
    return true;
  }

  HANDLER_ATTRIBUTES bool ARRAY_LENGTH() {
    ObjPtr<mirror::Object> array = GetVRegReference(B());
    if (UNLIKELY(array == nullptr)) {
      ThrowNullPointerExceptionFromInterpreter();
      return false;  // Pending exception.
    }
    SetVReg(A(), array->AsArray()->GetLength());
    return true;
  }

  HANDLER_ATTRIBUTES bool NEW_INSTANCE() {
    ObjPtr<mirror::Object> obj = nullptr;
    ObjPtr<mirror::Class> c =
        ResolveVerifyAndClinit(dex::TypeIndex(B()),
                               shadow_frame_.GetMethod(),
                               Self(),
                               false,
                               !shadow_frame_.GetMethod()->SkipAccessChecks());
    if (LIKELY(c != nullptr)) {
      // Don't allow finalizable objects to be allocated during a transaction since these can't
      // be finalized without a started runtime.
      if (TransactionChecker::AllocationConstraint(Self(), c)) {
        return false;  // Pending exception.
      }
      gc::AllocatorType allocator_type = Runtime::Current()->GetHeap()->GetCurrentAllocator();
      if (UNLIKELY(c->IsStringClass())) {
        obj = mirror::String::AllocEmptyString(Self(), allocator_type);
        // Do not record the allocated string in the transaction.
        // There can be no transaction records for this immutable object.
      } else {
        obj = AllocObjectFromCode(c, Self(), allocator_type);
        if (obj != nullptr) {
          TransactionChecker::RecordNewObject(obj);
        }
      }
    }
    if (UNLIKELY(obj == nullptr)) {
      return false;  // Pending exception.
    }
    obj->GetClass()->AssertInitializedOrInitializingInThread(Self());
    SetVRegReference(A(), obj);
    return true;
  }

  HANDLER_ATTRIBUTES bool NEW_ARRAY() {
    int32_t length = GetVReg(B());
    ObjPtr<mirror::Array> array = AllocArrayFromCode(
        dex::TypeIndex(C()),
        length,
        shadow_frame_.GetMethod(),
        Self(),
        Runtime::Current()->GetHeap()->GetCurrentAllocator());
    if (UNLIKELY(array == nullptr)) {
      return false;  // Pending exception.
    }
    TransactionChecker::RecordNewArray(array);
    SetVRegReference(A(), array);
    return true;
  }

  HANDLER_ATTRIBUTES bool FILLED_NEW_ARRAY() {
    return DoFilledNewArray</*is_range=*/ false>(inst_, shadow_frame_, Self(), ResultRegister());
  }

  HANDLER_ATTRIBUTES bool FILLED_NEW_ARRAY_RANGE() {
    return DoFilledNewArray</*is_range=*/ true>(inst_, shadow_frame_, Self(), ResultRegister());
  }

  HANDLER_ATTRIBUTES bool FILL_ARRAY_DATA() {
    const uint16_t* payload_addr = reinterpret_cast<const uint16_t*>(inst_) + B();
    const Instruction::ArrayDataPayload* payload =
        reinterpret_cast<const Instruction::ArrayDataPayload*>(payload_addr);
    ObjPtr<mirror::Object> obj = GetVRegReference(A());
    // If we have an active transaction, record old values before we overwrite them.
    TransactionChecker::RecordArrayElementsInTransaction(obj, payload->element_count);
    if (!FillArrayData(obj, payload)) {
      return false;  // Pending exception.
    }
    return true;
  }

  HANDLER_ATTRIBUTES bool THROW() {
    if (UNLIKELY(Self()->ObserveAsyncException())) {
      return false;  // Pending exception.
    }
    ObjPtr<mirror::Object> exception = GetVRegReference(A());
    if (UNLIKELY(exception == nullptr)) {
      ThrowNullPointerException();
    } else if (DoAssignabilityChecks() && !exception->GetClass()->IsThrowableClass()) {
      // This should never happen.
      std::string temp;
      Self()->ThrowNewExceptionF("Ljava/lang/InternalError;",
                                 "Throwing '%s' that is not instance of Throwable",
                                 exception->GetClass()->GetDescriptor(&temp));
    } else {
      Self()->SetException(exception->AsThrowable());
    }
    return false;  // Pending exception.
  }

  HANDLER_ATTRIBUTES bool GOTO() {
    return HandleBranch(A());
  }

  HANDLER_ATTRIBUTES bool GOTO_16() {
    return HandleBranch(A());
  }

  HANDLER_ATTRIBUTES bool GOTO_32() {
    return HandleBranch(A());
  }

  HANDLER_ATTRIBUTES bool PACKED_SWITCH() {
    return HandleBranch(DoPackedSwitch(inst_, shadow_frame_, inst_data_));
  }

  HANDLER_ATTRIBUTES bool SPARSE_SWITCH() {
    return HandleBranch(DoSparseSwitch(inst_, shadow_frame_, inst_data_));
  }

  HANDLER_ATTRIBUTES bool CMPL_FLOAT() {
    return HandleCmpl<float>(GetVRegFloat(B()), GetVRegFloat(C()));
  }

  HANDLER_ATTRIBUTES bool CMPG_FLOAT() {
    return HandleCmpg<float>(GetVRegFloat(B()), GetVRegFloat(C()));
  }

  HANDLER_ATTRIBUTES bool CMPL_DOUBLE() {
    return HandleCmpl<double>(GetVRegDouble(B()), GetVRegDouble(C()));
  }

  HANDLER_ATTRIBUTES bool CMPG_DOUBLE() {
    return HandleCmpg<double>(GetVRegDouble(B()), GetVRegDouble(C()));
  }

  HANDLER_ATTRIBUTES bool CMP_LONG() {
    return HandleCmpl<int64_t>(GetVRegLong(B()), GetVRegLong(C()));
  }

  HANDLER_ATTRIBUTES bool IF_EQ() {
    return HandleIf(GetVReg(A()) == GetVReg(B()), C());
  }

  HANDLER_ATTRIBUTES bool IF_NE() {
    return HandleIf(GetVReg(A()) != GetVReg(B()), C());
  }

  HANDLER_ATTRIBUTES bool IF_LT() {
    return HandleIf(GetVReg(A()) < GetVReg(B()), C());
  }

  HANDLER_ATTRIBUTES bool IF_GE() {
    return HandleIf(GetVReg(A()) >= GetVReg(B()), C());
  }

  HANDLER_ATTRIBUTES bool IF_GT() {
    return HandleIf(GetVReg(A()) > GetVReg(B()), C());
  }

  HANDLER_ATTRIBUTES bool IF_LE() {
    return HandleIf(GetVReg(A()) <= GetVReg(B()), C());
  }

  HANDLER_ATTRIBUTES bool IF_EQZ() {
    return HandleIf(GetVReg(A()) == 0, B());
  }

  HANDLER_ATTRIBUTES bool IF_NEZ() {
    return HandleIf(GetVReg(A()) != 0, B());
  }

  HANDLER_ATTRIBUTES bool IF_LTZ() {
    return HandleIf(GetVReg(A()) < 0, B());
  }

  HANDLER_ATTRIBUTES bool IF_GEZ() {
    return HandleIf(GetVReg(A()) >= 0, B());
  }

  HANDLER_ATTRIBUTES bool IF_GTZ() {
    return HandleIf(GetVReg(A()) > 0, B());
  }

  HANDLER_ATTRIBUTES bool IF_LEZ() {
    return HandleIf(GetVReg(A()) <= 0, B());
  }

  HANDLER_ATTRIBUTES bool AGET_BOOLEAN() {
    return HandleAGet<mirror::BooleanArray>(&InstructionHandler::SetVReg);
  }

  HANDLER_ATTRIBUTES bool AGET_BYTE() {
    return HandleAGet<mirror::ByteArray>(&InstructionHandler::SetVReg);
  }

  HANDLER_ATTRIBUTES bool AGET_CHAR() {
    return HandleAGet<mirror::CharArray>(&InstructionHandler::SetVReg);
  }

  HANDLER_ATTRIBUTES bool AGET_SHORT() {
    return HandleAGet<mirror::ShortArray>(&InstructionHandler::SetVReg);
  }

  HANDLER_ATTRIBUTES bool AGET() {
    return HandleAGet<mirror::IntArray>(&InstructionHandler::SetVReg);
  }

  HANDLER_ATTRIBUTES bool AGET_WIDE() {
    return HandleAGet<mirror::LongArray>(&InstructionHandler::SetVRegLong);
  }

  HANDLER_ATTRIBUTES bool AGET_OBJECT() {
    return HandleAGet<mirror::ObjectArray<mirror::Object>>(&InstructionHandler::SetVRegReference);
  }

  HANDLER_ATTRIBUTES bool APUT_BOOLEAN() {
    return HandleAPut<mirror::BooleanArray>(GetVReg(A()));
  }

  HANDLER_ATTRIBUTES bool APUT_BYTE() {
    return HandleAPut<mirror::ByteArray>(GetVReg(A()));
  }

  HANDLER_ATTRIBUTES bool APUT_CHAR() {
    return HandleAPut<mirror::CharArray>(GetVReg(A()));
  }

  HANDLER_ATTRIBUTES bool APUT_SHORT() {
    return HandleAPut<mirror::ShortArray>(GetVReg(A()));
  }

  HANDLER_ATTRIBUTES bool APUT() {
    return HandleAPut<mirror::IntArray>(GetVReg(A()));
  }

  HANDLER_ATTRIBUTES bool APUT_WIDE() {
    return HandleAPut<mirror::LongArray>(GetVRegLong(A()));
  }

  HANDLER_ATTRIBUTES bool APUT_OBJECT() {
    ObjPtr<mirror::Object> a = GetVRegReference(B());
    if (UNLIKELY(a == nullptr)) {
      ThrowNullPointerExceptionFromInterpreter();
      return false;  // Pending exception.
    }
    int32_t index = GetVReg(C());
    ObjPtr<mirror::Object> val = GetVRegReference(A());
    ObjPtr<mirror::ObjectArray<mirror::Object>> array = a->AsObjectArray<mirror::Object>();
    if (array->CheckIsValidIndex(index) && array->CheckAssignable(val)) {
      if (TransactionChecker::WriteConstraint(Self(), array) ||
          TransactionChecker::WriteValueConstraint(Self(), val)) {
        return false;
      }
      array->SetWithoutChecks<transaction_active>(index, val);
    } else {
      return false;  // Pending exception.
    }
    return true;
  }

  HANDLER_ATTRIBUTES bool IGET_BOOLEAN() {
    return HandleGet<InstancePrimitiveRead, Primitive::kPrimBoolean>();
  }

  HANDLER_ATTRIBUTES bool IGET_BYTE() {
    return HandleGet<InstancePrimitiveRead, Primitive::kPrimByte>();
  }

  HANDLER_ATTRIBUTES bool IGET_CHAR() {
    return HandleGet<InstancePrimitiveRead, Primitive::kPrimChar>();
  }

  HANDLER_ATTRIBUTES bool IGET_SHORT() {
    return HandleGet<InstancePrimitiveRead, Primitive::kPrimShort>();
  }

  HANDLER_ATTRIBUTES bool IGET() {
    return HandleGet<InstancePrimitiveRead, Primitive::kPrimInt>();
  }

  HANDLER_ATTRIBUTES bool IGET_WIDE() {
    return HandleGet<InstancePrimitiveRead, Primitive::kPrimLong>();
  }

  HANDLER_ATTRIBUTES bool IGET_OBJECT() {
    return HandleGet<InstanceObjectRead, Primitive::kPrimNot>();
  }

  HANDLER_ATTRIBUTES bool SGET_BOOLEAN() {
    return HandleGet<StaticPrimitiveRead, Primitive::kPrimBoolean>();
  }

  HANDLER_ATTRIBUTES bool SGET_BYTE() {
    return HandleGet<StaticPrimitiveRead, Primitive::kPrimByte>();
  }

  HANDLER_ATTRIBUTES bool SGET_CHAR() {
    return HandleGet<StaticPrimitiveRead, Primitive::kPrimChar>();
  }

  HANDLER_ATTRIBUTES bool SGET_SHORT() {
    return HandleGet<StaticPrimitiveRead, Primitive::kPrimShort>();
  }

  HANDLER_ATTRIBUTES bool SGET() {
    return HandleGet<StaticPrimitiveRead, Primitive::kPrimInt>();
  }

  HANDLER_ATTRIBUTES bool SGET_WIDE() {
    return HandleGet<StaticPrimitiveRead, Primitive::kPrimLong>();
  }

  HANDLER_ATTRIBUTES bool SGET_OBJECT() {
    return HandleGet<StaticObjectRead, Primitive::kPrimNot>();
  }

  HANDLER_ATTRIBUTES bool IPUT_BOOLEAN() {
    return HandlePut<InstancePrimitiveWrite, Primitive::kPrimBoolean>();
  }

  HANDLER_ATTRIBUTES bool IPUT_BYTE() {
    return HandlePut<InstancePrimitiveWrite, Primitive::kPrimByte>();
  }

  HANDLER_ATTRIBUTES bool IPUT_CHAR() {
    return HandlePut<InstancePrimitiveWrite, Primitive::kPrimChar>();
  }

  HANDLER_ATTRIBUTES bool IPUT_SHORT() {
    return HandlePut<InstancePrimitiveWrite, Primitive::kPrimShort>();
  }

  HANDLER_ATTRIBUTES bool IPUT() {
    return HandlePut<InstancePrimitiveWrite, Primitive::kPrimInt>();
  }

  HANDLER_ATTRIBUTES bool IPUT_WIDE() {
    return HandlePut<InstancePrimitiveWrite, Primitive::kPrimLong>();
  }

  HANDLER_ATTRIBUTES bool IPUT_OBJECT() {
    return HandlePut<InstanceObjectWrite, Primitive::kPrimNot>();
  }

  HANDLER_ATTRIBUTES bool SPUT_BOOLEAN() {
    return HandlePut<StaticPrimitiveWrite, Primitive::kPrimBoolean>();
  }

  HANDLER_ATTRIBUTES bool SPUT_BYTE() {
    return HandlePut<StaticPrimitiveWrite, Primitive::kPrimByte>();
  }

  HANDLER_ATTRIBUTES bool SPUT_CHAR() {
    return HandlePut<StaticPrimitiveWrite, Primitive::kPrimChar>();
  }

  HANDLER_ATTRIBUTES bool SPUT_SHORT() {
    return HandlePut<StaticPrimitiveWrite, Primitive::kPrimShort>();
  }

  HANDLER_ATTRIBUTES bool SPUT() {
    return HandlePut<StaticPrimitiveWrite, Primitive::kPrimInt>();
  }

  HANDLER_ATTRIBUTES bool SPUT_WIDE() {
    return HandlePut<StaticPrimitiveWrite, Primitive::kPrimLong>();
  }

  HANDLER_ATTRIBUTES bool SPUT_OBJECT() {
    return HandlePut<StaticObjectWrite, Primitive::kPrimNot>();
  }

  HANDLER_ATTRIBUTES bool INVOKE_VIRTUAL() {
    return HandleInvoke<kVirtual, /*is_range=*/ false>();
  }

  HANDLER_ATTRIBUTES bool INVOKE_VIRTUAL_RANGE() {
    return HandleInvoke<kVirtual, /*is_range=*/ true>();
  }

  HANDLER_ATTRIBUTES bool INVOKE_SUPER() {
    return HandleInvoke<kSuper, /*is_range=*/ false>();
  }

  HANDLER_ATTRIBUTES bool INVOKE_SUPER_RANGE() {
    return HandleInvoke<kSuper, /*is_range=*/ true>();
  }

  HANDLER_ATTRIBUTES bool INVOKE_DIRECT() {
    return HandleInvoke<kDirect, /*is_range=*/ false>();
  }

  HANDLER_ATTRIBUTES bool INVOKE_DIRECT_RANGE() {
    return HandleInvoke<kDirect, /*is_range=*/ true>();
  }

  HANDLER_ATTRIBUTES bool INVOKE_INTERFACE() {
    return HandleInvoke<kInterface, /*is_range=*/ false>();
  }

  HANDLER_ATTRIBUTES bool INVOKE_INTERFACE_RANGE() {
    return HandleInvoke<kInterface, /*is_range=*/ true>();
  }

  HANDLER_ATTRIBUTES bool INVOKE_STATIC() {
    return HandleInvoke<kStatic, /*is_range=*/ false>();
  }

  HANDLER_ATTRIBUTES bool INVOKE_STATIC_RANGE() {
    return HandleInvoke<kStatic, /*is_range=*/ true>();
  }

  HANDLER_ATTRIBUTES bool INVOKE_POLYMORPHIC() {
    DCHECK(Runtime::Current()->IsMethodHandlesEnabled());
    bool success = DoInvokePolymorphic</* is_range= */ false>(
        Self(), shadow_frame_, inst_, inst_data_, ResultRegister());
    return PossiblyHandlePendingExceptionOnInvoke(!success);
  }

  HANDLER_ATTRIBUTES bool INVOKE_POLYMORPHIC_RANGE() {
    DCHECK(Runtime::Current()->IsMethodHandlesEnabled());
    bool success = DoInvokePolymorphic</* is_range= */ true>(
        Self(), shadow_frame_, inst_, inst_data_, ResultRegister());
    return PossiblyHandlePendingExceptionOnInvoke(!success);
  }

  HANDLER_ATTRIBUTES bool INVOKE_CUSTOM() {
    DCHECK(Runtime::Current()->IsMethodHandlesEnabled());
    bool success = DoInvokeCustom</* is_range= */ false>(
        Self(), shadow_frame_, inst_, inst_data_, ResultRegister());
    return PossiblyHandlePendingExceptionOnInvoke(!success);
  }

  HANDLER_ATTRIBUTES bool INVOKE_CUSTOM_RANGE() {
    DCHECK(Runtime::Current()->IsMethodHandlesEnabled());
    bool success = DoInvokeCustom</* is_range= */ true>(
        Self(), shadow_frame_, inst_, inst_data_, ResultRegister());
    return PossiblyHandlePendingExceptionOnInvoke(!success);
  }

  HANDLER_ATTRIBUTES bool NEG_INT() {
    SetVReg(A(), -GetVReg(B()));
    return true;
  }

  HANDLER_ATTRIBUTES bool NOT_INT() {
    SetVReg(A(), ~GetVReg(B()));
    return true;
  }

  HANDLER_ATTRIBUTES bool NEG_LONG() {
    SetVRegLong(A(), -GetVRegLong(B()));
    return true;
  }

  HANDLER_ATTRIBUTES bool NOT_LONG() {
    SetVRegLong(A(), ~GetVRegLong(B()));
    return true;
  }

  HANDLER_ATTRIBUTES bool NEG_FLOAT() {
    SetVRegFloat(A(), -GetVRegFloat(B()));
    return true;
  }

  HANDLER_ATTRIBUTES bool NEG_DOUBLE() {
    SetVRegDouble(A(), -GetVRegDouble(B()));
    return true;
  }

  HANDLER_ATTRIBUTES bool INT_TO_LONG() {
    SetVRegLong(A(), GetVReg(B()));
    return true;
  }

  HANDLER_ATTRIBUTES bool INT_TO_FLOAT() {
    SetVRegFloat(A(), GetVReg(B()));
    return true;
  }

  HANDLER_ATTRIBUTES bool INT_TO_DOUBLE() {
    SetVRegDouble(A(), GetVReg(B()));
    return true;
  }

  HANDLER_ATTRIBUTES bool LONG_TO_INT() {
    SetVReg(A(), GetVRegLong(B()));
    return true;
  }

  HANDLER_ATTRIBUTES bool LONG_TO_FLOAT() {
    SetVRegFloat(A(), GetVRegLong(B()));
    return true;
  }

  HANDLER_ATTRIBUTES bool LONG_TO_DOUBLE() {
    SetVRegDouble(A(), GetVRegLong(B()));
    return true;
  }

  HANDLER_ATTRIBUTES bool FLOAT_TO_INT() {
    SetVReg(A(), art_float_to_integral<int32_t, float>(GetVRegFloat(B())));
    return true;
  }

  HANDLER_ATTRIBUTES bool FLOAT_TO_LONG() {
    SetVRegLong(A(), art_float_to_integral<int64_t, float>(GetVRegFloat(B())));
    return true;
  }

  HANDLER_ATTRIBUTES bool FLOAT_TO_DOUBLE() {
    SetVRegDouble(A(), GetVRegFloat(B()));
    return true;
  }

  HANDLER_ATTRIBUTES bool DOUBLE_TO_INT() {
    SetVReg(A(), art_float_to_integral<int32_t, double>(GetVRegDouble(B())));
    return true;
  }

  HANDLER_ATTRIBUTES bool DOUBLE_TO_LONG() {
    SetVRegLong(A(), art_float_to_integral<int64_t, double>(GetVRegDouble(B())));
    return true;
  }

  HANDLER_ATTRIBUTES bool DOUBLE_TO_FLOAT() {
    SetVRegFloat(A(), GetVRegDouble(B()));
    return true;
  }

  HANDLER_ATTRIBUTES bool INT_TO_BYTE() {
    SetVReg(A(), static_cast<int8_t>(GetVReg(B())));
    return true;
  }

  HANDLER_ATTRIBUTES bool INT_TO_CHAR() {
    SetVReg(A(), static_cast<uint16_t>(GetVReg(B())));
    return true;
  }

  HANDLER_ATTRIBUTES bool INT_TO_SHORT() {
    SetVReg(A(), static_cast<int16_t>(GetVReg(B())));
    return true;
  }

  HANDLER_ATTRIBUTES bool ADD_INT() {
    SetVReg(A(), SafeAdd(GetVReg(B()), GetVReg(C())));
    return true;
  }

  HANDLER_ATTRIBUTES bool SUB_INT() {
    SetVReg(A(), SafeSub(GetVReg(B()), GetVReg(C())));
    return true;
  }

  HANDLER_ATTRIBUTES bool MUL_INT() {
    SetVReg(A(), SafeMul(GetVReg(B()), GetVReg(C())));
    return true;
  }

  HANDLER_ATTRIBUTES bool DIV_INT() {
    return DoIntDivide(shadow_frame_, A(), GetVReg(B()), GetVReg(C()));
  }

  HANDLER_ATTRIBUTES bool REM_INT() {
    return DoIntRemainder(shadow_frame_, A(), GetVReg(B()), GetVReg(C()));
  }

  HANDLER_ATTRIBUTES bool SHL_INT() {
    SetVReg(A(), GetVReg(B()) << (GetVReg(C()) & 0x1f));
    return true;
  }

  HANDLER_ATTRIBUTES bool SHR_INT() {
    SetVReg(A(), GetVReg(B()) >> (GetVReg(C()) & 0x1f));
    return true;
  }

  HANDLER_ATTRIBUTES bool USHR_INT() {
    SetVReg(A(), static_cast<uint32_t>(GetVReg(B())) >> (GetVReg(C()) & 0x1f));
    return true;
  }

  HANDLER_ATTRIBUTES bool AND_INT() {
    SetVReg(A(), GetVReg(B()) & GetVReg(C()));
    return true;
  }

  HANDLER_ATTRIBUTES bool OR_INT() {
    SetVReg(A(), GetVReg(B()) | GetVReg(C()));
    return true;
  }

  HANDLER_ATTRIBUTES bool XOR_INT() {
    SetVReg(A(), GetVReg(B()) ^ GetVReg(C()));
    return true;
  }

  HANDLER_ATTRIBUTES bool ADD_LONG() {
    SetVRegLong(A(), SafeAdd(GetVRegLong(B()), GetVRegLong(C())));
    return true;
  }

  HANDLER_ATTRIBUTES bool SUB_LONG() {
    SetVRegLong(A(), SafeSub(GetVRegLong(B()), GetVRegLong(C())));
    return true;
  }

  HANDLER_ATTRIBUTES bool MUL_LONG() {
    SetVRegLong(A(), SafeMul(GetVRegLong(B()), GetVRegLong(C())));
    return true;
  }

  HANDLER_ATTRIBUTES bool DIV_LONG() {
    return DoLongDivide(shadow_frame_, A(), GetVRegLong(B()), GetVRegLong(C()));
  }

  HANDLER_ATTRIBUTES bool REM_LONG() {
    return DoLongRemainder(shadow_frame_, A(), GetVRegLong(B()), GetVRegLong(C()));
  }

  HANDLER_ATTRIBUTES bool AND_LONG() {
    SetVRegLong(A(), GetVRegLong(B()) & GetVRegLong(C()));
    return true;
  }

  HANDLER_ATTRIBUTES bool OR_LONG() {
    SetVRegLong(A(), GetVRegLong(B()) | GetVRegLong(C()));
    return true;
  }

  HANDLER_ATTRIBUTES bool XOR_LONG() {
    SetVRegLong(A(), GetVRegLong(B()) ^ GetVRegLong(C()));
    return true;
  }

  HANDLER_ATTRIBUTES bool SHL_LONG() {
    SetVRegLong(A(), GetVRegLong(B()) << (GetVReg(C()) & 0x3f));
    return true;
  }

  HANDLER_ATTRIBUTES bool SHR_LONG() {
    SetVRegLong(A(), GetVRegLong(B()) >> (GetVReg(C()) & 0x3f));
    return true;
  }

  HANDLER_ATTRIBUTES bool USHR_LONG() {
    SetVRegLong(A(), static_cast<uint64_t>(GetVRegLong(B())) >> (GetVReg(C()) & 0x3f));
    return true;
  }

  HANDLER_ATTRIBUTES bool ADD_FLOAT() {
    SetVRegFloat(A(), GetVRegFloat(B()) + GetVRegFloat(C()));
    return true;
  }

  HANDLER_ATTRIBUTES bool SUB_FLOAT() {
    SetVRegFloat(A(), GetVRegFloat(B()) - GetVRegFloat(C()));
    return true;
  }

  HANDLER_ATTRIBUTES bool MUL_FLOAT() {
    SetVRegFloat(A(), GetVRegFloat(B()) * GetVRegFloat(C()));
    return true;
  }

  HANDLER_ATTRIBUTES bool DIV_FLOAT() {
    SetVRegFloat(A(), GetVRegFloat(B()) / GetVRegFloat(C()));
    return true;
  }

  HANDLER_ATTRIBUTES bool REM_FLOAT() {
    SetVRegFloat(A(), fmodf(GetVRegFloat(B()), GetVRegFloat(C())));
    return true;
  }

  HANDLER_ATTRIBUTES bool ADD_DOUBLE() {
    SetVRegDouble(A(), GetVRegDouble(B()) + GetVRegDouble(C()));
    return true;
  }

  HANDLER_ATTRIBUTES bool SUB_DOUBLE() {
    SetVRegDouble(A(), GetVRegDouble(B()) - GetVRegDouble(C()));
    return true;
  }

  HANDLER_ATTRIBUTES bool MUL_DOUBLE() {
    SetVRegDouble(A(), GetVRegDouble(B()) * GetVRegDouble(C()));
    return true;
  }

  HANDLER_ATTRIBUTES bool DIV_DOUBLE() {
    SetVRegDouble(A(), GetVRegDouble(B()) / GetVRegDouble(C()));
    return true;
  }

  HANDLER_ATTRIBUTES bool REM_DOUBLE() {
    SetVRegDouble(A(), fmod(GetVRegDouble(B()), GetVRegDouble(C())));
    return true;
  }

  HANDLER_ATTRIBUTES bool ADD_INT_2ADDR() {
    SetVReg(A(), SafeAdd(GetVReg(A()), GetVReg(B())));
    return true;
  }

  HANDLER_ATTRIBUTES bool SUB_INT_2ADDR() {
    SetVReg(A(), SafeSub(GetVReg(A()), GetVReg(B())));
    return true;
  }

  HANDLER_ATTRIBUTES bool MUL_INT_2ADDR() {
    SetVReg(A(), SafeMul(GetVReg(A()), GetVReg(B())));
    return true;
  }

  HANDLER_ATTRIBUTES bool DIV_INT_2ADDR() {
    return DoIntDivide(shadow_frame_, A(), GetVReg(A()), GetVReg(B()));
  }

  HANDLER_ATTRIBUTES bool REM_INT_2ADDR() {
    return DoIntRemainder(shadow_frame_, A(), GetVReg(A()), GetVReg(B()));
  }

  HANDLER_ATTRIBUTES bool SHL_INT_2ADDR() {
    SetVReg(A(), GetVReg(A()) << (GetVReg(B()) & 0x1f));
    return true;
  }

  HANDLER_ATTRIBUTES bool SHR_INT_2ADDR() {
    SetVReg(A(), GetVReg(A()) >> (GetVReg(B()) & 0x1f));
    return true;
  }

  HANDLER_ATTRIBUTES bool USHR_INT_2ADDR() {
    SetVReg(A(), static_cast<uint32_t>(GetVReg(A())) >> (GetVReg(B()) & 0x1f));
    return true;
  }

  HANDLER_ATTRIBUTES bool AND_INT_2ADDR() {
    SetVReg(A(), GetVReg(A()) & GetVReg(B()));
    return true;
  }

  HANDLER_ATTRIBUTES bool OR_INT_2ADDR() {
    SetVReg(A(), GetVReg(A()) | GetVReg(B()));
    return true;
  }

  HANDLER_ATTRIBUTES bool XOR_INT_2ADDR() {
    SetVReg(A(), GetVReg(A()) ^ GetVReg(B()));
    return true;
  }

  HANDLER_ATTRIBUTES bool ADD_LONG_2ADDR() {
    SetVRegLong(A(), SafeAdd(GetVRegLong(A()), GetVRegLong(B())));
    return true;
  }

  HANDLER_ATTRIBUTES bool SUB_LONG_2ADDR() {
    SetVRegLong(A(), SafeSub(GetVRegLong(A()), GetVRegLong(B())));
    return true;
  }

  HANDLER_ATTRIBUTES bool MUL_LONG_2ADDR() {
    SetVRegLong(A(), SafeMul(GetVRegLong(A()), GetVRegLong(B())));
    return true;
  }

  HANDLER_ATTRIBUTES bool DIV_LONG_2ADDR() {
    return DoLongDivide(shadow_frame_, A(), GetVRegLong(A()), GetVRegLong(B()));
  }

  HANDLER_ATTRIBUTES bool REM_LONG_2ADDR() {
    return DoLongRemainder(shadow_frame_, A(), GetVRegLong(A()), GetVRegLong(B()));
  }

  HANDLER_ATTRIBUTES bool AND_LONG_2ADDR() {
    SetVRegLong(A(), GetVRegLong(A()) & GetVRegLong(B()));
    return true;
  }

  HANDLER_ATTRIBUTES bool OR_LONG_2ADDR() {
    SetVRegLong(A(), GetVRegLong(A()) | GetVRegLong(B()));
    return true;
  }

  HANDLER_ATTRIBUTES bool XOR_LONG_2ADDR() {
    SetVRegLong(A(), GetVRegLong(A()) ^ GetVRegLong(B()));
    return true;
  }

  HANDLER_ATTRIBUTES bool SHL_LONG_2ADDR() {
    SetVRegLong(A(), GetVRegLong(A()) << (GetVReg(B()) & 0x3f));
    return true;
  }

  HANDLER_ATTRIBUTES bool SHR_LONG_2ADDR() {
    SetVRegLong(A(), GetVRegLong(A()) >> (GetVReg(B()) & 0x3f));
    return true;
  }

  HANDLER_ATTRIBUTES bool USHR_LONG_2ADDR() {
    SetVRegLong(A(), static_cast<uint64_t>(GetVRegLong(A())) >> (GetVReg(B()) & 0x3f));
    return true;
  }

  HANDLER_ATTRIBUTES bool ADD_FLOAT_2ADDR() {
    SetVRegFloat(A(), GetVRegFloat(A()) + GetVRegFloat(B()));
    return true;
  }

  HANDLER_ATTRIBUTES bool SUB_FLOAT_2ADDR() {
    SetVRegFloat(A(), GetVRegFloat(A()) - GetVRegFloat(B()));
    return true;
  }

  HANDLER_ATTRIBUTES bool MUL_FLOAT_2ADDR() {
    SetVRegFloat(A(), GetVRegFloat(A()) * GetVRegFloat(B()));
    return true;
  }

  HANDLER_ATTRIBUTES bool DIV_FLOAT_2ADDR() {
    SetVRegFloat(A(), GetVRegFloat(A()) / GetVRegFloat(B()));
    return true;
  }

  HANDLER_ATTRIBUTES bool REM_FLOAT_2ADDR() {
    SetVRegFloat(A(), fmodf(GetVRegFloat(A()), GetVRegFloat(B())));
    return true;
  }

  HANDLER_ATTRIBUTES bool ADD_DOUBLE_2ADDR() {
    SetVRegDouble(A(), GetVRegDouble(A()) + GetVRegDouble(B()));
    return true;
  }

  HANDLER_ATTRIBUTES bool SUB_DOUBLE_2ADDR() {
    SetVRegDouble(A(), GetVRegDouble(A()) - GetVRegDouble(B()));
    return true;
  }

  HANDLER_ATTRIBUTES bool MUL_DOUBLE_2ADDR() {
    SetVRegDouble(A(), GetVRegDouble(A()) * GetVRegDouble(B()));
    return true;
  }

  HANDLER_ATTRIBUTES bool DIV_DOUBLE_2ADDR() {
    SetVRegDouble(A(), GetVRegDouble(A()) / GetVRegDouble(B()));
    return true;
  }

  HANDLER_ATTRIBUTES bool REM_DOUBLE_2ADDR() {
    SetVRegDouble(A(), fmod(GetVRegDouble(A()), GetVRegDouble(B())));
    return true;
  }

  HANDLER_ATTRIBUTES bool ADD_INT_LIT16() {
    SetVReg(A(), SafeAdd(GetVReg(B()), C()));
    return true;
  }

  HANDLER_ATTRIBUTES bool RSUB_INT() {
    SetVReg(A(), SafeSub(C(), GetVReg(B())));
    return true;
  }

  HANDLER_ATTRIBUTES bool MUL_INT_LIT16() {
    SetVReg(A(), SafeMul(GetVReg(B()), C()));
    return true;
  }

  HANDLER_ATTRIBUTES bool DIV_INT_LIT16() {
    return DoIntDivide(shadow_frame_, A(), GetVReg(B()), C());
  }

  HANDLER_ATTRIBUTES bool REM_INT_LIT16() {
    return DoIntRemainder(shadow_frame_, A(), GetVReg(B()), C());
  }

  HANDLER_ATTRIBUTES bool AND_INT_LIT16() {
    SetVReg(A(), GetVReg(B()) & C());
    return true;
  }

  HANDLER_ATTRIBUTES bool OR_INT_LIT16() {
    SetVReg(A(), GetVReg(B()) | C());
    return true;
  }

  HANDLER_ATTRIBUTES bool XOR_INT_LIT16() {
    SetVReg(A(), GetVReg(B()) ^ C());
    return true;
  }

  HANDLER_ATTRIBUTES bool ADD_INT_LIT8() {
    SetVReg(A(), SafeAdd(GetVReg(B()), C()));
    return true;
  }

  HANDLER_ATTRIBUTES bool RSUB_INT_LIT8() {
    SetVReg(A(), SafeSub(C(), GetVReg(B())));
    return true;
  }

  HANDLER_ATTRIBUTES bool MUL_INT_LIT8() {
    SetVReg(A(), SafeMul(GetVReg(B()), C()));
    return true;
  }

  HANDLER_ATTRIBUTES bool DIV_INT_LIT8() {
    return DoIntDivide(shadow_frame_, A(), GetVReg(B()), C());
  }

  HANDLER_ATTRIBUTES bool REM_INT_LIT8() {
    return DoIntRemainder(shadow_frame_, A(), GetVReg(B()), C());
  }

  HANDLER_ATTRIBUTES bool AND_INT_LIT8() {
    SetVReg(A(), GetVReg(B()) & C());
    return true;
  }

  HANDLER_ATTRIBUTES bool OR_INT_LIT8() {
    SetVReg(A(), GetVReg(B()) | C());
    return true;
  }

  HANDLER_ATTRIBUTES bool XOR_INT_LIT8() {
    SetVReg(A(), GetVReg(B()) ^ C());
    return true;
  }

  HANDLER_ATTRIBUTES bool SHL_INT_LIT8() {
    SetVReg(A(), GetVReg(B()) << (C() & 0x1f));
    return true;
  }

  HANDLER_ATTRIBUTES bool SHR_INT_LIT8() {
    SetVReg(A(), GetVReg(B()) >> (C() & 0x1f));
    return true;
  }

  HANDLER_ATTRIBUTES bool USHR_INT_LIT8() {
    SetVReg(A(), static_cast<uint32_t>(GetVReg(B())) >> (C() & 0x1f));
    return true;
  }

  HANDLER_ATTRIBUTES bool UNUSED_3E() {
    return HandleUnused();
  }

  HANDLER_ATTRIBUTES bool UNUSED_3F() {
    return HandleUnused();
  }

  HANDLER_ATTRIBUTES bool UNUSED_40() {
    return HandleUnused();
  }

  HANDLER_ATTRIBUTES bool UNUSED_41() {
    return HandleUnused();
  }

  HANDLER_ATTRIBUTES bool UNUSED_42() {
    return HandleUnused();
  }

  HANDLER_ATTRIBUTES bool UNUSED_43() {
    return HandleUnused();
  }

  HANDLER_ATTRIBUTES bool UNUSED_73() {
    return HandleUnused();
  }

  HANDLER_ATTRIBUTES bool UNUSED_79() {
    return HandleUnused();
  }

  HANDLER_ATTRIBUTES bool UNUSED_7A() {
    return HandleUnused();
  }

  HANDLER_ATTRIBUTES bool UNUSED_E3() {
    return HandleUnused();
  }

  HANDLER_ATTRIBUTES bool UNUSED_E4() {
    return HandleUnused();
  }

  HANDLER_ATTRIBUTES bool UNUSED_E5() {
    return HandleUnused();
  }

  HANDLER_ATTRIBUTES bool UNUSED_E6() {
    return HandleUnused();
  }

  HANDLER_ATTRIBUTES bool UNUSED_E7() {
    return HandleUnused();
  }

  HANDLER_ATTRIBUTES bool UNUSED_E8() {
    return HandleUnused();
  }

  HANDLER_ATTRIBUTES bool UNUSED_E9() {
    return HandleUnused();
  }

  HANDLER_ATTRIBUTES bool UNUSED_EA() {
    return HandleUnused();
  }

  HANDLER_ATTRIBUTES bool UNUSED_EB() {
    return HandleUnused();
  }

  HANDLER_ATTRIBUTES bool UNUSED_EC() {
    return HandleUnused();
  }

  HANDLER_ATTRIBUTES bool UNUSED_ED() {
    return HandleUnused();
  }

  HANDLER_ATTRIBUTES bool UNUSED_EE() {
    return HandleUnused();
  }

  HANDLER_ATTRIBUTES bool UNUSED_EF() {
    return HandleUnused();
  }

  HANDLER_ATTRIBUTES bool UNUSED_F0() {
    return HandleUnused();
  }

  HANDLER_ATTRIBUTES bool UNUSED_F1() {
    return HandleUnused();
  }

  HANDLER_ATTRIBUTES bool UNUSED_F2() {
    return HandleUnused();
  }

  HANDLER_ATTRIBUTES bool UNUSED_F3() {
    return HandleUnused();
  }

  HANDLER_ATTRIBUTES bool UNUSED_F4() {
    return HandleUnused();
  }

  HANDLER_ATTRIBUTES bool UNUSED_F5() {
    return HandleUnused();
  }

  HANDLER_ATTRIBUTES bool UNUSED_F6() {
    return HandleUnused();
  }

  HANDLER_ATTRIBUTES bool UNUSED_F7() {
    return HandleUnused();
  }

  HANDLER_ATTRIBUTES bool UNUSED_F8() {
    return HandleUnused();
  }

  HANDLER_ATTRIBUTES bool UNUSED_F9() {
    return HandleUnused();
  }

  ALWAYS_INLINE InstructionHandler(SwitchImplContext* ctx,
                                   const instrumentation::Instrumentation* instrumentation,
                                   Thread* self,
                                   ShadowFrame& shadow_frame,
                                   uint16_t dex_pc,
                                   const Instruction* inst,
                                   uint16_t inst_data,
                                   const Instruction*& next,
                                   bool& exit_interpreter_loop)
    : ctx_(ctx),
      instrumentation_(instrumentation),
      self_(self),
      shadow_frame_(shadow_frame),
      dex_pc_(dex_pc),
      inst_(inst),
      inst_data_(inst_data),
      next_(next),
      exit_interpreter_loop_(exit_interpreter_loop) {
  }

 private:
  bool DoAssignabilityChecks() const REQUIRES_SHARED(Locks::mutator_lock_) {
    return !shadow_frame_.GetMethod()->SkipAccessChecks();
  }

  ALWAYS_INLINE const CodeItemDataAccessor& Accessor() { return ctx_->accessor; }
  ALWAYS_INLINE const uint16_t* Insns() { return ctx_->accessor.Insns(); }
  ALWAYS_INLINE JValue* ResultRegister() { return &ctx_->result_register; }

  ALWAYS_INLINE Thread* Self() {
    DCHECK_EQ(self_, Thread::Current());
    return self_;
  }

  ALWAYS_INLINE int32_t DexPC() {
    DCHECK_EQ(dex_pc_, shadow_frame_.GetDexPC());
    return dex_pc_;
  }

  ALWAYS_INLINE const instrumentation::Instrumentation* Instrumentation() {
    return instrumentation_;
  }

  ALWAYS_INLINE int32_t A() { return inst_->VRegA(kFormat, inst_data_); }
  ALWAYS_INLINE int32_t B() { return inst_->VRegB(kFormat, inst_data_); }
  ALWAYS_INLINE int32_t C() { return inst_->VRegC(kFormat); }

  int32_t GetVReg(size_t i) const { return shadow_frame_.GetVReg(i); }
  int64_t GetVRegLong(size_t i) const { return shadow_frame_.GetVRegLong(i); }
  float GetVRegFloat(size_t i) const { return shadow_frame_.GetVRegFloat(i); }
  double GetVRegDouble(size_t i) const { return shadow_frame_.GetVRegDouble(i); }
  ObjPtr<mirror::Object> GetVRegReference(size_t i) const REQUIRES_SHARED(Locks::mutator_lock_) {
    return shadow_frame_.GetVRegReference(i);
  }

  void SetVReg(size_t i, int32_t val) { shadow_frame_.SetVReg(i, val); }
  void SetVRegLong(size_t i, int64_t val) { shadow_frame_.SetVRegLong(i, val); }
  void SetVRegFloat(size_t i, float val) { shadow_frame_.SetVRegFloat(i, val); }
  void SetVRegDouble(size_t i, double val) { shadow_frame_.SetVRegDouble(i, val); }
  void SetVRegReference(size_t i, ObjPtr<mirror::Object> val)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    shadow_frame_.SetVRegReference(i, val);
  }

  // Set the next instruction to be executed.  It is the 'fall-through' instruction by default.
  ALWAYS_INLINE void SetNextInstruction(const Instruction* next_inst) {
    DCHECK_LT(next_inst->GetDexPc(Insns()), Accessor().InsnsSizeInCodeUnits());
    next_ = next_inst;
  }

  // Stop interpreting the current method. (return statement, debugger-forced return, OSR, ...)
  ALWAYS_INLINE void ExitInterpreterLoop() {
    exit_interpreter_loop_ = true;
  }

  SwitchImplContext* const ctx_;
  const instrumentation::Instrumentation* const instrumentation_;
  Thread* const self_;
  ShadowFrame& shadow_frame_;
  uint32_t const dex_pc_;
  const Instruction* const inst_;
  uint16_t const inst_data_;
  const Instruction*& next_;

  bool& exit_interpreter_loop_;
};

// Don't inline in ASAN. It would create massive stack frame.
#if defined(ADDRESS_SANITIZER) || defined(HWADDRESS_SANITIZER)
#define ASAN_NO_INLINE NO_INLINE
#else
#define ASAN_NO_INLINE ALWAYS_INLINE
#endif

#define OPCODE_CASE(OPCODE, OPCODE_NAME, NAME, FORMAT, i, a, e, v)                                \
template<bool transaction_active>                                                                 \
ASAN_NO_INLINE NO_STACK_PROTECTOR static bool OP_##OPCODE_NAME(                                   \
    SwitchImplContext* ctx,                                                                       \
    const instrumentation::Instrumentation* instrumentation,                                      \
    Thread* self,                                                                                 \
    ShadowFrame& shadow_frame,                                                                    \
    uint16_t dex_pc,                                                                              \
    const Instruction* inst,                                                                      \
    uint16_t inst_data,                                                                           \
    const Instruction*& next,                                                                     \
    bool& exit) REQUIRES_SHARED(Locks::mutator_lock_) {                                           \
  InstructionHandler<transaction_active, Instruction::FORMAT> handler(                            \
      ctx, instrumentation, self, shadow_frame, dex_pc, inst, inst_data, next, exit);             \
  return LIKELY(handler.OPCODE_NAME());                                                           \
}
DEX_INSTRUCTION_LIST(OPCODE_CASE)
#undef OPCODE_CASE

template<bool transaction_active>
NO_STACK_PROTECTOR
void ExecuteSwitchImplCpp(SwitchImplContext* ctx) {
  Thread* self = ctx->self;
  const CodeItemDataAccessor& accessor = ctx->accessor;
  ShadowFrame& shadow_frame = ctx->shadow_frame;
  self->VerifyStack();

  uint32_t dex_pc = shadow_frame.GetDexPC();
  const auto* const instrumentation = Runtime::Current()->GetInstrumentation();
  const uint16_t* const insns = accessor.Insns();
  const Instruction* next = Instruction::At(insns + dex_pc);

  DCHECK(!shadow_frame.GetForceRetryInstruction())
      << "Entered interpreter from invoke without retry instruction being handled!";

  while (true) {
    const Instruction* const inst = next;
    dex_pc = inst->GetDexPc(insns);
    shadow_frame.SetDexPC(dex_pc);
    TraceExecution(shadow_frame, inst, dex_pc);
    uint16_t inst_data = inst->Fetch16(0);
    bool exit = false;
    bool success;  // Moved outside to keep frames small under asan.
    if (InstructionHandler<transaction_active, Instruction::kInvalidFormat>(
            ctx, instrumentation, self, shadow_frame, dex_pc, inst, inst_data, next, exit).
            Preamble()) {
      DCHECK_EQ(self->IsExceptionPending(), inst->Opcode(inst_data) == Instruction::MOVE_EXCEPTION);
      switch (inst->Opcode(inst_data)) {
#define OPCODE_CASE(OPCODE, OPCODE_NAME, NAME, FORMAT, i, a, e, v)                                \
        case OPCODE: {                                                                            \
          next = inst->RelativeAt(Instruction::SizeInCodeUnits(Instruction::FORMAT));             \
          success = OP_##OPCODE_NAME<transaction_active>(                                         \
              ctx, instrumentation, self, shadow_frame, dex_pc, inst, inst_data, next, exit);     \
          if (success) {                                                                          \
            continue;                                                                             \
          }                                                                                       \
          break;                                                                                  \
        }
  DEX_INSTRUCTION_LIST(OPCODE_CASE)
#undef OPCODE_CASE
      }
    }
    if (exit) {
      shadow_frame.SetDexPC(dex::kDexNoIndex);
      return;  // Return statement or debugger forced exit.
    }
    if (self->IsExceptionPending()) {
      if (!InstructionHandler<transaction_active, Instruction::kInvalidFormat>(
              ctx, instrumentation, self, shadow_frame, dex_pc, inst, inst_data, next, exit).
              HandlePendingException()) {
        shadow_frame.SetDexPC(dex::kDexNoIndex);
        return;  // Locally unhandled exception - return to caller.
      }
      // Continue execution in the catch block.
    }
  }
}  // NOLINT(readability/fn_size)

}  // namespace interpreter
}  // namespace art

#endif  // ART_RUNTIME_INTERPRETER_INTERPRETER_SWITCH_IMPL_INL_H_
