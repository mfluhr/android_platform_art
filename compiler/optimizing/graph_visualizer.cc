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

#include "graph_visualizer.h"

#include <dlfcn.h>

#include <cctype>
#include <ios>
#include <sstream>

#include "android-base/stringprintf.h"
#include "art_method.h"
#include "art_method-inl.h"
#include "base/intrusive_forward_list.h"
#include "bounds_check_elimination.h"
#include "builder.h"
#include "code_generator.h"
#include "data_type-inl.h"
#include "dead_code_elimination.h"
#include "dex/descriptors_names.h"
#include "disassembler.h"
#include "inliner.h"
#include "licm.h"
#include "nodes.h"
#include "optimization.h"
#include "reference_type_propagation.h"
#include "register_allocator_linear_scan.h"
#include "scoped_thread_state_change-inl.h"
#include "ssa_liveness_analysis.h"
#include "utils/assembler.h"

namespace art HIDDEN {

// Unique pass-name to identify that the dump is for printing to log.
constexpr const char* kDebugDumpName = "debug";
constexpr const char* kDebugDumpGraphName = "debug_graph";

using android::base::StringPrintf;

static bool HasWhitespace(const char* str) {
  DCHECK(str != nullptr);
  while (str[0] != 0) {
    if (isspace(str[0])) {
      return true;
    }
    str++;
  }
  return false;
}

class StringList {
 public:
  enum Format {
    kArrayBrackets,
    kSetBrackets,
  };

  // Create an empty list
  explicit StringList(Format format = kArrayBrackets) : format_(format), is_empty_(true) {}

  // Construct StringList from a linked list. List element class T
  // must provide methods `GetNext` and `Dump`.
  template<class T>
  explicit StringList(T* first_entry, Format format = kArrayBrackets) : StringList(format) {
    for (T* current = first_entry; current != nullptr; current = current->GetNext()) {
      current->Dump(NewEntryStream());
    }
  }
  // Construct StringList from a list of elements. The value type must provide method `Dump`.
  template <typename Container>
  explicit StringList(const Container& list, Format format = kArrayBrackets) : StringList(format) {
    for (const typename Container::value_type& current : list) {
      current.Dump(NewEntryStream());
    }
  }

  std::ostream& NewEntryStream() {
    if (is_empty_) {
      is_empty_ = false;
    } else {
      sstream_ << ",";
    }
    return sstream_;
  }

 private:
  Format format_;
  bool is_empty_;
  std::ostringstream sstream_;

  friend std::ostream& operator<<(std::ostream& os, const StringList& list);
};

std::ostream& operator<<(std::ostream& os, const StringList& list) {
  switch (list.format_) {
    case StringList::kArrayBrackets: return os << "[" << list.sstream_.str() << "]";
    case StringList::kSetBrackets:   return os << "{" << list.sstream_.str() << "}";
  }
}

// On target: load `libart-disassembler` only when required (to save on memory).
// On host: `libart-disassembler` should be linked directly (either as a static or dynamic lib)
#ifdef ART_TARGET
using create_disasm_prototype = Disassembler*(InstructionSet, DisassemblerOptions*);
#endif

class HGraphVisualizerDisassembler {
 public:
  HGraphVisualizerDisassembler(InstructionSet instruction_set,
                               const uint8_t* base_address,
                               const uint8_t* end_address)
      : instruction_set_(instruction_set), disassembler_(nullptr) {
#ifdef ART_TARGET
    constexpr const char* libart_disassembler_so_name =
        kIsDebugBuild ? "libartd-disassembler.so" : "libart-disassembler.so";
    libart_disassembler_handle_ = dlopen(libart_disassembler_so_name, RTLD_NOW);
    if (libart_disassembler_handle_ == nullptr) {
      LOG(ERROR) << "Failed to dlopen " << libart_disassembler_so_name << ": " << dlerror();
      return;
    }
    constexpr const char* create_disassembler_symbol = "create_disassembler";
    create_disasm_prototype* create_disassembler = reinterpret_cast<create_disasm_prototype*>(
        dlsym(libart_disassembler_handle_, create_disassembler_symbol));
    if (create_disassembler == nullptr) {
      LOG(ERROR) << "Could not find " << create_disassembler_symbol << " entry in "
                 << libart_disassembler_so_name << ": " << dlerror();
      return;
    }
#endif
    // Reading the disassembly from 0x0 is easier, so we print relative
    // addresses. We will only disassemble the code once everything has
    // been generated, so we can read data in literal pools.
    disassembler_ = std::unique_ptr<Disassembler>(create_disassembler(
            instruction_set,
            new DisassemblerOptions(/* absolute_addresses= */ false,
                                    base_address,
                                    end_address,
                                    /* can_read_literals= */ true,
                                    Is64BitInstructionSet(instruction_set)
                                        ? &Thread::DumpThreadOffset<PointerSize::k64>
                                        : &Thread::DumpThreadOffset<PointerSize::k32>)));
  }

  ~HGraphVisualizerDisassembler() {
    // We need to call ~Disassembler() before we close the library.
    disassembler_.reset();
#ifdef ART_TARGET
    if (libart_disassembler_handle_ != nullptr) {
      dlclose(libart_disassembler_handle_);
    }
#endif
  }

