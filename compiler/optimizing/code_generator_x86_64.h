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

#ifndef ART_COMPILER_OPTIMIZING_CODE_GENERATOR_X86_64_H_
#define ART_COMPILER_OPTIMIZING_CODE_GENERATOR_X86_64_H_

#include "arch/x86_64/instruction_set_features_x86_64.h"
#include "base/macros.h"
#include "code_generator.h"
#include "driver/compiler_options.h"
#include "nodes.h"
#include "parallel_move_resolver.h"
#include "utils/x86_64/assembler_x86_64.h"

namespace art HIDDEN {
namespace x86_64 {

static constexpr Register kMethodRegisterArgument = RDI;

// Use a local definition to prevent copying mistakes.
static constexpr size_t kX86_64WordSize = static_cast<size_t>(kX86_64PointerSize);

// Some x86_64 instructions require a register to be available as temp.
static constexpr Register TMP = R11;

static constexpr Register kParameterCoreRegisters[] = { RSI, RDX, RCX, R8, R9 };
static constexpr FloatRegister kParameterFloatRegisters[] =
    { XMM0, XMM1, XMM2, XMM3, XMM4, XMM5, XMM6, XMM7 };

static constexpr size_t kParameterCoreRegistersLength = arraysize(kParameterCoreRegisters);
static constexpr size_t kParameterFloatRegistersLength = arraysize(kParameterFloatRegisters);

static constexpr Register kRuntimeParameterCoreRegisters[] = { RDI, RSI, RDX, RCX };
static constexpr size_t kRuntimeParameterCoreRegistersLength =
    arraysize(kRuntimeParameterCoreRegisters);
static constexpr FloatRegister kRuntimeParameterFpuRegisters[] = { XMM0, XMM1 };
static constexpr size_t kRuntimeParameterFpuRegistersLength =
    arraysize(kRuntimeParameterFpuRegisters);

// These XMM registers are non-volatile in ART ABI, but volatile in native ABI.
// If the ART ABI changes, this list must be updated.  It is used to ensure that
// these are not clobbered by any direct call to native code (such as math intrinsics).
static constexpr FloatRegister non_volatile_xmm_regs[] = { XMM12, XMM13, XMM14, XMM15 };

#define UNIMPLEMENTED_INTRINSIC_LIST_X86_64(V) \
  V(MathSignumFloat)                           \
  V(MathSignumDouble)                          \
  V(MathCopySignFloat)                         \
  V(MathCopySignDouble)                        \
  V(CRC32Update)                               \
  V(CRC32UpdateBytes)                          \
  V(CRC32UpdateByteBuffer)                     \
  V(FP16ToFloat)                               \
  V(FP16ToHalf)                                \
  V(FP16Floor)                                 \
  V(FP16Ceil)                                  \
  V(FP16Rint)                                  \
  V(FP16Greater)                               \
  V(FP16GreaterEquals)                         \
  V(FP16Less)                                  \
  V(FP16LessEquals)                            \
  V(FP16Compare)                               \
  V(FP16Min)                                   \
  V(FP16Max)                                   \
  V(IntegerRemainderUnsigned)                  \
  V(LongRemainderUnsigned)                     \
  V(StringStringIndexOf)                       \
  V(StringStringIndexOfAfter)                  \
  V(StringBufferAppend)                        \
  V(StringBufferLength)                        \
  V(StringBufferToString)                      \
  V(StringBuilderAppendObject)                 \
  V(StringBuilderAppendString)                 \
  V(StringBuilderAppendCharSequence)           \
  V(StringBuilderAppendCharArray)              \
  V(StringBuilderAppendBoolean)                \
  V(StringBuilderAppendChar)                   \
  V(StringBuilderAppendInt)                    \
  V(StringBuilderAppendLong)                   \
  V(StringBuilderAppendFloat)                  \
  V(StringBuilderAppendDouble)                 \
  V(StringBuilderLength)                       \
  V(StringBuilderToString)                     \
  V(UnsafeArrayBaseOffset)                     \
  /* 1.8 */                                    \
  V(JdkUnsafeArrayBaseOffset)                  \
  V(MethodHandleInvoke)                        \


class InvokeRuntimeCallingConvention : public CallingConvention<Register, FloatRegister> {
 public:
  InvokeRuntimeCallingConvention()
      : CallingConvention(kRuntimeParameterCoreRegisters,
                          kRuntimeParameterCoreRegistersLength,
                          kRuntimeParameterFpuRegisters,
                          kRuntimeParameterFpuRegistersLength,
                          kX86_64PointerSize) {}

