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

#ifndef ART_COMPILER_OPTIMIZING_CODE_GENERATOR_X86_H_
#define ART_COMPILER_OPTIMIZING_CODE_GENERATOR_X86_H_

#include "arch/x86/instruction_set_features_x86.h"
#include "base/macros.h"
#include "base/pointer_size.h"
#include "code_generator.h"
#include "dex/dex_file_types.h"
#include "driver/compiler_options.h"
#include "nodes.h"
#include "parallel_move_resolver.h"
#include "utils/x86/assembler_x86.h"

namespace art HIDDEN {
namespace x86 {

// Use a local definition to prevent copying mistakes.
static constexpr size_t kX86WordSize = static_cast<size_t>(kX86PointerSize);

class CodeGeneratorX86;

static constexpr Register kParameterCoreRegisters[] = { ECX, EDX, EBX };
static constexpr RegisterPair kParameterCorePairRegisters[] = { ECX_EDX, EDX_EBX };
static constexpr size_t kParameterCoreRegistersLength = arraysize(kParameterCoreRegisters);
static constexpr XmmRegister kParameterFpuRegisters[] = { XMM0, XMM1, XMM2, XMM3 };
static constexpr size_t kParameterFpuRegistersLength = arraysize(kParameterFpuRegisters);

static constexpr Register kRuntimeParameterCoreRegisters[] = { EAX, ECX, EDX, EBX };
static constexpr size_t kRuntimeParameterCoreRegistersLength =
    arraysize(kRuntimeParameterCoreRegisters);
static constexpr XmmRegister kRuntimeParameterFpuRegisters[] = { XMM0, XMM1, XMM2, XMM3 };
static constexpr size_t kRuntimeParameterFpuRegistersLength =
    arraysize(kRuntimeParameterFpuRegisters);

#define UNIMPLEMENTED_INTRINSIC_LIST_X86(V) \
  V(MathSignumFloat)                        \
  V(MathSignumDouble)                       \
  V(MathCopySignFloat)                      \
  V(MathCopySignDouble)                     \
  V(MathRoundDouble)                        \
  V(FloatIsInfinite)                        \
  V(DoubleIsInfinite)                       \
  V(IntegerHighestOneBit)                   \
  V(LongHighestOneBit)                      \
  V(LongDivideUnsigned)                     \
  V(IntegerRemainderUnsigned)               \
  V(LongRemainderUnsigned)                  \
  V(CRC32Update)                            \
  V(CRC32UpdateBytes)                       \
  V(CRC32UpdateByteBuffer)                  \
  V(FP16ToFloat)                            \
  V(FP16ToHalf)                             \
  V(FP16Floor)                              \
  V(FP16Ceil)                               \
  V(FP16Rint)                               \
  V(FP16Greater)                            \
  V(FP16GreaterEquals)                      \
  V(FP16Less)                               \
  V(FP16LessEquals)                         \
  V(FP16Compare)                            \
  V(FP16Min)                                \
  V(FP16Max)                                \
  V(MathMultiplyHigh)                       \
  V(StringStringIndexOf)                    \
  V(StringStringIndexOfAfter)               \
  V(StringBufferAppend)                     \
  V(StringBufferLength)                     \
  V(StringBufferToString)                   \
  V(StringBuilderAppendObject)              \
  V(StringBuilderAppendString)              \
  V(StringBuilderAppendCharSequence)        \
  V(StringBuilderAppendCharArray)           \
  V(StringBuilderAppendBoolean)             \
  V(StringBuilderAppendChar)                \
  V(StringBuilderAppendInt)                 \
  V(StringBuilderAppendLong)                \
  V(StringBuilderAppendFloat)               \
  V(StringBuilderAppendDouble)              \
  V(StringBuilderLength)                    \
  V(StringBuilderToString)                  \
  V(UnsafeArrayBaseOffset)                  \
  /* 1.8 */                                 \
  V(MethodHandleInvokeExact)                \
  V(MethodHandleInvoke)                     \
  /* OpenJDK 11 */                          \
  V(JdkUnsafeArrayBaseOffset)

class InvokeRuntimeCallingConvention : public CallingConvention<Register, XmmRegister> {
 public:
  InvokeRuntimeCallingConvention()
      : CallingConvention(kRuntimeParameterCoreRegisters,
                          kRuntimeParameterCoreRegistersLength,
                          kRuntimeParameterFpuRegisters,
                          kRuntimeParameterFpuRegistersLength,
                          kX86PointerSize) {}

