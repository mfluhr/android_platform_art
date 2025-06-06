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

#ifndef ART_COMPILER_OPTIMIZING_LOAD_STORE_ANALYSIS_H_
#define ART_COMPILER_OPTIMIZING_LOAD_STORE_ANALYSIS_H_

#include "base/arena_allocator.h"
#include "base/arena_bit_vector.h"
#include "base/bit_vector-inl.h"
#include "base/macros.h"
#include "base/scoped_arena_allocator.h"
#include "base/scoped_arena_containers.h"
#include "base/stl_util.h"
#include "escape.h"
#include "nodes.h"
#include "optimizing/optimizing_compiler_stats.h"

namespace art HIDDEN {

// A ReferenceInfo contains additional info about a reference such as
// whether it's a singleton, returned, etc.
class ReferenceInfo : public DeletableArenaObject<kArenaAllocLSA> {
 public:
  ReferenceInfo(HInstruction* reference, size_t pos)
      : reference_(reference),
        position_(pos),
        is_singleton_(true),
        is_singleton_and_not_returned_(true),
        is_singleton_and_not_deopt_visible_(true) {
    CalculateEscape(reference_,
                    nullptr,
                    &is_singleton_,
                    &is_singleton_and_not_returned_,
                    &is_singleton_and_not_deopt_visible_);
  }

  HInstruction* GetReference() const {
    return reference_;
  }

  size_t GetPosition() const {
    return position_;
  }

  // Returns true if reference_ is the only name that can refer to its value during
  // the lifetime of the method. So it's guaranteed to not have any alias in
  // the method (including its callees).
  bool IsSingleton() const {
    return is_singleton_;
  }

  // Returns true if reference_ is a singleton and not returned to the caller or
  // used as an environment local of an HDeoptimize instruction.
  // The allocation and stores into reference_ may be eliminated for such cases.
  bool IsSingletonAndRemovable() const {
    return is_singleton_and_not_returned_ && is_singleton_and_not_deopt_visible_;
  }

  // Returns true if reference_ is a singleton and returned to the caller or
  // used as an environment local of an HDeoptimize instruction.
  bool IsSingletonAndNonRemovable() const {
    return is_singleton_ &&
           (!is_singleton_and_not_returned_ || !is_singleton_and_not_deopt_visible_);
  }

 private:
  HInstruction* const reference_;
  // Position in which it was inserted into the ref_infos_ vector. A smaller number means that this
  // reference was seen before a reference with a bigger number.
  const size_t position_;

  // Can only be referred to by a single name in the method.
  bool is_singleton_;
  // Is singleton and not returned to caller.
  bool is_singleton_and_not_returned_;
  // Is singleton and not used as an environment local of HDeoptimize.
  bool is_singleton_and_not_deopt_visible_;

  DISALLOW_COPY_AND_ASSIGN(ReferenceInfo);
};

// A heap location is a reference-offset/index pair that a value can be loaded from
// or stored to.
class HeapLocation : public ArenaObject<kArenaAllocLSA> {
 public:
  static constexpr size_t kInvalidFieldOffset = -1;
  // Default value for heap locations which are not vector data.
  static constexpr size_t kScalar = 1;
  // TODO: more fine-grained array types.
  static constexpr int16_t kDeclaringClassDefIndexForArrays = -1;

  HeapLocation(ReferenceInfo* ref_info,
               DataType::Type type,
               size_t offset,
               HInstruction* index,
               size_t vector_length,
               int16_t declaring_class_def_index,
               bool is_vec_op)
      : ref_info_(ref_info),
        type_(DataType::ToSigned(type)),
        offset_(offset),
        index_(index),
        vector_length_(vector_length),
        declaring_class_def_index_(declaring_class_def_index),
        has_aliased_locations_(false),
        is_vec_op_(is_vec_op) {
    DCHECK(ref_info != nullptr);
    DCHECK((offset == kInvalidFieldOffset && index != nullptr) ||
           (offset != kInvalidFieldOffset && index == nullptr));
  }

  ReferenceInfo* GetReferenceInfo() const { return ref_info_; }
  DataType::Type GetType() const { return type_; }
  size_t GetOffset() const { return offset_; }
  HInstruction* GetIndex() const { return index_; }
  size_t GetVectorLength() const { return vector_length_; }
  bool IsVecOp() const { return is_vec_op_; }

  // Returns the definition of declaring class' dex index.
  // It's kDeclaringClassDefIndexForArrays for an array element.
  int16_t GetDeclaringClassDefIndex() const {
    return declaring_class_def_index_;
  }

  bool IsArray() const {
    return index_ != nullptr;
  }