 private:
  DISALLOW_COPY_AND_ASSIGN(InvokeRuntimeCallingConvention);
};

class InvokeDexCallingConvention : public CallingConvention<Register, FloatRegister> {
 public:
  InvokeDexCallingConvention() : CallingConvention(
      kParameterCoreRegisters,
      kParameterCoreRegistersLength,
      kParameterFloatRegisters,
      kParameterFloatRegistersLength,
      kX86_64PointerSize) {}

 private:
  DISALLOW_COPY_AND_ASSIGN(InvokeDexCallingConvention);
};

class CriticalNativeCallingConventionVisitorX86_64 : public InvokeDexCallingConventionVisitor {
 public:
  explicit CriticalNativeCallingConventionVisitorX86_64(bool for_register_allocation)
      : for_register_allocation_(for_register_allocation) {}

  virtual ~CriticalNativeCallingConventionVisitorX86_64() {}

  Location GetNextLocation(DataType::Type type) override;
  Location GetReturnLocation(DataType::Type type) const override;
  Location GetMethodLocation() const override;

  size_t GetStackOffset() const { return stack_offset_; }

 private:
  // Register allocator does not support adjusting frame size, so we cannot provide final locations
  // of stack arguments for register allocation. We ask the register allocator for any location and
  // move these arguments to the right place after adjusting the SP when generating the call.
  const bool for_register_allocation_;
  size_t gpr_index_ = 0u;
  size_t fpr_index_ = 0u;
  size_t stack_offset_ = 0u;

  DISALLOW_COPY_AND_ASSIGN(CriticalNativeCallingConventionVisitorX86_64);
};

class FieldAccessCallingConventionX86_64 : public FieldAccessCallingConvention {
 public:
  FieldAccessCallingConventionX86_64() {}

  Location GetObjectLocation() const override {
    return Location::RegisterLocation(RSI);
  }
  Location GetFieldIndexLocation() const override {
    return Location::RegisterLocation(RDI);
  }
  Location GetReturnLocation([[maybe_unused]] DataType::Type type) const override {
    return Location::RegisterLocation(RAX);
  }
  Location GetSetValueLocation([[maybe_unused]] DataType::Type type,
                               bool is_instance) const override {
    return is_instance
        ? Location::RegisterLocation(RDX)
        : Location::RegisterLocation(RSI);
  }
  Location GetFpuLocation([[maybe_unused]] DataType::Type type) const override {
    return Location::FpuRegisterLocation(XMM0);
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(FieldAccessCallingConventionX86_64);
};


class InvokeDexCallingConventionVisitorX86_64 : public InvokeDexCallingConventionVisitor {
 public:
  InvokeDexCallingConventionVisitorX86_64() {}
  virtual ~InvokeDexCallingConventionVisitorX86_64() {}

  Location GetNextLocation(DataType::Type type) override;
  Location GetReturnLocation(DataType::Type type) const override;
  Location GetMethodLocation() const override;

 private:
  InvokeDexCallingConvention calling_convention;

  DISALLOW_COPY_AND_ASSIGN(InvokeDexCallingConventionVisitorX86_64);
};

class CodeGeneratorX86_64;

class ParallelMoveResolverX86_64 : public ParallelMoveResolverWithSwap {
 public:
  ParallelMoveResolverX86_64(ArenaAllocator* allocator, CodeGeneratorX86_64* codegen)
      : ParallelMoveResolverWithSwap(allocator), codegen_(codegen) {}

  void EmitMove(size_t index) override;
  void EmitSwap(size_t index) override;
  void SpillScratch(int reg) override;
  void RestoreScratch(int reg) override;

  X86_64Assembler* GetAssembler() const;

 private:
  void Exchange32(CpuRegister reg, int mem);
  void Exchange32(XmmRegister reg, int mem);
  void Exchange64(CpuRegister reg1, CpuRegister reg2);
  void Exchange64(CpuRegister reg, int mem);
  void Exchange64(XmmRegister reg, int mem);
  void Exchange128(XmmRegister reg, int mem);
  void ExchangeMemory32(int mem1, int mem2);
  void ExchangeMemory64(int mem1, int mem2, int num_of_qwords);