 private:
  DISALLOW_COPY_AND_ASSIGN(InvokeRuntimeCallingConvention);
};

class InvokeDexCallingConvention : public CallingConvention<Register, XmmRegister> {
 public:
  InvokeDexCallingConvention() : CallingConvention(
      kParameterCoreRegisters,
      kParameterCoreRegistersLength,
      kParameterFpuRegisters,
      kParameterFpuRegistersLength,
      kX86PointerSize) {}

  RegisterPair GetRegisterPairAt(size_t argument_index) {
    DCHECK_LT(argument_index + 1, GetNumberOfRegisters());
    return kParameterCorePairRegisters[argument_index];
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(InvokeDexCallingConvention);
};

class InvokeDexCallingConventionVisitorX86 : public InvokeDexCallingConventionVisitor {
 public:
  InvokeDexCallingConventionVisitorX86() {}
  virtual ~InvokeDexCallingConventionVisitorX86() {}

  Location GetNextLocation(DataType::Type type) override;
  Location GetReturnLocation(DataType::Type type) const override;
  Location GetMethodLocation() const override;

 private:
  InvokeDexCallingConvention calling_convention;

  DISALLOW_COPY_AND_ASSIGN(InvokeDexCallingConventionVisitorX86);
};

class CriticalNativeCallingConventionVisitorX86 : public InvokeDexCallingConventionVisitor {
 public:
  explicit CriticalNativeCallingConventionVisitorX86(bool for_register_allocation)
      : for_register_allocation_(for_register_allocation) {}

  virtual ~CriticalNativeCallingConventionVisitorX86() {}

  Location GetNextLocation(DataType::Type type) override;
  Location GetReturnLocation(DataType::Type type) const override;
  Location GetMethodLocation() const override;

  size_t GetStackOffset() const { return stack_offset_; }

 private:
  // Register allocator does not support adjusting frame size, so we cannot provide final locations
  // of stack arguments for register allocation. We ask the register allocator for any location and
  // move these arguments to the right place after adjusting the SP when generating the call.
  const bool for_register_allocation_;
  size_t stack_offset_ = 0u;

  DISALLOW_COPY_AND_ASSIGN(CriticalNativeCallingConventionVisitorX86);
};

class FieldAccessCallingConventionX86 : public FieldAccessCallingConvention {
 public:
  FieldAccessCallingConventionX86() {}

  Location GetObjectLocation() const override {
    return Location::RegisterLocation(ECX);
  }
  Location GetFieldIndexLocation() const override {
    return Location::RegisterLocation(EAX);
  }
  Location GetReturnLocation(DataType::Type type) const override {
    return DataType::Is64BitType(type)
        ? Location::RegisterPairLocation(EAX, EDX)
        : Location::RegisterLocation(EAX);
  }
  Location GetSetValueLocation(DataType::Type type, bool is_instance) const override {
    return DataType::Is64BitType(type)
        ? (is_instance
            ? Location::RegisterPairLocation(EDX, EBX)
            : Location::RegisterPairLocation(ECX, EDX))
        : (is_instance
            ? Location::RegisterLocation(EDX)
            : Location::RegisterLocation(ECX));
  }
  Location GetFpuLocation([[maybe_unused]] DataType::Type type) const override {
    return Location::FpuRegisterLocation(XMM0);
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(FieldAccessCallingConventionX86);
};

class ParallelMoveResolverX86 : public ParallelMoveResolverWithSwap {
 public:
  ParallelMoveResolverX86(ArenaAllocator* allocator, CodeGeneratorX86* codegen)
      : ParallelMoveResolverWithSwap(allocator), codegen_(codegen) {}

  void EmitMove(size_t index) override;
  void EmitSwap(size_t index) override;
  void SpillScratch(int reg) override;
  void RestoreScratch(int reg) override;