  bool HasAliasedLocations() const {
    return has_aliased_locations_;
  }

  void SetHasAliasedLocations(bool val) {
    has_aliased_locations_ = val;
  }

 private:
  // Reference for instance/static field, array element or vector data.
  ReferenceInfo* const ref_info_;
  // Type of data residing at HeapLocation (always signed for integral
  // data since e.g. a[i] and a[i] & 0xff are represented by differently
  // signed types; char vs short are disambiguated through the reference).
  const DataType::Type type_;
  // Offset of static/instance field.
  // Invalid when this HeapLocation is not field.
  const size_t offset_;
  // Index of an array element or starting index of vector data.
  // Invalid when this HeapLocation is not array.
  HInstruction* const index_;
  // Vector length of vector data.
  // When this HeapLocation is not vector data, it's value is kScalar.
  const size_t vector_length_;
  // Declaring class's def's dex index.
  // Invalid when this HeapLocation is not field access.
  const int16_t declaring_class_def_index_;
  // Has aliased heap locations in the method, due to either the
  // reference is aliased or the array element is aliased via different
  // index names.
  bool has_aliased_locations_;
  // Whether this HeapLocation represents a vector operation.
  bool is_vec_op_;

  DISALLOW_COPY_AND_ASSIGN(HeapLocation);
};

// A HeapLocationCollector collects all relevant heap locations and keeps
// an aliasing matrix for all locations.
class HeapLocationCollector final : public HGraphVisitor {
 public:
  static constexpr size_t kHeapLocationNotFound = -1;
  // Start with a single uint32_t word. That's enough bits for pair-wise
  // aliasing matrix of 8 heap locations.
  static constexpr uint32_t kInitialAliasingMatrixBitVectorSize = 32;

  HeapLocationCollector(HGraph* graph, ScopedArenaAllocator* allocator)
      : HGraphVisitor(graph),
        allocator_(allocator),
        ref_infos_(graph->GetCurrentInstructionId(), nullptr, allocator->Adapter(kArenaAllocLSA)),
        ref_infos_created_(0u),
        heap_locations_(allocator->Adapter(kArenaAllocLSA)),
        aliasing_matrix_(allocator, kInitialAliasingMatrixBitVectorSize, true, kArenaAllocLSA),
        has_heap_stores_(false) {}

  ~HeapLocationCollector() {
    CleanUp();
  }

  void CleanUp() {
    heap_locations_.clear();
    STLDeleteContainerPointers(ref_infos_.begin(), ref_infos_.end());
    ref_infos_.clear();
  }

  size_t GetNumberOfHeapLocations() const {
    return heap_locations_.size();
  }

  HeapLocation* GetHeapLocation(size_t index) const {
    return heap_locations_[index];
  }

  size_t GetHeapLocationIndex(const HeapLocation* hl) const {
    auto res = std::find(heap_locations_.cbegin(), heap_locations_.cend(), hl);
    return std::distance(heap_locations_.cbegin(), res);
  }

  HInstruction* HuntForOriginalReference(HInstruction* ref) const {
    // An original reference can be transformed by instructions like:
    //   i0 NewArray
    //   i1 HInstruction(i0)  <-- NullCheck, BoundType, IntermediateAddress.
    //   i2 ArrayGet(i1, index)
    DCHECK(ref != nullptr);
    while (ref->IsNullCheck() || ref->IsBoundType() || ref->IsIntermediateAddress()) {
      ref = ref->InputAt(0);
    }
    return ref;
  }

  ReferenceInfo* FindReferenceInfoOf(HInstruction* ref) const {
    return ref_infos_[ref->GetId()];
  }

  size_t GetFieldHeapLocation(HInstruction* object, const FieldInfo* field) const {
    DCHECK(object != nullptr);
    DCHECK(field != nullptr);
    return FindHeapLocationIndex(FindReferenceInfoOf(HuntForOriginalReference(object)),
                                 field->GetFieldType(),
                                 field->GetFieldOffset().SizeValue(),
                                 nullptr,
                                 HeapLocation::kScalar,
                                 field->GetDeclaringClassDefIndex(),
                                 /*is_vec_op=*/false);
  }