  void Disassemble(std::ostream& output, size_t start, size_t end) const {
    if (disassembler_ == nullptr) {
      return;
    }

    const uint8_t* base = disassembler_->GetDisassemblerOptions()->base_address_;
    if (instruction_set_ == InstructionSet::kThumb2) {
      // ARM and Thumb-2 use the same disassembler. The bottom bit of the
      // address is used to distinguish between the two.
      base += 1;
    }
    disassembler_->Dump(output, base + start, base + end);
  }

 private:
  InstructionSet instruction_set_;
  std::unique_ptr<Disassembler> disassembler_;

#ifdef ART_TARGET
  void* libart_disassembler_handle_;
#endif
};


/**
 * HGraph visitor to generate a file suitable for the c1visualizer tool and IRHydra.
 */
class HGraphVisualizerPrinter final : public HGraphDelegateVisitor {
 public:
  HGraphVisualizerPrinter(HGraph* graph,
                          std::ostream& output,
                          const char* pass_name,
                          bool is_after_pass,
                          bool graph_in_bad_state,
                          const CodeGenerator* codegen,
                          const BlockNamer& namer,
                          const DisassemblyInformation* disasm_info = nullptr)
      : HGraphDelegateVisitor(graph),
        output_(output),
        pass_name_(pass_name),
        is_after_pass_(is_after_pass),
        graph_in_bad_state_(graph_in_bad_state),
        codegen_(codegen),
        disasm_info_(disasm_info),
        namer_(namer),
        disassembler_(disasm_info_ != nullptr
                      ? new HGraphVisualizerDisassembler(
                            codegen_->GetInstructionSet(),
                            codegen_->GetAssembler().CodeBufferBaseAddress(),
                            codegen_->GetAssembler().CodeBufferBaseAddress()
                                + codegen_->GetAssembler().CodeSize())
                      : nullptr),
        indent_(0) {}

  void Flush() {
    // We use "\n" instead of std::endl to avoid implicit flushing which
    // generates too many syscalls during debug-GC tests (b/27826765).
    output_ << std::flush;
  }

  void StartTag(const char* name) {
    AddIndent();
    output_ << "begin_" << name << "\n";
    indent_++;
  }

  void EndTag(const char* name) {
    indent_--;
    AddIndent();
    output_ << "end_" << name << "\n";
  }

  void PrintProperty(const char* name, HBasicBlock* blk) {
    AddIndent();
    output_ << name << " \"" << namer_.GetName(blk) << "\"\n";
  }

  void PrintProperty(const char* name, const char* property) {
    AddIndent();
    output_ << name << " \"" << property << "\"\n";
  }

  void PrintProperty(const char* name, const char* property, int id) {
    AddIndent();
    output_ << name << " \"" << property << id << "\"\n";
  }

  void PrintEmptyProperty(const char* name) {
    AddIndent();
    output_ << name << "\n";
  }

  void PrintTime(const char* name) {
    AddIndent();
    output_ << name << " " << time(nullptr) << "\n";
  }

  void PrintInt(const char* name, int value) {
    AddIndent();
    output_ << name << " " << value << "\n";
  }

  void AddIndent() {
    for (size_t i = 0; i < indent_; ++i) {
      output_ << "  ";
    }
  }

  void PrintPredecessors(HBasicBlock* block) {
    AddIndent();
    output_ << "predecessors";
    for (HBasicBlock* predecessor : block->GetPredecessors()) {
      output_ << " \"" << namer_.GetName(predecessor) << "\" ";
    }
    if (block->IsEntryBlock() && (disasm_info_ != nullptr)) {
      output_ << " \"" << kDisassemblyBlockFrameEntry << "\" ";
    }
    output_<< "\n";
  }

  void PrintSuccessors(HBasicBlock* block) {
    AddIndent();
    output_ << "successors";
    for (HBasicBlock* successor : block->GetNormalSuccessors()) {
      output_ << " \"" << namer_.GetName(successor) << "\" ";
    }
    output_<< "\n";
  }

  void PrintExceptionHandlers(HBasicBlock* block) {
    bool has_slow_paths = block->IsExitBlock() &&
                          (disasm_info_ != nullptr) &&
                          !disasm_info_->GetSlowPathIntervals().empty();
    if (IsDebugDump() && block->GetExceptionalSuccessors().empty() && !has_slow_paths) {
      return;
    }
    AddIndent();
    output_ << "xhandlers";
    for (HBasicBlock* handler : block->GetExceptionalSuccessors()) {
      output_ << " \"" << namer_.GetName(handler) << "\" ";
    }
    if (has_slow_paths) {
      output_ << " \"" << kDisassemblyBlockSlowPaths << "\" ";
    }
    output_<< "\n";
  }