  X86Assembler* GetAssembler() const;

 private:
  void Exchange(Register reg, int mem);
  void Exchange32(XmmRegister reg, int mem);
  void Exchange128(XmmRegister reg, int mem);
  void ExchangeMemory(int mem1, int mem2, int number_of_words);
  void MoveMemoryToMemory(int dst, int src, int number_of_words);

  CodeGeneratorX86* const codegen_;

  DISALLOW_COPY_AND_ASSIGN(ParallelMoveResolverX86);
};

class LocationsBuilderX86 : public HGraphVisitor {
 public:
  LocationsBuilderX86(HGraph* graph, CodeGeneratorX86* codegen)
      : HGraphVisitor(graph), codegen_(codegen) {}

#define DECLARE_VISIT_INSTRUCTION(name, super)     \
  void Visit##name(H##name* instr) override;

  FOR_EACH_CONCRETE_INSTRUCTION_COMMON(DECLARE_VISIT_INSTRUCTION)
  FOR_EACH_CONCRETE_INSTRUCTION_X86(DECLARE_VISIT_INSTRUCTION)
  FOR_EACH_CONCRETE_INSTRUCTION_X86_COMMON(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

  void VisitInstruction(HInstruction* instruction) override {
    LOG(FATAL) << "Unreachable instruction " << instruction->DebugName()
               << " (id " << instruction->GetId() << ")";
  }

 private:
  void HandleBitwiseOperation(HBinaryOperation* instruction);
  void HandleInvoke(HInvoke* invoke);
  void HandleCondition(HCondition* condition);
  void HandleRotate(HBinaryOperation* rotate);
  void HandleShift(HBinaryOperation* instruction);
  void HandleFieldSet(HInstruction* instruction,
                      const FieldInfo& field_info,
                      WriteBarrierKind write_barrier_kind);
  void HandleFieldGet(HInstruction* instruction, const FieldInfo& field_info);
  bool CpuHasAvxFeatureFlag();
  bool CpuHasAvx2FeatureFlag();

  CodeGeneratorX86* const codegen_;
  InvokeDexCallingConventionVisitorX86 parameter_visitor_;

  DISALLOW_COPY_AND_ASSIGN(LocationsBuilderX86);
};

class InstructionCodeGeneratorX86 : public InstructionCodeGenerator {
 public:
  InstructionCodeGeneratorX86(HGraph* graph, CodeGeneratorX86* codegen);

#define DECLARE_VISIT_INSTRUCTION(name, super)     \
  void Visit##name(H##name* instr) override;

  FOR_EACH_CONCRETE_INSTRUCTION_COMMON(DECLARE_VISIT_INSTRUCTION)
  FOR_EACH_CONCRETE_INSTRUCTION_X86(DECLARE_VISIT_INSTRUCTION)
  FOR_EACH_CONCRETE_INSTRUCTION_X86_COMMON(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

  void VisitInstruction(HInstruction* instruction) override {
    LOG(FATAL) << "Unreachable instruction " << instruction->DebugName()
               << " (id " << instruction->GetId() << ")";
  }

  X86Assembler* GetAssembler() const { return assembler_; }

  // The compare/jump sequence will generate about (1.5 * num_entries) instructions. A jump
  // table version generates 7 instructions and num_entries literals. Compare/jump sequence will
  // generates less code/data with a small num_entries.
  static constexpr uint32_t kPackedSwitchJumpTableThreshold = 5;

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
                      DataType::Type type,
                      Address field_addr,
                      Register base,
                      bool is_volatile,
                      bool value_can_be_null,
                      WriteBarrierKind write_barrier_kind);

