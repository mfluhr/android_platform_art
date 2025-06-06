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

#include "code_generator_x86_64.h"

#include "arch/x86_64/jni_frame_x86_64.h"
#include "art_method-inl.h"
#include "class_root-inl.h"
#include "class_table.h"
#include "code_generator_utils.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "gc/accounting/card_table.h"
#include "gc/space/image_space.h"
#include "heap_poisoning.h"
#include "interpreter/mterp/nterp.h"
#include "intrinsics.h"
#include "intrinsics_list.h"
#include "intrinsics_utils.h"
#include "intrinsics_x86_64.h"
#include "jit/profiling_info.h"
#include "linker/linker_patch.h"
#include "lock_word.h"
#include "mirror/array-inl.h"
#include "mirror/class-inl.h"
#include "mirror/method_type.h"
#include "mirror/object_reference.h"
#include "mirror/var_handle.h"
#include "optimizing/nodes.h"
#include "profiling_info_builder.h"
#include "scoped_thread_state_change-inl.h"
#include "thread.h"
#include "trace.h"
#include "utils/assembler.h"
#include "utils/stack_checks.h"
#include "utils/x86_64/assembler_x86_64.h"
#include "utils/x86_64/constants_x86_64.h"
#include "utils/x86_64/managed_register_x86_64.h"

namespace art HIDDEN {

template<class MirrorType>
class GcRoot;

namespace x86_64 {

static constexpr int kCurrentMethodStackOffset = 0;
// The compare/jump sequence will generate about (1.5 * num_entries) instructions. A jump
// table version generates 7 instructions and num_entries literals. Compare/jump sequence will
// generates less code/data with a small num_entries.
static constexpr uint32_t kPackedSwitchJumpTableThreshold = 5;

static constexpr Register kCoreCalleeSaves[] = { RBX, RBP, R12, R13, R14, R15 };
static constexpr FloatRegister kFpuCalleeSaves[] = { XMM12, XMM13, XMM14, XMM15 };

static constexpr int kC2ConditionMask = 0x400;

static RegisterSet OneRegInReferenceOutSaveEverythingCallerSaves() {
  // Custom calling convention: RAX serves as both input and output.
  RegisterSet caller_saves = RegisterSet::Empty();
  caller_saves.Add(Location::RegisterLocation(RAX));
  return caller_saves;
}

// NOLINT on __ macro to suppress wrong warning/fix (misc-macro-parentheses) from clang-tidy.
#define __ down_cast<X86_64Assembler*>(codegen->GetAssembler())->  // NOLINT
#define QUICK_ENTRY_POINT(x) QUICK_ENTRYPOINT_OFFSET(kX86_64PointerSize, x).Int32Value()

class NullCheckSlowPathX86_64 : public SlowPathCode {
 public:
  explicit NullCheckSlowPathX86_64(HNullCheck* instruction) : SlowPathCode(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) override {
    CodeGeneratorX86_64* x86_64_codegen = down_cast<CodeGeneratorX86_64*>(codegen);
    __ Bind(GetEntryLabel());
    if (instruction_->CanThrowIntoCatchBlock()) {
      // Live registers will be restored in the catch block if caught.
      SaveLiveRegisters(codegen, instruction_->GetLocations());
    }
    x86_64_codegen->InvokeRuntime(kQuickThrowNullPointer, instruction_, this);
    CheckEntrypointTypes<kQuickThrowNullPointer, void, void>();
  }

  bool IsFatal() const override { return true; }

  const char* GetDescription() const override { return "NullCheckSlowPathX86_64"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(NullCheckSlowPathX86_64);
};

class DivZeroCheckSlowPathX86_64 : public SlowPathCode {
 public:
  explicit DivZeroCheckSlowPathX86_64(HDivZeroCheck* instruction) : SlowPathCode(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) override {
    CodeGeneratorX86_64* x86_64_codegen = down_cast<CodeGeneratorX86_64*>(codegen);
    __ Bind(GetEntryLabel());
    x86_64_codegen->InvokeRuntime(kQuickThrowDivZero, instruction_, this);
    CheckEntrypointTypes<kQuickThrowDivZero, void, void>();
  }

  bool IsFatal() const override { return true; }

  const char* GetDescription() const override { return "DivZeroCheckSlowPathX86_64"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(DivZeroCheckSlowPathX86_64);
};

class DivRemMinusOneSlowPathX86_64 : public SlowPathCode {
 public:
  DivRemMinusOneSlowPathX86_64(HInstruction* at, Register reg, DataType::Type type, bool is_div)
      : SlowPathCode(at), cpu_reg_(CpuRegister(reg)), type_(type), is_div_(is_div) {}

  void EmitNativeCode(CodeGenerator* codegen) override {
    __ Bind(GetEntryLabel());
    if (type_ == DataType::Type::kInt32) {
      if (is_div_) {
        __ negl(cpu_reg_);
      } else {
        __ xorl(cpu_reg_, cpu_reg_);
      }

    } else {
      DCHECK_EQ(DataType::Type::kInt64, type_);
      if (is_div_) {
        __ negq(cpu_reg_);
      } else {
        __ xorl(cpu_reg_, cpu_reg_);
      }
    }
    __ jmp(GetExitLabel());
  }

  const char* GetDescription() const override { return "DivRemMinusOneSlowPathX86_64"; }

 private:
  const CpuRegister cpu_reg_;
  const DataType::Type type_;
  const bool is_div_;
  DISALLOW_COPY_AND_ASSIGN(DivRemMinusOneSlowPathX86_64);
};

class SuspendCheckSlowPathX86_64 : public SlowPathCode {
 public:
  SuspendCheckSlowPathX86_64(HSuspendCheck* instruction, HBasicBlock* successor)
      : SlowPathCode(instruction), successor_(successor) {}

  void EmitNativeCode(CodeGenerator* codegen) override {
    LocationSummary* locations = instruction_->GetLocations();
    CodeGeneratorX86_64* x86_64_codegen = down_cast<CodeGeneratorX86_64*>(codegen);
    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);  // Only saves full width XMM for SIMD.
    x86_64_codegen->InvokeRuntime(kQuickTestSuspend, instruction_, this);
    CheckEntrypointTypes<kQuickTestSuspend, void, void>();
    RestoreLiveRegisters(codegen, locations);  // Only restores full width XMM for SIMD.
    if (successor_ == nullptr) {
      __ jmp(GetReturnLabel());
    } else {
      __ jmp(x86_64_codegen->GetLabelOf(successor_));
    }
  }

  Label* GetReturnLabel() {
    DCHECK(successor_ == nullptr);
    return &return_label_;
  }

  HBasicBlock* GetSuccessor() const {
    return successor_;
  }

  const char* GetDescription() const override { return "SuspendCheckSlowPathX86_64"; }

 private:
  HBasicBlock* const successor_;
  Label return_label_;

  DISALLOW_COPY_AND_ASSIGN(SuspendCheckSlowPathX86_64);
};

class BoundsCheckSlowPathX86_64 : public SlowPathCode {
 public:
  explicit BoundsCheckSlowPathX86_64(HBoundsCheck* instruction)
    : SlowPathCode(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) override {
    LocationSummary* locations = instruction_->GetLocations();
    CodeGeneratorX86_64* x86_64_codegen = down_cast<CodeGeneratorX86_64*>(codegen);
    __ Bind(GetEntryLabel());
    if (instruction_->CanThrowIntoCatchBlock()) {
      // Live registers will be restored in the catch block if caught.
      SaveLiveRegisters(codegen, locations);
    }

    Location index_loc = locations->InAt(0);
    Location length_loc = locations->InAt(1);
    InvokeRuntimeCallingConvention calling_convention;
    Location index_arg = Location::RegisterLocation(calling_convention.GetRegisterAt(0));
    Location length_arg = Location::RegisterLocation(calling_convention.GetRegisterAt(1));

    // Are we using an array length from memory?
    if (!length_loc.IsValid()) {
      DCHECK(instruction_->InputAt(1)->IsArrayLength());
      HArrayLength* array_length = instruction_->InputAt(1)->AsArrayLength();
      DCHECK(array_length->IsEmittedAtUseSite());
      uint32_t len_offset = CodeGenerator::GetArrayLengthOffset(array_length);
      Location array_loc = array_length->GetLocations()->InAt(0);
      Address array_len(array_loc.AsRegister<CpuRegister>(), len_offset);
      if (!index_loc.Equals(length_arg)) {
        // The index is not clobbered by loading the length directly to `length_arg`.
        __ movl(length_arg.AsRegister<CpuRegister>(), array_len);
        x86_64_codegen->Move(index_arg, index_loc);
      } else if (!array_loc.Equals(index_arg)) {
        // The array reference is not clobbered by the index move.
        x86_64_codegen->Move(index_arg, index_loc);
        __ movl(length_arg.AsRegister<CpuRegister>(), array_len);
      } else {
        // Load the array length into `TMP`.
        DCHECK(codegen->IsBlockedCoreRegister(TMP));
        __ movl(CpuRegister(TMP), array_len);
        // Single move to CPU register does not clobber `TMP`.
        x86_64_codegen->Move(index_arg, index_loc);
        __ movl(length_arg.AsRegister<CpuRegister>(), CpuRegister(TMP));
      }
      if (mirror::kUseStringCompression && array_length->IsStringLength()) {
        __ shrl(length_arg.AsRegister<CpuRegister>(), Immediate(1));
      }
    } else {
      // We're moving two locations to locations that could overlap,
      // so we need a parallel move resolver.
      codegen->EmitParallelMoves(
          index_loc,
          index_arg,
          DataType::Type::kInt32,
          length_loc,
          length_arg,
          DataType::Type::kInt32);
    }

    QuickEntrypointEnum entrypoint = instruction_->AsBoundsCheck()->IsStringCharAt()
        ? kQuickThrowStringBounds
        : kQuickThrowArrayBounds;
    x86_64_codegen->InvokeRuntime(entrypoint, instruction_, this);
    CheckEntrypointTypes<kQuickThrowStringBounds, void, int32_t, int32_t>();
    CheckEntrypointTypes<kQuickThrowArrayBounds, void, int32_t, int32_t>();
  }

  bool IsFatal() const override { return true; }

  const char* GetDescription() const override { return "BoundsCheckSlowPathX86_64"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(BoundsCheckSlowPathX86_64);
};

class LoadMethodTypeSlowPathX86_64: public SlowPathCode {
 public:
  explicit LoadMethodTypeSlowPathX86_64(HLoadMethodType* mt) : SlowPathCode(mt) {}

  void EmitNativeCode(CodeGenerator* codegen) override {
    LocationSummary* locations = instruction_->GetLocations();
    DCHECK(!locations->GetLiveRegisters()->ContainsCoreRegister(locations->Out().reg()));

    CodeGeneratorX86_64* x86_64_codegen = down_cast<CodeGeneratorX86_64*>(codegen);
    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    const dex::ProtoIndex proto_index = instruction_->AsLoadMethodType()->GetProtoIndex();
    // Custom calling convention: RAX serves as both input and output.
    __ movl(CpuRegister(RAX), Immediate(proto_index.index_));
    x86_64_codegen->InvokeRuntime(kQuickResolveMethodType, instruction_, this);
    CheckEntrypointTypes<kQuickResolveMethodType, void*, uint32_t>();
    x86_64_codegen->Move(locations->Out(), Location::RegisterLocation(RAX));
    RestoreLiveRegisters(codegen, locations);

    __ jmp(GetExitLabel());
  }

  const char* GetDescription() const override { return "LoadMethodTypeSlowPathX86_64"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(LoadMethodTypeSlowPathX86_64);
};

class LoadClassSlowPathX86_64 : public SlowPathCode {
 public:
  LoadClassSlowPathX86_64(HLoadClass* cls, HInstruction* at)
      : SlowPathCode(at), cls_(cls) {
    DCHECK(at->IsLoadClass() || at->IsClinitCheck());
    DCHECK_EQ(instruction_->IsLoadClass(), cls_ == instruction_);
  }

  void EmitNativeCode(CodeGenerator* codegen) override {
    LocationSummary* locations = instruction_->GetLocations();
    Location out = locations->Out();
    bool must_resolve_type = instruction_->IsLoadClass() && cls_->MustResolveTypeOnSlowPath();
    bool must_do_clinit = instruction_->IsClinitCheck() || cls_->MustGenerateClinitCheck();

    CodeGeneratorX86_64* x86_64_codegen = down_cast<CodeGeneratorX86_64*>(codegen);
    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    // Custom calling convention: RAX serves as both input and output.
    if (must_resolve_type) {
      DCHECK(IsSameDexFile(cls_->GetDexFile(), x86_64_codegen->GetGraph()->GetDexFile()) ||
             x86_64_codegen->GetCompilerOptions().WithinOatFile(&cls_->GetDexFile()) ||
             ContainsElement(Runtime::Current()->GetClassLinker()->GetBootClassPath(),
                             &cls_->GetDexFile()));
      dex::TypeIndex type_index = cls_->GetTypeIndex();
      __ movl(CpuRegister(RAX), Immediate(type_index.index_));
      if (cls_->NeedsAccessCheck()) {
        CheckEntrypointTypes<kQuickResolveTypeAndVerifyAccess, void*, uint32_t>();
        x86_64_codegen->InvokeRuntime(kQuickResolveTypeAndVerifyAccess, instruction_, this);
      } else {
        CheckEntrypointTypes<kQuickResolveType, void*, uint32_t>();
        x86_64_codegen->InvokeRuntime(kQuickResolveType, instruction_, this);
      }
      // If we also must_do_clinit, the resolved type is now in the correct register.
    } else {
      DCHECK(must_do_clinit);
      Location source = instruction_->IsLoadClass() ? out : locations->InAt(0);
      x86_64_codegen->Move(Location::RegisterLocation(RAX), source);
    }
    if (must_do_clinit) {
      x86_64_codegen->InvokeRuntime(kQuickInitializeStaticStorage, instruction_, this);
      CheckEntrypointTypes<kQuickInitializeStaticStorage, void*, mirror::Class*>();
    }

    // Move the class to the desired location.
    if (out.IsValid()) {
      DCHECK(out.IsRegister() && !locations->GetLiveRegisters()->ContainsCoreRegister(out.reg()));
      x86_64_codegen->Move(out, Location::RegisterLocation(RAX));
    }

    RestoreLiveRegisters(codegen, locations);
    __ jmp(GetExitLabel());
  }

  const char* GetDescription() const override { return "LoadClassSlowPathX86_64"; }

 private:
  // The class this slow path will load.
  HLoadClass* const cls_;

  DISALLOW_COPY_AND_ASSIGN(LoadClassSlowPathX86_64);
};

class LoadStringSlowPathX86_64 : public SlowPathCode {
 public:
  explicit LoadStringSlowPathX86_64(HLoadString* instruction) : SlowPathCode(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) override {
    LocationSummary* locations = instruction_->GetLocations();
    DCHECK(!locations->GetLiveRegisters()->ContainsCoreRegister(locations->Out().reg()));

    CodeGeneratorX86_64* x86_64_codegen = down_cast<CodeGeneratorX86_64*>(codegen);
    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    const dex::StringIndex string_index = instruction_->AsLoadString()->GetStringIndex();
    // Custom calling convention: RAX serves as both input and output.
    __ movl(CpuRegister(RAX), Immediate(string_index.index_));
    x86_64_codegen->InvokeRuntime(kQuickResolveString, instruction_, this);
    CheckEntrypointTypes<kQuickResolveString, void*, uint32_t>();
    x86_64_codegen->Move(locations->Out(), Location::RegisterLocation(RAX));
    RestoreLiveRegisters(codegen, locations);

    __ jmp(GetExitLabel());
  }

  const char* GetDescription() const override { return "LoadStringSlowPathX86_64"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(LoadStringSlowPathX86_64);
};

class TypeCheckSlowPathX86_64 : public SlowPathCode {
 public:
  TypeCheckSlowPathX86_64(HInstruction* instruction, bool is_fatal)
      : SlowPathCode(instruction), is_fatal_(is_fatal) {}

  void EmitNativeCode(CodeGenerator* codegen) override {
    LocationSummary* locations = instruction_->GetLocations();
    DCHECK(instruction_->IsCheckCast()
           || !locations->GetLiveRegisters()->ContainsCoreRegister(locations->Out().reg()));

    CodeGeneratorX86_64* x86_64_codegen = down_cast<CodeGeneratorX86_64*>(codegen);
    __ Bind(GetEntryLabel());

    if (kPoisonHeapReferences &&
        instruction_->IsCheckCast() &&
        instruction_->AsCheckCast()->GetTypeCheckKind() == TypeCheckKind::kInterfaceCheck) {
      // First, unpoison the `cls` reference that was poisoned for direct memory comparison.
      __ UnpoisonHeapReference(locations->InAt(1).AsRegister<CpuRegister>());
    }

    if (!is_fatal_ || instruction_->CanThrowIntoCatchBlock()) {
      SaveLiveRegisters(codegen, locations);
    }

    // We're moving two locations to locations that could overlap, so we need a parallel
    // move resolver.
    InvokeRuntimeCallingConvention calling_convention;
    codegen->EmitParallelMoves(locations->InAt(0),
                               Location::RegisterLocation(calling_convention.GetRegisterAt(0)),
                               DataType::Type::kReference,
                               locations->InAt(1),
                               Location::RegisterLocation(calling_convention.GetRegisterAt(1)),
                               DataType::Type::kReference);
    if (instruction_->IsInstanceOf()) {
      x86_64_codegen->InvokeRuntime(kQuickInstanceofNonTrivial, instruction_, this);
      CheckEntrypointTypes<kQuickInstanceofNonTrivial, size_t, mirror::Object*, mirror::Class*>();
    } else {
      DCHECK(instruction_->IsCheckCast());
      x86_64_codegen->InvokeRuntime(kQuickCheckInstanceOf, instruction_, this);
      CheckEntrypointTypes<kQuickCheckInstanceOf, void, mirror::Object*, mirror::Class*>();
    }

    if (!is_fatal_) {
      if (instruction_->IsInstanceOf()) {
        x86_64_codegen->Move(locations->Out(), Location::RegisterLocation(RAX));
      }

      RestoreLiveRegisters(codegen, locations);
      __ jmp(GetExitLabel());
    }
  }

  const char* GetDescription() const override { return "TypeCheckSlowPathX86_64"; }

  bool IsFatal() const override { return is_fatal_; }

 private:
  const bool is_fatal_;

  DISALLOW_COPY_AND_ASSIGN(TypeCheckSlowPathX86_64);
};

class DeoptimizationSlowPathX86_64 : public SlowPathCode {
 public:
  explicit DeoptimizationSlowPathX86_64(HDeoptimize* instruction)
      : SlowPathCode(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) override {
    CodeGeneratorX86_64* x86_64_codegen = down_cast<CodeGeneratorX86_64*>(codegen);
    __ Bind(GetEntryLabel());
    LocationSummary* locations = instruction_->GetLocations();
    SaveLiveRegisters(codegen, locations);
    InvokeRuntimeCallingConvention calling_convention;
    x86_64_codegen->Load32BitValue(
        CpuRegister(calling_convention.GetRegisterAt(0)),
        static_cast<uint32_t>(instruction_->AsDeoptimize()->GetDeoptimizationKind()));
    x86_64_codegen->InvokeRuntime(kQuickDeoptimize, instruction_, this);
    CheckEntrypointTypes<kQuickDeoptimize, void, DeoptimizationKind>();
  }

  const char* GetDescription() const override { return "DeoptimizationSlowPathX86_64"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(DeoptimizationSlowPathX86_64);
};

class ArraySetSlowPathX86_64 : public SlowPathCode {
 public:
  explicit ArraySetSlowPathX86_64(HInstruction* instruction) : SlowPathCode(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) override {
    LocationSummary* locations = instruction_->GetLocations();
    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    InvokeRuntimeCallingConvention calling_convention;
    HParallelMove parallel_move(codegen->GetGraph()->GetAllocator());
    parallel_move.AddMove(
        locations->InAt(0),
        Location::RegisterLocation(calling_convention.GetRegisterAt(0)),
        DataType::Type::kReference,
        nullptr);
    parallel_move.AddMove(
        locations->InAt(1),
        Location::RegisterLocation(calling_convention.GetRegisterAt(1)),
        DataType::Type::kInt32,
        nullptr);
    parallel_move.AddMove(
        locations->InAt(2),
        Location::RegisterLocation(calling_convention.GetRegisterAt(2)),
        DataType::Type::kReference,
        nullptr);
    codegen->GetMoveResolver()->EmitNativeCode(&parallel_move);

    CodeGeneratorX86_64* x86_64_codegen = down_cast<CodeGeneratorX86_64*>(codegen);
    x86_64_codegen->InvokeRuntime(kQuickAputObject, instruction_, this);
    CheckEntrypointTypes<kQuickAputObject, void, mirror::Array*, int32_t, mirror::Object*>();
    RestoreLiveRegisters(codegen, locations);
    __ jmp(GetExitLabel());
  }

  const char* GetDescription() const override { return "ArraySetSlowPathX86_64"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(ArraySetSlowPathX86_64);
};

// Slow path marking an object reference `ref` during a read
// barrier. The field `obj.field` in the object `obj` holding this
// reference does not get updated by this slow path after marking (see
// ReadBarrierMarkAndUpdateFieldSlowPathX86_64 below for that).
//
// This means that after the execution of this slow path, `ref` will
// always be up-to-date, but `obj.field` may not; i.e., after the
// flip, `ref` will be a to-space reference, but `obj.field` will
// probably still be a from-space reference (unless it gets updated by
// another thread, or if another thread installed another object
// reference (different from `ref`) in `obj.field`).
class ReadBarrierMarkSlowPathX86_64 : public SlowPathCode {
 public:
  ReadBarrierMarkSlowPathX86_64(HInstruction* instruction,
                                Location ref,
                                bool unpoison_ref_before_marking)
      : SlowPathCode(instruction),
        ref_(ref),
        unpoison_ref_before_marking_(unpoison_ref_before_marking) {
  }

  const char* GetDescription() const override { return "ReadBarrierMarkSlowPathX86_64"; }

  void EmitNativeCode(CodeGenerator* codegen) override {
    DCHECK(codegen->EmitReadBarrier());
    LocationSummary* locations = instruction_->GetLocations();
    CpuRegister ref_cpu_reg = ref_.AsRegister<CpuRegister>();
    Register ref_reg = ref_cpu_reg.AsRegister();
    DCHECK(locations->CanCall());
    DCHECK(!locations->GetLiveRegisters()->ContainsCoreRegister(ref_reg)) << ref_reg;
    DCHECK(instruction_->IsInstanceFieldGet() ||
           instruction_->IsStaticFieldGet() ||
           instruction_->IsArrayGet() ||
           instruction_->IsArraySet() ||
           instruction_->IsLoadClass() ||
           instruction_->IsLoadMethodType() ||
           instruction_->IsLoadString() ||
           instruction_->IsInstanceOf() ||
           instruction_->IsCheckCast() ||
           (instruction_->IsInvoke() && instruction_->GetLocations()->Intrinsified()))
        << "Unexpected instruction in read barrier marking slow path: "
        << instruction_->DebugName();

    __ Bind(GetEntryLabel());
    if (unpoison_ref_before_marking_) {
      // Object* ref = ref_addr->AsMirrorPtr()
      __ MaybeUnpoisonHeapReference(ref_cpu_reg);
    }
    // No need to save live registers; it's taken care of by the
    // entrypoint. Also, there is no need to update the stack mask,
    // as this runtime call will not trigger a garbage collection.
    CodeGeneratorX86_64* x86_64_codegen = down_cast<CodeGeneratorX86_64*>(codegen);
    DCHECK_NE(ref_reg, RSP);
    DCHECK(0 <= ref_reg && ref_reg < kNumberOfCpuRegisters) << ref_reg;
    // "Compact" slow path, saving two moves.
    //
    // Instead of using the standard runtime calling convention (input
    // and output in R0):
    //
    //   RDI <- ref
    //   RAX <- ReadBarrierMark(RDI)
    //   ref <- RAX
    //
    // we just use rX (the register containing `ref`) as input and output
    // of a dedicated entrypoint:
    //
    //   rX <- ReadBarrierMarkRegX(rX)
    //
    int32_t entry_point_offset =
        Thread::ReadBarrierMarkEntryPointsOffset<kX86_64PointerSize>(ref_reg);
    // This runtime call does not require a stack map.
    x86_64_codegen->InvokeRuntimeWithoutRecordingPcInfo(entry_point_offset, instruction_, this);
    __ jmp(GetExitLabel());
  }

 private:
  // The location (register) of the marked object reference.
  const Location ref_;
  // Should the reference in `ref_` be unpoisoned prior to marking it?
  const bool unpoison_ref_before_marking_;

  DISALLOW_COPY_AND_ASSIGN(ReadBarrierMarkSlowPathX86_64);
};

// Slow path marking an object reference `ref` during a read barrier,
// and if needed, atomically updating the field `obj.field` in the
// object `obj` holding this reference after marking (contrary to
// ReadBarrierMarkSlowPathX86_64 above, which never tries to update
// `obj.field`).
//
// This means that after the execution of this slow path, both `ref`
// and `obj.field` will be up-to-date; i.e., after the flip, both will
// hold the same to-space reference (unless another thread installed
// another object reference (different from `ref`) in `obj.field`).
class ReadBarrierMarkAndUpdateFieldSlowPathX86_64 : public SlowPathCode {
 public:
  ReadBarrierMarkAndUpdateFieldSlowPathX86_64(HInstruction* instruction,
                                              Location ref,
                                              CpuRegister obj,
                                              const Address& field_addr,
                                              bool unpoison_ref_before_marking,
                                              CpuRegister temp1,
                                              CpuRegister temp2)
      : SlowPathCode(instruction),
        ref_(ref),
        obj_(obj),
        field_addr_(field_addr),
        unpoison_ref_before_marking_(unpoison_ref_before_marking),
        temp1_(temp1),
        temp2_(temp2) {
  }

  const char* GetDescription() const override {
    return "ReadBarrierMarkAndUpdateFieldSlowPathX86_64";
  }

  void EmitNativeCode(CodeGenerator* codegen) override {
    DCHECK(codegen->EmitReadBarrier());
    LocationSummary* locations = instruction_->GetLocations();
    CpuRegister ref_cpu_reg = ref_.AsRegister<CpuRegister>();
    Register ref_reg = ref_cpu_reg.AsRegister();
    DCHECK(locations->CanCall());
    DCHECK(!locations->GetLiveRegisters()->ContainsCoreRegister(ref_reg)) << ref_reg;
    DCHECK((instruction_->IsInvoke() && instruction_->GetLocations()->Intrinsified()))
        << "Unexpected instruction in read barrier marking and field updating slow path: "
        << instruction_->DebugName();
    HInvoke* invoke = instruction_->AsInvoke();
    DCHECK(IsUnsafeCASReference(invoke) ||
           IsUnsafeGetAndSetReference(invoke) ||
           IsVarHandleCASFamily(invoke)) << invoke->GetIntrinsic();

    __ Bind(GetEntryLabel());
    if (unpoison_ref_before_marking_) {
      // Object* ref = ref_addr->AsMirrorPtr()
      __ MaybeUnpoisonHeapReference(ref_cpu_reg);
    }

    // Save the old (unpoisoned) reference.
    __ movl(temp1_, ref_cpu_reg);

    // No need to save live registers; it's taken care of by the
    // entrypoint. Also, there is no need to update the stack mask,
    // as this runtime call will not trigger a garbage collection.
    CodeGeneratorX86_64* x86_64_codegen = down_cast<CodeGeneratorX86_64*>(codegen);
    DCHECK_NE(ref_reg, RSP);
    DCHECK(0 <= ref_reg && ref_reg < kNumberOfCpuRegisters) << ref_reg;
    // "Compact" slow path, saving two moves.
    //
    // Instead of using the standard runtime calling convention (input
    // and output in R0):
    //
    //   RDI <- ref
    //   RAX <- ReadBarrierMark(RDI)
    //   ref <- RAX
    //
    // we just use rX (the register containing `ref`) as input and output
    // of a dedicated entrypoint:
    //
    //   rX <- ReadBarrierMarkRegX(rX)
    //
    int32_t entry_point_offset =
        Thread::ReadBarrierMarkEntryPointsOffset<kX86_64PointerSize>(ref_reg);
    // This runtime call does not require a stack map.
    x86_64_codegen->InvokeRuntimeWithoutRecordingPcInfo(entry_point_offset, instruction_, this);

    // If the new reference is different from the old reference,
    // update the field in the holder (`*field_addr`).
    //
    // Note that this field could also hold a different object, if
    // another thread had concurrently changed it. In that case, the
    // LOCK CMPXCHGL instruction in the compare-and-set (CAS)
    // operation below would abort the CAS, leaving the field as-is.
    NearLabel done;
    __ cmpl(temp1_, ref_cpu_reg);
    __ j(kEqual, &done);

    // Update the holder's field atomically.  This may fail if
    // mutator updates before us, but it's OK.  This is achived
    // using a strong compare-and-set (CAS) operation with relaxed
    // memory synchronization ordering, where the expected value is
    // the old reference and the desired value is the new reference.
    // This operation is implemented with a 32-bit LOCK CMPXLCHG
    // instruction, which requires the expected value (the old
    // reference) to be in EAX.  Save RAX beforehand, and move the
    // expected value (stored in `temp1_`) into EAX.
    __ movq(temp2_, CpuRegister(RAX));
    __ movl(CpuRegister(RAX), temp1_);

    // Convenience aliases.
    CpuRegister base = obj_;
    CpuRegister expected = CpuRegister(RAX);
    CpuRegister value = ref_cpu_reg;

    bool base_equals_value = (base.AsRegister() == value.AsRegister());
    Register value_reg = ref_reg;
    if (kPoisonHeapReferences) {
      if (base_equals_value) {
        // If `base` and `value` are the same register location, move
        // `value_reg` to a temporary register.  This way, poisoning
        // `value_reg` won't invalidate `base`.
        value_reg = temp1_.AsRegister();
        __ movl(CpuRegister(value_reg), base);
      }

      // Check that the register allocator did not assign the location
      // of `expected` (RAX) to `value` nor to `base`, so that heap
      // poisoning (when enabled) works as intended below.
      // - If `value` were equal to `expected`, both references would
      //   be poisoned twice, meaning they would not be poisoned at
      //   all, as heap poisoning uses address negation.
      // - If `base` were equal to `expected`, poisoning `expected`
      //   would invalidate `base`.
      DCHECK_NE(value_reg, expected.AsRegister());
      DCHECK_NE(base.AsRegister(), expected.AsRegister());

      __ PoisonHeapReference(expected);
      __ PoisonHeapReference(CpuRegister(value_reg));
    }

    __ LockCmpxchgl(field_addr_, CpuRegister(value_reg));

    // If heap poisoning is enabled, we need to unpoison the values
    // that were poisoned earlier.
    if (kPoisonHeapReferences) {
      if (base_equals_value) {
        // `value_reg` has been moved to a temporary register, no need
        // to unpoison it.
      } else {
        __ UnpoisonHeapReference(CpuRegister(value_reg));
      }
      // No need to unpoison `expected` (RAX), as it is be overwritten below.
    }

    // Restore RAX.
    __ movq(CpuRegister(RAX), temp2_);

    __ Bind(&done);
    __ jmp(GetExitLabel());
  }

 private:
  // The location (register) of the marked object reference.
  const Location ref_;
  // The register containing the object holding the marked object reference field.
  const CpuRegister obj_;
  // The address of the marked reference field.  The base of this address must be `obj_`.
  const Address field_addr_;

  // Should the reference in `ref_` be unpoisoned prior to marking it?
  const bool unpoison_ref_before_marking_;

  const CpuRegister temp1_;
  const CpuRegister temp2_;

  DISALLOW_COPY_AND_ASSIGN(ReadBarrierMarkAndUpdateFieldSlowPathX86_64);
};

// Slow path generating a read barrier for a heap reference.
class ReadBarrierForHeapReferenceSlowPathX86_64 : public SlowPathCode {
 public:
  ReadBarrierForHeapReferenceSlowPathX86_64(HInstruction* instruction,
                                            Location out,
                                            Location ref,
                                            Location obj,
                                            uint32_t offset,
                                            Location index)
      : SlowPathCode(instruction),
        out_(out),
        ref_(ref),
        obj_(obj),
        offset_(offset),
        index_(index) {
    // If `obj` is equal to `out` or `ref`, it means the initial
    // object has been overwritten by (or after) the heap object
    // reference load to be instrumented, e.g.:
    //
    //   __ movl(out, Address(out, offset));
    //   codegen_->GenerateReadBarrierSlow(instruction, out_loc, out_loc, out_loc, offset);
    //
    // In that case, we have lost the information about the original
    // object, and the emitted read barrier cannot work properly.
    DCHECK(!obj.Equals(out)) << "obj=" << obj << " out=" << out;
    DCHECK(!obj.Equals(ref)) << "obj=" << obj << " ref=" << ref;
}

  void EmitNativeCode(CodeGenerator* codegen) override {
    DCHECK(codegen->EmitReadBarrier());
    CodeGeneratorX86_64* x86_64_codegen = down_cast<CodeGeneratorX86_64*>(codegen);
    LocationSummary* locations = instruction_->GetLocations();
    CpuRegister reg_out = out_.AsRegister<CpuRegister>();
    DCHECK(locations->CanCall());
    DCHECK(!locations->GetLiveRegisters()->ContainsCoreRegister(reg_out.AsRegister())) << out_;
    DCHECK(instruction_->IsInstanceFieldGet() ||
           instruction_->IsStaticFieldGet() ||
           instruction_->IsArrayGet() ||
           instruction_->IsInstanceOf() ||
           instruction_->IsCheckCast() ||
           (instruction_->IsInvoke() && instruction_->GetLocations()->Intrinsified()))
        << "Unexpected instruction in read barrier for heap reference slow path: "
        << instruction_->DebugName();

    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    // We may have to change the index's value, but as `index_` is a
    // constant member (like other "inputs" of this slow path),
    // introduce a copy of it, `index`.
    Location index = index_;
    if (index_.IsValid()) {
      // Handle `index_` for HArrayGet and UnsafeGetObject/UnsafeGetObjectVolatile intrinsics.
      if (instruction_->IsArrayGet()) {
        // Compute real offset and store it in index_.
        Register index_reg = index_.AsRegister<CpuRegister>().AsRegister();
        DCHECK(locations->GetLiveRegisters()->ContainsCoreRegister(index_reg));
        if (codegen->IsCoreCalleeSaveRegister(index_reg)) {
          // We are about to change the value of `index_reg` (see the
          // calls to art::x86_64::X86_64Assembler::shll and
          // art::x86_64::X86_64Assembler::AddImmediate below), but it
          // has not been saved by the previous call to
          // art::SlowPathCode::SaveLiveRegisters, as it is a
          // callee-save register --
          // art::SlowPathCode::SaveLiveRegisters does not consider
          // callee-save registers, as it has been designed with the
          // assumption that callee-save registers are supposed to be
          // handled by the called function.  So, as a callee-save
          // register, `index_reg` _would_ eventually be saved onto
          // the stack, but it would be too late: we would have
          // changed its value earlier.  Therefore, we manually save
          // it here into another freely available register,
          // `free_reg`, chosen of course among the caller-save
          // registers (as a callee-save `free_reg` register would
          // exhibit the same problem).
          //
          // Note we could have requested a temporary register from
          // the register allocator instead; but we prefer not to, as
          // this is a slow path, and we know we can find a
          // caller-save register that is available.
          Register free_reg = FindAvailableCallerSaveRegister(codegen).AsRegister();
          __ movl(CpuRegister(free_reg), CpuRegister(index_reg));
          index_reg = free_reg;
          index = Location::RegisterLocation(index_reg);
        } else {
          // The initial register stored in `index_` has already been
          // saved in the call to art::SlowPathCode::SaveLiveRegisters
          // (as it is not a callee-save register), so we can freely
          // use it.
        }
        // Shifting the index value contained in `index_reg` by the
        // scale factor (2) cannot overflow in practice, as the
        // runtime is unable to allocate object arrays with a size
        // larger than 2^26 - 1 (that is, 2^28 - 4 bytes).
        __ shll(CpuRegister(index_reg), Immediate(TIMES_4));
        static_assert(
            sizeof(mirror::HeapReference<mirror::Object>) == sizeof(int32_t),
            "art::mirror::HeapReference<art::mirror::Object> and int32_t have different sizes.");
        __ AddImmediate(CpuRegister(index_reg), Immediate(offset_));
      } else {
        // In the case of the UnsafeGetObject/UnsafeGetObjectVolatile
        // intrinsics, `index_` is not shifted by a scale factor of 2
        // (as in the case of ArrayGet), as it is actually an offset
        // to an object field within an object.
        DCHECK(instruction_->IsInvoke()) << instruction_->DebugName();
        DCHECK(instruction_->GetLocations()->Intrinsified());
        DCHECK((instruction_->AsInvoke()->GetIntrinsic() == Intrinsics::kUnsafeGetObject) ||
               (instruction_->AsInvoke()->GetIntrinsic() == Intrinsics::kUnsafeGetObjectVolatile) ||
               (instruction_->AsInvoke()->GetIntrinsic() == Intrinsics::kJdkUnsafeGetReference) ||
               (instruction_->AsInvoke()->GetIntrinsic() ==
                    Intrinsics::kJdkUnsafeGetReferenceVolatile) ||
               (instruction_->AsInvoke()->GetIntrinsic() ==
                    Intrinsics::kJdkUnsafeGetReferenceAcquire))
            << instruction_->AsInvoke()->GetIntrinsic();
        DCHECK_EQ(offset_, 0U);
        DCHECK(index_.IsRegister());
      }
    }

    // We're moving two or three locations to locations that could
    // overlap, so we need a parallel move resolver.
    InvokeRuntimeCallingConvention calling_convention;
    HParallelMove parallel_move(codegen->GetGraph()->GetAllocator());
    parallel_move.AddMove(ref_,
                          Location::RegisterLocation(calling_convention.GetRegisterAt(0)),
                          DataType::Type::kReference,
                          nullptr);
    parallel_move.AddMove(obj_,
                          Location::RegisterLocation(calling_convention.GetRegisterAt(1)),
                          DataType::Type::kReference,
                          nullptr);
    if (index.IsValid()) {
      parallel_move.AddMove(index,
                            Location::RegisterLocation(calling_convention.GetRegisterAt(2)),
                            DataType::Type::kInt32,
                            nullptr);
      codegen->GetMoveResolver()->EmitNativeCode(&parallel_move);
    } else {
      codegen->GetMoveResolver()->EmitNativeCode(&parallel_move);
      __ movl(CpuRegister(calling_convention.GetRegisterAt(2)), Immediate(offset_));
    }
    x86_64_codegen->InvokeRuntime(kQuickReadBarrierSlow, instruction_, this);
    CheckEntrypointTypes<
        kQuickReadBarrierSlow, mirror::Object*, mirror::Object*, mirror::Object*, uint32_t>();
    x86_64_codegen->Move(out_, Location::RegisterLocation(RAX));

    RestoreLiveRegisters(codegen, locations);
    __ jmp(GetExitLabel());
  }

  const char* GetDescription() const override {
    return "ReadBarrierForHeapReferenceSlowPathX86_64";
  }

 private:
  CpuRegister FindAvailableCallerSaveRegister(CodeGenerator* codegen) {
    size_t ref = static_cast<int>(ref_.AsRegister<CpuRegister>().AsRegister());
    size_t obj = static_cast<int>(obj_.AsRegister<CpuRegister>().AsRegister());
    for (size_t i = 0, e = codegen->GetNumberOfCoreRegisters(); i < e; ++i) {
      if (i != ref && i != obj && !codegen->IsCoreCalleeSaveRegister(i)) {
        return static_cast<CpuRegister>(i);
      }
    }
    // We shall never fail to find a free caller-save register, as
    // there are more than two core caller-save registers on x86-64
    // (meaning it is possible to find one which is different from
    // `ref` and `obj`).
    DCHECK_GT(codegen->GetNumberOfCoreCallerSaveRegisters(), 2u);
    LOG(FATAL) << "Could not find a free caller-save register";
    UNREACHABLE();
  }

  const Location out_;
  const Location ref_;
  const Location obj_;
  const uint32_t offset_;
  // An additional location containing an index to an array.
  // Only used for HArrayGet and the UnsafeGetObject &
  // UnsafeGetObjectVolatile intrinsics.
  const Location index_;

  DISALLOW_COPY_AND_ASSIGN(ReadBarrierForHeapReferenceSlowPathX86_64);
};

// Slow path generating a read barrier for a GC root.
class ReadBarrierForRootSlowPathX86_64 : public SlowPathCode {
 public:
  ReadBarrierForRootSlowPathX86_64(HInstruction* instruction, Location out, Location root)
      : SlowPathCode(instruction), out_(out), root_(root) {
  }

  void EmitNativeCode(CodeGenerator* codegen) override {
    DCHECK(codegen->EmitReadBarrier());
    LocationSummary* locations = instruction_->GetLocations();
    DCHECK(locations->CanCall());
    DCHECK(!locations->GetLiveRegisters()->ContainsCoreRegister(out_.reg()));
    DCHECK(instruction_->IsLoadClass() || instruction_->IsLoadString())
        << "Unexpected instruction in read barrier for GC root slow path: "
        << instruction_->DebugName();

    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    InvokeRuntimeCallingConvention calling_convention;
    CodeGeneratorX86_64* x86_64_codegen = down_cast<CodeGeneratorX86_64*>(codegen);
    x86_64_codegen->Move(Location::RegisterLocation(calling_convention.GetRegisterAt(0)), root_);
    x86_64_codegen->InvokeRuntime(kQuickReadBarrierForRootSlow, instruction_, this);
    CheckEntrypointTypes<kQuickReadBarrierForRootSlow, mirror::Object*, GcRoot<mirror::Object>*>();
    x86_64_codegen->Move(out_, Location::RegisterLocation(RAX));

    RestoreLiveRegisters(codegen, locations);
    __ jmp(GetExitLabel());
  }

  const char* GetDescription() const override { return "ReadBarrierForRootSlowPathX86_64"; }

 private:
  const Location out_;
  const Location root_;

  DISALLOW_COPY_AND_ASSIGN(ReadBarrierForRootSlowPathX86_64);
};

class MethodEntryExitHooksSlowPathX86_64 : public SlowPathCode {
 public:
  explicit MethodEntryExitHooksSlowPathX86_64(HInstruction* instruction)
      : SlowPathCode(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) override {
    CodeGeneratorX86_64* x86_64_codegen = down_cast<CodeGeneratorX86_64*>(codegen);
    LocationSummary* locations = instruction_->GetLocations();
    QuickEntrypointEnum entry_point =
        (instruction_->IsMethodEntryHook()) ? kQuickMethodEntryHook : kQuickMethodExitHook;
    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);
    if (instruction_->IsMethodExitHook()) {
      // Load FrameSize to pass to the exit hook.
      __ movq(CpuRegister(R8), Immediate(codegen->GetFrameSize()));
    }
    x86_64_codegen->InvokeRuntime(entry_point, instruction_, this);
    RestoreLiveRegisters(codegen, locations);
    __ jmp(GetExitLabel());
  }

  const char* GetDescription() const override {
    return "MethodEntryExitHooksSlowPath";
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(MethodEntryExitHooksSlowPathX86_64);
};

class CompileOptimizedSlowPathX86_64 : public SlowPathCode {
 public:
  CompileOptimizedSlowPathX86_64(HSuspendCheck* suspend_check, uint64_t counter_address)
      : SlowPathCode(suspend_check),
        counter_address_(counter_address) {}

  void EmitNativeCode(CodeGenerator* codegen) override {
    CodeGeneratorX86_64* x86_64_codegen = down_cast<CodeGeneratorX86_64*>(codegen);
    __ Bind(GetEntryLabel());
    __ movq(CpuRegister(TMP), Immediate(counter_address_));
    __ movw(Address(CpuRegister(TMP), 0), Immediate(ProfilingInfo::GetOptimizeThreshold()));
    if (instruction_ != nullptr) {
      // Only saves full width XMM for SIMD.
      SaveLiveRegisters(codegen, instruction_->GetLocations());
    }
    x86_64_codegen->GenerateInvokeRuntime(
        GetThreadOffset<kX86_64PointerSize>(kQuickCompileOptimized).Int32Value());
    if (instruction_ != nullptr) {
      // Only restores full width XMM for SIMD.
      RestoreLiveRegisters(codegen, instruction_->GetLocations());
    }
    __ jmp(GetExitLabel());
  }

  const char* GetDescription() const override {
    return "CompileOptimizedSlowPath";
  }

 private:
  uint64_t counter_address_;

  DISALLOW_COPY_AND_ASSIGN(CompileOptimizedSlowPathX86_64);
};

#undef __
// NOLINT on __ macro to suppress wrong warning/fix (misc-macro-parentheses) from clang-tidy.
#define __ down_cast<X86_64Assembler*>(GetAssembler())->  // NOLINT

inline Condition X86_64IntegerCondition(IfCondition cond) {
  switch (cond) {
    case kCondEQ: return kEqual;
    case kCondNE: return kNotEqual;
    case kCondLT: return kLess;
    case kCondLE: return kLessEqual;
    case kCondGT: return kGreater;
    case kCondGE: return kGreaterEqual;
    case kCondB:  return kBelow;
    case kCondBE: return kBelowEqual;
    case kCondA:  return kAbove;
    case kCondAE: return kAboveEqual;
  }
  LOG(FATAL) << "Unreachable";
  UNREACHABLE();
}

// Maps FP condition to x86_64 name.
inline Condition X86_64FPCondition(IfCondition cond) {
  switch (cond) {
    case kCondEQ: return kEqual;
    case kCondNE: return kNotEqual;
    case kCondLT: return kBelow;
    case kCondLE: return kBelowEqual;
    case kCondGT: return kAbove;
    case kCondGE: return kAboveEqual;
    default:      break;  // should not happen
  }
  LOG(FATAL) << "Unreachable";
  UNREACHABLE();
}

void CodeGeneratorX86_64::BlockNonVolatileXmmRegisters(LocationSummary* locations) {
  // We have to ensure that the native code we call directly (such as @CriticalNative
  // or some intrinsic helpers, say Math.sin()) doesn't clobber the XMM registers
  // which are non-volatile for ART, but volatile for Native calls.  This will ensure
  // that they are saved in the prologue and properly restored.
  for (FloatRegister fp_reg : non_volatile_xmm_regs) {
    locations->AddTemp(Location::FpuRegisterLocation(fp_reg));
  }
}

HInvokeStaticOrDirect::DispatchInfo CodeGeneratorX86_64::GetSupportedInvokeStaticOrDirectDispatch(
    const HInvokeStaticOrDirect::DispatchInfo& desired_dispatch_info,
    [[maybe_unused]] ArtMethod* method) {
  return desired_dispatch_info;
}

void CodeGeneratorX86_64::LoadMethod(MethodLoadKind load_kind, Location temp, HInvoke* invoke) {
  switch (load_kind) {
    case MethodLoadKind::kBootImageLinkTimePcRelative:
      DCHECK(GetCompilerOptions().IsBootImage() || GetCompilerOptions().IsBootImageExtension());
      __ leal(temp.AsRegister<CpuRegister>(),
              Address::Absolute(kPlaceholder32BitOffset, /* no_rip= */ false));
      RecordBootImageMethodPatch(invoke);
      break;
    case MethodLoadKind::kBootImageRelRo: {
      // Note: Boot image is in the low 4GiB and the entry is 32-bit, so emit a 32-bit load.
      __ movl(temp.AsRegister<CpuRegister>(),
              Address::Absolute(kPlaceholder32BitOffset, /* no_rip= */ false));
      RecordBootImageRelRoPatch(GetBootImageOffset(invoke));
      break;
    }
    case MethodLoadKind::kAppImageRelRo: {
      DCHECK(GetCompilerOptions().IsAppImage());
      __ movl(temp.AsRegister<CpuRegister>(),
              Address::Absolute(CodeGeneratorX86_64::kPlaceholder32BitOffset, /* no_rip= */ false));
      RecordAppImageMethodPatch(invoke);
      break;
    }
    case MethodLoadKind::kBssEntry: {
      __ movq(temp.AsRegister<CpuRegister>(),
              Address::Absolute(kPlaceholder32BitOffset, /* no_rip= */ false));
      RecordMethodBssEntryPatch(invoke);
      // No need for memory fence, thanks to the x86-64 memory model.
      break;
    }
    case MethodLoadKind::kJitDirectAddress: {
      Load64BitValue(temp.AsRegister<CpuRegister>(),
                     reinterpret_cast<int64_t>(invoke->GetResolvedMethod()));
      break;
    }
    case MethodLoadKind::kRuntimeCall: {
      // Test situation, don't do anything.
      break;
    }
    default: {
      LOG(FATAL) << "Load kind should have already been handled " << load_kind;
      UNREACHABLE();
    }
  }
}

void CodeGeneratorX86_64::GenerateStaticOrDirectCall(
    HInvokeStaticOrDirect* invoke, Location temp, SlowPathCode* slow_path) {
  // All registers are assumed to be correctly set up.

  Location callee_method = temp;  // For all kinds except kRecursive, callee will be in temp.
  switch (invoke->GetMethodLoadKind()) {
    case MethodLoadKind::kStringInit: {
      // temp = thread->string_init_entrypoint
      uint32_t offset =
          GetThreadOffset<kX86_64PointerSize>(invoke->GetStringInitEntryPoint()).Int32Value();
      __ gs()->movq(temp.AsRegister<CpuRegister>(), Address::Absolute(offset, /* no_rip= */ true));
      break;
    }
    case MethodLoadKind::kRecursive: {
      callee_method = invoke->GetLocations()->InAt(invoke->GetCurrentMethodIndex());
      break;
    }
    case MethodLoadKind::kRuntimeCall: {
      GenerateInvokeStaticOrDirectRuntimeCall(invoke, temp, slow_path);
      return;  // No code pointer retrieval; the runtime performs the call directly.
    }
    case MethodLoadKind::kBootImageLinkTimePcRelative:
      // For kCallCriticalNative we skip loading the method and do the call directly.
      if (invoke->GetCodePtrLocation() == CodePtrLocation::kCallCriticalNative) {
        break;
      }
      FALLTHROUGH_INTENDED;
    default: {
      LoadMethod(invoke->GetMethodLoadKind(), temp, invoke);
      break;
    }
  }

  switch (invoke->GetCodePtrLocation()) {
    case CodePtrLocation::kCallSelf:
      DCHECK(!GetGraph()->HasShouldDeoptimizeFlag());
      __ call(&frame_entry_label_);
      RecordPcInfo(invoke, slow_path);
      break;
    case CodePtrLocation::kCallCriticalNative: {
      size_t out_frame_size =
          PrepareCriticalNativeCall<CriticalNativeCallingConventionVisitorX86_64,
                                    kNativeStackAlignment,
                                    GetCriticalNativeDirectCallFrameSize>(invoke);
      if (invoke->GetMethodLoadKind() == MethodLoadKind::kBootImageLinkTimePcRelative) {
        DCHECK(GetCompilerOptions().IsBootImage() || GetCompilerOptions().IsBootImageExtension());
        __ call(Address::Absolute(kPlaceholder32BitOffset, /* no_rip= */ false));
        RecordBootImageJniEntrypointPatch(invoke);
      } else {
        // (callee_method + offset_of_jni_entry_point)()
        __ call(Address(callee_method.AsRegister<CpuRegister>(),
                         ArtMethod::EntryPointFromJniOffset(kX86_64PointerSize).SizeValue()));
      }
      RecordPcInfo(invoke, slow_path);
      // Zero-/sign-extend the result when needed due to native and managed ABI mismatch.
      switch (invoke->GetType()) {
        case DataType::Type::kBool:
          __ movzxb(CpuRegister(RAX), CpuRegister(RAX));
          break;
        case DataType::Type::kInt8:
          __ movsxb(CpuRegister(RAX), CpuRegister(RAX));
          break;
        case DataType::Type::kUint16:
          __ movzxw(CpuRegister(RAX), CpuRegister(RAX));
          break;
        case DataType::Type::kInt16:
          __ movsxw(CpuRegister(RAX), CpuRegister(RAX));
          break;
        case DataType::Type::kInt32:
        case DataType::Type::kInt64:
        case DataType::Type::kFloat32:
        case DataType::Type::kFloat64:
        case DataType::Type::kVoid:
          break;
        default:
          DCHECK(false) << invoke->GetType();
          break;
      }
      if (out_frame_size != 0u) {
        DecreaseFrame(out_frame_size);
      }
      break;
    }
    case CodePtrLocation::kCallArtMethod:
      // (callee_method + offset_of_quick_compiled_code)()
      __ call(Address(callee_method.AsRegister<CpuRegister>(),
                      ArtMethod::EntryPointFromQuickCompiledCodeOffset(
                          kX86_64PointerSize).SizeValue()));
      RecordPcInfo(invoke, slow_path);
      break;
  }

  DCHECK(!IsLeafMethod());
}

void CodeGeneratorX86_64::GenerateVirtualCall(
    HInvokeVirtual* invoke, Location temp_in, SlowPathCode* slow_path) {
  CpuRegister temp = temp_in.AsRegister<CpuRegister>();
  size_t method_offset = mirror::Class::EmbeddedVTableEntryOffset(
      invoke->GetVTableIndex(), kX86_64PointerSize).SizeValue();

  // Use the calling convention instead of the location of the receiver, as
  // intrinsics may have put the receiver in a different register. In the intrinsics
  // slow path, the arguments have been moved to the right place, so here we are
  // guaranteed that the receiver is the first register of the calling convention.
  InvokeDexCallingConvention calling_convention;
  Register receiver = calling_convention.GetRegisterAt(0);

  size_t class_offset = mirror::Object::ClassOffset().SizeValue();
  // /* HeapReference<Class> */ temp = receiver->klass_
  __ movl(temp, Address(CpuRegister(receiver), class_offset));
  MaybeRecordImplicitNullCheck(invoke);
  // Instead of simply (possibly) unpoisoning `temp` here, we should
  // emit a read barrier for the previous class reference load.
  // However this is not required in practice, as this is an
  // intermediate/temporary reference and because the current
  // concurrent copying collector keeps the from-space memory
  // intact/accessible until the end of the marking phase (the
  // concurrent copying collector may not in the future).
  __ MaybeUnpoisonHeapReference(temp);

  MaybeGenerateInlineCacheCheck(invoke, temp);

  // temp = temp->GetMethodAt(method_offset);
  __ movq(temp, Address(temp, method_offset));
  // call temp->GetEntryPoint();
  __ call(Address(temp, ArtMethod::EntryPointFromQuickCompiledCodeOffset(
      kX86_64PointerSize).SizeValue()));
  RecordPcInfo(invoke, slow_path);
}

void CodeGeneratorX86_64::RecordBootImageIntrinsicPatch(uint32_t intrinsic_data) {
  boot_image_other_patches_.emplace_back(/* target_dex_file= */ nullptr, intrinsic_data);
  __ Bind(&boot_image_other_patches_.back().label);
}

void CodeGeneratorX86_64::RecordBootImageRelRoPatch(uint32_t boot_image_offset) {
  boot_image_other_patches_.emplace_back(/* target_dex_file= */ nullptr, boot_image_offset);
  __ Bind(&boot_image_other_patches_.back().label);
}

void CodeGeneratorX86_64::RecordBootImageMethodPatch(HInvoke* invoke) {
  boot_image_method_patches_.emplace_back(invoke->GetResolvedMethodReference().dex_file,
                                          invoke->GetResolvedMethodReference().index);
  __ Bind(&boot_image_method_patches_.back().label);
}

void CodeGeneratorX86_64::RecordAppImageMethodPatch(HInvoke* invoke) {
  app_image_method_patches_.emplace_back(invoke->GetResolvedMethodReference().dex_file,
                                         invoke->GetResolvedMethodReference().index);
  __ Bind(&app_image_method_patches_.back().label);
}

void CodeGeneratorX86_64::RecordMethodBssEntryPatch(HInvoke* invoke) {
  DCHECK(IsSameDexFile(GetGraph()->GetDexFile(), *invoke->GetMethodReference().dex_file) ||
         GetCompilerOptions().WithinOatFile(invoke->GetMethodReference().dex_file) ||
         ContainsElement(Runtime::Current()->GetClassLinker()->GetBootClassPath(),
                         invoke->GetMethodReference().dex_file));
  method_bss_entry_patches_.emplace_back(invoke->GetMethodReference().dex_file,
                                         invoke->GetMethodReference().index);
  __ Bind(&method_bss_entry_patches_.back().label);
}

void CodeGeneratorX86_64::RecordBootImageTypePatch(const DexFile& dex_file,
                                                   dex::TypeIndex type_index) {
  boot_image_type_patches_.emplace_back(&dex_file, type_index.index_);
  __ Bind(&boot_image_type_patches_.back().label);
}

void CodeGeneratorX86_64::RecordAppImageTypePatch(const DexFile& dex_file,
                                                  dex::TypeIndex type_index) {
  app_image_type_patches_.emplace_back(&dex_file, type_index.index_);
  __ Bind(&app_image_type_patches_.back().label);
}

Label* CodeGeneratorX86_64::NewTypeBssEntryPatch(HLoadClass* load_class) {
  ArenaDeque<PatchInfo<Label>>* patches = nullptr;
  switch (load_class->GetLoadKind()) {
    case HLoadClass::LoadKind::kBssEntry:
      patches = &type_bss_entry_patches_;
      break;
    case HLoadClass::LoadKind::kBssEntryPublic:
      patches = &public_type_bss_entry_patches_;
      break;
    case HLoadClass::LoadKind::kBssEntryPackage:
      patches = &package_type_bss_entry_patches_;
      break;
    default:
      LOG(FATAL) << "Unexpected load kind: " << load_class->GetLoadKind();
      UNREACHABLE();
  }
  patches->emplace_back(&load_class->GetDexFile(), load_class->GetTypeIndex().index_);
  return &patches->back().label;
}

void CodeGeneratorX86_64::RecordBootImageStringPatch(HLoadString* load_string) {
  boot_image_string_patches_.emplace_back(
      &load_string->GetDexFile(), load_string->GetStringIndex().index_);
  __ Bind(&boot_image_string_patches_.back().label);
}

Label* CodeGeneratorX86_64::NewStringBssEntryPatch(HLoadString* load_string) {
  string_bss_entry_patches_.emplace_back(
      &load_string->GetDexFile(), load_string->GetStringIndex().index_);
  return &string_bss_entry_patches_.back().label;
}

Label* CodeGeneratorX86_64::NewMethodTypeBssEntryPatch(HLoadMethodType* load_method_type) {
  method_type_bss_entry_patches_.emplace_back(
      &load_method_type->GetDexFile(), load_method_type->GetProtoIndex().index_);
  return &method_type_bss_entry_patches_.back().label;
}

void CodeGeneratorX86_64::RecordBootImageJniEntrypointPatch(HInvokeStaticOrDirect* invoke) {
  boot_image_jni_entrypoint_patches_.emplace_back(invoke->GetResolvedMethodReference().dex_file,
                                                  invoke->GetResolvedMethodReference().index);
  __ Bind(&boot_image_jni_entrypoint_patches_.back().label);
}

void CodeGeneratorX86_64::LoadBootImageAddress(CpuRegister reg, uint32_t boot_image_reference) {
  if (GetCompilerOptions().IsBootImage()) {
    __ leal(reg,
            Address::Absolute(CodeGeneratorX86_64::kPlaceholder32BitOffset, /* no_rip= */ false));
    RecordBootImageIntrinsicPatch(boot_image_reference);
  } else if (GetCompilerOptions().GetCompilePic()) {
    __ movl(reg,
            Address::Absolute(CodeGeneratorX86_64::kPlaceholder32BitOffset, /* no_rip= */ false));
    RecordBootImageRelRoPatch(boot_image_reference);
  } else {
    DCHECK(GetCompilerOptions().IsJitCompiler());
    gc::Heap* heap = Runtime::Current()->GetHeap();
    DCHECK(!heap->GetBootImageSpaces().empty());
    const uint8_t* address = heap->GetBootImageSpaces()[0]->Begin() + boot_image_reference;
    __ movl(reg, Immediate(dchecked_integral_cast<uint32_t>(reinterpret_cast<uintptr_t>(address))));
  }
}

void CodeGeneratorX86_64::LoadIntrinsicDeclaringClass(CpuRegister reg, HInvoke* invoke) {
  DCHECK_NE(invoke->GetIntrinsic(), Intrinsics::kNone);
  if (GetCompilerOptions().IsBootImage()) {
    // Load the type the same way as for HLoadClass::LoadKind::kBootImageLinkTimePcRelative.
    __ leal(reg,
            Address::Absolute(CodeGeneratorX86_64::kPlaceholder32BitOffset, /* no_rip= */ false));
    MethodReference target_method = invoke->GetResolvedMethodReference();
    dex::TypeIndex type_idx = target_method.dex_file->GetMethodId(target_method.index).class_idx_;
    boot_image_type_patches_.emplace_back(target_method.dex_file, type_idx.index_);
    __ Bind(&boot_image_type_patches_.back().label);
  } else {
    uint32_t boot_image_offset = GetBootImageOffsetOfIntrinsicDeclaringClass(invoke);
    LoadBootImageAddress(reg, boot_image_offset);
  }
}

void CodeGeneratorX86_64::LoadClassRootForIntrinsic(CpuRegister reg, ClassRoot class_root) {
  if (GetCompilerOptions().IsBootImage()) {
    ScopedObjectAccess soa(Thread::Current());
    ObjPtr<mirror::Class> klass = GetClassRoot(class_root);
    boot_image_type_patches_.emplace_back(&klass->GetDexFile(), klass->GetDexTypeIndex().index_);
    __ Bind(&boot_image_type_patches_.back().label);
  } else {
    uint32_t boot_image_offset = GetBootImageOffset(class_root);
    LoadBootImageAddress(reg, boot_image_offset);
  }
}

// The label points to the end of the "movl" or another instruction but the literal offset
// for method patch needs to point to the embedded constant which occupies the last 4 bytes.
constexpr uint32_t kLabelPositionToLiteralOffsetAdjustment = 4u;

template <linker::LinkerPatch (*Factory)(size_t, const DexFile*, uint32_t, uint32_t)>
inline void CodeGeneratorX86_64::EmitPcRelativeLinkerPatches(
    const ArenaDeque<PatchInfo<Label>>& infos,
    ArenaVector<linker::LinkerPatch>* linker_patches) {
  for (const PatchInfo<Label>& info : infos) {
    uint32_t literal_offset = info.label.Position() - kLabelPositionToLiteralOffsetAdjustment;
    linker_patches->push_back(
        Factory(literal_offset, info.target_dex_file, info.label.Position(), info.offset_or_index));
  }
}

template <linker::LinkerPatch (*Factory)(size_t, uint32_t, uint32_t)>
linker::LinkerPatch NoDexFileAdapter(size_t literal_offset,
                                     const DexFile* target_dex_file,
                                     uint32_t pc_insn_offset,
                                     uint32_t boot_image_offset) {
  DCHECK(target_dex_file == nullptr);  // Unused for these patches, should be null.
  return Factory(literal_offset, pc_insn_offset, boot_image_offset);
}

void CodeGeneratorX86_64::EmitLinkerPatches(ArenaVector<linker::LinkerPatch>* linker_patches) {
  DCHECK(linker_patches->empty());
  size_t size =
      boot_image_method_patches_.size() +
      app_image_method_patches_.size() +
      method_bss_entry_patches_.size() +
      boot_image_type_patches_.size() +
      app_image_type_patches_.size() +
      type_bss_entry_patches_.size() +
      public_type_bss_entry_patches_.size() +
      package_type_bss_entry_patches_.size() +
      boot_image_string_patches_.size() +
      string_bss_entry_patches_.size() +
      method_type_bss_entry_patches_.size() +
      boot_image_jni_entrypoint_patches_.size() +
      boot_image_other_patches_.size();
  linker_patches->reserve(size);
  if (GetCompilerOptions().IsBootImage() || GetCompilerOptions().IsBootImageExtension()) {
    EmitPcRelativeLinkerPatches<linker::LinkerPatch::RelativeMethodPatch>(
        boot_image_method_patches_, linker_patches);
    EmitPcRelativeLinkerPatches<linker::LinkerPatch::RelativeTypePatch>(
        boot_image_type_patches_, linker_patches);
    EmitPcRelativeLinkerPatches<linker::LinkerPatch::RelativeStringPatch>(
        boot_image_string_patches_, linker_patches);
  } else {
    DCHECK(boot_image_method_patches_.empty());
    DCHECK(boot_image_type_patches_.empty());
    DCHECK(boot_image_string_patches_.empty());
  }
  DCHECK_IMPLIES(!GetCompilerOptions().IsAppImage(), app_image_method_patches_.empty());
  DCHECK_IMPLIES(!GetCompilerOptions().IsAppImage(), app_image_type_patches_.empty());
  if (GetCompilerOptions().IsBootImage()) {
    EmitPcRelativeLinkerPatches<NoDexFileAdapter<linker::LinkerPatch::IntrinsicReferencePatch>>(
        boot_image_other_patches_, linker_patches);
  } else {
    EmitPcRelativeLinkerPatches<NoDexFileAdapter<linker::LinkerPatch::BootImageRelRoPatch>>(
        boot_image_other_patches_, linker_patches);
    EmitPcRelativeLinkerPatches<linker::LinkerPatch::MethodAppImageRelRoPatch>(
        app_image_method_patches_, linker_patches);
    EmitPcRelativeLinkerPatches<linker::LinkerPatch::TypeAppImageRelRoPatch>(
        app_image_type_patches_, linker_patches);
  }
  EmitPcRelativeLinkerPatches<linker::LinkerPatch::MethodBssEntryPatch>(
      method_bss_entry_patches_, linker_patches);
  EmitPcRelativeLinkerPatches<linker::LinkerPatch::TypeBssEntryPatch>(
      type_bss_entry_patches_, linker_patches);
  EmitPcRelativeLinkerPatches<linker::LinkerPatch::PublicTypeBssEntryPatch>(
      public_type_bss_entry_patches_, linker_patches);
  EmitPcRelativeLinkerPatches<linker::LinkerPatch::PackageTypeBssEntryPatch>(
      package_type_bss_entry_patches_, linker_patches);
  EmitPcRelativeLinkerPatches<linker::LinkerPatch::StringBssEntryPatch>(
      string_bss_entry_patches_, linker_patches);
  EmitPcRelativeLinkerPatches<linker::LinkerPatch::MethodTypeBssEntryPatch>(
      method_type_bss_entry_patches_, linker_patches);
  EmitPcRelativeLinkerPatches<linker::LinkerPatch::RelativeJniEntrypointPatch>(
      boot_image_jni_entrypoint_patches_, linker_patches);
  DCHECK_EQ(size, linker_patches->size());
}

void CodeGeneratorX86_64::DumpCoreRegister(std::ostream& stream, int reg) const {
  stream << Register(reg);
}

void CodeGeneratorX86_64::DumpFloatingPointRegister(std::ostream& stream, int reg) const {
  stream << FloatRegister(reg);
}

const X86_64InstructionSetFeatures& CodeGeneratorX86_64::GetInstructionSetFeatures() const {
  return *GetCompilerOptions().GetInstructionSetFeatures()->AsX86_64InstructionSetFeatures();
}

size_t CodeGeneratorX86_64::SaveCoreRegister(size_t stack_index, uint32_t reg_id) {
  __ movq(Address(CpuRegister(RSP), stack_index), CpuRegister(reg_id));
  return kX86_64WordSize;
}

size_t CodeGeneratorX86_64::RestoreCoreRegister(size_t stack_index, uint32_t reg_id) {
  __ movq(CpuRegister(reg_id), Address(CpuRegister(RSP), stack_index));
  return kX86_64WordSize;
}

size_t CodeGeneratorX86_64::SaveFloatingPointRegister(size_t stack_index, uint32_t reg_id) {
  if (GetGraph()->HasSIMD()) {
    __ movups(Address(CpuRegister(RSP), stack_index), XmmRegister(reg_id));
  } else {
    __ movsd(Address(CpuRegister(RSP), stack_index), XmmRegister(reg_id));
  }
  return GetSlowPathFPWidth();
}

size_t CodeGeneratorX86_64::RestoreFloatingPointRegister(size_t stack_index, uint32_t reg_id) {
  if (GetGraph()->HasSIMD()) {
    __ movups(XmmRegister(reg_id), Address(CpuRegister(RSP), stack_index));
  } else {
    __ movsd(XmmRegister(reg_id), Address(CpuRegister(RSP), stack_index));
  }
  return GetSlowPathFPWidth();
}

void CodeGeneratorX86_64::InvokeRuntime(QuickEntrypointEnum entrypoint,
                                        HInstruction* instruction,
                                        SlowPathCode* slow_path) {
  ValidateInvokeRuntime(entrypoint, instruction, slow_path);
  GenerateInvokeRuntime(GetThreadOffset<kX86_64PointerSize>(entrypoint).Int32Value());
  if (EntrypointRequiresStackMap(entrypoint)) {
    RecordPcInfo(instruction, slow_path);
  }
}

void CodeGeneratorX86_64::InvokeRuntimeWithoutRecordingPcInfo(int32_t entry_point_offset,
                                                              HInstruction* instruction,
                                                              SlowPathCode* slow_path) {
  ValidateInvokeRuntimeWithoutRecordingPcInfo(instruction, slow_path);
  GenerateInvokeRuntime(entry_point_offset);
}

void CodeGeneratorX86_64::GenerateInvokeRuntime(int32_t entry_point_offset) {
  __ gs()->call(Address::Absolute(entry_point_offset, /* no_rip= */ true));
}

namespace detail {

// Mark which intrinsics we don't have handcrafted code for.
template <Intrinsics T>
struct IsUnimplemented {
  bool is_unimplemented = false;
};

#define TRUE_OVERRIDE(Name)                     \
  template <>                                   \
  struct IsUnimplemented<Intrinsics::k##Name> { \
    bool is_unimplemented = true;               \
  };
UNIMPLEMENTED_INTRINSIC_LIST_X86_64(TRUE_OVERRIDE)
#undef TRUE_OVERRIDE

static constexpr bool kIsIntrinsicUnimplemented[] = {
    false,  // kNone
#define IS_UNIMPLEMENTED(Intrinsic, ...) \
    IsUnimplemented<Intrinsics::k##Intrinsic>().is_unimplemented,
    ART_INTRINSICS_LIST(IS_UNIMPLEMENTED)
#undef IS_UNIMPLEMENTED
};

}  // namespace detail

static constexpr int kNumberOfCpuRegisterPairs = 0;
// Use a fake return address register to mimic Quick.
static constexpr Register kFakeReturnRegister = Register(kLastCpuRegister + 1);
CodeGeneratorX86_64::CodeGeneratorX86_64(HGraph* graph,
                                         const CompilerOptions& compiler_options,
                                         OptimizingCompilerStats* stats)
    : CodeGenerator(graph,
                    kNumberOfCpuRegisters,
                    kNumberOfFloatRegisters,
                    kNumberOfCpuRegisterPairs,
                    ComputeRegisterMask(kCoreCalleeSaves, arraysize(kCoreCalleeSaves))
                        | (1 << kFakeReturnRegister),
                    ComputeRegisterMask(kFpuCalleeSaves, arraysize(kFpuCalleeSaves)),
                    compiler_options,
                    stats,
                    ArrayRef<const bool>(detail::kIsIntrinsicUnimplemented)),
      block_labels_(nullptr),
      location_builder_(graph, this),
      instruction_visitor_(graph, this),
      move_resolver_(graph->GetAllocator(), this),
      assembler_(graph->GetAllocator(),
                 compiler_options.GetInstructionSetFeatures()->AsX86_64InstructionSetFeatures()),
      constant_area_start_(0),
      boot_image_method_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      app_image_method_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      method_bss_entry_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      boot_image_type_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      app_image_type_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      type_bss_entry_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      public_type_bss_entry_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      package_type_bss_entry_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      boot_image_string_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      string_bss_entry_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      method_type_bss_entry_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      boot_image_jni_entrypoint_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      boot_image_other_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      jit_string_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      jit_class_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      jit_method_type_patches_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)),
      fixups_to_jump_tables_(graph->GetAllocator()->Adapter(kArenaAllocCodeGenerator)) {
  AddAllocatedRegister(Location::RegisterLocation(kFakeReturnRegister));
}

InstructionCodeGeneratorX86_64::InstructionCodeGeneratorX86_64(HGraph* graph,
                                                               CodeGeneratorX86_64* codegen)
      : InstructionCodeGenerator(graph, codegen),
        assembler_(codegen->GetAssembler()),
        codegen_(codegen) {}

void CodeGeneratorX86_64::SetupBlockedRegisters() const {
  // Stack register is always reserved.
  blocked_core_registers_[RSP] = true;

  // Block the register used as TMP.
  blocked_core_registers_[TMP] = true;
}

static dwarf::Reg DWARFReg(Register reg) {
  return dwarf::Reg::X86_64Core(static_cast<int>(reg));
}

static dwarf::Reg DWARFReg(FloatRegister reg) {
  return dwarf::Reg::X86_64Fp(static_cast<int>(reg));
}

void LocationsBuilderX86_64::VisitMethodEntryHook(HMethodEntryHook* method_hook) {
  LocationSummary* locations = new (GetGraph()->GetAllocator())
      LocationSummary(method_hook, LocationSummary::kCallOnSlowPath);
  // We use rdtsc to record the timestamp for method profiling. rdtsc returns
  // two 32-bit values in EAX + EDX even on 64-bit architectures.
  locations->AddTemp(Location::RegisterLocation(RAX));
  locations->AddTemp(Location::RegisterLocation(RDX));
}

void InstructionCodeGeneratorX86_64::GenerateMethodEntryExitHook(HInstruction* instruction) {
  SlowPathCode* slow_path =
      new (codegen_->GetScopedAllocator()) MethodEntryExitHooksSlowPathX86_64(instruction);
  LocationSummary* locations = instruction->GetLocations();
  codegen_->AddSlowPath(slow_path);

  if (instruction->IsMethodExitHook()) {
    // Check if we are required to check if the caller needs a deoptimization. Strictly speaking it
    // would be sufficient to check if CheckCallerForDeopt bit is set. Though it is faster to check
    // if it is just non-zero. kCHA bit isn't used in debuggable runtimes as cha optimization is
    // disabled in debuggable runtime. The other bit is used when this method itself requires a
    // deoptimization due to redefinition. So it is safe to just check for non-zero value here.
    __ cmpl(Address(CpuRegister(RSP), codegen_->GetStackOffsetOfShouldDeoptimizeFlag()),
            Immediate(0));
    __ j(kNotEqual, slow_path->GetEntryLabel());
  }

  uint64_t address = reinterpret_cast64<uint64_t>(Runtime::Current()->GetInstrumentation());
  MemberOffset  offset = instruction->IsMethodExitHook() ?
      instrumentation::Instrumentation::HaveMethodExitListenersOffset()
      : instrumentation::Instrumentation::HaveMethodEntryListenersOffset();
  __ movq(CpuRegister(TMP), Immediate(address + offset.Int32Value()));
  __ cmpb(Address(CpuRegister(TMP), 0),
          Immediate(instrumentation::Instrumentation::kFastTraceListeners));
  // Check if there are any method entry / exit listeners. If no, continue with execution.
  __ j(kLess, slow_path->GetExitLabel());
  // Check if there are any slow method entry / exit listeners. If yes, take the slow path.
  __ j(kGreater, slow_path->GetEntryLabel());

  // Check if there is place in the buffer for a new entry, if no, take slow path.
  CpuRegister init_entry = locations->GetTemp(0).AsRegister<CpuRegister>();
  // Use a register that is different from RAX and RDX. RDTSC returns result in RAX and RDX and we
  // use curr entry to store the result into the buffer.
  CpuRegister curr_entry = CpuRegister(TMP);
  DCHECK(curr_entry.AsRegister() != RAX);
  DCHECK(curr_entry.AsRegister() != RDX);
  uint64_t trace_buffer_curr_entry_offset =
      Thread::TraceBufferCurrPtrOffset<kX86_64PointerSize>().SizeValue();
  __ gs()->movq(CpuRegister(curr_entry),
                Address::Absolute(trace_buffer_curr_entry_offset, /* no_rip= */ true));
  __ subq(CpuRegister(curr_entry), Immediate(kNumEntriesForWallClock * sizeof(void*)));
  __ gs()->movq(init_entry,
                Address::Absolute(Thread::TraceBufferPtrOffset<kX86_64PointerSize>().SizeValue(),
                                  /* no_rip= */ true));
  __ cmpq(curr_entry, init_entry);
  __ j(kLess, slow_path->GetEntryLabel());

  // Update the index in the `Thread`.
  __ gs()->movq(Address::Absolute(trace_buffer_curr_entry_offset, /* no_rip= */ true),
                CpuRegister(curr_entry));

  // Record method pointer and action.
  CpuRegister method = init_entry;
  __ movq(CpuRegister(method), Address(CpuRegister(RSP), kCurrentMethodStackOffset));
  // Use last two bits to encode trace method action. For MethodEntry it is 0
  // so no need to set the bits since they are 0 already.
  if (instruction->IsMethodExitHook()) {
    DCHECK_GE(ArtMethod::Alignment(kRuntimePointerSize), static_cast<size_t>(4));
    static_assert(enum_cast<int32_t>(TraceAction::kTraceMethodEnter) == 0);
    static_assert(enum_cast<int32_t>(TraceAction::kTraceMethodExit) == 1);
    __ orq(method, Immediate(enum_cast<int32_t>(TraceAction::kTraceMethodExit)));
  }
  __ movq(Address(curr_entry, kMethodOffsetInBytes), CpuRegister(method));
  // Get the timestamp. rdtsc returns timestamp in RAX + RDX even in 64-bit architectures.
  __ rdtsc();
  __ shlq(CpuRegister(RDX), Immediate(32));
  __ orq(CpuRegister(RAX), CpuRegister(RDX));
  __ movq(Address(curr_entry, kTimestampOffsetInBytes), CpuRegister(RAX));
  __ Bind(slow_path->GetExitLabel());
}

void InstructionCodeGeneratorX86_64::VisitMethodEntryHook(HMethodEntryHook* instruction) {
  DCHECK(codegen_->GetCompilerOptions().IsJitCompiler() && GetGraph()->IsDebuggable());
  DCHECK(codegen_->RequiresCurrentMethod());
  GenerateMethodEntryExitHook(instruction);
}

void SetInForReturnValue(HInstruction* instr, LocationSummary* locations) {
  switch (instr->InputAt(0)->GetType()) {
    case DataType::Type::kReference:
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      locations->SetInAt(0, Location::RegisterLocation(RAX));
      break;

    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64:
      locations->SetInAt(0, Location::FpuRegisterLocation(XMM0));
      break;

    case DataType::Type::kVoid:
      locations->SetInAt(0, Location::NoLocation());
      break;

    default:
      LOG(FATAL) << "Unexpected return type " << instr->InputAt(0)->GetType();
  }
}

void LocationsBuilderX86_64::VisitMethodExitHook(HMethodExitHook* method_hook) {
  LocationSummary* locations = new (GetGraph()->GetAllocator())
      LocationSummary(method_hook, LocationSummary::kCallOnSlowPath);
  SetInForReturnValue(method_hook, locations);
  // We use rdtsc to record the timestamp for method profiling. rdtsc returns
  // two 32-bit values in EAX + EDX even on 64-bit architectures.
  locations->AddTemp(Location::RegisterLocation(RAX));
  locations->AddTemp(Location::RegisterLocation(RDX));
}

void InstructionCodeGeneratorX86_64::VisitMethodExitHook(HMethodExitHook* instruction) {
  DCHECK(codegen_->GetCompilerOptions().IsJitCompiler() && GetGraph()->IsDebuggable());
  DCHECK(codegen_->RequiresCurrentMethod());
  GenerateMethodEntryExitHook(instruction);
}

void CodeGeneratorX86_64::MaybeIncrementHotness(HSuspendCheck* suspend_check, bool is_frame_entry) {
  if (GetCompilerOptions().CountHotnessInCompiledCode()) {
    NearLabel overflow;
    Register method = kMethodRegisterArgument;
    if (!is_frame_entry) {
      CHECK(RequiresCurrentMethod());
      method = TMP;
      __ movq(CpuRegister(method), Address(CpuRegister(RSP), kCurrentMethodStackOffset));
    }
    __ cmpw(Address(CpuRegister(method), ArtMethod::HotnessCountOffset().Int32Value()),
            Immediate(interpreter::kNterpHotnessValue));
    __ j(kEqual, &overflow);
    __ addw(Address(CpuRegister(method), ArtMethod::HotnessCountOffset().Int32Value()),
            Immediate(-1));
    __ Bind(&overflow);
  }

  if (GetGraph()->IsCompilingBaseline() &&
      GetGraph()->IsUsefulOptimizing() &&
      !Runtime::Current()->IsAotCompiler()) {
    ProfilingInfo* info = GetGraph()->GetProfilingInfo();
    DCHECK(info != nullptr);
    CHECK(!HasEmptyFrame());
    uint64_t address = reinterpret_cast64<uint64_t>(info) +
        ProfilingInfo::BaselineHotnessCountOffset().Int32Value();
    SlowPathCode* slow_path =
        new (GetScopedAllocator()) CompileOptimizedSlowPathX86_64(suspend_check, address);
    AddSlowPath(slow_path);
    // Note: if the address was in the 32bit range, we could use
    // Address::Absolute and avoid this movq.
    __ movq(CpuRegister(TMP), Immediate(address));
    // With multiple threads, this can overflow. This is OK, we will eventually get to see
    // it reaching 0. Also, at this point we have no register available to look
    // at the counter directly.
    __ addw(Address(CpuRegister(TMP), 0), Immediate(-1));
    __ j(kEqual, slow_path->GetEntryLabel());
    __ Bind(slow_path->GetExitLabel());
  }
}

void CodeGeneratorX86_64::GenerateFrameEntry() {
  __ cfi().SetCurrentCFAOffset(kX86_64WordSize);  // return address

  // Check if we need to generate the clinit check. We will jump to the
  // resolution stub if the class is not initialized and the executing thread is
  // not the thread initializing it.
  // We do this before constructing the frame to get the correct stack trace if
  // an exception is thrown.
  if (GetCompilerOptions().ShouldCompileWithClinitCheck(GetGraph()->GetArtMethod())) {
    NearLabel resolution;
    // Check if we're visibly initialized.

    // We don't emit a read barrier here to save on code size. We rely on the
    // resolution trampoline to do a suspend check before re-entering this code.
    __ movl(CpuRegister(TMP),
            Address(CpuRegister(kMethodRegisterArgument),
                    ArtMethod::DeclaringClassOffset().Int32Value()));
    __ cmpb(Address(CpuRegister(TMP), kClassStatusByteOffset),
            Immediate(kShiftedVisiblyInitializedValue));
    __ j(kAboveEqual, &frame_entry_label_);

    // Check if we're initializing and the thread initializing is the one
    // executing the code.
    __ cmpb(Address(CpuRegister(TMP), kClassStatusByteOffset),
            Immediate(kShiftedInitializingValue));
    __ j(kBelow, &resolution);

    __ movl(CpuRegister(TMP),
            Address(CpuRegister(TMP), mirror::Class::ClinitThreadIdOffset().Int32Value()));
    __ gs()->cmpl(
        CpuRegister(TMP),
        Address::Absolute(Thread::TidOffset<kX86_64PointerSize>().Int32Value(), /*no_rip=*/ true));
    __ j(kEqual, &frame_entry_label_);
    __ Bind(&resolution);

    // Jump to the resolution stub.
    ThreadOffset64 entrypoint_offset =
        GetThreadOffset<kX86_64PointerSize>(kQuickQuickResolutionTrampoline);
    __ gs()->jmp(Address::Absolute(entrypoint_offset, /*no_rip=*/ true));
  }

  __ Bind(&frame_entry_label_);
  bool skip_overflow_check = IsLeafMethod()
      && !FrameNeedsStackCheck(GetFrameSize(), InstructionSet::kX86_64);
  DCHECK(GetCompilerOptions().GetImplicitStackOverflowChecks());


  if (!skip_overflow_check) {
    size_t reserved_bytes = GetStackOverflowReservedBytes(InstructionSet::kX86_64);
    __ testq(CpuRegister(RAX), Address(CpuRegister(RSP), -static_cast<int32_t>(reserved_bytes)));
    RecordPcInfoForFrameOrBlockEntry();
  }

  if (!HasEmptyFrame()) {
    // Make sure the frame size isn't unreasonably large.
    DCHECK_LE(GetFrameSize(), GetMaximumFrameSize());

    for (int i = arraysize(kCoreCalleeSaves) - 1; i >= 0; --i) {
      Register reg = kCoreCalleeSaves[i];
      if (allocated_registers_.ContainsCoreRegister(reg)) {
        __ pushq(CpuRegister(reg));
        __ cfi().AdjustCFAOffset(kX86_64WordSize);
        __ cfi().RelOffset(DWARFReg(reg), 0);
      }
    }

    int adjust = GetFrameSize() - GetCoreSpillSize();
    IncreaseFrame(adjust);
    uint32_t xmm_spill_location = GetFpuSpillStart();
    size_t xmm_spill_slot_size = GetCalleePreservedFPWidth();

    for (int i = arraysize(kFpuCalleeSaves) - 1; i >= 0; --i) {
      if (allocated_registers_.ContainsFloatingPointRegister(kFpuCalleeSaves[i])) {
        int offset = xmm_spill_location + (xmm_spill_slot_size * i);
        __ movsd(Address(CpuRegister(RSP), offset), XmmRegister(kFpuCalleeSaves[i]));
        __ cfi().RelOffset(DWARFReg(kFpuCalleeSaves[i]), offset);
      }
    }

    // Save the current method if we need it. Note that we do not
    // do this in HCurrentMethod, as the instruction might have been removed
    // in the SSA graph.
    if (RequiresCurrentMethod()) {
      CHECK(!HasEmptyFrame());
      __ movq(Address(CpuRegister(RSP), kCurrentMethodStackOffset),
              CpuRegister(kMethodRegisterArgument));
    }

    if (GetGraph()->HasShouldDeoptimizeFlag()) {
      CHECK(!HasEmptyFrame());
      // Initialize should_deoptimize flag to 0.
      __ movl(Address(CpuRegister(RSP), GetStackOffsetOfShouldDeoptimizeFlag()), Immediate(0));
    }
  }

  MaybeIncrementHotness(/* suspend_check= */ nullptr, /* is_frame_entry= */ true);
}

void CodeGeneratorX86_64::GenerateFrameExit() {
  __ cfi().RememberState();
  if (!HasEmptyFrame()) {
    uint32_t xmm_spill_location = GetFpuSpillStart();
    size_t xmm_spill_slot_size = GetCalleePreservedFPWidth();
    for (size_t i = 0; i < arraysize(kFpuCalleeSaves); ++i) {
      if (allocated_registers_.ContainsFloatingPointRegister(kFpuCalleeSaves[i])) {
        int offset = xmm_spill_location + (xmm_spill_slot_size * i);
        __ movsd(XmmRegister(kFpuCalleeSaves[i]), Address(CpuRegister(RSP), offset));
        __ cfi().Restore(DWARFReg(kFpuCalleeSaves[i]));
      }
    }

    int adjust = GetFrameSize() - GetCoreSpillSize();
    DecreaseFrame(adjust);

    for (size_t i = 0; i < arraysize(kCoreCalleeSaves); ++i) {
      Register reg = kCoreCalleeSaves[i];
      if (allocated_registers_.ContainsCoreRegister(reg)) {
        __ popq(CpuRegister(reg));
        __ cfi().AdjustCFAOffset(-static_cast<int>(kX86_64WordSize));
        __ cfi().Restore(DWARFReg(reg));
      }
    }
  }
  __ ret();
  __ cfi().RestoreState();
  __ cfi().DefCFAOffset(GetFrameSize());
}

void CodeGeneratorX86_64::Bind(HBasicBlock* block) {
  __ Bind(GetLabelOf(block));
}

void CodeGeneratorX86_64::Move(Location destination, Location source) {
  if (source.Equals(destination)) {
    return;
  }
  if (destination.IsRegister()) {
    CpuRegister dest = destination.AsRegister<CpuRegister>();
    if (source.IsRegister()) {
      __ movq(dest, source.AsRegister<CpuRegister>());
    } else if (source.IsFpuRegister()) {
      __ movq(dest, source.AsFpuRegister<XmmRegister>());
    } else if (source.IsStackSlot()) {
      __ movl(dest, Address(CpuRegister(RSP), source.GetStackIndex()));
    } else if (source.IsConstant()) {
      HConstant* constant = source.GetConstant();
      if (constant->IsLongConstant()) {
        Load64BitValue(dest, constant->AsLongConstant()->GetValue());
      } else if (constant->IsDoubleConstant()) {
        Load64BitValue(dest, GetInt64ValueOf(constant));
      } else {
        Load32BitValue(dest, GetInt32ValueOf(constant));
      }
    } else {
      DCHECK(source.IsDoubleStackSlot());
      __ movq(dest, Address(CpuRegister(RSP), source.GetStackIndex()));
    }
  } else if (destination.IsFpuRegister()) {
    XmmRegister dest = destination.AsFpuRegister<XmmRegister>();
    if (source.IsRegister()) {
      __ movq(dest, source.AsRegister<CpuRegister>());
    } else if (source.IsFpuRegister()) {
      __ movaps(dest, source.AsFpuRegister<XmmRegister>());
    } else if (source.IsConstant()) {
      HConstant* constant = source.GetConstant();
      int64_t value = CodeGenerator::GetInt64ValueOf(constant);
      if (constant->IsFloatConstant()) {
        Load32BitValue(dest, static_cast<int32_t>(value));
      } else {
        Load64BitValue(dest, value);
      }
    } else if (source.IsStackSlot()) {
      __ movss(dest, Address(CpuRegister(RSP), source.GetStackIndex()));
    } else {
      DCHECK(source.IsDoubleStackSlot());
      __ movsd(dest, Address(CpuRegister(RSP), source.GetStackIndex()));
    }
  } else if (destination.IsStackSlot()) {
    if (source.IsRegister()) {
      __ movl(Address(CpuRegister(RSP), destination.GetStackIndex()),
              source.AsRegister<CpuRegister>());
    } else if (source.IsFpuRegister()) {
      __ movss(Address(CpuRegister(RSP), destination.GetStackIndex()),
               source.AsFpuRegister<XmmRegister>());
    } else if (source.IsConstant()) {
      HConstant* constant = source.GetConstant();
      int32_t value = GetInt32ValueOf(constant);
      __ movl(Address(CpuRegister(RSP), destination.GetStackIndex()), Immediate(value));
    } else {
      DCHECK(source.IsStackSlot()) << source;
      __ movl(CpuRegister(TMP), Address(CpuRegister(RSP), source.GetStackIndex()));
      __ movl(Address(CpuRegister(RSP), destination.GetStackIndex()), CpuRegister(TMP));
    }
  } else {
    DCHECK(destination.IsDoubleStackSlot());
    if (source.IsRegister()) {
      __ movq(Address(CpuRegister(RSP), destination.GetStackIndex()),
              source.AsRegister<CpuRegister>());
    } else if (source.IsFpuRegister()) {
      __ movsd(Address(CpuRegister(RSP), destination.GetStackIndex()),
               source.AsFpuRegister<XmmRegister>());
    } else if (source.IsConstant()) {
      HConstant* constant = source.GetConstant();
      DCHECK(constant->IsLongConstant() || constant->IsDoubleConstant());
      int64_t value = GetInt64ValueOf(constant);
      Store64BitValueToStack(destination, value);
    } else {
      DCHECK(source.IsDoubleStackSlot());
      __ movq(CpuRegister(TMP), Address(CpuRegister(RSP), source.GetStackIndex()));
      __ movq(Address(CpuRegister(RSP), destination.GetStackIndex()), CpuRegister(TMP));
    }
  }
}

void CodeGeneratorX86_64::LoadFromMemoryNoReference(DataType::Type type,
                                                    Location dst,
                                                    Address src) {
  switch (type) {
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
      __ movzxb(dst.AsRegister<CpuRegister>(), src);
      break;
    case DataType::Type::kInt8:
      __ movsxb(dst.AsRegister<CpuRegister>(), src);
      break;
    case DataType::Type::kUint16:
      __ movzxw(dst.AsRegister<CpuRegister>(), src);
      break;
    case DataType::Type::kInt16:
      __ movsxw(dst.AsRegister<CpuRegister>(), src);
      break;
    case DataType::Type::kInt32:
    case DataType::Type::kUint32:
      __ movl(dst.AsRegister<CpuRegister>(), src);
      break;
    case DataType::Type::kInt64:
    case DataType::Type::kUint64:
      __ movq(dst.AsRegister<CpuRegister>(), src);
      break;
    case DataType::Type::kFloat32:
      __ movss(dst.AsFpuRegister<XmmRegister>(), src);
      break;
    case DataType::Type::kFloat64:
      __ movsd(dst.AsFpuRegister<XmmRegister>(), src);
      break;
    case DataType::Type::kVoid:
    case DataType::Type::kReference:
      LOG(FATAL) << "Unreachable type " << type;
      UNREACHABLE();
  }
}

void CodeGeneratorX86_64::MoveConstant(Location location, int32_t value) {
  DCHECK(location.IsRegister());
  Load64BitValue(location.AsRegister<CpuRegister>(), static_cast<int64_t>(value));
}

void CodeGeneratorX86_64::MoveLocation(Location dst,
                                       Location src,
                                       [[maybe_unused]] DataType::Type dst_type) {
  Move(dst, src);
}

void CodeGeneratorX86_64::AddLocationAsTemp(Location location, LocationSummary* locations) {
  if (location.IsRegister()) {
    locations->AddTemp(location);
  } else {
    UNIMPLEMENTED(FATAL) << "AddLocationAsTemp not implemented for location " << location;
  }
}

void InstructionCodeGeneratorX86_64::HandleGoto(HInstruction* got, HBasicBlock* successor) {
  if (successor->IsExitBlock()) {
    DCHECK(got->GetPrevious()->AlwaysThrows());
    return;  // no code needed
  }

  HBasicBlock* block = got->GetBlock();
  HInstruction* previous = got->GetPrevious();

  HLoopInformation* info = block->GetLoopInformation();
  if (info != nullptr && info->IsBackEdge(*block) && info->HasSuspendCheck()) {
    codegen_->MaybeIncrementHotness(info->GetSuspendCheck(), /* is_frame_entry= */ false);
    GenerateSuspendCheck(info->GetSuspendCheck(), successor);
    return;
  }

  if (block->IsEntryBlock() && (previous != nullptr) && previous->IsSuspendCheck()) {
    GenerateSuspendCheck(previous->AsSuspendCheck(), nullptr);
  }
  if (!codegen_->GoesToNextBlock(got->GetBlock(), successor)) {
    __ jmp(codegen_->GetLabelOf(successor));
  }
}

void LocationsBuilderX86_64::VisitGoto(HGoto* got) {
  got->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86_64::VisitGoto(HGoto* got) {
  HandleGoto(got, got->GetSuccessor());
}

void LocationsBuilderX86_64::VisitTryBoundary(HTryBoundary* try_boundary) {
  try_boundary->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86_64::VisitTryBoundary(HTryBoundary* try_boundary) {
  HBasicBlock* successor = try_boundary->GetNormalFlowSuccessor();
  if (!successor->IsExitBlock()) {
    HandleGoto(try_boundary, successor);
  }
}

void LocationsBuilderX86_64::VisitExit(HExit* exit) {
  exit->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86_64::VisitExit([[maybe_unused]] HExit* exit) {}

template<class LabelType>
void InstructionCodeGeneratorX86_64::GenerateFPJumps(HCondition* cond,
                                                     LabelType* true_label,
                                                     LabelType* false_label) {
  if (cond->IsFPConditionTrueIfNaN()) {
    __ j(kUnordered, true_label);
  } else if (cond->IsFPConditionFalseIfNaN()) {
    __ j(kUnordered, false_label);
  }
  __ j(X86_64FPCondition(cond->GetCondition()), true_label);
}

void InstructionCodeGeneratorX86_64::GenerateCompareTest(HCondition* condition) {
  LocationSummary* locations = condition->GetLocations();

  Location left = locations->InAt(0);
  Location right = locations->InAt(1);
  DataType::Type type = condition->InputAt(0)->GetType();
  switch (type) {
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
    case DataType::Type::kReference: {
      codegen_->GenerateIntCompare(left, right);
      break;
    }
    case DataType::Type::kInt64: {
      codegen_->GenerateLongCompare(left, right);
      break;
    }
    case DataType::Type::kFloat32: {
      if (right.IsFpuRegister()) {
        __ ucomiss(left.AsFpuRegister<XmmRegister>(), right.AsFpuRegister<XmmRegister>());
      } else if (right.IsConstant()) {
        __ ucomiss(left.AsFpuRegister<XmmRegister>(),
                   codegen_->LiteralFloatAddress(
                       right.GetConstant()->AsFloatConstant()->GetValue()));
      } else {
        DCHECK(right.IsStackSlot());
        __ ucomiss(left.AsFpuRegister<XmmRegister>(),
                   Address(CpuRegister(RSP), right.GetStackIndex()));
      }
      break;
    }
    case DataType::Type::kFloat64: {
      if (right.IsFpuRegister()) {
        __ ucomisd(left.AsFpuRegister<XmmRegister>(), right.AsFpuRegister<XmmRegister>());
      } else if (right.IsConstant()) {
        __ ucomisd(left.AsFpuRegister<XmmRegister>(),
                   codegen_->LiteralDoubleAddress(
                       right.GetConstant()->AsDoubleConstant()->GetValue()));
      } else {
        DCHECK(right.IsDoubleStackSlot());
        __ ucomisd(left.AsFpuRegister<XmmRegister>(),
                   Address(CpuRegister(RSP), right.GetStackIndex()));
      }
      break;
    }
    default:
      LOG(FATAL) << "Unexpected condition type " << type;
  }
}

template<class LabelType>
void InstructionCodeGeneratorX86_64::GenerateCompareTestAndBranch(HCondition* condition,
                                                                  LabelType* true_target_in,
                                                                  LabelType* false_target_in) {
  // Generated branching requires both targets to be explicit. If either of the
  // targets is nullptr (fallthrough) use and bind `fallthrough_target` instead.
  LabelType fallthrough_target;
  LabelType* true_target = true_target_in == nullptr ? &fallthrough_target : true_target_in;
  LabelType* false_target = false_target_in == nullptr ? &fallthrough_target : false_target_in;

  // Generate the comparison to set the CC.
  GenerateCompareTest(condition);

  // Now generate the correct jump(s).
  DataType::Type type = condition->InputAt(0)->GetType();
  switch (type) {
    case DataType::Type::kInt64: {
      __ j(X86_64IntegerCondition(condition->GetCondition()), true_target);
      break;
    }
    case DataType::Type::kFloat32: {
      GenerateFPJumps(condition, true_target, false_target);
      break;
    }
    case DataType::Type::kFloat64: {
      GenerateFPJumps(condition, true_target, false_target);
      break;
    }
    default:
      LOG(FATAL) << "Unexpected condition type " << type;
  }

  if (false_target != &fallthrough_target) {
    __ jmp(false_target);
  }

  if (fallthrough_target.IsLinked()) {
    __ Bind(&fallthrough_target);
  }
}

static bool AreEflagsSetFrom(HInstruction* cond,
                             HInstruction* branch,
                             const CompilerOptions& compiler_options) {
  // Moves may affect the eflags register (move zero uses xorl), so the EFLAGS
  // are set only strictly before `branch`. We can't use the eflags on long
  // conditions if they are materialized due to the complex branching.
  return cond->IsCondition() &&
         cond->GetNext() == branch &&
         !DataType::IsFloatingPointType(cond->InputAt(0)->GetType()) &&
         !(cond->GetBlock()->GetGraph()->IsCompilingBaseline() &&
           compiler_options.ProfileBranches());
}

template<class LabelType>
void InstructionCodeGeneratorX86_64::GenerateTestAndBranch(HInstruction* instruction,
                                                           size_t condition_input_index,
                                                           LabelType* true_target,
                                                           LabelType* false_target) {
  HInstruction* cond = instruction->InputAt(condition_input_index);

  if (true_target == nullptr && false_target == nullptr) {
    // Nothing to do. The code always falls through.
    return;
  } else if (cond->IsIntConstant()) {
    // Constant condition, statically compared against "true" (integer value 1).
    if (cond->AsIntConstant()->IsTrue()) {
      if (true_target != nullptr) {
        __ jmp(true_target);
      }
    } else {
      DCHECK(cond->AsIntConstant()->IsFalse()) << cond->AsIntConstant()->GetValue();
      if (false_target != nullptr) {
        __ jmp(false_target);
      }
    }
    return;
  }

  // The following code generates these patterns:
  //  (1) true_target == nullptr && false_target != nullptr
  //        - opposite condition true => branch to false_target
  //  (2) true_target != nullptr && false_target == nullptr
  //        - condition true => branch to true_target
  //  (3) true_target != nullptr && false_target != nullptr
  //        - condition true => branch to true_target
  //        - branch to false_target
  if (IsBooleanValueOrMaterializedCondition(cond)) {
    if (AreEflagsSetFrom(cond, instruction, codegen_->GetCompilerOptions())) {
      if (true_target == nullptr) {
        __ j(X86_64IntegerCondition(cond->AsCondition()->GetOppositeCondition()), false_target);
      } else {
        __ j(X86_64IntegerCondition(cond->AsCondition()->GetCondition()), true_target);
      }
    } else {
      // Materialized condition, compare against 0.
      Location lhs = instruction->GetLocations()->InAt(condition_input_index);
      if (lhs.IsRegister()) {
        __ testl(lhs.AsRegister<CpuRegister>(), lhs.AsRegister<CpuRegister>());
      } else {
        __ cmpl(Address(CpuRegister(RSP), lhs.GetStackIndex()), Immediate(0));
      }
      if (true_target == nullptr) {
        __ j(kEqual, false_target);
      } else {
        __ j(kNotEqual, true_target);
      }
    }
  } else {
    // Condition has not been materialized, use its inputs as the
    // comparison and its condition as the branch condition.
    HCondition* condition = cond->AsCondition();

    // If this is a long or FP comparison that has been folded into
    // the HCondition, generate the comparison directly.
    DataType::Type type = condition->InputAt(0)->GetType();
    if (type == DataType::Type::kInt64 || DataType::IsFloatingPointType(type)) {
      GenerateCompareTestAndBranch(condition, true_target, false_target);
      return;
    }

    Location lhs = condition->GetLocations()->InAt(0);
    Location rhs = condition->GetLocations()->InAt(1);
    codegen_->GenerateIntCompare(lhs, rhs);
      if (true_target == nullptr) {
      __ j(X86_64IntegerCondition(condition->GetOppositeCondition()), false_target);
    } else {
      __ j(X86_64IntegerCondition(condition->GetCondition()), true_target);
    }
  }

  // If neither branch falls through (case 3), the conditional branch to `true_target`
  // was already emitted (case 2) and we need to emit a jump to `false_target`.
  if (true_target != nullptr && false_target != nullptr) {
    __ jmp(false_target);
  }
}

void LocationsBuilderX86_64::VisitIf(HIf* if_instr) {
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(if_instr);
  if (IsBooleanValueOrMaterializedCondition(if_instr->InputAt(0))) {
    if (GetGraph()->IsCompilingBaseline() &&
        codegen_->GetCompilerOptions().ProfileBranches() &&
        !Runtime::Current()->IsAotCompiler()) {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->AddTemp(Location::RequiresRegister());
    } else {
      locations->SetInAt(0, Location::Any());
    }
  }
}

void InstructionCodeGeneratorX86_64::VisitIf(HIf* if_instr) {
  HBasicBlock* true_successor = if_instr->IfTrueSuccessor();
  HBasicBlock* false_successor = if_instr->IfFalseSuccessor();
  Label* true_target = codegen_->GoesToNextBlock(if_instr->GetBlock(), true_successor) ?
      nullptr : codegen_->GetLabelOf(true_successor);
  Label* false_target = codegen_->GoesToNextBlock(if_instr->GetBlock(), false_successor) ?
      nullptr : codegen_->GetLabelOf(false_successor);
  if (IsBooleanValueOrMaterializedCondition(if_instr->InputAt(0))) {
    if (GetGraph()->IsCompilingBaseline() &&
        codegen_->GetCompilerOptions().ProfileBranches() &&
        !Runtime::Current()->IsAotCompiler()) {
      DCHECK(if_instr->InputAt(0)->IsCondition());
      CpuRegister temp = if_instr->GetLocations()->GetTemp(0).AsRegister<CpuRegister>();
      ProfilingInfo* info = GetGraph()->GetProfilingInfo();
      DCHECK(info != nullptr);
      BranchCache* cache = info->GetBranchCache(if_instr->GetDexPc());
      // Currently, not all If branches are profiled.
      if (cache != nullptr) {
        uint64_t address =
            reinterpret_cast64<uint64_t>(cache) + BranchCache::FalseOffset().Int32Value();
        static_assert(
            BranchCache::TrueOffset().Int32Value() - BranchCache::FalseOffset().Int32Value() == 2,
            "Unexpected offsets for BranchCache");
        NearLabel done;
        Location lhs = if_instr->GetLocations()->InAt(0);
        __ movq(CpuRegister(TMP), Immediate(address));
        __ movzxw(temp, Address(CpuRegister(TMP), lhs.AsRegister<CpuRegister>(), TIMES_2, 0));
        __ addw(temp, Immediate(1));
        __ j(kZero, &done);
        __ movw(Address(CpuRegister(TMP), lhs.AsRegister<CpuRegister>(), TIMES_2, 0), temp);
        __ Bind(&done);
      }
    }
  }
  GenerateTestAndBranch(if_instr, /* condition_input_index= */ 0, true_target, false_target);
}

void LocationsBuilderX86_64::VisitDeoptimize(HDeoptimize* deoptimize) {
  LocationSummary* locations = new (GetGraph()->GetAllocator())
      LocationSummary(deoptimize, LocationSummary::kCallOnSlowPath);
  InvokeRuntimeCallingConvention calling_convention;
  RegisterSet caller_saves = RegisterSet::Empty();
  caller_saves.Add(Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->SetCustomSlowPathCallerSaves(caller_saves);
  if (IsBooleanValueOrMaterializedCondition(deoptimize->InputAt(0))) {
    locations->SetInAt(0, Location::Any());
  }
}

void InstructionCodeGeneratorX86_64::VisitDeoptimize(HDeoptimize* deoptimize) {
  SlowPathCode* slow_path = deopt_slow_paths_.NewSlowPath<DeoptimizationSlowPathX86_64>(deoptimize);
  GenerateTestAndBranch<Label>(deoptimize,
                               /* condition_input_index= */ 0,
                               slow_path->GetEntryLabel(),
                               /* false_target= */ nullptr);
}

void LocationsBuilderX86_64::VisitShouldDeoptimizeFlag(HShouldDeoptimizeFlag* flag) {
  LocationSummary* locations = new (GetGraph()->GetAllocator())
      LocationSummary(flag, LocationSummary::kNoCall);
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorX86_64::VisitShouldDeoptimizeFlag(HShouldDeoptimizeFlag* flag) {
  __ movl(flag->GetLocations()->Out().AsRegister<CpuRegister>(),
          Address(CpuRegister(RSP), codegen_->GetStackOffsetOfShouldDeoptimizeFlag()));
}

static bool SelectCanUseCMOV(HSelect* select) {
  // There are no conditional move instructions for XMMs.
  if (DataType::IsFloatingPointType(select->GetType())) {
    return false;
  }

  // A FP condition doesn't generate the single CC that we need.
  HInstruction* condition = select->GetCondition();
  if (condition->IsCondition() &&
      DataType::IsFloatingPointType(condition->InputAt(0)->GetType())) {
    return false;
  }

  // We can generate a CMOV for this Select.
  return true;
}

void LocationsBuilderX86_64::VisitSelect(HSelect* select) {
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(select);
  if (DataType::IsFloatingPointType(select->GetType())) {
    locations->SetInAt(0, Location::RequiresFpuRegister());
    locations->SetInAt(1, Location::Any());
  } else {
    locations->SetInAt(0, Location::RequiresRegister());
    if (SelectCanUseCMOV(select)) {
      if (select->InputAt(1)->IsConstant()) {
        locations->SetInAt(1, Location::RequiresRegister());
      } else {
        locations->SetInAt(1, Location::Any());
      }
    } else {
      locations->SetInAt(1, Location::Any());
    }
  }
  if (IsBooleanValueOrMaterializedCondition(select->GetCondition())) {
    locations->SetInAt(2, Location::RequiresRegister());
  }
  locations->SetOut(Location::SameAsFirstInput());
}

void InstructionCodeGeneratorX86_64::VisitSelect(HSelect* select) {
  LocationSummary* locations = select->GetLocations();
  if (SelectCanUseCMOV(select)) {
    // If both the condition and the source types are integer, we can generate
    // a CMOV to implement Select.
    CpuRegister value_false = locations->InAt(0).AsRegister<CpuRegister>();
    Location value_true_loc = locations->InAt(1);
    DCHECK(locations->InAt(0).Equals(locations->Out()));

    HInstruction* select_condition = select->GetCondition();
    Condition cond = kNotEqual;

    // Figure out how to test the 'condition'.
    if (select_condition->IsCondition()) {
      HCondition* condition = select_condition->AsCondition();
      if (!condition->IsEmittedAtUseSite()) {
        // This was a previously materialized condition.
        // Can we use the existing condition code?
        if (AreEflagsSetFrom(condition, select, codegen_->GetCompilerOptions())) {
          // Materialization was the previous instruction.  Condition codes are right.
          cond = X86_64IntegerCondition(condition->GetCondition());
        } else {
          // No, we have to recreate the condition code.
          CpuRegister cond_reg = locations->InAt(2).AsRegister<CpuRegister>();
          __ testl(cond_reg, cond_reg);
        }
      } else {
        GenerateCompareTest(condition);
        cond = X86_64IntegerCondition(condition->GetCondition());
      }
    } else {
      // Must be a Boolean condition, which needs to be compared to 0.
      CpuRegister cond_reg = locations->InAt(2).AsRegister<CpuRegister>();
      __ testl(cond_reg, cond_reg);
    }

    // If the condition is true, overwrite the output, which already contains false.
    // Generate the correct sized CMOV.
    bool is_64_bit = DataType::Is64BitType(select->GetType());
    if (value_true_loc.IsRegister()) {
      __ cmov(cond, value_false, value_true_loc.AsRegister<CpuRegister>(), is_64_bit);
    } else {
      __ cmov(cond,
              value_false,
              Address(CpuRegister(RSP), value_true_loc.GetStackIndex()), is_64_bit);
    }
  } else {
    NearLabel false_target;
    GenerateTestAndBranch<NearLabel>(select,
                                     /* condition_input_index= */ 2,
                                     /* true_target= */ nullptr,
                                     &false_target);
    codegen_->MoveLocation(locations->Out(), locations->InAt(1), select->GetType());
    __ Bind(&false_target);
  }
}

void LocationsBuilderX86_64::VisitNop(HNop* nop) {
  new (GetGraph()->GetAllocator()) LocationSummary(nop);
}

void InstructionCodeGeneratorX86_64::VisitNop(HNop*) {
  // The environment recording already happened in CodeGenerator::Compile.
}

void CodeGeneratorX86_64::IncreaseFrame(size_t adjustment) {
  __ subq(CpuRegister(RSP), Immediate(adjustment));
  __ cfi().AdjustCFAOffset(adjustment);
}

void CodeGeneratorX86_64::DecreaseFrame(size_t adjustment) {
  __ addq(CpuRegister(RSP), Immediate(adjustment));
  __ cfi().AdjustCFAOffset(-adjustment);
}

void CodeGeneratorX86_64::GenerateNop() {
  __ nop();
}

void LocationsBuilderX86_64::HandleCondition(HCondition* cond) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(cond, LocationSummary::kNoCall);
  // Handle the long/FP comparisons made in instruction simplification.
  switch (cond->InputAt(0)->GetType()) {
    case DataType::Type::kInt64:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::Any());
      break;
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::Any());
      break;
    default:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::Any());
      break;
  }
  if (!cond->IsEmittedAtUseSite()) {
    locations->SetOut(Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorX86_64::HandleCondition(HCondition* cond) {
  if (cond->IsEmittedAtUseSite()) {
    return;
  }

  LocationSummary* locations = cond->GetLocations();
  Location lhs = locations->InAt(0);
  Location rhs = locations->InAt(1);
  CpuRegister reg = locations->Out().AsRegister<CpuRegister>();
  NearLabel true_label, false_label;

  switch (cond->InputAt(0)->GetType()) {
    default:
      // Integer case.

      // Clear output register: setcc only sets the low byte.
      __ xorl(reg, reg);

      codegen_->GenerateIntCompare(lhs, rhs);
      __ setcc(X86_64IntegerCondition(cond->GetCondition()), reg);
      return;
    case DataType::Type::kInt64:
      // Clear output register: setcc only sets the low byte.
      __ xorl(reg, reg);

      codegen_->GenerateLongCompare(lhs, rhs);
      __ setcc(X86_64IntegerCondition(cond->GetCondition()), reg);
      return;
    case DataType::Type::kFloat32: {
      XmmRegister lhs_reg = lhs.AsFpuRegister<XmmRegister>();
      if (rhs.IsConstant()) {
        float value = rhs.GetConstant()->AsFloatConstant()->GetValue();
        __ ucomiss(lhs_reg, codegen_->LiteralFloatAddress(value));
      } else if (rhs.IsStackSlot()) {
        __ ucomiss(lhs_reg, Address(CpuRegister(RSP), rhs.GetStackIndex()));
      } else {
        __ ucomiss(lhs_reg, rhs.AsFpuRegister<XmmRegister>());
      }
      GenerateFPJumps(cond, &true_label, &false_label);
      break;
    }
    case DataType::Type::kFloat64: {
      XmmRegister lhs_reg = lhs.AsFpuRegister<XmmRegister>();
      if (rhs.IsConstant()) {
        double value = rhs.GetConstant()->AsDoubleConstant()->GetValue();
        __ ucomisd(lhs_reg, codegen_->LiteralDoubleAddress(value));
      } else if (rhs.IsDoubleStackSlot()) {
        __ ucomisd(lhs_reg, Address(CpuRegister(RSP), rhs.GetStackIndex()));
      } else {
        __ ucomisd(lhs_reg, rhs.AsFpuRegister<XmmRegister>());
      }
      GenerateFPJumps(cond, &true_label, &false_label);
      break;
    }
  }

  // Convert the jumps into the result.
  NearLabel done_label;

  // False case: result = 0.
  __ Bind(&false_label);
  __ xorl(reg, reg);
  __ jmp(&done_label);

  // True case: result = 1.
  __ Bind(&true_label);
  __ movl(reg, Immediate(1));
  __ Bind(&done_label);
}

void LocationsBuilderX86_64::VisitEqual(HEqual* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorX86_64::VisitEqual(HEqual* comp) {
  HandleCondition(comp);
}

void LocationsBuilderX86_64::VisitNotEqual(HNotEqual* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorX86_64::VisitNotEqual(HNotEqual* comp) {
  HandleCondition(comp);
}

void LocationsBuilderX86_64::VisitLessThan(HLessThan* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorX86_64::VisitLessThan(HLessThan* comp) {
  HandleCondition(comp);
}

void LocationsBuilderX86_64::VisitLessThanOrEqual(HLessThanOrEqual* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorX86_64::VisitLessThanOrEqual(HLessThanOrEqual* comp) {
  HandleCondition(comp);
}

void LocationsBuilderX86_64::VisitGreaterThan(HGreaterThan* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorX86_64::VisitGreaterThan(HGreaterThan* comp) {
  HandleCondition(comp);
}

void LocationsBuilderX86_64::VisitGreaterThanOrEqual(HGreaterThanOrEqual* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorX86_64::VisitGreaterThanOrEqual(HGreaterThanOrEqual* comp) {
  HandleCondition(comp);
}

void LocationsBuilderX86_64::VisitBelow(HBelow* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorX86_64::VisitBelow(HBelow* comp) {
  HandleCondition(comp);
}

void LocationsBuilderX86_64::VisitBelowOrEqual(HBelowOrEqual* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorX86_64::VisitBelowOrEqual(HBelowOrEqual* comp) {
  HandleCondition(comp);
}

void LocationsBuilderX86_64::VisitAbove(HAbove* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorX86_64::VisitAbove(HAbove* comp) {
  HandleCondition(comp);
}

void LocationsBuilderX86_64::VisitAboveOrEqual(HAboveOrEqual* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorX86_64::VisitAboveOrEqual(HAboveOrEqual* comp) {
  HandleCondition(comp);
}

void LocationsBuilderX86_64::VisitCompare(HCompare* compare) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(compare, LocationSummary::kNoCall);
  switch (compare->GetComparisonType()) {
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
    case DataType::Type::kUint32:
    case DataType::Type::kInt64:
    case DataType::Type::kUint64: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::Any());
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;
    }
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64: {
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::Any());
      locations->SetOut(Location::RequiresRegister());
      break;
    }
    default:
      LOG(FATAL) << "Unexpected type for compare operation " << compare->InputAt(0)->GetType();
  }
}

void InstructionCodeGeneratorX86_64::VisitCompare(HCompare* compare) {
  LocationSummary* locations = compare->GetLocations();
  CpuRegister out = locations->Out().AsRegister<CpuRegister>();
  Location left = locations->InAt(0);
  Location right = locations->InAt(1);

  NearLabel less, greater, done;
  DataType::Type type = compare->GetComparisonType();
  Condition less_cond = kLess;

  switch (type) {
    case DataType::Type::kUint32:
      less_cond = kBelow;
      FALLTHROUGH_INTENDED;
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32: {
      codegen_->GenerateIntCompare(left, right);
      break;
    }
    case DataType::Type::kUint64:
      less_cond = kBelow;
      FALLTHROUGH_INTENDED;
    case DataType::Type::kInt64: {
      codegen_->GenerateLongCompare(left, right);
      break;
    }
    case DataType::Type::kFloat32: {
      XmmRegister left_reg = left.AsFpuRegister<XmmRegister>();
      if (right.IsConstant()) {
        float value = right.GetConstant()->AsFloatConstant()->GetValue();
        __ ucomiss(left_reg, codegen_->LiteralFloatAddress(value));
      } else if (right.IsStackSlot()) {
        __ ucomiss(left_reg, Address(CpuRegister(RSP), right.GetStackIndex()));
      } else {
        __ ucomiss(left_reg, right.AsFpuRegister<XmmRegister>());
      }
      __ j(kUnordered, compare->IsGtBias() ? &greater : &less);
      less_cond = kBelow;  //  ucomis{s,d} sets CF
      break;
    }
    case DataType::Type::kFloat64: {
      XmmRegister left_reg = left.AsFpuRegister<XmmRegister>();
      if (right.IsConstant()) {
        double value = right.GetConstant()->AsDoubleConstant()->GetValue();
        __ ucomisd(left_reg, codegen_->LiteralDoubleAddress(value));
      } else if (right.IsDoubleStackSlot()) {
        __ ucomisd(left_reg, Address(CpuRegister(RSP), right.GetStackIndex()));
      } else {
        __ ucomisd(left_reg, right.AsFpuRegister<XmmRegister>());
      }
      __ j(kUnordered, compare->IsGtBias() ? &greater : &less);
      less_cond = kBelow;  //  ucomis{s,d} sets CF
      break;
    }
    default:
      LOG(FATAL) << "Unexpected compare type " << type;
  }

  __ movl(out, Immediate(0));
  __ j(kEqual, &done);
  __ j(less_cond, &less);

  __ Bind(&greater);
  __ movl(out, Immediate(1));
  __ jmp(&done);

  __ Bind(&less);
  __ movl(out, Immediate(-1));

  __ Bind(&done);
}

void LocationsBuilderX86_64::VisitIntConstant(HIntConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorX86_64::VisitIntConstant([[maybe_unused]] HIntConstant* constant) {
  // Will be generated at use site.
}

void LocationsBuilderX86_64::VisitNullConstant(HNullConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorX86_64::VisitNullConstant([[maybe_unused]] HNullConstant* constant) {
  // Will be generated at use site.
}

void LocationsBuilderX86_64::VisitLongConstant(HLongConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorX86_64::VisitLongConstant([[maybe_unused]] HLongConstant* constant) {
  // Will be generated at use site.
}

void LocationsBuilderX86_64::VisitFloatConstant(HFloatConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorX86_64::VisitFloatConstant([[maybe_unused]] HFloatConstant* constant) {
  // Will be generated at use site.
}

void LocationsBuilderX86_64::VisitDoubleConstant(HDoubleConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorX86_64::VisitDoubleConstant(
    [[maybe_unused]] HDoubleConstant* constant) {
  // Will be generated at use site.
}

void LocationsBuilderX86_64::VisitConstructorFence(HConstructorFence* constructor_fence) {
  constructor_fence->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86_64::VisitConstructorFence(
    [[maybe_unused]] HConstructorFence* constructor_fence) {
  codegen_->GenerateMemoryBarrier(MemBarrierKind::kStoreStore);
}

void LocationsBuilderX86_64::VisitMemoryBarrier(HMemoryBarrier* memory_barrier) {
  memory_barrier->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86_64::VisitMemoryBarrier(HMemoryBarrier* memory_barrier) {
  codegen_->GenerateMemoryBarrier(memory_barrier->GetBarrierKind());
}

void LocationsBuilderX86_64::VisitReturnVoid(HReturnVoid* ret) {
  ret->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86_64::VisitReturnVoid([[maybe_unused]] HReturnVoid* ret) {
  codegen_->GenerateFrameExit();
}

void LocationsBuilderX86_64::VisitReturn(HReturn* ret) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(ret, LocationSummary::kNoCall);
  SetInForReturnValue(ret, locations);
}

void InstructionCodeGeneratorX86_64::VisitReturn(HReturn* ret) {
  switch (ret->InputAt(0)->GetType()) {
    case DataType::Type::kReference:
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      DCHECK_EQ(ret->GetLocations()->InAt(0).AsRegister<CpuRegister>().AsRegister(), RAX);
      break;

    case DataType::Type::kFloat32: {
      DCHECK_EQ(ret->GetLocations()->InAt(0).AsFpuRegister<XmmRegister>().AsFloatRegister(),
                XMM0);
      // To simplify callers of an OSR method, we put the return value in both
      // floating point and core register.
      if (GetGraph()->IsCompilingOsr()) {
        __ movd(CpuRegister(RAX), XmmRegister(XMM0));
      }
      break;
    }
    case DataType::Type::kFloat64: {
      DCHECK_EQ(ret->GetLocations()->InAt(0).AsFpuRegister<XmmRegister>().AsFloatRegister(),
                XMM0);
      // To simplify callers of an OSR method, we put the return value in both
      // floating point and core register.
      if (GetGraph()->IsCompilingOsr()) {
        __ movq(CpuRegister(RAX), XmmRegister(XMM0));
      }
      break;
    }

    default:
      LOG(FATAL) << "Unexpected return type " << ret->InputAt(0)->GetType();
  }
  codegen_->GenerateFrameExit();
}

Location InvokeDexCallingConventionVisitorX86_64::GetReturnLocation(DataType::Type type) const {
  switch (type) {
    case DataType::Type::kReference:
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kUint32:
    case DataType::Type::kInt32:
    case DataType::Type::kUint64:
    case DataType::Type::kInt64:
      return Location::RegisterLocation(RAX);

    case DataType::Type::kVoid:
      return Location::NoLocation();

    case DataType::Type::kFloat64:
    case DataType::Type::kFloat32:
      return Location::FpuRegisterLocation(XMM0);
  }
}

Location InvokeDexCallingConventionVisitorX86_64::GetMethodLocation() const {
  return Location::RegisterLocation(kMethodRegisterArgument);
}

Location InvokeDexCallingConventionVisitorX86_64::GetNextLocation(DataType::Type type) {
  switch (type) {
    case DataType::Type::kReference:
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32: {
      uint32_t index = gp_index_++;
      stack_index_++;
      if (index < calling_convention.GetNumberOfRegisters()) {
        return Location::RegisterLocation(calling_convention.GetRegisterAt(index));
      } else {
        return Location::StackSlot(calling_convention.GetStackOffsetOf(stack_index_ - 1));
      }
    }

    case DataType::Type::kInt64: {
      uint32_t index = gp_index_;
      stack_index_ += 2;
      if (index < calling_convention.GetNumberOfRegisters()) {
        gp_index_ += 1;
        return Location::RegisterLocation(calling_convention.GetRegisterAt(index));
      } else {
        gp_index_ += 2;
        return Location::DoubleStackSlot(calling_convention.GetStackOffsetOf(stack_index_ - 2));
      }
    }

    case DataType::Type::kFloat32: {
      uint32_t index = float_index_++;
      stack_index_++;
      if (index < calling_convention.GetNumberOfFpuRegisters()) {
        return Location::FpuRegisterLocation(calling_convention.GetFpuRegisterAt(index));
      } else {
        return Location::StackSlot(calling_convention.GetStackOffsetOf(stack_index_ - 1));
      }
    }

    case DataType::Type::kFloat64: {
      uint32_t index = float_index_++;
      stack_index_ += 2;
      if (index < calling_convention.GetNumberOfFpuRegisters()) {
        return Location::FpuRegisterLocation(calling_convention.GetFpuRegisterAt(index));
      } else {
        return Location::DoubleStackSlot(calling_convention.GetStackOffsetOf(stack_index_ - 2));
      }
    }

    case DataType::Type::kUint32:
    case DataType::Type::kUint64:
    case DataType::Type::kVoid:
      LOG(FATAL) << "Unexpected parameter type " << type;
      UNREACHABLE();
  }
  return Location::NoLocation();
}

Location CriticalNativeCallingConventionVisitorX86_64::GetNextLocation(DataType::Type type) {
  DCHECK_NE(type, DataType::Type::kReference);

  Location location = Location::NoLocation();
  if (DataType::IsFloatingPointType(type)) {
    if (fpr_index_ < kParameterFloatRegistersLength) {
      location = Location::FpuRegisterLocation(kParameterFloatRegisters[fpr_index_]);
      ++fpr_index_;
    }
  } else {
    // Native ABI uses the same registers as managed, except that the method register RDI
    // is a normal argument.
    if (gpr_index_ < 1u + kParameterCoreRegistersLength) {
      location = Location::RegisterLocation(
          gpr_index_ == 0u ? RDI : kParameterCoreRegisters[gpr_index_ - 1u]);
      ++gpr_index_;
    }
  }
  if (location.IsInvalid()) {
    if (DataType::Is64BitType(type)) {
      location = Location::DoubleStackSlot(stack_offset_);
    } else {
      location = Location::StackSlot(stack_offset_);
    }
    stack_offset_ += kFramePointerSize;

    if (for_register_allocation_) {
      location = Location::Any();
    }
  }
  return location;
}

Location CriticalNativeCallingConventionVisitorX86_64::GetReturnLocation(DataType::Type type)
    const {
  // We perform conversion to the managed ABI return register after the call if needed.
  InvokeDexCallingConventionVisitorX86_64 dex_calling_convention;
  return dex_calling_convention.GetReturnLocation(type);
}

Location CriticalNativeCallingConventionVisitorX86_64::GetMethodLocation() const {
  // Pass the method in the hidden argument RAX.
  return Location::RegisterLocation(RAX);
}

void LocationsBuilderX86_64::VisitInvokeUnresolved(HInvokeUnresolved* invoke) {
  // The trampoline uses the same calling convention as dex calling conventions,
  // except instead of loading arg0/r0 with the target Method*, arg0/r0 will contain
  // the method_idx.
  HandleInvoke(invoke);
}

void InstructionCodeGeneratorX86_64::VisitInvokeUnresolved(HInvokeUnresolved* invoke) {
  codegen_->GenerateInvokeUnresolvedRuntimeCall(invoke);
}

void LocationsBuilderX86_64::VisitInvokeStaticOrDirect(HInvokeStaticOrDirect* invoke) {
  // Explicit clinit checks triggered by static invokes must have been pruned by
  // art::PrepareForRegisterAllocation.
  DCHECK(!invoke->IsStaticWithExplicitClinitCheck());

  IntrinsicLocationsBuilderX86_64 intrinsic(codegen_);
  if (intrinsic.TryDispatch(invoke)) {
    return;
  }

  if (invoke->GetCodePtrLocation() == CodePtrLocation::kCallCriticalNative) {
    CriticalNativeCallingConventionVisitorX86_64 calling_convention_visitor(
        /*for_register_allocation=*/ true);
    CodeGenerator::CreateCommonInvokeLocationSummary(invoke, &calling_convention_visitor);
    CodeGeneratorX86_64::BlockNonVolatileXmmRegisters(invoke->GetLocations());
  } else {
    HandleInvoke(invoke);
  }
}

static bool TryGenerateIntrinsicCode(HInvoke* invoke, CodeGeneratorX86_64* codegen) {
  if (invoke->GetLocations()->Intrinsified()) {
    IntrinsicCodeGeneratorX86_64 intrinsic(codegen);
    intrinsic.Dispatch(invoke);
    return true;
  }
  return false;
}

void InstructionCodeGeneratorX86_64::VisitInvokeStaticOrDirect(HInvokeStaticOrDirect* invoke) {
  // Explicit clinit checks triggered by static invokes must have been pruned by
  // art::PrepareForRegisterAllocation.
  DCHECK(!invoke->IsStaticWithExplicitClinitCheck());

  if (TryGenerateIntrinsicCode(invoke, codegen_)) {
    return;
  }

  LocationSummary* locations = invoke->GetLocations();
  codegen_->GenerateStaticOrDirectCall(
      invoke, locations->HasTemps() ? locations->GetTemp(0) : Location::NoLocation());
}

void LocationsBuilderX86_64::HandleInvoke(HInvoke* invoke) {
  InvokeDexCallingConventionVisitorX86_64 calling_convention_visitor;
  CodeGenerator::CreateCommonInvokeLocationSummary(invoke, &calling_convention_visitor);
}

void LocationsBuilderX86_64::VisitInvokeVirtual(HInvokeVirtual* invoke) {
  IntrinsicLocationsBuilderX86_64 intrinsic(codegen_);
  if (intrinsic.TryDispatch(invoke)) {
    return;
  }

  HandleInvoke(invoke);
}

void InstructionCodeGeneratorX86_64::VisitInvokeVirtual(HInvokeVirtual* invoke) {
  if (TryGenerateIntrinsicCode(invoke, codegen_)) {
    return;
  }

  codegen_->GenerateVirtualCall(invoke, invoke->GetLocations()->GetTemp(0));
  DCHECK(!codegen_->IsLeafMethod());
}

void LocationsBuilderX86_64::VisitInvokeInterface(HInvokeInterface* invoke) {
  HandleInvoke(invoke);
  // Add the hidden argument.
  if (invoke->GetHiddenArgumentLoadKind() == MethodLoadKind::kRecursive) {
    invoke->GetLocations()->SetInAt(invoke->GetNumberOfArguments() - 1,
                                    Location::RegisterLocation(RAX));
  }
  invoke->GetLocations()->AddTemp(Location::RegisterLocation(RAX));
}

void CodeGeneratorX86_64::MaybeGenerateInlineCacheCheck(HInstruction* instruction,
                                                        CpuRegister klass) {
  DCHECK_EQ(RDI, klass.AsRegister());
  if (ProfilingInfoBuilder::IsInlineCacheUseful(instruction->AsInvoke(), this)) {
    ProfilingInfo* info = GetGraph()->GetProfilingInfo();
    DCHECK(info != nullptr);
    InlineCache* cache = ProfilingInfoBuilder::GetInlineCache(
        info, GetCompilerOptions(), instruction->AsInvoke());
    if (cache != nullptr) {
      uint64_t address = reinterpret_cast64<uint64_t>(cache);
      NearLabel done;
      __ movq(CpuRegister(TMP), Immediate(address));
      // Fast path for a monomorphic cache.
      __ cmpl(Address(CpuRegister(TMP), InlineCache::ClassesOffset().Int32Value()), klass);
      __ j(kEqual, &done);
      GenerateInvokeRuntime(
          GetThreadOffset<kX86_64PointerSize>(kQuickUpdateInlineCache).Int32Value());
      __ Bind(&done);
    } else {
      // This is unexpected, but we don't guarantee stable compilation across
      // JIT runs so just warn about it.
      ScopedObjectAccess soa(Thread::Current());
      LOG(WARNING) << "Missing inline cache for " << GetGraph()->GetArtMethod()->PrettyMethod();
    }
  }
}

void InstructionCodeGeneratorX86_64::VisitInvokeInterface(HInvokeInterface* invoke) {
  // TODO: b/18116999, our IMTs can miss an IncompatibleClassChangeError.
  LocationSummary* locations = invoke->GetLocations();
  CpuRegister temp = locations->GetTemp(0).AsRegister<CpuRegister>();
  Location receiver = locations->InAt(0);
  size_t class_offset = mirror::Object::ClassOffset().SizeValue();

  if (receiver.IsStackSlot()) {
    __ movl(temp, Address(CpuRegister(RSP), receiver.GetStackIndex()));
    // /* HeapReference<Class> */ temp = temp->klass_
    __ movl(temp, Address(temp, class_offset));
  } else {
    // /* HeapReference<Class> */ temp = receiver->klass_
    __ movl(temp, Address(receiver.AsRegister<CpuRegister>(), class_offset));
  }
  codegen_->MaybeRecordImplicitNullCheck(invoke);
  // Instead of simply (possibly) unpoisoning `temp` here, we should
  // emit a read barrier for the previous class reference load.
  // However this is not required in practice, as this is an
  // intermediate/temporary reference and because the current
  // concurrent copying collector keeps the from-space memory
  // intact/accessible until the end of the marking phase (the
  // concurrent copying collector may not in the future).
  __ MaybeUnpoisonHeapReference(temp);

  codegen_->MaybeGenerateInlineCacheCheck(invoke, temp);

  if (invoke->GetHiddenArgumentLoadKind() != MethodLoadKind::kRecursive &&
      invoke->GetHiddenArgumentLoadKind() != MethodLoadKind::kRuntimeCall) {
    Location hidden_reg = locations->GetTemp(1);
    // Set the hidden argument. This is safe to do this here, as RAX
    // won't be modified thereafter, before the `call` instruction.
    // We also do it after MaybeGenerateInlineCache that may use RAX.
    DCHECK_EQ(RAX, hidden_reg.AsRegister<Register>());
    codegen_->LoadMethod(invoke->GetHiddenArgumentLoadKind(), hidden_reg, invoke);
  }

  // temp = temp->GetAddressOfIMT()
  __ movq(temp,
      Address(temp, mirror::Class::ImtPtrOffset(kX86_64PointerSize).Uint32Value()));
  // temp = temp->GetImtEntryAt(method_offset);
  uint32_t method_offset = static_cast<uint32_t>(ImTable::OffsetOfElement(
      invoke->GetImtIndex(), kX86_64PointerSize));
  // temp = temp->GetImtEntryAt(method_offset);
  __ movq(temp, Address(temp, method_offset));
  if (invoke->GetHiddenArgumentLoadKind() == MethodLoadKind::kRuntimeCall) {
    // We pass the method from the IMT in case of a conflict. This will ensure
    // we go into the runtime to resolve the actual method.
    Location hidden_reg = locations->GetTemp(1);
    __ movq(hidden_reg.AsRegister<CpuRegister>(), temp);
  }
  // call temp->GetEntryPoint();
  __ call(Address(
      temp, ArtMethod::EntryPointFromQuickCompiledCodeOffset(kX86_64PointerSize).SizeValue()));

  DCHECK(!codegen_->IsLeafMethod());
  codegen_->RecordPcInfo(invoke);
}

void LocationsBuilderX86_64::VisitInvokePolymorphic(HInvokePolymorphic* invoke) {
  IntrinsicLocationsBuilderX86_64 intrinsic(codegen_);
  if (intrinsic.TryDispatch(invoke)) {
    return;
  }
  HandleInvoke(invoke);
}

void InstructionCodeGeneratorX86_64::VisitInvokePolymorphic(HInvokePolymorphic* invoke) {
  if (TryGenerateIntrinsicCode(invoke, codegen_)) {
    return;
  }
  codegen_->GenerateInvokePolymorphicCall(invoke);
}

void LocationsBuilderX86_64::VisitInvokeCustom(HInvokeCustom* invoke) {
  HandleInvoke(invoke);
}

void InstructionCodeGeneratorX86_64::VisitInvokeCustom(HInvokeCustom* invoke) {
  codegen_->GenerateInvokeCustomCall(invoke);
}

void LocationsBuilderX86_64::VisitNeg(HNeg* neg) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(neg, LocationSummary::kNoCall);
  switch (neg->GetResultType()) {
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetOut(Location::SameAsFirstInput());
      break;

    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetOut(Location::SameAsFirstInput());
      locations->AddTemp(Location::RequiresFpuRegister());
      break;

    default:
      LOG(FATAL) << "Unexpected neg type " << neg->GetResultType();
  }
}

void InstructionCodeGeneratorX86_64::VisitNeg(HNeg* neg) {
  LocationSummary* locations = neg->GetLocations();
  Location out = locations->Out();
  Location in = locations->InAt(0);
  switch (neg->GetResultType()) {
    case DataType::Type::kInt32:
      DCHECK(in.IsRegister());
      DCHECK(in.Equals(out));
      __ negl(out.AsRegister<CpuRegister>());
      break;

    case DataType::Type::kInt64:
      DCHECK(in.IsRegister());
      DCHECK(in.Equals(out));
      __ negq(out.AsRegister<CpuRegister>());
      break;

    case DataType::Type::kFloat32: {
      DCHECK(in.Equals(out));
      XmmRegister mask = locations->GetTemp(0).AsFpuRegister<XmmRegister>();
      // Implement float negation with an exclusive or with value
      // 0x80000000 (mask for bit 31, representing the sign of a
      // single-precision floating-point number).
      __ movss(mask, codegen_->LiteralInt32Address(0x80000000));
      __ xorps(out.AsFpuRegister<XmmRegister>(), mask);
      break;
    }

    case DataType::Type::kFloat64: {
      DCHECK(in.Equals(out));
      XmmRegister mask = locations->GetTemp(0).AsFpuRegister<XmmRegister>();
      // Implement double negation with an exclusive or with value
      // 0x8000000000000000 (mask for bit 63, representing the sign of
      // a double-precision floating-point number).
      __ movsd(mask, codegen_->LiteralInt64Address(INT64_C(0x8000000000000000)));
      __ xorpd(out.AsFpuRegister<XmmRegister>(), mask);
      break;
    }

    default:
      LOG(FATAL) << "Unexpected neg type " << neg->GetResultType();
  }
}

void LocationsBuilderX86_64::VisitTypeConversion(HTypeConversion* conversion) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(conversion, LocationSummary::kNoCall);
  DataType::Type result_type = conversion->GetResultType();
  DataType::Type input_type = conversion->GetInputType();
  DCHECK(!DataType::IsTypeConversionImplicit(input_type, result_type))
      << input_type << " -> " << result_type;

  switch (result_type) {
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
      DCHECK(DataType::IsIntegralType(input_type)) << input_type;
      locations->SetInAt(0, Location::Any());
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;

    case DataType::Type::kInt32:
      switch (input_type) {
        case DataType::Type::kInt64:
          locations->SetInAt(0, Location::Any());
          locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
          break;

        case DataType::Type::kFloat32:
          locations->SetInAt(0, Location::RequiresFpuRegister());
          locations->SetOut(Location::RequiresRegister());
          break;

        case DataType::Type::kFloat64:
          locations->SetInAt(0, Location::RequiresFpuRegister());
          locations->SetOut(Location::RequiresRegister());
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case DataType::Type::kInt64:
      switch (input_type) {
        case DataType::Type::kBool:
        case DataType::Type::kUint8:
        case DataType::Type::kInt8:
        case DataType::Type::kUint16:
        case DataType::Type::kInt16:
        case DataType::Type::kInt32:
          // TODO: We would benefit from a (to-be-implemented)
          // Location::RegisterOrStackSlot requirement for this input.
          locations->SetInAt(0, Location::RequiresRegister());
          locations->SetOut(Location::RequiresRegister());
          break;

        case DataType::Type::kFloat32:
          locations->SetInAt(0, Location::RequiresFpuRegister());
          locations->SetOut(Location::RequiresRegister());
          break;

        case DataType::Type::kFloat64:
          locations->SetInAt(0, Location::RequiresFpuRegister());
          locations->SetOut(Location::RequiresRegister());
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case DataType::Type::kFloat32:
      switch (input_type) {
        case DataType::Type::kBool:
        case DataType::Type::kUint8:
        case DataType::Type::kInt8:
        case DataType::Type::kUint16:
        case DataType::Type::kInt16:
        case DataType::Type::kInt32:
          locations->SetInAt(0, Location::Any());
          locations->SetOut(Location::RequiresFpuRegister());
          break;

        case DataType::Type::kInt64:
          locations->SetInAt(0, Location::Any());
          locations->SetOut(Location::RequiresFpuRegister());
          break;

        case DataType::Type::kFloat64:
          locations->SetInAt(0, Location::Any());
          locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case DataType::Type::kFloat64:
      switch (input_type) {
        case DataType::Type::kBool:
        case DataType::Type::kUint8:
        case DataType::Type::kInt8:
        case DataType::Type::kUint16:
        case DataType::Type::kInt16:
        case DataType::Type::kInt32:
          locations->SetInAt(0, Location::Any());
          locations->SetOut(Location::RequiresFpuRegister());
          break;

        case DataType::Type::kInt64:
          locations->SetInAt(0, Location::Any());
          locations->SetOut(Location::RequiresFpuRegister());
          break;

        case DataType::Type::kFloat32:
          locations->SetInAt(0, Location::Any());
          locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    default:
      LOG(FATAL) << "Unexpected type conversion from " << input_type
                 << " to " << result_type;
  }
}

void InstructionCodeGeneratorX86_64::VisitTypeConversion(HTypeConversion* conversion) {
  LocationSummary* locations = conversion->GetLocations();
  Location out = locations->Out();
  Location in = locations->InAt(0);
  DataType::Type result_type = conversion->GetResultType();
  DataType::Type input_type = conversion->GetInputType();
  DCHECK(!DataType::IsTypeConversionImplicit(input_type, result_type))
      << input_type << " -> " << result_type;
  switch (result_type) {
    case DataType::Type::kUint8:
      switch (input_type) {
        case DataType::Type::kInt8:
        case DataType::Type::kUint16:
        case DataType::Type::kInt16:
        case DataType::Type::kInt32:
        case DataType::Type::kInt64:
          if (in.IsRegister()) {
            __ movzxb(out.AsRegister<CpuRegister>(), in.AsRegister<CpuRegister>());
          } else if (in.IsStackSlot() || in.IsDoubleStackSlot()) {
            __ movzxb(out.AsRegister<CpuRegister>(),
                      Address(CpuRegister(RSP), in.GetStackIndex()));
          } else {
            __ movl(out.AsRegister<CpuRegister>(),
                    Immediate(static_cast<uint8_t>(Int64FromConstant(in.GetConstant()))));
          }
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case DataType::Type::kInt8:
      switch (input_type) {
        case DataType::Type::kUint8:
        case DataType::Type::kUint16:
        case DataType::Type::kInt16:
        case DataType::Type::kInt32:
        case DataType::Type::kInt64:
          if (in.IsRegister()) {
            __ movsxb(out.AsRegister<CpuRegister>(), in.AsRegister<CpuRegister>());
          } else if (in.IsStackSlot() || in.IsDoubleStackSlot()) {
            __ movsxb(out.AsRegister<CpuRegister>(),
                      Address(CpuRegister(RSP), in.GetStackIndex()));
          } else {
            __ movl(out.AsRegister<CpuRegister>(),
                    Immediate(static_cast<int8_t>(Int64FromConstant(in.GetConstant()))));
          }
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case DataType::Type::kUint16:
      switch (input_type) {
        case DataType::Type::kInt8:
        case DataType::Type::kInt16:
        case DataType::Type::kInt32:
        case DataType::Type::kInt64:
          if (in.IsRegister()) {
            __ movzxw(out.AsRegister<CpuRegister>(), in.AsRegister<CpuRegister>());
          } else if (in.IsStackSlot() || in.IsDoubleStackSlot()) {
            __ movzxw(out.AsRegister<CpuRegister>(),
                      Address(CpuRegister(RSP), in.GetStackIndex()));
          } else {
            __ movl(out.AsRegister<CpuRegister>(),
                    Immediate(static_cast<uint16_t>(Int64FromConstant(in.GetConstant()))));
          }
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case DataType::Type::kInt16:
      switch (input_type) {
        case DataType::Type::kUint16:
        case DataType::Type::kInt32:
        case DataType::Type::kInt64:
          if (in.IsRegister()) {
            __ movsxw(out.AsRegister<CpuRegister>(), in.AsRegister<CpuRegister>());
          } else if (in.IsStackSlot() || in.IsDoubleStackSlot()) {
            __ movsxw(out.AsRegister<CpuRegister>(),
                      Address(CpuRegister(RSP), in.GetStackIndex()));
          } else {
            __ movl(out.AsRegister<CpuRegister>(),
                    Immediate(static_cast<int16_t>(Int64FromConstant(in.GetConstant()))));
          }
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case DataType::Type::kInt32:
      switch (input_type) {
        case DataType::Type::kInt64:
          if (in.IsRegister()) {
            __ movl(out.AsRegister<CpuRegister>(), in.AsRegister<CpuRegister>());
          } else if (in.IsDoubleStackSlot()) {
            __ movl(out.AsRegister<CpuRegister>(),
                    Address(CpuRegister(RSP), in.GetStackIndex()));
          } else {
            DCHECK(in.IsConstant());
            DCHECK(in.GetConstant()->IsLongConstant());
            int64_t value = in.GetConstant()->AsLongConstant()->GetValue();
            __ movl(out.AsRegister<CpuRegister>(), Immediate(static_cast<int32_t>(value)));
          }
          break;

        case DataType::Type::kFloat32: {
          XmmRegister input = in.AsFpuRegister<XmmRegister>();
          CpuRegister output = out.AsRegister<CpuRegister>();
          NearLabel done, nan;

          __ movl(output, Immediate(kPrimIntMax));
          // if input >= (float)INT_MAX goto done
          __ comiss(input, codegen_->LiteralFloatAddress(static_cast<float>(kPrimIntMax)));
          __ j(kAboveEqual, &done);
          // if input == NaN goto nan
          __ j(kUnordered, &nan);
          // output = float-to-int-truncate(input)
          __ cvttss2si(output, input, false);
          __ jmp(&done);
          __ Bind(&nan);
          //  output = 0
          __ xorl(output, output);
          __ Bind(&done);
          break;
        }

        case DataType::Type::kFloat64: {
          XmmRegister input = in.AsFpuRegister<XmmRegister>();
          CpuRegister output = out.AsRegister<CpuRegister>();
          NearLabel done, nan;

          __ movl(output, Immediate(kPrimIntMax));
          // if input >= (double)INT_MAX goto done
          __ comisd(input, codegen_->LiteralDoubleAddress(kPrimIntMax));
          __ j(kAboveEqual, &done);
          // if input == NaN goto nan
          __ j(kUnordered, &nan);
          // output = double-to-int-truncate(input)
          __ cvttsd2si(output, input);
          __ jmp(&done);
          __ Bind(&nan);
          //  output = 0
          __ xorl(output, output);
          __ Bind(&done);
          break;
        }

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case DataType::Type::kInt64:
      switch (input_type) {
        DCHECK(out.IsRegister());
        case DataType::Type::kBool:
        case DataType::Type::kUint8:
        case DataType::Type::kInt8:
        case DataType::Type::kUint16:
        case DataType::Type::kInt16:
        case DataType::Type::kInt32:
          DCHECK(in.IsRegister());
          __ movsxd(out.AsRegister<CpuRegister>(), in.AsRegister<CpuRegister>());
          break;

        case DataType::Type::kFloat32: {
          XmmRegister input = in.AsFpuRegister<XmmRegister>();
          CpuRegister output = out.AsRegister<CpuRegister>();
          NearLabel done, nan;

          codegen_->Load64BitValue(output, kPrimLongMax);
          // if input >= (float)LONG_MAX goto done
          __ comiss(input, codegen_->LiteralFloatAddress(static_cast<float>(kPrimLongMax)));
          __ j(kAboveEqual, &done);
          // if input == NaN goto nan
          __ j(kUnordered, &nan);
          // output = float-to-long-truncate(input)
          __ cvttss2si(output, input, true);
          __ jmp(&done);
          __ Bind(&nan);
          //  output = 0
          __ xorl(output, output);
          __ Bind(&done);
          break;
        }

        case DataType::Type::kFloat64: {
          XmmRegister input = in.AsFpuRegister<XmmRegister>();
          CpuRegister output = out.AsRegister<CpuRegister>();
          NearLabel done, nan;

          codegen_->Load64BitValue(output, kPrimLongMax);
          // if input >= (double)LONG_MAX goto done
          __ comisd(input, codegen_->LiteralDoubleAddress(
                static_cast<double>(kPrimLongMax)));
          __ j(kAboveEqual, &done);
          // if input == NaN goto nan
          __ j(kUnordered, &nan);
          // output = double-to-long-truncate(input)
          __ cvttsd2si(output, input, true);
          __ jmp(&done);
          __ Bind(&nan);
          //  output = 0
          __ xorl(output, output);
          __ Bind(&done);
          break;
        }

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case DataType::Type::kFloat32:
      switch (input_type) {
        case DataType::Type::kBool:
        case DataType::Type::kUint8:
        case DataType::Type::kInt8:
        case DataType::Type::kUint16:
        case DataType::Type::kInt16:
        case DataType::Type::kInt32:
          if (in.IsRegister()) {
            __ cvtsi2ss(out.AsFpuRegister<XmmRegister>(), in.AsRegister<CpuRegister>(), false);
          } else if (in.IsConstant()) {
            int32_t v = in.GetConstant()->AsIntConstant()->GetValue();
            XmmRegister dest = out.AsFpuRegister<XmmRegister>();
            codegen_->Load32BitValue(dest, static_cast<float>(v));
          } else {
            __ cvtsi2ss(out.AsFpuRegister<XmmRegister>(),
                        Address(CpuRegister(RSP), in.GetStackIndex()), false);
          }
          break;

        case DataType::Type::kInt64:
          if (in.IsRegister()) {
            __ cvtsi2ss(out.AsFpuRegister<XmmRegister>(), in.AsRegister<CpuRegister>(), true);
          } else if (in.IsConstant()) {
            int64_t v = in.GetConstant()->AsLongConstant()->GetValue();
            XmmRegister dest = out.AsFpuRegister<XmmRegister>();
            codegen_->Load32BitValue(dest, static_cast<float>(v));
          } else {
            __ cvtsi2ss(out.AsFpuRegister<XmmRegister>(),
                        Address(CpuRegister(RSP), in.GetStackIndex()), true);
          }
          break;

        case DataType::Type::kFloat64:
          if (in.IsFpuRegister()) {
            __ cvtsd2ss(out.AsFpuRegister<XmmRegister>(), in.AsFpuRegister<XmmRegister>());
          } else if (in.IsConstant()) {
            double v = in.GetConstant()->AsDoubleConstant()->GetValue();
            XmmRegister dest = out.AsFpuRegister<XmmRegister>();
            codegen_->Load32BitValue(dest, static_cast<float>(v));
          } else {
            __ cvtsd2ss(out.AsFpuRegister<XmmRegister>(),
                        Address(CpuRegister(RSP), in.GetStackIndex()));
          }
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case DataType::Type::kFloat64:
      switch (input_type) {
        case DataType::Type::kBool:
        case DataType::Type::kUint8:
        case DataType::Type::kInt8:
        case DataType::Type::kUint16:
        case DataType::Type::kInt16:
        case DataType::Type::kInt32:
          if (in.IsRegister()) {
            __ cvtsi2sd(out.AsFpuRegister<XmmRegister>(), in.AsRegister<CpuRegister>(), false);
          } else if (in.IsConstant()) {
            int32_t v = in.GetConstant()->AsIntConstant()->GetValue();
            XmmRegister dest = out.AsFpuRegister<XmmRegister>();
            codegen_->Load64BitValue(dest, static_cast<double>(v));
          } else {
            __ cvtsi2sd(out.AsFpuRegister<XmmRegister>(),
                        Address(CpuRegister(RSP), in.GetStackIndex()), false);
          }
          break;

        case DataType::Type::kInt64:
          if (in.IsRegister()) {
            __ cvtsi2sd(out.AsFpuRegister<XmmRegister>(), in.AsRegister<CpuRegister>(), true);
          } else if (in.IsConstant()) {
            int64_t v = in.GetConstant()->AsLongConstant()->GetValue();
            XmmRegister dest = out.AsFpuRegister<XmmRegister>();
            codegen_->Load64BitValue(dest, static_cast<double>(v));
          } else {
            __ cvtsi2sd(out.AsFpuRegister<XmmRegister>(),
                        Address(CpuRegister(RSP), in.GetStackIndex()), true);
          }
          break;

        case DataType::Type::kFloat32:
          if (in.IsFpuRegister()) {
            __ cvtss2sd(out.AsFpuRegister<XmmRegister>(), in.AsFpuRegister<XmmRegister>());
          } else if (in.IsConstant()) {
            float v = in.GetConstant()->AsFloatConstant()->GetValue();
            XmmRegister dest = out.AsFpuRegister<XmmRegister>();
            codegen_->Load64BitValue(dest, static_cast<double>(v));
          } else {
            __ cvtss2sd(out.AsFpuRegister<XmmRegister>(),
                        Address(CpuRegister(RSP), in.GetStackIndex()));
          }
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    default:
      LOG(FATAL) << "Unexpected type conversion from " << input_type
                 << " to " << result_type;
  }
}

void LocationsBuilderX86_64::VisitAdd(HAdd* add) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(add, LocationSummary::kNoCall);
  switch (add->GetResultType()) {
    case DataType::Type::kInt32: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RegisterOrConstant(add->InputAt(1)));
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;
    }

    case DataType::Type::kInt64: {
      locations->SetInAt(0, Location::RequiresRegister());
      // We can use a leaq or addq if the constant can fit in an immediate.
      locations->SetInAt(1, Location::RegisterOrInt32Constant(add->InputAt(1)));
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;
    }

    case DataType::Type::kFloat64:
    case DataType::Type::kFloat32: {
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::Any());
      locations->SetOut(Location::SameAsFirstInput());
      break;
    }

    default:
      LOG(FATAL) << "Unexpected add type " << add->GetResultType();
  }
}

void InstructionCodeGeneratorX86_64::VisitAdd(HAdd* add) {
  LocationSummary* locations = add->GetLocations();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);
  Location out = locations->Out();

  switch (add->GetResultType()) {
    case DataType::Type::kInt32: {
      if (second.IsRegister()) {
        if (out.AsRegister<Register>() == first.AsRegister<Register>()) {
          __ addl(out.AsRegister<CpuRegister>(), second.AsRegister<CpuRegister>());
        } else if (out.AsRegister<Register>() == second.AsRegister<Register>()) {
          __ addl(out.AsRegister<CpuRegister>(), first.AsRegister<CpuRegister>());
        } else {
          __ leal(out.AsRegister<CpuRegister>(), Address(
              first.AsRegister<CpuRegister>(), second.AsRegister<CpuRegister>(), TIMES_1, 0));
        }
      } else if (second.IsConstant()) {
        if (out.AsRegister<Register>() == first.AsRegister<Register>()) {
          __ addl(out.AsRegister<CpuRegister>(),
                  Immediate(second.GetConstant()->AsIntConstant()->GetValue()));
        } else {
          __ leal(out.AsRegister<CpuRegister>(), Address(
              first.AsRegister<CpuRegister>(), second.GetConstant()->AsIntConstant()->GetValue()));
        }
      } else {
        DCHECK(first.Equals(locations->Out()));
        __ addl(first.AsRegister<CpuRegister>(), Address(CpuRegister(RSP), second.GetStackIndex()));
      }
      break;
    }

    case DataType::Type::kInt64: {
      if (second.IsRegister()) {
        if (out.AsRegister<Register>() == first.AsRegister<Register>()) {
          __ addq(out.AsRegister<CpuRegister>(), second.AsRegister<CpuRegister>());
        } else if (out.AsRegister<Register>() == second.AsRegister<Register>()) {
          __ addq(out.AsRegister<CpuRegister>(), first.AsRegister<CpuRegister>());
        } else {
          __ leaq(out.AsRegister<CpuRegister>(), Address(
              first.AsRegister<CpuRegister>(), second.AsRegister<CpuRegister>(), TIMES_1, 0));
        }
      } else {
        DCHECK(second.IsConstant());
        int64_t value = second.GetConstant()->AsLongConstant()->GetValue();
        int32_t int32_value = Low32Bits(value);
        DCHECK_EQ(int32_value, value);
        if (out.AsRegister<Register>() == first.AsRegister<Register>()) {
          __ addq(out.AsRegister<CpuRegister>(), Immediate(int32_value));
        } else {
          __ leaq(out.AsRegister<CpuRegister>(), Address(
              first.AsRegister<CpuRegister>(), int32_value));
        }
      }
      break;
    }

    case DataType::Type::kFloat32: {
      if (second.IsFpuRegister()) {
        __ addss(first.AsFpuRegister<XmmRegister>(), second.AsFpuRegister<XmmRegister>());
      } else if (second.IsConstant()) {
        __ addss(first.AsFpuRegister<XmmRegister>(),
                 codegen_->LiteralFloatAddress(
                     second.GetConstant()->AsFloatConstant()->GetValue()));
      } else {
        DCHECK(second.IsStackSlot());
        __ addss(first.AsFpuRegister<XmmRegister>(),
                 Address(CpuRegister(RSP), second.GetStackIndex()));
      }
      break;
    }

    case DataType::Type::kFloat64: {
      if (second.IsFpuRegister()) {
        __ addsd(first.AsFpuRegister<XmmRegister>(), second.AsFpuRegister<XmmRegister>());
      } else if (second.IsConstant()) {
        __ addsd(first.AsFpuRegister<XmmRegister>(),
                 codegen_->LiteralDoubleAddress(
                     second.GetConstant()->AsDoubleConstant()->GetValue()));
      } else {
        DCHECK(second.IsDoubleStackSlot());
        __ addsd(first.AsFpuRegister<XmmRegister>(),
                 Address(CpuRegister(RSP), second.GetStackIndex()));
      }
      break;
    }

    default:
      LOG(FATAL) << "Unexpected add type " << add->GetResultType();
  }
}

void LocationsBuilderX86_64::VisitSub(HSub* sub) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(sub, LocationSummary::kNoCall);
  switch (sub->GetResultType()) {
    case DataType::Type::kInt32: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::Any());
      locations->SetOut(Location::SameAsFirstInput());
      break;
    }
    case DataType::Type::kInt64: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RegisterOrInt32Constant(sub->InputAt(1)));
      locations->SetOut(Location::SameAsFirstInput());
      break;
    }
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64: {
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::Any());
      locations->SetOut(Location::SameAsFirstInput());
      break;
    }
    default:
      LOG(FATAL) << "Unexpected sub type " << sub->GetResultType();
  }
}

void InstructionCodeGeneratorX86_64::VisitSub(HSub* sub) {
  LocationSummary* locations = sub->GetLocations();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);
  DCHECK(first.Equals(locations->Out()));
  switch (sub->GetResultType()) {
    case DataType::Type::kInt32: {
      if (second.IsRegister()) {
        __ subl(first.AsRegister<CpuRegister>(), second.AsRegister<CpuRegister>());
      } else if (second.IsConstant()) {
        Immediate imm(second.GetConstant()->AsIntConstant()->GetValue());
        __ subl(first.AsRegister<CpuRegister>(), imm);
      } else {
        __ subl(first.AsRegister<CpuRegister>(), Address(CpuRegister(RSP), second.GetStackIndex()));
      }
      break;
    }
    case DataType::Type::kInt64: {
      if (second.IsConstant()) {
        int64_t value = second.GetConstant()->AsLongConstant()->GetValue();
        DCHECK(IsInt<32>(value));
        __ subq(first.AsRegister<CpuRegister>(), Immediate(static_cast<int32_t>(value)));
      } else {
        __ subq(first.AsRegister<CpuRegister>(), second.AsRegister<CpuRegister>());
      }
      break;
    }

    case DataType::Type::kFloat32: {
      if (second.IsFpuRegister()) {
        __ subss(first.AsFpuRegister<XmmRegister>(), second.AsFpuRegister<XmmRegister>());
      } else if (second.IsConstant()) {
        __ subss(first.AsFpuRegister<XmmRegister>(),
                 codegen_->LiteralFloatAddress(
                     second.GetConstant()->AsFloatConstant()->GetValue()));
      } else {
        DCHECK(second.IsStackSlot());
        __ subss(first.AsFpuRegister<XmmRegister>(),
                 Address(CpuRegister(RSP), second.GetStackIndex()));
      }
      break;
    }

    case DataType::Type::kFloat64: {
      if (second.IsFpuRegister()) {
        __ subsd(first.AsFpuRegister<XmmRegister>(), second.AsFpuRegister<XmmRegister>());
      } else if (second.IsConstant()) {
        __ subsd(first.AsFpuRegister<XmmRegister>(),
                 codegen_->LiteralDoubleAddress(
                     second.GetConstant()->AsDoubleConstant()->GetValue()));
      } else {
        DCHECK(second.IsDoubleStackSlot());
        __ subsd(first.AsFpuRegister<XmmRegister>(),
                 Address(CpuRegister(RSP), second.GetStackIndex()));
      }
      break;
    }

    default:
      LOG(FATAL) << "Unexpected sub type " << sub->GetResultType();
  }
}

void LocationsBuilderX86_64::VisitMul(HMul* mul) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(mul, LocationSummary::kNoCall);
  switch (mul->GetResultType()) {
    case DataType::Type::kInt32: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::Any());
      if (mul->InputAt(1)->IsIntConstant()) {
        // Can use 3 operand multiply.
        locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      } else {
        locations->SetOut(Location::SameAsFirstInput());
      }
      break;
    }
    case DataType::Type::kInt64: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::Any());
      if (mul->InputAt(1)->IsLongConstant() &&
          IsInt<32>(mul->InputAt(1)->AsLongConstant()->GetValue())) {
        // Can use 3 operand multiply.
        locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      } else {
        locations->SetOut(Location::SameAsFirstInput());
      }
      break;
    }
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64: {
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::Any());
      locations->SetOut(Location::SameAsFirstInput());
      break;
    }

    default:
      LOG(FATAL) << "Unexpected mul type " << mul->GetResultType();
  }
}

void InstructionCodeGeneratorX86_64::VisitMul(HMul* mul) {
  LocationSummary* locations = mul->GetLocations();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);
  Location out = locations->Out();
  switch (mul->GetResultType()) {
    case DataType::Type::kInt32:
      // The constant may have ended up in a register, so test explicitly to avoid
      // problems where the output may not be the same as the first operand.
      if (mul->InputAt(1)->IsIntConstant()) {
        Immediate imm(mul->InputAt(1)->AsIntConstant()->GetValue());
        __ imull(out.AsRegister<CpuRegister>(), first.AsRegister<CpuRegister>(), imm);
      } else if (second.IsRegister()) {
        DCHECK(first.Equals(out));
        __ imull(first.AsRegister<CpuRegister>(), second.AsRegister<CpuRegister>());
      } else {
        DCHECK(first.Equals(out));
        DCHECK(second.IsStackSlot());
        __ imull(first.AsRegister<CpuRegister>(),
                 Address(CpuRegister(RSP), second.GetStackIndex()));
      }
      break;
    case DataType::Type::kInt64: {
      // The constant may have ended up in a register, so test explicitly to avoid
      // problems where the output may not be the same as the first operand.
      if (mul->InputAt(1)->IsLongConstant()) {
        int64_t value = mul->InputAt(1)->AsLongConstant()->GetValue();
        if (IsInt<32>(value)) {
          __ imulq(out.AsRegister<CpuRegister>(), first.AsRegister<CpuRegister>(),
                   Immediate(static_cast<int32_t>(value)));
        } else {
          // Have to use the constant area.
          DCHECK(first.Equals(out));
          __ imulq(first.AsRegister<CpuRegister>(), codegen_->LiteralInt64Address(value));
        }
      } else if (second.IsRegister()) {
        DCHECK(first.Equals(out));
        __ imulq(first.AsRegister<CpuRegister>(), second.AsRegister<CpuRegister>());
      } else {
        DCHECK(second.IsDoubleStackSlot());
        DCHECK(first.Equals(out));
        __ imulq(first.AsRegister<CpuRegister>(),
                 Address(CpuRegister(RSP), second.GetStackIndex()));
      }
      break;
    }

    case DataType::Type::kFloat32: {
      DCHECK(first.Equals(out));
      if (second.IsFpuRegister()) {
        __ mulss(first.AsFpuRegister<XmmRegister>(), second.AsFpuRegister<XmmRegister>());
      } else if (second.IsConstant()) {
        __ mulss(first.AsFpuRegister<XmmRegister>(),
                 codegen_->LiteralFloatAddress(
                     second.GetConstant()->AsFloatConstant()->GetValue()));
      } else {
        DCHECK(second.IsStackSlot());
        __ mulss(first.AsFpuRegister<XmmRegister>(),
                 Address(CpuRegister(RSP), second.GetStackIndex()));
      }
      break;
    }

    case DataType::Type::kFloat64: {
      DCHECK(first.Equals(out));
      if (second.IsFpuRegister()) {
        __ mulsd(first.AsFpuRegister<XmmRegister>(), second.AsFpuRegister<XmmRegister>());
      } else if (second.IsConstant()) {
        __ mulsd(first.AsFpuRegister<XmmRegister>(),
                 codegen_->LiteralDoubleAddress(
                     second.GetConstant()->AsDoubleConstant()->GetValue()));
      } else {
        DCHECK(second.IsDoubleStackSlot());
        __ mulsd(first.AsFpuRegister<XmmRegister>(),
                 Address(CpuRegister(RSP), second.GetStackIndex()));
      }
      break;
    }

    default:
      LOG(FATAL) << "Unexpected mul type " << mul->GetResultType();
  }
}

void InstructionCodeGeneratorX86_64::PushOntoFPStack(Location source, uint32_t temp_offset,
                                                     uint32_t stack_adjustment, bool is_float) {
  if (source.IsStackSlot()) {
    DCHECK(is_float);
    __ flds(Address(CpuRegister(RSP), source.GetStackIndex() + stack_adjustment));
  } else if (source.IsDoubleStackSlot()) {
    DCHECK(!is_float);
    __ fldl(Address(CpuRegister(RSP), source.GetStackIndex() + stack_adjustment));
  } else {
    // Write the value to the temporary location on the stack and load to FP stack.
    if (is_float) {
      Location stack_temp = Location::StackSlot(temp_offset);
      codegen_->Move(stack_temp, source);
      __ flds(Address(CpuRegister(RSP), temp_offset));
    } else {
      Location stack_temp = Location::DoubleStackSlot(temp_offset);
      codegen_->Move(stack_temp, source);
      __ fldl(Address(CpuRegister(RSP), temp_offset));
    }
  }
}

void InstructionCodeGeneratorX86_64::GenerateRemFP(HRem *rem) {
  DataType::Type type = rem->GetResultType();
  bool is_float = type == DataType::Type::kFloat32;
  size_t elem_size = DataType::Size(type);
  LocationSummary* locations = rem->GetLocations();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);
  Location out = locations->Out();

  // Create stack space for 2 elements.
  // TODO: enhance register allocator to ask for stack temporaries.
  __ subq(CpuRegister(RSP), Immediate(2 * elem_size));

  // Load the values to the FP stack in reverse order, using temporaries if needed.
  PushOntoFPStack(second, elem_size, 2 * elem_size, is_float);
  PushOntoFPStack(first, 0, 2 * elem_size, is_float);

  // Loop doing FPREM until we stabilize.
  NearLabel retry;
  __ Bind(&retry);
  __ fprem();

  // Move FP status to AX.
  __ fstsw();

  // And see if the argument reduction is complete. This is signaled by the
  // C2 FPU flag bit set to 0.
  __ andl(CpuRegister(RAX), Immediate(kC2ConditionMask));
  __ j(kNotEqual, &retry);

  // We have settled on the final value. Retrieve it into an XMM register.
  // Store FP top of stack to real stack.
  if (is_float) {
    __ fsts(Address(CpuRegister(RSP), 0));
  } else {
    __ fstl(Address(CpuRegister(RSP), 0));
  }

  // Pop the 2 items from the FP stack.
  __ fucompp();

  // Load the value from the stack into an XMM register.
  DCHECK(out.IsFpuRegister()) << out;
  if (is_float) {
    __ movss(out.AsFpuRegister<XmmRegister>(), Address(CpuRegister(RSP), 0));
  } else {
    __ movsd(out.AsFpuRegister<XmmRegister>(), Address(CpuRegister(RSP), 0));
  }

  // And remove the temporary stack space we allocated.
  __ addq(CpuRegister(RSP), Immediate(2 * elem_size));
}

void InstructionCodeGeneratorX86_64::DivRemOneOrMinusOne(HBinaryOperation* instruction) {
  DCHECK(instruction->IsDiv() || instruction->IsRem());

  LocationSummary* locations = instruction->GetLocations();
  Location second = locations->InAt(1);
  DCHECK(second.IsConstant());

  CpuRegister output_register = locations->Out().AsRegister<CpuRegister>();
  CpuRegister input_register = locations->InAt(0).AsRegister<CpuRegister>();
  int64_t imm = Int64FromConstant(second.GetConstant());

  DCHECK(imm == 1 || imm == -1);

  switch (instruction->GetResultType()) {
    case DataType::Type::kInt32: {
      if (instruction->IsRem()) {
        __ xorl(output_register, output_register);
      } else {
        __ movl(output_register, input_register);
        if (imm == -1) {
          __ negl(output_register);
        }
      }
      break;
    }

    case DataType::Type::kInt64: {
      if (instruction->IsRem()) {
        __ xorl(output_register, output_register);
      } else {
        __ movq(output_register, input_register);
        if (imm == -1) {
          __ negq(output_register);
        }
      }
      break;
    }

    default:
      LOG(FATAL) << "Unexpected type for div by (-)1 " << instruction->GetResultType();
  }
}
void InstructionCodeGeneratorX86_64::RemByPowerOfTwo(HRem* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Location second = locations->InAt(1);
  CpuRegister out = locations->Out().AsRegister<CpuRegister>();
  CpuRegister numerator = locations->InAt(0).AsRegister<CpuRegister>();
  int64_t imm = Int64FromConstant(second.GetConstant());
  DCHECK(IsPowerOfTwo(AbsOrMin(imm)));
  uint64_t abs_imm = AbsOrMin(imm);
  CpuRegister tmp = locations->GetTemp(0).AsRegister<CpuRegister>();
  if (instruction->GetResultType() == DataType::Type::kInt32) {
    NearLabel done;
    __ movl(out, numerator);
    __ andl(out, Immediate(abs_imm-1));
    __ j(Condition::kZero, &done);
    __ leal(tmp, Address(out, static_cast<int32_t>(~(abs_imm-1))));
    __ testl(numerator, numerator);
    __ cmov(Condition::kLess, out, tmp, false);
    __ Bind(&done);

  } else {
    DCHECK_EQ(instruction->GetResultType(), DataType::Type::kInt64);
    codegen_->Load64BitValue(tmp, abs_imm - 1);
    NearLabel done;

    __ movq(out, numerator);
    __ andq(out, tmp);
    __ j(Condition::kZero, &done);
    __ movq(tmp, numerator);
    __ sarq(tmp, Immediate(63));
    __ shlq(tmp, Immediate(WhichPowerOf2(abs_imm)));
    __ orq(out, tmp);
    __ Bind(&done);
  }
}
void InstructionCodeGeneratorX86_64::DivByPowerOfTwo(HDiv* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Location second = locations->InAt(1);

  CpuRegister output_register = locations->Out().AsRegister<CpuRegister>();
  CpuRegister numerator = locations->InAt(0).AsRegister<CpuRegister>();

  int64_t imm = Int64FromConstant(second.GetConstant());
  DCHECK(IsPowerOfTwo(AbsOrMin(imm)));
  uint64_t abs_imm = AbsOrMin(imm);

  CpuRegister tmp = locations->GetTemp(0).AsRegister<CpuRegister>();

  if (instruction->GetResultType() == DataType::Type::kInt32) {
    // When denominator is equal to 2, we can add signed bit and numerator to tmp.
    // Below we are using addl instruction instead of cmov which give us 1 cycle benefit.
    if (abs_imm == 2) {
      __ leal(tmp, Address(numerator, 0));
      __ shrl(tmp, Immediate(31));
      __ addl(tmp, numerator);
    } else {
      __ leal(tmp, Address(numerator, abs_imm - 1));
      __ testl(numerator, numerator);
      __ cmov(kGreaterEqual, tmp, numerator);
    }
    int shift = CTZ(imm);
    __ sarl(tmp, Immediate(shift));

    if (imm < 0) {
      __ negl(tmp);
    }

    __ movl(output_register, tmp);
  } else {
    DCHECK_EQ(instruction->GetResultType(), DataType::Type::kInt64);
    CpuRegister rdx = locations->GetTemp(0).AsRegister<CpuRegister>();
    if (abs_imm == 2) {
      __ movq(rdx, numerator);
      __ shrq(rdx, Immediate(63));
      __ addq(rdx, numerator);
    } else {
      codegen_->Load64BitValue(rdx, abs_imm - 1);
      __ addq(rdx, numerator);
      __ testq(numerator, numerator);
      __ cmov(kGreaterEqual, rdx, numerator);
    }
    int shift = CTZ(imm);
    __ sarq(rdx, Immediate(shift));

    if (imm < 0) {
      __ negq(rdx);
    }

    __ movq(output_register, rdx);
  }
}

void InstructionCodeGeneratorX86_64::GenerateDivRemWithAnyConstant(HBinaryOperation* instruction) {
  DCHECK(instruction->IsDiv() || instruction->IsRem());

  LocationSummary* locations = instruction->GetLocations();
  Location second = locations->InAt(1);

  CpuRegister numerator = instruction->IsDiv() ? locations->GetTemp(1).AsRegister<CpuRegister>()
      : locations->GetTemp(0).AsRegister<CpuRegister>();
  CpuRegister eax = locations->InAt(0).AsRegister<CpuRegister>();
  CpuRegister edx = instruction->IsDiv() ? locations->GetTemp(0).AsRegister<CpuRegister>()
      : locations->Out().AsRegister<CpuRegister>();
  CpuRegister out = locations->Out().AsRegister<CpuRegister>();

  DCHECK_EQ(RAX, eax.AsRegister());
  DCHECK_EQ(RDX, edx.AsRegister());
  if (instruction->IsDiv()) {
    DCHECK_EQ(RAX, out.AsRegister());
  } else {
    DCHECK_EQ(RDX, out.AsRegister());
  }

  int64_t magic;
  int shift;

  // TODO: can these branches be written as one?
  if (instruction->GetResultType() == DataType::Type::kInt32) {
    int imm = second.GetConstant()->AsIntConstant()->GetValue();

    CalculateMagicAndShiftForDivRem(imm, false /* is_long= */, &magic, &shift);

    __ movl(numerator, eax);

    __ movl(eax, Immediate(magic));
    __ imull(numerator);

    if (imm > 0 && magic < 0) {
      __ addl(edx, numerator);
    } else if (imm < 0 && magic > 0) {
      __ subl(edx, numerator);
    }

    if (shift != 0) {
      __ sarl(edx, Immediate(shift));
    }

    __ movl(eax, edx);
    __ shrl(edx, Immediate(31));
    __ addl(edx, eax);

    if (instruction->IsRem()) {
      __ movl(eax, numerator);
      __ imull(edx, Immediate(imm));
      __ subl(eax, edx);
      __ movl(edx, eax);
    } else {
      __ movl(eax, edx);
    }
  } else {
    int64_t imm = second.GetConstant()->AsLongConstant()->GetValue();

    DCHECK_EQ(instruction->GetResultType(), DataType::Type::kInt64);

    CpuRegister rax = eax;
    CpuRegister rdx = edx;

    CalculateMagicAndShiftForDivRem(imm, true /* is_long= */, &magic, &shift);

    // Save the numerator.
    __ movq(numerator, rax);

    // RAX = magic
    codegen_->Load64BitValue(rax, magic);

    // RDX:RAX = magic * numerator
    __ imulq(numerator);

    if (imm > 0 && magic < 0) {
      // RDX += numerator
      __ addq(rdx, numerator);
    } else if (imm < 0 && magic > 0) {
      // RDX -= numerator
      __ subq(rdx, numerator);
    }

    // Shift if needed.
    if (shift != 0) {
      __ sarq(rdx, Immediate(shift));
    }

    // RDX += 1 if RDX < 0
    __ movq(rax, rdx);
    __ shrq(rdx, Immediate(63));
    __ addq(rdx, rax);

    if (instruction->IsRem()) {
      __ movq(rax, numerator);

      if (IsInt<32>(imm)) {
        __ imulq(rdx, Immediate(static_cast<int32_t>(imm)));
      } else {
        __ imulq(rdx, codegen_->LiteralInt64Address(imm));
      }

      __ subq(rax, rdx);
      __ movq(rdx, rax);
    } else {
      __ movq(rax, rdx);
    }
  }
}

void InstructionCodeGeneratorX86_64::GenerateDivRemIntegral(HBinaryOperation* instruction) {
  DCHECK(instruction->IsDiv() || instruction->IsRem());
  DataType::Type type = instruction->GetResultType();
  DCHECK(type == DataType::Type::kInt32 || type == DataType::Type::kInt64);

  bool is_div = instruction->IsDiv();
  LocationSummary* locations = instruction->GetLocations();

  CpuRegister out = locations->Out().AsRegister<CpuRegister>();
  Location second = locations->InAt(1);

  DCHECK_EQ(RAX, locations->InAt(0).AsRegister<CpuRegister>().AsRegister());
  DCHECK_EQ(is_div ? RAX : RDX, out.AsRegister());

  if (second.IsConstant()) {
    int64_t imm = Int64FromConstant(second.GetConstant());

    if (imm == 0) {
      // Do not generate anything. DivZeroCheck would prevent any code to be executed.
    } else if (imm == 1 || imm == -1) {
      DivRemOneOrMinusOne(instruction);
    } else if (IsPowerOfTwo(AbsOrMin(imm))) {
      if (is_div) {
        DivByPowerOfTwo(instruction->AsDiv());
      } else {
        RemByPowerOfTwo(instruction->AsRem());
      }
    } else {
      DCHECK(imm <= -2 || imm >= 2);
      GenerateDivRemWithAnyConstant(instruction);
    }
  } else {
    SlowPathCode* slow_path =
        new (codegen_->GetScopedAllocator()) DivRemMinusOneSlowPathX86_64(
            instruction, out.AsRegister(), type, is_div);
    codegen_->AddSlowPath(slow_path);

    CpuRegister second_reg = second.AsRegister<CpuRegister>();
    // 0x80000000(00000000)/-1 triggers an arithmetic exception!
    // Dividing by -1 is actually negation and -0x800000000(00000000) = 0x80000000(00000000)
    // so it's safe to just use negl instead of more complex comparisons.
    if (type == DataType::Type::kInt32) {
      __ cmpl(second_reg, Immediate(-1));
      __ j(kEqual, slow_path->GetEntryLabel());
      // edx:eax <- sign-extended of eax
      __ cdq();
      // eax = quotient, edx = remainder
      __ idivl(second_reg);
    } else {
      __ cmpq(second_reg, Immediate(-1));
      __ j(kEqual, slow_path->GetEntryLabel());
      // rdx:rax <- sign-extended of rax
      __ cqo();
      // rax = quotient, rdx = remainder
      __ idivq(second_reg);
    }
    __ Bind(slow_path->GetExitLabel());
  }
}

void LocationsBuilderX86_64::VisitDiv(HDiv* div) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(div, LocationSummary::kNoCall);
  switch (div->GetResultType()) {
    case DataType::Type::kInt32:
    case DataType::Type::kInt64: {
      locations->SetInAt(0, Location::RegisterLocation(RAX));
      locations->SetInAt(1, Location::RegisterOrConstant(div->InputAt(1)));
      locations->SetOut(Location::SameAsFirstInput());
      // Intel uses edx:eax as the dividend.
      locations->AddTemp(Location::RegisterLocation(RDX));
      // We need to save the numerator while we tweak rax and rdx. As we are using imul in a way
      // which enforces results to be in RAX and RDX, things are simpler if we use RDX also as
      // output and request another temp.
      if (div->InputAt(1)->IsConstant()) {
        locations->AddTemp(Location::RequiresRegister());
      }
      break;
    }

    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64: {
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::Any());
      locations->SetOut(Location::SameAsFirstInput());
      break;
    }

    default:
      LOG(FATAL) << "Unexpected div type " << div->GetResultType();
  }
}

void InstructionCodeGeneratorX86_64::VisitDiv(HDiv* div) {
  LocationSummary* locations = div->GetLocations();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);
  DCHECK(first.Equals(locations->Out()));

  DataType::Type type = div->GetResultType();
  switch (type) {
    case DataType::Type::kInt32:
    case DataType::Type::kInt64: {
      GenerateDivRemIntegral(div);
      break;
    }

    case DataType::Type::kFloat32: {
      if (second.IsFpuRegister()) {
        __ divss(first.AsFpuRegister<XmmRegister>(), second.AsFpuRegister<XmmRegister>());
      } else if (second.IsConstant()) {
        __ divss(first.AsFpuRegister<XmmRegister>(),
                 codegen_->LiteralFloatAddress(
                     second.GetConstant()->AsFloatConstant()->GetValue()));
      } else {
        DCHECK(second.IsStackSlot());
        __ divss(first.AsFpuRegister<XmmRegister>(),
                 Address(CpuRegister(RSP), second.GetStackIndex()));
      }
      break;
    }

    case DataType::Type::kFloat64: {
      if (second.IsFpuRegister()) {
        __ divsd(first.AsFpuRegister<XmmRegister>(), second.AsFpuRegister<XmmRegister>());
      } else if (second.IsConstant()) {
        __ divsd(first.AsFpuRegister<XmmRegister>(),
                 codegen_->LiteralDoubleAddress(
                     second.GetConstant()->AsDoubleConstant()->GetValue()));
      } else {
        DCHECK(second.IsDoubleStackSlot());
        __ divsd(first.AsFpuRegister<XmmRegister>(),
                 Address(CpuRegister(RSP), second.GetStackIndex()));
      }
      break;
    }

    default:
      LOG(FATAL) << "Unexpected div type " << div->GetResultType();
  }
}

void LocationsBuilderX86_64::VisitRem(HRem* rem) {
  DataType::Type type = rem->GetResultType();
  LocationSummary* locations =
    new (GetGraph()->GetAllocator()) LocationSummary(rem, LocationSummary::kNoCall);

  switch (type) {
    case DataType::Type::kInt32:
    case DataType::Type::kInt64: {
      locations->SetInAt(0, Location::RegisterLocation(RAX));
      locations->SetInAt(1, Location::RegisterOrConstant(rem->InputAt(1)));
      // Intel uses rdx:rax as the dividend and puts the remainder in rdx
      locations->SetOut(Location::RegisterLocation(RDX));
      // We need to save the numerator while we tweak eax and edx. As we are using imul in a way
      // which enforces results to be in RAX and RDX, things are simpler if we use EAX also as
      // output and request another temp.
      if (rem->InputAt(1)->IsConstant()) {
        locations->AddTemp(Location::RequiresRegister());
      }
      break;
    }

    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64: {
      locations->SetInAt(0, Location::Any());
      locations->SetInAt(1, Location::Any());
      locations->SetOut(Location::RequiresFpuRegister());
      locations->AddTemp(Location::RegisterLocation(RAX));
      break;
    }

    default:
      LOG(FATAL) << "Unexpected rem type " << type;
  }
}

void InstructionCodeGeneratorX86_64::VisitRem(HRem* rem) {
  DataType::Type type = rem->GetResultType();
  switch (type) {
    case DataType::Type::kInt32:
    case DataType::Type::kInt64: {
      GenerateDivRemIntegral(rem);
      break;
    }
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64: {
      GenerateRemFP(rem);
      break;
    }
    default:
      LOG(FATAL) << "Unexpected rem type " << rem->GetResultType();
  }
}

static void CreateMinMaxLocations(ArenaAllocator* allocator, HBinaryOperation* minmax) {
  LocationSummary* locations = new (allocator) LocationSummary(minmax);
  switch (minmax->GetResultType()) {
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RequiresRegister());
      locations->SetOut(Location::SameAsFirstInput());
      break;
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      // The following is sub-optimal, but all we can do for now. It would be fine to also accept
      // the second input to be the output (we can simply swap inputs).
      locations->SetOut(Location::SameAsFirstInput());
      break;
    default:
      LOG(FATAL) << "Unexpected type for HMinMax " << minmax->GetResultType();
  }
}

void InstructionCodeGeneratorX86_64::GenerateMinMaxInt(LocationSummary* locations,
                                                       bool is_min,
                                                       DataType::Type type) {
  Location op1_loc = locations->InAt(0);
  Location op2_loc = locations->InAt(1);

  // Shortcut for same input locations.
  if (op1_loc.Equals(op2_loc)) {
    // Can return immediately, as op1_loc == out_loc.
    // Note: if we ever support separate registers, e.g., output into memory, we need to check for
    //       a copy here.
    DCHECK(locations->Out().Equals(op1_loc));
    return;
  }

  CpuRegister out = locations->Out().AsRegister<CpuRegister>();
  CpuRegister op2 = op2_loc.AsRegister<CpuRegister>();

  //  (out := op1)
  //  out <=? op2
  //  if out is min jmp done
  //  out := op2
  // done:

  if (type == DataType::Type::kInt64) {
    __ cmpq(out, op2);
    __ cmov(is_min ? Condition::kGreater : Condition::kLess, out, op2, /*is64bit*/ true);
  } else {
    DCHECK_EQ(type, DataType::Type::kInt32);
    __ cmpl(out, op2);
    __ cmov(is_min ? Condition::kGreater : Condition::kLess, out, op2, /*is64bit*/ false);
  }
}

void InstructionCodeGeneratorX86_64::GenerateMinMaxFP(LocationSummary* locations,
                                                      bool is_min,
                                                      DataType::Type type) {
  Location op1_loc = locations->InAt(0);
  Location op2_loc = locations->InAt(1);
  Location out_loc = locations->Out();
  XmmRegister out = out_loc.AsFpuRegister<XmmRegister>();

  // Shortcut for same input locations.
  if (op1_loc.Equals(op2_loc)) {
    DCHECK(out_loc.Equals(op1_loc));
    return;
  }

  //  (out := op1)
  //  out <=? op2
  //  if Nan jmp Nan_label
  //  if out is min jmp done
  //  if op2 is min jmp op2_label
  //  handle -0/+0
  //  jmp done
  // Nan_label:
  //  out := NaN
  // op2_label:
  //  out := op2
  // done:
  //
  // This removes one jmp, but needs to copy one input (op1) to out.
  //
  // TODO: This is straight from Quick. Make NaN an out-of-line slowpath?

  XmmRegister op2 = op2_loc.AsFpuRegister<XmmRegister>();

  NearLabel nan, done, op2_label;
  if (type == DataType::Type::kFloat64) {
    __ ucomisd(out, op2);
  } else {
    DCHECK_EQ(type, DataType::Type::kFloat32);
    __ ucomiss(out, op2);
  }

  __ j(Condition::kParityEven, &nan);

  __ j(is_min ? Condition::kAbove : Condition::kBelow, &op2_label);
  __ j(is_min ? Condition::kBelow : Condition::kAbove, &done);

  // Handle 0.0/-0.0.
  if (is_min) {
    if (type == DataType::Type::kFloat64) {
      __ orpd(out, op2);
    } else {
      __ orps(out, op2);
    }
  } else {
    if (type == DataType::Type::kFloat64) {
      __ andpd(out, op2);
    } else {
      __ andps(out, op2);
    }
  }
  __ jmp(&done);

  // NaN handling.
  __ Bind(&nan);
  if (type == DataType::Type::kFloat64) {
    __ movsd(out, codegen_->LiteralInt64Address(INT64_C(0x7FF8000000000000)));
  } else {
    __ movss(out, codegen_->LiteralInt32Address(INT32_C(0x7FC00000)));
  }
  __ jmp(&done);

  // out := op2;
  __ Bind(&op2_label);
  if (type == DataType::Type::kFloat64) {
    __ movsd(out, op2);
  } else {
    __ movss(out, op2);
  }

  // Done.
  __ Bind(&done);
}

void InstructionCodeGeneratorX86_64::GenerateMinMax(HBinaryOperation* minmax, bool is_min) {
  DataType::Type type = minmax->GetResultType();
  switch (type) {
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      GenerateMinMaxInt(minmax->GetLocations(), is_min, type);
      break;
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64:
      GenerateMinMaxFP(minmax->GetLocations(), is_min, type);
      break;
    default:
      LOG(FATAL) << "Unexpected type for HMinMax " << type;
  }
}

void LocationsBuilderX86_64::VisitMin(HMin* min) {
  CreateMinMaxLocations(GetGraph()->GetAllocator(), min);
}

void InstructionCodeGeneratorX86_64::VisitMin(HMin* min) {
  GenerateMinMax(min, /*is_min*/ true);
}

void LocationsBuilderX86_64::VisitMax(HMax* max) {
  CreateMinMaxLocations(GetGraph()->GetAllocator(), max);
}

void InstructionCodeGeneratorX86_64::VisitMax(HMax* max) {
  GenerateMinMax(max, /*is_min*/ false);
}

void LocationsBuilderX86_64::VisitAbs(HAbs* abs) {
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(abs);
  switch (abs->GetResultType()) {
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetOut(Location::SameAsFirstInput());
      locations->AddTemp(Location::RequiresRegister());
      break;
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetOut(Location::SameAsFirstInput());
      locations->AddTemp(Location::RequiresFpuRegister());
      break;
    default:
      LOG(FATAL) << "Unexpected type for HAbs " << abs->GetResultType();
  }
}

void InstructionCodeGeneratorX86_64::VisitAbs(HAbs* abs) {
  LocationSummary* locations = abs->GetLocations();
  switch (abs->GetResultType()) {
    case DataType::Type::kInt32: {
      CpuRegister out = locations->Out().AsRegister<CpuRegister>();
      CpuRegister mask = locations->GetTemp(0).AsRegister<CpuRegister>();
      // Create mask.
      __ movl(mask, out);
      __ sarl(mask, Immediate(31));
      // Add mask.
      __ addl(out, mask);
      __ xorl(out, mask);
      break;
    }
    case DataType::Type::kInt64: {
      CpuRegister out = locations->Out().AsRegister<CpuRegister>();
      CpuRegister mask = locations->GetTemp(0).AsRegister<CpuRegister>();
      // Create mask.
      __ movq(mask, out);
      __ sarq(mask, Immediate(63));
      // Add mask.
      __ addq(out, mask);
      __ xorq(out, mask);
      break;
    }
    case DataType::Type::kFloat32: {
      XmmRegister out = locations->Out().AsFpuRegister<XmmRegister>();
      XmmRegister mask = locations->GetTemp(0).AsFpuRegister<XmmRegister>();
      __ movss(mask, codegen_->LiteralInt32Address(INT32_C(0x7FFFFFFF)));
      __ andps(out, mask);
      break;
    }
    case DataType::Type::kFloat64: {
      XmmRegister out = locations->Out().AsFpuRegister<XmmRegister>();
      XmmRegister mask = locations->GetTemp(0).AsFpuRegister<XmmRegister>();
      __ movsd(mask, codegen_->LiteralInt64Address(INT64_C(0x7FFFFFFFFFFFFFFF)));
      __ andpd(out, mask);
      break;
    }
    default:
      LOG(FATAL) << "Unexpected type for HAbs " << abs->GetResultType();
  }
}

void LocationsBuilderX86_64::VisitDivZeroCheck(HDivZeroCheck* instruction) {
  LocationSummary* locations = codegen_->CreateThrowingSlowPathLocations(instruction);
  locations->SetInAt(0, Location::Any());
}

void InstructionCodeGeneratorX86_64::VisitDivZeroCheck(HDivZeroCheck* instruction) {
  SlowPathCode* slow_path =
      new (codegen_->GetScopedAllocator()) DivZeroCheckSlowPathX86_64(instruction);
  codegen_->AddSlowPath(slow_path);

  LocationSummary* locations = instruction->GetLocations();
  Location value = locations->InAt(0);

  switch (instruction->GetType()) {
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32: {
      if (value.IsRegister()) {
        __ testl(value.AsRegister<CpuRegister>(), value.AsRegister<CpuRegister>());
        __ j(kEqual, slow_path->GetEntryLabel());
      } else if (value.IsStackSlot()) {
        __ cmpl(Address(CpuRegister(RSP), value.GetStackIndex()), Immediate(0));
        __ j(kEqual, slow_path->GetEntryLabel());
      } else {
        DCHECK(value.IsConstant()) << value;
        if (value.GetConstant()->AsIntConstant()->GetValue() == 0) {
          __ jmp(slow_path->GetEntryLabel());
        }
      }
      break;
    }
    case DataType::Type::kInt64: {
      if (value.IsRegister()) {
        __ testq(value.AsRegister<CpuRegister>(), value.AsRegister<CpuRegister>());
        __ j(kEqual, slow_path->GetEntryLabel());
      } else if (value.IsDoubleStackSlot()) {
        __ cmpq(Address(CpuRegister(RSP), value.GetStackIndex()), Immediate(0));
        __ j(kEqual, slow_path->GetEntryLabel());
      } else {
        DCHECK(value.IsConstant()) << value;
        if (value.GetConstant()->AsLongConstant()->GetValue() == 0) {
          __ jmp(slow_path->GetEntryLabel());
        }
      }
      break;
    }
    default:
      LOG(FATAL) << "Unexpected type for HDivZeroCheck " << instruction->GetType();
  }
}

void LocationsBuilderX86_64::HandleShift(HBinaryOperation* op) {
  DCHECK(op->IsShl() || op->IsShr() || op->IsUShr());

  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(op, LocationSummary::kNoCall);

  switch (op->GetResultType()) {
    case DataType::Type::kInt32:
    case DataType::Type::kInt64: {
      locations->SetInAt(0, Location::RequiresRegister());
      // The shift count needs to be in CL.
      locations->SetInAt(1, Location::ByteRegisterOrConstant(RCX, op->InputAt(1)));
      locations->SetOut(Location::SameAsFirstInput());
      break;
    }
    default:
      LOG(FATAL) << "Unexpected operation type " << op->GetResultType();
  }
}

void InstructionCodeGeneratorX86_64::HandleShift(HBinaryOperation* op) {
  DCHECK(op->IsShl() || op->IsShr() || op->IsUShr());

  LocationSummary* locations = op->GetLocations();
  CpuRegister first_reg = locations->InAt(0).AsRegister<CpuRegister>();
  Location second = locations->InAt(1);

  switch (op->GetResultType()) {
    case DataType::Type::kInt32: {
      if (second.IsRegister()) {
        CpuRegister second_reg = second.AsRegister<CpuRegister>();
        if (op->IsShl()) {
          __ shll(first_reg, second_reg);
        } else if (op->IsShr()) {
          __ sarl(first_reg, second_reg);
        } else {
          __ shrl(first_reg, second_reg);
        }
      } else {
        Immediate imm(second.GetConstant()->AsIntConstant()->GetValue() & kMaxIntShiftDistance);
        if (op->IsShl()) {
          __ shll(first_reg, imm);
        } else if (op->IsShr()) {
          __ sarl(first_reg, imm);
        } else {
          __ shrl(first_reg, imm);
        }
      }
      break;
    }
    case DataType::Type::kInt64: {
      if (second.IsRegister()) {
        CpuRegister second_reg = second.AsRegister<CpuRegister>();
        if (op->IsShl()) {
          __ shlq(first_reg, second_reg);
        } else if (op->IsShr()) {
          __ sarq(first_reg, second_reg);
        } else {
          __ shrq(first_reg, second_reg);
        }
      } else {
        Immediate imm(second.GetConstant()->AsIntConstant()->GetValue() & kMaxLongShiftDistance);
        if (op->IsShl()) {
          __ shlq(first_reg, imm);
        } else if (op->IsShr()) {
          __ sarq(first_reg, imm);
        } else {
          __ shrq(first_reg, imm);
        }
      }
      break;
    }
    default:
      LOG(FATAL) << "Unexpected operation type " << op->GetResultType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::HandleRotate(HBinaryOperation* rotate) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(rotate, LocationSummary::kNoCall);

  switch (rotate->GetResultType()) {
    case DataType::Type::kInt32:
    case DataType::Type::kInt64: {
      locations->SetInAt(0, Location::RequiresRegister());
      // The shift count needs to be in CL (unless it is a constant).
      locations->SetInAt(1, Location::ByteRegisterOrConstant(RCX, rotate->InputAt(1)));
      locations->SetOut(Location::SameAsFirstInput());
      break;
    }
    default:
      LOG(FATAL) << "Unexpected operation type " << rotate->GetResultType();
      UNREACHABLE();
  }
}

void InstructionCodeGeneratorX86_64::HandleRotate(HBinaryOperation* rotate) {
  LocationSummary* locations = rotate->GetLocations();
  CpuRegister first_reg = locations->InAt(0).AsRegister<CpuRegister>();
  Location second = locations->InAt(1);

  switch (rotate->GetResultType()) {
    case DataType::Type::kInt32:
      if (second.IsRegister()) {
        CpuRegister second_reg = second.AsRegister<CpuRegister>();
        if (rotate->IsRor()) {
          __ rorl(first_reg, second_reg);
        } else {
          DCHECK(rotate->IsRol());
          __ roll(first_reg, second_reg);
        }
      } else {
        Immediate imm(second.GetConstant()->AsIntConstant()->GetValue() & kMaxIntShiftDistance);
        if (rotate->IsRor()) {
          __ rorl(first_reg, imm);
        } else {
          DCHECK(rotate->IsRol());
          __ roll(first_reg, imm);
        }
      }
      break;
    case DataType::Type::kInt64:
      if (second.IsRegister()) {
        CpuRegister second_reg = second.AsRegister<CpuRegister>();
        if (rotate->IsRor()) {
          __ rorq(first_reg, second_reg);
        } else {
          DCHECK(rotate->IsRol());
          __ rolq(first_reg, second_reg);
        }
      } else {
        Immediate imm(second.GetConstant()->AsIntConstant()->GetValue() & kMaxLongShiftDistance);
        if (rotate->IsRor()) {
          __ rorq(first_reg, imm);
        } else {
          DCHECK(rotate->IsRol());
          __ rolq(first_reg, imm);
        }
      }
      break;
    default:
      LOG(FATAL) << "Unexpected operation type " << rotate->GetResultType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitRol(HRol* rol) {
  HandleRotate(rol);
}

void InstructionCodeGeneratorX86_64::VisitRol(HRol* rol) {
  HandleRotate(rol);
}

void LocationsBuilderX86_64::VisitRor(HRor* ror) {
  HandleRotate(ror);
}

void InstructionCodeGeneratorX86_64::VisitRor(HRor* ror) {
  HandleRotate(ror);
}

void LocationsBuilderX86_64::VisitShl(HShl* shl) {
  HandleShift(shl);
}

void InstructionCodeGeneratorX86_64::VisitShl(HShl* shl) {
  HandleShift(shl);
}

void LocationsBuilderX86_64::VisitShr(HShr* shr) {
  HandleShift(shr);
}

void InstructionCodeGeneratorX86_64::VisitShr(HShr* shr) {
  HandleShift(shr);
}

void LocationsBuilderX86_64::VisitUShr(HUShr* ushr) {
  HandleShift(ushr);
}

void InstructionCodeGeneratorX86_64::VisitUShr(HUShr* ushr) {
  HandleShift(ushr);
}

void LocationsBuilderX86_64::VisitNewInstance(HNewInstance* instruction) {
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(
      instruction, LocationSummary::kCallOnMainOnly);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->SetOut(Location::RegisterLocation(RAX));
}

void InstructionCodeGeneratorX86_64::VisitNewInstance(HNewInstance* instruction) {
  codegen_->InvokeRuntime(instruction->GetEntrypoint(), instruction);
  CheckEntrypointTypes<kQuickAllocObjectWithChecks, void*, mirror::Class*>();
  DCHECK(!codegen_->IsLeafMethod());
}

void LocationsBuilderX86_64::VisitNewArray(HNewArray* instruction) {
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(
      instruction, LocationSummary::kCallOnMainOnly);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetOut(Location::RegisterLocation(RAX));
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
}

void InstructionCodeGeneratorX86_64::VisitNewArray(HNewArray* instruction) {
  // Note: if heap poisoning is enabled, the entry point takes care of poisoning the reference.
  QuickEntrypointEnum entrypoint = CodeGenerator::GetArrayAllocationEntrypoint(instruction);
  codegen_->InvokeRuntime(entrypoint, instruction);
  CheckEntrypointTypes<kQuickAllocArrayResolved, void*, mirror::Class*, int32_t>();
  DCHECK(!codegen_->IsLeafMethod());
}

void LocationsBuilderX86_64::VisitParameterValue(HParameterValue* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(instruction, LocationSummary::kNoCall);
  Location location = parameter_visitor_.GetNextLocation(instruction->GetType());
  if (location.IsStackSlot()) {
    location = Location::StackSlot(location.GetStackIndex() + codegen_->GetFrameSize());
  } else if (location.IsDoubleStackSlot()) {
    location = Location::DoubleStackSlot(location.GetStackIndex() + codegen_->GetFrameSize());
  }
  locations->SetOut(location);
}

void InstructionCodeGeneratorX86_64::VisitParameterValue(
    [[maybe_unused]] HParameterValue* instruction) {
  // Nothing to do, the parameter is already at its location.
}

void LocationsBuilderX86_64::VisitCurrentMethod(HCurrentMethod* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetOut(Location::RegisterLocation(kMethodRegisterArgument));
}

void InstructionCodeGeneratorX86_64::VisitCurrentMethod(
    [[maybe_unused]] HCurrentMethod* instruction) {
  // Nothing to do, the method is already at its location.
}

void LocationsBuilderX86_64::VisitClassTableGet(HClassTableGet* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorX86_64::VisitClassTableGet(HClassTableGet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  if (instruction->GetTableKind() == HClassTableGet::TableKind::kVTable) {
    uint32_t method_offset = mirror::Class::EmbeddedVTableEntryOffset(
        instruction->GetIndex(), kX86_64PointerSize).SizeValue();
    __ movq(locations->Out().AsRegister<CpuRegister>(),
            Address(locations->InAt(0).AsRegister<CpuRegister>(), method_offset));
  } else {
    uint32_t method_offset = static_cast<uint32_t>(ImTable::OffsetOfElement(
        instruction->GetIndex(), kX86_64PointerSize));
    __ movq(locations->Out().AsRegister<CpuRegister>(),
            Address(locations->InAt(0).AsRegister<CpuRegister>(),
            mirror::Class::ImtPtrOffset(kX86_64PointerSize).Uint32Value()));
    __ movq(locations->Out().AsRegister<CpuRegister>(),
            Address(locations->Out().AsRegister<CpuRegister>(), method_offset));
  }
}

void LocationsBuilderX86_64::VisitNot(HNot* not_) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(not_, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::SameAsFirstInput());
}

void InstructionCodeGeneratorX86_64::VisitNot(HNot* not_) {
  LocationSummary* locations = not_->GetLocations();
  DCHECK_EQ(locations->InAt(0).AsRegister<CpuRegister>().AsRegister(),
            locations->Out().AsRegister<CpuRegister>().AsRegister());
  Location out = locations->Out();
  switch (not_->GetResultType()) {
    case DataType::Type::kInt32:
      __ notl(out.AsRegister<CpuRegister>());
      break;

    case DataType::Type::kInt64:
      __ notq(out.AsRegister<CpuRegister>());
      break;

    default:
      LOG(FATAL) << "Unimplemented type for not operation " << not_->GetResultType();
  }
}

void LocationsBuilderX86_64::VisitBooleanNot(HBooleanNot* bool_not) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(bool_not, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::SameAsFirstInput());
}

void InstructionCodeGeneratorX86_64::VisitBooleanNot(HBooleanNot* bool_not) {
  LocationSummary* locations = bool_not->GetLocations();
  DCHECK_EQ(locations->InAt(0).AsRegister<CpuRegister>().AsRegister(),
            locations->Out().AsRegister<CpuRegister>().AsRegister());
  Location out = locations->Out();
  __ xorl(out.AsRegister<CpuRegister>(), Immediate(1));
}

void LocationsBuilderX86_64::VisitPhi(HPhi* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(instruction, LocationSummary::kNoCall);
  for (size_t i = 0, e = locations->GetInputCount(); i < e; ++i) {
    locations->SetInAt(i, Location::Any());
  }
  locations->SetOut(Location::Any());
}

void InstructionCodeGeneratorX86_64::VisitPhi([[maybe_unused]] HPhi* instruction) {
  LOG(FATAL) << "Unimplemented";
}

void CodeGeneratorX86_64::GenerateMemoryBarrier(MemBarrierKind kind) {
  /*
   * According to the JSR-133 Cookbook, for x86-64 only StoreLoad/AnyAny barriers need memory fence.
   * All other barriers (LoadAny, AnyStore, StoreStore) are nops due to the x86-64 memory model.
   * For those cases, all we need to ensure is that there is a scheduling barrier in place.
   */
  switch (kind) {
    case MemBarrierKind::kAnyAny: {
      MemoryFence();
      break;
    }
    case MemBarrierKind::kAnyStore:
    case MemBarrierKind::kLoadAny:
    case MemBarrierKind::kStoreStore: {
      // nop
      break;
    }
    case MemBarrierKind::kNTStoreStore:
      // Non-Temporal Store/Store needs an explicit fence.
      MemoryFence(/* non-temporal= */ true);
      break;
  }
}

void LocationsBuilderX86_64::HandleFieldGet(HInstruction* instruction) {
  DCHECK(instruction->IsInstanceFieldGet() || instruction->IsStaticFieldGet());

  bool object_field_get_with_read_barrier =
      (instruction->GetType() == DataType::Type::kReference) && codegen_->EmitReadBarrier();
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(instruction,
                                                       object_field_get_with_read_barrier
                                                           ? LocationSummary::kCallOnSlowPath
                                                           : LocationSummary::kNoCall);
  if (object_field_get_with_read_barrier && kUseBakerReadBarrier) {
    locations->SetCustomSlowPathCallerSaves(RegisterSet::Empty());  // No caller-save registers.
  }
  // receiver_input
  locations->SetInAt(0, Location::RequiresRegister());
  if (DataType::IsFloatingPointType(instruction->GetType())) {
    locations->SetOut(Location::RequiresFpuRegister());
  } else {
    // The output overlaps for an object field get when read barriers are
    // enabled: we do not want the move to overwrite the object's location, as
    // we need it to emit the read barrier. For predicated instructions we can
    // always overlap since the output is SameAsFirst and the default value.
    locations->SetOut(
        Location::RequiresRegister(),
        object_field_get_with_read_barrier ? Location::kOutputOverlap : Location::kNoOutputOverlap);
  }
}

void InstructionCodeGeneratorX86_64::HandleFieldGet(HInstruction* instruction,
                                                    const FieldInfo& field_info) {
  DCHECK(instruction->IsInstanceFieldGet() || instruction->IsStaticFieldGet());

  LocationSummary* locations = instruction->GetLocations();
  Location base_loc = locations->InAt(0);
  CpuRegister base = base_loc.AsRegister<CpuRegister>();
  Location out = locations->Out();
  bool is_volatile = field_info.IsVolatile();
  DCHECK_EQ(DataType::Size(field_info.GetFieldType()), DataType::Size(instruction->GetType()));
  DataType::Type load_type = instruction->GetType();
  uint32_t offset = field_info.GetFieldOffset().Uint32Value();

  if (load_type == DataType::Type::kReference) {
    // /* HeapReference<Object> */ out = *(base + offset)
    if (codegen_->EmitBakerReadBarrier()) {
      // Note that a potential implicit null check is handled in this
      // CodeGeneratorX86_64::GenerateFieldLoadWithBakerReadBarrier call.
      codegen_->GenerateFieldLoadWithBakerReadBarrier(
          instruction, out, base, offset, /* needs_null_check= */ true);
      if (is_volatile) {
        codegen_->GenerateMemoryBarrier(MemBarrierKind::kLoadAny);
      }
    } else {
      __ movl(out.AsRegister<CpuRegister>(), Address(base, offset));
      codegen_->MaybeRecordImplicitNullCheck(instruction);
      if (is_volatile) {
        codegen_->GenerateMemoryBarrier(MemBarrierKind::kLoadAny);
      }
      // If read barriers are enabled, emit read barriers other than
      // Baker's using a slow path (and also unpoison the loaded
      // reference, if heap poisoning is enabled).
      codegen_->MaybeGenerateReadBarrierSlow(instruction, out, out, base_loc, offset);
    }
  } else {
    codegen_->LoadFromMemoryNoReference(load_type, out, Address(base, offset));
    codegen_->MaybeRecordImplicitNullCheck(instruction);
    if (is_volatile) {
      codegen_->GenerateMemoryBarrier(MemBarrierKind::kLoadAny);
    }
  }
}

void LocationsBuilderX86_64::HandleFieldSet(HInstruction* instruction,
                                            const FieldInfo& field_info,
                                            WriteBarrierKind write_barrier_kind) {
  DCHECK(instruction->IsInstanceFieldSet() || instruction->IsStaticFieldSet());

  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(instruction, LocationSummary::kNoCall);
  DataType::Type field_type = field_info.GetFieldType();
  bool is_volatile = field_info.IsVolatile();
  bool needs_write_barrier =
      codegen_->StoreNeedsWriteBarrier(field_type, instruction->InputAt(1), write_barrier_kind);
  bool check_gc_card =
      codegen_->ShouldCheckGCCard(field_type, instruction->InputAt(1), write_barrier_kind);

  locations->SetInAt(0, Location::RequiresRegister());
  if (DataType::IsFloatingPointType(instruction->InputAt(1)->GetType())) {
    if (is_volatile) {
      // In order to satisfy the semantics of volatile, this must be a single instruction store.
      locations->SetInAt(1, Location::FpuRegisterOrInt32Constant(instruction->InputAt(1)));
    } else {
      locations->SetInAt(1, Location::FpuRegisterOrConstant(instruction->InputAt(1)));
    }
  } else {
    if (is_volatile) {
      // In order to satisfy the semantics of volatile, this must be a single instruction store.
      locations->SetInAt(1, Location::RegisterOrInt32Constant(instruction->InputAt(1)));
    } else {
      locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
    }
  }

  // TODO(solanes): We could reduce the temp usage but it requires some non-trivial refactoring of
  // InstructionCodeGeneratorX86_64::HandleFieldSet, GenerateVarHandleSet due to `extra_temp_index`.
  if (needs_write_barrier ||
      check_gc_card ||
      (kPoisonHeapReferences && field_type == DataType::Type::kReference)) {
    // Temporary registers for the write barrier / reference poisoning.
    locations->AddRegisterTemps(2);
  }
}

void InstructionCodeGeneratorX86_64::Bswap(Location value,
                                           DataType::Type type,
                                           CpuRegister* temp) {
  switch (type) {
    case DataType::Type::kInt16:
      // This should sign-extend, even if reimplemented with an XCHG of 8-bit registers.
      __ bswapl(value.AsRegister<CpuRegister>());
      __ sarl(value.AsRegister<CpuRegister>(), Immediate(16));
      break;
    case DataType::Type::kUint16:
      // TODO: Can be done with an XCHG of 8-bit registers. This is straight from Quick.
      __ bswapl(value.AsRegister<CpuRegister>());
      __ shrl(value.AsRegister<CpuRegister>(), Immediate(16));
      break;
    case DataType::Type::kInt32:
    case DataType::Type::kUint32:
      __ bswapl(value.AsRegister<CpuRegister>());
      break;
    case DataType::Type::kInt64:
    case DataType::Type::kUint64:
      __ bswapq(value.AsRegister<CpuRegister>());
      break;
    case DataType::Type::kFloat32: {
      DCHECK_NE(temp, nullptr);
      __ movd(*temp, value.AsFpuRegister<XmmRegister>());
      __ bswapl(*temp);
      __ movd(value.AsFpuRegister<XmmRegister>(), *temp);
      break;
    }
    case DataType::Type::kFloat64: {
      DCHECK_NE(temp, nullptr);
      __ movq(*temp, value.AsFpuRegister<XmmRegister>());
      __ bswapq(*temp);
      __ movq(value.AsFpuRegister<XmmRegister>(), *temp);
      break;
    }
    default:
      LOG(FATAL) << "Unexpected type for reverse-bytes: " << type;
      UNREACHABLE();
  }
}

void InstructionCodeGeneratorX86_64::HandleFieldSet(HInstruction* instruction,
                                                    uint32_t value_index,
                                                    uint32_t extra_temp_index,
                                                    DataType::Type field_type,
                                                    Address field_addr,
                                                    CpuRegister base,
                                                    bool is_volatile,
                                                    bool is_atomic,
                                                    bool value_can_be_null,
                                                    bool byte_swap,
                                                    WriteBarrierKind write_barrier_kind) {
  LocationSummary* locations = instruction->GetLocations();
  Location value = locations->InAt(value_index);

  if (is_volatile) {
    codegen_->GenerateMemoryBarrier(MemBarrierKind::kAnyStore);
  }

  bool maybe_record_implicit_null_check_done = false;

  if (value.IsConstant()) {
    switch (field_type) {
      case DataType::Type::kBool:
      case DataType::Type::kUint8:
      case DataType::Type::kInt8:
        __ movb(field_addr, Immediate(CodeGenerator::GetInt8ValueOf(value.GetConstant())));
        break;
      case DataType::Type::kUint16:
      case DataType::Type::kInt16: {
        int16_t v = CodeGenerator::GetInt16ValueOf(value.GetConstant());
        if (byte_swap) {
          v = BSWAP(v);
        }
        __ movw(field_addr, Immediate(v));
        break;
      }
      case DataType::Type::kUint32:
      case DataType::Type::kInt32:
      case DataType::Type::kFloat32:
      case DataType::Type::kReference: {
        int32_t v = CodeGenerator::GetInt32ValueOf(value.GetConstant());
        if (byte_swap) {
          v = BSWAP(v);
        }
        DCHECK_IMPLIES(field_type == DataType::Type::kReference, v == 0);
        // Note: if heap poisoning is enabled, no need to poison
        // (negate) `v` if it is a reference, as it would be null.
        __ movl(field_addr, Immediate(v));
        break;
      }
      case DataType::Type::kUint64:
      case DataType::Type::kInt64:
      case DataType::Type::kFloat64: {
        int64_t v = CodeGenerator::GetInt64ValueOf(value.GetConstant());
        if (byte_swap) {
          v = BSWAP(v);
        }
        if (is_atomic) {
          // Move constant into a register, then atomically store the register to memory.
          CpuRegister temp = locations->GetTemp(extra_temp_index).AsRegister<CpuRegister>();
          __ movq(temp, Immediate(v));
          __ movq(field_addr, temp);
        } else {
          Address field_addr2 = Address::displace(field_addr, sizeof(int32_t));
          codegen_->MoveInt64ToAddress(field_addr, field_addr2, v, instruction);
        }
        maybe_record_implicit_null_check_done = true;
        break;
      }
      case DataType::Type::kVoid:
        LOG(FATAL) << "Unreachable type " << field_type;
        UNREACHABLE();
    }
  } else {
    if (byte_swap) {
      // Swap byte order in-place in the input register (we will restore it later).
      CpuRegister temp = locations->GetTemp(extra_temp_index).AsRegister<CpuRegister>();
      Bswap(value, field_type, &temp);
    }

    switch (field_type) {
      case DataType::Type::kBool:
      case DataType::Type::kUint8:
      case DataType::Type::kInt8:
        __ movb(field_addr, value.AsRegister<CpuRegister>());
        break;
      case DataType::Type::kUint16:
      case DataType::Type::kInt16:
        __ movw(field_addr, value.AsRegister<CpuRegister>());
        break;
      case DataType::Type::kUint32:
      case DataType::Type::kInt32:
      case DataType::Type::kReference:
        if (kPoisonHeapReferences && field_type == DataType::Type::kReference) {
          CpuRegister temp = locations->GetTemp(extra_temp_index).AsRegister<CpuRegister>();
          __ movl(temp, value.AsRegister<CpuRegister>());
          __ PoisonHeapReference(temp);
          __ movl(field_addr, temp);
        } else {
          __ movl(field_addr, value.AsRegister<CpuRegister>());
        }
        break;
      case DataType::Type::kUint64:
      case DataType::Type::kInt64:
        __ movq(field_addr, value.AsRegister<CpuRegister>());
        break;
      case DataType::Type::kFloat32:
        __ movss(field_addr, value.AsFpuRegister<XmmRegister>());
        break;
      case DataType::Type::kFloat64:
        __ movsd(field_addr, value.AsFpuRegister<XmmRegister>());
        break;
      case DataType::Type::kVoid:
        LOG(FATAL) << "Unreachable type " << field_type;
        UNREACHABLE();
    }

    if (byte_swap) {
      // Restore byte order.
      CpuRegister temp = locations->GetTemp(extra_temp_index).AsRegister<CpuRegister>();
      Bswap(value, field_type, &temp);
    }
  }

  if (!maybe_record_implicit_null_check_done) {
    codegen_->MaybeRecordImplicitNullCheck(instruction);
  }

  bool needs_write_barrier =
      codegen_->StoreNeedsWriteBarrier(field_type, instruction->InputAt(1), write_barrier_kind);
  if (needs_write_barrier) {
    if (value.IsConstant()) {
      DCHECK(value.GetConstant()->IsNullConstant());
      if (write_barrier_kind == WriteBarrierKind::kEmitBeingReliedOn) {
        DCHECK_NE(extra_temp_index, 0u);
        CpuRegister temp = locations->GetTemp(0).AsRegister<CpuRegister>();
        CpuRegister card = locations->GetTemp(extra_temp_index).AsRegister<CpuRegister>();
        codegen_->MarkGCCard(temp, card, base);
      }
    } else {
      DCHECK_NE(extra_temp_index, 0u);
      CpuRegister temp = locations->GetTemp(0).AsRegister<CpuRegister>();
      CpuRegister card = locations->GetTemp(extra_temp_index).AsRegister<CpuRegister>();
      codegen_->MaybeMarkGCCard(
          temp,
          card,
          base,
          value.AsRegister<CpuRegister>(),
          value_can_be_null && write_barrier_kind == WriteBarrierKind::kEmitNotBeingReliedOn);
    }
  } else if (codegen_->ShouldCheckGCCard(
                 field_type, instruction->InputAt(value_index), write_barrier_kind)) {
    DCHECK_NE(extra_temp_index, 0u);
    DCHECK(value.IsRegister());
    CpuRegister temp = locations->GetTemp(0).AsRegister<CpuRegister>();
    CpuRegister card = locations->GetTemp(extra_temp_index).AsRegister<CpuRegister>();
    codegen_->CheckGCCardIsValid(temp, card, base);
  }

  if (is_volatile) {
    codegen_->GenerateMemoryBarrier(MemBarrierKind::kAnyAny);
  }
}

void InstructionCodeGeneratorX86_64::HandleFieldSet(HInstruction* instruction,
                                                    const FieldInfo& field_info,
                                                    bool value_can_be_null,
                                                    WriteBarrierKind write_barrier_kind) {
  DCHECK(instruction->IsInstanceFieldSet() || instruction->IsStaticFieldSet());

  LocationSummary* locations = instruction->GetLocations();
  CpuRegister base = locations->InAt(0).AsRegister<CpuRegister>();
  bool is_volatile = field_info.IsVolatile();
  DataType::Type field_type = field_info.GetFieldType();
  uint32_t offset = field_info.GetFieldOffset().Uint32Value();

  HandleFieldSet(instruction,
                 /*value_index=*/ 1,
                 /*extra_temp_index=*/ 1,
                 field_type,
                 Address(base, offset),
                 base,
                 is_volatile,
                 /*is_atomic=*/ false,
                 value_can_be_null,
                 /*byte_swap=*/ false,
                 write_barrier_kind);
}

void LocationsBuilderX86_64::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  HandleFieldSet(instruction, instruction->GetFieldInfo(), instruction->GetWriteBarrierKind());
}

void InstructionCodeGeneratorX86_64::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  HandleFieldSet(instruction,
                 instruction->GetFieldInfo(),
                 instruction->GetValueCanBeNull(),
                 instruction->GetWriteBarrierKind());
}

void LocationsBuilderX86_64::VisitInstanceFieldGet(HInstanceFieldGet* instruction) {
  HandleFieldGet(instruction);
}

void InstructionCodeGeneratorX86_64::VisitInstanceFieldGet(HInstanceFieldGet* instruction) {
  HandleFieldGet(instruction, instruction->GetFieldInfo());
}

void LocationsBuilderX86_64::VisitStaticFieldGet(HStaticFieldGet* instruction) {
  HandleFieldGet(instruction);
}

void InstructionCodeGeneratorX86_64::VisitStaticFieldGet(HStaticFieldGet* instruction) {
  HandleFieldGet(instruction, instruction->GetFieldInfo());
}

void LocationsBuilderX86_64::VisitStaticFieldSet(HStaticFieldSet* instruction) {
  HandleFieldSet(instruction, instruction->GetFieldInfo(), instruction->GetWriteBarrierKind());
}

void InstructionCodeGeneratorX86_64::VisitStaticFieldSet(HStaticFieldSet* instruction) {
  HandleFieldSet(instruction,
                 instruction->GetFieldInfo(),
                 instruction->GetValueCanBeNull(),
                 instruction->GetWriteBarrierKind());
}

void LocationsBuilderX86_64::VisitStringBuilderAppend(HStringBuilderAppend* instruction) {
  codegen_->CreateStringBuilderAppendLocations(instruction, Location::RegisterLocation(RAX));
}

void InstructionCodeGeneratorX86_64::VisitStringBuilderAppend(HStringBuilderAppend* instruction) {
  __ movl(CpuRegister(RDI), Immediate(instruction->GetFormat()->GetValue()));
  codegen_->InvokeRuntime(kQuickStringBuilderAppend, instruction);
}

void LocationsBuilderX86_64::VisitUnresolvedInstanceFieldGet(
    HUnresolvedInstanceFieldGet* instruction) {
  FieldAccessCallingConventionX86_64 calling_convention;
  codegen_->CreateUnresolvedFieldLocationSummary(
      instruction, instruction->GetFieldType(), calling_convention);
}

void InstructionCodeGeneratorX86_64::VisitUnresolvedInstanceFieldGet(
    HUnresolvedInstanceFieldGet* instruction) {
  FieldAccessCallingConventionX86_64 calling_convention;
  codegen_->GenerateUnresolvedFieldAccess(instruction,
                                          instruction->GetFieldType(),
                                          instruction->GetFieldIndex(),
                                          calling_convention);
}

void LocationsBuilderX86_64::VisitUnresolvedInstanceFieldSet(
    HUnresolvedInstanceFieldSet* instruction) {
  FieldAccessCallingConventionX86_64 calling_convention;
  codegen_->CreateUnresolvedFieldLocationSummary(
      instruction, instruction->GetFieldType(), calling_convention);
}

void InstructionCodeGeneratorX86_64::VisitUnresolvedInstanceFieldSet(
    HUnresolvedInstanceFieldSet* instruction) {
  FieldAccessCallingConventionX86_64 calling_convention;
  codegen_->GenerateUnresolvedFieldAccess(instruction,
                                          instruction->GetFieldType(),
                                          instruction->GetFieldIndex(),
                                          calling_convention);
}

void LocationsBuilderX86_64::VisitUnresolvedStaticFieldGet(
    HUnresolvedStaticFieldGet* instruction) {
  FieldAccessCallingConventionX86_64 calling_convention;
  codegen_->CreateUnresolvedFieldLocationSummary(
      instruction, instruction->GetFieldType(), calling_convention);
}

void InstructionCodeGeneratorX86_64::VisitUnresolvedStaticFieldGet(
    HUnresolvedStaticFieldGet* instruction) {
  FieldAccessCallingConventionX86_64 calling_convention;
  codegen_->GenerateUnresolvedFieldAccess(instruction,
                                          instruction->GetFieldType(),
                                          instruction->GetFieldIndex(),
                                          calling_convention);
}

void LocationsBuilderX86_64::VisitUnresolvedStaticFieldSet(
    HUnresolvedStaticFieldSet* instruction) {
  FieldAccessCallingConventionX86_64 calling_convention;
  codegen_->CreateUnresolvedFieldLocationSummary(
      instruction, instruction->GetFieldType(), calling_convention);
}

void InstructionCodeGeneratorX86_64::VisitUnresolvedStaticFieldSet(
    HUnresolvedStaticFieldSet* instruction) {
  FieldAccessCallingConventionX86_64 calling_convention;
  codegen_->GenerateUnresolvedFieldAccess(instruction,
                                          instruction->GetFieldType(),
                                          instruction->GetFieldIndex(),
                                          calling_convention);
}

void LocationsBuilderX86_64::VisitNullCheck(HNullCheck* instruction) {
  LocationSummary* locations = codegen_->CreateThrowingSlowPathLocations(instruction);
  Location loc = codegen_->GetCompilerOptions().GetImplicitNullChecks()
      ? Location::RequiresRegister()
      : Location::Any();
  locations->SetInAt(0, loc);
}

void CodeGeneratorX86_64::GenerateImplicitNullCheck(HNullCheck* instruction) {
  if (CanMoveNullCheckToUser(instruction)) {
    return;
  }
  LocationSummary* locations = instruction->GetLocations();
  Location obj = locations->InAt(0);

  __ testl(CpuRegister(RAX), Address(obj.AsRegister<CpuRegister>(), 0));
  RecordPcInfo(instruction);
}

void CodeGeneratorX86_64::GenerateExplicitNullCheck(HNullCheck* instruction) {
  SlowPathCode* slow_path = new (GetScopedAllocator()) NullCheckSlowPathX86_64(instruction);
  AddSlowPath(slow_path);

  LocationSummary* locations = instruction->GetLocations();
  Location obj = locations->InAt(0);

  if (obj.IsRegister()) {
    __ testl(obj.AsRegister<CpuRegister>(), obj.AsRegister<CpuRegister>());
  } else if (obj.IsStackSlot()) {
    __ cmpl(Address(CpuRegister(RSP), obj.GetStackIndex()), Immediate(0));
  } else {
    DCHECK(obj.IsConstant()) << obj;
    DCHECK(obj.GetConstant()->IsNullConstant());
    __ jmp(slow_path->GetEntryLabel());
    return;
  }
  __ j(kEqual, slow_path->GetEntryLabel());
}

void InstructionCodeGeneratorX86_64::VisitNullCheck(HNullCheck* instruction) {
  codegen_->GenerateNullCheck(instruction);
}

void LocationsBuilderX86_64::VisitArrayGet(HArrayGet* instruction) {
  bool object_array_get_with_read_barrier =
      (instruction->GetType() == DataType::Type::kReference) && codegen_->EmitReadBarrier();
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(instruction,
                                                       object_array_get_with_read_barrier
                                                           ? LocationSummary::kCallOnSlowPath
                                                           : LocationSummary::kNoCall);
  if (object_array_get_with_read_barrier && kUseBakerReadBarrier) {
    locations->SetCustomSlowPathCallerSaves(RegisterSet::Empty());  // No caller-save registers.
  }
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
  if (DataType::IsFloatingPointType(instruction->GetType())) {
    locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
  } else {
    // The output overlaps for an object array get when read barriers
    // are enabled: we do not want the move to overwrite the array's
    // location, as we need it to emit the read barrier.
    locations->SetOut(
        Location::RequiresRegister(),
        object_array_get_with_read_barrier ? Location::kOutputOverlap : Location::kNoOutputOverlap);
  }
}

void InstructionCodeGeneratorX86_64::VisitArrayGet(HArrayGet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Location obj_loc = locations->InAt(0);
  CpuRegister obj = obj_loc.AsRegister<CpuRegister>();
  Location index = locations->InAt(1);
  Location out_loc = locations->Out();
  uint32_t data_offset = CodeGenerator::GetArrayDataOffset(instruction);

  DataType::Type type = instruction->GetType();
  if (type == DataType::Type::kReference) {
    static_assert(
        sizeof(mirror::HeapReference<mirror::Object>) == sizeof(int32_t),
        "art::mirror::HeapReference<art::mirror::Object> and int32_t have different sizes.");
    // /* HeapReference<Object> */ out =
    //     *(obj + data_offset + index * sizeof(HeapReference<Object>))
    if (codegen_->EmitBakerReadBarrier()) {
      // Note that a potential implicit null check is handled in this
      // CodeGeneratorX86_64::GenerateArrayLoadWithBakerReadBarrier call.
      codegen_->GenerateArrayLoadWithBakerReadBarrier(
          instruction, out_loc, obj, data_offset, index, /* needs_null_check= */ true);
    } else {
      CpuRegister out = out_loc.AsRegister<CpuRegister>();
      __ movl(out, CodeGeneratorX86_64::ArrayAddress(obj, index, TIMES_4, data_offset));
      codegen_->MaybeRecordImplicitNullCheck(instruction);
      // If read barriers are enabled, emit read barriers other than
      // Baker's using a slow path (and also unpoison the loaded
      // reference, if heap poisoning is enabled).
      if (index.IsConstant()) {
        uint32_t offset =
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + data_offset;
        codegen_->MaybeGenerateReadBarrierSlow(instruction, out_loc, out_loc, obj_loc, offset);
      } else {
        codegen_->MaybeGenerateReadBarrierSlow(
            instruction, out_loc, out_loc, obj_loc, data_offset, index);
      }
    }
  } else {
    if (type == DataType::Type::kUint16
        && mirror::kUseStringCompression
        && instruction->IsStringCharAt()) {
      // Branch cases into compressed and uncompressed for each index's type.
      CpuRegister out = out_loc.AsRegister<CpuRegister>();
      uint32_t count_offset = mirror::String::CountOffset().Uint32Value();
      NearLabel done, not_compressed;
      __ testb(Address(obj, count_offset), Immediate(1));
      codegen_->MaybeRecordImplicitNullCheck(instruction);
      static_assert(static_cast<uint32_t>(mirror::StringCompressionFlag::kCompressed) == 0u,
                    "Expecting 0=compressed, 1=uncompressed");
      __ j(kNotZero, &not_compressed);
      __ movzxb(out, CodeGeneratorX86_64::ArrayAddress(obj, index, TIMES_1, data_offset));
      __ jmp(&done);
      __ Bind(&not_compressed);
      __ movzxw(out, CodeGeneratorX86_64::ArrayAddress(obj, index, TIMES_2, data_offset));
      __ Bind(&done);
    } else {
      ScaleFactor scale = CodeGenerator::ScaleFactorForType(type);
      Address src = CodeGeneratorX86_64::ArrayAddress(obj, index, scale, data_offset);
      codegen_->LoadFromMemoryNoReference(type, out_loc, src);
    }
    codegen_->MaybeRecordImplicitNullCheck(instruction);
  }
}

void LocationsBuilderX86_64::VisitArraySet(HArraySet* instruction) {
  DataType::Type value_type = instruction->GetComponentType();

  WriteBarrierKind write_barrier_kind = instruction->GetWriteBarrierKind();
  bool needs_write_barrier =
      codegen_->StoreNeedsWriteBarrier(value_type, instruction->GetValue(), write_barrier_kind);
  bool check_gc_card =
      codegen_->ShouldCheckGCCard(value_type, instruction->GetValue(), write_barrier_kind);
  bool needs_type_check = instruction->NeedsTypeCheck();

  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(
      instruction,
      needs_type_check ? LocationSummary::kCallOnSlowPath : LocationSummary::kNoCall);

  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
  if (DataType::IsFloatingPointType(value_type)) {
    locations->SetInAt(2, Location::FpuRegisterOrConstant(instruction->InputAt(2)));
  } else {
    locations->SetInAt(2, Location::RegisterOrConstant(instruction->InputAt(2)));
  }

  if (needs_write_barrier || check_gc_card) {
    // Used by reference poisoning, type checking, emitting write barrier, or checking write
    // barrier.
    locations->AddTemp(Location::RequiresRegister());
    // Only used when emitting a write barrier, or when checking for the card table.
    locations->AddTemp(Location::RequiresRegister());
  } else if ((kPoisonHeapReferences && value_type == DataType::Type::kReference) ||
             instruction->NeedsTypeCheck()) {
    // Used for poisoning or type checking.
    locations->AddTemp(Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorX86_64::VisitArraySet(HArraySet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Location array_loc = locations->InAt(0);
  CpuRegister array = array_loc.AsRegister<CpuRegister>();
  Location index = locations->InAt(1);
  Location value = locations->InAt(2);
  DataType::Type value_type = instruction->GetComponentType();
  bool needs_type_check = instruction->NeedsTypeCheck();
  const WriteBarrierKind write_barrier_kind = instruction->GetWriteBarrierKind();
  bool needs_write_barrier =
      codegen_->StoreNeedsWriteBarrier(value_type, instruction->GetValue(), write_barrier_kind);

  switch (value_type) {
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
    case DataType::Type::kInt8: {
      uint32_t offset = mirror::Array::DataOffset(sizeof(uint8_t)).Uint32Value();
      Address address = CodeGeneratorX86_64::ArrayAddress(array, index, TIMES_1, offset);
      if (value.IsRegister()) {
        __ movb(address, value.AsRegister<CpuRegister>());
      } else {
        __ movb(address, Immediate(CodeGenerator::GetInt8ValueOf(value.GetConstant())));
      }
      codegen_->MaybeRecordImplicitNullCheck(instruction);
      break;
    }

    case DataType::Type::kUint16:
    case DataType::Type::kInt16: {
      uint32_t offset = mirror::Array::DataOffset(sizeof(uint16_t)).Uint32Value();
      Address address = CodeGeneratorX86_64::ArrayAddress(array, index, TIMES_2, offset);
      if (value.IsRegister()) {
        __ movw(address, value.AsRegister<CpuRegister>());
      } else {
        DCHECK(value.IsConstant()) << value;
        __ movw(address, Immediate(CodeGenerator::GetInt16ValueOf(value.GetConstant())));
      }
      codegen_->MaybeRecordImplicitNullCheck(instruction);
      break;
    }

    case DataType::Type::kReference: {
      uint32_t offset = mirror::Array::DataOffset(sizeof(int32_t)).Uint32Value();
      Address address = CodeGeneratorX86_64::ArrayAddress(array, index, TIMES_4, offset);

      if (!value.IsRegister()) {
        // Just setting null.
        DCHECK(instruction->InputAt(2)->IsNullConstant());
        DCHECK(value.IsConstant()) << value;
        __ movl(address, Immediate(0));
        codegen_->MaybeRecordImplicitNullCheck(instruction);
        if (write_barrier_kind == WriteBarrierKind::kEmitBeingReliedOn) {
          // We need to set a write barrier here even though we are writing null, since this write
          // barrier is being relied on.
          DCHECK(needs_write_barrier);
          CpuRegister temp = locations->GetTemp(0).AsRegister<CpuRegister>();
          CpuRegister card = locations->GetTemp(1).AsRegister<CpuRegister>();
          codegen_->MarkGCCard(temp, card, array);
        }
        DCHECK(!needs_type_check);
        break;
      }

      CpuRegister register_value = value.AsRegister<CpuRegister>();
      const bool can_value_be_null = instruction->GetValueCanBeNull();
      // The WriteBarrierKind::kEmitNotBeingReliedOn case is able to skip the write barrier when its
      // value is null (without an extra CompareAndBranchIfZero since we already checked if the
      // value is null for the type check).
      const bool skip_marking_gc_card =
          can_value_be_null && write_barrier_kind == WriteBarrierKind::kEmitNotBeingReliedOn;
      NearLabel do_store;
      NearLabel skip_writing_card;
      if (can_value_be_null) {
        __ testl(register_value, register_value);
        if (skip_marking_gc_card) {
          __ j(kEqual, &skip_writing_card);
        } else {
          __ j(kEqual, &do_store);
        }
      }

      SlowPathCode* slow_path = nullptr;
      if (needs_type_check) {
        slow_path = new (codegen_->GetScopedAllocator()) ArraySetSlowPathX86_64(instruction);
        codegen_->AddSlowPath(slow_path);

        const uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();
        const uint32_t super_offset = mirror::Class::SuperClassOffset().Int32Value();
        const uint32_t component_offset = mirror::Class::ComponentTypeOffset().Int32Value();

        // Note that when Baker read barriers are enabled, the type
        // checks are performed without read barriers.  This is fine,
        // even in the case where a class object is in the from-space
        // after the flip, as a comparison involving such a type would
        // not produce a false positive; it may of course produce a
        // false negative, in which case we would take the ArraySet
        // slow path.

        CpuRegister temp = locations->GetTemp(0).AsRegister<CpuRegister>();
        // /* HeapReference<Class> */ temp = array->klass_
        __ movl(temp, Address(array, class_offset));
        codegen_->MaybeRecordImplicitNullCheck(instruction);
        __ MaybeUnpoisonHeapReference(temp);

        // /* HeapReference<Class> */ temp = temp->component_type_
        __ movl(temp, Address(temp, component_offset));
        // If heap poisoning is enabled, no need to unpoison `temp`
        // nor the object reference in `register_value->klass`, as
        // we are comparing two poisoned references.
        __ cmpl(temp, Address(register_value, class_offset));

        if (instruction->StaticTypeOfArrayIsObjectArray()) {
          NearLabel do_put;
          __ j(kEqual, &do_put);
          // If heap poisoning is enabled, the `temp` reference has
          // not been unpoisoned yet; unpoison it now.
          __ MaybeUnpoisonHeapReference(temp);

          // If heap poisoning is enabled, no need to unpoison the
          // heap reference loaded below, as it is only used for a
          // comparison with null.
          __ cmpl(Address(temp, super_offset), Immediate(0));
          __ j(kNotEqual, slow_path->GetEntryLabel());
          __ Bind(&do_put);
        } else {
          __ j(kNotEqual, slow_path->GetEntryLabel());
        }
      }

      if (can_value_be_null && !skip_marking_gc_card) {
        DCHECK(do_store.IsLinked());
        __ Bind(&do_store);
      }

      if (needs_write_barrier) {
        CpuRegister temp = locations->GetTemp(0).AsRegister<CpuRegister>();
        CpuRegister card = locations->GetTemp(1).AsRegister<CpuRegister>();
        codegen_->MarkGCCard(temp, card, array);
      } else if (codegen_->ShouldCheckGCCard(
                     value_type, instruction->GetValue(), write_barrier_kind)) {
        CpuRegister temp = locations->GetTemp(0).AsRegister<CpuRegister>();
        CpuRegister card = locations->GetTemp(1).AsRegister<CpuRegister>();
        codegen_->CheckGCCardIsValid(temp, card, array);
      }

      if (skip_marking_gc_card) {
        // Note that we don't check that the GC card is valid as it can be correctly clean.
        DCHECK(skip_writing_card.IsLinked());
        __ Bind(&skip_writing_card);
      }

      Location source = value;
      if (kPoisonHeapReferences) {
        Location temp_loc = locations->GetTemp(0);
        CpuRegister temp = temp_loc.AsRegister<CpuRegister>();
        __ movl(temp, register_value);
        __ PoisonHeapReference(temp);
        source = temp_loc;
      }

      __ movl(address, source.AsRegister<CpuRegister>());

      if (can_value_be_null || !needs_type_check) {
        codegen_->MaybeRecordImplicitNullCheck(instruction);
      }

      if (slow_path != nullptr) {
        __ Bind(slow_path->GetExitLabel());
      }

      break;
    }

    case DataType::Type::kInt32: {
      uint32_t offset = mirror::Array::DataOffset(sizeof(int32_t)).Uint32Value();
      Address address = CodeGeneratorX86_64::ArrayAddress(array, index, TIMES_4, offset);
      if (value.IsRegister()) {
        __ movl(address, value.AsRegister<CpuRegister>());
      } else {
        DCHECK(value.IsConstant()) << value;
        int32_t v = CodeGenerator::GetInt32ValueOf(value.GetConstant());
        __ movl(address, Immediate(v));
      }
      codegen_->MaybeRecordImplicitNullCheck(instruction);
      break;
    }

    case DataType::Type::kInt64: {
      uint32_t offset = mirror::Array::DataOffset(sizeof(int64_t)).Uint32Value();
      Address address = CodeGeneratorX86_64::ArrayAddress(array, index, TIMES_8, offset);
      if (value.IsRegister()) {
        __ movq(address, value.AsRegister<CpuRegister>());
        codegen_->MaybeRecordImplicitNullCheck(instruction);
      } else {
        int64_t v = value.GetConstant()->AsLongConstant()->GetValue();
        Address address_high =
            CodeGeneratorX86_64::ArrayAddress(array, index, TIMES_8, offset + sizeof(int32_t));
        codegen_->MoveInt64ToAddress(address, address_high, v, instruction);
      }
      break;
    }

    case DataType::Type::kFloat32: {
      uint32_t offset = mirror::Array::DataOffset(sizeof(float)).Uint32Value();
      Address address = CodeGeneratorX86_64::ArrayAddress(array, index, TIMES_4, offset);
      if (value.IsFpuRegister()) {
        __ movss(address, value.AsFpuRegister<XmmRegister>());
      } else {
        DCHECK(value.IsConstant());
        int32_t v = bit_cast<int32_t, float>(value.GetConstant()->AsFloatConstant()->GetValue());
        __ movl(address, Immediate(v));
      }
      codegen_->MaybeRecordImplicitNullCheck(instruction);
      break;
    }

    case DataType::Type::kFloat64: {
      uint32_t offset = mirror::Array::DataOffset(sizeof(double)).Uint32Value();
      Address address = CodeGeneratorX86_64::ArrayAddress(array, index, TIMES_8, offset);
      if (value.IsFpuRegister()) {
        __ movsd(address, value.AsFpuRegister<XmmRegister>());
        codegen_->MaybeRecordImplicitNullCheck(instruction);
      } else {
        int64_t v = bit_cast<int64_t, double>(value.GetConstant()->AsDoubleConstant()->GetValue());
        Address address_high =
            CodeGeneratorX86_64::ArrayAddress(array, index, TIMES_8, offset + sizeof(int32_t));
        codegen_->MoveInt64ToAddress(address, address_high, v, instruction);
      }
      break;
    }

    case DataType::Type::kUint32:
    case DataType::Type::kUint64:
    case DataType::Type::kVoid:
      LOG(FATAL) << "Unreachable type " << instruction->GetType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitArrayLength(HArrayLength* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  if (!instruction->IsEmittedAtUseSite()) {
    locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
  }
}

void InstructionCodeGeneratorX86_64::VisitArrayLength(HArrayLength* instruction) {
  if (instruction->IsEmittedAtUseSite()) {
    return;
  }

  LocationSummary* locations = instruction->GetLocations();
  uint32_t offset = CodeGenerator::GetArrayLengthOffset(instruction);
  CpuRegister obj = locations->InAt(0).AsRegister<CpuRegister>();
  CpuRegister out = locations->Out().AsRegister<CpuRegister>();
  __ movl(out, Address(obj, offset));
  codegen_->MaybeRecordImplicitNullCheck(instruction);
  // Mask out most significant bit in case the array is String's array of char.
  if (mirror::kUseStringCompression && instruction->IsStringLength()) {
    __ shrl(out, Immediate(1));
  }
}

void LocationsBuilderX86_64::VisitBoundsCheck(HBoundsCheck* instruction) {
  RegisterSet caller_saves = RegisterSet::Empty();
  InvokeRuntimeCallingConvention calling_convention;
  caller_saves.Add(Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  caller_saves.Add(Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  LocationSummary* locations = codegen_->CreateThrowingSlowPathLocations(instruction, caller_saves);
  locations->SetInAt(0, Location::RegisterOrConstant(instruction->InputAt(0)));
  HInstruction* length = instruction->InputAt(1);
  if (!length->IsEmittedAtUseSite()) {
    locations->SetInAt(1, Location::RegisterOrConstant(length));
  }
}

void InstructionCodeGeneratorX86_64::VisitBoundsCheck(HBoundsCheck* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Location index_loc = locations->InAt(0);
  Location length_loc = locations->InAt(1);
  SlowPathCode* slow_path =
      new (codegen_->GetScopedAllocator()) BoundsCheckSlowPathX86_64(instruction);

  if (length_loc.IsConstant()) {
    int32_t length = CodeGenerator::GetInt32ValueOf(length_loc.GetConstant());
    if (index_loc.IsConstant()) {
      // BCE will remove the bounds check if we are guarenteed to pass.
      int32_t index = CodeGenerator::GetInt32ValueOf(index_loc.GetConstant());
      if (index < 0 || index >= length) {
        codegen_->AddSlowPath(slow_path);
        __ jmp(slow_path->GetEntryLabel());
      } else {
        // Some optimization after BCE may have generated this, and we should not
        // generate a bounds check if it is a valid range.
      }
      return;
    }

    // We have to reverse the jump condition because the length is the constant.
    CpuRegister index_reg = index_loc.AsRegister<CpuRegister>();
    __ cmpl(index_reg, Immediate(length));
    codegen_->AddSlowPath(slow_path);
    __ j(kAboveEqual, slow_path->GetEntryLabel());
  } else {
    HInstruction* array_length = instruction->InputAt(1);
    if (array_length->IsEmittedAtUseSite()) {
      // Address the length field in the array.
      DCHECK(array_length->IsArrayLength());
      uint32_t len_offset = CodeGenerator::GetArrayLengthOffset(array_length->AsArrayLength());
      Location array_loc = array_length->GetLocations()->InAt(0);
      Address array_len(array_loc.AsRegister<CpuRegister>(), len_offset);
      if (mirror::kUseStringCompression && instruction->IsStringCharAt()) {
        // TODO: if index_loc.IsConstant(), compare twice the index (to compensate for
        // the string compression flag) with the in-memory length and avoid the temporary.
        CpuRegister length_reg = CpuRegister(TMP);
        __ movl(length_reg, array_len);
        codegen_->MaybeRecordImplicitNullCheck(array_length);
        __ shrl(length_reg, Immediate(1));
        codegen_->GenerateIntCompare(length_reg, index_loc);
      } else {
        // Checking the bound for general case:
        // Array of char or String's array when the compression feature off.
        if (index_loc.IsConstant()) {
          int32_t value = CodeGenerator::GetInt32ValueOf(index_loc.GetConstant());
          __ cmpl(array_len, Immediate(value));
        } else {
          __ cmpl(array_len, index_loc.AsRegister<CpuRegister>());
        }
        codegen_->MaybeRecordImplicitNullCheck(array_length);
      }
    } else {
      codegen_->GenerateIntCompare(length_loc, index_loc);
    }
    codegen_->AddSlowPath(slow_path);
    __ j(kBelowEqual, slow_path->GetEntryLabel());
  }
}

void CodeGeneratorX86_64::MaybeMarkGCCard(CpuRegister temp,
                                          CpuRegister card,
                                          CpuRegister object,
                                          CpuRegister value,
                                          bool emit_null_check) {
  NearLabel is_null;
  if (emit_null_check) {
    __ testl(value, value);
    __ j(kEqual, &is_null);
  }
  MarkGCCard(temp, card, object);
  if (emit_null_check) {
    __ Bind(&is_null);
  }
}

void CodeGeneratorX86_64::MarkGCCard(CpuRegister temp, CpuRegister card, CpuRegister object) {
  // Load the address of the card table into `card`.
  __ gs()->movq(card,
                Address::Absolute(Thread::CardTableOffset<kX86_64PointerSize>().Int32Value(),
                                  /* no_rip= */ true));
  // Calculate the offset (in the card table) of the card corresponding to `object`.
  __ movq(temp, object);
  __ shrq(temp, Immediate(gc::accounting::CardTable::kCardShift));
  // Write the `art::gc::accounting::CardTable::kCardDirty` value into the
  // `object`'s card.
  //
  // Register `card` contains the address of the card table. Note that the card
  // table's base is biased during its creation so that it always starts at an
  // address whose least-significant byte is equal to `kCardDirty` (see
  // art::gc::accounting::CardTable::Create). Therefore the MOVB instruction
  // below writes the `kCardDirty` (byte) value into the `object`'s card
  // (located at `card + object >> kCardShift`).
  //
  // This dual use of the value in register `card` (1. to calculate the location
  // of the card to mark; and 2. to load the `kCardDirty` value) saves a load
  // (no need to explicitly load `kCardDirty` as an immediate value).
  __ movb(Address(temp, card, TIMES_1, 0), card);
}

void CodeGeneratorX86_64::CheckGCCardIsValid(CpuRegister temp,
                                             CpuRegister card,
                                             CpuRegister object) {
  NearLabel done;
  // Load the address of the card table into `card`.
  __ gs()->movq(card,
                Address::Absolute(Thread::CardTableOffset<kX86_64PointerSize>().Int32Value(),
                                  /* no_rip= */ true));
  // Calculate the offset (in the card table) of the card corresponding to `object`.
  __ movq(temp, object);
  __ shrq(temp, Immediate(gc::accounting::CardTable::kCardShift));
  // assert (!clean || !self->is_gc_marking)
  __ cmpb(Address(temp, card, TIMES_1, 0), Immediate(gc::accounting::CardTable::kCardClean));
  __ j(kNotEqual, &done);
  __ gs()->cmpl(
      Address::Absolute(Thread::IsGcMarkingOffset<kX86_64PointerSize>(), /* no_rip= */ true),
      Immediate(0));
  __ j(kEqual, &done);
  __ int3();
  __ Bind(&done);
}

void LocationsBuilderX86_64::VisitParallelMove([[maybe_unused]] HParallelMove* instruction) {
  LOG(FATAL) << "Unimplemented";
}

void InstructionCodeGeneratorX86_64::VisitParallelMove(HParallelMove* instruction) {
  if (instruction->GetNext()->IsSuspendCheck() &&
      instruction->GetBlock()->GetLoopInformation() != nullptr) {
    HSuspendCheck* suspend_check = instruction->GetNext()->AsSuspendCheck();
    // The back edge will generate the suspend check.
    codegen_->ClearSpillSlotsFromLoopPhisInStackMap(suspend_check, instruction);
  }

  codegen_->GetMoveResolver()->EmitNativeCode(instruction);
}

void LocationsBuilderX86_64::VisitSuspendCheck(HSuspendCheck* instruction) {
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(
      instruction, LocationSummary::kCallOnSlowPath);
  // In suspend check slow path, usually there are no caller-save registers at all.
  // If SIMD instructions are present, however, we force spilling all live SIMD
  // registers in full width (since the runtime only saves/restores lower part).
  locations->SetCustomSlowPathCallerSaves(
      GetGraph()->HasSIMD() ? RegisterSet::AllFpu() : RegisterSet::Empty());
}

void InstructionCodeGeneratorX86_64::VisitSuspendCheck(HSuspendCheck* instruction) {
  HBasicBlock* block = instruction->GetBlock();
  if (block->GetLoopInformation() != nullptr) {
    DCHECK(block->GetLoopInformation()->GetSuspendCheck() == instruction);
    // The back edge will generate the suspend check.
    return;
  }
  if (block->IsEntryBlock() && instruction->GetNext()->IsGoto()) {
    // The goto will generate the suspend check.
    return;
  }
  GenerateSuspendCheck(instruction, nullptr);
}

void InstructionCodeGeneratorX86_64::GenerateSuspendCheck(HSuspendCheck* instruction,
                                                          HBasicBlock* successor) {
  SuspendCheckSlowPathX86_64* slow_path =
      down_cast<SuspendCheckSlowPathX86_64*>(instruction->GetSlowPath());
  if (slow_path == nullptr) {
    slow_path =
        new (codegen_->GetScopedAllocator()) SuspendCheckSlowPathX86_64(instruction, successor);
    instruction->SetSlowPath(slow_path);
    codegen_->AddSlowPath(slow_path);
    if (successor != nullptr) {
      DCHECK(successor->IsLoopHeader());
    }
  } else {
    DCHECK_EQ(slow_path->GetSuccessor(), successor);
  }

  __ gs()->testl(Address::Absolute(Thread::ThreadFlagsOffset<kX86_64PointerSize>().Int32Value(),
                                   /* no_rip= */ true),
                 Immediate(Thread::SuspendOrCheckpointRequestFlags()));
  if (successor == nullptr) {
    __ j(kNotZero, slow_path->GetEntryLabel());
    __ Bind(slow_path->GetReturnLabel());
  } else {
    __ j(kZero, codegen_->GetLabelOf(successor));
    __ jmp(slow_path->GetEntryLabel());
  }
}

X86_64Assembler* ParallelMoveResolverX86_64::GetAssembler() const {
  return codegen_->GetAssembler();
}

void ParallelMoveResolverX86_64::EmitMove(size_t index) {
  MoveOperands* move = moves_[index];
  Location source = move->GetSource();
  Location destination = move->GetDestination();

  if (source.IsRegister()) {
    if (destination.IsRegister()) {
      __ movq(destination.AsRegister<CpuRegister>(), source.AsRegister<CpuRegister>());
    } else if (destination.IsStackSlot()) {
      __ movl(Address(CpuRegister(RSP), destination.GetStackIndex()),
              source.AsRegister<CpuRegister>());
    } else {
      DCHECK(destination.IsDoubleStackSlot());
      __ movq(Address(CpuRegister(RSP), destination.GetStackIndex()),
              source.AsRegister<CpuRegister>());
    }
  } else if (source.IsStackSlot()) {
    if (destination.IsRegister()) {
      __ movl(destination.AsRegister<CpuRegister>(),
              Address(CpuRegister(RSP), source.GetStackIndex()));
    } else if (destination.IsFpuRegister()) {
      __ movss(destination.AsFpuRegister<XmmRegister>(),
              Address(CpuRegister(RSP), source.GetStackIndex()));
    } else {
      DCHECK(destination.IsStackSlot());
      __ movl(CpuRegister(TMP), Address(CpuRegister(RSP), source.GetStackIndex()));
      __ movl(Address(CpuRegister(RSP), destination.GetStackIndex()), CpuRegister(TMP));
    }
  } else if (source.IsDoubleStackSlot()) {
    if (destination.IsRegister()) {
      __ movq(destination.AsRegister<CpuRegister>(),
              Address(CpuRegister(RSP), source.GetStackIndex()));
    } else if (destination.IsFpuRegister()) {
      __ movsd(destination.AsFpuRegister<XmmRegister>(),
               Address(CpuRegister(RSP), source.GetStackIndex()));
    } else {
      DCHECK(destination.IsDoubleStackSlot()) << destination;
      __ movq(CpuRegister(TMP), Address(CpuRegister(RSP), source.GetStackIndex()));
      __ movq(Address(CpuRegister(RSP), destination.GetStackIndex()), CpuRegister(TMP));
    }
  } else if (source.IsSIMDStackSlot()) {
    if (destination.IsFpuRegister()) {
      __ movups(destination.AsFpuRegister<XmmRegister>(),
                Address(CpuRegister(RSP), source.GetStackIndex()));
    } else {
      DCHECK(destination.IsSIMDStackSlot());
      size_t high = kX86_64WordSize;
      __ movq(CpuRegister(TMP), Address(CpuRegister(RSP), source.GetStackIndex()));
      __ movq(Address(CpuRegister(RSP), destination.GetStackIndex()), CpuRegister(TMP));
      __ movq(CpuRegister(TMP), Address(CpuRegister(RSP), source.GetStackIndex() + high));
      __ movq(Address(CpuRegister(RSP), destination.GetStackIndex() + high), CpuRegister(TMP));
    }
  } else if (source.IsConstant()) {
    HConstant* constant = source.GetConstant();
    if (constant->IsIntConstant() || constant->IsNullConstant()) {
      int32_t value = CodeGenerator::GetInt32ValueOf(constant);
      if (destination.IsRegister()) {
        if (value == 0) {
          __ xorl(destination.AsRegister<CpuRegister>(), destination.AsRegister<CpuRegister>());
        } else {
          __ movl(destination.AsRegister<CpuRegister>(), Immediate(value));
        }
      } else {
        DCHECK(destination.IsStackSlot()) << destination;
        __ movl(Address(CpuRegister(RSP), destination.GetStackIndex()), Immediate(value));
      }
    } else if (constant->IsLongConstant()) {
      int64_t value = constant->AsLongConstant()->GetValue();
      if (destination.IsRegister()) {
        codegen_->Load64BitValue(destination.AsRegister<CpuRegister>(), value);
      } else {
        DCHECK(destination.IsDoubleStackSlot()) << destination;
        codegen_->Store64BitValueToStack(destination, value);
      }
    } else if (constant->IsFloatConstant()) {
      float fp_value = constant->AsFloatConstant()->GetValue();
      if (destination.IsFpuRegister()) {
        XmmRegister dest = destination.AsFpuRegister<XmmRegister>();
        codegen_->Load32BitValue(dest, fp_value);
      } else {
        DCHECK(destination.IsStackSlot()) << destination;
        Immediate imm(bit_cast<int32_t, float>(fp_value));
        __ movl(Address(CpuRegister(RSP), destination.GetStackIndex()), imm);
      }
    } else {
      DCHECK(constant->IsDoubleConstant()) << constant->DebugName();
      double fp_value =  constant->AsDoubleConstant()->GetValue();
      int64_t value = bit_cast<int64_t, double>(fp_value);
      if (destination.IsFpuRegister()) {
        XmmRegister dest = destination.AsFpuRegister<XmmRegister>();
        codegen_->Load64BitValue(dest, fp_value);
      } else {
        DCHECK(destination.IsDoubleStackSlot()) << destination;
        codegen_->Store64BitValueToStack(destination, value);
      }
    }
  } else if (source.IsFpuRegister()) {
    if (destination.IsFpuRegister()) {
      __ movaps(destination.AsFpuRegister<XmmRegister>(), source.AsFpuRegister<XmmRegister>());
    } else if (destination.IsStackSlot()) {
      __ movss(Address(CpuRegister(RSP), destination.GetStackIndex()),
               source.AsFpuRegister<XmmRegister>());
    } else if (destination.IsDoubleStackSlot()) {
      __ movsd(Address(CpuRegister(RSP), destination.GetStackIndex()),
               source.AsFpuRegister<XmmRegister>());
    } else {
       DCHECK(destination.IsSIMDStackSlot());
      __ movups(Address(CpuRegister(RSP), destination.GetStackIndex()),
                source.AsFpuRegister<XmmRegister>());
    }
  }
}

void ParallelMoveResolverX86_64::Exchange32(CpuRegister reg, int mem) {
  __ movl(CpuRegister(TMP), Address(CpuRegister(RSP), mem));
  __ movl(Address(CpuRegister(RSP), mem), reg);
  __ movl(reg, CpuRegister(TMP));
}

void ParallelMoveResolverX86_64::Exchange64(CpuRegister reg1, CpuRegister reg2) {
  __ movq(CpuRegister(TMP), reg1);
  __ movq(reg1, reg2);
  __ movq(reg2, CpuRegister(TMP));
}

void ParallelMoveResolverX86_64::Exchange64(CpuRegister reg, int mem) {
  __ movq(CpuRegister(TMP), Address(CpuRegister(RSP), mem));
  __ movq(Address(CpuRegister(RSP), mem), reg);
  __ movq(reg, CpuRegister(TMP));
}

void ParallelMoveResolverX86_64::Exchange32(XmmRegister reg, int mem) {
  __ movl(CpuRegister(TMP), Address(CpuRegister(RSP), mem));
  __ movss(Address(CpuRegister(RSP), mem), reg);
  __ movd(reg, CpuRegister(TMP));
}

void ParallelMoveResolverX86_64::Exchange64(XmmRegister reg, int mem) {
  __ movq(CpuRegister(TMP), Address(CpuRegister(RSP), mem));
  __ movsd(Address(CpuRegister(RSP), mem), reg);
  __ movq(reg, CpuRegister(TMP));
}

void ParallelMoveResolverX86_64::Exchange128(XmmRegister reg, int mem) {
  size_t extra_slot = 2 * kX86_64WordSize;
  __ subq(CpuRegister(RSP), Immediate(extra_slot));
  __ movups(Address(CpuRegister(RSP), 0), XmmRegister(reg));
  ExchangeMemory64(0, mem + extra_slot, 2);
  __ movups(XmmRegister(reg), Address(CpuRegister(RSP), 0));
  __ addq(CpuRegister(RSP), Immediate(extra_slot));
}

void ParallelMoveResolverX86_64::ExchangeMemory32(int mem1, int mem2) {
  ScratchRegisterScope ensure_scratch(
      this, TMP, RAX, codegen_->GetNumberOfCoreRegisters());

  int stack_offset = ensure_scratch.IsSpilled() ? kX86_64WordSize : 0;
  __ movl(CpuRegister(TMP), Address(CpuRegister(RSP), mem1 + stack_offset));
  __ movl(CpuRegister(ensure_scratch.GetRegister()),
          Address(CpuRegister(RSP), mem2 + stack_offset));
  __ movl(Address(CpuRegister(RSP), mem2 + stack_offset), CpuRegister(TMP));
  __ movl(Address(CpuRegister(RSP), mem1 + stack_offset),
          CpuRegister(ensure_scratch.GetRegister()));
}

void ParallelMoveResolverX86_64::ExchangeMemory64(int mem1, int mem2, int num_of_qwords) {
  ScratchRegisterScope ensure_scratch(
      this, TMP, RAX, codegen_->GetNumberOfCoreRegisters());

  int stack_offset = ensure_scratch.IsSpilled() ? kX86_64WordSize : 0;

  // Now that temp registers are available (possibly spilled), exchange blocks of memory.
  for (int i = 0; i < num_of_qwords; i++) {
    __ movq(CpuRegister(TMP),
            Address(CpuRegister(RSP), mem1 + stack_offset));
    __ movq(CpuRegister(ensure_scratch.GetRegister()),
            Address(CpuRegister(RSP), mem2 + stack_offset));
    __ movq(Address(CpuRegister(RSP), mem2 + stack_offset),
            CpuRegister(TMP));
    __ movq(Address(CpuRegister(RSP), mem1 + stack_offset),
            CpuRegister(ensure_scratch.GetRegister()));
    stack_offset += kX86_64WordSize;
  }
}

void ParallelMoveResolverX86_64::EmitSwap(size_t index) {
  MoveOperands* move = moves_[index];
  Location source = move->GetSource();
  Location destination = move->GetDestination();

  if (source.IsRegister() && destination.IsRegister()) {
    Exchange64(source.AsRegister<CpuRegister>(), destination.AsRegister<CpuRegister>());
  } else if (source.IsRegister() && destination.IsStackSlot()) {
    Exchange32(source.AsRegister<CpuRegister>(), destination.GetStackIndex());
  } else if (source.IsStackSlot() && destination.IsRegister()) {
    Exchange32(destination.AsRegister<CpuRegister>(), source.GetStackIndex());
  } else if (source.IsStackSlot() && destination.IsStackSlot()) {
    ExchangeMemory32(destination.GetStackIndex(), source.GetStackIndex());
  } else if (source.IsRegister() && destination.IsDoubleStackSlot()) {
    Exchange64(source.AsRegister<CpuRegister>(), destination.GetStackIndex());
  } else if (source.IsDoubleStackSlot() && destination.IsRegister()) {
    Exchange64(destination.AsRegister<CpuRegister>(), source.GetStackIndex());
  } else if (source.IsDoubleStackSlot() && destination.IsDoubleStackSlot()) {
    ExchangeMemory64(destination.GetStackIndex(), source.GetStackIndex(), 1);
  } else if (source.IsFpuRegister() && destination.IsFpuRegister()) {
    __ movq(CpuRegister(TMP), source.AsFpuRegister<XmmRegister>());
    __ movaps(source.AsFpuRegister<XmmRegister>(), destination.AsFpuRegister<XmmRegister>());
    __ movq(destination.AsFpuRegister<XmmRegister>(), CpuRegister(TMP));
  } else if (source.IsFpuRegister() && destination.IsStackSlot()) {
    Exchange32(source.AsFpuRegister<XmmRegister>(), destination.GetStackIndex());
  } else if (source.IsStackSlot() && destination.IsFpuRegister()) {
    Exchange32(destination.AsFpuRegister<XmmRegister>(), source.GetStackIndex());
  } else if (source.IsFpuRegister() && destination.IsDoubleStackSlot()) {
    Exchange64(source.AsFpuRegister<XmmRegister>(), destination.GetStackIndex());
  } else if (source.IsDoubleStackSlot() && destination.IsFpuRegister()) {
    Exchange64(destination.AsFpuRegister<XmmRegister>(), source.GetStackIndex());
  } else if (source.IsSIMDStackSlot() && destination.IsSIMDStackSlot()) {
    ExchangeMemory64(destination.GetStackIndex(), source.GetStackIndex(), 2);
  } else if (source.IsFpuRegister() && destination.IsSIMDStackSlot()) {
    Exchange128(source.AsFpuRegister<XmmRegister>(), destination.GetStackIndex());
  } else if (destination.IsFpuRegister() && source.IsSIMDStackSlot()) {
    Exchange128(destination.AsFpuRegister<XmmRegister>(), source.GetStackIndex());
  } else {
    LOG(FATAL) << "Unimplemented swap between " << source << " and " << destination;
  }
}


void ParallelMoveResolverX86_64::SpillScratch(int reg) {
  __ pushq(CpuRegister(reg));
}


void ParallelMoveResolverX86_64::RestoreScratch(int reg) {
  __ popq(CpuRegister(reg));
}

void InstructionCodeGeneratorX86_64::GenerateClassInitializationCheck(
    SlowPathCode* slow_path, CpuRegister class_reg) {
  __ cmpb(Address(class_reg, kClassStatusByteOffset), Immediate(kShiftedVisiblyInitializedValue));
  __ j(kBelow, slow_path->GetEntryLabel());
  __ Bind(slow_path->GetExitLabel());
}

void InstructionCodeGeneratorX86_64::GenerateBitstringTypeCheckCompare(HTypeCheckInstruction* check,
                                                                       CpuRegister temp) {
  uint32_t path_to_root = check->GetBitstringPathToRoot();
  uint32_t mask = check->GetBitstringMask();
  DCHECK(IsPowerOfTwo(mask + 1));
  size_t mask_bits = WhichPowerOf2(mask + 1);

  if (mask_bits == 16u) {
    // Compare the bitstring in memory.
    __ cmpw(Address(temp, mirror::Class::StatusOffset()), Immediate(path_to_root));
  } else {
    // /* uint32_t */ temp = temp->status_
    __ movl(temp, Address(temp, mirror::Class::StatusOffset()));
    // Compare the bitstring bits using SUB.
    __ subl(temp, Immediate(path_to_root));
    // Shift out bits that do not contribute to the comparison.
    __ shll(temp, Immediate(32u - mask_bits));
  }
}

HLoadClass::LoadKind CodeGeneratorX86_64::GetSupportedLoadClassKind(
    HLoadClass::LoadKind desired_class_load_kind) {
  switch (desired_class_load_kind) {
    case HLoadClass::LoadKind::kInvalid:
      LOG(FATAL) << "UNREACHABLE";
      UNREACHABLE();
    case HLoadClass::LoadKind::kReferrersClass:
      break;
    case HLoadClass::LoadKind::kBootImageLinkTimePcRelative:
    case HLoadClass::LoadKind::kBootImageRelRo:
    case HLoadClass::LoadKind::kAppImageRelRo:
    case HLoadClass::LoadKind::kBssEntry:
    case HLoadClass::LoadKind::kBssEntryPublic:
    case HLoadClass::LoadKind::kBssEntryPackage:
      DCHECK(!GetCompilerOptions().IsJitCompiler());
      break;
    case HLoadClass::LoadKind::kJitBootImageAddress:
    case HLoadClass::LoadKind::kJitTableAddress:
      DCHECK(GetCompilerOptions().IsJitCompiler());
      break;
    case HLoadClass::LoadKind::kRuntimeCall:
      break;
  }
  return desired_class_load_kind;
}

void LocationsBuilderX86_64::VisitLoadClass(HLoadClass* cls) {
  HLoadClass::LoadKind load_kind = cls->GetLoadKind();
  if (load_kind == HLoadClass::LoadKind::kRuntimeCall) {
    // Custom calling convention: RAX serves as both input and output.
    CodeGenerator::CreateLoadClassRuntimeCallLocationSummary(
        cls,
        Location::RegisterLocation(RAX),
        Location::RegisterLocation(RAX));
    return;
  }
  DCHECK_EQ(cls->NeedsAccessCheck(),
            load_kind == HLoadClass::LoadKind::kBssEntryPublic ||
                load_kind == HLoadClass::LoadKind::kBssEntryPackage);

  const bool requires_read_barrier = !cls->IsInImage() && codegen_->EmitReadBarrier();
  LocationSummary::CallKind call_kind = (cls->NeedsEnvironment() || requires_read_barrier)
      ? LocationSummary::kCallOnSlowPath
      : LocationSummary::kNoCall;
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(cls, call_kind);
  if (kUseBakerReadBarrier && requires_read_barrier && !cls->NeedsEnvironment()) {
    locations->SetCustomSlowPathCallerSaves(RegisterSet::Empty());  // No caller-save registers.
  }

  if (load_kind == HLoadClass::LoadKind::kReferrersClass) {
    locations->SetInAt(0, Location::RequiresRegister());
  }
  locations->SetOut(Location::RequiresRegister());
  if (load_kind == HLoadClass::LoadKind::kBssEntry ||
      load_kind == HLoadClass::LoadKind::kBssEntryPublic ||
      load_kind == HLoadClass::LoadKind::kBssEntryPackage) {
    if (codegen_->EmitNonBakerReadBarrier()) {
      // For non-Baker read barrier we have a temp-clobbering call.
    } else {
      // Rely on the type resolution and/or initialization to save everything.
      locations->SetCustomSlowPathCallerSaves(OneRegInReferenceOutSaveEverythingCallerSaves());
    }
  }
}

Label* CodeGeneratorX86_64::NewJitRootClassPatch(const DexFile& dex_file,
                                                 dex::TypeIndex type_index,
                                                 Handle<mirror::Class> handle) {
  ReserveJitClassRoot(TypeReference(&dex_file, type_index), handle);
  // Add a patch entry and return the label.
  jit_class_patches_.emplace_back(&dex_file, type_index.index_);
  PatchInfo<Label>* info = &jit_class_patches_.back();
  return &info->label;
}

// NO_THREAD_SAFETY_ANALYSIS as we manipulate handles whose internal object we know does not
// move.
void InstructionCodeGeneratorX86_64::VisitLoadClass(HLoadClass* cls) NO_THREAD_SAFETY_ANALYSIS {
  HLoadClass::LoadKind load_kind = cls->GetLoadKind();
  if (load_kind == HLoadClass::LoadKind::kRuntimeCall) {
    codegen_->GenerateLoadClassRuntimeCall(cls);
    return;
  }
  DCHECK_EQ(cls->NeedsAccessCheck(),
            load_kind == HLoadClass::LoadKind::kBssEntryPublic ||
                load_kind == HLoadClass::LoadKind::kBssEntryPackage);

  LocationSummary* locations = cls->GetLocations();
  Location out_loc = locations->Out();
  CpuRegister out = out_loc.AsRegister<CpuRegister>();

  const ReadBarrierOption read_barrier_option =
      cls->IsInImage() ? kWithoutReadBarrier : codegen_->GetCompilerReadBarrierOption();
  bool generate_null_check = false;
  switch (load_kind) {
    case HLoadClass::LoadKind::kReferrersClass: {
      DCHECK(!cls->CanCallRuntime());
      DCHECK(!cls->MustGenerateClinitCheck());
      // /* GcRoot<mirror::Class> */ out = current_method->declaring_class_
      CpuRegister current_method = locations->InAt(0).AsRegister<CpuRegister>();
      GenerateGcRootFieldLoad(
          cls,
          out_loc,
          Address(current_method, ArtMethod::DeclaringClassOffset().Int32Value()),
          /* fixup_label= */ nullptr,
          read_barrier_option);
      break;
    }
    case HLoadClass::LoadKind::kBootImageLinkTimePcRelative:
      DCHECK(codegen_->GetCompilerOptions().IsBootImage() ||
             codegen_->GetCompilerOptions().IsBootImageExtension());
      DCHECK_EQ(read_barrier_option, kWithoutReadBarrier);
      __ leal(out,
              Address::Absolute(CodeGeneratorX86_64::kPlaceholder32BitOffset, /* no_rip= */ false));
      codegen_->RecordBootImageTypePatch(cls->GetDexFile(), cls->GetTypeIndex());
      break;
    case HLoadClass::LoadKind::kBootImageRelRo: {
      DCHECK(!codegen_->GetCompilerOptions().IsBootImage());
      __ movl(out,
              Address::Absolute(CodeGeneratorX86_64::kPlaceholder32BitOffset, /* no_rip= */ false));
      codegen_->RecordBootImageRelRoPatch(CodeGenerator::GetBootImageOffset(cls));
      break;
    }
    case HLoadClass::LoadKind::kAppImageRelRo: {
      DCHECK(codegen_->GetCompilerOptions().IsAppImage());
      DCHECK_EQ(read_barrier_option, kWithoutReadBarrier);
      __ movl(out,
              Address::Absolute(CodeGeneratorX86_64::kPlaceholder32BitOffset, /* no_rip= */ false));
      codegen_->RecordAppImageTypePatch(cls->GetDexFile(), cls->GetTypeIndex());
      break;
    }
    case HLoadClass::LoadKind::kBssEntry:
    case HLoadClass::LoadKind::kBssEntryPublic:
    case HLoadClass::LoadKind::kBssEntryPackage: {
      Address address = Address::Absolute(CodeGeneratorX86_64::kPlaceholder32BitOffset,
                                          /* no_rip= */ false);
      Label* fixup_label = codegen_->NewTypeBssEntryPatch(cls);
      // /* GcRoot<mirror::Class> */ out = *address  /* PC-relative */
      GenerateGcRootFieldLoad(cls, out_loc, address, fixup_label, read_barrier_option);
      // No need for memory fence, thanks to the x86-64 memory model.
      generate_null_check = true;
      break;
    }
    case HLoadClass::LoadKind::kJitBootImageAddress: {
      DCHECK_EQ(read_barrier_option, kWithoutReadBarrier);
      uint32_t address = reinterpret_cast32<uint32_t>(cls->GetClass().Get());
      DCHECK_NE(address, 0u);
      __ movl(out, Immediate(static_cast<int32_t>(address)));  // Zero-extended.
      break;
    }
    case HLoadClass::LoadKind::kJitTableAddress: {
      Address address = Address::Absolute(CodeGeneratorX86_64::kPlaceholder32BitOffset,
                                          /* no_rip= */ true);
      Label* fixup_label =
          codegen_->NewJitRootClassPatch(cls->GetDexFile(), cls->GetTypeIndex(), cls->GetClass());
      // /* GcRoot<mirror::Class> */ out = *address
      GenerateGcRootFieldLoad(cls, out_loc, address, fixup_label, read_barrier_option);
      break;
    }
    default:
      LOG(FATAL) << "Unexpected load kind: " << cls->GetLoadKind();
      UNREACHABLE();
  }

  if (generate_null_check || cls->MustGenerateClinitCheck()) {
    DCHECK(cls->CanCallRuntime());
    SlowPathCode* slow_path =
        new (codegen_->GetScopedAllocator()) LoadClassSlowPathX86_64(cls, cls);
    codegen_->AddSlowPath(slow_path);
    if (generate_null_check) {
      __ testl(out, out);
      __ j(kEqual, slow_path->GetEntryLabel());
    }
    if (cls->MustGenerateClinitCheck()) {
      GenerateClassInitializationCheck(slow_path, out);
    } else {
      __ Bind(slow_path->GetExitLabel());
    }
  }
}

void LocationsBuilderX86_64::VisitClinitCheck(HClinitCheck* check) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(check, LocationSummary::kCallOnSlowPath);
  locations->SetInAt(0, Location::RequiresRegister());
  if (check->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
  // Rely on the type initialization to save everything we need.
  locations->SetCustomSlowPathCallerSaves(OneRegInReferenceOutSaveEverythingCallerSaves());
}

void LocationsBuilderX86_64::VisitLoadMethodHandle(HLoadMethodHandle* load) {
  // Custom calling convention: RAX serves as both input and output.
  Location location = Location::RegisterLocation(RAX);
  CodeGenerator::CreateLoadMethodHandleRuntimeCallLocationSummary(load, location, location);
}

void InstructionCodeGeneratorX86_64::VisitLoadMethodHandle(HLoadMethodHandle* load) {
  codegen_->GenerateLoadMethodHandleRuntimeCall(load);
}

Label* CodeGeneratorX86_64::NewJitRootMethodTypePatch(const DexFile& dex_file,
                                                      dex::ProtoIndex proto_index,
                                                      Handle<mirror::MethodType> handle) {
  ReserveJitMethodTypeRoot(ProtoReference(&dex_file, proto_index), handle);
  // Add a patch entry and return the label.
  jit_method_type_patches_.emplace_back(&dex_file, proto_index.index_);
  PatchInfo<Label>* info = &jit_method_type_patches_.back();
  return &info->label;
}

void LocationsBuilderX86_64::VisitLoadMethodType(HLoadMethodType* load) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(load, LocationSummary::kCallOnSlowPath);
  if (load->GetLoadKind() == HLoadMethodType::LoadKind::kRuntimeCall) {
    Location location = Location::RegisterLocation(RAX);
    CodeGenerator::CreateLoadMethodTypeRuntimeCallLocationSummary(load, location, location);
  } else {
    locations->SetOut(Location::RequiresRegister());
    if (load->GetLoadKind() == HLoadMethodType::LoadKind::kBssEntry) {
      if (codegen_->EmitNonBakerReadBarrier()) {
        // For non-Baker read barrier we have a temp-clobbering call.
      } else {
        // Rely on the pResolveMethodType to save everything.
        locations->SetCustomSlowPathCallerSaves(OneRegInReferenceOutSaveEverythingCallerSaves());
      }
    }
  }
}

void InstructionCodeGeneratorX86_64::VisitLoadMethodType(HLoadMethodType* load) {
  LocationSummary* locations = load->GetLocations();
  Location out_loc = locations->Out();
  CpuRegister out = out_loc.AsRegister<CpuRegister>();

  switch (load->GetLoadKind()) {
    case HLoadMethodType::LoadKind::kBssEntry: {
      Address address = Address::Absolute(CodeGeneratorX86_64::kPlaceholder32BitOffset,
                                          /* no_rip= */ false);
      Label* fixup_label = codegen_->NewMethodTypeBssEntryPatch(load);
      // /* GcRoot<mirror::MethodType> */ out = *address  /* PC-relative */
      GenerateGcRootFieldLoad(
          load, out_loc, address, fixup_label, codegen_->GetCompilerReadBarrierOption());
      // No need for memory fence, thanks to the x86-64 memory model.
      SlowPathCode* slow_path =
          new (codegen_->GetScopedAllocator()) LoadMethodTypeSlowPathX86_64(load);
      codegen_->AddSlowPath(slow_path);
      __ testl(out, out);
      __ j(kEqual, slow_path->GetEntryLabel());
      __ Bind(slow_path->GetExitLabel());
      return;
    }
    case HLoadMethodType::LoadKind::kJitTableAddress: {
      Address address = Address::Absolute(CodeGeneratorX86_64::kPlaceholder32BitOffset,
                                          /* no_rip= */ true);
      Handle<mirror::MethodType> method_type = load->GetMethodType();
      DCHECK(method_type != nullptr);
      Label* fixup_label = codegen_->NewJitRootMethodTypePatch(
          load->GetDexFile(), load->GetProtoIndex(), method_type);
      GenerateGcRootFieldLoad(
          load, out_loc, address, fixup_label, codegen_->GetCompilerReadBarrierOption());
      return;
    }
    default:
      DCHECK_EQ(load->GetLoadKind(), HLoadMethodType::LoadKind::kRuntimeCall);
      codegen_->GenerateLoadMethodTypeRuntimeCall(load);
      break;
  }
}

void InstructionCodeGeneratorX86_64::VisitClinitCheck(HClinitCheck* check) {
  // We assume the class to not be null.
  SlowPathCode* slow_path =
      new (codegen_->GetScopedAllocator()) LoadClassSlowPathX86_64(check->GetLoadClass(), check);
  codegen_->AddSlowPath(slow_path);
  GenerateClassInitializationCheck(slow_path,
                                   check->GetLocations()->InAt(0).AsRegister<CpuRegister>());
}

HLoadString::LoadKind CodeGeneratorX86_64::GetSupportedLoadStringKind(
    HLoadString::LoadKind desired_string_load_kind) {
  switch (desired_string_load_kind) {
    case HLoadString::LoadKind::kBootImageLinkTimePcRelative:
    case HLoadString::LoadKind::kBootImageRelRo:
    case HLoadString::LoadKind::kBssEntry:
      DCHECK(!GetCompilerOptions().IsJitCompiler());
      break;
    case HLoadString::LoadKind::kJitBootImageAddress:
    case HLoadString::LoadKind::kJitTableAddress:
      DCHECK(GetCompilerOptions().IsJitCompiler());
      break;
    case HLoadString::LoadKind::kRuntimeCall:
      break;
  }
  return desired_string_load_kind;
}

void LocationsBuilderX86_64::VisitLoadString(HLoadString* load) {
  LocationSummary::CallKind call_kind = codegen_->GetLoadStringCallKind(load);
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(load, call_kind);
  if (load->GetLoadKind() == HLoadString::LoadKind::kRuntimeCall) {
    locations->SetOut(Location::RegisterLocation(RAX));
  } else {
    locations->SetOut(Location::RequiresRegister());
    if (load->GetLoadKind() == HLoadString::LoadKind::kBssEntry) {
      if (codegen_->EmitNonBakerReadBarrier()) {
        // For non-Baker read barrier we have a temp-clobbering call.
      } else {
        // Rely on the pResolveString to save everything.
        locations->SetCustomSlowPathCallerSaves(OneRegInReferenceOutSaveEverythingCallerSaves());
      }
    }
  }
}

Label* CodeGeneratorX86_64::NewJitRootStringPatch(const DexFile& dex_file,
                                                  dex::StringIndex string_index,
                                                  Handle<mirror::String> handle) {
  ReserveJitStringRoot(StringReference(&dex_file, string_index), handle);
  // Add a patch entry and return the label.
  jit_string_patches_.emplace_back(&dex_file, string_index.index_);
  PatchInfo<Label>* info = &jit_string_patches_.back();
  return &info->label;
}

// NO_THREAD_SAFETY_ANALYSIS as we manipulate handles whose internal object we know does not
// move.
void InstructionCodeGeneratorX86_64::VisitLoadString(HLoadString* load) NO_THREAD_SAFETY_ANALYSIS {
  LocationSummary* locations = load->GetLocations();
  Location out_loc = locations->Out();
  CpuRegister out = out_loc.AsRegister<CpuRegister>();

  switch (load->GetLoadKind()) {
    case HLoadString::LoadKind::kBootImageLinkTimePcRelative: {
      DCHECK(codegen_->GetCompilerOptions().IsBootImage() ||
             codegen_->GetCompilerOptions().IsBootImageExtension());
      __ leal(out,
              Address::Absolute(CodeGeneratorX86_64::kPlaceholder32BitOffset, /* no_rip= */ false));
      codegen_->RecordBootImageStringPatch(load);
      return;
    }
    case HLoadString::LoadKind::kBootImageRelRo: {
      DCHECK(!codegen_->GetCompilerOptions().IsBootImage());
      __ movl(out,
              Address::Absolute(CodeGeneratorX86_64::kPlaceholder32BitOffset, /* no_rip= */ false));
      codegen_->RecordBootImageRelRoPatch(CodeGenerator::GetBootImageOffset(load));
      return;
    }
    case HLoadString::LoadKind::kBssEntry: {
      Address address = Address::Absolute(CodeGeneratorX86_64::kPlaceholder32BitOffset,
                                          /* no_rip= */ false);
      Label* fixup_label = codegen_->NewStringBssEntryPatch(load);
      // /* GcRoot<mirror::Class> */ out = *address  /* PC-relative */
      GenerateGcRootFieldLoad(
          load, out_loc, address, fixup_label, codegen_->GetCompilerReadBarrierOption());
      // No need for memory fence, thanks to the x86-64 memory model.
      SlowPathCode* slow_path = new (codegen_->GetScopedAllocator()) LoadStringSlowPathX86_64(load);
      codegen_->AddSlowPath(slow_path);
      __ testl(out, out);
      __ j(kEqual, slow_path->GetEntryLabel());
      __ Bind(slow_path->GetExitLabel());
      return;
    }
    case HLoadString::LoadKind::kJitBootImageAddress: {
      uint32_t address = reinterpret_cast32<uint32_t>(load->GetString().Get());
      DCHECK_NE(address, 0u);
      __ movl(out, Immediate(static_cast<int32_t>(address)));  // Zero-extended.
      return;
    }
    case HLoadString::LoadKind::kJitTableAddress: {
      Address address = Address::Absolute(CodeGeneratorX86_64::kPlaceholder32BitOffset,
                                          /* no_rip= */ true);
      Label* fixup_label = codegen_->NewJitRootStringPatch(
          load->GetDexFile(), load->GetStringIndex(), load->GetString());
      // /* GcRoot<mirror::String> */ out = *address
      GenerateGcRootFieldLoad(
          load, out_loc, address, fixup_label, codegen_->GetCompilerReadBarrierOption());
      return;
    }
    default:
      break;
  }

  // Custom calling convention: RAX serves as both input and output.
  __ movl(CpuRegister(RAX), Immediate(load->GetStringIndex().index_));
  codegen_->InvokeRuntime(kQuickResolveString, load);
  CheckEntrypointTypes<kQuickResolveString, void*, uint32_t>();
}

static Address GetExceptionTlsAddress() {
  return Address::Absolute(Thread::ExceptionOffset<kX86_64PointerSize>().Int32Value(),
                           /* no_rip= */ true);
}

void LocationsBuilderX86_64::VisitLoadException(HLoadException* load) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(load, LocationSummary::kNoCall);
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorX86_64::VisitLoadException(HLoadException* load) {
  __ gs()->movl(load->GetLocations()->Out().AsRegister<CpuRegister>(), GetExceptionTlsAddress());
}

void LocationsBuilderX86_64::VisitClearException(HClearException* clear) {
  new (GetGraph()->GetAllocator()) LocationSummary(clear, LocationSummary::kNoCall);
}

void InstructionCodeGeneratorX86_64::VisitClearException([[maybe_unused]] HClearException* clear) {
  __ gs()->movl(GetExceptionTlsAddress(), Immediate(0));
}

void LocationsBuilderX86_64::VisitThrow(HThrow* instruction) {
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(
      instruction, LocationSummary::kCallOnMainOnly);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
}

void InstructionCodeGeneratorX86_64::VisitThrow(HThrow* instruction) {
  codegen_->InvokeRuntime(kQuickDeliverException, instruction);
  CheckEntrypointTypes<kQuickDeliverException, void, mirror::Object*>();
}

// Temp is used for read barrier.
static size_t NumberOfInstanceOfTemps(bool emit_read_barrier, TypeCheckKind type_check_kind) {
  if (type_check_kind == TypeCheckKind::kInterfaceCheck) {
    return 1;
  }
  if (emit_read_barrier &&
      !kUseBakerReadBarrier &&
      (type_check_kind == TypeCheckKind::kAbstractClassCheck ||
       type_check_kind == TypeCheckKind::kClassHierarchyCheck ||
       type_check_kind == TypeCheckKind::kArrayObjectCheck)) {
    return 1;
  }
  return 0;
}

// Interface case has 2 temps, one for holding the number of interfaces, one for the current
// interface pointer, the current interface is compared in memory.
// The other checks have one temp for loading the object's class.
static size_t NumberOfCheckCastTemps(bool emit_read_barrier, TypeCheckKind type_check_kind) {
  return 1 + NumberOfInstanceOfTemps(emit_read_barrier, type_check_kind);
}

void LocationsBuilderX86_64::VisitInstanceOf(HInstanceOf* instruction) {
  LocationSummary::CallKind call_kind = LocationSummary::kNoCall;
  TypeCheckKind type_check_kind = instruction->GetTypeCheckKind();
  bool baker_read_barrier_slow_path = false;
  switch (type_check_kind) {
    case TypeCheckKind::kExactCheck:
    case TypeCheckKind::kAbstractClassCheck:
    case TypeCheckKind::kClassHierarchyCheck:
    case TypeCheckKind::kArrayObjectCheck:
    case TypeCheckKind::kInterfaceCheck: {
      bool needs_read_barrier = codegen_->InstanceOfNeedsReadBarrier(instruction);
      call_kind = needs_read_barrier ? LocationSummary::kCallOnSlowPath : LocationSummary::kNoCall;
      baker_read_barrier_slow_path = (kUseBakerReadBarrier && needs_read_barrier) &&
                                     (type_check_kind != TypeCheckKind::kInterfaceCheck);
      break;
    }
    case TypeCheckKind::kArrayCheck:
    case TypeCheckKind::kUnresolvedCheck:
      call_kind = LocationSummary::kCallOnSlowPath;
      break;
    case TypeCheckKind::kBitstringCheck:
      break;
  }

  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(instruction, call_kind);
  if (baker_read_barrier_slow_path) {
    locations->SetCustomSlowPathCallerSaves(RegisterSet::Empty());  // No caller-save registers.
  }
  locations->SetInAt(0, Location::RequiresRegister());
  if (type_check_kind == TypeCheckKind::kBitstringCheck) {
    locations->SetInAt(1, Location::ConstantLocation(instruction->InputAt(1)));
    locations->SetInAt(2, Location::ConstantLocation(instruction->InputAt(2)));
    locations->SetInAt(3, Location::ConstantLocation(instruction->InputAt(3)));
  } else if (type_check_kind == TypeCheckKind::kInterfaceCheck) {
    locations->SetInAt(1, Location::RequiresRegister());
  } else {
    locations->SetInAt(1, Location::Any());
  }
  // Note that TypeCheckSlowPathX86_64 uses this "out" register too.
  locations->SetOut(Location::RequiresRegister());
  locations->AddRegisterTemps(
      NumberOfInstanceOfTemps(codegen_->EmitReadBarrier(), type_check_kind));
}

void InstructionCodeGeneratorX86_64::VisitInstanceOf(HInstanceOf* instruction) {
  TypeCheckKind type_check_kind = instruction->GetTypeCheckKind();
  LocationSummary* locations = instruction->GetLocations();
  Location obj_loc = locations->InAt(0);
  CpuRegister obj = obj_loc.AsRegister<CpuRegister>();
  Location cls = locations->InAt(1);
  Location out_loc =  locations->Out();
  CpuRegister out = out_loc.AsRegister<CpuRegister>();
  const size_t num_temps = NumberOfInstanceOfTemps(codegen_->EmitReadBarrier(), type_check_kind);
  DCHECK_LE(num_temps, 1u);
  Location maybe_temp_loc = (num_temps >= 1u) ? locations->GetTemp(0) : Location::NoLocation();
  const uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();
  const uint32_t super_offset = mirror::Class::SuperClassOffset().Int32Value();
  const uint32_t component_offset = mirror::Class::ComponentTypeOffset().Int32Value();
  const uint32_t primitive_offset = mirror::Class::PrimitiveTypeOffset().Int32Value();
  const uint32_t iftable_offset = mirror::Class::IfTableOffset().Uint32Value();
  const uint32_t array_length_offset = mirror::Array::LengthOffset().Uint32Value();
  const uint32_t object_array_data_offset =
      mirror::Array::DataOffset(kHeapReferenceSize).Uint32Value();
  SlowPathCode* slow_path = nullptr;
  NearLabel done, zero;

  // Return 0 if `obj` is null.
  // Avoid null check if we know obj is not null.
  if (instruction->MustDoNullCheck()) {
    __ testl(obj, obj);
    __ j(kEqual, &zero);
  }

  switch (type_check_kind) {
    case TypeCheckKind::kExactCheck: {
      ReadBarrierOption read_barrier_option =
          codegen_->ReadBarrierOptionForInstanceOf(instruction);
      // /* HeapReference<Class> */ out = obj->klass_
      GenerateReferenceLoadTwoRegisters(instruction,
                                        out_loc,
                                        obj_loc,
                                        class_offset,
                                        read_barrier_option);
      if (cls.IsRegister()) {
        __ cmpl(out, cls.AsRegister<CpuRegister>());
      } else {
        DCHECK(cls.IsStackSlot()) << cls;
        __ cmpl(out, Address(CpuRegister(RSP), cls.GetStackIndex()));
      }
      if (zero.IsLinked()) {
        // Classes must be equal for the instanceof to succeed.
        __ j(kNotEqual, &zero);
        __ movl(out, Immediate(1));
        __ jmp(&done);
      } else {
        __ setcc(kEqual, out);
        // setcc only sets the low byte.
        __ andl(out, Immediate(1));
      }
      break;
    }

    case TypeCheckKind::kAbstractClassCheck: {
      ReadBarrierOption read_barrier_option =
          codegen_->ReadBarrierOptionForInstanceOf(instruction);
      // /* HeapReference<Class> */ out = obj->klass_
      GenerateReferenceLoadTwoRegisters(instruction,
                                        out_loc,
                                        obj_loc,
                                        class_offset,
                                        read_barrier_option);
      // If the class is abstract, we eagerly fetch the super class of the
      // object to avoid doing a comparison we know will fail.
      NearLabel loop, success;
      __ Bind(&loop);
      // /* HeapReference<Class> */ out = out->super_class_
      GenerateReferenceLoadOneRegister(instruction,
                                       out_loc,
                                       super_offset,
                                       maybe_temp_loc,
                                       read_barrier_option);
      __ testl(out, out);
      // If `out` is null, we use it for the result, and jump to `done`.
      __ j(kEqual, &done);
      if (cls.IsRegister()) {
        __ cmpl(out, cls.AsRegister<CpuRegister>());
      } else {
        DCHECK(cls.IsStackSlot()) << cls;
        __ cmpl(out, Address(CpuRegister(RSP), cls.GetStackIndex()));
      }
      __ j(kNotEqual, &loop);
      __ movl(out, Immediate(1));
      if (zero.IsLinked()) {
        __ jmp(&done);
      }
      break;
    }

    case TypeCheckKind::kClassHierarchyCheck: {
      ReadBarrierOption read_barrier_option =
          codegen_->ReadBarrierOptionForInstanceOf(instruction);
      // /* HeapReference<Class> */ out = obj->klass_
      GenerateReferenceLoadTwoRegisters(instruction,
                                        out_loc,
                                        obj_loc,
                                        class_offset,
                                        read_barrier_option);
      // Walk over the class hierarchy to find a match.
      NearLabel loop, success;
      __ Bind(&loop);
      if (cls.IsRegister()) {
        __ cmpl(out, cls.AsRegister<CpuRegister>());
      } else {
        DCHECK(cls.IsStackSlot()) << cls;
        __ cmpl(out, Address(CpuRegister(RSP), cls.GetStackIndex()));
      }
      __ j(kEqual, &success);
      // /* HeapReference<Class> */ out = out->super_class_
      GenerateReferenceLoadOneRegister(instruction,
                                       out_loc,
                                       super_offset,
                                       maybe_temp_loc,
                                       read_barrier_option);
      __ testl(out, out);
      __ j(kNotEqual, &loop);
      // If `out` is null, we use it for the result, and jump to `done`.
      __ jmp(&done);
      __ Bind(&success);
      __ movl(out, Immediate(1));
      if (zero.IsLinked()) {
        __ jmp(&done);
      }
      break;
    }

    case TypeCheckKind::kArrayObjectCheck: {
      ReadBarrierOption read_barrier_option =
          codegen_->ReadBarrierOptionForInstanceOf(instruction);
      // /* HeapReference<Class> */ out = obj->klass_
      GenerateReferenceLoadTwoRegisters(instruction,
                                        out_loc,
                                        obj_loc,
                                        class_offset,
                                        read_barrier_option);
      // Do an exact check.
      NearLabel exact_check;
      if (cls.IsRegister()) {
        __ cmpl(out, cls.AsRegister<CpuRegister>());
      } else {
        DCHECK(cls.IsStackSlot()) << cls;
        __ cmpl(out, Address(CpuRegister(RSP), cls.GetStackIndex()));
      }
      __ j(kEqual, &exact_check);
      // Otherwise, we need to check that the object's class is a non-primitive array.
      // /* HeapReference<Class> */ out = out->component_type_
      GenerateReferenceLoadOneRegister(instruction,
                                       out_loc,
                                       component_offset,
                                       maybe_temp_loc,
                                       read_barrier_option);
      __ testl(out, out);
      // If `out` is null, we use it for the result, and jump to `done`.
      __ j(kEqual, &done);
      __ cmpw(Address(out, primitive_offset), Immediate(Primitive::kPrimNot));
      __ j(kNotEqual, &zero);
      __ Bind(&exact_check);
      __ movl(out, Immediate(1));
      __ jmp(&done);
      break;
    }

    case TypeCheckKind::kArrayCheck: {
      // No read barrier since the slow path will retry upon failure.
      // /* HeapReference<Class> */ out = obj->klass_
      GenerateReferenceLoadTwoRegisters(instruction,
                                        out_loc,
                                        obj_loc,
                                        class_offset,
                                        kWithoutReadBarrier);
      if (cls.IsRegister()) {
        __ cmpl(out, cls.AsRegister<CpuRegister>());
      } else {
        DCHECK(cls.IsStackSlot()) << cls;
        __ cmpl(out, Address(CpuRegister(RSP), cls.GetStackIndex()));
      }
      DCHECK(locations->OnlyCallsOnSlowPath());
      slow_path = new (codegen_->GetScopedAllocator()) TypeCheckSlowPathX86_64(
          instruction, /* is_fatal= */ false);
      codegen_->AddSlowPath(slow_path);
      __ j(kNotEqual, slow_path->GetEntryLabel());
      __ movl(out, Immediate(1));
      if (zero.IsLinked()) {
        __ jmp(&done);
      }
      break;
    }

    case TypeCheckKind::kInterfaceCheck: {
      if (codegen_->InstanceOfNeedsReadBarrier(instruction)) {
        DCHECK(locations->OnlyCallsOnSlowPath());
        slow_path = new (codegen_->GetScopedAllocator()) TypeCheckSlowPathX86_64(
            instruction, /* is_fatal= */ false);
        codegen_->AddSlowPath(slow_path);
        if (codegen_->EmitNonBakerReadBarrier()) {
          __ jmp(slow_path->GetEntryLabel());
          break;
        }
        // For Baker read barrier, take the slow path while marking.
        __ gs()->cmpl(
            Address::Absolute(Thread::IsGcMarkingOffset<kX86_64PointerSize>(), /* no_rip= */ true),
            Immediate(0));
        __ j(kNotEqual, slow_path->GetEntryLabel());
      }

      // Fast-path without read barriers.
      CpuRegister temp = maybe_temp_loc.AsRegister<CpuRegister>();
      // /* HeapReference<Class> */ temp = obj->klass_
      __ movl(temp, Address(obj, class_offset));
      __ MaybeUnpoisonHeapReference(temp);
      // /* HeapReference<Class> */ temp = temp->iftable_
      __ movl(temp, Address(temp, iftable_offset));
      __ MaybeUnpoisonHeapReference(temp);
      // Load the size of the `IfTable`. The `Class::iftable_` is never null.
      __ movl(out, Address(temp, array_length_offset));
      // Maybe poison the `cls` for direct comparison with memory.
      __ MaybePoisonHeapReference(cls.AsRegister<CpuRegister>());
      // Loop through the iftable and check if any class matches.
      NearLabel loop, end;
      __ Bind(&loop);
      // Check if we still have an entry to compare.
      __ subl(out, Immediate(2));
      __ j(kNegative, (zero.IsLinked() && !kPoisonHeapReferences) ? &zero : &end);
      // Go to next interface if the classes do not match.
      __ cmpl(cls.AsRegister<CpuRegister>(),
              CodeGeneratorX86_64::ArrayAddress(temp, out_loc, TIMES_4, object_array_data_offset));
      __ j(kNotEqual, &loop);
      if (zero.IsLinked()) {
        __ movl(out, Immediate(1));
        // If `cls` was poisoned above, unpoison it.
        __ MaybeUnpoisonHeapReference(cls.AsRegister<CpuRegister>());
        __ jmp(&done);
        if (kPoisonHeapReferences) {
          // The false case needs to unpoison the class before jumping to `zero`.
          __ Bind(&end);
          __ UnpoisonHeapReference(cls.AsRegister<CpuRegister>());
          __ jmp(&zero);
        }
      } else {
        // To reduce branching, use the fact that the false case branches with a `-2` in `out`.
        __ movl(out, Immediate(-1));
        __ Bind(&end);
        __ addl(out, Immediate(2));
        // If `cls` was poisoned above, unpoison it.
        __ MaybeUnpoisonHeapReference(cls.AsRegister<CpuRegister>());
      }
      break;
    }

    case TypeCheckKind::kUnresolvedCheck: {
      // Note that we indeed only call on slow path, but we always go
      // into the slow path for the unresolved check case.
      //
      // We cannot directly call the InstanceofNonTrivial runtime
      // entry point without resorting to a type checking slow path
      // here (i.e. by calling InvokeRuntime directly), as it would
      // require to assign fixed registers for the inputs of this
      // HInstanceOf instruction (following the runtime calling
      // convention), which might be cluttered by the potential first
      // read barrier emission at the beginning of this method.
      //
      // TODO: Introduce a new runtime entry point taking the object
      // to test (instead of its class) as argument, and let it deal
      // with the read barrier issues. This will let us refactor this
      // case of the `switch` code as it was previously (with a direct
      // call to the runtime not using a type checking slow path).
      // This should also be beneficial for the other cases above.
      DCHECK(locations->OnlyCallsOnSlowPath());
      slow_path = new (codegen_->GetScopedAllocator()) TypeCheckSlowPathX86_64(
          instruction, /* is_fatal= */ false);
      codegen_->AddSlowPath(slow_path);
      __ jmp(slow_path->GetEntryLabel());
      break;
    }

    case TypeCheckKind::kBitstringCheck: {
      // /* HeapReference<Class> */ temp = obj->klass_
      GenerateReferenceLoadTwoRegisters(instruction,
                                        out_loc,
                                        obj_loc,
                                        class_offset,
                                        kWithoutReadBarrier);

      GenerateBitstringTypeCheckCompare(instruction, out);
      if (zero.IsLinked()) {
        __ j(kNotEqual, &zero);
        __ movl(out, Immediate(1));
        __ jmp(&done);
      } else {
        __ setcc(kEqual, out);
        // setcc only sets the low byte.
        __ andl(out, Immediate(1));
      }
      break;
    }
  }

  if (zero.IsLinked()) {
    __ Bind(&zero);
    __ xorl(out, out);
  }

  if (done.IsLinked()) {
    __ Bind(&done);
  }

  if (slow_path != nullptr) {
    __ Bind(slow_path->GetExitLabel());
  }
}

void LocationsBuilderX86_64::VisitCheckCast(HCheckCast* instruction) {
  TypeCheckKind type_check_kind = instruction->GetTypeCheckKind();
  LocationSummary::CallKind call_kind = codegen_->GetCheckCastCallKind(instruction);
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(instruction, call_kind);
  locations->SetInAt(0, Location::RequiresRegister());
  if (type_check_kind == TypeCheckKind::kInterfaceCheck) {
    // Require a register for the interface check since there is a loop that compares the class to
    // a memory address.
    locations->SetInAt(1, Location::RequiresRegister());
  } else if (type_check_kind == TypeCheckKind::kBitstringCheck) {
    locations->SetInAt(1, Location::ConstantLocation(instruction->InputAt(1)));
    locations->SetInAt(2, Location::ConstantLocation(instruction->InputAt(2)));
    locations->SetInAt(3, Location::ConstantLocation(instruction->InputAt(3)));
  } else {
    locations->SetInAt(1, Location::Any());
  }
  locations->AddRegisterTemps(NumberOfCheckCastTemps(codegen_->EmitReadBarrier(), type_check_kind));
}

void InstructionCodeGeneratorX86_64::VisitCheckCast(HCheckCast* instruction) {
  TypeCheckKind type_check_kind = instruction->GetTypeCheckKind();
  LocationSummary* locations = instruction->GetLocations();
  Location obj_loc = locations->InAt(0);
  CpuRegister obj = obj_loc.AsRegister<CpuRegister>();
  Location cls = locations->InAt(1);
  Location temp_loc = locations->GetTemp(0);
  CpuRegister temp = temp_loc.AsRegister<CpuRegister>();
  const size_t num_temps = NumberOfCheckCastTemps(codegen_->EmitReadBarrier(), type_check_kind);
  DCHECK_GE(num_temps, 1u);
  DCHECK_LE(num_temps, 2u);
  Location maybe_temp2_loc = (num_temps >= 2u) ? locations->GetTemp(1) : Location::NoLocation();
  const uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();
  const uint32_t super_offset = mirror::Class::SuperClassOffset().Int32Value();
  const uint32_t component_offset = mirror::Class::ComponentTypeOffset().Int32Value();
  const uint32_t primitive_offset = mirror::Class::PrimitiveTypeOffset().Int32Value();
  const uint32_t iftable_offset = mirror::Class::IfTableOffset().Uint32Value();
  const uint32_t array_length_offset = mirror::Array::LengthOffset().Uint32Value();
  const uint32_t object_array_data_offset =
      mirror::Array::DataOffset(kHeapReferenceSize).Uint32Value();

  bool is_type_check_slow_path_fatal = codegen_->IsTypeCheckSlowPathFatal(instruction);
  SlowPathCode* type_check_slow_path =
      new (codegen_->GetScopedAllocator()) TypeCheckSlowPathX86_64(
          instruction, is_type_check_slow_path_fatal);
  codegen_->AddSlowPath(type_check_slow_path);


  NearLabel done;
  // Avoid null check if we know obj is not null.
  if (instruction->MustDoNullCheck()) {
    __ testl(obj, obj);
    __ j(kEqual, &done);
  }

  switch (type_check_kind) {
    case TypeCheckKind::kExactCheck:
    case TypeCheckKind::kArrayCheck: {
      // /* HeapReference<Class> */ temp = obj->klass_
      GenerateReferenceLoadTwoRegisters(instruction,
                                        temp_loc,
                                        obj_loc,
                                        class_offset,
                                        kWithoutReadBarrier);
      if (cls.IsRegister()) {
        __ cmpl(temp, cls.AsRegister<CpuRegister>());
      } else {
        DCHECK(cls.IsStackSlot()) << cls;
        __ cmpl(temp, Address(CpuRegister(RSP), cls.GetStackIndex()));
      }
      // Jump to slow path for throwing the exception or doing a
      // more involved array check.
      __ j(kNotEqual, type_check_slow_path->GetEntryLabel());
      break;
    }

    case TypeCheckKind::kAbstractClassCheck: {
      // /* HeapReference<Class> */ temp = obj->klass_
      GenerateReferenceLoadTwoRegisters(instruction,
                                        temp_loc,
                                        obj_loc,
                                        class_offset,
                                        kWithoutReadBarrier);
      // If the class is abstract, we eagerly fetch the super class of the
      // object to avoid doing a comparison we know will fail.
      NearLabel loop;
      __ Bind(&loop);
      // /* HeapReference<Class> */ temp = temp->super_class_
      GenerateReferenceLoadOneRegister(instruction,
                                       temp_loc,
                                       super_offset,
                                       maybe_temp2_loc,
                                       kWithoutReadBarrier);

      // If the class reference currently in `temp` is null, jump to the slow path to throw the
      // exception.
      __ testl(temp, temp);
      // Otherwise, compare the classes.
      __ j(kZero, type_check_slow_path->GetEntryLabel());
      if (cls.IsRegister()) {
        __ cmpl(temp, cls.AsRegister<CpuRegister>());
      } else {
        DCHECK(cls.IsStackSlot()) << cls;
        __ cmpl(temp, Address(CpuRegister(RSP), cls.GetStackIndex()));
      }
      __ j(kNotEqual, &loop);
      break;
    }

    case TypeCheckKind::kClassHierarchyCheck: {
      // /* HeapReference<Class> */ temp = obj->klass_
      GenerateReferenceLoadTwoRegisters(instruction,
                                        temp_loc,
                                        obj_loc,
                                        class_offset,
                                        kWithoutReadBarrier);
      // Walk over the class hierarchy to find a match.
      NearLabel loop;
      __ Bind(&loop);
      if (cls.IsRegister()) {
        __ cmpl(temp, cls.AsRegister<CpuRegister>());
      } else {
        DCHECK(cls.IsStackSlot()) << cls;
        __ cmpl(temp, Address(CpuRegister(RSP), cls.GetStackIndex()));
      }
      __ j(kEqual, &done);

      // /* HeapReference<Class> */ temp = temp->super_class_
      GenerateReferenceLoadOneRegister(instruction,
                                       temp_loc,
                                       super_offset,
                                       maybe_temp2_loc,
                                       kWithoutReadBarrier);

      // If the class reference currently in `temp` is not null, jump
      // back at the beginning of the loop.
      __ testl(temp, temp);
      __ j(kNotZero, &loop);
      // Otherwise, jump to the slow path to throw the exception.
      __ jmp(type_check_slow_path->GetEntryLabel());
      break;
    }

    case TypeCheckKind::kArrayObjectCheck: {
      // /* HeapReference<Class> */ temp = obj->klass_
      GenerateReferenceLoadTwoRegisters(instruction,
                                        temp_loc,
                                        obj_loc,
                                        class_offset,
                                        kWithoutReadBarrier);
      // Do an exact check.
      NearLabel check_non_primitive_component_type;
      if (cls.IsRegister()) {
        __ cmpl(temp, cls.AsRegister<CpuRegister>());
      } else {
        DCHECK(cls.IsStackSlot()) << cls;
        __ cmpl(temp, Address(CpuRegister(RSP), cls.GetStackIndex()));
      }
      __ j(kEqual, &done);

      // Otherwise, we need to check that the object's class is a non-primitive array.
      // /* HeapReference<Class> */ temp = temp->component_type_
      GenerateReferenceLoadOneRegister(instruction,
                                       temp_loc,
                                       component_offset,
                                       maybe_temp2_loc,
                                       kWithoutReadBarrier);

      // If the component type is not null (i.e. the object is indeed
      // an array), jump to label `check_non_primitive_component_type`
      // to further check that this component type is not a primitive
      // type.
      __ testl(temp, temp);
      // Otherwise, jump to the slow path to throw the exception.
      __ j(kZero, type_check_slow_path->GetEntryLabel());
      __ cmpw(Address(temp, primitive_offset), Immediate(Primitive::kPrimNot));
      __ j(kNotEqual, type_check_slow_path->GetEntryLabel());
      break;
    }

    case TypeCheckKind::kUnresolvedCheck: {
      // We always go into the type check slow path for the unresolved case.
      //
      // We cannot directly call the CheckCast runtime entry point
      // without resorting to a type checking slow path here (i.e. by
      // calling InvokeRuntime directly), as it would require to
      // assign fixed registers for the inputs of this HInstanceOf
      // instruction (following the runtime calling convention), which
      // might be cluttered by the potential first read barrier
      // emission at the beginning of this method.
      __ jmp(type_check_slow_path->GetEntryLabel());
      break;
    }

    case TypeCheckKind::kInterfaceCheck: {
      // Fast path for the interface check. Try to avoid read barriers to improve the fast path.
      // We can not get false positives by doing this.
      // /* HeapReference<Class> */ temp = obj->klass_
      GenerateReferenceLoadTwoRegisters(instruction,
                                        temp_loc,
                                        obj_loc,
                                        class_offset,
                                        kWithoutReadBarrier);

      // /* HeapReference<Class> */ temp = temp->iftable_
      GenerateReferenceLoadOneRegister(instruction,
                                       temp_loc,
                                       iftable_offset,
                                       maybe_temp2_loc,
                                       kWithoutReadBarrier);
      // Load the size of the `IfTable`. The `Class::iftable_` is never null.
      __ movl(maybe_temp2_loc.AsRegister<CpuRegister>(), Address(temp, array_length_offset));
      // Maybe poison the `cls` for direct comparison with memory.
      __ MaybePoisonHeapReference(cls.AsRegister<CpuRegister>());
      // Loop through the iftable and check if any class matches.
      NearLabel start_loop;
      __ Bind(&start_loop);
      // Check if we still have an entry to compare.
      __ subl(maybe_temp2_loc.AsRegister<CpuRegister>(), Immediate(2));
      __ j(kNegative, type_check_slow_path->GetEntryLabel());
      // Go to next interface if the classes do not match.
      __ cmpl(cls.AsRegister<CpuRegister>(),
              CodeGeneratorX86_64::ArrayAddress(temp,
                                                maybe_temp2_loc,
                                                TIMES_4,
                                                object_array_data_offset));
      __ j(kNotEqual, &start_loop);  // Return if same class.
      // If `cls` was poisoned above, unpoison it.
      __ MaybeUnpoisonHeapReference(cls.AsRegister<CpuRegister>());
      break;
    }

    case TypeCheckKind::kBitstringCheck: {
      // /* HeapReference<Class> */ temp = obj->klass_
      GenerateReferenceLoadTwoRegisters(instruction,
                                        temp_loc,
                                        obj_loc,
                                        class_offset,
                                        kWithoutReadBarrier);

      GenerateBitstringTypeCheckCompare(instruction, temp);
      __ j(kNotEqual, type_check_slow_path->GetEntryLabel());
      break;
    }
  }

  if (done.IsLinked()) {
    __ Bind(&done);
  }

  __ Bind(type_check_slow_path->GetExitLabel());
}

void LocationsBuilderX86_64::VisitMonitorOperation(HMonitorOperation* instruction) {
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(
      instruction, LocationSummary::kCallOnMainOnly);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
}

void InstructionCodeGeneratorX86_64::VisitMonitorOperation(HMonitorOperation* instruction) {
  codegen_->InvokeRuntime(instruction->IsEnter() ? kQuickLockObject : kQuickUnlockObject,
                          instruction);
  if (instruction->IsEnter()) {
    CheckEntrypointTypes<kQuickLockObject, void, mirror::Object*>();
  } else {
    CheckEntrypointTypes<kQuickUnlockObject, void, mirror::Object*>();
  }
}

void LocationsBuilderX86_64::VisitX86AndNot(HX86AndNot* instruction) {
  DCHECK(codegen_->GetInstructionSetFeatures().HasAVX2());
  DCHECK(DataType::IsIntOrLongType(instruction->GetType())) << instruction->GetType();
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  // There is no immediate variant of negated bitwise and in X86.
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void LocationsBuilderX86_64::VisitX86MaskOrResetLeastSetBit(HX86MaskOrResetLeastSetBit* instruction) {
  DCHECK(codegen_->GetInstructionSetFeatures().HasAVX2());
  DCHECK(DataType::IsIntOrLongType(instruction->GetType())) << instruction->GetType();
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorX86_64::VisitX86AndNot(HX86AndNot* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);
  Location dest = locations->Out();
  __ andn(dest.AsRegister<CpuRegister>(), first.AsRegister<CpuRegister>(), second.AsRegister<CpuRegister>());
}

void InstructionCodeGeneratorX86_64::VisitX86MaskOrResetLeastSetBit(HX86MaskOrResetLeastSetBit* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Location src = locations->InAt(0);
  Location dest = locations->Out();
  switch (instruction->GetOpKind()) {
    case HInstruction::kAnd:
      __ blsr(dest.AsRegister<CpuRegister>(), src.AsRegister<CpuRegister>());
      break;
    case HInstruction::kXor:
      __ blsmsk(dest.AsRegister<CpuRegister>(), src.AsRegister<CpuRegister>());
      break;
    default:
      LOG(FATAL) << "Unreachable";
  }
}

void LocationsBuilderX86_64::VisitAnd(HAnd* instruction) { HandleBitwiseOperation(instruction); }
void LocationsBuilderX86_64::VisitOr(HOr* instruction) { HandleBitwiseOperation(instruction); }
void LocationsBuilderX86_64::VisitXor(HXor* instruction) { HandleBitwiseOperation(instruction); }

void LocationsBuilderX86_64::HandleBitwiseOperation(HBinaryOperation* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(instruction, LocationSummary::kNoCall);
  DCHECK(instruction->GetResultType() == DataType::Type::kInt32
         || instruction->GetResultType() == DataType::Type::kInt64);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::Any());
  locations->SetOut(Location::SameAsFirstInput());
}

void InstructionCodeGeneratorX86_64::VisitAnd(HAnd* instruction) {
  HandleBitwiseOperation(instruction);
}

void InstructionCodeGeneratorX86_64::VisitOr(HOr* instruction) {
  HandleBitwiseOperation(instruction);
}

void InstructionCodeGeneratorX86_64::VisitXor(HXor* instruction) {
  HandleBitwiseOperation(instruction);
}

void InstructionCodeGeneratorX86_64::HandleBitwiseOperation(HBinaryOperation* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);
  DCHECK(first.Equals(locations->Out()));

  if (instruction->GetResultType() == DataType::Type::kInt32) {
    if (second.IsRegister()) {
      if (instruction->IsAnd()) {
        __ andl(first.AsRegister<CpuRegister>(), second.AsRegister<CpuRegister>());
      } else if (instruction->IsOr()) {
        __ orl(first.AsRegister<CpuRegister>(), second.AsRegister<CpuRegister>());
      } else {
        DCHECK(instruction->IsXor());
        __ xorl(first.AsRegister<CpuRegister>(), second.AsRegister<CpuRegister>());
      }
    } else if (second.IsConstant()) {
      Immediate imm(second.GetConstant()->AsIntConstant()->GetValue());
      if (instruction->IsAnd()) {
        __ andl(first.AsRegister<CpuRegister>(), imm);
      } else if (instruction->IsOr()) {
        __ orl(first.AsRegister<CpuRegister>(), imm);
      } else {
        DCHECK(instruction->IsXor());
        __ xorl(first.AsRegister<CpuRegister>(), imm);
      }
    } else {
      Address address(CpuRegister(RSP), second.GetStackIndex());
      if (instruction->IsAnd()) {
        __ andl(first.AsRegister<CpuRegister>(), address);
      } else if (instruction->IsOr()) {
        __ orl(first.AsRegister<CpuRegister>(), address);
      } else {
        DCHECK(instruction->IsXor());
        __ xorl(first.AsRegister<CpuRegister>(), address);
      }
    }
  } else {
    DCHECK_EQ(instruction->GetResultType(), DataType::Type::kInt64);
    CpuRegister first_reg = first.AsRegister<CpuRegister>();
    bool second_is_constant = false;
    int64_t value = 0;
    if (second.IsConstant()) {
      second_is_constant = true;
      value = second.GetConstant()->AsLongConstant()->GetValue();
    }
    bool is_int32_value = IsInt<32>(value);

    if (instruction->IsAnd()) {
      if (second_is_constant) {
        if (is_int32_value) {
          __ andq(first_reg, Immediate(static_cast<int32_t>(value)));
        } else {
          __ andq(first_reg, codegen_->LiteralInt64Address(value));
        }
      } else if (second.IsDoubleStackSlot()) {
        __ andq(first_reg, Address(CpuRegister(RSP), second.GetStackIndex()));
      } else {
        __ andq(first_reg, second.AsRegister<CpuRegister>());
      }
    } else if (instruction->IsOr()) {
      if (second_is_constant) {
        if (is_int32_value) {
          __ orq(first_reg, Immediate(static_cast<int32_t>(value)));
        } else {
          __ orq(first_reg, codegen_->LiteralInt64Address(value));
        }
      } else if (second.IsDoubleStackSlot()) {
        __ orq(first_reg, Address(CpuRegister(RSP), second.GetStackIndex()));
      } else {
        __ orq(first_reg, second.AsRegister<CpuRegister>());
      }
    } else {
      DCHECK(instruction->IsXor());
      if (second_is_constant) {
        if (is_int32_value) {
          __ xorq(first_reg, Immediate(static_cast<int32_t>(value)));
        } else {
          __ xorq(first_reg, codegen_->LiteralInt64Address(value));
        }
      } else if (second.IsDoubleStackSlot()) {
        __ xorq(first_reg, Address(CpuRegister(RSP), second.GetStackIndex()));
      } else {
        __ xorq(first_reg, second.AsRegister<CpuRegister>());
      }
    }
  }
}

void InstructionCodeGeneratorX86_64::GenerateReferenceLoadOneRegister(
    HInstruction* instruction,
    Location out,
    uint32_t offset,
    Location maybe_temp,
    ReadBarrierOption read_barrier_option) {
  CpuRegister out_reg = out.AsRegister<CpuRegister>();
  if (read_barrier_option == kWithReadBarrier) {
    DCHECK(codegen_->EmitReadBarrier());
    if (kUseBakerReadBarrier) {
      // Load with fast path based Baker's read barrier.
      // /* HeapReference<Object> */ out = *(out + offset)
      codegen_->GenerateFieldLoadWithBakerReadBarrier(
          instruction, out, out_reg, offset, /* needs_null_check= */ false);
    } else {
      // Load with slow path based read barrier.
      // Save the value of `out` into `maybe_temp` before overwriting it
      // in the following move operation, as we will need it for the
      // read barrier below.
      DCHECK(maybe_temp.IsRegister()) << maybe_temp;
      __ movl(maybe_temp.AsRegister<CpuRegister>(), out_reg);
      // /* HeapReference<Object> */ out = *(out + offset)
      __ movl(out_reg, Address(out_reg, offset));
      codegen_->GenerateReadBarrierSlow(instruction, out, out, maybe_temp, offset);
    }
  } else {
    // Plain load with no read barrier.
    // /* HeapReference<Object> */ out = *(out + offset)
    __ movl(out_reg, Address(out_reg, offset));
    __ MaybeUnpoisonHeapReference(out_reg);
  }
}

void InstructionCodeGeneratorX86_64::GenerateReferenceLoadTwoRegisters(
    HInstruction* instruction,
    Location out,
    Location obj,
    uint32_t offset,
    ReadBarrierOption read_barrier_option) {
  CpuRegister out_reg = out.AsRegister<CpuRegister>();
  CpuRegister obj_reg = obj.AsRegister<CpuRegister>();
  if (read_barrier_option == kWithReadBarrier) {
    DCHECK(codegen_->EmitReadBarrier());
    if (kUseBakerReadBarrier) {
      // Load with fast path based Baker's read barrier.
      // /* HeapReference<Object> */ out = *(obj + offset)
      codegen_->GenerateFieldLoadWithBakerReadBarrier(
          instruction, out, obj_reg, offset, /* needs_null_check= */ false);
    } else {
      // Load with slow path based read barrier.
      // /* HeapReference<Object> */ out = *(obj + offset)
      __ movl(out_reg, Address(obj_reg, offset));
      codegen_->GenerateReadBarrierSlow(instruction, out, out, obj, offset);
    }
  } else {
    // Plain load with no read barrier.
    // /* HeapReference<Object> */ out = *(obj + offset)
    __ movl(out_reg, Address(obj_reg, offset));
    __ MaybeUnpoisonHeapReference(out_reg);
  }
}

void InstructionCodeGeneratorX86_64::GenerateGcRootFieldLoad(
    HInstruction* instruction,
    Location root,
    const Address& address,
    Label* fixup_label,
    ReadBarrierOption read_barrier_option) {
  CpuRegister root_reg = root.AsRegister<CpuRegister>();
  if (read_barrier_option == kWithReadBarrier) {
    DCHECK(codegen_->EmitReadBarrier());
    if (kUseBakerReadBarrier) {
      // Fast path implementation of art::ReadBarrier::BarrierForRoot when
      // Baker's read barrier are used:
      //
      //   root = obj.field;
      //   temp = Thread::Current()->pReadBarrierMarkReg ## root.reg()
      //   if (temp != null) {
      //     root = temp(root)
      //   }

      // /* GcRoot<mirror::Object> */ root = *address
      __ movl(root_reg, address);
      if (fixup_label != nullptr) {
        __ Bind(fixup_label);
      }
      static_assert(
          sizeof(mirror::CompressedReference<mirror::Object>) == sizeof(GcRoot<mirror::Object>),
          "art::mirror::CompressedReference<mirror::Object> and art::GcRoot<mirror::Object> "
          "have different sizes.");
      static_assert(sizeof(mirror::CompressedReference<mirror::Object>) == sizeof(int32_t),
                    "art::mirror::CompressedReference<mirror::Object> and int32_t "
                    "have different sizes.");

      // Slow path marking the GC root `root`.
      SlowPathCode* slow_path = new (codegen_->GetScopedAllocator()) ReadBarrierMarkSlowPathX86_64(
          instruction, root, /* unpoison_ref_before_marking= */ false);
      codegen_->AddSlowPath(slow_path);

      // Test the `Thread::Current()->pReadBarrierMarkReg ## root.reg()` entrypoint.
      const int32_t entry_point_offset =
          Thread::ReadBarrierMarkEntryPointsOffset<kX86_64PointerSize>(root.reg());
      __ gs()->cmpl(Address::Absolute(entry_point_offset, /* no_rip= */ true), Immediate(0));
      // The entrypoint is null when the GC is not marking.
      __ j(kNotEqual, slow_path->GetEntryLabel());
      __ Bind(slow_path->GetExitLabel());
    } else {
      // GC root loaded through a slow path for read barriers other
      // than Baker's.
      // /* GcRoot<mirror::Object>* */ root = address
      __ leaq(root_reg, address);
      if (fixup_label != nullptr) {
        __ Bind(fixup_label);
      }
      // /* mirror::Object* */ root = root->Read()
      codegen_->GenerateReadBarrierForRootSlow(instruction, root, root);
    }
  } else {
    // Plain GC root load with no read barrier.
    // /* GcRoot<mirror::Object> */ root = *address
    __ movl(root_reg, address);
    if (fixup_label != nullptr) {
      __ Bind(fixup_label);
    }
    // Note that GC roots are not affected by heap poisoning, thus we
    // do not have to unpoison `root_reg` here.
  }
}

void CodeGeneratorX86_64::GenerateFieldLoadWithBakerReadBarrier(HInstruction* instruction,
                                                                Location ref,
                                                                CpuRegister obj,
                                                                uint32_t offset,
                                                                bool needs_null_check) {
  DCHECK(EmitBakerReadBarrier());

  // /* HeapReference<Object> */ ref = *(obj + offset)
  Address src(obj, offset);
  GenerateReferenceLoadWithBakerReadBarrier(instruction, ref, obj, src, needs_null_check);
}

void CodeGeneratorX86_64::GenerateArrayLoadWithBakerReadBarrier(HInstruction* instruction,
                                                                Location ref,
                                                                CpuRegister obj,
                                                                uint32_t data_offset,
                                                                Location index,
                                                                bool needs_null_check) {
  DCHECK(EmitBakerReadBarrier());

  static_assert(
      sizeof(mirror::HeapReference<mirror::Object>) == sizeof(int32_t),
      "art::mirror::HeapReference<art::mirror::Object> and int32_t have different sizes.");
  // /* HeapReference<Object> */ ref =
  //     *(obj + data_offset + index * sizeof(HeapReference<Object>))
  Address src = CodeGeneratorX86_64::ArrayAddress(obj, index, TIMES_4, data_offset);
  GenerateReferenceLoadWithBakerReadBarrier(instruction, ref, obj, src, needs_null_check);
}

void CodeGeneratorX86_64::GenerateReferenceLoadWithBakerReadBarrier(HInstruction* instruction,
                                                                    Location ref,
                                                                    CpuRegister obj,
                                                                    const Address& src,
                                                                    bool needs_null_check,
                                                                    bool always_update_field,
                                                                    CpuRegister* temp1,
                                                                    CpuRegister* temp2) {
  DCHECK(EmitBakerReadBarrier());

  // In slow path based read barriers, the read barrier call is
  // inserted after the original load. However, in fast path based
  // Baker's read barriers, we need to perform the load of
  // mirror::Object::monitor_ *before* the original reference load.
  // This load-load ordering is required by the read barrier.
  // The fast path/slow path (for Baker's algorithm) should look like:
  //
  //   uint32_t rb_state = Lockword(obj->monitor_).ReadBarrierState();
  //   lfence;  // Load fence or artificial data dependency to prevent load-load reordering
  //   HeapReference<Object> ref = *src;  // Original reference load.
  //   bool is_gray = (rb_state == ReadBarrier::GrayState());
  //   if (is_gray) {
  //     ref = ReadBarrier::Mark(ref);  // Performed by runtime entrypoint slow path.
  //   }
  //
  // Note: the original implementation in ReadBarrier::Barrier is
  // slightly more complex as:
  // - it implements the load-load fence using a data dependency on
  //   the high-bits of rb_state, which are expected to be all zeroes
  //   (we use CodeGeneratorX86_64::GenerateMemoryBarrier instead
  //   here, which is a no-op thanks to the x86-64 memory model);
  // - it performs additional checks that we do not do here for
  //   performance reasons.

  CpuRegister ref_reg = ref.AsRegister<CpuRegister>();
  uint32_t monitor_offset = mirror::Object::MonitorOffset().Int32Value();

  // Given the numeric representation, it's enough to check the low bit of the rb_state.
  static_assert(ReadBarrier::NonGrayState() == 0, "Expecting non-gray to have value 0");
  static_assert(ReadBarrier::GrayState() == 1, "Expecting gray to have value 1");
  constexpr uint32_t gray_byte_position = LockWord::kReadBarrierStateShift / kBitsPerByte;
  constexpr uint32_t gray_bit_position = LockWord::kReadBarrierStateShift % kBitsPerByte;
  constexpr int32_t test_value = static_cast<int8_t>(1 << gray_bit_position);

  // if (rb_state == ReadBarrier::GrayState())
  //   ref = ReadBarrier::Mark(ref);
  // At this point, just do the "if" and make sure that flags are preserved until the branch.
  __ testb(Address(obj, monitor_offset + gray_byte_position), Immediate(test_value));
  if (needs_null_check) {
    MaybeRecordImplicitNullCheck(instruction);
  }

  // Load fence to prevent load-load reordering.
  // Note that this is a no-op, thanks to the x86-64 memory model.
  GenerateMemoryBarrier(MemBarrierKind::kLoadAny);

  // The actual reference load.
  // /* HeapReference<Object> */ ref = *src
  __ movl(ref_reg, src);  // Flags are unaffected.

  // Note: Reference unpoisoning modifies the flags, so we need to delay it after the branch.
  // Slow path marking the object `ref` when it is gray.
  SlowPathCode* slow_path;
  if (always_update_field) {
    DCHECK(temp1 != nullptr);
    DCHECK(temp2 != nullptr);
    slow_path = new (GetScopedAllocator()) ReadBarrierMarkAndUpdateFieldSlowPathX86_64(
        instruction, ref, obj, src, /* unpoison_ref_before_marking= */ true, *temp1, *temp2);
  } else {
    slow_path = new (GetScopedAllocator()) ReadBarrierMarkSlowPathX86_64(
        instruction, ref, /* unpoison_ref_before_marking= */ true);
  }
  AddSlowPath(slow_path);

  // We have done the "if" of the gray bit check above, now branch based on the flags.
  __ j(kNotZero, slow_path->GetEntryLabel());

  // Object* ref = ref_addr->AsMirrorPtr()
  __ MaybeUnpoisonHeapReference(ref_reg);

  __ Bind(slow_path->GetExitLabel());
}

void CodeGeneratorX86_64::GenerateReadBarrierSlow(HInstruction* instruction,
                                                  Location out,
                                                  Location ref,
                                                  Location obj,
                                                  uint32_t offset,
                                                  Location index) {
  DCHECK(EmitReadBarrier());

  // Insert a slow path based read barrier *after* the reference load.
  //
  // If heap poisoning is enabled, the unpoisoning of the loaded
  // reference will be carried out by the runtime within the slow
  // path.
  //
  // Note that `ref` currently does not get unpoisoned (when heap
  // poisoning is enabled), which is alright as the `ref` argument is
  // not used by the artReadBarrierSlow entry point.
  //
  // TODO: Unpoison `ref` when it is used by artReadBarrierSlow.
  SlowPathCode* slow_path = new (GetScopedAllocator())
      ReadBarrierForHeapReferenceSlowPathX86_64(instruction, out, ref, obj, offset, index);
  AddSlowPath(slow_path);

  __ jmp(slow_path->GetEntryLabel());
  __ Bind(slow_path->GetExitLabel());
}

void CodeGeneratorX86_64::MaybeGenerateReadBarrierSlow(HInstruction* instruction,
                                                       Location out,
                                                       Location ref,
                                                       Location obj,
                                                       uint32_t offset,
                                                       Location index) {
  if (EmitReadBarrier()) {
    // Baker's read barriers shall be handled by the fast path
    // (CodeGeneratorX86_64::GenerateReferenceLoadWithBakerReadBarrier).
    DCHECK(!kUseBakerReadBarrier);
    // If heap poisoning is enabled, unpoisoning will be taken care of
    // by the runtime within the slow path.
    GenerateReadBarrierSlow(instruction, out, ref, obj, offset, index);
  } else if (kPoisonHeapReferences) {
    __ UnpoisonHeapReference(out.AsRegister<CpuRegister>());
  }
}

void CodeGeneratorX86_64::GenerateReadBarrierForRootSlow(HInstruction* instruction,
                                                         Location out,
                                                         Location root) {
  DCHECK(EmitReadBarrier());

  // Insert a slow path based read barrier *after* the GC root load.
  //
  // Note that GC roots are not affected by heap poisoning, so we do
  // not need to do anything special for this here.
  SlowPathCode* slow_path =
      new (GetScopedAllocator()) ReadBarrierForRootSlowPathX86_64(instruction, out, root);
  AddSlowPath(slow_path);

  __ jmp(slow_path->GetEntryLabel());
  __ Bind(slow_path->GetExitLabel());
}

void LocationsBuilderX86_64::VisitBoundType([[maybe_unused]] HBoundType* instruction) {
  // Nothing to do, this should be removed during prepare for register allocator.
  LOG(FATAL) << "Unreachable";
}

void InstructionCodeGeneratorX86_64::VisitBoundType([[maybe_unused]] HBoundType* instruction) {
  // Nothing to do, this should be removed during prepare for register allocator.
  LOG(FATAL) << "Unreachable";
}

// Simple implementation of packed switch - generate cascaded compare/jumps.
void LocationsBuilderX86_64::VisitPackedSwitch(HPackedSwitch* switch_instr) {
  LocationSummary* locations =
      new (GetGraph()->GetAllocator()) LocationSummary(switch_instr, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->AddRegisterTemps(2);
}

void InstructionCodeGeneratorX86_64::VisitPackedSwitch(HPackedSwitch* switch_instr) {
  int32_t lower_bound = switch_instr->GetStartValue();
  uint32_t num_entries = switch_instr->GetNumEntries();
  LocationSummary* locations = switch_instr->GetLocations();
  CpuRegister value_reg_in = locations->InAt(0).AsRegister<CpuRegister>();
  CpuRegister temp_reg = locations->GetTemp(0).AsRegister<CpuRegister>();
  CpuRegister base_reg = locations->GetTemp(1).AsRegister<CpuRegister>();
  HBasicBlock* default_block = switch_instr->GetDefaultBlock();

  // Should we generate smaller inline compare/jumps?
  if (num_entries <= kPackedSwitchJumpTableThreshold) {
    // Figure out the correct compare values and jump conditions.
    // Handle the first compare/branch as a special case because it might
    // jump to the default case.
    DCHECK_GT(num_entries, 2u);
    Condition first_condition;
    uint32_t index;
    const ArenaVector<HBasicBlock*>& successors = switch_instr->GetBlock()->GetSuccessors();
    if (lower_bound != 0) {
      first_condition = kLess;
      __ cmpl(value_reg_in, Immediate(lower_bound));
      __ j(first_condition, codegen_->GetLabelOf(default_block));
      __ j(kEqual, codegen_->GetLabelOf(successors[0]));

      index = 1;
    } else {
      // Handle all the compare/jumps below.
      first_condition = kBelow;
      index = 0;
    }

    // Handle the rest of the compare/jumps.
    for (; index + 1 < num_entries; index += 2) {
      int32_t compare_to_value = lower_bound + index + 1;
      __ cmpl(value_reg_in, Immediate(compare_to_value));
      // Jump to successors[index] if value < case_value[index].
      __ j(first_condition, codegen_->GetLabelOf(successors[index]));
      // Jump to successors[index + 1] if value == case_value[index + 1].
      __ j(kEqual, codegen_->GetLabelOf(successors[index + 1]));
    }

    if (index != num_entries) {
      // There are an odd number of entries. Handle the last one.
      DCHECK_EQ(index + 1, num_entries);
      __ cmpl(value_reg_in, Immediate(static_cast<int32_t>(lower_bound + index)));
      __ j(kEqual, codegen_->GetLabelOf(successors[index]));
    }

    // And the default for any other value.
    if (!codegen_->GoesToNextBlock(switch_instr->GetBlock(), default_block)) {
      __ jmp(codegen_->GetLabelOf(default_block));
    }
    return;
  }

  // Remove the bias, if needed.
  Register value_reg_out = value_reg_in.AsRegister();
  if (lower_bound != 0) {
    __ leal(temp_reg, Address(value_reg_in, -lower_bound));
    value_reg_out = temp_reg.AsRegister();
  }
  CpuRegister value_reg(value_reg_out);

  // Is the value in range?
  __ cmpl(value_reg, Immediate(num_entries - 1));
  __ j(kAbove, codegen_->GetLabelOf(default_block));

  // We are in the range of the table.
  // Load the address of the jump table in the constant area.
  __ leaq(base_reg, codegen_->LiteralCaseTable(switch_instr));

  // Load the (signed) offset from the jump table.
  __ movsxd(temp_reg, Address(base_reg, value_reg, TIMES_4, 0));

  // Add the offset to the address of the table base.
  __ addq(temp_reg, base_reg);

  // And jump.
  __ jmp(temp_reg);
}

void LocationsBuilderX86_64::VisitIntermediateAddress(
    [[maybe_unused]] HIntermediateAddress* instruction) {
  LOG(FATAL) << "Unreachable";
}

void InstructionCodeGeneratorX86_64::VisitIntermediateAddress(
    [[maybe_unused]] HIntermediateAddress* instruction) {
  LOG(FATAL) << "Unreachable";
}

void CodeGeneratorX86_64::Load32BitValue(CpuRegister dest, int32_t value) {
  if (value == 0) {
    __ xorl(dest, dest);
  } else {
    __ movl(dest, Immediate(value));
  }
}

void CodeGeneratorX86_64::Load64BitValue(CpuRegister dest, int64_t value) {
  if (value == 0) {
    // Clears upper bits too.
    __ xorl(dest, dest);
  } else if (IsUint<32>(value)) {
    // We can use a 32 bit move, as it will zero-extend and is shorter.
    __ movl(dest, Immediate(static_cast<int32_t>(value)));
  } else {
    __ movq(dest, Immediate(value));
  }
}

void CodeGeneratorX86_64::Load32BitValue(XmmRegister dest, int32_t value) {
  if (value == 0) {
    __ xorps(dest, dest);
  } else {
    __ movss(dest, LiteralInt32Address(value));
  }
}

void CodeGeneratorX86_64::Load64BitValue(XmmRegister dest, int64_t value) {
  if (value == 0) {
    __ xorpd(dest, dest);
  } else {
    __ movsd(dest, LiteralInt64Address(value));
  }
}

void CodeGeneratorX86_64::Load32BitValue(XmmRegister dest, float value) {
  Load32BitValue(dest, bit_cast<int32_t, float>(value));
}

void CodeGeneratorX86_64::Load64BitValue(XmmRegister dest, double value) {
  Load64BitValue(dest, bit_cast<int64_t, double>(value));
}

void CodeGeneratorX86_64::Compare32BitValue(CpuRegister dest, int32_t value) {
  if (value == 0) {
    __ testl(dest, dest);
  } else {
    __ cmpl(dest, Immediate(value));
  }
}

void CodeGeneratorX86_64::Compare64BitValue(CpuRegister dest, int64_t value) {
  if (IsInt<32>(value)) {
    if (value == 0) {
      __ testq(dest, dest);
    } else {
      __ cmpq(dest, Immediate(static_cast<int32_t>(value)));
    }
  } else {
    // Value won't fit in an int.
    __ cmpq(dest, LiteralInt64Address(value));
  }
}

void CodeGeneratorX86_64::GenerateIntCompare(Location lhs, Location rhs) {
  CpuRegister lhs_reg = lhs.AsRegister<CpuRegister>();
  GenerateIntCompare(lhs_reg, rhs);
}

void CodeGeneratorX86_64::GenerateIntCompare(CpuRegister lhs, Location rhs) {
  if (rhs.IsConstant()) {
    int32_t value = CodeGenerator::GetInt32ValueOf(rhs.GetConstant());
    Compare32BitValue(lhs, value);
  } else if (rhs.IsStackSlot()) {
    __ cmpl(lhs, Address(CpuRegister(RSP), rhs.GetStackIndex()));
  } else {
    __ cmpl(lhs, rhs.AsRegister<CpuRegister>());
  }
}

void CodeGeneratorX86_64::GenerateLongCompare(Location lhs, Location rhs) {
  CpuRegister lhs_reg = lhs.AsRegister<CpuRegister>();
  if (rhs.IsConstant()) {
    int64_t value = rhs.GetConstant()->AsLongConstant()->GetValue();
    Compare64BitValue(lhs_reg, value);
  } else if (rhs.IsDoubleStackSlot()) {
    __ cmpq(lhs_reg, Address(CpuRegister(RSP), rhs.GetStackIndex()));
  } else {
    __ cmpq(lhs_reg, rhs.AsRegister<CpuRegister>());
  }
}

Address CodeGeneratorX86_64::ArrayAddress(CpuRegister obj,
                                          Location index,
                                          ScaleFactor scale,
                                          uint32_t data_offset) {
  return index.IsConstant()
      ? Address(obj, (index.GetConstant()->AsIntConstant()->GetValue() << scale) + data_offset)
      : Address(obj, index.AsRegister<CpuRegister>(), scale, data_offset);
}

void CodeGeneratorX86_64::Store64BitValueToStack(Location dest, int64_t value) {
  DCHECK(dest.IsDoubleStackSlot());
  if (IsInt<32>(value)) {
    // Can move directly as an int32 constant.
    __ movq(Address(CpuRegister(RSP), dest.GetStackIndex()),
            Immediate(static_cast<int32_t>(value)));
  } else {
    Load64BitValue(CpuRegister(TMP), value);
    __ movq(Address(CpuRegister(RSP), dest.GetStackIndex()), CpuRegister(TMP));
  }
}

/**
 * Class to handle late fixup of offsets into constant area.
 */
class RIPFixup : public AssemblerFixup, public ArenaObject<kArenaAllocCodeGenerator> {
 public:
  RIPFixup(CodeGeneratorX86_64& codegen, size_t offset)
      : codegen_(&codegen), offset_into_constant_area_(offset) {}

 protected:
  void SetOffset(size_t offset) { offset_into_constant_area_ = offset; }

  CodeGeneratorX86_64* codegen_;

 private:
  void Process(const MemoryRegion& region, int pos) override {
    // Patch the correct offset for the instruction.  We use the address of the
    // 'next' instruction, which is 'pos' (patch the 4 bytes before).
    int32_t constant_offset = codegen_->ConstantAreaStart() + offset_into_constant_area_;
    int32_t relative_position = constant_offset - pos;

    // Patch in the right value.
    region.StoreUnaligned<int32_t>(pos - 4, relative_position);
  }

  // Location in constant area that the fixup refers to.
  size_t offset_into_constant_area_;
};

/**
 t * Class to handle late fixup of offsets to a jump table that will be created in the
 * constant area.
 */
class JumpTableRIPFixup : public RIPFixup {
 public:
  JumpTableRIPFixup(CodeGeneratorX86_64& codegen, HPackedSwitch* switch_instr)
      : RIPFixup(codegen, -1), switch_instr_(switch_instr) {}

  void CreateJumpTable() {
    X86_64Assembler* assembler = codegen_->GetAssembler();

    // Ensure that the reference to the jump table has the correct offset.
    const int32_t offset_in_constant_table = assembler->ConstantAreaSize();
    SetOffset(offset_in_constant_table);

    // Compute the offset from the start of the function to this jump table.
    const int32_t current_table_offset = assembler->CodeSize() + offset_in_constant_table;

    // Populate the jump table with the correct values for the jump table.
    int32_t num_entries = switch_instr_->GetNumEntries();
    HBasicBlock* block = switch_instr_->GetBlock();
    const ArenaVector<HBasicBlock*>& successors = block->GetSuccessors();
    // The value that we want is the target offset - the position of the table.
    for (int32_t i = 0; i < num_entries; i++) {
      HBasicBlock* b = successors[i];
      Label* l = codegen_->GetLabelOf(b);
      DCHECK(l->IsBound());
      int32_t offset_to_block = l->Position() - current_table_offset;
      assembler->AppendInt32(offset_to_block);
    }
  }

 private:
  const HPackedSwitch* switch_instr_;
};

void CodeGeneratorX86_64::Finalize() {
  // Generate the constant area if needed.
  X86_64Assembler* assembler = GetAssembler();
  if (!assembler->IsConstantAreaEmpty() || !fixups_to_jump_tables_.empty()) {
    // Align to 4 byte boundary to reduce cache misses, as the data is 4 and 8 byte values.
    assembler->Align(4, 0);
    constant_area_start_ = assembler->CodeSize();

    // Populate any jump tables.
    for (JumpTableRIPFixup* jump_table : fixups_to_jump_tables_) {
      jump_table->CreateJumpTable();
    }

    // And now add the constant area to the generated code.
    assembler->AddConstantArea();
  }

  // And finish up.
  CodeGenerator::Finalize();
}

Address CodeGeneratorX86_64::LiteralDoubleAddress(double v) {
  AssemblerFixup* fixup = new (GetGraph()->GetAllocator()) RIPFixup(*this, __ AddDouble(v));
  return Address::RIP(fixup);
}

Address CodeGeneratorX86_64::LiteralFloatAddress(float v) {
  AssemblerFixup* fixup = new (GetGraph()->GetAllocator()) RIPFixup(*this, __ AddFloat(v));
  return Address::RIP(fixup);
}

Address CodeGeneratorX86_64::LiteralInt32Address(int32_t v) {
  AssemblerFixup* fixup = new (GetGraph()->GetAllocator()) RIPFixup(*this, __ AddInt32(v));
  return Address::RIP(fixup);
}

Address CodeGeneratorX86_64::LiteralInt64Address(int64_t v) {
  AssemblerFixup* fixup = new (GetGraph()->GetAllocator()) RIPFixup(*this, __ AddInt64(v));
  return Address::RIP(fixup);
}

// TODO: trg as memory.
void CodeGeneratorX86_64::MoveFromReturnRegister(Location trg, DataType::Type type) {
  if (!trg.IsValid()) {
    DCHECK_EQ(type, DataType::Type::kVoid);
    return;
  }

  DCHECK_NE(type, DataType::Type::kVoid);

  Location return_loc = InvokeDexCallingConventionVisitorX86_64().GetReturnLocation(type);
  if (trg.Equals(return_loc)) {
    return;
  }

  // Let the parallel move resolver take care of all of this.
  HParallelMove parallel_move(GetGraph()->GetAllocator());
  parallel_move.AddMove(return_loc, trg, type, nullptr);
  GetMoveResolver()->EmitNativeCode(&parallel_move);
}

Address CodeGeneratorX86_64::LiteralCaseTable(HPackedSwitch* switch_instr) {
  // Create a fixup to be used to create and address the jump table.
  JumpTableRIPFixup* table_fixup =
      new (GetGraph()->GetAllocator()) JumpTableRIPFixup(*this, switch_instr);

  // We have to populate the jump tables.
  fixups_to_jump_tables_.push_back(table_fixup);
  return Address::RIP(table_fixup);
}

void CodeGeneratorX86_64::MoveInt64ToAddress(const Address& addr_low,
                                             const Address& addr_high,
                                             int64_t v,
                                             HInstruction* instruction) {
  if (IsInt<32>(v)) {
    int32_t v_32 = v;
    __ movq(addr_low, Immediate(v_32));
    MaybeRecordImplicitNullCheck(instruction);
  } else {
    // Didn't fit in a register.  Do it in pieces.
    int32_t low_v = Low32Bits(v);
    int32_t high_v = High32Bits(v);
    __ movl(addr_low, Immediate(low_v));
    MaybeRecordImplicitNullCheck(instruction);
    __ movl(addr_high, Immediate(high_v));
  }
}

void CodeGeneratorX86_64::PatchJitRootUse(uint8_t* code,
                                          const uint8_t* roots_data,
                                          const PatchInfo<Label>& info,
                                          uint64_t index_in_table) const {
  uint32_t code_offset = info.label.Position() - kLabelPositionToLiteralOffsetAdjustment;
  uintptr_t address =
      reinterpret_cast<uintptr_t>(roots_data) + index_in_table * sizeof(GcRoot<mirror::Object>);
  using unaligned_uint32_t __attribute__((__aligned__(1))) = uint32_t;
  reinterpret_cast<unaligned_uint32_t*>(code + code_offset)[0] =
      dchecked_integral_cast<uint32_t>(address);
}

void CodeGeneratorX86_64::EmitJitRootPatches(uint8_t* code, const uint8_t* roots_data) {
  for (const PatchInfo<Label>& info : jit_string_patches_) {
    StringReference string_reference(info.target_dex_file, dex::StringIndex(info.offset_or_index));
    uint64_t index_in_table = GetJitStringRootIndex(string_reference);
    PatchJitRootUse(code, roots_data, info, index_in_table);
  }

  for (const PatchInfo<Label>& info : jit_class_patches_) {
    TypeReference type_reference(info.target_dex_file, dex::TypeIndex(info.offset_or_index));
    uint64_t index_in_table = GetJitClassRootIndex(type_reference);
    PatchJitRootUse(code, roots_data, info, index_in_table);
  }

  for (const PatchInfo<Label>& info : jit_method_type_patches_) {
    ProtoReference proto_reference(info.target_dex_file, dex::ProtoIndex(info.offset_or_index));
    uint64_t index_in_table = GetJitMethodTypeRootIndex(proto_reference);
    PatchJitRootUse(code, roots_data, info, index_in_table);
  }
}

bool LocationsBuilderX86_64::CpuHasAvxFeatureFlag() {
  return codegen_->GetInstructionSetFeatures().HasAVX();
}

bool LocationsBuilderX86_64::CpuHasAvx2FeatureFlag() {
  return codegen_->GetInstructionSetFeatures().HasAVX2();
}

bool InstructionCodeGeneratorX86_64::CpuHasAvxFeatureFlag() {
  return codegen_->GetInstructionSetFeatures().HasAVX();
}

bool InstructionCodeGeneratorX86_64::CpuHasAvx2FeatureFlag() {
  return codegen_->GetInstructionSetFeatures().HasAVX2();
}

void LocationsBuilderX86_64::VisitBitwiseNegatedRight(
    [[maybe_unused]] HBitwiseNegatedRight* instruction) {
  LOG(FATAL) << "Unimplemented";
}

void InstructionCodeGeneratorX86_64::VisitBitwiseNegatedRight(
    [[maybe_unused]] HBitwiseNegatedRight* instruction) {
  LOG(FATAL) << "Unimplemented";
}

#undef __

}  // namespace x86_64
}  // namespace art