  void DumpLocation(std::ostream& stream, const Location& location) {
    DCHECK(codegen_ != nullptr);
    if (location.IsRegister()) {
      codegen_->DumpCoreRegister(stream, location.reg());
    } else if (location.IsFpuRegister()) {
      codegen_->DumpFloatingPointRegister(stream, location.reg());
    } else if (location.IsConstant()) {
      stream << "#";
      HConstant* constant = location.GetConstant();
      if (constant->IsIntConstant()) {
        stream << constant->AsIntConstant()->GetValue();
      } else if (constant->IsLongConstant()) {
        stream << constant->AsLongConstant()->GetValue();
      } else if (constant->IsFloatConstant()) {
        stream << constant->AsFloatConstant()->GetValue();
      } else if (constant->IsDoubleConstant()) {
        stream << constant->AsDoubleConstant()->GetValue();
      } else if (constant->IsNullConstant()) {
        stream << "null";
      }
    } else if (location.IsInvalid()) {
      stream << "invalid";
    } else if (location.IsStackSlot()) {
      stream << location.GetStackIndex() << "(sp)";
    } else if (location.IsFpuRegisterPair()) {
      codegen_->DumpFloatingPointRegister(stream, location.low());
      stream << "|";
      codegen_->DumpFloatingPointRegister(stream, location.high());
    } else if (location.IsRegisterPair()) {
      codegen_->DumpCoreRegister(stream, location.low());
      stream << "|";
      codegen_->DumpCoreRegister(stream, location.high());
    } else if (location.IsUnallocated()) {
      stream << "unallocated";
    } else if (location.IsDoubleStackSlot()) {
      stream << "2x" << location.GetStackIndex() << "(sp)";
    } else {
      DCHECK(location.IsSIMDStackSlot());
      stream << "4x" << location.GetStackIndex() << "(sp)";
    }
  }

  std::ostream& StartAttributeStream(const char* name = nullptr) {
    if (name == nullptr) {
      output_ << " ";
    } else {
      DCHECK(!HasWhitespace(name)) << "Checker does not allow spaces in attributes";
      output_ << " " << name << ":";
    }
    return output_;
  }

  void VisitParallelMove(HParallelMove* instruction) override {
    StartAttributeStream("liveness") << instruction->GetLifetimePosition();
    StringList moves;
    for (size_t i = 0, e = instruction->NumMoves(); i < e; ++i) {
      MoveOperands* move = instruction->MoveOperandsAt(i);
      std::ostream& str = moves.NewEntryStream();
      DumpLocation(str, move->GetSource());
      str << "->";
      DumpLocation(str, move->GetDestination());
    }
    StartAttributeStream("moves") << moves;
  }

  void VisitParameterValue(HParameterValue* instruction) override {
    StartAttributeStream("is_this") << std::boolalpha << instruction->IsThis() << std::noboolalpha;
  }

  void VisitIntConstant(HIntConstant* instruction) override {
    StartAttributeStream() << instruction->GetValue();
  }

  void VisitLongConstant(HLongConstant* instruction) override {
    StartAttributeStream() << instruction->GetValue();
  }

  void VisitFloatConstant(HFloatConstant* instruction) override {
    StartAttributeStream() << instruction->GetValue();
  }

  void VisitDoubleConstant(HDoubleConstant* instruction) override {
    StartAttributeStream() << instruction->GetValue();
  }

  void VisitPhi(HPhi* phi) override {
    StartAttributeStream("reg") << phi->GetRegNumber();
    StartAttributeStream("is_catch_phi") << std::boolalpha << phi->IsCatchPhi() << std::noboolalpha;
    StartAttributeStream("is_live") << std::boolalpha << phi->IsLive() << std::noboolalpha;
  }

  void VisitMemoryBarrier(HMemoryBarrier* barrier) override {
    StartAttributeStream("kind") << barrier->GetBarrierKind();
  }

  void VisitMonitorOperation(HMonitorOperation* monitor) override {
    StartAttributeStream("kind") << (monitor->IsEnter() ? "enter" : "exit");
  }

  void VisitLoadClass(HLoadClass* load_class) override {
    StartAttributeStream("load_kind") << load_class->GetLoadKind();
    StartAttributeStream("in_image") << std::boolalpha << load_class->IsInImage();
    StartAttributeStream("class_name")
        << load_class->GetDexFile().PrettyType(load_class->GetTypeIndex());
    StartAttributeStream("gen_clinit_check")
        << std::boolalpha << load_class->MustGenerateClinitCheck() << std::noboolalpha;
    StartAttributeStream("needs_access_check") << std::boolalpha
        << load_class->NeedsAccessCheck() << std::noboolalpha;
  }

  void VisitLoadMethodHandle(HLoadMethodHandle* load_method_handle) override {
    StartAttributeStream("load_kind") << "RuntimeCall";
    StartAttributeStream("method_handle_index") << load_method_handle->GetMethodHandleIndex();
  }

  void VisitLoadMethodType(HLoadMethodType* load_method_type) override {
    StartAttributeStream("load_kind") << "RuntimeCall";
    const DexFile& dex_file = load_method_type->GetDexFile();
    if (dex_file.NumProtoIds() >= load_method_type->GetProtoIndex().index_) {
      const dex::ProtoId& proto_id = dex_file.GetProtoId(load_method_type->GetProtoIndex());
      StartAttributeStream("method_type") << dex_file.GetProtoSignature(proto_id);
    } else {
      StartAttributeStream("method_type")
          << "<<Unknown proto-idx: " << load_method_type->GetProtoIndex() << ">>";
    }
  }

  void VisitLoadString(HLoadString* load_string) override {
    StartAttributeStream("load_kind") << load_string->GetLoadKind();
  }