 private:
  // Generate code for the given suspend check. If not null, `successor`
  // is the block to branch to if the suspend check is not needed, and after
  // the suspend call.
  void GenerateSuspendCheck(HSuspendCheck* check, HBasicBlock* successor);
  void GenerateClassInitializationCheck(SlowPathCode* slow_path, Register class_reg);
  void GenerateBitstringTypeCheckCompare(HTypeCheckInstruction* check, Register temp);
  void HandleBitwiseOperation(HBinaryOperation* instruction);
  void GenerateDivRemIntegral(HBinaryOperation* instruction);
  void DivRemOneOrMinusOne(HBinaryOperation* instruction);
  void DivByPowerOfTwo(HDiv* instruction);
  void RemByPowerOfTwo(HRem* instruction);
  void GenerateDivRemWithAnyConstant(HBinaryOperation* instruction);
  void GenerateRemFP(HRem* rem);
  void HandleCondition(HCondition* condition);
  void HandleShift(HBinaryOperation* instruction);
  void GenerateShlLong(const Location& loc, Register shifter);
  void GenerateShrLong(const Location& loc, Register shifter);
  void GenerateUShrLong(const Location& loc, Register shifter);
  void GenerateShlLong(const Location& loc, int shift);
  void GenerateShrLong(const Location& loc, int shift);
  void GenerateUShrLong(const Location& loc, int shift);
  void GenerateMinMaxInt(LocationSummary* locations, bool is_min, DataType::Type type);
  void GenerateMinMaxFP(LocationSummary* locations, bool is_min, DataType::Type type);
  void GenerateMinMax(HBinaryOperation* minmax, bool is_min);

  void HandleFieldSet(HInstruction* instruction,
                      const FieldInfo& field_info,
                      bool value_can_be_null,
                      WriteBarrierKind write_barrier_kind);
  void HandleFieldGet(HInstruction* instruction, const FieldInfo& field_info);
  void HandleRotate(HBinaryOperation* rotate);

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

  // Push value to FPU stack. `is_fp` specifies whether the value is floating point or not.
  // `is_wide` specifies whether it is long/double or not.
  void PushOntoFPStack(Location source, uint32_t temp_offset,
                       uint32_t stack_adjustment, bool is_fp, bool is_wide);

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
  template<class LabelType>
  void GenerateLongComparesAndJumps(HCondition* cond,
                                    LabelType* true_label,
                                    LabelType* false_label);

  void HandleGoto(HInstruction* got, HBasicBlock* successor);
  void GenPackedSwitchWithCompares(Register value_reg,
                                   int32_t lower_bound,
                                   uint32_t num_entries,
                                   HBasicBlock* switch_block,
                                   HBasicBlock* default_block);

  void GenerateFPCompare(Location lhs, Location rhs, HInstruction* insn, bool is_double);
  bool CpuHasAvxFeatureFlag();
  bool CpuHasAvx2FeatureFlag();

  void GenerateMethodEntryExitHook(HInstruction* instruction);

  X86Assembler* const assembler_;
  CodeGeneratorX86* const codegen_;

  DISALLOW_COPY_AND_ASSIGN(InstructionCodeGeneratorX86);
};

class JumpTableRIPFixup;

class CodeGeneratorX86 : public CodeGenerator {
 public:
  CodeGeneratorX86(HGraph* graph,
                   const CompilerOptions& compiler_options,
                   OptimizingCompilerStats* stats = nullptr);
  virtual ~CodeGeneratorX86() {}

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
    return kX86WordSize;
  }

  size_t GetSlowPathFPWidth() const override {
    return GetGraph()->HasSIMD()
        ? GetSIMDRegisterWidth()
        : 2 * kX86WordSize;  //  8 bytes == 2 words for each spill
  }

  size_t GetCalleePreservedFPWidth() const override {
    return 2 * kX86WordSize;
  }

  size_t GetSIMDRegisterWidth() const override {
    return 4 * kX86WordSize;
  }

  HGraphVisitor* GetLocationBuilder() override {
    return &location_builder_;
  }

  HGraphVisitor* GetInstructionVisitor() override {
    return &instruction_visitor_;
  }

  X86Assembler* GetAssembler() override {
    return &assembler_;
  }

  const X86Assembler& GetAssembler() const override {
    return assembler_;
  }

  uintptr_t GetAddressOf(HBasicBlock* block) override {
    return GetLabelOf(block)->Position();
  }

  void SetupBlockedRegisters() const override;

  void DumpCoreRegister(std::ostream& stream, int reg) const override;
  void DumpFloatingPointRegister(std::ostream& stream, int reg) const override;