  CodeGeneratorX86_64* const codegen_;

  DISALLOW_COPY_AND_ASSIGN(ParallelMoveResolverX86_64);
};

class LocationsBuilderX86_64 : public HGraphVisitor {
 public:
  LocationsBuilderX86_64(HGraph* graph, CodeGeneratorX86_64* codegen)
      : HGraphVisitor(graph), codegen_(codegen) {}

#define DECLARE_VISIT_INSTRUCTION(name, super)     \
  void Visit##name(H##name* instr) override;

  FOR_EACH_CONCRETE_INSTRUCTION_COMMON(DECLARE_VISIT_INSTRUCTION)
  FOR_EACH_CONCRETE_INSTRUCTION_X86_64(DECLARE_VISIT_INSTRUCTION)
  FOR_EACH_CONCRETE_INSTRUCTION_X86_COMMON(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

  void VisitInstruction(HInstruction* instruction) override {
    LOG(FATAL) << "Unreachable instruction " << instruction->DebugName()
               << " (id " << instruction->GetId() << ")";
  }

 private:
  void HandleInvoke(HInvoke* invoke);
  void HandleBitwiseOperation(HBinaryOperation* operation);
  void HandleCondition(HCondition* condition);
  void HandleShift(HBinaryOperation* operation);
  void HandleRotate(HBinaryOperation* rotate);
  void HandleFieldSet(HInstruction* instruction,
                      const FieldInfo& field_info,
                      WriteBarrierKind write_barrier_kind);
  void HandleFieldGet(HInstruction* instruction);
  bool CpuHasAvxFeatureFlag();
  bool CpuHasAvx2FeatureFlag();

  CodeGeneratorX86_64* const codegen_;
  InvokeDexCallingConventionVisitorX86_64 parameter_visitor_;

  DISALLOW_COPY_AND_ASSIGN(LocationsBuilderX86_64);
};

class InstructionCodeGeneratorX86_64 : public InstructionCodeGenerator {
 public:
  InstructionCodeGeneratorX86_64(HGraph* graph, CodeGeneratorX86_64* codegen);

#define DECLARE_VISIT_INSTRUCTION(name, super)     \
  void Visit##name(H##name* instr) override;

