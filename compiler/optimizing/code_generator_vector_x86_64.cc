/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "mirror/array-inl.h"
#include "mirror/string.h"

namespace art HIDDEN {
namespace x86_64 {

// NOLINT on __ macro to suppress wrong warning/fix (misc-macro-parentheses) from clang-tidy.
#define __ down_cast<X86_64Assembler*>(GetAssembler())->  // NOLINT

void LocationsBuilderX86_64::VisitVecReplicateScalar(HVecReplicateScalar* instruction) {
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(instruction);
  HInstruction* input = instruction->InputAt(0);
  bool is_zero = IsZeroBitPattern(input);
  switch (instruction->GetPackedType()) {
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      locations->SetInAt(0, is_zero ? Location::ConstantLocation(input)
                                    : Location::RequiresRegister());
      locations->SetOut(Location::RequiresFpuRegister());
      break;
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64:
      locations->SetInAt(0, is_zero ? Location::ConstantLocation(input)
                                    : Location::RequiresFpuRegister());
      locations->SetOut(is_zero ? Location::RequiresFpuRegister()
                                : Location::SameAsFirstInput());
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
}

void InstructionCodeGeneratorX86_64::VisitVecReplicateScalar(HVecReplicateScalar* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  XmmRegister dst = locations->Out().AsFpuRegister<XmmRegister>();

  bool cpu_has_avx = CpuHasAvxFeatureFlag();
  // Shorthand for any type of zero.
  if (IsZeroBitPattern(instruction->InputAt(0))) {
    cpu_has_avx ? __ vxorps(dst, dst, dst) : __ xorps(dst, dst);
    return;
  }

  switch (instruction->GetPackedType()) {
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
      DCHECK_EQ(16u, instruction->GetVectorLength());
      __ movd(dst, locations->InAt(0).AsRegister<CpuRegister>());
      __ punpcklbw(dst, dst);
      __ punpcklwd(dst, dst);
      __ pshufd(dst, dst, Immediate(0));
      break;
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
      DCHECK_EQ(8u, instruction->GetVectorLength());
      __ movd(dst, locations->InAt(0).AsRegister<CpuRegister>());
      __ punpcklwd(dst, dst);
      __ pshufd(dst, dst, Immediate(0));
      break;
    case DataType::Type::kInt32:
      DCHECK_EQ(4u, instruction->GetVectorLength());
      __ movd(dst, locations->InAt(0).AsRegister<CpuRegister>());
      __ pshufd(dst, dst, Immediate(0));
      break;
    case DataType::Type::kInt64:
      DCHECK_EQ(2u, instruction->GetVectorLength());
      __ movq(dst, locations->InAt(0).AsRegister<CpuRegister>());
      __ punpcklqdq(dst, dst);
      break;
    case DataType::Type::kFloat32:
      DCHECK_EQ(4u, instruction->GetVectorLength());
      DCHECK(locations->InAt(0).Equals(locations->Out()));
      __ shufps(dst, dst, Immediate(0));
      break;
    case DataType::Type::kFloat64:
      DCHECK_EQ(2u, instruction->GetVectorLength());
      DCHECK(locations->InAt(0).Equals(locations->Out()));
      __ shufpd(dst, dst, Immediate(0));
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitVecExtractScalar(HVecExtractScalar* instruction) {
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(instruction);
  switch (instruction->GetPackedType()) {
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresRegister());
      break;
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetOut(Location::SameAsFirstInput());
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
}

void InstructionCodeGeneratorX86_64::VisitVecExtractScalar(HVecExtractScalar* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  XmmRegister src = locations->InAt(0).AsFpuRegister<XmmRegister>();
  switch (instruction->GetPackedType()) {
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:  // TODO: up to here, and?
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
    case DataType::Type::kInt32:
      DCHECK_EQ(4u, instruction->GetVectorLength());
      __ movd(locations->Out().AsRegister<CpuRegister>(), src);
      break;
    case DataType::Type::kInt64:
      DCHECK_EQ(2u, instruction->GetVectorLength());
      __ movq(locations->Out().AsRegister<CpuRegister>(), src);
      break;
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64:
      DCHECK_LE(2u, instruction->GetVectorLength());
      DCHECK_LE(instruction->GetVectorLength(), 4u);
      DCHECK(locations->InAt(0).Equals(locations->Out()));  // no code required
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
}

// Helper to set up locations for vector unary operations.
static void CreateVecUnOpLocations(ArenaAllocator* allocator, HVecUnaryOperation* instruction) {
  LocationSummary* locations = new (allocator) LocationSummary(instruction);
  switch (instruction->GetPackedType()) {
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresFpuRegister());
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitVecReduce(HVecReduce* instruction) {
  CreateVecUnOpLocations(GetGraph()->GetAllocator(), instruction);
  // Long reduction or min/max require a temporary.
  if (instruction->GetPackedType() == DataType::Type::kInt64 ||
      instruction->GetReductionKind() == HVecReduce::kMin ||
      instruction->GetReductionKind() == HVecReduce::kMax) {
    instruction->GetLocations()->AddTemp(Location::RequiresFpuRegister());
  }
}

void InstructionCodeGeneratorX86_64::VisitVecReduce(HVecReduce* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  XmmRegister src = locations->InAt(0).AsFpuRegister<XmmRegister>();
  XmmRegister dst = locations->Out().AsFpuRegister<XmmRegister>();
  switch (instruction->GetPackedType()) {
    case DataType::Type::kInt32:
      DCHECK_EQ(4u, instruction->GetVectorLength());
      switch (instruction->GetReductionKind()) {
        case HVecReduce::kSum:
          __ movaps(dst, src);
          __ phaddd(dst, dst);
          __ phaddd(dst, dst);
          break;
        case HVecReduce::kMin:
        case HVecReduce::kMax:
          // Historical note: We've had a broken implementation here. b/117863065
          // Do not draw on the old code if we ever want to bring MIN/MAX reduction back.
          LOG(FATAL) << "Unsupported reduction type.";
      }
      break;
    case DataType::Type::kInt64: {
      DCHECK_EQ(2u, instruction->GetVectorLength());
      XmmRegister tmp = locations->GetTemp(0).AsFpuRegister<XmmRegister>();
      switch (instruction->GetReductionKind()) {
        case HVecReduce::kSum:
          __ movaps(tmp, src);
          __ movaps(dst, src);
          __ punpckhqdq(tmp, tmp);
          __ paddq(dst, tmp);
          break;
        case HVecReduce::kMin:
        case HVecReduce::kMax:
          LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      }
      break;
    }
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitVecCnv(HVecCnv* instruction) {
  CreateVecUnOpLocations(GetGraph()->GetAllocator(), instruction);
}

void InstructionCodeGeneratorX86_64::VisitVecCnv(HVecCnv* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  XmmRegister src = locations->InAt(0).AsFpuRegister<XmmRegister>();
  XmmRegister dst = locations->Out().AsFpuRegister<XmmRegister>();
  DataType::Type from = instruction->GetInputType();
  DataType::Type to = instruction->GetResultType();
  if (from == DataType::Type::kInt32 && to == DataType::Type::kFloat32) {
    DCHECK_EQ(4u, instruction->GetVectorLength());
    __ cvtdq2ps(dst, src);
  } else {
    LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
  }
}

void LocationsBuilderX86_64::VisitVecNeg(HVecNeg* instruction) {
  CreateVecUnOpLocations(GetGraph()->GetAllocator(), instruction);
}

void InstructionCodeGeneratorX86_64::VisitVecNeg(HVecNeg* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  XmmRegister src = locations->InAt(0).AsFpuRegister<XmmRegister>();
  XmmRegister dst = locations->Out().AsFpuRegister<XmmRegister>();
  switch (instruction->GetPackedType()) {
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
      DCHECK_EQ(16u, instruction->GetVectorLength());
      __ pxor(dst, dst);
      __ psubb(dst, src);
      break;
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
      DCHECK_EQ(8u, instruction->GetVectorLength());
      __ pxor(dst, dst);
      __ psubw(dst, src);
      break;
    case DataType::Type::kInt32:
      DCHECK_EQ(4u, instruction->GetVectorLength());
      __ pxor(dst, dst);
      __ psubd(dst, src);
      break;
    case DataType::Type::kInt64:
      DCHECK_EQ(2u, instruction->GetVectorLength());
      __ pxor(dst, dst);
      __ psubq(dst, src);
      break;
    case DataType::Type::kFloat32:
      DCHECK_EQ(4u, instruction->GetVectorLength());
      __ xorps(dst, dst);
      __ subps(dst, src);
      break;
    case DataType::Type::kFloat64:
      DCHECK_EQ(2u, instruction->GetVectorLength());
      __ xorpd(dst, dst);
      __ subpd(dst, src);
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitVecAbs(HVecAbs* instruction) {
  CreateVecUnOpLocations(GetGraph()->GetAllocator(), instruction);
  // Integral-abs requires a temporary for the comparison.
  if (instruction->GetPackedType() == DataType::Type::kInt32) {
    instruction->GetLocations()->AddTemp(Location::RequiresFpuRegister());
  }
}

void InstructionCodeGeneratorX86_64::VisitVecAbs(HVecAbs* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  XmmRegister src = locations->InAt(0).AsFpuRegister<XmmRegister>();
  XmmRegister dst = locations->Out().AsFpuRegister<XmmRegister>();
  switch (instruction->GetPackedType()) {
    case DataType::Type::kInt32: {
      DCHECK_EQ(4u, instruction->GetVectorLength());
      XmmRegister tmp = locations->GetTemp(0).AsFpuRegister<XmmRegister>();
      __ movaps(dst, src);
      __ pxor(tmp, tmp);
      __ pcmpgtd(tmp, dst);
      __ pxor(dst, tmp);
      __ psubd(dst, tmp);
      break;
    }
    case DataType::Type::kFloat32:
      DCHECK_EQ(4u, instruction->GetVectorLength());
      __ pcmpeqb(dst, dst);  // all ones
      __ psrld(dst, Immediate(1));
      __ andps(dst, src);
      break;
    case DataType::Type::kFloat64:
      DCHECK_EQ(2u, instruction->GetVectorLength());
      __ pcmpeqb(dst, dst);  // all ones
      __ psrlq(dst, Immediate(1));
      __ andpd(dst, src);
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitVecNot(HVecNot* instruction) {
  CreateVecUnOpLocations(GetGraph()->GetAllocator(), instruction);
  // Boolean-not requires a temporary to construct the 16 x one.
  if (instruction->GetPackedType() == DataType::Type::kBool) {
    instruction->GetLocations()->AddTemp(Location::RequiresFpuRegister());
  }
}

void InstructionCodeGeneratorX86_64::VisitVecNot(HVecNot* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  XmmRegister src = locations->InAt(0).AsFpuRegister<XmmRegister>();
  XmmRegister dst = locations->Out().AsFpuRegister<XmmRegister>();
  switch (instruction->GetPackedType()) {
    case DataType::Type::kBool: {  // special case boolean-not
      DCHECK_EQ(16u, instruction->GetVectorLength());
      XmmRegister tmp = locations->GetTemp(0).AsFpuRegister<XmmRegister>();
      __ pxor(dst, dst);
      __ pcmpeqb(tmp, tmp);  // all ones
      __ psubb(dst, tmp);  // 16 x one
      __ pxor(dst, src);
      break;
    }
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      DCHECK_LE(2u, instruction->GetVectorLength());
      DCHECK_LE(instruction->GetVectorLength(), 16u);
      __ pcmpeqb(dst, dst);  // all ones
      __ pxor(dst, src);
      break;
    case DataType::Type::kFloat32:
      DCHECK_EQ(4u, instruction->GetVectorLength());
      __ pcmpeqb(dst, dst);  // all ones
      __ xorps(dst, src);
      break;
    case DataType::Type::kFloat64:
      DCHECK_EQ(2u, instruction->GetVectorLength());
      __ pcmpeqb(dst, dst);  // all ones
      __ xorpd(dst, src);
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
}

// Helper to set up locations for vector binary operations.
static void CreateVecBinOpLocations(ArenaAllocator* allocator, HVecBinaryOperation* instruction) {
  LocationSummary* locations = new (allocator) LocationSummary(instruction);
  switch (instruction->GetPackedType()) {
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      locations->SetOut(Location::SameAsFirstInput());
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
}

static void CreateVecTerOpLocations(ArenaAllocator* allocator, HVecOperation* instruction) {
  LocationSummary* locations = new (allocator) LocationSummary(instruction);
  switch (instruction->GetPackedType()) {
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresFpuRegister());
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type";
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitVecAdd(HVecAdd* instruction) {
  if (CpuHasAvxFeatureFlag()) {
    CreateVecTerOpLocations(GetGraph()->GetAllocator(), instruction);
  } else {
    CreateVecBinOpLocations(GetGraph()->GetAllocator(), instruction);
  }
}

void InstructionCodeGeneratorX86_64::VisitVecAdd(HVecAdd* instruction) {
  bool cpu_has_avx = CpuHasAvxFeatureFlag();
  LocationSummary* locations = instruction->GetLocations();
  XmmRegister src = locations->InAt(1).AsFpuRegister<XmmRegister>();
  XmmRegister other_src = locations->InAt(0).AsFpuRegister<XmmRegister>();
  XmmRegister dst = locations->Out().AsFpuRegister<XmmRegister>();
  DCHECK(cpu_has_avx || other_src == dst);
  switch (instruction->GetPackedType()) {
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
      DCHECK_EQ(16u, instruction->GetVectorLength());
      cpu_has_avx ? __ vpaddb(dst, other_src, src) : __ paddb(dst, src);
      break;
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
      DCHECK_EQ(8u, instruction->GetVectorLength());
      cpu_has_avx ? __ vpaddw(dst, other_src, src) : __ paddw(dst, src);
      break;
    case DataType::Type::kInt32:
      DCHECK_EQ(4u, instruction->GetVectorLength());
      cpu_has_avx ? __ vpaddd(dst, other_src, src) : __ paddd(dst, src);
      break;
    case DataType::Type::kInt64:
      DCHECK_EQ(2u, instruction->GetVectorLength());
      cpu_has_avx ? __ vpaddq(dst, other_src, src) : __ paddq(dst, src);
      break;
    case DataType::Type::kFloat32:
      DCHECK_EQ(4u, instruction->GetVectorLength());
      cpu_has_avx ? __ vaddps(dst, other_src, src) : __ addps(dst, src);
      break;
    case DataType::Type::kFloat64:
      DCHECK_EQ(2u, instruction->GetVectorLength());
      cpu_has_avx ? __ vaddpd(dst, other_src, src) : __ addpd(dst, src);
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitVecSaturationAdd(HVecSaturationAdd* instruction) {
  CreateVecBinOpLocations(GetGraph()->GetAllocator(), instruction);
}

void InstructionCodeGeneratorX86_64::VisitVecSaturationAdd(HVecSaturationAdd* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  DCHECK(locations->InAt(0).Equals(locations->Out()));
  XmmRegister src = locations->InAt(1).AsFpuRegister<XmmRegister>();
  XmmRegister dst = locations->Out().AsFpuRegister<XmmRegister>();
  switch (instruction->GetPackedType()) {
    case DataType::Type::kUint8:
      DCHECK_EQ(16u, instruction->GetVectorLength());
      __ paddusb(dst, src);
      break;
    case DataType::Type::kInt8:
      DCHECK_EQ(16u, instruction->GetVectorLength());
      __ paddsb(dst, src);
      break;
    case DataType::Type::kUint16:
      DCHECK_EQ(8u, instruction->GetVectorLength());
      __ paddusw(dst, src);
      break;
    case DataType::Type::kInt16:
      DCHECK_EQ(8u, instruction->GetVectorLength());
      __ paddsw(dst, src);
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitVecHalvingAdd(HVecHalvingAdd* instruction) {
  CreateVecBinOpLocations(GetGraph()->GetAllocator(), instruction);
}

void InstructionCodeGeneratorX86_64::VisitVecHalvingAdd(HVecHalvingAdd* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  DCHECK(locations->InAt(0).Equals(locations->Out()));
  XmmRegister src = locations->InAt(1).AsFpuRegister<XmmRegister>();
  XmmRegister dst = locations->Out().AsFpuRegister<XmmRegister>();

  DCHECK(instruction->IsRounded());

  switch (instruction->GetPackedType()) {
    case DataType::Type::kUint8:
      DCHECK_EQ(16u, instruction->GetVectorLength());
      __ pavgb(dst, src);
      break;
    case DataType::Type::kUint16:
      DCHECK_EQ(8u, instruction->GetVectorLength());
      __ pavgw(dst, src);
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitVecSub(HVecSub* instruction) {
  if (CpuHasAvxFeatureFlag()) {
    CreateVecTerOpLocations(GetGraph()->GetAllocator(), instruction);
  } else {
    CreateVecBinOpLocations(GetGraph()->GetAllocator(), instruction);
  }
}

void InstructionCodeGeneratorX86_64::VisitVecSub(HVecSub* instruction) {
  bool cpu_has_avx = CpuHasAvxFeatureFlag();
  LocationSummary* locations = instruction->GetLocations();
  XmmRegister src = locations->InAt(1).AsFpuRegister<XmmRegister>();
  XmmRegister other_src = locations->InAt(0).AsFpuRegister<XmmRegister>();
  XmmRegister dst = locations->Out().AsFpuRegister<XmmRegister>();
  DCHECK(cpu_has_avx || other_src == dst);
  switch (instruction->GetPackedType()) {
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
      DCHECK_EQ(16u, instruction->GetVectorLength());
      cpu_has_avx ? __ vpsubb(dst, other_src, src) : __ psubb(dst, src);
      break;
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
      DCHECK_EQ(8u, instruction->GetVectorLength());
      cpu_has_avx ? __ vpsubw(dst, other_src, src) : __ psubw(dst, src);
      break;
    case DataType::Type::kInt32:
      DCHECK_EQ(4u, instruction->GetVectorLength());
      cpu_has_avx ? __ vpsubd(dst, other_src, src) : __ psubd(dst, src);
      break;
    case DataType::Type::kInt64:
      DCHECK_EQ(2u, instruction->GetVectorLength());
      cpu_has_avx ? __ vpsubq(dst, other_src, src) : __ psubq(dst, src);
      break;
    case DataType::Type::kFloat32:
      DCHECK_EQ(4u, instruction->GetVectorLength());
      cpu_has_avx ? __ vsubps(dst, other_src, src) : __ subps(dst, src);
      break;
    case DataType::Type::kFloat64:
      DCHECK_EQ(2u, instruction->GetVectorLength());
      cpu_has_avx ? __ vsubpd(dst, other_src, src) : __ subpd(dst, src);
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitVecSaturationSub(HVecSaturationSub* instruction) {
  CreateVecBinOpLocations(GetGraph()->GetAllocator(), instruction);
}

void InstructionCodeGeneratorX86_64::VisitVecSaturationSub(HVecSaturationSub* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  DCHECK(locations->InAt(0).Equals(locations->Out()));
  XmmRegister src = locations->InAt(1).AsFpuRegister<XmmRegister>();
  XmmRegister dst = locations->Out().AsFpuRegister<XmmRegister>();
  switch (instruction->GetPackedType()) {
    case DataType::Type::kUint8:
      DCHECK_EQ(16u, instruction->GetVectorLength());
      __ psubusb(dst, src);
      break;
    case DataType::Type::kInt8:
      DCHECK_EQ(16u, instruction->GetVectorLength());
      __ psubsb(dst, src);
      break;
    case DataType::Type::kUint16:
      DCHECK_EQ(8u, instruction->GetVectorLength());
      __ psubusw(dst, src);
      break;
    case DataType::Type::kInt16:
      DCHECK_EQ(8u, instruction->GetVectorLength());
      __ psubsw(dst, src);
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitVecMul(HVecMul* instruction) {
  if (CpuHasAvxFeatureFlag()) {
    CreateVecTerOpLocations(GetGraph()->GetAllocator(), instruction);
  } else {
    CreateVecBinOpLocations(GetGraph()->GetAllocator(), instruction);
  }
}

void InstructionCodeGeneratorX86_64::VisitVecMul(HVecMul* instruction) {
  bool cpu_has_avx = CpuHasAvxFeatureFlag();
  LocationSummary* locations = instruction->GetLocations();
  XmmRegister src = locations->InAt(1).AsFpuRegister<XmmRegister>();
  XmmRegister other_src = locations->InAt(0).AsFpuRegister<XmmRegister>();
  XmmRegister dst = locations->Out().AsFpuRegister<XmmRegister>();
  DCHECK(cpu_has_avx || other_src == dst);
  switch (instruction->GetPackedType()) {
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
      DCHECK_EQ(8u, instruction->GetVectorLength());
      cpu_has_avx ? __ vpmullw(dst, other_src, src) : __ pmullw(dst, src);
      break;
    case DataType::Type::kInt32:
      DCHECK_EQ(4u, instruction->GetVectorLength());
      cpu_has_avx ? __ vpmulld(dst, other_src, src): __ pmulld(dst, src);
      break;
    case DataType::Type::kFloat32:
      DCHECK_EQ(4u, instruction->GetVectorLength());
      cpu_has_avx ? __ vmulps(dst, other_src, src) : __ mulps(dst, src);
      break;
    case DataType::Type::kFloat64:
      DCHECK_EQ(2u, instruction->GetVectorLength());
      cpu_has_avx ? __ vmulpd(dst, other_src, src) : __ mulpd(dst, src);
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitVecDiv(HVecDiv* instruction) {
  if (CpuHasAvxFeatureFlag()) {
    CreateVecTerOpLocations(GetGraph()->GetAllocator(), instruction);
  } else {
    CreateVecBinOpLocations(GetGraph()->GetAllocator(), instruction);
  }
}

void InstructionCodeGeneratorX86_64::VisitVecDiv(HVecDiv* instruction) {
  bool cpu_has_avx = CpuHasAvxFeatureFlag();
  LocationSummary* locations = instruction->GetLocations();
  XmmRegister src = locations->InAt(1).AsFpuRegister<XmmRegister>();
  XmmRegister other_src = locations->InAt(0).AsFpuRegister<XmmRegister>();
  XmmRegister dst = locations->Out().AsFpuRegister<XmmRegister>();
  DCHECK(cpu_has_avx || other_src == dst);
  switch (instruction->GetPackedType()) {
    case DataType::Type::kFloat32:
      DCHECK_EQ(4u, instruction->GetVectorLength());
      cpu_has_avx ? __ vdivps(dst, other_src, src) : __ divps(dst, src);
      break;
    case DataType::Type::kFloat64:
      DCHECK_EQ(2u, instruction->GetVectorLength());
      cpu_has_avx ? __ vdivpd(dst, other_src, src) : __ divpd(dst, src);
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitVecMin(HVecMin* instruction) {
  CreateVecBinOpLocations(GetGraph()->GetAllocator(), instruction);
}

void InstructionCodeGeneratorX86_64::VisitVecMin(HVecMin* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  DCHECK(locations->InAt(0).Equals(locations->Out()));
  XmmRegister src = locations->InAt(1).AsFpuRegister<XmmRegister>();
  XmmRegister dst = locations->Out().AsFpuRegister<XmmRegister>();
  switch (instruction->GetPackedType()) {
    case DataType::Type::kUint8:
      DCHECK_EQ(16u, instruction->GetVectorLength());
      __ pminub(dst, src);
      break;
    case DataType::Type::kInt8:
      DCHECK_EQ(16u, instruction->GetVectorLength());
      __ pminsb(dst, src);
      break;
    case DataType::Type::kUint16:
      DCHECK_EQ(8u, instruction->GetVectorLength());
      __ pminuw(dst, src);
      break;
    case DataType::Type::kInt16:
      DCHECK_EQ(8u, instruction->GetVectorLength());
      __ pminsw(dst, src);
      break;
    case DataType::Type::kUint32:
      DCHECK_EQ(4u, instruction->GetVectorLength());
      __ pminud(dst, src);
      break;
    case DataType::Type::kInt32:
      DCHECK_EQ(4u, instruction->GetVectorLength());
      __ pminsd(dst, src);
      break;
    // Next cases are sloppy wrt 0.0 vs -0.0.
    case DataType::Type::kFloat32:
      DCHECK_EQ(4u, instruction->GetVectorLength());
      __ minps(dst, src);
      break;
    case DataType::Type::kFloat64:
      DCHECK_EQ(2u, instruction->GetVectorLength());
      __ minpd(dst, src);
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitVecMax(HVecMax* instruction) {
  CreateVecBinOpLocations(GetGraph()->GetAllocator(), instruction);
}

void InstructionCodeGeneratorX86_64::VisitVecMax(HVecMax* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  DCHECK(locations->InAt(0).Equals(locations->Out()));
  XmmRegister src = locations->InAt(1).AsFpuRegister<XmmRegister>();
  XmmRegister dst = locations->Out().AsFpuRegister<XmmRegister>();
  switch (instruction->GetPackedType()) {
    case DataType::Type::kUint8:
      DCHECK_EQ(16u, instruction->GetVectorLength());
      __ pmaxub(dst, src);
      break;
    case DataType::Type::kInt8:
      DCHECK_EQ(16u, instruction->GetVectorLength());
      __ pmaxsb(dst, src);
      break;
    case DataType::Type::kUint16:
      DCHECK_EQ(8u, instruction->GetVectorLength());
      __ pmaxuw(dst, src);
      break;
    case DataType::Type::kInt16:
      DCHECK_EQ(8u, instruction->GetVectorLength());
      __ pmaxsw(dst, src);
      break;
    case DataType::Type::kUint32:
      DCHECK_EQ(4u, instruction->GetVectorLength());
      __ pmaxud(dst, src);
      break;
    case DataType::Type::kInt32:
      DCHECK_EQ(4u, instruction->GetVectorLength());
      __ pmaxsd(dst, src);
      break;
    // Next cases are sloppy wrt 0.0 vs -0.0.
    case DataType::Type::kFloat32:
      DCHECK_EQ(4u, instruction->GetVectorLength());
      __ maxps(dst, src);
      break;
    case DataType::Type::kFloat64:
      DCHECK_EQ(2u, instruction->GetVectorLength());
      __ maxpd(dst, src);
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitVecAnd(HVecAnd* instruction) {
  if (CpuHasAvxFeatureFlag()) {
    CreateVecTerOpLocations(GetGraph()->GetAllocator(), instruction);
  } else {
    CreateVecBinOpLocations(GetGraph()->GetAllocator(), instruction);
  }
}

void InstructionCodeGeneratorX86_64::VisitVecAnd(HVecAnd* instruction) {
  bool cpu_has_avx = CpuHasAvxFeatureFlag();
  LocationSummary* locations = instruction->GetLocations();
  XmmRegister other_src = locations->InAt(0).AsFpuRegister<XmmRegister>();
  XmmRegister src = locations->InAt(1).AsFpuRegister<XmmRegister>();
  XmmRegister dst = locations->Out().AsFpuRegister<XmmRegister>();
  DCHECK(cpu_has_avx || other_src == dst);
  switch (instruction->GetPackedType()) {
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      DCHECK_LE(2u, instruction->GetVectorLength());
      DCHECK_LE(instruction->GetVectorLength(), 16u);
      cpu_has_avx ? __ vpand(dst, other_src, src) : __ pand(dst, src);
      break;
    case DataType::Type::kFloat32:
      DCHECK_EQ(4u, instruction->GetVectorLength());
      cpu_has_avx ? __ vandps(dst, other_src, src) : __ andps(dst, src);
      break;
    case DataType::Type::kFloat64:
      DCHECK_EQ(2u, instruction->GetVectorLength());
      cpu_has_avx ? __ vandpd(dst, other_src, src) : __ andpd(dst, src);
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitVecAndNot(HVecAndNot* instruction) {
  if (CpuHasAvxFeatureFlag()) {
    CreateVecTerOpLocations(GetGraph()->GetAllocator(), instruction);
  } else {
    CreateVecBinOpLocations(GetGraph()->GetAllocator(), instruction);
  }
}

void InstructionCodeGeneratorX86_64::VisitVecAndNot(HVecAndNot* instruction) {
  bool cpu_has_avx = CpuHasAvxFeatureFlag();
  LocationSummary* locations = instruction->GetLocations();
  XmmRegister other_src = locations->InAt(0).AsFpuRegister<XmmRegister>();
  XmmRegister src = locations->InAt(1).AsFpuRegister<XmmRegister>();
  XmmRegister dst = locations->Out().AsFpuRegister<XmmRegister>();
  DCHECK(cpu_has_avx || other_src == dst);
  switch (instruction->GetPackedType()) {
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      DCHECK_LE(2u, instruction->GetVectorLength());
      DCHECK_LE(instruction->GetVectorLength(), 16u);
      cpu_has_avx ? __ vpandn(dst, other_src, src) : __ pandn(dst, src);
      break;
    case DataType::Type::kFloat32:
      DCHECK_EQ(4u, instruction->GetVectorLength());
      cpu_has_avx ? __ vandnps(dst, other_src, src) : __ andnps(dst, src);
      break;
    case DataType::Type::kFloat64:
      DCHECK_EQ(2u, instruction->GetVectorLength());
      cpu_has_avx ? __ vandnpd(dst, other_src, src) : __ andnpd(dst, src);
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitVecOr(HVecOr* instruction) {
  if (CpuHasAvxFeatureFlag()) {
    CreateVecTerOpLocations(GetGraph()->GetAllocator(), instruction);
  } else {
    CreateVecBinOpLocations(GetGraph()->GetAllocator(), instruction);
  }
}

void InstructionCodeGeneratorX86_64::VisitVecOr(HVecOr* instruction) {
  bool cpu_has_avx = CpuHasAvxFeatureFlag();
  LocationSummary* locations = instruction->GetLocations();
  XmmRegister other_src = locations->InAt(0).AsFpuRegister<XmmRegister>();
  XmmRegister src = locations->InAt(1).AsFpuRegister<XmmRegister>();
  XmmRegister dst = locations->Out().AsFpuRegister<XmmRegister>();
  DCHECK(cpu_has_avx || other_src == dst);
  switch (instruction->GetPackedType()) {
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      DCHECK_LE(2u, instruction->GetVectorLength());
      DCHECK_LE(instruction->GetVectorLength(), 16u);
      cpu_has_avx ? __ vpor(dst, other_src, src) : __ por(dst, src);
      break;
    case DataType::Type::kFloat32:
      DCHECK_EQ(4u, instruction->GetVectorLength());
      cpu_has_avx ? __ vorps(dst, other_src, src) : __ orps(dst, src);
      break;
    case DataType::Type::kFloat64:
      DCHECK_EQ(2u, instruction->GetVectorLength());
      cpu_has_avx ? __ vorpd(dst, other_src, src) : __ orpd(dst, src);
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitVecXor(HVecXor* instruction) {
  if (CpuHasAvxFeatureFlag()) {
    CreateVecTerOpLocations(GetGraph()->GetAllocator(), instruction);
  } else {
    CreateVecBinOpLocations(GetGraph()->GetAllocator(), instruction);
  }
}

void InstructionCodeGeneratorX86_64::VisitVecXor(HVecXor* instruction) {
  bool cpu_has_avx = CpuHasAvxFeatureFlag();
  LocationSummary* locations = instruction->GetLocations();
  XmmRegister other_src = locations->InAt(0).AsFpuRegister<XmmRegister>();
  XmmRegister src = locations->InAt(1).AsFpuRegister<XmmRegister>();
  XmmRegister dst = locations->Out().AsFpuRegister<XmmRegister>();
  DCHECK(cpu_has_avx || other_src == dst);
  switch (instruction->GetPackedType()) {
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      DCHECK_LE(2u, instruction->GetVectorLength());
      DCHECK_LE(instruction->GetVectorLength(), 16u);
      cpu_has_avx ? __ vpxor(dst, other_src, src) : __ pxor(dst, src);
      break;
    case DataType::Type::kFloat32:
      DCHECK_EQ(4u, instruction->GetVectorLength());
      cpu_has_avx ? __ vxorps(dst, other_src, src) : __ xorps(dst, src);
      break;
    case DataType::Type::kFloat64:
      DCHECK_EQ(2u, instruction->GetVectorLength());
      cpu_has_avx ? __ vxorpd(dst, other_src, src) : __ xorpd(dst, src);
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
}

// Helper to set up locations for vector shift operations.
static void CreateVecShiftLocations(ArenaAllocator* allocator, HVecBinaryOperation* instruction) {
  LocationSummary* locations = new (allocator) LocationSummary(instruction);
  switch (instruction->GetPackedType()) {
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::ConstantLocation(instruction->InputAt(1)));
      locations->SetOut(Location::SameAsFirstInput());
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitVecShl(HVecShl* instruction) {
  CreateVecShiftLocations(GetGraph()->GetAllocator(), instruction);
}

void InstructionCodeGeneratorX86_64::VisitVecShl(HVecShl* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  DCHECK(locations->InAt(0).Equals(locations->Out()));
  int32_t value = locations->InAt(1).GetConstant()->AsIntConstant()->GetValue();
  XmmRegister dst = locations->Out().AsFpuRegister<XmmRegister>();
  switch (instruction->GetPackedType()) {
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
      DCHECK_EQ(8u, instruction->GetVectorLength());
      __ psllw(dst, Immediate(static_cast<int8_t>(value)));
      break;
    case DataType::Type::kInt32:
      DCHECK_EQ(4u, instruction->GetVectorLength());
      __ pslld(dst, Immediate(static_cast<int8_t>(value)));
      break;
    case DataType::Type::kInt64:
      DCHECK_EQ(2u, instruction->GetVectorLength());
      __ psllq(dst, Immediate(static_cast<int8_t>(value)));
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitVecShr(HVecShr* instruction) {
  CreateVecShiftLocations(GetGraph()->GetAllocator(), instruction);
}

void InstructionCodeGeneratorX86_64::VisitVecShr(HVecShr* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  DCHECK(locations->InAt(0).Equals(locations->Out()));
  int32_t value = locations->InAt(1).GetConstant()->AsIntConstant()->GetValue();
  XmmRegister dst = locations->Out().AsFpuRegister<XmmRegister>();
  switch (instruction->GetPackedType()) {
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
      DCHECK_EQ(8u, instruction->GetVectorLength());
      __ psraw(dst, Immediate(static_cast<int8_t>(value)));
      break;
    case DataType::Type::kInt32:
      DCHECK_EQ(4u, instruction->GetVectorLength());
      __ psrad(dst, Immediate(static_cast<int8_t>(value)));
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitVecUShr(HVecUShr* instruction) {
  CreateVecShiftLocations(GetGraph()->GetAllocator(), instruction);
}

void InstructionCodeGeneratorX86_64::VisitVecUShr(HVecUShr* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  DCHECK(locations->InAt(0).Equals(locations->Out()));
  int32_t value = locations->InAt(1).GetConstant()->AsIntConstant()->GetValue();
  XmmRegister dst = locations->Out().AsFpuRegister<XmmRegister>();
  switch (instruction->GetPackedType()) {
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
      DCHECK_EQ(8u, instruction->GetVectorLength());
      __ psrlw(dst, Immediate(static_cast<int8_t>(value)));
      break;
    case DataType::Type::kInt32:
      DCHECK_EQ(4u, instruction->GetVectorLength());
      __ psrld(dst, Immediate(static_cast<int8_t>(value)));
      break;
    case DataType::Type::kInt64:
      DCHECK_EQ(2u, instruction->GetVectorLength());
      __ psrlq(dst, Immediate(static_cast<int8_t>(value)));
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitVecSetScalars(HVecSetScalars* instruction) {
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(instruction);

  DCHECK_EQ(1u, instruction->InputCount());  // only one input currently implemented

  HInstruction* input = instruction->InputAt(0);
  bool is_zero = IsZeroBitPattern(input);

  switch (instruction->GetPackedType()) {
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      locations->SetInAt(0, is_zero ? Location::ConstantLocation(input)
                                    : Location::RequiresRegister());
      locations->SetOut(Location::RequiresFpuRegister());
      break;
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64:
      locations->SetInAt(0, is_zero ? Location::ConstantLocation(input)
                                    : Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresFpuRegister());
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
}

void InstructionCodeGeneratorX86_64::VisitVecSetScalars(HVecSetScalars* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  XmmRegister dst = locations->Out().AsFpuRegister<XmmRegister>();

  DCHECK_EQ(1u, instruction->InputCount());  // only one input currently implemented

  // Zero out all other elements first.
  bool cpu_has_avx = CpuHasAvxFeatureFlag();
  cpu_has_avx ? __ vxorps(dst, dst, dst) : __ xorps(dst, dst);

  // Shorthand for any type of zero.
  if (IsZeroBitPattern(instruction->InputAt(0))) {
    return;
  }

  // Set required elements.
  switch (instruction->GetPackedType()) {
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:  // TODO: up to here, and?
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
    case DataType::Type::kInt32:
      DCHECK_EQ(4u, instruction->GetVectorLength());
      __ movd(dst, locations->InAt(0).AsRegister<CpuRegister>());
      break;
    case DataType::Type::kInt64:
      DCHECK_EQ(2u, instruction->GetVectorLength());
      __ movq(dst, locations->InAt(0).AsRegister<CpuRegister>());
      break;
    case DataType::Type::kFloat32:
      DCHECK_EQ(4u, instruction->GetVectorLength());
      __ movss(dst, locations->InAt(0).AsFpuRegister<XmmRegister>());
      break;
    case DataType::Type::kFloat64:
      DCHECK_EQ(2u, instruction->GetVectorLength());
      __ movsd(dst, locations->InAt(0).AsFpuRegister<XmmRegister>());
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
}

// Helper to set up locations for vector accumulations.
static void CreateVecAccumLocations(ArenaAllocator* allocator, HVecOperation* instruction) {
  LocationSummary* locations = new (allocator) LocationSummary(instruction);
  switch (instruction->GetPackedType()) {
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      locations->SetInAt(2, Location::RequiresFpuRegister());
      locations->SetOut(Location::SameAsFirstInput());
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitVecMultiplyAccumulate(HVecMultiplyAccumulate* instruction) {
  CreateVecAccumLocations(GetGraph()->GetAllocator(), instruction);
}

void InstructionCodeGeneratorX86_64::VisitVecMultiplyAccumulate(HVecMultiplyAccumulate* instruction) {
  // TODO: pmaddwd?
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
}

void LocationsBuilderX86_64::VisitVecSADAccumulate(HVecSADAccumulate* instruction) {
  CreateVecAccumLocations(GetGraph()->GetAllocator(), instruction);
}

void InstructionCodeGeneratorX86_64::VisitVecSADAccumulate(HVecSADAccumulate* instruction) {
  // TODO: psadbw for unsigned?
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
}

void LocationsBuilderX86_64::VisitVecDotProd(HVecDotProd* instruction) {
  LocationSummary* locations = new (GetGraph()->GetAllocator()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresFpuRegister());
  locations->SetInAt(1, Location::RequiresFpuRegister());
  locations->SetInAt(2, Location::RequiresFpuRegister());
  locations->SetOut(Location::SameAsFirstInput());
  locations->AddTemp(Location::RequiresFpuRegister());
}

void InstructionCodeGeneratorX86_64::VisitVecDotProd(HVecDotProd* instruction) {
  bool cpu_has_avx = CpuHasAvxFeatureFlag();
  LocationSummary* locations = instruction->GetLocations();
  XmmRegister acc = locations->InAt(0).AsFpuRegister<XmmRegister>();
  XmmRegister left = locations->InAt(1).AsFpuRegister<XmmRegister>();
  XmmRegister right = locations->InAt(2).AsFpuRegister<XmmRegister>();
  switch (instruction->GetPackedType()) {
    case DataType::Type::kInt32: {
      DCHECK_EQ(4u, instruction->GetVectorLength());
      XmmRegister tmp = locations->GetTemp(0).AsFpuRegister<XmmRegister>();
      if (!cpu_has_avx) {
        __ movaps(tmp, right);
        __ pmaddwd(tmp, left);
        __ paddd(acc, tmp);
      } else {
        __ vpmaddwd(tmp, left, right);
        __ vpaddd(acc, acc, tmp);
      }
      break;
    }
    default:
      LOG(FATAL) << "Unsupported SIMD Type" << instruction->GetPackedType();
      UNREACHABLE();
  }
}

// Helper to set up locations for vector memory operations.
static void CreateVecMemLocations(ArenaAllocator* allocator,
                                  HVecMemoryOperation* instruction,
                                  bool is_load) {
  LocationSummary* locations = new (allocator) LocationSummary(instruction);
  switch (instruction->GetPackedType()) {
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
    case DataType::Type::kFloat32:
    case DataType::Type::kFloat64:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
      if (is_load) {
        locations->SetOut(Location::RequiresFpuRegister());
      } else {
        locations->SetInAt(2, Location::RequiresFpuRegister());
      }
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
}

// Helper to construct address for vector memory operations.
static Address VecAddress(LocationSummary* locations, size_t size, bool is_string_char_at) {
  Location base = locations->InAt(0);
  Location index = locations->InAt(1);
  ScaleFactor scale = TIMES_1;
  switch (size) {
    case 2: scale = TIMES_2; break;
    case 4: scale = TIMES_4; break;
    case 8: scale = TIMES_8; break;
    default: break;
  }
  // Incorporate the string or array offset in the address computation.
  uint32_t offset = is_string_char_at
      ? mirror::String::ValueOffset().Uint32Value()
      : mirror::Array::DataOffset(size).Uint32Value();
  return CodeGeneratorX86_64::ArrayAddress(base.AsRegister<CpuRegister>(), index, scale, offset);
}

void LocationsBuilderX86_64::VisitVecLoad(HVecLoad* instruction) {
  CreateVecMemLocations(GetGraph()->GetAllocator(), instruction, /*is_load*/ true);
  // String load requires a temporary for the compressed load.
  if (mirror::kUseStringCompression && instruction->IsStringCharAt()) {
    instruction->GetLocations()->AddTemp(Location::RequiresFpuRegister());
  }
}

void InstructionCodeGeneratorX86_64::VisitVecLoad(HVecLoad* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  size_t size = DataType::Size(instruction->GetPackedType());
  Address address = VecAddress(locations, size, instruction->IsStringCharAt());
  XmmRegister reg = locations->Out().AsFpuRegister<XmmRegister>();
  bool is_aligned16 = instruction->GetAlignment().IsAlignedAt(16);
  switch (instruction->GetPackedType()) {
    case DataType::Type::kInt16:  // (short) s.charAt(.) can yield HVecLoad/Int16/StringCharAt.
    case DataType::Type::kUint16:
      DCHECK_EQ(8u, instruction->GetVectorLength());
      // Special handling of compressed/uncompressed string load.
      if (mirror::kUseStringCompression && instruction->IsStringCharAt()) {
        NearLabel done, not_compressed;
        XmmRegister tmp = locations->GetTemp(0).AsFpuRegister<XmmRegister>();
        // Test compression bit.
        static_assert(static_cast<uint32_t>(mirror::StringCompressionFlag::kCompressed) == 0u,
                      "Expecting 0=compressed, 1=uncompressed");
        uint32_t count_offset = mirror::String::CountOffset().Uint32Value();
        __ testb(Address(locations->InAt(0).AsRegister<CpuRegister>(), count_offset), Immediate(1));
        __ j(kNotZero, &not_compressed);
        // Zero extend 8 compressed bytes into 8 chars.
        __ movsd(reg, VecAddress(locations, 1, instruction->IsStringCharAt()));
        __ pxor(tmp, tmp);
        __ punpcklbw(reg, tmp);
        __ jmp(&done);
        // Load 8 direct uncompressed chars.
        __ Bind(&not_compressed);
        is_aligned16 ?  __ movdqa(reg, address) :  __ movdqu(reg, address);
        __ Bind(&done);
        return;
      }
      FALLTHROUGH_INTENDED;
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      DCHECK_LE(2u, instruction->GetVectorLength());
      DCHECK_LE(instruction->GetVectorLength(), 16u);
      is_aligned16 ? __ movdqa(reg, address) : __ movdqu(reg, address);
      break;
    case DataType::Type::kFloat32:
      DCHECK_EQ(4u, instruction->GetVectorLength());
      is_aligned16 ? __ movaps(reg, address) : __ movups(reg, address);
      break;
    case DataType::Type::kFloat64:
      DCHECK_EQ(2u, instruction->GetVectorLength());
      is_aligned16 ? __ movapd(reg, address) : __ movupd(reg, address);
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitVecStore(HVecStore* instruction) {
  CreateVecMemLocations(GetGraph()->GetAllocator(), instruction, /*is_load*/ false);
}

void InstructionCodeGeneratorX86_64::VisitVecStore(HVecStore* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  size_t size = DataType::Size(instruction->GetPackedType());
  Address address = VecAddress(locations, size, /*is_string_char_at*/ false);
  XmmRegister reg = locations->InAt(2).AsFpuRegister<XmmRegister>();
  bool is_aligned16 = instruction->GetAlignment().IsAlignedAt(16);
  switch (instruction->GetPackedType()) {
    case DataType::Type::kBool:
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
    case DataType::Type::kInt64:
      DCHECK_LE(2u, instruction->GetVectorLength());
      DCHECK_LE(instruction->GetVectorLength(), 16u);
      is_aligned16 ? __ movdqa(address, reg) : __ movdqu(address, reg);
      break;
    case DataType::Type::kFloat32:
      DCHECK_EQ(4u, instruction->GetVectorLength());
      is_aligned16 ? __ movaps(address, reg) : __ movups(address, reg);
      break;
    case DataType::Type::kFloat64:
      DCHECK_EQ(2u, instruction->GetVectorLength());
      is_aligned16 ? __ movapd(address, reg) : __ movupd(address, reg);
      break;
    default:
      LOG(FATAL) << "Unsupported SIMD type: " << instruction->GetPackedType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitVecPredSetAll(HVecPredSetAll* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void InstructionCodeGeneratorX86_64::VisitVecPredSetAll(HVecPredSetAll* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void LocationsBuilderX86_64::VisitVecPredWhile(HVecPredWhile* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void InstructionCodeGeneratorX86_64::VisitVecPredWhile(HVecPredWhile* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void LocationsBuilderX86_64::VisitVecPredToBoolean(HVecPredToBoolean* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void InstructionCodeGeneratorX86_64::VisitVecPredToBoolean(HVecPredToBoolean* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void LocationsBuilderX86_64::VisitVecEqual(HVecEqual* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void InstructionCodeGeneratorX86_64::VisitVecEqual(HVecEqual* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void LocationsBuilderX86_64::VisitVecNotEqual(HVecNotEqual* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void InstructionCodeGeneratorX86_64::VisitVecNotEqual(HVecNotEqual* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void LocationsBuilderX86_64::VisitVecLessThan(HVecLessThan* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void InstructionCodeGeneratorX86_64::VisitVecLessThan(HVecLessThan* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void LocationsBuilderX86_64::VisitVecLessThanOrEqual(HVecLessThanOrEqual* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void InstructionCodeGeneratorX86_64::VisitVecLessThanOrEqual(HVecLessThanOrEqual* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void LocationsBuilderX86_64::VisitVecGreaterThan(HVecGreaterThan* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void InstructionCodeGeneratorX86_64::VisitVecGreaterThan(HVecGreaterThan* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void LocationsBuilderX86_64::VisitVecGreaterThanOrEqual(HVecGreaterThanOrEqual* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void InstructionCodeGeneratorX86_64::VisitVecGreaterThanOrEqual(
    HVecGreaterThanOrEqual* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void LocationsBuilderX86_64::VisitVecBelow(HVecBelow* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void InstructionCodeGeneratorX86_64::VisitVecBelow(HVecBelow* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void LocationsBuilderX86_64::VisitVecBelowOrEqual(HVecBelowOrEqual* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void InstructionCodeGeneratorX86_64::VisitVecBelowOrEqual(HVecBelowOrEqual* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void LocationsBuilderX86_64::VisitVecAbove(HVecAbove* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void InstructionCodeGeneratorX86_64::VisitVecAbove(HVecAbove* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void LocationsBuilderX86_64::VisitVecAboveOrEqual(HVecAboveOrEqual* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void InstructionCodeGeneratorX86_64::VisitVecAboveOrEqual(HVecAboveOrEqual* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void LocationsBuilderX86_64::VisitVecPredNot(HVecPredNot* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

void InstructionCodeGeneratorX86_64::VisitVecPredNot(HVecPredNot* instruction) {
  LOG(FATAL) << "No SIMD for " << instruction->GetId();
  UNREACHABLE();
}

#undef __

}  // namespace x86_64
}  // namespace art