  ParallelMoveResolverX86* GetMoveResolver() override {
    return &move_resolver_;
  }

  InstructionSet GetInstructionSet() const override {
    return InstructionSet::kX86;
  }

  const X86InstructionSetFeatures& GetInstructionSetFeatures() const;

  // Helper method to move a 32bits value between two locations.
  void Move32(Location destination, Location source);
  // Helper method to move a 64bits value between two locations.
  void Move64(Location destination, Location source);
  // Helper method to load a value from an address to a register.
  void LoadFromMemoryNoBarrier(DataType::Type dst_type,
                               Location dst,
                               Address src,
                               HInstruction* instr = nullptr,
                               XmmRegister temp = kNoXmmRegister,
                               bool is_atomic_load = false);
  // Helper method to move a primitive value from a location to an address.
  void MoveToMemory(DataType::Type src_type,
                    Location src,
                    Register dst_base,
                    Register dst_index = Register::kNoRegister,
                    ScaleFactor dst_scale = TIMES_1,
                    int32_t dst_disp = 0);

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
  // Generate a call to a static or direct method.
  void GenerateStaticOrDirectCall(
      HInvokeStaticOrDirect* invoke, Location temp, SlowPathCode* slow_path = nullptr) override;
  // Generate a call to a virtual method.
  void GenerateVirtualCall(
      HInvokeVirtual* invoke, Location temp, SlowPathCode* slow_path = nullptr) override;

  void RecordBootImageIntrinsicPatch(HX86ComputeBaseMethodAddress* method_address,
                                     uint32_t intrinsic_data);
  void RecordBootImageRelRoPatch(HX86ComputeBaseMethodAddress* method_address,
                                 uint32_t boot_image_offset);
  void RecordBootImageMethodPatch(HInvoke* invoke);
  void RecordAppImageMethodPatch(HInvoke* invoke);
  void RecordMethodBssEntryPatch(HInvoke* invoke);
  void RecordBootImageTypePatch(HLoadClass* load_class);
  void RecordAppImageTypePatch(HLoadClass* load_class);
  Label* NewTypeBssEntryPatch(HLoadClass* load_class);
  void RecordBootImageStringPatch(HLoadString* load_string);
  Label* NewStringBssEntryPatch(HLoadString* load_string);
  void RecordBootImageJniEntrypointPatch(HInvokeStaticOrDirect* invoke);

  void LoadBootImageAddress(Register reg,
                            uint32_t boot_image_reference,
                            HInvokeStaticOrDirect* invoke);
  void LoadIntrinsicDeclaringClass(Register reg, HInvokeStaticOrDirect* invoke);

  Label* NewJitRootStringPatch(const DexFile& dex_file,
                               dex::StringIndex string_index,
                               Handle<mirror::String> handle);
  Label* NewJitRootClassPatch(const DexFile& dex_file,
                              dex::TypeIndex type_index,
                              Handle<mirror::Class> handle);

  void MoveFromReturnRegister(Location trg, DataType::Type type) override;

  // Emit linker patches.
  void EmitLinkerPatches(ArenaVector<linker::LinkerPatch>* linker_patches) override;

  void PatchJitRootUse(uint8_t* code,
                       const uint8_t* roots_data,
                       const PatchInfo<Label>& info,
                       uint64_t index_in_table) const;
  void EmitJitRootPatches(uint8_t* code, const uint8_t* roots_data) override;

  // Emit a write barrier if:
  // A) emit_null_check is false
  // B) emit_null_check is true, and value is not null.
  void MaybeMarkGCCard(
      Register temp, Register card, Register object, Register value, bool emit_null_check);

  // Emit a write barrier unconditionally.
  void MarkGCCard(Register temp, Register card, Register object);

  // Crash if the card table is not valid. This check is only emitted for the CC GC. We assert
  // `(!clean || !self->is_gc_marking)`, since the card table should not be set to clean when the CC
  // GC is marking for eliminated write barriers.
  void CheckGCCardIsValid(Register temp, Register card, Register object);

  void GenerateMemoryBarrier(MemBarrierKind kind);

  Label* GetLabelOf(HBasicBlock* block) const {
    return CommonGetLabelOf<Label>(block_labels_, block);
  }

