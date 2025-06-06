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

#ifndef ART_RUNTIME_INTERPRETER_INTERPRETER_COMMON_H_
#define ART_RUNTIME_INTERPRETER_INTERPRETER_COMMON_H_

#include "android-base/macros.h"
#include "instrumentation.h"
#include "interpreter.h"

#include <math.h>

#include <atomic>
#include <iostream>
#include <sstream>

#include <android-base/logging.h>
#include <android-base/stringprintf.h>

#include "art_field-inl.h"
#include "art_method-inl.h"
#include "base/locks.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/pointer_size.h"
#include "class_linker-inl.h"
#include "class_root-inl.h"
#include "common_dex_operations.h"
#include "common_throws.h"
#include "dex/dex_file-inl.h"
#include "dex/dex_instruction-inl.h"
#include "entrypoints/entrypoint_utils-inl.h"
#include "handle_scope-inl.h"
#include "interpreter_cache-inl.h"
#include "interpreter_switch_impl.h"
#include "intrinsics_list.h"
#include "jit/jit-inl.h"
#include "mirror/call_site.h"
#include "mirror/class-inl.h"
#include "mirror/dex_cache.h"
#include "mirror/method.h"
#include "mirror/method_handles_lookup.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/string-inl.h"
#include "obj_ptr.h"
#include "stack.h"
#include "thread.h"
#include "thread-inl.h"
#include "unstarted_runtime.h"
#include "verifier/method_verifier.h"

namespace art HIDDEN {
namespace interpreter {

EXPORT void ThrowNullPointerExceptionFromInterpreter()
    REQUIRES_SHARED(Locks::mutator_lock_);

static inline void DoMonitorEnter(Thread* self, ShadowFrame* frame, ObjPtr<mirror::Object> ref)
    NO_THREAD_SAFETY_ANALYSIS
    REQUIRES(!Roles::uninterruptible_) {
  DCHECK(!ref.IsNull());
  StackHandleScope<1> hs(self);
  Handle<mirror::Object> h_ref(hs.NewHandle(ref));
  h_ref->MonitorEnter(self);
  DCHECK(self->HoldsLock(h_ref.Get()));
  if (UNLIKELY(self->IsExceptionPending())) {
    bool unlocked = h_ref->MonitorExit(self);
    DCHECK(unlocked);
    return;
  }
  if (frame->GetMethod()->MustCountLocks()) {
    DCHECK(!frame->GetMethod()->SkipAccessChecks());
    frame->GetLockCountData().AddMonitor(self, h_ref.Get());
  }
}

static inline void DoMonitorExit(Thread* self, ShadowFrame* frame, ObjPtr<mirror::Object> ref)
    NO_THREAD_SAFETY_ANALYSIS
    REQUIRES(!Roles::uninterruptible_) {
  StackHandleScope<1> hs(self);
  Handle<mirror::Object> h_ref(hs.NewHandle(ref));
  h_ref->MonitorExit(self);
  if (frame->GetMethod()->MustCountLocks()) {
    DCHECK(!frame->GetMethod()->SkipAccessChecks());
    frame->GetLockCountData().RemoveMonitorOrThrow(self, h_ref.Get());
  }
}

static inline bool DoMonitorCheckOnExit(Thread* self, ShadowFrame* frame)
    NO_THREAD_SAFETY_ANALYSIS
    REQUIRES(!Roles::uninterruptible_) {
  if (frame->GetMethod()->MustCountLocks()) {
    DCHECK(!frame->GetMethod()->SkipAccessChecks());
    return frame->GetLockCountData().CheckAllMonitorsReleasedOrThrow(self);
  }
  return true;
}

// Invokes the given method. This is part of the invocation support and is used by DoInvoke,
// DoFastInvoke and DoInvokeVirtualQuick functions.
// Returns true on success, otherwise throws an exception and returns false.
template<bool is_range>
EXPORT bool DoCall(ArtMethod* called_method,
                   Thread* self,
                   ShadowFrame& shadow_frame,
                   const Instruction* inst,
                   uint16_t inst_data,
                   bool string_init,
                   JValue* result);

// Called by the switch interpreter to know if we can stay in it.
bool ShouldStayInSwitchInterpreter(ArtMethod* method)
    REQUIRES_SHARED(Locks::mutator_lock_);

// Throws exception if we are getting close to the end of the stack.
NO_INLINE bool CheckStackOverflow(Thread* self, size_t frame_size)
    REQUIRES_SHARED(Locks::mutator_lock_);


// Sends the normal method exit event.
// Returns true if the events succeeded and false if there is a pending exception.
template <typename T>
bool SendMethodExitEvents(
    Thread* self,
    const instrumentation::Instrumentation* instrumentation,
    ShadowFrame& frame,
    ArtMethod* method,
    T& result) REQUIRES_SHARED(Locks::mutator_lock_);

static inline ALWAYS_INLINE WARN_UNUSED bool
NeedsMethodExitEvent(const instrumentation::Instrumentation* ins)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  return ins->HasMethodExitListeners() || ins->HasWatchedFramePopListeners();
}

COLD_ATTR void UnlockHeldMonitors(Thread* self, ShadowFrame* shadow_frame)
    REQUIRES_SHARED(Locks::mutator_lock_);

EXPORT
void PerformNonStandardReturn(Thread* self,
                              ShadowFrame& frame,
                              JValue& result,
                              const instrumentation::Instrumentation* instrumentation,
                              bool unlock_monitors = true) REQUIRES_SHARED(Locks::mutator_lock_);

// Handles all invoke-XXX/range instructions except for invoke-polymorphic[/range].
// Returns true on success, otherwise throws an exception and returns false.
template<InvokeType type, bool is_range>
static ALWAYS_INLINE bool DoInvoke(Thread* self,
                                   ShadowFrame& shadow_frame,
                                   const Instruction* inst,
                                   uint16_t inst_data,
                                   JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  // Make sure to check for async exceptions before anything else.
  if (UNLIKELY(self->ObserveAsyncException())) {
    return false;
  }
  const uint32_t vregC = is_range ? inst->VRegC_3rc() : inst->VRegC_35c();
  ObjPtr<mirror::Object> obj = type == kStatic ? nullptr : shadow_frame.GetVRegReference(vregC);
  ArtMethod* sf_method = shadow_frame.GetMethod();
  bool string_init = false;
  ArtMethod* called_method = FindMethodToCall<type>(
      self, sf_method, &obj, *inst, /* only_lookup_tls_cache= */ false, &string_init);
  if (called_method == nullptr) {
    DCHECK(self->IsExceptionPending());
    result->SetJ(0);
    return false;
  }

  return DoCall<is_range>(
      called_method, self, shadow_frame, inst, inst_data, string_init, result);
}

static inline ObjPtr<mirror::MethodHandle> ResolveMethodHandle(Thread* self,
                                                               uint32_t method_handle_index,
                                                               ArtMethod* referrer)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  return class_linker->ResolveMethodHandle(self, method_handle_index, referrer);
}