  FOR_EACH_CONCRETE_INSTRUCTION_COMMON(DECLARE_VISIT_INSTRUCTION)
  FOR_EACH_CONCRETE_INSTRUCTION_X86_64(DECLARE_VISIT_INSTRUCTION)
  FOR_EACH_CONCRETE_INSTRUCTION_X86_COMMON(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

  void VisitInstruction(HInstruction* instruction) override {
    LOG(FATAL) << "Unreachable instruction " << instruction->DebugName()
               << " (id " << instruction->GetId() << ")";
  }

  X86_64Assembler* GetAssembler() const { return assembler_; }

  // Generate a GC root reference load:
  //
  //   root <- *address
  //
  // while honoring read barriers based on read_barrier_option.
  void GenerateGcRootFieldLoad(HInstruction* instruction,
                               Location root,
                               const Address& address,
                               Label* fixup_label,
                               ReadBarrierOption read_barrier_option);
  void HandleFieldSet(HInstruction* instruction,
                      uint32_t value_index,
                      uint32_t extra_temp_index,
                      DataType::Type field_type,
                      Address field_addr,
                      CpuRegister base,
                      bool is_volatile,
                      bool is_atomic,
                      bool value_can_be_null,
                      bool byte_swap,
                      WriteBarrierKind write_barrier_kind);

  void Bswap(Location value, DataType::Type type, CpuRegister* temp = nullptr);

 private:
  // Generate code for the given suspend check. If not null, `successor`
  // is the block to branch to if the suspend check is not needed, and after
  // the suspend call.
  void GenerateSuspendCheck(HSuspendCheck* instruction, HBasicBlock* successor);
  void GenerateClassInitializationCheck(SlowPathCode* slow_path, CpuRegister class_reg);
  void GenerateBitstringTypeCheckCompare(HTypeCheckInstruction* check, CpuRegister temp);
  void HandleBitwiseOperation(HBinaryOperation* operation);
  void GenerateRemFP(HRem* rem);
  void DivRemOneOrMinusOne(HBinaryOperation* instruction);
  void DivByPowerOfTwo(HDiv* instruction);
  void RemByPowerOfTwo(HRem* instruction);
  void GenerateDivRemWithAnyConstant(HBinaryOperation* instruction);
  void GenerateDivRemIntegral(HBinaryOperation* instruction);
  void HandleCondition(HCondition* condition);
  void HandleShift(HBinaryOperation* operation);
  void HandleRotate(HBinaryOperation* rotate);

  void HandleFieldSet(HInstruction* instruction,
                      const FieldInfo& field_info,
                      bool value_can_be_null,
                      WriteBarrierKind write_barrier_kind);
  void HandleFieldGet(HInstruction* instruction, const FieldInfo& field_info);

  void GenerateMinMaxInt(LocationSummary* locations, bool is_min, DataType::Type type);
  void GenerateMinMaxFP(LocationSummary* locations, bool is_min, DataType::Type type);
  void GenerateMinMax(HBinaryOperation* minmax, bool is_min);
  void GenerateMethodEntryExitHook(HInstruction* instruction);

  // Generate a heap reference load using one register `out`:
  //
  //   out <- *(out + offset)
  //
  // while honoring heap poisoning and/or read barriers (if any).
  //
  // Location `maybe_temp` is used when generating a read barrier and
  // shall be a register in that case; it may be an invalid location
  // otherwise.
  void GenerateReferenceLoadOneRegister(HInstruction* instruction,
                                        Location out,
                                        uint32_t offset,
                                        Location maybe_temp,
                                        ReadBarrierOption read_barrier_option);
  // Generate a heap reference load using two different registers
  // `out` and `obj`:
  //
  //   out <- *(obj + offset)
  //
  // while honoring heap poisoning and/or read barriers (if any).
  //
  // Location `maybe_temp` is used when generating a Baker's (fast
  // path) read barrier and shall be a register in that case; it may
  // be an invalid location otherwise.
  void GenerateReferenceLoadTwoRegisters(HInstruction* instruction,
                                         Location out,
                                         Location obj,
                                         uint32_t offset,
                                         ReadBarrierOption read_barrier_option);

  void PushOntoFPStack(Location source, uint32_t temp_offset,
                       uint32_t stack_adjustment, bool is_float);
  void GenerateCompareTest(HCondition* condition);
  template<class LabelType>
  void GenerateTestAndBranch(HInstruction* instruction,
                             size_t condition_input_index,
                             LabelType* true_target,
                             LabelType* false_target);
  template<class LabelType>
  void GenerateCompareTestAndBranch(HCondition* condition,
                                    LabelType* true_target,
                                    LabelType* false_target);
  template<class LabelType>
  void GenerateFPJumps(HCondition* cond, LabelType* true_label, LabelType* false_label);

  void HandleGoto(HInstruction* got, HBasicBlock* successor);

  bool CpuHasAvxFeatureFlag();
  bool CpuHasAvx2FeatureFlag();

  X86_64Assembler* const assembler_;
  CodeGeneratorX86_64* const codegen_;

  DISALLOW_COPY_AND_ASSIGN(InstructionCodeGeneratorX86_64);
};

// Class for fixups to jump tables.
class JumpTableRIPFixup;

class CodeGeneratorX86_64 : public CodeGenerator {
 public:
  CodeGeneratorX86_64(HGraph* graph,
                  const CompilerOptions& compiler_options,
                  OptimizingCompilerStats* stats = nullptr);
  virtual ~CodeGeneratorX86_64() {}

  void GenerateFrameEntry() override;
  void GenerateFrameExit() override;
  void Bind(HBasicBlock* block) override;
  void MoveConstant(Location destination, int32_t value) override;
  void MoveLocation(Location dst, Location src, DataType::Type dst_type) override;
  void AddLocationAsTemp(Location location, LocationSummary* locations) override;

  size_t SaveCoreRegister(size_t stack_index, uint32_t reg_id) override;
  size_t RestoreCoreRegister(size_t stack_index, uint32_t reg_id) override;
  size_t SaveFloatingPointRegister(size_t stack_index, uint32_t reg_id) override;
  size_t RestoreFloatingPointRegister(size_t stack_index, uint32_t reg_id) override;