  void HandleTypeCheckInstruction(HTypeCheckInstruction* check) {
    StartAttributeStream("check_kind") << check->GetTypeCheckKind();
    StartAttributeStream("must_do_null_check") << std::boolalpha
        << check->MustDoNullCheck() << std::noboolalpha;
    if (check->GetTypeCheckKind() == TypeCheckKind::kBitstringCheck) {
      StartAttributeStream("path_to_root") << std::hex
          << "0x" << check->GetBitstringPathToRoot() << std::dec;
      StartAttributeStream("mask") << std::hex << "0x" << check->GetBitstringMask() << std::dec;
    }
  }

  void VisitCheckCast(HCheckCast* check_cast) override {
    HandleTypeCheckInstruction(check_cast);
  }

  void VisitInstanceOf(HInstanceOf* instance_of) override {
    HandleTypeCheckInstruction(instance_of);
  }

  void VisitArrayLength(HArrayLength* array_length) override {
    StartAttributeStream("is_string_length") << std::boolalpha
        << array_length->IsStringLength() << std::noboolalpha;
    if (array_length->IsEmittedAtUseSite()) {
      StartAttributeStream("emitted_at_use") << "true";
    }
  }

  void VisitBoundsCheck(HBoundsCheck* bounds_check) override {
    StartAttributeStream("is_string_char_at") << std::boolalpha
        << bounds_check->IsStringCharAt() << std::noboolalpha;
  }

  void VisitSuspendCheck(HSuspendCheck* suspend_check) override {
    StartAttributeStream("is_no_op")
        << std::boolalpha << suspend_check->IsNoOp() << std::noboolalpha;
  }

  void VisitArrayGet(HArrayGet* array_get) override {
    StartAttributeStream("is_string_char_at") << std::boolalpha
        << array_get->IsStringCharAt() << std::noboolalpha;
  }

  void VisitArraySet(HArraySet* array_set) override {
    StartAttributeStream("value_can_be_null")
        << std::boolalpha << array_set->GetValueCanBeNull() << std::noboolalpha;
    StartAttributeStream("needs_type_check")
        << std::boolalpha << array_set->NeedsTypeCheck() << std::noboolalpha;
    StartAttributeStream("static_type_of_array_is_object_array")
        << std::boolalpha << array_set->StaticTypeOfArrayIsObjectArray() << std::noboolalpha;
    StartAttributeStream("can_trigger_gc")
        << std::boolalpha << array_set->GetSideEffects().Includes(SideEffects::CanTriggerGC())
        << std::noboolalpha;
    StartAttributeStream("write_barrier_kind") << array_set->GetWriteBarrierKind();
  }

  void VisitNewInstance(HNewInstance* new_instance) override {
    StartAttributeStream("is_finalizable")
        << std::boolalpha << new_instance->IsFinalizable() << std::noboolalpha;
    StartAttributeStream("is_partial_materialization")
        << std::boolalpha << new_instance->IsPartialMaterialization() << std::noboolalpha;
  }

  void VisitCompare(HCompare* compare) override {
    StartAttributeStream("bias") << compare->GetBias();
    StartAttributeStream("comparison_type") << compare->GetComparisonType();
  }

  void VisitCondition(HCondition* condition) override {
    StartAttributeStream("bias") << condition->GetBias();
    StartAttributeStream("emitted_at_use_site")
        << std::boolalpha << condition->IsEmittedAtUseSite() << std::noboolalpha;
  }

  void VisitIf(HIf* if_instr) override {
    StartAttributeStream("true_count") << if_instr->GetTrueCount();
    StartAttributeStream("false_count") << if_instr->GetFalseCount();
  }

  void VisitInvoke(HInvoke* invoke) override {
    StartAttributeStream("dex_file_index") << invoke->GetMethodReference().index;
    ArtMethod* method = invoke->GetResolvedMethod();
    // We don't print signatures, which conflict with c1visualizer format.
    static constexpr bool kWithSignature = false;
    // Note that we can only use the graph's dex file for the unresolved case. The
    // other invokes might be coming from inlined methods.
    ScopedObjectAccess soa(Thread::Current());
    std::string method_name = (method == nullptr)
        ? invoke->GetMethodReference().PrettyMethod(kWithSignature)
        : method->PrettyMethod(kWithSignature);
    StartAttributeStream("method_name") << method_name;
    StartAttributeStream("always_throws") << std::boolalpha
                                          << invoke->AlwaysThrows()
                                          << std::noboolalpha;
    if (method != nullptr) {
      StartAttributeStream("method_index") << method->GetMethodIndex();
    }
    StartAttributeStream("intrinsic") << invoke->GetIntrinsic();
  }

  void VisitInvokeUnresolved(HInvokeUnresolved* invoke) override {
    VisitInvoke(invoke);
    StartAttributeStream("invoke_type") << invoke->GetInvokeType();
  }

  void VisitInvokeStaticOrDirect(HInvokeStaticOrDirect* invoke) override {
    VisitInvoke(invoke);
    StartAttributeStream("method_load_kind") << invoke->GetMethodLoadKind();
    if (invoke->IsStatic()) {
      StartAttributeStream("clinit_check") << invoke->GetClinitCheckRequirement();
    }
  }

  void VisitInvokeVirtual(HInvokeVirtual* invoke) override {
    VisitInvoke(invoke);
  }

  void VisitInvokePolymorphic(HInvokePolymorphic* invoke) override {
    VisitInvoke(invoke);
    StartAttributeStream("invoke_type") << "InvokePolymorphic";
  }