static inline ObjPtr<mirror::MethodType> ResolveMethodType(Thread* self,
                                                           dex::ProtoIndex method_type_index,
                                                           ArtMethod* referrer)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  return class_linker->ResolveMethodType(self, method_type_index, referrer);
}

#define DECLARE_SIGNATURE_POLYMORPHIC_HANDLER(Name, ...)              \
bool Do ## Name(Thread* self,                                         \
                ShadowFrame& shadow_frame,                            \
                const Instruction* inst,                              \
                uint16_t inst_data,                                   \
                JValue* result) REQUIRES_SHARED(Locks::mutator_lock_);
ART_INTRINSICS_LIST(DECLARE_SIGNATURE_POLYMORPHIC_HANDLER)
#undef DECLARE_SIGNATURE_POLYMORPHIC_HANDLER

// Performs a invoke-polymorphic or invoke-polymorphic-range.
template<bool is_range>
EXPORT bool DoInvokePolymorphic(Thread* self,
                                ShadowFrame& shadow_frame,
                                const Instruction* inst,
                                uint16_t inst_data,
                                JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_);

EXPORT bool DoInvokeCustom(Thread* self,
                           ShadowFrame& shadow_frame,
                           uint32_t call_site_idx,
                           const InstructionOperands* operands,
                           JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_);

// Performs a custom invoke (invoke-custom/invoke-custom-range).
template<bool is_range>
bool DoInvokeCustom(Thread* self,
                    ShadowFrame& shadow_frame,
                    const Instruction* inst,
                    uint16_t inst_data,
                    JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  const uint32_t call_site_idx = is_range ? inst->VRegB_3rc() : inst->VRegB_35c();
  if (is_range) {
    RangeInstructionOperands operands(inst->VRegC_3rc(), inst->VRegA_3rc());
    return DoInvokeCustom(self, shadow_frame, call_site_idx, &operands, result);
  } else {
    uint32_t args[Instruction::kMaxVarArgRegs];
    inst->GetVarArgs(args, inst_data);
    VarArgsInstructionOperands operands(args, inst->VRegA_35c());
    return DoInvokeCustom(self, shadow_frame, call_site_idx, &operands, result);
  }
}