  // Generate code to invoke a runtime entry point.
  void InvokeRuntime(QuickEntrypointEnum entrypoint,
                     HInstruction* instruction,
                     SlowPathCode* slow_path = nullptr) override;

  // Generate code to invoke a runtime entry point, but do not record
  // PC-related information in a stack map.
  void InvokeRuntimeWithoutRecordingPcInfo(int32_t entry_point_offset,
                                           HInstruction* instruction,
                                           SlowPathCode* slow_path);

  void GenerateInvokeRuntime(int32_t entry_point_offset);

  size_t GetWordSize() const override {
    return kX86_64WordSize;
  }

  size_t GetSlowPathFPWidth() const override {
    return GetGraph()->HasSIMD()
        ? GetSIMDRegisterWidth()
        : 1 * kX86_64WordSize;  //  8 bytes == 1 x86_64 words for each spill
  }

  size_t GetCalleePreservedFPWidth() const override {
    return 1 * kX86_64WordSize;
  }

  size_t GetSIMDRegisterWidth() const override {
    return 2 * kX86_64WordSize;
  }

  HGraphVisitor* GetLocationBuilder() override {
    return &location_builder_;
  }

  HGraphVisitor* GetInstructionVisitor() override {
    return &instruction_visitor_;
  }

  X86_64Assembler* GetAssembler() override {
    return &assembler_;
  }

  const X86_64Assembler& GetAssembler() const override {
    return assembler_;
  }

  ParallelMoveResolverX86_64* GetMoveResolver() override {
    return &move_resolver_;
  }

  uintptr_t GetAddressOf(HBasicBlock* block) override {
    return GetLabelOf(block)->Position();
  }

  void SetupBlockedRegisters() const override;
  void DumpCoreRegister(std::ostream& stream, int reg) const override;
  void DumpFloatingPointRegister(std::ostream& stream, int reg) const override;
  void Finalize() override;

  InstructionSet GetInstructionSet() const override {
    return InstructionSet::kX86_64;
  }

  InstructionCodeGeneratorX86_64* GetInstructionCodegen() {
    return down_cast<InstructionCodeGeneratorX86_64*>(GetInstructionVisitor());
  }

  const X86_64InstructionSetFeatures& GetInstructionSetFeatures() const;

  // Emit a write barrier if:
  // A) emit_null_check is false
  // B) emit_null_check is true, and value is not null.
  void MaybeMarkGCCard(CpuRegister temp,
                       CpuRegister card,
                       CpuRegister object,
                       CpuRegister value,
                       bool emit_null_check);

  // Emit a write barrier unconditionally.
  void MarkGCCard(CpuRegister temp, CpuRegister card, CpuRegister object);

  // Crash if the card table is not valid. This check is only emitted for the CC GC. We assert
  // `(!clean || !self->is_gc_marking)`, since the card table should not be set to clean when the CC
  // GC is marking for eliminated write barriers.
  void CheckGCCardIsValid(CpuRegister temp, CpuRegister card, CpuRegister object);

  void GenerateMemoryBarrier(MemBarrierKind kind);

  // Helper method to move a value between two locations.
  void Move(Location destination, Location source);
  // Helper method to load a value of non-reference type from memory.
  void LoadFromMemoryNoReference(DataType::Type type, Location dst, Address src);

  Label* GetLabelOf(HBasicBlock* block) const {
    return CommonGetLabelOf<Label>(block_labels_, block);
  }

  void Initialize() override {
    block_labels_ = CommonInitializeLabels<Label>();
  }

  bool NeedsTwoRegisters([[maybe_unused]] DataType::Type type) const override { return false; }

  // Check if the desired_string_load_kind is supported. If it is, return it,
  // otherwise return a fall-back kind that should be used instead.
  HLoadString::LoadKind GetSupportedLoadStringKind(
      HLoadString::LoadKind desired_string_load_kind) override;

  // Check if the desired_class_load_kind is supported. If it is, return it,
  // otherwise return a fall-back kind that should be used instead.
  HLoadClass::LoadKind GetSupportedLoadClassKind(
      HLoadClass::LoadKind desired_class_load_kind) override;