  void VisitInstanceFieldGet(HInstanceFieldGet* iget) override {
    StartAttributeStream("field_name") <<
        iget->GetFieldInfo().GetDexFile().PrettyField(iget->GetFieldInfo().GetFieldIndex(),
                                                      /* with type */ false);
    StartAttributeStream("field_type") << iget->GetFieldType();
  }

  void VisitInstanceFieldSet(HInstanceFieldSet* iset) override {
    StartAttributeStream("field_name") <<
        iset->GetFieldInfo().GetDexFile().PrettyField(iset->GetFieldInfo().GetFieldIndex(),
                                                      /* with type */ false);
    StartAttributeStream("field_type") << iset->GetFieldType();
    StartAttributeStream("write_barrier_kind") << iset->GetWriteBarrierKind();
    StartAttributeStream("value_can_be_null")
        << std::boolalpha << iset->GetValueCanBeNull() << std::noboolalpha;
  }

  void VisitStaticFieldGet(HStaticFieldGet* sget) override {
    StartAttributeStream("field_name") <<
        sget->GetFieldInfo().GetDexFile().PrettyField(sget->GetFieldInfo().GetFieldIndex(),
                                                      /* with type */ false);
    StartAttributeStream("field_type") << sget->GetFieldType();
  }

  void VisitStaticFieldSet(HStaticFieldSet* sset) override {
    StartAttributeStream("field_name") <<
        sset->GetFieldInfo().GetDexFile().PrettyField(sset->GetFieldInfo().GetFieldIndex(),
                                                      /* with type */ false);
    StartAttributeStream("field_type") << sset->GetFieldType();
    StartAttributeStream("write_barrier_kind") << sset->GetWriteBarrierKind();
    StartAttributeStream("value_can_be_null")
        << std::boolalpha << sset->GetValueCanBeNull() << std::noboolalpha;
  }

  void VisitUnresolvedInstanceFieldGet(HUnresolvedInstanceFieldGet* field_access) override {
    StartAttributeStream("field_type") << field_access->GetFieldType();
  }

  void VisitUnresolvedInstanceFieldSet(HUnresolvedInstanceFieldSet* field_access) override {
    StartAttributeStream("field_type") << field_access->GetFieldType();
  }

  void VisitUnresolvedStaticFieldGet(HUnresolvedStaticFieldGet* field_access) override {
    StartAttributeStream("field_type") << field_access->GetFieldType();
  }

  void VisitUnresolvedStaticFieldSet(HUnresolvedStaticFieldSet* field_access) override {
    StartAttributeStream("field_type") << field_access->GetFieldType();
  }

  void VisitTryBoundary(HTryBoundary* try_boundary) override {
    StartAttributeStream("kind") << (try_boundary->IsEntry() ? "entry" : "exit");
  }

  void VisitGoto(HGoto* instruction) override {
    StartAttributeStream("target") << namer_.GetName(instruction->GetBlock()->GetSingleSuccessor());
  }

  void VisitDeoptimize(HDeoptimize* deoptimize) override {
    StartAttributeStream("kind") << deoptimize->GetKind();
  }

  void VisitVecOperation(HVecOperation* vec_operation) override {
    StartAttributeStream("packed_type") << vec_operation->GetPackedType();
  }

  void VisitVecMemoryOperation(HVecMemoryOperation* vec_mem_operation) override {
    VisitVecOperation(vec_mem_operation);
    StartAttributeStream("alignment") << vec_mem_operation->GetAlignment().ToString();
  }

  void VisitVecHalvingAdd(HVecHalvingAdd* hadd) override {
    VisitVecBinaryOperation(hadd);
    StartAttributeStream("rounded") << std::boolalpha << hadd->IsRounded() << std::noboolalpha;
  }

  void VisitVecMultiplyAccumulate(HVecMultiplyAccumulate* instruction) override {
    VisitVecOperation(instruction);
    StartAttributeStream("kind") << instruction->GetOpKind();
  }

  void VisitVecDotProd(HVecDotProd* instruction) override {
    VisitVecOperation(instruction);
    DataType::Type arg_type = instruction->InputAt(1)->AsVecOperation()->GetPackedType();
    StartAttributeStream("type") << (instruction->IsZeroExtending() ?
                                    DataType::ToUnsigned(arg_type) :
                                    DataType::ToSigned(arg_type));
  }

  void VisitBitwiseNegatedRight(HBitwiseNegatedRight* instruction) override {
    StartAttributeStream("kind") << instruction->GetOpKind();
  }

#if defined(ART_ENABLE_CODEGEN_arm) || defined(ART_ENABLE_CODEGEN_arm64)
  void VisitMultiplyAccumulate(HMultiplyAccumulate* instruction) override {
    StartAttributeStream("kind") << instruction->GetOpKind();
  }

  void VisitDataProcWithShifterOp(HDataProcWithShifterOp* instruction) override {
    StartAttributeStream("kind") << instruction->GetInstrKind() << "+" << instruction->GetOpKind();
    if (HDataProcWithShifterOp::IsShiftOp(instruction->GetOpKind())) {
      StartAttributeStream("shift") << instruction->GetShiftAmount();
    }
  }
#endif

#if defined(ART_ENABLE_CODEGEN_riscv64)
  void VisitRiscv64ShiftAdd(HRiscv64ShiftAdd* instruction) override {
    StartAttributeStream("distance") << instruction->GetDistance();
  }
#endif