  void Initialize() override {
    block_labels_ = CommonInitializeLabels<Label>();
  }

  bool NeedsTwoRegisters(DataType::Type type) const override {
    return type == DataType::Type::kInt64;
  }

  bool ShouldSplitLongMoves() const override { return true; }

  Label* GetFrameEntryLabel() { return &frame_entry_label_; }

  void AddMethodAddressOffset(HX86ComputeBaseMethodAddress* method_base, int32_t offset) {
    method_address_offset_.Put(method_base->GetId(), offset);
  }

  int32_t GetMethodAddressOffset(HX86ComputeBaseMethodAddress* method_base) const {
    return method_address_offset_.Get(method_base->GetId());
  }

  int32_t ConstantAreaStart() const {
    return constant_area_start_;
  }

  Address LiteralDoubleAddress(double v, HX86ComputeBaseMethodAddress* method_base, Register reg);
  Address LiteralFloatAddress(float v, HX86ComputeBaseMethodAddress* method_base, Register reg);
  Address LiteralInt32Address(int32_t v, HX86ComputeBaseMethodAddress* method_base, Register reg);
  Address LiteralInt64Address(int64_t v, HX86ComputeBaseMethodAddress* method_base, Register reg);

  // Load a 32-bit value into a register in the most efficient manner.
  void Load32BitValue(Register dest, int32_t value);

  // Compare a register with a 32-bit value in the most efficient manner.
  void Compare32BitValue(Register dest, int32_t value);

  // Compare int values. Supports only register locations for `lhs`.
  void GenerateIntCompare(Location lhs, Location rhs);
  void GenerateIntCompare(Register lhs, Location rhs);

  // Construct address for array access.
  static Address ArrayAddress(Register obj,
                              Location index,
                              ScaleFactor scale,
                              uint32_t data_offset);

  Address LiteralCaseTable(HX86PackedSwitch* switch_instr, Register reg, Register value);

  void Finalize() override;