  // Check if the desired_dispatch_info is supported. If it is, return it,
  // otherwise return a fall-back info that should be used instead.
  HInvokeStaticOrDirect::DispatchInfo GetSupportedInvokeStaticOrDirectDispatch(
      const HInvokeStaticOrDirect::DispatchInfo& desired_dispatch_info,
      ArtMethod* method) override;

  void LoadMethod(MethodLoadKind load_kind, Location temp, HInvoke* invoke);
  void GenerateStaticOrDirectCall(
      HInvokeStaticOrDirect* invoke, Location temp, SlowPathCode* slow_path = nullptr) override;
  void GenerateVirtualCall(
      HInvokeVirtual* invoke, Location temp, SlowPathCode* slow_path = nullptr) override;

  void RecordBootImageIntrinsicPatch(uint32_t intrinsic_data);
  void RecordBootImageRelRoPatch(uint32_t boot_image_offset);
  void RecordBootImageMethodPatch(HInvoke* invoke);
  void RecordAppImageMethodPatch(HInvoke* invoke);
  void RecordMethodBssEntryPatch(HInvoke* invoke);
  void RecordBootImageTypePatch(const DexFile& dex_file, dex::TypeIndex type_index);
  void RecordAppImageTypePatch(const DexFile& dex_file, dex::TypeIndex type_index);
  Label* NewTypeBssEntryPatch(HLoadClass* load_class);
  void RecordBootImageStringPatch(HLoadString* load_string);
  Label* NewStringBssEntryPatch(HLoadString* load_string);
  Label* NewMethodTypeBssEntryPatch(HLoadMethodType* load_method_type);
  void RecordBootImageJniEntrypointPatch(HInvokeStaticOrDirect* invoke);
  Label* NewJitRootStringPatch(const DexFile& dex_file,
                               dex::StringIndex string_index,
                               Handle<mirror::String> handle);
  Label* NewJitRootClassPatch(const DexFile& dex_file,
                              dex::TypeIndex type_index,
                              Handle<mirror::Class> handle);
  Label* NewJitRootMethodTypePatch(const DexFile& dex_file,
                                   dex::ProtoIndex proto_index,
                                   Handle<mirror::MethodType> method_type);

  void LoadBootImageAddress(CpuRegister reg, uint32_t boot_image_reference);
  void LoadIntrinsicDeclaringClass(CpuRegister reg, HInvoke* invoke);
  void LoadClassRootForIntrinsic(CpuRegister reg, ClassRoot class_root);

  void EmitLinkerPatches(ArenaVector<linker::LinkerPatch>* linker_patches) override;

  void PatchJitRootUse(uint8_t* code,
                       const uint8_t* roots_data,
                       const PatchInfo<Label>& info,
                       uint64_t index_in_table) const;

  void EmitJitRootPatches(uint8_t* code, const uint8_t* roots_data) override;

  // Fast path implementation of ReadBarrier::Barrier for a heap
  // reference field load when Baker's read barriers are used.
  void GenerateFieldLoadWithBakerReadBarrier(HInstruction* instruction,
                                             Location ref,
                                             CpuRegister obj,
                                             uint32_t offset,
                                             bool needs_null_check);
  // Fast path implementation of ReadBarrier::Barrier for a heap
  // reference array load when Baker's read barriers are used.
  void GenerateArrayLoadWithBakerReadBarrier(HInstruction* instruction,
                                             Location ref,
                                             CpuRegister obj,
                                             uint32_t data_offset,
                                             Location index,
                                             bool needs_null_check);
  // Factored implementation, used by GenerateFieldLoadWithBakerReadBarrier,
  // GenerateArrayLoadWithBakerReadBarrier and some intrinsics.
  //
  // Load the object reference located at address `src`, held by
  // object `obj`, into `ref`, and mark it if needed.  The base of
  // address `src` must be `obj`.
  //
  // If `always_update_field` is true, the value of the reference is
  // atomically updated in the holder (`obj`).  This operation
  // requires two temporary registers, which must be provided as
  // non-null pointers (`temp1` and `temp2`).
  void GenerateReferenceLoadWithBakerReadBarrier(HInstruction* instruction,
                                                 Location ref,
                                                 CpuRegister obj,
                                                 const Address& src,
                                                 bool needs_null_check,
                                                 bool always_update_field = false,
                                                 CpuRegister* temp1 = nullptr,
                                                 CpuRegister* temp2 = nullptr);