  bool IsPass(const char* name) {
    return strcmp(pass_name_, name) == 0;
  }

  bool IsDebugDump() {
    return IsPass(kDebugDumpGraphName) || IsPass(kDebugDumpName);
  }

  void PrintInstruction(HInstruction* instruction) {
    output_ << instruction->DebugName();
    HConstInputsRef inputs = instruction->GetInputs();
    if (!inputs.empty()) {
      StringList input_list;
      for (const HInstruction* input : inputs) {
        input_list.NewEntryStream() << DataType::TypeId(input->GetType()) << input->GetId();
      }
      StartAttributeStream() << input_list;
    }
    if (instruction->GetDexPc() != kNoDexPc) {
      StartAttributeStream("dex_pc") << instruction->GetDexPc();
    } else {
      StartAttributeStream("dex_pc") << "n/a";
    }
    HBasicBlock* block = instruction->GetBlock();
    StartAttributeStream("block") << namer_.GetName(block);

    instruction->Accept(this);
    if (instruction->HasEnvironment()) {
      StringList envs;
      for (HEnvironment* environment = instruction->GetEnvironment();
           environment != nullptr;
           environment = environment->GetParent()) {
        StringList vregs;
        for (size_t i = 0, e = environment->Size(); i < e; ++i) {
          HInstruction* insn = environment->GetInstructionAt(i);
          if (insn != nullptr) {
            vregs.NewEntryStream() << DataType::TypeId(insn->GetType()) << insn->GetId();
          } else {
            vregs.NewEntryStream() << "_";
          }
        }
        envs.NewEntryStream() << vregs;
      }
      StartAttributeStream("env") << envs;
    }
    if (IsPass(SsaLivenessAnalysis::kLivenessPassName)
        && is_after_pass_
        && instruction->GetLifetimePosition() != kNoLifetime) {
      StartAttributeStream("liveness") << instruction->GetLifetimePosition();
      if (instruction->HasLiveInterval()) {
        LiveInterval* interval = instruction->GetLiveInterval();
        StartAttributeStream("ranges")
            << StringList(interval->GetFirstRange(), StringList::kSetBrackets);
        StartAttributeStream("uses") << StringList(interval->GetUses());
        StartAttributeStream("env_uses") << StringList(interval->GetEnvironmentUses());
        StartAttributeStream("is_fixed") << interval->IsFixed();
        StartAttributeStream("is_split") << interval->IsSplit();
        StartAttributeStream("is_low") << interval->IsLowInterval();
        StartAttributeStream("is_high") << interval->IsHighInterval();
      }
    }

    if (IsPass(RegisterAllocator::kRegisterAllocatorPassName) && is_after_pass_) {
      StartAttributeStream("liveness") << instruction->GetLifetimePosition();
      LocationSummary* locations = instruction->GetLocations();
      if (locations != nullptr) {
        StringList input_list;
        for (size_t i = 0, e = locations->GetInputCount(); i < e; ++i) {
          DumpLocation(input_list.NewEntryStream(), locations->InAt(i));
        }
        std::ostream& attr = StartAttributeStream("locations");
        attr << input_list << "->";
        DumpLocation(attr, locations->Out());
      }
    }

    HLoopInformation* loop_info = (block != nullptr) ? block->GetLoopInformation() : nullptr;
    if (loop_info == nullptr) {
      StartAttributeStream("loop") << "none";
    } else {
      StartAttributeStream("loop") << namer_.GetName(loop_info->GetHeader());
      HLoopInformation* outer = loop_info->GetPreHeader()->GetLoopInformation();
      if (outer != nullptr) {
        StartAttributeStream("outer_loop") << namer_.GetName(outer->GetHeader());
      } else {
        StartAttributeStream("outer_loop") << "none";
      }
      StartAttributeStream("irreducible")
          << std::boolalpha << loop_info->IsIrreducible() << std::noboolalpha;
    }

    // For the builder and the inliner, we want to add extra information on HInstructions
    // that have reference types, and also HInstanceOf/HCheckcast.
    if ((IsPass(HGraphBuilder::kBuilderPassName)
        || IsPass(HInliner::kInlinerPassName)
        || IsDebugDump())
        && (instruction->GetType() == DataType::Type::kReference ||
            instruction->IsInstanceOf() ||
            instruction->IsCheckCast())) {
      ReferenceTypeInfo info = (instruction->GetType() == DataType::Type::kReference)
          ? instruction->IsLoadClass()
              ? instruction->AsLoadClass()->GetLoadedClassRTI()
              : instruction->GetReferenceTypeInfo()
          : instruction->IsInstanceOf()
              ? instruction->AsInstanceOf()->GetTargetClassRTI()
              : instruction->AsCheckCast()->GetTargetClassRTI();
      ScopedObjectAccess soa(Thread::Current());
      if (info.IsValid()) {
        StartAttributeStream("klass")
            << mirror::Class::PrettyDescriptor(info.GetTypeHandle().Get());
        if (instruction->GetType() == DataType::Type::kReference) {
          StartAttributeStream("can_be_null")
              << std::boolalpha << instruction->CanBeNull() << std::noboolalpha;
        }
        StartAttributeStream("exact") << std::boolalpha << info.IsExact() << std::noboolalpha;
      } else if (instruction->IsLoadClass() ||
                 instruction->IsInstanceOf() ||
                 instruction->IsCheckCast()) {
        StartAttributeStream("klass") << "unresolved";
      } else {
        StartAttributeStream("klass") << "invalid";
      }
    }
    if (disasm_info_ != nullptr) {
      DCHECK(disassembler_ != nullptr);
      // If the information is available, disassemble the code generated for
      // this instruction.
      auto it = disasm_info_->GetInstructionIntervals().find(instruction);
      if (it != disasm_info_->GetInstructionIntervals().end()
          && it->second.start != it->second.end) {
        output_ << "\n";
        disassembler_->Disassemble(output_, it->second.start, it->second.end);
      }
    }
  }