  size_t GetArrayHeapLocation(HInstruction* instruction) const {
    DCHECK(instruction != nullptr);
    HInstruction* array = instruction->InputAt(0);
    HInstruction* index = instruction->InputAt(1);
    DataType::Type type = instruction->GetType();
    size_t vector_length = HeapLocation::kScalar;
    const bool is_vec_op = instruction->IsVecStore() || instruction->IsVecLoad();
    if (instruction->IsArraySet()) {
      type = instruction->AsArraySet()->GetComponentType();
    } else if (is_vec_op) {
      HVecOperation* vec_op = instruction->AsVecOperation();
      type = vec_op->GetPackedType();
      vector_length = vec_op->GetVectorLength();
    } else {
      DCHECK(instruction->IsArrayGet());
    }
    return FindHeapLocationIndex(FindReferenceInfoOf(HuntForOriginalReference(array)),
                                 type,
                                 HeapLocation::kInvalidFieldOffset,
                                 index,
                                 vector_length,
                                 HeapLocation::kDeclaringClassDefIndexForArrays,
                                 is_vec_op);
  }

  bool HasHeapStores() const {
    return has_heap_stores_;
  }

  // Find and return the heap location index in heap_locations_.
  // NOTE: When heap locations are created, potentially aliasing/overlapping
  // accesses are given different indexes. This find function also
  // doesn't take aliasing/overlapping into account. For example,
  // this function returns three different indexes for:
  // - ref_info=array, index=i, vector_length=kScalar;
  // - ref_info=array, index=i, vector_length=2;
  // - ref_info=array, index=i, vector_length=4;
  // In later analysis, ComputeMayAlias() and MayAlias() compute and tell whether
  // these indexes alias.
  size_t FindHeapLocationIndex(ReferenceInfo* ref_info,
                               DataType::Type type,
                               size_t offset,
                               HInstruction* index,
                               size_t vector_length,
                               int16_t declaring_class_def_index,
                               bool is_vec_op) const {
    DataType::Type lookup_type = DataType::ToSigned(type);
    for (size_t i = 0; i < heap_locations_.size(); i++) {
      HeapLocation* loc = heap_locations_[i];
      if (loc->GetReferenceInfo() == ref_info &&
          loc->GetType() == lookup_type &&
          loc->GetOffset() == offset &&
          loc->GetIndex() == index &&
          loc->GetVectorLength() == vector_length &&
          loc->GetDeclaringClassDefIndex() == declaring_class_def_index &&
          loc->IsVecOp() == is_vec_op) {
        return i;
      }
    }
    return kHeapLocationNotFound;
  }

  bool InstructionEligibleForLSERemoval(HInstruction* inst) const;

  // Get some estimated statistics based on our analysis.
  void DumpReferenceStats(OptimizingCompilerStats* stats);

  // Returns true if heap_locations_[index1] and heap_locations_[index2] may alias.
  bool MayAlias(size_t index1, size_t index2) const {
    if (index1 < index2) {
      return aliasing_matrix_.IsBitSet(AliasingMatrixPosition(index1, index2));
    } else if (index1 > index2) {
      return aliasing_matrix_.IsBitSet(AliasingMatrixPosition(index2, index1));
    } else {
      DCHECK(false) << "index1 and index2 are expected to be different";
      return true;
    }
  }

  void BuildAliasingMatrix() {
    const size_t number_of_locations = heap_locations_.size();
    if (number_of_locations == 0) {
      return;
    }
    size_t pos = 0;
    // Compute aliasing info between every pair of different heap locations.
    // Save the result in a matrix represented as a BitVector.
    for (size_t i = 0; i < number_of_locations - 1; i++) {
      for (size_t j = i + 1; j < number_of_locations; j++) {
        if (ComputeMayAlias(i, j)) {
          aliasing_matrix_.SetBit(CheckedAliasingMatrixPosition(i, j, pos));
        }
        pos++;
      }
    }
  }

  static bool CanReferencesAlias(ReferenceInfo* ref_info1, ReferenceInfo* ref_info2) {
    if (ref_info1 == ref_info2) {
      return true;
    } else if (ref_info1->IsSingleton()) {
      return false;
    } else if (ref_info2->IsSingleton()) {
      return false;
    } else if (!MayAliasWithPreexistenceChecking(ref_info1, ref_info2) ||
        !MayAliasWithPreexistenceChecking(ref_info2, ref_info1)) {
      return false;
    }
    return true;
  }

 private:
  // An allocation cannot alias with a name which already exists at the point
  // of the allocation, such as a parameter or a load happening before the allocation.
  static bool MayAliasWithPreexistenceChecking(ReferenceInfo* ref_info1, ReferenceInfo* ref_info2) {
    if (ref_info1->GetReference()->IsNewInstance() || ref_info1->GetReference()->IsNewArray()) {
      // Any reference that can alias with the allocation must appear after it in the block/in
      // the block's successors. In reverse post order, those instructions will be visited after
      // the allocation.
      return ref_info2->GetPosition() >= ref_info1->GetPosition();
    }
    return true;
  }