template<Primitive::Type field_type>
ALWAYS_INLINE static JValue GetFieldValue(const ShadowFrame& shadow_frame, uint32_t vreg)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  JValue field_value;
  switch (field_type) {
    case Primitive::kPrimBoolean:
      field_value.SetZ(static_cast<uint8_t>(shadow_frame.GetVReg(vreg)));
      break;
    case Primitive::kPrimByte:
      field_value.SetB(static_cast<int8_t>(shadow_frame.GetVReg(vreg)));
      break;
    case Primitive::kPrimChar:
      field_value.SetC(static_cast<uint16_t>(shadow_frame.GetVReg(vreg)));
      break;
    case Primitive::kPrimShort:
      field_value.SetS(static_cast<int16_t>(shadow_frame.GetVReg(vreg)));
      break;
    case Primitive::kPrimInt:
      field_value.SetI(shadow_frame.GetVReg(vreg));
      break;
    case Primitive::kPrimLong:
      field_value.SetJ(shadow_frame.GetVRegLong(vreg));
      break;
    case Primitive::kPrimNot:
      field_value.SetL(shadow_frame.GetVRegReference(vreg));
      break;
    default:
      LOG(FATAL) << "Unreachable: " << field_type;
      UNREACHABLE();
  }
  return field_value;
}

LIBART_PROTECTED
extern "C" size_t NterpGetStaticField(Thread* self,
                                      ArtMethod* caller,
                                      const uint16_t* dex_pc_ptr,
                                      size_t resolve_field_type);

LIBART_PROTECTED
extern "C" uint32_t NterpGetInstanceFieldOffset(Thread* self,
                                                ArtMethod* caller,
                                                const uint16_t* dex_pc_ptr,
                                                size_t resolve_field_type,
                                                uint32_t* registers);

static inline void GetFieldInfo(Thread* self,
                                ShadowFrame& shadow_frame,
                                const uint16_t* dex_pc_ptr,
                                bool is_static,
                                bool resolve_field_type,
                                ArtField** field,
                                bool* is_volatile,
                                MemberOffset* offset)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  size_t tls_value = 0u;
  if (!self->GetInterpreterCache()->Get(self, dex_pc_ptr, &tls_value)) {
    if (is_static) {
      tls_value = NterpGetStaticField(
          self, shadow_frame.GetMethod(), dex_pc_ptr, resolve_field_type);
    } else {
      tls_value = NterpGetInstanceFieldOffset(self,
                                              shadow_frame.GetMethod(),
                                              dex_pc_ptr,
                                              resolve_field_type,
                                              shadow_frame.GetVRegAddr(0));
    }

    if (self->IsExceptionPending()) {
      return;
    }
  }

  if (is_static) {
    DCHECK_NE(tls_value, 0u);
    *is_volatile = ((tls_value & 1) != 0);
    *field = reinterpret_cast<ArtField*>(tls_value & ~static_cast<size_t>(1u));
    *offset = (*field)->GetOffset();
  } else {
    *is_volatile = (static_cast<int32_t>(tls_value) < 0);
    *offset = MemberOffset(std::abs(static_cast<int32_t>(tls_value)));
  }
}

// Handles string resolution for const-string and const-string-jumbo instructions. Also ensures the
// java.lang.String class is initialized.
static inline ObjPtr<mirror::String> ResolveString(Thread* self,
                                                   ShadowFrame& shadow_frame,
                                                   dex::StringIndex string_idx)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ObjPtr<mirror::Class> java_lang_string_class = GetClassRoot<mirror::String>();
  if (UNLIKELY(!java_lang_string_class->IsVisiblyInitialized())) {
    StackHandleScope<1> hs(self);
    Handle<mirror::Class> h_class(hs.NewHandle(java_lang_string_class));
    if (UNLIKELY(!Runtime::Current()->GetClassLinker()->EnsureInitialized(
                      self, h_class, /*can_init_fields=*/ true, /*can_init_parents=*/ true))) {
      DCHECK(self->IsExceptionPending());
      return nullptr;
    }
    DCHECK(h_class->IsInitializing());
  }
  ArtMethod* method = shadow_frame.GetMethod();
  ObjPtr<mirror::String> string_ptr =
      Runtime::Current()->GetClassLinker()->ResolveString(string_idx, method);
  return string_ptr;
}