  void PrintInstructions(const HInstructionList& list) {
    for (HInstructionIterator it(list); !it.Done(); it.Advance()) {
      HInstruction* instruction = it.Current();
      int bci = 0;
      size_t num_uses = instruction->GetUses().SizeSlow();
      AddIndent();
      output_ << bci << " " << num_uses << " "
              << DataType::TypeId(instruction->GetType()) << instruction->GetId() << " ";
      PrintInstruction(instruction);
      output_ << " " << kEndInstructionMarker << "\n";
    }
  }

  void DumpStartOfDisassemblyBlock(const char* block_name,
                                   int predecessor_index,
                                   int successor_index) {
    StartTag("block");
    PrintProperty("name", block_name);
    PrintInt("from_bci", -1);
    PrintInt("to_bci", -1);
    if (predecessor_index != -1) {
      PrintProperty("predecessors", "B", predecessor_index);
    } else {
      PrintEmptyProperty("predecessors");
    }
    if (successor_index != -1) {
      PrintProperty("successors", "B", successor_index);
    } else {
      PrintEmptyProperty("successors");
    }
    PrintEmptyProperty("xhandlers");
    PrintEmptyProperty("flags");
    StartTag("states");
    StartTag("locals");
    PrintInt("size", 0);
    PrintProperty("method", "None");
    EndTag("locals");
    EndTag("states");
    StartTag("HIR");
  }

  void DumpEndOfDisassemblyBlock() {
    EndTag("HIR");
    EndTag("block");
  }

  void DumpDisassemblyBlockForFrameEntry() {
    DumpStartOfDisassemblyBlock(kDisassemblyBlockFrameEntry,
                                -1,
                                GetGraph()->GetEntryBlock()->GetBlockId());
    output_ << "    0 0 disasm " << kDisassemblyBlockFrameEntry << " ";
    GeneratedCodeInterval frame_entry = disasm_info_->GetFrameEntryInterval();
    if (frame_entry.start != frame_entry.end) {
      output_ << "\n";
      disassembler_->Disassemble(output_, frame_entry.start, frame_entry.end);
    }
    output_ << kEndInstructionMarker << "\n";
    DumpEndOfDisassemblyBlock();
  }

  void DumpDisassemblyBlockForSlowPaths() {
    if (disasm_info_->GetSlowPathIntervals().empty()) {
      return;
    }
    // If the graph has an exit block we attach the block for the slow paths
    // after it. Else we just add the block to the graph without linking it to
    // any other.
    DumpStartOfDisassemblyBlock(
        kDisassemblyBlockSlowPaths,
        GetGraph()->HasExitBlock() ? GetGraph()->GetExitBlock()->GetBlockId() : -1,
        -1);
    for (SlowPathCodeInfo info : disasm_info_->GetSlowPathIntervals()) {
      output_ << "    0 0 disasm " << info.slow_path->GetDescription() << "\n";
      disassembler_->Disassemble(output_, info.code_interval.start, info.code_interval.end);
      output_ << kEndInstructionMarker << "\n";
    }
    DumpEndOfDisassemblyBlock();
  }

  void Run() {
    StartTag("cfg");
    std::ostringstream oss;
    oss << pass_name_;
    if (!IsDebugDump()) {
      oss << " (" << (GetGraph()->IsCompilingBaseline() ? "baseline " : "")
          << (is_after_pass_ ? "after" : "before")
          << (graph_in_bad_state_ ? ", bad_state" : "") << ")";
    }
    PrintProperty("name", oss.str().c_str());
    if (disasm_info_ != nullptr) {
      DumpDisassemblyBlockForFrameEntry();
    }
    VisitInsertionOrder();
    if (disasm_info_ != nullptr) {
      DumpDisassemblyBlockForSlowPaths();
    }
    EndTag("cfg");
    Flush();
  }

  void Run(HInstruction* instruction) {
    output_ << DataType::TypeId(instruction->GetType()) << instruction->GetId() << " ";
    PrintInstruction(instruction);
    Flush();
  }