  // Generate a read barrier for a heap reference within `instruction`
  // using a slow path.
  //
  // A read barrier for an object reference read from the heap is
  // implemented as a call to the artReadBarrierSlow runtime entry
  // point, which is passed the values in locations `ref`, `obj`, and
  // `offset`:
  //
  //   mirror::Object* artReadBarrierSlow(mirror::Object* ref,
  //                                      mirror::Object* obj,
  //                                      uint32_t offset);
  //
  // The `out` location contains the value returned by
  // artReadBarrierSlow.
  //
  // When `index` provided (i.e., when it is different from
  // Location::NoLocation()), the offset value passed to
  // artReadBarrierSlow is adjusted to take `index` into account.
  void GenerateReadBarrierSlow(HInstruction* instruction,
                               Location out,
                               Location ref,
                               Location obj,
                               uint32_t offset,
                               Location index = Location::NoLocation());

  // If read barriers are enabled, generate a read barrier for a heap
  // reference using a slow path. If heap poisoning is enabled, also
  // unpoison the reference in `out`.
  void MaybeGenerateReadBarrierSlow(HInstruction* instruction,
                                    Location out,
                                    Location ref,
                                    Location obj,
                                    uint32_t offset,
                                    Location index = Location::NoLocation());

  // Generate a read barrier for a GC root within `instruction` using
  // a slow path.
  //
  // A read barrier for an object reference GC root is implemented as
  // a call to the artReadBarrierForRootSlow runtime entry point,
  // which is passed the value in location `root`:
  //
  //   mirror::Object* artReadBarrierForRootSlow(GcRoot<mirror::Object>* root);
  //
  // The `out` location contains the value returned by
  // artReadBarrierForRootSlow.
  void GenerateReadBarrierForRootSlow(HInstruction* instruction, Location out, Location root);

  int ConstantAreaStart() const {
    return constant_area_start_;
  }

  Address LiteralDoubleAddress(double v);
  Address LiteralFloatAddress(float v);
  Address LiteralInt32Address(int32_t v);
  Address LiteralInt64Address(int64_t v);

  // Load a 32/64-bit value into a register in the most efficient manner.
  void Load32BitValue(CpuRegister dest, int32_t value);
  void Load64BitValue(CpuRegister dest, int64_t value);
  void Load32BitValue(XmmRegister dest, int32_t value);
  void Load64BitValue(XmmRegister dest, int64_t value);
  void Load32BitValue(XmmRegister dest, float value);
  void Load64BitValue(XmmRegister dest, double value);

  // Compare a register with a 32/64-bit value in the most efficient manner.
  void Compare32BitValue(CpuRegister dest, int32_t value);
  void Compare64BitValue(CpuRegister dest, int64_t value);

  // Compare int values. Supports register locations for `lhs`.
  void GenerateIntCompare(Location lhs, Location rhs);
  void GenerateIntCompare(CpuRegister lhs, Location rhs);

  // Compare long values. Supports only register locations for `lhs`.
  void GenerateLongCompare(Location lhs, Location rhs);

  // Construct address for array access.
  static Address ArrayAddress(CpuRegister obj,
                              Location index,
                              ScaleFactor scale,
                              uint32_t data_offset);

  Address LiteralCaseTable(HPackedSwitch* switch_instr);

  // Store a 64 bit value into a DoubleStackSlot in the most efficient manner.
  void Store64BitValueToStack(Location dest, int64_t value);

  void MoveFromReturnRegister(Location trg, DataType::Type type) override;

  // Assign a 64 bit constant to an address.
  void MoveInt64ToAddress(const Address& addr_low,
                          const Address& addr_high,
                          int64_t v,
                          HInstruction* instruction);

  // Ensure that prior stores complete to memory before subsequent loads.
  // The locked add implementation will avoid serializing device memory, but will
  // touch (but not change) the top of the stack.
  // The 'non_temporal' parameter should be used to ensure ordering of non-temporal stores.
  void MemoryFence(bool force_mfence = false) {
    if (!force_mfence) {
      assembler_.lock()->addl(Address(CpuRegister(RSP), 0), Immediate(0));
    } else {
      assembler_.mfence();
    }
  }