// Handles div-int, div-int/2addr, div-int/li16 and div-int/lit8 instructions.
// Returns true on success, otherwise throws a java.lang.ArithmeticException and return false.
static inline bool DoIntDivide(ShadowFrame& shadow_frame, size_t result_reg,
                               int32_t dividend, int32_t divisor)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  constexpr int32_t kMinInt = std::numeric_limits<int32_t>::min();
  if (UNLIKELY(divisor == 0)) {
    ThrowArithmeticExceptionDivideByZero();
    return false;
  }
  if (UNLIKELY(dividend == kMinInt && divisor == -1)) {
    shadow_frame.SetVReg(result_reg, kMinInt);
  } else {
    shadow_frame.SetVReg(result_reg, dividend / divisor);
  }
  return true;
}

// Handles rem-int, rem-int/2addr, rem-int/li16 and rem-int/lit8 instructions.
// Returns true on success, otherwise throws a java.lang.ArithmeticException and return false.
static inline bool DoIntRemainder(ShadowFrame& shadow_frame, size_t result_reg,
                                  int32_t dividend, int32_t divisor)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  constexpr int32_t kMinInt = std::numeric_limits<int32_t>::min();
  if (UNLIKELY(divisor == 0)) {
    ThrowArithmeticExceptionDivideByZero();
    return false;
  }
  if (UNLIKELY(dividend == kMinInt && divisor == -1)) {
    shadow_frame.SetVReg(result_reg, 0);
  } else {
    shadow_frame.SetVReg(result_reg, dividend % divisor);
  }
  return true;
}

// Handles div-long and div-long-2addr instructions.
// Returns true on success, otherwise throws a java.lang.ArithmeticException and return false.
static inline bool DoLongDivide(ShadowFrame& shadow_frame,
                                size_t result_reg,
                                int64_t dividend,
                                int64_t divisor)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  const int64_t kMinLong = std::numeric_limits<int64_t>::min();
  if (UNLIKELY(divisor == 0)) {
    ThrowArithmeticExceptionDivideByZero();
    return false;
  }
  if (UNLIKELY(dividend == kMinLong && divisor == -1)) {
    shadow_frame.SetVRegLong(result_reg, kMinLong);
  } else {
    shadow_frame.SetVRegLong(result_reg, dividend / divisor);
  }
  return true;
}

// Handles rem-long and rem-long-2addr instructions.
// Returns true on success, otherwise throws a java.lang.ArithmeticException and return false.
static inline bool DoLongRemainder(ShadowFrame& shadow_frame,
                                   size_t result_reg,
                                   int64_t dividend,
                                   int64_t divisor)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  const int64_t kMinLong = std::numeric_limits<int64_t>::min();
  if (UNLIKELY(divisor == 0)) {
    ThrowArithmeticExceptionDivideByZero();
    return false;
  }
  if (UNLIKELY(dividend == kMinLong && divisor == -1)) {
    shadow_frame.SetVRegLong(result_reg, 0);
  } else {
    shadow_frame.SetVRegLong(result_reg, dividend % divisor);
  }
  return true;
}

// Handles filled-new-array and filled-new-array-range instructions.
// Returns true on success, otherwise throws an exception and returns false.
template <bool is_range>
EXPORT bool DoFilledNewArray(const Instruction* inst,
                             const ShadowFrame& shadow_frame,
                             Thread* self,
                             JValue* result);

// Handles packed-switch instruction.
// Returns the branch offset to the next instruction to execute.
static inline int32_t DoPackedSwitch(const Instruction* inst,
                                     const ShadowFrame& shadow_frame,
                                     uint16_t inst_data)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  DCHECK(inst->Opcode() == Instruction::PACKED_SWITCH);
  const uint16_t* switch_data = reinterpret_cast<const uint16_t*>(inst) + inst->VRegB_31t();
  int32_t test_val = shadow_frame.GetVReg(inst->VRegA_31t(inst_data));
  DCHECK_EQ(switch_data[0], static_cast<uint16_t>(Instruction::kPackedSwitchSignature));
  uint16_t size = switch_data[1];
  if (size == 0) {
    // Empty packed switch, move forward by 3 (size of PACKED_SWITCH).
    return 3;
  }
  const int32_t* keys = reinterpret_cast<const int32_t*>(&switch_data[2]);
  DCHECK_ALIGNED(keys, 4);
  int32_t first_key = keys[0];
  const int32_t* targets = reinterpret_cast<const int32_t*>(&switch_data[4]);
  DCHECK_ALIGNED(targets, 4);
  int32_t index = test_val - first_key;
  if (index >= 0 && index < size) {
    return targets[index];
  } else {
    // No corresponding value: move forward by 3 (size of PACKED_SWITCH).
    return 3;
  }
}