  void VisitBasicBlock(HBasicBlock* block) override {
    StartTag("block");
    PrintProperty("name", block);
    if (block->GetLifetimeStart() != kNoLifetime) {
      // Piggy back on these fields to show the lifetime of the block.
      PrintInt("from_bci", block->GetLifetimeStart());
      PrintInt("to_bci", block->GetLifetimeEnd());
    } else if (!IsDebugDump()) {
      // Don't print useless information to logcat.
      PrintInt("from_bci", -1);
      PrintInt("to_bci", -1);
    }
    PrintPredecessors(block);
    PrintSuccessors(block);
    PrintExceptionHandlers(block);

    if (block->IsCatchBlock()) {
      PrintProperty("flags", "catch_block");
    } else if (block->IsTryBlock()) {
      std::stringstream flags_properties;
      flags_properties << "try_start "
                       << namer_.GetName(block->GetTryCatchInformation()->GetTryEntry().GetBlock());
      PrintProperty("flags", flags_properties.str().c_str());
    } else if (!IsDebugDump()) {
      // Don't print useless information to logcat
      PrintEmptyProperty("flags");
    }

    if (block->GetDominator() != nullptr) {
      PrintProperty("dominator", block->GetDominator());
    }

    if (!IsDebugDump() || !block->GetPhis().IsEmpty()) {
      StartTag("states");
      StartTag("locals");
      PrintInt("size", 0);
      PrintProperty("method", "None");
      for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
        AddIndent();
        HInstruction* instruction = it.Current();
        output_ << instruction->GetId() << " " << DataType::TypeId(instruction->GetType())
                << instruction->GetId() << "[ ";
        for (const HInstruction* input : instruction->GetInputs()) {
          output_ << input->GetId() << " ";
        }
        output_ << "]\n";
      }
      EndTag("locals");
      EndTag("states");
    }

    StartTag("HIR");
    PrintInstructions(block->GetPhis());
    PrintInstructions(block->GetInstructions());
    EndTag("HIR");
    EndTag("block");
  }

  static constexpr const char* const kEndInstructionMarker = "<|@";
  static constexpr const char* const kDisassemblyBlockFrameEntry = "FrameEntry";
  static constexpr const char* const kDisassemblyBlockSlowPaths = "SlowPaths";

 private:
  std::ostream& output_;
  const char* pass_name_;
  const bool is_after_pass_;
  const bool graph_in_bad_state_;
  const CodeGenerator* codegen_;
  const DisassemblyInformation* disasm_info_;
  const BlockNamer& namer_;
  std::unique_ptr<HGraphVisualizerDisassembler> disassembler_;
  size_t indent_;

  DISALLOW_COPY_AND_ASSIGN(HGraphVisualizerPrinter);
};

std::ostream& HGraphVisualizer::OptionalDefaultNamer::PrintName(std::ostream& os,
                                                                HBasicBlock* blk) const {
  if (namer_) {
    return namer_->get().PrintName(os, blk);
  } else {
    return BlockNamer::PrintName(os, blk);
  }
}

HGraphVisualizer::HGraphVisualizer(std::ostream* output,
                                   HGraph* graph,
                                   const CodeGenerator* codegen,
                                   std::optional<std::reference_wrapper<const BlockNamer>> namer)
    : output_(output), graph_(graph), codegen_(codegen), namer_(namer) {}

void HGraphVisualizer::PrintHeader(const char* method_name) const {
  DCHECK(output_ != nullptr);
  HGraphVisualizerPrinter printer(graph_, *output_, "", true, false, codegen_, namer_);
  printer.StartTag("compilation");
  printer.PrintProperty("name", method_name);
  printer.PrintProperty("method", method_name);
  printer.PrintTime("date");
  printer.EndTag("compilation");
  printer.Flush();
}

std::string HGraphVisualizer::InsertMetaDataAsCompilationBlock(const std::string& meta_data) {
  std::string time_str = std::to_string(time(nullptr));
  std::string quoted_meta_data = "\"" + meta_data + "\"";
  return StringPrintf("begin_compilation\n"
                      "  name %s\n"
                      "  method %s\n"
                      "  date %s\n"
                      "end_compilation\n",
                      quoted_meta_data.c_str(),
                      quoted_meta_data.c_str(),
                      time_str.c_str());
}

void HGraphVisualizer::DumpGraphDebug() const {
  DumpGraph(/* pass_name= */ kDebugDumpGraphName,
            /* is_after_pass= */ false,
            /* graph_in_bad_state= */ true);
}

void HGraphVisualizer::DumpGraph(const char* pass_name,
                                 bool is_after_pass,
                                 bool graph_in_bad_state) const {
  DCHECK(output_ != nullptr);
  if (!graph_->GetBlocks().empty()) {
    HGraphVisualizerPrinter printer(graph_,
                                    *output_,
                                    pass_name,
                                    is_after_pass,
                                    graph_in_bad_state,
                                    codegen_,
                                    namer_);
    printer.Run();
  }
}

void HGraphVisualizer::DumpGraphWithDisassembly() const {
  DCHECK(output_ != nullptr);
  if (!graph_->GetBlocks().empty()) {
    HGraphVisualizerPrinter printer(graph_,
                                    *output_,
                                    "disassembly",
                                    /* is_after_pass= */ true,
                                    /* graph_in_bad_state= */ false,
                                    codegen_,
                                    namer_,
                                    codegen_->GetDisassemblyInformation());
    printer.Run();
  }
}

void HGraphVisualizer::DumpInstruction(std::ostream* output,
                                       HGraph* graph,
                                       HInstruction* instruction) {
  BlockNamer namer;
  HGraphVisualizerPrinter printer(graph,
                                  *output,
                                  /* pass_name= */ kDebugDumpName,
                                  /* is_after_pass= */ false,
                                  /* graph_in_bad_state= */ false,
                                  /* codegen= */ nullptr,
                                  /* namer= */ namer);
  printer.Run(instruction);
}

}  // namespace art