  bool CanArrayElementsAlias(const HInstruction* idx1,
                             const size_t vector_length1,
                             const HInstruction* idx2,
                             const size_t vector_length2) const;

  // `index1` and `index2` are indices in the array of collected heap locations.
  // Returns the position in the bit vector that tracks whether the two heap
  // locations may alias.
  size_t AliasingMatrixPosition(size_t index1, size_t index2) const {
    DCHECK(index2 > index1);
    const size_t number_of_locations = heap_locations_.size();
    // It's (num_of_locations - 1) + ... + (num_of_locations - index1) + (index2 - index1 - 1).
    return (number_of_locations * index1 - (1 + index1) * index1 / 2 + (index2 - index1 - 1));
  }

  // An additional position is passed in to make sure the calculated position is correct.
  size_t CheckedAliasingMatrixPosition(size_t index1, size_t index2, size_t position) {
    size_t calculated_position = AliasingMatrixPosition(index1, index2);
    DCHECK_EQ(calculated_position, position);
    return calculated_position;
  }

  // Compute if two locations may alias to each other.
  bool ComputeMayAlias(size_t index1, size_t index2) const {
    DCHECK_NE(index1, index2);
    HeapLocation* loc1 = heap_locations_[index1];
    HeapLocation* loc2 = heap_locations_[index2];
    if (loc1->GetOffset() != loc2->GetOffset()) {
      // Either two different instance fields, or one is an instance
      // field and the other is an array data.
      return false;
    }
    if (loc1->GetDeclaringClassDefIndex() != loc2->GetDeclaringClassDefIndex()) {
      // Different types.
      return false;
    }
    if (!CanReferencesAlias(loc1->GetReferenceInfo(), loc2->GetReferenceInfo())) {
      return false;
    }
    if (loc1->IsArray() && loc2->IsArray()) {
      HInstruction* idx1 = loc1->GetIndex();
      HInstruction* idx2 = loc2->GetIndex();
      size_t vector_length1 = loc1->GetVectorLength();
      size_t vector_length2 = loc2->GetVectorLength();
      if (!CanArrayElementsAlias(idx1, vector_length1, idx2, vector_length2)) {
        return false;
      }
    }
    loc1->SetHasAliasedLocations(true);
    loc2->SetHasAliasedLocations(true);
    return true;
  }

  ReferenceInfo* GetOrCreateReferenceInfo(HInstruction* instruction) {
    ReferenceInfo* ref_info = FindReferenceInfoOf(instruction);
    if (ref_info == nullptr) {
      ref_info = new (allocator_) ReferenceInfo(instruction, ref_infos_created_);
      ref_infos_created_++;
      ref_infos_[instruction->GetId()] = ref_info;
    }
    return ref_info;
  }

  void CreateReferenceInfoForReferenceType(HInstruction* instruction) {
    if (instruction->GetType() != DataType::Type::kReference) {
      return;
    }
    DCHECK(FindReferenceInfoOf(instruction) == nullptr);
    GetOrCreateReferenceInfo(instruction);
  }

  void MaybeCreateHeapLocation(HInstruction* ref,
                               DataType::Type type,
                               size_t offset,
                               HInstruction* index,
                               size_t vector_length,
                               int16_t declaring_class_def_index,
                               bool is_vec_op) {
    HInstruction* original_ref = HuntForOriginalReference(ref);
    ReferenceInfo* ref_info = GetOrCreateReferenceInfo(original_ref);
    size_t heap_location_idx = FindHeapLocationIndex(
        ref_info, type, offset, index, vector_length, declaring_class_def_index, is_vec_op);
    if (heap_location_idx == kHeapLocationNotFound) {
      HeapLocation* heap_loc = new (allocator_) HeapLocation(
          ref_info, type, offset, index, vector_length, declaring_class_def_index, is_vec_op);
      heap_locations_.push_back(heap_loc);
    }
  }

  void VisitFieldAccess(HFieldAccess* instruction) override {
    HInstruction* ref = instruction->InputAt(0);
    const FieldInfo& field_info = instruction->GetFieldInfo();
    DataType::Type type = field_info.GetFieldType();
    const uint16_t declaring_class_def_index = field_info.GetDeclaringClassDefIndex();
    const size_t offset = field_info.GetFieldOffset().SizeValue();
    MaybeCreateHeapLocation(ref,
                            type,
                            offset,
                            nullptr,
                            HeapLocation::kScalar,
                            declaring_class_def_index,
                            /*is_vec_op=*/false);
  }