// Handles sparse-switch instruction.
// Returns the branch offset to the next instruction to execute.
static inline int32_t DoSparseSwitch(const Instruction* inst, const ShadowFrame& shadow_frame,
                                     uint16_t inst_data)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  DCHECK(inst->Opcode() == Instruction::SPARSE_SWITCH);
  const uint16_t* switch_data = reinterpret_cast<const uint16_t*>(inst) + inst->VRegB_31t();
  int32_t test_val = shadow_frame.GetVReg(inst->VRegA_31t(inst_data));
  DCHECK_EQ(switch_data[0], static_cast<uint16_t>(Instruction::kSparseSwitchSignature));
  uint16_t size = switch_data[1];
  // Return length of SPARSE_SWITCH if size is 0.
  if (size == 0) {
    return 3;
  }
  const int32_t* keys = reinterpret_cast<const int32_t*>(&switch_data[2]);
  DCHECK_ALIGNED(keys, 4);
  const int32_t* entries = keys + size;
  DCHECK_ALIGNED(entries, 4);
  int lo = 0;
  int hi = size - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    int32_t foundVal = keys[mid];
    if (test_val < foundVal) {
      hi = mid - 1;
    } else if (test_val > foundVal) {
      lo = mid + 1;
    } else {
      return entries[mid];
    }
  }
  // No corresponding value: move forward by 3 (size of SPARSE_SWITCH).
  return 3;
}

// We execute any instrumentation events triggered by throwing and/or handing the pending exception
// and change the shadow_frames dex_pc to the appropriate exception handler if the current method
// has one. If the exception has been handled and the shadow_frame is now pointing to a catch clause
// we return true. If the current method is unable to handle the exception we return false.
// This function accepts a null Instrumentation* as a way to cause instrumentation events not to be
// reported.
// TODO We might wish to reconsider how we cause some events to be ignored.
EXPORT bool MoveToExceptionHandler(Thread* self,
                                   ShadowFrame& shadow_frame,
                                   bool skip_listeners,
                                   bool skip_throw_listener) REQUIRES_SHARED(Locks::mutator_lock_);

NO_RETURN EXPORT void UnexpectedOpcode(const Instruction* inst, const ShadowFrame& shadow_frame)
    COLD_ATTR
    REQUIRES_SHARED(Locks::mutator_lock_);

// Set true if you want TraceExecution invocation before each bytecode execution.
constexpr bool kTraceExecutionEnabled = false;

static inline void TraceExecution(const ShadowFrame& shadow_frame, const Instruction* inst,
                                  const uint32_t dex_pc)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (kTraceExecutionEnabled) {
#define TRACE_LOG std::cerr
    std::ostringstream oss;
    oss << shadow_frame.GetMethod()->PrettyMethod()
        << android::base::StringPrintf("\n0x%x: ", dex_pc)
        << inst->DumpString(shadow_frame.GetMethod()->GetDexFile()) << "\n";
    for (uint32_t i = 0; i < shadow_frame.NumberOfVRegs(); ++i) {
      uint32_t raw_value = shadow_frame.GetVReg(i);
      ObjPtr<mirror::Object> ref_value = shadow_frame.GetVRegReference(i);
      oss << android::base::StringPrintf(" vreg%u=0x%08X", i, raw_value);
      if (ref_value != nullptr) {
        if (ref_value->GetClass()->IsStringClass() &&
            !ref_value->AsString()->IsValueNull()) {
          oss << "/java.lang.String \"" << ref_value->AsString()->ToModifiedUtf8() << "\"";
        } else {
          oss << "/" << ref_value->PrettyTypeOf();
        }
      }
    }
    TRACE_LOG << oss.str() << "\n";
#undef TRACE_LOG
  }
}

static inline bool IsBackwardBranch(int32_t branch_offset) {
  return branch_offset <= 0;
}

// The arg_offset is the offset to the first input register in the frame.
void ArtInterpreterToCompiledCodeBridge(Thread* self,
                                        ArtMethod* caller,
                                        ShadowFrame* shadow_frame,
                                        uint16_t arg_offset,
                                        JValue* result);

// Set string value created from StringFactory.newStringFromXXX() into all aliases of
// StringFactory.newEmptyString().
void SetStringInitValueToAllAliases(ShadowFrame* shadow_frame,
                                    uint16_t this_obj_vreg,
                                    JValue result);

}  // namespace interpreter
}  // namespace art

#endif  // ART_RUNTIME_INTERPRETER_INTERPRETER_COMMON_H_