  void IncreaseFrame(size_t adjustment) override;
  void DecreaseFrame(size_t adjustment) override;

  void GenerateNop() override;
  void GenerateImplicitNullCheck(HNullCheck* instruction) override;
  void GenerateExplicitNullCheck(HNullCheck* instruction) override;
  void MaybeGenerateInlineCacheCheck(HInstruction* instruction, CpuRegister cls);

  void MaybeIncrementHotness(HSuspendCheck* suspend_check, bool is_frame_entry);

  static void BlockNonVolatileXmmRegisters(LocationSummary* locations);

  // When we don't know the proper offset for the value, we use kPlaceholder32BitOffset.
  // We will fix this up in the linker later to have the right value.
  static constexpr int32_t kPlaceholder32BitOffset = 256;

 private:
  template <linker::LinkerPatch (*Factory)(size_t, const DexFile*, uint32_t, uint32_t)>
  static void EmitPcRelativeLinkerPatches(const ArenaDeque<PatchInfo<Label>>& infos,
                                          ArenaVector<linker::LinkerPatch>* linker_patches);

  // Labels for each block that will be compiled.
  Label* block_labels_;  // Indexed by block id.
  Label frame_entry_label_;
  LocationsBuilderX86_64 location_builder_;
  InstructionCodeGeneratorX86_64 instruction_visitor_;
  ParallelMoveResolverX86_64 move_resolver_;
  X86_64Assembler assembler_;

  // Offset to the start of the constant area in the assembled code.
  // Used for fixups to the constant area.
  int constant_area_start_;

  // PC-relative method patch info for kBootImageLinkTimePcRelative.
  ArenaDeque<PatchInfo<Label>> boot_image_method_patches_;
  // PC-relative method patch info for kAppImageRelRo.
  ArenaDeque<PatchInfo<Label>> app_image_method_patches_;
  // PC-relative method patch info for kBssEntry.
  ArenaDeque<PatchInfo<Label>> method_bss_entry_patches_;
  // PC-relative type patch info for kBootImageLinkTimePcRelative.
  ArenaDeque<PatchInfo<Label>> boot_image_type_patches_;
  // PC-relative type patch info for kAppImageRelRo.
  ArenaDeque<PatchInfo<Label>> app_image_type_patches_;
  // PC-relative type patch info for kBssEntry.
  ArenaDeque<PatchInfo<Label>> type_bss_entry_patches_;
  // PC-relative public type patch info for kBssEntryPublic.
  ArenaDeque<PatchInfo<Label>> public_type_bss_entry_patches_;
  // PC-relative package type patch info for kBssEntryPackage.
  ArenaDeque<PatchInfo<Label>> package_type_bss_entry_patches_;
  // PC-relative String patch info for kBootImageLinkTimePcRelative.
  ArenaDeque<PatchInfo<Label>> boot_image_string_patches_;
  // PC-relative String patch info for kBssEntry.
  ArenaDeque<PatchInfo<Label>> string_bss_entry_patches_;
  // PC-relative MethodType patch info for kBssEntry.
  ArenaDeque<PatchInfo<Label>> method_type_bss_entry_patches_;
  // PC-relative method patch info for kBootImageLinkTimePcRelative+kCallCriticalNative.
  ArenaDeque<PatchInfo<Label>> boot_image_jni_entrypoint_patches_;
  // PC-relative patch info for IntrinsicObjects for the boot image,
  // and for method/type/string patches for kBootImageRelRo otherwise.
  ArenaDeque<PatchInfo<Label>> boot_image_other_patches_;

  // Patches for string literals in JIT compiled code.
  ArenaDeque<PatchInfo<Label>> jit_string_patches_;
  // Patches for class literals in JIT compiled code.
  ArenaDeque<PatchInfo<Label>> jit_class_patches_;
  // Patches for method type in JIT compiled code.
  ArenaDeque<PatchInfo<Label>> jit_method_type_patches_;

  // Fixups for jump tables need to be handled specially.
  ArenaVector<JumpTableRIPFixup*> fixups_to_jump_tables_;

  DISALLOW_COPY_AND_ASSIGN(CodeGeneratorX86_64);
};

}  // namespace x86_64
}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_CODE_GENERATOR_X86_64_H_