  void VisitArrayAccess(HInstruction* array,
                        HInstruction* index,
                        DataType::Type type,
                        size_t vector_length,
                        bool is_vec_op) {
    MaybeCreateHeapLocation(array,
                            type,
                            HeapLocation::kInvalidFieldOffset,
                            index,
                            vector_length,
                            HeapLocation::kDeclaringClassDefIndexForArrays,
                            is_vec_op);
  }

  void VisitInstanceFieldGet(HInstanceFieldGet* instruction) override {
    CreateReferenceInfoForReferenceType(instruction);
    VisitFieldAccess(instruction);
  }

  void VisitInstanceFieldSet(HInstanceFieldSet* instruction) override {
    has_heap_stores_ = true;
    VisitFieldAccess(instruction);
  }

  void VisitStaticFieldGet(HStaticFieldGet* instruction) override {
    CreateReferenceInfoForReferenceType(instruction);
    VisitFieldAccess(instruction);
  }

  void VisitStaticFieldSet(HStaticFieldSet* instruction) override {
    has_heap_stores_ = true;
    VisitFieldAccess(instruction);
  }

  // We intentionally don't collect HUnresolvedInstanceField/HUnresolvedStaticField accesses
  // since we cannot accurately track the fields.

  void VisitArrayGet(HArrayGet* instruction) override {
    HInstruction* array = instruction->InputAt(0);
    HInstruction* index = instruction->InputAt(1);
    DataType::Type type = instruction->GetType();
    VisitArrayAccess(array, index, type, HeapLocation::kScalar, /*is_vec_op=*/false);
    CreateReferenceInfoForReferenceType(instruction);
  }

  void VisitArraySet(HArraySet* instruction) override {
    HInstruction* array = instruction->InputAt(0);
    HInstruction* index = instruction->InputAt(1);
    DataType::Type type = instruction->GetComponentType();
    VisitArrayAccess(array, index, type, HeapLocation::kScalar, /*is_vec_op=*/false);
    has_heap_stores_ = true;
  }

  void VisitVecLoad(HVecLoad* instruction) override {
    DCHECK(!instruction->IsPredicated());
    HInstruction* array = instruction->InputAt(0);
    HInstruction* index = instruction->InputAt(1);
    DataType::Type type = instruction->GetPackedType();
    VisitArrayAccess(array, index, type, instruction->GetVectorLength(), /*is_vec_op=*/true);
    CreateReferenceInfoForReferenceType(instruction);
  }

  void VisitVecStore(HVecStore* instruction) override {
    DCHECK(!instruction->IsPredicated());
    HInstruction* array = instruction->InputAt(0);
    HInstruction* index = instruction->InputAt(1);
    DataType::Type type = instruction->GetPackedType();
    VisitArrayAccess(array, index, type, instruction->GetVectorLength(), /*is_vec_op=*/true);
    has_heap_stores_ = true;
  }

  void VisitInstruction(HInstruction* instruction) override {
    // Any new-instance or new-array cannot alias with references that
    // pre-exist the new-instance/new-array. The entries of ref_infos_ keep track of the order of
    // creation of reference values since we visit the blocks in reverse post order.
    //
    // By default, VisitXXX() (including VisitPhi()) calls VisitInstruction(),
    // unless VisitXXX() is overridden. VisitInstanceFieldGet() etc. above
    // also call CreateReferenceInfoForReferenceType() explicitly.
    CreateReferenceInfoForReferenceType(instruction);
  }

  ScopedArenaAllocator* allocator_;
  // All references used for heap accesses, accessed via instruction id.
  ScopedArenaVector<ReferenceInfo*> ref_infos_;
  // How many non-null ReferenceInfo* are in ref_infos_.
  size_t ref_infos_created_;
  ScopedArenaVector<HeapLocation*> heap_locations_;    // All heap locations.
  ArenaBitVector aliasing_matrix_;    // aliasing info between each pair of locations.
  bool has_heap_stores_;    // If there is no heap stores, LSE acts as GVN with better
                            // alias analysis and won't be as effective.

  DISALLOW_COPY_AND_ASSIGN(HeapLocationCollector);
};

class LoadStoreAnalysis {
 public:
  explicit LoadStoreAnalysis(HGraph* graph,
                             OptimizingCompilerStats* stats,
                             ScopedArenaAllocator* local_allocator)
      : graph_(graph), stats_(stats), heap_location_collector_(graph, local_allocator) {}

  const HeapLocationCollector& GetHeapLocationCollector() const {
    return heap_location_collector_;
  }

  bool Run();

 private:
  HGraph* graph_;
  OptimizingCompilerStats* stats_;
  HeapLocationCollector heap_location_collector_;

  DISALLOW_COPY_AND_ASSIGN(LoadStoreAnalysis);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_LOAD_STORE_ANALYSIS_H_