  // Fast path implementation of ReadBarrier::Barrier for a heap
  // reference field load when Baker's read barriers are used.
  void GenerateFieldLoadWithBakerReadBarrier(HInstruction* instruction,
                                             Location ref,
                                             Register obj,
                                             uint32_t offset,
                                             bool needs_null_check);
  // Fast path implementation of ReadBarrier::Barrier for a heap
  // reference array load when Baker's read barriers are used.
  void GenerateArrayLoadWithBakerReadBarrier(HInstruction* instruction,
                                             Location ref,
                                             Register obj,
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
  // requires a temporary register, which must be provided as a
  // non-null pointer (`temp`).
  void GenerateReferenceLoadWithBakerReadBarrier(HInstruction* instruction,
                                                 Location ref,
                                                 Register obj,
                                                 const Address& src,
                                                 bool needs_null_check,
                                                 bool always_update_field = false,
                                                 Register* temp = nullptr);

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
  // When `index` is provided (i.e. for array accesses), the offset
  // value passed to artReadBarrierSlow is adjusted to take `index`
  // into account.
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

  // Ensure that prior stores complete to memory before subsequent loads.
  // The locked add implementation will avoid serializing device memory, but will
  // touch (but not change) the top of the stack.
  // The 'non_temporal' parameter should be used to ensure ordering of non-temporal stores.
  void MemoryFence(bool non_temporal = false) {
    if (!non_temporal) {
      assembler_.lock()->addl(Address(ESP, 0), Immediate(0));
    } else {
      assembler_.mfence();
    }
  }

  void IncreaseFrame(size_t adjustment) override;
  void DecreaseFrame(size_t adjustment) override;

  void GenerateNop() override;
  void GenerateImplicitNullCheck(HNullCheck* instruction) override;
  void GenerateExplicitNullCheck(HNullCheck* instruction) override;

  void MaybeGenerateInlineCacheCheck(HInstruction* instruction, Register klass);
  void MaybeIncrementHotness(HSuspendCheck* suspend_check, bool is_frame_entry);

  // When we don't know the proper offset for the value, we use kPlaceholder32BitOffset.
  // The correct value will be inserted when processing Assembler fixups.
  static constexpr int32_t kPlaceholder32BitOffset = 256;

 private:
  struct X86PcRelativePatchInfo : PatchInfo<Label> {
    X86PcRelativePatchInfo(HX86ComputeBaseMethodAddress* address,
                           const DexFile* target_dex_file,
                           uint32_t target_index)
        : PatchInfo(target_dex_file, target_index),
          method_address(address) {}
    HX86ComputeBaseMethodAddress* method_address;
  };

  template <linker::LinkerPatch (*Factory)(size_t, const DexFile*, uint32_t, uint32_t)>
  void EmitPcRelativeLinkerPatches(const ArenaDeque<X86PcRelativePatchInfo>& infos,
                                   ArenaVector<linker::LinkerPatch>* linker_patches);

  Register GetInvokeExtraParameter(HInvoke* invoke, Register temp);
  Register GetInvokeStaticOrDirectExtraParameter(HInvokeStaticOrDirect* invoke, Register temp);

  // Labels for each block that will be compiled.
  Label* block_labels_;  // Indexed by block id.
  Label frame_entry_label_;
  LocationsBuilderX86 location_builder_;
  InstructionCodeGeneratorX86 instruction_visitor_;
  ParallelMoveResolverX86 move_resolver_;
  X86Assembler assembler_;

  // PC-relative method patch info for kBootImageLinkTimePcRelative.
  ArenaDeque<X86PcRelativePatchInfo> boot_image_method_patches_;
  // PC-relative method patch info for kAppImageRelRo.
  ArenaDeque<X86PcRelativePatchInfo> app_image_method_patches_;
  // PC-relative method patch info for kBssEntry.
  ArenaDeque<X86PcRelativePatchInfo> method_bss_entry_patches_;
  // PC-relative type patch info for kBootImageLinkTimePcRelative.
  ArenaDeque<X86PcRelativePatchInfo> boot_image_type_patches_;
  // PC-relative type patch info for kAppImageRelRo.
  ArenaDeque<X86PcRelativePatchInfo> app_image_type_patches_;
  // PC-relative type patch info for kBssEntry.
  ArenaDeque<X86PcRelativePatchInfo> type_bss_entry_patches_;
  // PC-relative public type patch info for kBssEntryPublic.
  ArenaDeque<X86PcRelativePatchInfo> public_type_bss_entry_patches_;
  // PC-relative package type patch info for kBssEntryPackage.
  ArenaDeque<X86PcRelativePatchInfo> package_type_bss_entry_patches_;
  // PC-relative String patch info for kBootImageLinkTimePcRelative.
  ArenaDeque<X86PcRelativePatchInfo> boot_image_string_patches_;
  // PC-relative String patch info for kBssEntry.
  ArenaDeque<X86PcRelativePatchInfo> string_bss_entry_patches_;
  // PC-relative method patch info for kBootImageLinkTimePcRelative+kCallCriticalNative.
  ArenaDeque<X86PcRelativePatchInfo> boot_image_jni_entrypoint_patches_;
  // PC-relative patch info for IntrinsicObjects for the boot image,
  // and for method/type/string patches for kBootImageRelRo otherwise.
  ArenaDeque<X86PcRelativePatchInfo> boot_image_other_patches_;

  // Patches for string root accesses in JIT compiled code.
  ArenaDeque<PatchInfo<Label>> jit_string_patches_;
  // Patches for class root accesses in JIT compiled code.
  ArenaDeque<PatchInfo<Label>> jit_class_patches_;

  // Offset to the start of the constant area in the assembled code.
  // Used for fixups to the constant area.
  int32_t constant_area_start_;

  // Fixups for jump tables that need to be patched after the constant table is generated.
  ArenaVector<JumpTableRIPFixup*> fixups_to_jump_tables_;

  // Maps a HX86ComputeBaseMethodAddress instruction id, to its offset in the
  // compiled code.
  ArenaSafeMap<uint32_t, int32_t> method_address_offset_;

  DISALLOW_COPY_AND_ASSIGN(CodeGeneratorX86);
};

}  // namespace x86
}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_CODE_GENERATOR_X86_H_
