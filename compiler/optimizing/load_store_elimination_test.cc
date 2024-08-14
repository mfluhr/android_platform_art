/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "load_store_elimination.h"

#include <initializer_list>
#include <memory>
#include <tuple>
#include <variant>

#include "base/iteration_range.h"
#include "compilation_kind.h"
#include "dex/dex_file_types.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "entrypoints/quick/quick_entrypoints_enum.h"
#include "gtest/gtest.h"
#include "handle_scope.h"
#include "load_store_analysis.h"
#include "nodes.h"
#include "optimizing/data_type.h"
#include "optimizing/instruction_simplifier.h"
#include "optimizing/optimizing_compiler_stats.h"
#include "optimizing_unit_test.h"
#include "scoped_thread_state_change.h"

namespace art HIDDEN {

static constexpr bool kDebugLseTests = false;

#define CHECK_SUBROUTINE_FAILURE() \
  do {                             \
    if (HasFatalFailure()) {       \
      return;                      \
    }                              \
  } while (false)

template <typename SuperTest>
class LoadStoreEliminationTestBase : public SuperTest, public OptimizingUnitTestHelper {
 public:
  LoadStoreEliminationTestBase() {
    this->use_boot_image_ = true;  // Make the Runtime creation cheaper.
  }

  void SetUp() override {
    SuperTest::SetUp();
    if (kDebugLseTests) {
      gLogVerbosity.compiler = true;
    }
  }

  void TearDown() override {
    SuperTest::TearDown();
    if (kDebugLseTests) {
      gLogVerbosity.compiler = false;
    }
  }

  void PerformLSE() {
    graph_->BuildDominatorTree();
    LoadStoreElimination lse(graph_, /*stats=*/nullptr);
    lse.Run();
    std::ostringstream oss;
    EXPECT_TRUE(CheckGraph(oss)) << oss.str();
  }

  void PerformLSE(const AdjacencyListGraph& blks) {
    // PerformLSE expects this to be empty, and the creation of
    // an `AdjacencyListGraph` computes it.
    graph_->ClearDominanceInformation();
    if (kDebugLseTests) {
      LOG(INFO) << "Pre LSE " << blks;
    }
    PerformLSE();
    if (kDebugLseTests) {
      LOG(INFO) << "Post LSE " << blks;
    }
  }

  // Create instructions shared among tests.
  void CreateEntryBlockInstructions() {
    HInstruction* c1 = graph_->GetIntConstant(1);
    HInstruction* c4 = graph_->GetIntConstant(4);
    i_add1_ = MakeBinOp<HAdd>(entry_block_, DataType::Type::kInt32, i_, c1);
    i_add4_ = MakeBinOp<HAdd>(entry_block_, DataType::Type::kInt32, i_, c4);
    MakeGoto(entry_block_);
  }

  // Create the major CFG used by tests:
  //    entry
  //      |
  //  pre_header
  //      |
  //    loop[]
  //      |
  //   return
  //      |
  //     exit
  void CreateTestControlFlowGraph() {
    InitGraphAndParameters();
    pre_header_ = AddNewBlock();
    loop_ = AddNewBlock();

    entry_block_->ReplaceSuccessor(return_block_, pre_header_);
    pre_header_->AddSuccessor(loop_);
    loop_->AddSuccessor(loop_);
    loop_->AddSuccessor(return_block_);

    HInstruction* c0 = graph_->GetIntConstant(0);
    HInstruction* c1 = graph_->GetIntConstant(1);
    HInstruction* c128 = graph_->GetIntConstant(128);

    CreateEntryBlockInstructions();

    // pre_header block
    //   phi = 0;
    phi_ = MakePhi(loop_, {c0, /* placeholder */ c0});
    MakeGoto(pre_header_);

    // loop block:
    //   suspend_check
    //   phi++;
    //   if (phi >= 128)
    suspend_check_ = MakeSuspendCheck(loop_);
    HInstruction* inc_phi = MakeBinOp<HAdd>(loop_, DataType::Type::kInt32, phi_, c1);
    HInstruction* cmp = MakeCondition<HGreaterThanOrEqual>(loop_, phi_, c128);
    MakeIf(loop_, cmp);
    phi_->ReplaceInput(inc_phi, 1u);  // Update back-edge input.

    CreateEnvForSuspendCheck();
  }

  void CreateEnvForSuspendCheck() {
    ManuallyBuildEnvFor(suspend_check_, {array_, i_, j_});
  }

  // Create the diamond-shaped CFG:
  //      upper
  //      /   \
  //    left  right
  //      \   /
  //      down
  //
  // Return: the basic blocks forming the CFG in the following order {upper, left, right, down}.
  std::tuple<HBasicBlock*, HBasicBlock*, HBasicBlock*, HBasicBlock*> CreateDiamondShapedCFG() {
    InitGraphAndParameters();
    CreateEntryBlockInstructions();

    auto [upper, left, right] = CreateDiamondPattern(return_block_);

    HInstruction* cmp = MakeCondition<HGreaterThanOrEqual>(upper, i_, j_);
    MakeIf(upper, cmp);

    return std::make_tuple(upper, left, right, return_block_);
  }

  // Add a HVecLoad instruction to the end of the provided basic block.
  //
  // Return: the created HVecLoad instruction.
  HInstruction* AddVecLoad(HBasicBlock* block, HInstruction* array, HInstruction* index) {
    DCHECK(block != nullptr);
    DCHECK(array != nullptr);
    DCHECK(index != nullptr);
    HInstruction* vload =
        new (GetAllocator()) HVecLoad(GetAllocator(),
                                      array,
                                      index,
                                      DataType::Type::kInt32,
                                      SideEffects::ArrayReadOfType(DataType::Type::kInt32),
                                      4,
                                      /*is_string_char_at*/ false,
                                      kNoDexPc);
    block->InsertInstructionBefore(vload, block->GetLastInstruction());
    return vload;
  }

  // Add a HVecStore instruction to the end of the provided basic block.
  // If no vdata is specified, generate HVecStore: array[index] = [1,1,1,1].
  //
  // Return: the created HVecStore instruction.
  HInstruction* AddVecStore(HBasicBlock* block,
                            HInstruction* array,
                            HInstruction* index,
                            HInstruction* vdata = nullptr) {
    DCHECK(block != nullptr);
    DCHECK(array != nullptr);
    DCHECK(index != nullptr);
    if (vdata == nullptr) {
      HInstruction* c1 = graph_->GetIntConstant(1);
      vdata = new (GetAllocator())
          HVecReplicateScalar(GetAllocator(), c1, DataType::Type::kInt32, 4, kNoDexPc);
      block->InsertInstructionBefore(vdata, block->GetLastInstruction());
    }
    HInstruction* vstore =
        new (GetAllocator()) HVecStore(GetAllocator(),
                                       array,
                                       index,
                                       vdata,
                                       DataType::Type::kInt32,
                                       SideEffects::ArrayWriteOfType(DataType::Type::kInt32),
                                       4,
                                       kNoDexPc);
    block->InsertInstructionBefore(vstore, block->GetLastInstruction());
    return vstore;
  }

  // Add a HArrayGet instruction to the end of the provided basic block.
  //
  // Return: the created HArrayGet instruction.
  HInstruction* AddArrayGet(HBasicBlock* block, HInstruction* array, HInstruction* index) {
    DCHECK(block != nullptr);
    DCHECK(array != nullptr);
    DCHECK(index != nullptr);
    HInstruction* get = new (GetAllocator()) HArrayGet(array, index, DataType::Type::kInt32, 0);
    block->InsertInstructionBefore(get, block->GetLastInstruction());
    return get;
  }

  // Add a HArraySet instruction to the end of the provided basic block.
  // If no data is specified, generate HArraySet: array[index] = 1.
  //
  // Return: the created HArraySet instruction.
  HInstruction* AddArraySet(HBasicBlock* block,
                            HInstruction* array,
                            HInstruction* index,
                            HInstruction* data = nullptr) {
    DCHECK(block != nullptr);
    DCHECK(array != nullptr);
    DCHECK(index != nullptr);
    if (data == nullptr) {
      data = graph_->GetIntConstant(1);
    }
    HInstruction* store =
        new (GetAllocator()) HArraySet(array, index, data, DataType::Type::kInt32, 0);
    block->InsertInstructionBefore(store, block->GetLastInstruction());
    return store;
  }

  void InitGraphAndParameters() {
    return_block_ = InitEntryMainExitGraphWithReturnVoid();
    array_ = MakeParam(DataType::Type::kInt32);
    i_ = MakeParam(DataType::Type::kInt32);
    j_ = MakeParam(DataType::Type::kInt32);
  }

  HBasicBlock* return_block_;
  HBasicBlock* pre_header_;
  HBasicBlock* loop_;

  HInstruction* array_;
  HInstruction* i_;
  HInstruction* j_;
  HInstruction* i_add1_;
  HInstruction* i_add4_;
  HInstruction* suspend_check_;

  HPhi* phi_;
};

class LoadStoreEliminationTest : public LoadStoreEliminationTestBase<CommonCompilerTest> {};

enum class TestOrder { kSameAsAlloc, kReverseOfAlloc };
std::ostream& operator<<(std::ostream& os, const TestOrder& ord) {
  switch (ord) {
    case TestOrder::kSameAsAlloc:
      return os << "SameAsAlloc";
    case TestOrder::kReverseOfAlloc:
      return os << "ReverseOfAlloc";
  }
}

TEST_F(LoadStoreEliminationTest, ArrayGetSetElimination) {
  CreateTestControlFlowGraph();

  HInstruction* c1 = graph_->GetIntConstant(1);
  HInstruction* c2 = graph_->GetIntConstant(2);
  HInstruction* c3 = graph_->GetIntConstant(3);

  // array[1] = 1;
  // x = array[1];  <--- Remove.
  // y = array[2];
  // array[1] = 1;  <--- Remove, since it stores same value.
  // array[i] = 3;  <--- MAY alias.
  // array[1] = 1;  <--- Cannot remove, even if it stores the same value.
  AddArraySet(entry_block_, array_, c1, c1);
  HInstruction* load1 = AddArrayGet(entry_block_, array_, c1);
  HInstruction* load2 = AddArrayGet(entry_block_, array_, c2);
  HInstruction* store1 = AddArraySet(entry_block_, array_, c1, c1);
  AddArraySet(entry_block_, array_, i_, c3);
  HInstruction* store2 = AddArraySet(entry_block_, array_, c1, c1);

  PerformLSE();

  ASSERT_TRUE(IsRemoved(load1));
  ASSERT_FALSE(IsRemoved(load2));
  ASSERT_TRUE(IsRemoved(store1));
  ASSERT_FALSE(IsRemoved(store2));
}

TEST_F(LoadStoreEliminationTest, SameHeapValue1) {
  CreateTestControlFlowGraph();

  HInstruction* c1 = graph_->GetIntConstant(1);
  HInstruction* c2 = graph_->GetIntConstant(2);

  // Test LSE handling same value stores on array.
  // array[1] = 1;
  // array[2] = 1;
  // array[1] = 1;  <--- Can remove.
  // array[1] = 2;  <--- Can NOT remove.
  AddArraySet(entry_block_, array_, c1, c1);
  AddArraySet(entry_block_, array_, c2, c1);
  HInstruction* store1 = AddArraySet(entry_block_, array_, c1, c1);
  HInstruction* store2 = AddArraySet(entry_block_, array_, c1, c2);

  PerformLSE();

  ASSERT_TRUE(IsRemoved(store1));
  ASSERT_FALSE(IsRemoved(store2));
}

TEST_F(LoadStoreEliminationTest, SameHeapValue2) {
  CreateTestControlFlowGraph();

  // Test LSE handling same value stores on vector.
  // vdata = [0x1, 0x2, 0x3, 0x4, ...]
  // VecStore array[i...] = vdata;
  // VecStore array[j...] = vdata;  <--- MAY ALIAS.
  // VecStore array[i...] = vdata;  <--- Cannot Remove, even if it's same value.
  AddVecStore(entry_block_, array_, i_);
  AddVecStore(entry_block_, array_, j_);
  HInstruction* vstore = AddVecStore(entry_block_, array_, i_);

  // TODO: enable LSE for graphs with predicated SIMD.
  graph_->SetHasTraditionalSIMD(true);
  PerformLSE();

  ASSERT_FALSE(IsRemoved(vstore));
}

TEST_F(LoadStoreEliminationTest, SameHeapValue3) {
  CreateTestControlFlowGraph();

  // VecStore array[i...] = vdata;
  // VecStore array[i+1...] = vdata;  <--- MAY alias due to partial overlap.
  // VecStore array[i...] = vdata;    <--- Cannot remove, even if it's same value.
  AddVecStore(entry_block_, array_, i_);
  AddVecStore(entry_block_, array_, i_add1_);
  HInstruction* vstore = AddVecStore(entry_block_, array_, i_);

  // TODO: enable LSE for graphs with predicated SIMD.
  graph_->SetHasTraditionalSIMD(true);
  PerformLSE();

  ASSERT_FALSE(IsRemoved(vstore));
}

TEST_F(LoadStoreEliminationTest, OverlappingLoadStore) {
  CreateTestControlFlowGraph();

  HInstruction* c1 = graph_->GetIntConstant(1);

  // Test LSE handling array LSE when there is vector store in between.
  // a[i] = 1;
  // .. = a[i];                <-- Remove.
  // a[i,i+1,i+2,i+3] = data;  <-- PARTIAL OVERLAP !
  // .. = a[i];                <-- Cannot remove.
  AddArraySet(entry_block_, array_, i_, c1);
  HInstruction* load1 = AddArrayGet(entry_block_, array_, i_);
  AddVecStore(entry_block_, array_, i_);
  HInstruction* load2 = AddArrayGet(entry_block_, array_, i_);

  // Test LSE handling vector load/store partial overlap.
  // a[i,i+1,i+2,i+3] = data;
  // a[i+4,i+5,i+6,i+7] = data;
  // .. = a[i,i+1,i+2,i+3];
  // .. = a[i+4,i+5,i+6,i+7];
  // a[i+1,i+2,i+3,i+4] = data;  <-- PARTIAL OVERLAP !
  // .. = a[i,i+1,i+2,i+3];
  // .. = a[i+4,i+5,i+6,i+7];
  AddVecStore(entry_block_, array_, i_);
  AddVecStore(entry_block_, array_, i_add4_);
  HInstruction* vload1 = AddVecLoad(entry_block_, array_, i_);
  HInstruction* vload2 = AddVecLoad(entry_block_, array_, i_add4_);
  AddVecStore(entry_block_, array_, i_add1_);
  HInstruction* vload3 = AddVecLoad(entry_block_, array_, i_);
  HInstruction* vload4 = AddVecLoad(entry_block_, array_, i_add4_);

  // Test LSE handling vector LSE when there is array store in between.
  // a[i,i+1,i+2,i+3] = data;
  // a[i+1] = 1;                 <-- PARTIAL OVERLAP !
  // .. = a[i,i+1,i+2,i+3];
  AddVecStore(entry_block_, array_, i_);
  AddArraySet(entry_block_, array_, i_, c1);
  HInstruction* vload5 = AddVecLoad(entry_block_, array_, i_);

  // TODO: enable LSE for graphs with predicated SIMD.
  graph_->SetHasTraditionalSIMD(true);
  PerformLSE();

  ASSERT_TRUE(IsRemoved(load1));
  ASSERT_FALSE(IsRemoved(load2));

  ASSERT_TRUE(IsRemoved(vload1));
  ASSERT_TRUE(IsRemoved(vload2));
  ASSERT_FALSE(IsRemoved(vload3));
  ASSERT_FALSE(IsRemoved(vload4));

  ASSERT_FALSE(IsRemoved(vload5));
}
// function (int[] a, int j) {
// a[j] = 1;
// for (int i=0; i<128; i++) {
//    /* doesn't do any write */
// }
// a[j] = 1;
TEST_F(LoadStoreEliminationTest, StoreAfterLoopWithoutSideEffects) {
  CreateTestControlFlowGraph();

  HInstruction* c1 = graph_->GetIntConstant(1);

  // a[j] = 1
  AddArraySet(pre_header_, array_, j_, c1);

  // LOOP BODY:
  // .. = a[i,i+1,i+2,i+3];
  AddVecLoad(loop_, array_, phi_);

  // a[j] = 1;
  HInstruction* array_set = AddArraySet(return_block_, array_, j_, c1);

  // TODO: enable LSE for graphs with predicated SIMD.
  graph_->SetHasTraditionalSIMD(true);
  PerformLSE();

  ASSERT_TRUE(IsRemoved(array_set));
}

// function (int[] a, int j) {
//   int[] b = new int[128];
//   a[j] = 0;
//   for (int phi=0; phi<128; phi++) {
//     a[phi,phi+1,phi+2,phi+3] = [1,1,1,1];
//     b[phi,phi+1,phi+2,phi+3] = a[phi,phi+1,phi+2,phi+3];
//   }
//   a[j] = 0;
// }
TEST_F(LoadStoreEliminationTest, StoreAfterSIMDLoopWithSideEffects) {
  CreateTestControlFlowGraph();

  HInstruction* c0 = graph_->GetIntConstant(0);
  HInstruction* c128 = graph_->GetIntConstant(128);

  HInstruction* array_b = new (GetAllocator()) HNewArray(c0, c128, 0, 0);
  pre_header_->InsertInstructionBefore(array_b, pre_header_->GetLastInstruction());
  array_b->CopyEnvironmentFrom(suspend_check_->GetEnvironment());

  // a[j] = 0;
  AddArraySet(pre_header_, array_, j_, c0);

  // LOOP BODY:
  // a[phi,phi+1,phi+2,phi+3] = [1,1,1,1];
  // b[phi,phi+1,phi+2,phi+3] = a[phi,phi+1,phi+2,phi+3];
  AddVecStore(loop_, array_, phi_);
  HInstruction* vload = AddVecLoad(loop_, array_, phi_);
  AddVecStore(loop_, array_b, phi_, vload);

  // a[j] = 0;
  HInstruction* a_set = AddArraySet(return_block_, array_, j_, c0);

  // TODO: enable LSE for graphs with predicated SIMD.
  graph_->SetHasTraditionalSIMD(true);
  PerformLSE();

  ASSERT_TRUE(IsRemoved(vload));
  ASSERT_FALSE(IsRemoved(a_set));  // Cannot remove due to write side-effect in the loop.
}

// function (int[] a, int j) {
//   int[] b = new int[128];
//   a[j] = 0;
//   for (int phi=0; phi<128; phi++) {
//     a[phi,phi+1,phi+2,phi+3] = [1,1,1,1];
//     b[phi,phi+1,phi+2,phi+3] = a[phi,phi+1,phi+2,phi+3];
//   }
//   x = a[j];
// }
TEST_F(LoadStoreEliminationTest, LoadAfterSIMDLoopWithSideEffects) {
  CreateTestControlFlowGraph();

  HInstruction* c0 = graph_->GetIntConstant(0);
  HInstruction* c128 = graph_->GetIntConstant(128);

  HInstruction* array_b = new (GetAllocator()) HNewArray(c0, c128, 0, 0);
  pre_header_->InsertInstructionBefore(array_b, pre_header_->GetLastInstruction());
  array_b->CopyEnvironmentFrom(suspend_check_->GetEnvironment());

  // a[j] = 0;
  AddArraySet(pre_header_, array_, j_, c0);

  // LOOP BODY:
  // a[phi,phi+1,phi+2,phi+3] = [1,1,1,1];
  // b[phi,phi+1,phi+2,phi+3] = a[phi,phi+1,phi+2,phi+3];
  AddVecStore(loop_, array_, phi_);
  HInstruction* vload = AddVecLoad(loop_, array_, phi_);
  AddVecStore(loop_, array_b, phi_, vload);

  // x = a[j];
  HInstruction* load = AddArrayGet(return_block_, array_, j_);

  // TODO: enable LSE for graphs with predicated SIMD.
  graph_->SetHasTraditionalSIMD(true);
  PerformLSE();

  ASSERT_TRUE(IsRemoved(vload));
  ASSERT_FALSE(IsRemoved(load));  // Cannot remove due to write side-effect in the loop.
}

// Check that merging works correctly when there are VecStors in predecessors.
//
//                  vstore1: a[i,... i + 3] = [1,...1]
//                       /          \
//                      /            \
// vstore2: a[i,... i + 3] = [1,...1]  vstore3: a[i+1, ... i + 4] = [1, ... 1]
//                     \              /
//                      \            /
//                  vstore4: a[i,... i + 3] = [1,...1]
//
// Expected:
//   'vstore2' is removed.
//   'vstore3' is not removed.
//   'vstore4' is not removed. Such cases are not supported at the moment.
TEST_F(LoadStoreEliminationTest, MergePredecessorVecStores) {
  auto [upper, left, right, down] = CreateDiamondShapedCFG();

  // upper: a[i,... i + 3] = [1,...1]
  HInstruction* vstore1 = AddVecStore(upper, array_, i_);
  HInstruction* vdata = vstore1->InputAt(2);

  // left: a[i,... i + 3] = [1,...1]
  HInstruction* vstore2 = AddVecStore(left, array_, i_, vdata);

  // right: a[i+1, ... i + 4] = [1, ... 1]
  HInstruction* vstore3 = AddVecStore(right, array_, i_add1_, vdata);

  // down: a[i,... i + 3] = [1,...1]
  HInstruction* vstore4 = AddVecStore(down, array_, i_, vdata);

  // TODO: enable LSE for graphs with predicated SIMD.
  graph_->SetHasTraditionalSIMD(true);
  PerformLSE();

  ASSERT_TRUE(IsRemoved(vstore2));
  ASSERT_FALSE(IsRemoved(vstore3));
  ASSERT_FALSE(IsRemoved(vstore4));
}

// Check that merging works correctly when there are ArraySets in predecessors.
//
//          a[i] = 1
//        /          \
//       /            \
// store1: a[i] = 1  store2: a[i+1] = 1
//       \            /
//        \          /
//          store3: a[i] = 1
//
// Expected:
//   'store1' is removed.
//   'store2' is not removed.
//   'store3' is removed.
TEST_F(LoadStoreEliminationTest, MergePredecessorStores) {
  auto [upper, left, right, down] = CreateDiamondShapedCFG();

  // upper: a[i,... i + 3] = [1,...1]
  AddArraySet(upper, array_, i_);

  // left: a[i,... i + 3] = [1,...1]
  HInstruction* store1 = AddArraySet(left, array_, i_);

  // right: a[i+1, ... i + 4] = [1, ... 1]
  HInstruction* store2 = AddArraySet(right, array_, i_add1_);

  // down: a[i,... i + 3] = [1,...1]
  HInstruction* store3 = AddArraySet(down, array_, i_);

  PerformLSE();

  ASSERT_TRUE(IsRemoved(store1));
  ASSERT_FALSE(IsRemoved(store2));
  ASSERT_TRUE(IsRemoved(store3));
}

// Check that redundant VStore/VLoad are removed from a SIMD loop.
//
//  LOOP BODY
//     vstore1: a[i,... i + 3] = [1,...1]
//     vload:   x = a[i,... i + 3]
//     vstore2: b[i,... i + 3] = x
//     vstore3: a[i,... i + 3] = [1,...1]
//
// Return 'a' from the method to make it escape.
//
// Expected:
//   'vstore1' is not removed.
//   'vload' is removed.
//   'vstore2' is removed because 'b' does not escape.
//   'vstore3' is removed.
TEST_F(LoadStoreEliminationTest, RedundantVStoreVLoadInLoop) {
  CreateTestControlFlowGraph();

  HInstruction* c0 = graph_->GetIntConstant(0);
  HInstruction* c128 = graph_->GetIntConstant(128);

  HInstruction* array_a = new (GetAllocator()) HNewArray(c0, c128, 0, 0);
  pre_header_->InsertInstructionBefore(array_a, pre_header_->GetLastInstruction());
  array_a->CopyEnvironmentFrom(suspend_check_->GetEnvironment());

  ASSERT_TRUE(return_block_->GetLastInstruction()->IsReturnVoid());
  HInstruction* ret = new (GetAllocator()) HReturn(array_a);
  return_block_->ReplaceAndRemoveInstructionWith(return_block_->GetLastInstruction(), ret);

  HInstruction* array_b = new (GetAllocator()) HNewArray(c0, c128, 0, 0);
  pre_header_->InsertInstructionBefore(array_b, pre_header_->GetLastInstruction());
  array_b->CopyEnvironmentFrom(suspend_check_->GetEnvironment());

  // LOOP BODY:
  //    a[i,... i + 3] = [1,...1]
  //    x = a[i,... i + 3]
  //    b[i,... i + 3] = x
  //    a[i,... i + 3] = [1,...1]
  HInstruction* vstore1 = AddVecStore(loop_, array_a, phi_);
  HInstruction* vload = AddVecLoad(loop_, array_a, phi_);
  HInstruction* vstore2 = AddVecStore(loop_, array_b, phi_, vload);
  HInstruction* vstore3 = AddVecStore(loop_, array_a, phi_, vstore1->InputAt(2));

  // TODO: enable LSE for graphs with predicated SIMD.
  graph_->SetHasTraditionalSIMD(true);
  PerformLSE();

  ASSERT_FALSE(IsRemoved(vstore1));
  ASSERT_TRUE(IsRemoved(vload));
  ASSERT_TRUE(IsRemoved(vstore2));
  ASSERT_TRUE(IsRemoved(vstore3));
}

// Loop writes invalidate only possibly aliased heap locations.
TEST_F(LoadStoreEliminationTest, StoreAfterLoopWithSideEffects) {
  CreateTestControlFlowGraph();

  HInstruction* c0 = graph_->GetIntConstant(0);
  HInstruction* c2 = graph_->GetIntConstant(2);
  HInstruction* c128 = graph_->GetIntConstant(128);

  // array[0] = 2;
  // loop:
  //   b[i] = array[i]
  // array[0] = 2
  HInstruction* store1 = AddArraySet(entry_block_, array_, c0, c2);

  HInstruction* array_b = new (GetAllocator()) HNewArray(c0, c128, 0, 0);
  pre_header_->InsertInstructionBefore(array_b, pre_header_->GetLastInstruction());
  array_b->CopyEnvironmentFrom(suspend_check_->GetEnvironment());

  HInstruction* load = AddArrayGet(loop_, array_, phi_);
  HInstruction* store2 = AddArraySet(loop_, array_b, phi_, load);

  HInstruction* store3 = AddArraySet(return_block_, array_, c0, c2);

  PerformLSE();

  ASSERT_FALSE(IsRemoved(store1));
  ASSERT_TRUE(IsRemoved(store2));
  ASSERT_TRUE(IsRemoved(store3));
}

// Loop writes invalidate only possibly aliased heap locations.
TEST_F(LoadStoreEliminationTest, StoreAfterLoopWithSideEffects2) {
  CreateTestControlFlowGraph();

  // Add another array parameter that may alias with `array_`.
  // Note: We're not adding it to the suspend check environment.
  HInstruction* array2 = MakeParam(DataType::Type::kInt32);

  HInstruction* c0 = graph_->GetIntConstant(0);
  HInstruction* c2 = graph_->GetIntConstant(2);

  // array[0] = 2;
  // loop:
  //   array2[i] = array[i]
  // array[0] = 2
  HInstruction* store1 = AddArraySet(pre_header_, array_, c0, c2);

  HInstruction* load = AddArrayGet(loop_, array_, phi_);
  HInstruction* store2 = AddArraySet(loop_, array2, phi_, load);

  HInstruction* store3 = AddArraySet(return_block_, array_, c0, c2);

  PerformLSE();

  ASSERT_FALSE(IsRemoved(store1));
  ASSERT_FALSE(IsRemoved(store2));
  ASSERT_FALSE(IsRemoved(store3));
}

// As it is not allowed to use defaults for VecLoads, check if there is a new created array
// a VecLoad used in a loop and after it is not replaced with a default.
TEST_F(LoadStoreEliminationTest, VLoadDefaultValueInLoopWithoutWriteSideEffects) {
  CreateTestControlFlowGraph();

  HInstruction* c0 = graph_->GetIntConstant(0);
  HInstruction* c128 = graph_->GetIntConstant(128);

  HInstruction* array_a = new (GetAllocator()) HNewArray(c0, c128, 0, 0);
  pre_header_->InsertInstructionBefore(array_a, pre_header_->GetLastInstruction());
  array_a->CopyEnvironmentFrom(suspend_check_->GetEnvironment());

  // LOOP BODY:
  //    v = a[i,... i + 3]
  // array[0,... 3] = v
  HInstruction* vload = AddVecLoad(loop_, array_a, phi_);
  HInstruction* vstore = AddVecStore(return_block_, array_, c0, vload);

  // TODO: enable LSE for graphs with predicated SIMD.
  graph_->SetHasTraditionalSIMD(true);
  PerformLSE();

  ASSERT_FALSE(IsRemoved(vload));
  ASSERT_FALSE(IsRemoved(vstore));
}

// As it is not allowed to use defaults for VecLoads, check if there is a new created array
// a VecLoad is not replaced with a default.
TEST_F(LoadStoreEliminationTest, VLoadDefaultValue) {
  CreateTestControlFlowGraph();

  HInstruction* c0 = graph_->GetIntConstant(0);
  HInstruction* c128 = graph_->GetIntConstant(128);

  HInstruction* array_a = new (GetAllocator()) HNewArray(c0, c128, 0, 0);
  pre_header_->InsertInstructionBefore(array_a, pre_header_->GetLastInstruction());
  array_a->CopyEnvironmentFrom(suspend_check_->GetEnvironment());

  // v = a[0,... 3]
  // array[0,... 3] = v
  HInstruction* vload = AddVecLoad(pre_header_, array_a, c0);
  HInstruction* vstore = AddVecStore(return_block_, array_, c0, vload);

  // TODO: enable LSE for graphs with predicated SIMD.
  graph_->SetHasTraditionalSIMD(true);
  PerformLSE();

  ASSERT_FALSE(IsRemoved(vload));
  ASSERT_FALSE(IsRemoved(vstore));
}

// As it is allowed to use defaults for ordinary loads, check if there is a new created array
// a load used in a loop and after it is replaced with a default.
TEST_F(LoadStoreEliminationTest, LoadDefaultValueInLoopWithoutWriteSideEffects) {
  CreateTestControlFlowGraph();

  HInstruction* c0 = graph_->GetIntConstant(0);
  HInstruction* c128 = graph_->GetIntConstant(128);

  HInstruction* array_a = new (GetAllocator()) HNewArray(c0, c128, 0, 0);
  pre_header_->InsertInstructionBefore(array_a, pre_header_->GetLastInstruction());
  array_a->CopyEnvironmentFrom(suspend_check_->GetEnvironment());

  // LOOP BODY:
  //    v = a[i]
  // array[0] = v
  HInstruction* load = AddArrayGet(loop_, array_a, phi_);
  HInstruction* store = AddArraySet(return_block_, array_, c0, load);

  PerformLSE();

  ASSERT_TRUE(IsRemoved(load));
  ASSERT_FALSE(IsRemoved(store));
}

// As it is allowed to use defaults for ordinary loads, check if there is a new created array
// a load is replaced with a default.
TEST_F(LoadStoreEliminationTest, LoadDefaultValue) {
  CreateTestControlFlowGraph();

  HInstruction* c0 = graph_->GetIntConstant(0);
  HInstruction* c128 = graph_->GetIntConstant(128);

  HInstruction* array_a = new (GetAllocator()) HNewArray(c0, c128, 0, 0);
  pre_header_->InsertInstructionBefore(array_a, pre_header_->GetLastInstruction());
  array_a->CopyEnvironmentFrom(suspend_check_->GetEnvironment());

  // v = a[0]
  // array[0] = v
  HInstruction* load = AddArrayGet(pre_header_, array_a, c0);
  HInstruction* store = AddArraySet(return_block_, array_, c0, load);

  PerformLSE();

  ASSERT_TRUE(IsRemoved(load));
  ASSERT_FALSE(IsRemoved(store));
}

// As it is not allowed to use defaults for VecLoads but allowed for regular loads,
// check if there is a new created array, a VecLoad and a load used in a loop and after it,
// VecLoad is not replaced with a default but the load is.
TEST_F(LoadStoreEliminationTest, VLoadAndLoadDefaultValueInLoopWithoutWriteSideEffects) {
  CreateTestControlFlowGraph();

  HInstruction* c0 = graph_->GetIntConstant(0);
  HInstruction* c128 = graph_->GetIntConstant(128);

  HInstruction* array_a = new (GetAllocator()) HNewArray(c0, c128, 0, 0);
  pre_header_->InsertInstructionBefore(array_a, pre_header_->GetLastInstruction());
  array_a->CopyEnvironmentFrom(suspend_check_->GetEnvironment());

  // LOOP BODY:
  //    v = a[i,... i + 3]
  //    v1 = a[i]
  // array[0,... 3] = v
  // array[0] = v1
  HInstruction* vload = AddVecLoad(loop_, array_a, phi_);
  HInstruction* load = AddArrayGet(loop_, array_a, phi_);
  HInstruction* vstore = AddVecStore(return_block_, array_, c0, vload);
  HInstruction* store = AddArraySet(return_block_, array_, c0, load);

  // TODO: enable LSE for graphs with predicated SIMD.
  graph_->SetHasTraditionalSIMD(true);
  PerformLSE();

  ASSERT_FALSE(IsRemoved(vload));
  ASSERT_TRUE(IsRemoved(load));
  ASSERT_FALSE(IsRemoved(vstore));
  ASSERT_FALSE(IsRemoved(store));
}

// As it is not allowed to use defaults for VecLoads but allowed for regular loads,
// check if there is a new created array, a VecLoad and a load,
// VecLoad is not replaced with a default but the load is.
TEST_F(LoadStoreEliminationTest, VLoadAndLoadDefaultValue) {
  CreateTestControlFlowGraph();

  HInstruction* c0 = graph_->GetIntConstant(0);
  HInstruction* c128 = graph_->GetIntConstant(128);

  HInstruction* array_a = new (GetAllocator()) HNewArray(c0, c128, 0, 0);
  pre_header_->InsertInstructionBefore(array_a, pre_header_->GetLastInstruction());
  array_a->CopyEnvironmentFrom(suspend_check_->GetEnvironment());

  // v = a[0,... 3]
  // v1 = a[0]
  // array[0,... 3] = v
  // array[0] = v1
  HInstruction* vload = AddVecLoad(pre_header_, array_a, c0);
  HInstruction* load = AddArrayGet(pre_header_, array_a, c0);
  HInstruction* vstore = AddVecStore(return_block_, array_, c0, vload);
  HInstruction* store = AddArraySet(return_block_, array_, c0, load);

  // TODO: enable LSE for graphs with predicated SIMD.
  graph_->SetHasTraditionalSIMD(true);
  PerformLSE();

  ASSERT_FALSE(IsRemoved(vload));
  ASSERT_TRUE(IsRemoved(load));
  ASSERT_FALSE(IsRemoved(vstore));
  ASSERT_FALSE(IsRemoved(store));
}

// It is not allowed to use defaults for VecLoads. However it should not prevent from removing
// loads getting the same value.
// Check a load getting a known value is eliminated (a loop test case).
TEST_F(LoadStoreEliminationTest, VLoadDefaultValueAndVLoadInLoopWithoutWriteSideEffects) {
  CreateTestControlFlowGraph();

  HInstruction* c0 = graph_->GetIntConstant(0);
  HInstruction* c128 = graph_->GetIntConstant(128);

  HInstruction* array_a = new (GetAllocator()) HNewArray(c0, c128, 0, 0);
  pre_header_->InsertInstructionBefore(array_a, pre_header_->GetLastInstruction());
  array_a->CopyEnvironmentFrom(suspend_check_->GetEnvironment());

  // LOOP BODY:
  //    v = a[i,... i + 3]
  //    v1 = a[i,... i + 3]
  // array[0,... 3] = v
  // array[128,... 131] = v1
  HInstruction* vload1 = AddVecLoad(loop_, array_a, phi_);
  HInstruction* vload2 = AddVecLoad(loop_, array_a, phi_);
  HInstruction* vstore1 = AddVecStore(return_block_, array_, c0, vload1);
  HInstruction* vstore2 = AddVecStore(return_block_, array_, c128, vload2);

  // TODO: enable LSE for graphs with predicated SIMD.
  graph_->SetHasTraditionalSIMD(true);
  PerformLSE();

  ASSERT_FALSE(IsRemoved(vload1));
  ASSERT_TRUE(IsRemoved(vload2));
  ASSERT_FALSE(IsRemoved(vstore1));
  ASSERT_FALSE(IsRemoved(vstore2));
}

// It is not allowed to use defaults for VecLoads. However it should not prevent from removing
// loads getting the same value.
// Check a load getting a known value is eliminated.
TEST_F(LoadStoreEliminationTest, VLoadDefaultValueAndVLoad) {
  CreateTestControlFlowGraph();

  HInstruction* c0 = graph_->GetIntConstant(0);
  HInstruction* c128 = graph_->GetIntConstant(128);

  HInstruction* array_a = new (GetAllocator()) HNewArray(c0, c128, 0, 0);
  pre_header_->InsertInstructionBefore(array_a, pre_header_->GetLastInstruction());
  array_a->CopyEnvironmentFrom(suspend_check_->GetEnvironment());

  // v = a[0,... 3]
  // v1 = a[0,... 3]
  // array[0,... 3] = v
  // array[128,... 131] = v1
  HInstruction* vload1 = AddVecLoad(pre_header_, array_a, c0);
  HInstruction* vload2 = AddVecLoad(pre_header_, array_a, c0);
  HInstruction* vstore1 = AddVecStore(return_block_, array_, c0, vload1);
  HInstruction* vstore2 = AddVecStore(return_block_, array_, c128, vload2);

  // TODO: enable LSE for graphs with predicated SIMD.
  graph_->SetHasTraditionalSIMD(true);
  PerformLSE();

  ASSERT_FALSE(IsRemoved(vload1));
  ASSERT_TRUE(IsRemoved(vload2));
  ASSERT_FALSE(IsRemoved(vstore1));
  ASSERT_FALSE(IsRemoved(vstore2));
}

// Object o = new Obj();
// // Needed because otherwise we short-circuit LSA since GVN would get almost
// // everything other than this. Also since this isn't expected to be a very
// // common pattern it's not worth changing the LSA logic.
// o.foo = 3;
// return o.shadow$_klass_;
TEST_F(LoadStoreEliminationTest, DefaultShadowClass) {
  CreateGraph();
  AdjacencyListGraph blocks(
      graph_, GetAllocator(), "entry", "exit", {{"entry", "main"}, {"main", "exit"}});
#define GET_BLOCK(name) HBasicBlock* name = blocks.Get(#name)
  GET_BLOCK(entry);
  GET_BLOCK(main);
  GET_BLOCK(exit);
#undef GET_BLOCK

  HInstruction* suspend_check = MakeSuspendCheck(entry);
  MakeGoto(entry);
  ManuallyBuildEnvFor(suspend_check, {});

  HInstruction* cls = MakeLoadClass(main);
  HInstruction* new_inst = MakeNewInstance(main, cls);
  HInstruction* const_fence = new (GetAllocator()) HConstructorFence(new_inst, 0, GetAllocator());
  main->AddInstruction(const_fence);
  HInstruction* set_field =
      MakeIFieldSet(main, new_inst, graph_->GetIntConstant(33), MemberOffset(32));
  HInstruction* get_field =
      MakeIFieldGet(main, new_inst, DataType::Type::kReference, mirror::Object::ClassOffset());
  HReturn* return_val = MakeReturn(main, get_field);
  cls->CopyEnvironmentFrom(suspend_check->GetEnvironment());
  new_inst->CopyEnvironmentFrom(suspend_check->GetEnvironment());

  MakeExit(exit);

  graph_->ClearDominanceInformation();
  PerformLSE();

  EXPECT_INS_REMOVED(new_inst);
  EXPECT_INS_REMOVED(const_fence);
  EXPECT_INS_REMOVED(get_field);
  EXPECT_INS_REMOVED(set_field);
  EXPECT_INS_RETAINED(cls);
  EXPECT_INS_EQ(cls, return_val->InputAt(0));
}

// Object o = new Obj();
// // Needed because otherwise we short-circuit LSA since GVN would get almost
// // everything other than this. Also since this isn't expected to be a very
// // common pattern (only a single java function, Object.identityHashCode,
// // ever reads this field) it's not worth changing the LSA logic.
// o.foo = 3;
// return o.shadow$_monitor_;
TEST_F(LoadStoreEliminationTest, DefaultShadowMonitor) {
  CreateGraph();
  AdjacencyListGraph blocks(
      graph_, GetAllocator(), "entry", "exit", {{"entry", "main"}, {"main", "exit"}});
#define GET_BLOCK(name) HBasicBlock* name = blocks.Get(#name)
  GET_BLOCK(entry);
  GET_BLOCK(main);
  GET_BLOCK(exit);
#undef GET_BLOCK

  HInstruction* suspend_check = MakeSuspendCheck(entry);
  MakeGoto(entry);
  ManuallyBuildEnvFor(suspend_check, {});

  HInstruction* cls = MakeLoadClass(main);
  HInstruction* new_inst = MakeNewInstance(main, cls);
  HInstruction* const_fence = new (GetAllocator()) HConstructorFence(new_inst, 0, GetAllocator());
  main->AddInstruction(const_fence);
  HInstruction* set_field =
      MakeIFieldSet(main, new_inst, graph_->GetIntConstant(33), MemberOffset(32));
  HInstruction* get_field =
      MakeIFieldGet(main, new_inst, DataType::Type::kInt32, mirror::Object::MonitorOffset());
  HReturn* return_val = MakeReturn(main, get_field);
  cls->CopyEnvironmentFrom(suspend_check->GetEnvironment());
  new_inst->CopyEnvironmentFrom(suspend_check->GetEnvironment());

  MakeExit(exit);

  graph_->ClearDominanceInformation();
  PerformLSE();

  EXPECT_INS_REMOVED(new_inst);
  EXPECT_INS_REMOVED(const_fence);
  EXPECT_INS_REMOVED(get_field);
  EXPECT_INS_REMOVED(set_field);
  EXPECT_INS_RETAINED(cls);
  EXPECT_INS_EQ(graph_->GetIntConstant(0), return_val->InputAt(0));
}

// void DO_CAL() {
//   int i = 1;
//   int[] w = new int[80];
//   int t = 0;
//   while (i < 80) {
//     w[i] = PLEASE_INTERLEAVE(w[i - 1], 1)
//     t = PLEASE_SELECT(w[i], t);
//     i++;
//   }
//   return t;
// }
TEST_F(LoadStoreEliminationTest, ArrayLoopOverlap) {
  ScopedObjectAccess soa(Thread::Current());
  VariableSizedHandleScope vshs(soa.Self());
  CreateGraph(&vshs);
  AdjacencyListGraph blocks(graph_,
                            GetAllocator(),
                            "entry",
                            "exit",
                            { { "entry", "loop_pre_header" },
                              { "loop_pre_header", "loop_entry" },
                              { "loop_entry", "loop_body" },
                              { "loop_entry", "loop_post" },
                              { "loop_body", "loop_entry" },
                              { "loop_post", "exit" } });
#define GET_BLOCK(name) HBasicBlock* name = blocks.Get(#name)
  GET_BLOCK(entry);
  GET_BLOCK(loop_pre_header);
  GET_BLOCK(loop_entry);
  GET_BLOCK(loop_body);
  GET_BLOCK(loop_post);
  GET_BLOCK(exit);
#undef GET_BLOCK

  HInstruction* zero_const = graph_->GetConstant(DataType::Type::kInt32, 0);
  HInstruction* one_const = graph_->GetConstant(DataType::Type::kInt32, 1);
  HInstruction* eighty_const = graph_->GetConstant(DataType::Type::kInt32, 80);
  MakeGoto(entry);

  HInstruction* alloc_w = MakeNewArray(loop_pre_header, zero_const, eighty_const);
  MakeGoto(loop_pre_header);
  // environment
  ManuallyBuildEnvFor(alloc_w, {});

  // loop-start
  HPhi* i_phi = MakePhi(loop_entry, {one_const, /* placeholder */ one_const});
  HPhi* t_phi = MakePhi(loop_entry, {zero_const, /* placeholder */ zero_const});
  HInstruction* suspend = MakeSuspendCheck(loop_entry);
  HInstruction* i_cmp_top = MakeCondition<HGreaterThanOrEqual>(loop_entry, i_phi, eighty_const);
  MakeIf(loop_entry, i_cmp_top);
  CHECK_EQ(loop_entry->GetSuccessors().size(), 2u);
  if (loop_entry->GetNormalSuccessors()[1] != loop_body) {
    loop_entry->SwapSuccessors();
  }
  CHECK_EQ(loop_entry->GetPredecessors().size(), 2u);
  if (loop_entry->GetPredecessors()[0] != loop_pre_header) {
    loop_entry->SwapPredecessors();
  }

  // environment
  ManuallyBuildEnvFor(suspend, { alloc_w, i_phi, t_phi });

  // BODY
  HInstruction* last_i = MakeBinOp<HSub>(loop_body, DataType::Type::kInt32, i_phi, one_const);
  HInstruction* last_get = MakeArrayGet(loop_body, alloc_w, last_i, DataType::Type::kInt32);
  HInvoke* body_value =
      MakeInvokeStatic(loop_body, DataType::Type::kInt32, { last_get, one_const });
  HInstruction* body_set =
      MakeArraySet(loop_body, alloc_w, i_phi, body_value, DataType::Type::kInt32);
  HInstruction* body_get = MakeArrayGet(loop_body, alloc_w, i_phi, DataType::Type::kInt32);
  HInvoke* t_next = MakeInvokeStatic(loop_body, DataType::Type::kInt32, { body_get, t_phi });
  HInstruction* i_next = MakeBinOp<HAdd>(loop_body, DataType::Type::kInt32, i_phi, one_const);
  MakeGoto(loop_body);
  body_value->CopyEnvironmentFrom(suspend->GetEnvironment());

  i_phi->ReplaceInput(i_next, 1u);  // Update back-edge input.
  t_phi->ReplaceInput(t_next, 1u);  // Update back-edge input.
  t_next->CopyEnvironmentFrom(suspend->GetEnvironment());

  // loop-post
  MakeReturn(loop_post, t_phi);

  // exit
  MakeExit(exit);

  graph_->ClearDominanceInformation();
  graph_->ClearLoopInformation();
  PerformLSE();

  // TODO Technically this is optimizable. LSE just needs to add phis to keep
  // track of the last `N` values set where `N` is how many locations we can go
  // back into the array.
  if (IsRemoved(last_get)) {
    // If we were able to remove the previous read the entire array should be removable.
    EXPECT_INS_REMOVED(body_set);
    EXPECT_INS_REMOVED(alloc_w);
  } else {
    // This is the branch we actually take for now. If we rely on being able to
    // read the array we'd better remember to write to it as well.
    EXPECT_INS_RETAINED(body_set);
  }
  // The last 'get' should always be removable.
  EXPECT_INS_REMOVED(body_get);
}

// void DO_CAL2() {
//   int i = 1;
//   int[] w = new int[80];
//   int t = 0;
//   while (i < 80) {
//     w[i] = PLEASE_INTERLEAVE(w[i - 1], 1) // <-- removed
//     t = PLEASE_SELECT(w[i], t);
//     w[i] = PLEASE_INTERLEAVE(w[i - 1], 1) // <-- removed
//     t = PLEASE_SELECT(w[i], t);
//     w[i] = PLEASE_INTERLEAVE(w[i - 1], 1) // <-- kept
//     t = PLEASE_SELECT(w[i], t);
//     i++;
//   }
//   return t;
// }
TEST_F(LoadStoreEliminationTest, ArrayLoopOverlap2) {
  ScopedObjectAccess soa(Thread::Current());
  VariableSizedHandleScope vshs(soa.Self());
  CreateGraph(&vshs);
  AdjacencyListGraph blocks(graph_,
                            GetAllocator(),
                            "entry",
                            "exit",
                            { { "entry", "loop_pre_header" },
                              { "loop_pre_header", "loop_entry" },
                              { "loop_entry", "loop_body" },
                              { "loop_entry", "loop_post" },
                              { "loop_body", "loop_entry" },
                              { "loop_post", "exit" } });
#define GET_BLOCK(name) HBasicBlock* name = blocks.Get(#name)
  GET_BLOCK(entry);
  GET_BLOCK(loop_pre_header);
  GET_BLOCK(loop_entry);
  GET_BLOCK(loop_body);
  GET_BLOCK(loop_post);
  GET_BLOCK(exit);
#undef GET_BLOCK

  HInstruction* zero_const = graph_->GetConstant(DataType::Type::kInt32, 0);
  HInstruction* one_const = graph_->GetConstant(DataType::Type::kInt32, 1);
  HInstruction* eighty_const = graph_->GetConstant(DataType::Type::kInt32, 80);
  MakeGoto(entry);

  HInstruction* alloc_w = MakeNewArray(loop_pre_header, zero_const, eighty_const);
  MakeGoto(loop_pre_header);
  // environment
  ManuallyBuildEnvFor(alloc_w, {});

  // loop-start
  HPhi* i_phi = MakePhi(loop_entry, {one_const, /* placeholder */ one_const});
  HPhi* t_phi = MakePhi(loop_entry, {zero_const, /* placeholder */ zero_const});
  HInstruction* suspend = MakeSuspendCheck(loop_entry);
  HInstruction* i_cmp_top = MakeCondition<HGreaterThanOrEqual>(loop_entry, i_phi, eighty_const);
  MakeIf(loop_entry, i_cmp_top);
  CHECK_EQ(loop_entry->GetSuccessors().size(), 2u);
  if (loop_entry->GetNormalSuccessors()[1] != loop_body) {
    loop_entry->SwapSuccessors();
  }
  CHECK_EQ(loop_entry->GetPredecessors().size(), 2u);
  if (loop_entry->GetPredecessors()[0] != loop_pre_header) {
    loop_entry->SwapPredecessors();
  }

  // environment
  ManuallyBuildEnvFor(suspend, { alloc_w, i_phi, t_phi });

  // BODY
  HInstruction* last_i = MakeBinOp<HSub>(loop_body, DataType::Type::kInt32, i_phi, one_const);
  auto make_instructions = [&](HInstruction* last_t_value) {
    HInstruction* last_get = MakeArrayGet(loop_body, alloc_w, last_i, DataType::Type::kInt32);
    HInvoke* body_value =
       MakeInvokeStatic(loop_body, DataType::Type::kInt32, { last_get, one_const });
    HInstruction* body_set =
        MakeArraySet(loop_body, alloc_w, i_phi, body_value, DataType::Type::kInt32);
    HInstruction* body_get = MakeArrayGet(loop_body, alloc_w, i_phi, DataType::Type::kInt32);
    HInvoke* t_next =
        MakeInvokeStatic(loop_body, DataType::Type::kInt32, { body_get, last_t_value });
    return std::make_tuple(last_get, body_value, body_set, body_get, t_next);
  };
  auto [last_get_1, body_value_1, body_set_1, body_get_1, t_next_1] = make_instructions(t_phi);
  auto [last_get_2, body_value_2, body_set_2, body_get_2, t_next_2] = make_instructions(t_next_1);
  auto [last_get_3, body_value_3, body_set_3, body_get_3, t_next_3] = make_instructions(t_next_2);
  HInstruction* i_next = MakeBinOp<HAdd>(loop_body, DataType::Type::kInt32, i_phi, one_const);
  MakeGoto(loop_body);
  body_value_1->CopyEnvironmentFrom(suspend->GetEnvironment());
  body_value_2->CopyEnvironmentFrom(suspend->GetEnvironment());
  body_value_3->CopyEnvironmentFrom(suspend->GetEnvironment());

  i_phi->ReplaceInput(i_next, 1u);  // Update back-edge input.
  t_phi->ReplaceInput(t_next_3, 1u);  // Update back-edge input.
  t_next_1->CopyEnvironmentFrom(suspend->GetEnvironment());
  t_next_2->CopyEnvironmentFrom(suspend->GetEnvironment());
  t_next_3->CopyEnvironmentFrom(suspend->GetEnvironment());

  // loop-post
  MakeReturn(loop_post, t_phi);

  // exit
  MakeExit(exit);

  graph_->ClearDominanceInformation();
  graph_->ClearLoopInformation();
  PerformLSE();

  // TODO Technically this is optimizable. LSE just needs to add phis to keep
  // track of the last `N` values set where `N` is how many locations we can go
  // back into the array.
  if (IsRemoved(last_get_1)) {
    // If we were able to remove the previous read the entire array should be removable.
    EXPECT_INS_REMOVED(body_set_1);
    EXPECT_INS_REMOVED(body_set_2);
    EXPECT_INS_REMOVED(body_set_3);
    EXPECT_INS_REMOVED(last_get_1);
    EXPECT_INS_REMOVED(last_get_2);
    EXPECT_INS_REMOVED(alloc_w);
  } else {
    // This is the branch we actually take for now. If we rely on being able to
    // read the array we'd better remember to write to it as well.
    EXPECT_INS_RETAINED(body_set_3);
  }
  // The last 'get' should always be removable.
  EXPECT_INS_REMOVED(body_get_1);
  EXPECT_INS_REMOVED(body_get_2);
  EXPECT_INS_REMOVED(body_get_3);
  // shadowed writes should always be removed
  EXPECT_INS_REMOVED(body_set_1);
  EXPECT_INS_REMOVED(body_set_2);
}

TEST_F(LoadStoreEliminationTest, ArrayNonLoopPhi) {
  ScopedObjectAccess soa(Thread::Current());
  VariableSizedHandleScope vshs(soa.Self());
  CreateGraph(&vshs);
  AdjacencyListGraph blocks(graph_,
                            GetAllocator(),
                            "entry",
                            "exit",
                            { { "entry", "start" },
                              { "start", "left" },
                              { "start", "right" },
                              { "left", "ret" },
                              { "right", "ret" },
                              { "ret", "exit" } });
#define GET_BLOCK(name) HBasicBlock* name = blocks.Get(#name)
  GET_BLOCK(entry);
  GET_BLOCK(start);
  GET_BLOCK(left);
  GET_BLOCK(right);
  GET_BLOCK(ret);
  GET_BLOCK(exit);
#undef GET_BLOCK

  HInstruction* zero_const = graph_->GetConstant(DataType::Type::kInt32, 0);
  HInstruction* one_const = graph_->GetConstant(DataType::Type::kInt32, 1);
  HInstruction* two_const = graph_->GetConstant(DataType::Type::kInt32, 2);
  HInstruction* param = MakeParam(DataType::Type::kBool);

  MakeGoto(entry);

  HInstruction* alloc_w = MakeNewArray(start, zero_const, two_const);
  MakeIf(start, param);
  // environment
  ManuallyBuildEnvFor(alloc_w, {});

  // left
  HInvoke* left_value = MakeInvokeStatic(left, DataType::Type::kInt32, { zero_const });
  HInstruction* left_set_1 =
      MakeArraySet(left, alloc_w, zero_const, left_value, DataType::Type::kInt32);
  HInstruction* left_set_2 =
      MakeArraySet(left, alloc_w, one_const, zero_const, DataType::Type::kInt32);
  MakeGoto(left);
  ManuallyBuildEnvFor(left_value, { alloc_w });

  // right
  HInvoke* right_value = MakeInvokeStatic(right, DataType::Type::kInt32, { one_const });
  HInstruction* right_set_1 =
      MakeArraySet(right, alloc_w, zero_const, right_value, DataType::Type::kInt32);
  HInstruction* right_set_2 =
      MakeArraySet(right, alloc_w, one_const, zero_const, DataType::Type::kInt32);
  MakeGoto(right);
  ManuallyBuildEnvFor(right_value, { alloc_w });

  // ret
  HInstruction* read_1 = MakeArrayGet(ret, alloc_w, zero_const, DataType::Type::kInt32);
  HInstruction* read_2 = MakeArrayGet(ret, alloc_w, one_const, DataType::Type::kInt32);
  HInstruction* add = MakeBinOp<HAdd>(ret, DataType::Type::kInt32, read_1, read_2);
  MakeReturn(ret, add);

  // exit
  MakeExit(exit);

  graph_->ClearDominanceInformation();
  graph_->ClearLoopInformation();
  PerformLSE();

  EXPECT_INS_REMOVED(read_1);
  EXPECT_INS_REMOVED(read_2);
  EXPECT_INS_REMOVED(left_set_1);
  EXPECT_INS_REMOVED(left_set_2);
  EXPECT_INS_REMOVED(right_set_1);
  EXPECT_INS_REMOVED(right_set_2);
  EXPECT_INS_REMOVED(alloc_w);

  EXPECT_INS_RETAINED(left_value);
  EXPECT_INS_RETAINED(right_value);
}

TEST_F(LoadStoreEliminationTest, ArrayMergeDefault) {
  ScopedObjectAccess soa(Thread::Current());
  VariableSizedHandleScope vshs(soa.Self());
  CreateGraph(&vshs);
  AdjacencyListGraph blocks(graph_,
                            GetAllocator(),
                            "entry",
                            "exit",
                            { { "entry", "start" },
                              { "start", "left" },
                              { "start", "right" },
                              { "left", "ret" },
                              { "right", "ret" },
                              { "ret", "exit" } });
#define GET_BLOCK(name) HBasicBlock* name = blocks.Get(#name)
  GET_BLOCK(entry);
  GET_BLOCK(start);
  GET_BLOCK(left);
  GET_BLOCK(right);
  GET_BLOCK(ret);
  GET_BLOCK(exit);
#undef GET_BLOCK

  HInstruction* zero_const = graph_->GetConstant(DataType::Type::kInt32, 0);
  HInstruction* one_const = graph_->GetConstant(DataType::Type::kInt32, 1);
  HInstruction* two_const = graph_->GetConstant(DataType::Type::kInt32, 2);
  HInstruction* param = MakeParam(DataType::Type::kBool);
  MakeGoto(entry);

  HInstruction* alloc_w = MakeNewArray(start, zero_const, two_const);
  MakeIf(start, param);
  // environment
  ArenaVector<HInstruction*> alloc_locals({}, GetAllocator()->Adapter(kArenaAllocInstruction));
  ManuallyBuildEnvFor(alloc_w, {});

  // left
  HInstruction* left_set_1 =
      MakeArraySet(left, alloc_w, zero_const, one_const, DataType::Type::kInt32);
  HInstruction* left_set_2 =
      MakeArraySet(left, alloc_w, zero_const, zero_const, DataType::Type::kInt32);
  MakeGoto(left);

  // right
  HInstruction* right_set_1 =
      MakeArraySet(right, alloc_w, one_const, one_const, DataType::Type::kInt32);
  HInstruction* right_set_2 =
      MakeArraySet(right, alloc_w, one_const, zero_const, DataType::Type::kInt32);
  MakeGoto(right);

  // ret
  HInstruction* read_1 = MakeArrayGet(ret, alloc_w, zero_const, DataType::Type::kInt32);
  HInstruction* read_2 = MakeArrayGet(ret, alloc_w, one_const, DataType::Type::kInt32);
  HInstruction* add = MakeBinOp<HAdd>(ret, DataType::Type::kInt32, read_1, read_2);
  MakeReturn(ret, add);

  // exit
  MakeExit(exit);

  graph_->ClearDominanceInformation();
  graph_->ClearLoopInformation();
  PerformLSE();

  EXPECT_INS_REMOVED(read_1);
  EXPECT_INS_REMOVED(read_2);
  EXPECT_INS_REMOVED(left_set_1);
  EXPECT_INS_REMOVED(left_set_2);
  EXPECT_INS_REMOVED(right_set_1);
  EXPECT_INS_REMOVED(right_set_2);
  EXPECT_INS_REMOVED(alloc_w);
}

// Regression test for b/187487955.
// We previusly failed to consider aliasing between an array location
// with index `idx` defined in the loop (such as a loop Phi) and another
// array location with index `idx + constant`. This could have led to
// replacing the load with, for example, the default value 0.
TEST_F(LoadStoreEliminationTest, ArrayLoopAliasing1) {
  ScopedObjectAccess soa(Thread::Current());
  VariableSizedHandleScope vshs(soa.Self());
  CreateGraph(&vshs);
  AdjacencyListGraph blocks(graph_,
                            GetAllocator(),
                            "entry",
                            "exit",
                            { { "entry", "preheader" },
                              { "preheader", "loop" },
                              { "loop", "body" },
                              { "body", "loop" },
                              { "loop", "ret" },
                              { "ret", "exit" } });
#define GET_BLOCK(name) HBasicBlock* name = blocks.Get(#name)
  GET_BLOCK(entry);
  GET_BLOCK(preheader);
  GET_BLOCK(loop);
  GET_BLOCK(body);
  GET_BLOCK(ret);
  GET_BLOCK(exit);
#undef GET_BLOCK
  HInstruction* n = MakeParam(DataType::Type::kInt32);
  HInstruction* c0 = graph_->GetIntConstant(0);
  HInstruction* c1 = graph_->GetIntConstant(1);

  // entry
  HInstruction* cls = MakeLoadClass(entry);
  HInstruction* array = MakeNewArray(entry, cls, n);
  MakeGoto(entry);
  ManuallyBuildEnvFor(cls, {});
  ManuallyBuildEnvFor(array, {});

  MakeGoto(preheader);

  // loop
  HPhi* i_phi = MakePhi(loop, {c0, /*placeholder*/ c0});
  HInstruction* loop_suspend_check = MakeSuspendCheck(loop);
  HInstruction* loop_cond = MakeCondition<HLessThan>(loop, i_phi, n);
  HIf* loop_if = MakeIf(loop, loop_cond);
  CHECK(loop_if->IfTrueSuccessor() == body);
  ManuallyBuildEnvFor(loop_suspend_check, {});

  // body
  HInstruction* body_set =
      MakeArraySet(body, array, i_phi, i_phi, DataType::Type::kInt32, /*dex_pc=*/ 0u);
  HInstruction* body_add = MakeBinOp<HAdd>(body, DataType::Type::kInt32, i_phi, c1);
  MakeGoto(body);

  // Update  `i_phi`'s back-edge input.
  i_phi->ReplaceInput(body_add, 1u);

  // ret
  HInstruction* ret_sub = MakeBinOp<HSub>(ret, DataType::Type::kInt32, i_phi, c1);
  HInstruction* ret_get = MakeArrayGet(ret, array, ret_sub, DataType::Type::kInt32);
  MakeReturn(ret, ret_get);

  // exit
  MakeExit(exit);

  graph_->ClearDominanceInformation();
  graph_->ClearLoopInformation();
  PerformLSE();

  EXPECT_INS_RETAINED(cls);
  EXPECT_INS_RETAINED(array);
  EXPECT_INS_RETAINED(body_set);
  EXPECT_INS_RETAINED(ret_get);
}

// Regression test for b/187487955.
// Similar to the `ArrayLoopAliasing1` test above but with additional load
// that marks a loop Phi placeholder as kept which used to trigger a DCHECK().
// There is also an LSE run-test for this but it relies on BCE eliminating
// BoundsCheck instructions and adds extra code in loop body to avoid
// loop unrolling. This gtest does not need to jump through those hoops
// as we do not unnecessarily run those optimization passes.
TEST_F(LoadStoreEliminationTest, ArrayLoopAliasing2) {
  ScopedObjectAccess soa(Thread::Current());
  VariableSizedHandleScope vshs(soa.Self());
  CreateGraph(&vshs);
  AdjacencyListGraph blocks(graph_,
                            GetAllocator(),
                            "entry",
                            "exit",
                            { { "entry", "preheader" },
                              { "preheader", "loop" },
                              { "loop", "body" },
                              { "body", "loop" },
                              { "loop", "ret" },
                              { "ret", "exit" } });
#define GET_BLOCK(name) HBasicBlock* name = blocks.Get(#name)
  GET_BLOCK(entry);
  GET_BLOCK(preheader);
  GET_BLOCK(loop);
  GET_BLOCK(body);
  GET_BLOCK(ret);
  GET_BLOCK(exit);
#undef GET_BLOCK
  HInstruction* n = MakeParam(DataType::Type::kInt32);
  HInstruction* c0 = graph_->GetIntConstant(0);
  HInstruction* c1 = graph_->GetIntConstant(1);

  // entry
  HInstruction* cls = MakeLoadClass(entry);
  HInstruction* array = MakeNewArray(entry, cls, n);
  MakeGoto(entry);
  ManuallyBuildEnvFor(cls, {});
  ManuallyBuildEnvFor(array, {});

  MakeGoto(preheader);

  // loop
  HPhi* i_phi = MakePhi(loop, {c0, /* placeholder */ c0});
  HInstruction* loop_suspend_check = MakeSuspendCheck(loop);
  HInstruction* loop_cond = MakeCondition<HLessThan>(loop, i_phi, n);
  HIf* loop_if = MakeIf(loop, loop_cond);
  CHECK(loop_if->IfTrueSuccessor() == body);
  ManuallyBuildEnvFor(loop_suspend_check, {});

  // body
  HInstruction* body_set = MakeArraySet(body, array, i_phi, i_phi, DataType::Type::kInt32);
  HInstruction* body_add = MakeBinOp<HAdd>(body, DataType::Type::kInt32, i_phi, c1);
  MakeGoto(body);

  // Update  `i_phi`'s back-edge input.
  i_phi->ReplaceInput(body_add, 1u);

  // ret
  HInstruction* ret_sub = MakeBinOp<HSub>(ret, DataType::Type::kInt32, i_phi, c1);
  HInstruction* ret_get1 = MakeArrayGet(ret, array, ret_sub, DataType::Type::kInt32);
  HInstruction* ret_get2 = MakeArrayGet(ret, array, i_phi, DataType::Type::kInt32);
  HInstruction* ret_add = MakeBinOp<HAdd>(ret, DataType::Type::kInt32, ret_get1, ret_get2);
  MakeReturn(ret, ret_add);

  // exit
  MakeExit(exit);

  graph_->ClearDominanceInformation();
  graph_->ClearLoopInformation();
  PerformLSE();

  EXPECT_INS_RETAINED(cls);
  EXPECT_INS_RETAINED(array);
  EXPECT_INS_RETAINED(body_set);
  EXPECT_INS_RETAINED(ret_get1);
  EXPECT_INS_RETAINED(ret_get2);
}

// // ENTRY
// obj = new Obj();
// // ALL should be kept
// switch (parameter_value) {
//   case 1:
//     // Case1
//     obj.field = 1;
//     call_func(obj);
//     break;
//   case 2:
//     // Case2
//     obj.field = 2;
//     call_func(obj);
//     // We don't know what obj.field is now we aren't able to eliminate the read below!
//     break;
//   default:
//     // Case3
//     // TODO This only happens because of limitations on our LSE which is unable
//     //      to materialize co-dependent loop and non-loop phis.
//     // Ideally we'd want to generate
//     // P1 = PHI[3, loop_val]
//     // while (test()) {
//     //   if (test2()) { goto; } else { goto; }
//     //   loop_val = [P1, 5]
//     // }
//     // Currently we aren't able to unfortunately.
//     obj.field = 3;
//     while (test()) {
//       if (test2()) { } else { obj.field = 5; }
//     }
//     break;
// }
// EXIT
// return obj.field
TEST_F(LoadStoreEliminationTest, PartialUnknownMerge) {
  CreateGraph();
  AdjacencyListGraph blks(SetupFromAdjacencyList("entry",
                                                 "exit",
                                                 {{"entry", "bswitch"},
                                                  {"bswitch", "case1"},
                                                  {"bswitch", "case2"},
                                                  {"bswitch", "case3"},
                                                  {"case1", "breturn"},
                                                  {"case2", "breturn"},
                                                  {"case3", "loop_pre_header"},
                                                  {"loop_pre_header", "loop_header"},
                                                  {"loop_header", "loop_body"},
                                                  {"loop_body", "loop_if_left"},
                                                  {"loop_body", "loop_if_right"},
                                                  {"loop_if_left", "loop_end"},
                                                  {"loop_if_right", "loop_end"},
                                                  {"loop_end", "loop_header"},
                                                  {"loop_header", "breturn"},
                                                  {"breturn", "exit"}}));
#define GET_BLOCK(name) HBasicBlock* name = blks.Get(#name)
  GET_BLOCK(entry);
  GET_BLOCK(bswitch);
  GET_BLOCK(exit);
  GET_BLOCK(breturn);
  GET_BLOCK(case1);
  GET_BLOCK(case2);
  GET_BLOCK(case3);

  GET_BLOCK(loop_pre_header);
  GET_BLOCK(loop_header);
  GET_BLOCK(loop_body);
  GET_BLOCK(loop_if_left);
  GET_BLOCK(loop_if_right);
  GET_BLOCK(loop_end);
#undef GET_BLOCK
  HInstruction* switch_val = MakeParam(DataType::Type::kInt32);
  HInstruction* c1 = graph_->GetIntConstant(1);
  HInstruction* c2 = graph_->GetIntConstant(2);
  HInstruction* c3 = graph_->GetIntConstant(3);
  HInstruction* c5 = graph_->GetIntConstant(5);

  HInstruction* cls = MakeLoadClass(entry);
  HInstruction* new_inst = MakeNewInstance(entry, cls);
  MakeGoto(entry);
  ManuallyBuildEnvFor(cls, {});
  new_inst->CopyEnvironmentFrom(cls->GetEnvironment());

  HInstruction* switch_inst = new (GetAllocator()) HPackedSwitch(0, 2, switch_val);
  bswitch->AddInstruction(switch_inst);

  HInstruction* write_c1 = MakeIFieldSet(case1, new_inst, c1, MemberOffset(32));
  HInstruction* call_c1 = MakeInvokeStatic(case1, DataType::Type::kVoid, { new_inst });
  MakeGoto(case1);
  call_c1->CopyEnvironmentFrom(cls->GetEnvironment());

  HInstruction* write_c2 = MakeIFieldSet(case2, new_inst, c2, MemberOffset(32));
  HInstruction* call_c2 = MakeInvokeStatic(case2, DataType::Type::kVoid, { new_inst });
  MakeGoto(case2);
  call_c2->CopyEnvironmentFrom(cls->GetEnvironment());

  HInstruction* write_c3 = MakeIFieldSet(case3, new_inst, c3, MemberOffset(32));
  MakeGoto(case3);

  MakeGoto(loop_pre_header);

  HInstruction* suspend_check_header = MakeSuspendCheck(loop_header);
  HInstruction* call_loop_header = MakeInvokeStatic(loop_header, DataType::Type::kBool, {});
  MakeIf(loop_header, call_loop_header);
  call_loop_header->CopyEnvironmentFrom(cls->GetEnvironment());
  suspend_check_header->CopyEnvironmentFrom(cls->GetEnvironment());

  HInstruction* call_loop_body = MakeInvokeStatic(loop_body, DataType::Type::kBool, {});
  MakeIf(loop_body, call_loop_body);
  call_loop_body->CopyEnvironmentFrom(cls->GetEnvironment());

  MakeGoto(loop_if_left);

  HInstruction* write_loop_right = MakeIFieldSet(loop_if_right, new_inst, c5, MemberOffset(32));
  MakeGoto(loop_if_right);

  MakeGoto(loop_end);

  HInstruction* read_bottom =
      MakeIFieldGet(breturn, new_inst, DataType::Type::kInt32, MemberOffset(32));
  MakeReturn(breturn, read_bottom);

  MakeExit(exit);

  PerformLSE(blks);

  EXPECT_INS_RETAINED(read_bottom);
  EXPECT_INS_RETAINED(write_c1);
  EXPECT_INS_RETAINED(write_c2);
  EXPECT_INS_RETAINED(write_c3);
  EXPECT_INS_RETAINED(write_loop_right);
}

// // ENTRY
// obj = new Obj();
// if (parameter_value) {
//   // LEFT
//   obj.field = 1;
//   call_func(obj);
//   // We don't know what obj.field is now we aren't able to eliminate the read below!
// } else {
//   // DO NOT ELIMINATE
//   obj.field = 2;
//   // RIGHT
// }
// EXIT
// return obj.field
// This test runs with partial LSE disabled.
TEST_F(LoadStoreEliminationTest, PartialLoadPreserved) {
  ScopedObjectAccess soa(Thread::Current());
  VariableSizedHandleScope vshs(soa.Self());
  CreateGraph(&vshs);
  AdjacencyListGraph blks(SetupFromAdjacencyList("entry",
                                                 "exit_REAL",
                                                 { { "entry", "left" },
                                                   { "entry", "right" },
                                                   { "left", "exit" },
                                                   { "right", "exit" },
                                                   { "exit", "exit_REAL" } }));
  HBasicBlock* entry = blks.Get("entry");
  HBasicBlock* left = blks.Get("left");
  HBasicBlock* right = blks.Get("right");
  HBasicBlock* exit = blks.Get("exit");
  HInstruction* bool_value = MakeParam(DataType::Type::kBool);
  HInstruction* c1 = graph_->GetIntConstant(1);
  HInstruction* c2 = graph_->GetIntConstant(2);

  HInstruction* cls = MakeLoadClass(entry);
  HInstruction* new_inst = MakeNewInstance(entry, cls);
  MakeIf(entry, bool_value);
  ManuallyBuildEnvFor(cls, {});
  new_inst->CopyEnvironmentFrom(cls->GetEnvironment());

  HInstruction* write_left = MakeIFieldSet(left, new_inst, c1, MemberOffset(32));
  HInstruction* call_left = MakeInvokeStatic(left, DataType::Type::kVoid, { new_inst });
  MakeGoto(left);
  call_left->CopyEnvironmentFrom(cls->GetEnvironment());

  HInstruction* write_right = MakeIFieldSet(right, new_inst, c2, MemberOffset(32));
  MakeGoto(right);

  HInstruction* read_bottom =
      MakeIFieldGet(exit, new_inst, DataType::Type::kInt32, MemberOffset(32));
  MakeReturn(exit, read_bottom);

  PerformLSE(blks);

  EXPECT_INS_RETAINED(read_bottom) << *read_bottom;
  EXPECT_INS_RETAINED(write_right) << *write_right;
}

// // ENTRY
// obj = new Obj();
// if (parameter_value) {
//   // LEFT
//   obj.field = 1;
//   call_func(obj);
//   // We don't know what obj.field is now we aren't able to eliminate the read below!
// } else {
//   // DO NOT ELIMINATE
//   if (param2) {
//     obj.field = 2;
//   } else {
//     obj.field = 3;
//   }
//   // RIGHT
// }
// EXIT
// return obj.field
// NB This test is for non-partial LSE flow. Normally the obj.field writes will be removed
TEST_F(LoadStoreEliminationTest, PartialLoadPreserved2) {
  ScopedObjectAccess soa(Thread::Current());
  VariableSizedHandleScope vshs(soa.Self());
  CreateGraph(&vshs);
  AdjacencyListGraph blks(SetupFromAdjacencyList("entry",
                                                 "exit_REAL",
                                                 { { "entry", "left" },
                                                   { "entry", "right_start" },
                                                   { "left", "exit" },
                                                   { "right_start", "right_first" },
                                                   { "right_start", "right_second" },
                                                   { "right_first", "right_end" },
                                                   { "right_second", "right_end" },
                                                   { "right_end", "exit" },
                                                   { "exit", "exit_REAL" } }));
  HBasicBlock* entry = blks.Get("entry");
  HBasicBlock* left = blks.Get("left");
  HBasicBlock* right_start = blks.Get("right_start");
  HBasicBlock* right_first = blks.Get("right_first");
  HBasicBlock* right_second = blks.Get("right_second");
  HBasicBlock* right_end = blks.Get("right_end");
  HBasicBlock* exit = blks.Get("exit");
  HInstruction* bool_value = MakeParam(DataType::Type::kBool);
  HInstruction* bool_value_2 = MakeParam(DataType::Type::kBool);
  HInstruction* c1 = graph_->GetIntConstant(1);
  HInstruction* c2 = graph_->GetIntConstant(2);
  HInstruction* c3 = graph_->GetIntConstant(3);

  HInstruction* cls = MakeLoadClass(entry);
  HInstruction* new_inst = MakeNewInstance(entry, cls);
  MakeIf(entry, bool_value);
  ManuallyBuildEnvFor(cls, {});
  new_inst->CopyEnvironmentFrom(cls->GetEnvironment());

  HInstruction* write_left = MakeIFieldSet(left, new_inst, c1, MemberOffset(32));
  HInstruction* call_left = MakeInvokeStatic(left, DataType::Type::kVoid, { new_inst });
  MakeGoto(left);
  call_left->CopyEnvironmentFrom(cls->GetEnvironment());

  MakeIf(right_start, bool_value_2);

  HInstruction* write_right_first = MakeIFieldSet(right_first, new_inst, c2, MemberOffset(32));
  MakeGoto(right_first);

  HInstruction* write_right_second = MakeIFieldSet(right_second, new_inst, c3, MemberOffset(32));
  MakeGoto(right_second);

  MakeGoto(right_end);

  HInstruction* read_bottom =
      MakeIFieldGet(exit, new_inst, DataType::Type::kInt32, MemberOffset(32));
  MakeReturn(exit, read_bottom);

  PerformLSE(blks);

  EXPECT_INS_RETAINED(read_bottom);
  EXPECT_INS_RETAINED(write_right_first);
  EXPECT_INS_RETAINED(write_right_second);
}

// // ENTRY
// obj = new Obj();
// if (parameter_value) {
//   // LEFT
//   // DO NOT ELIMINATE
//   obj.field = 1;
//   while (true) {
//     bool esc = escape(obj);
//     if (esc) break;
//     // DO NOT ELIMINATE
//     obj.field = 3;
//   }
// } else {
//   // RIGHT
//   // DO NOT ELIMINATE
//   obj.field = 2;
// }
// // DO NOT ELIMINATE
// return obj.field;
// EXIT
TEST_F(LoadStoreEliminationTest, PartialLoadPreserved3) {
  ScopedObjectAccess soa(Thread::Current());
  VariableSizedHandleScope vshs(soa.Self());
  CreateGraph(&vshs);
  AdjacencyListGraph blks(SetupFromAdjacencyList("entry",
                                                 "exit",
                                                 {{"entry", "entry_post"},
                                                  {"entry_post", "right"},
                                                  {"right", "return_block"},
                                                  {"entry_post", "left_pre"},
                                                  {"left_pre", "left_loop"},
                                                  {"left_loop", "left_loop_post"},
                                                  {"left_loop_post", "left_loop"},
                                                  {"left_loop", "return_block"},
                                                  {"return_block", "exit"}}));
#define GET_BLOCK(name) HBasicBlock* name = blks.Get(#name)
  GET_BLOCK(entry);
  GET_BLOCK(entry_post);
  GET_BLOCK(exit);
  GET_BLOCK(return_block);
  GET_BLOCK(left_pre);
  GET_BLOCK(left_loop);
  GET_BLOCK(left_loop_post);
  GET_BLOCK(right);
#undef GET_BLOCK
  // Left-loops first successor is the break.
  if (left_loop->GetSuccessors()[0] != return_block) {
    left_loop->SwapSuccessors();
  }
  HInstruction* bool_value = MakeParam(DataType::Type::kBool);
  HInstruction* c1 = graph_->GetIntConstant(1);
  HInstruction* c2 = graph_->GetIntConstant(2);
  HInstruction* c3 = graph_->GetIntConstant(3);

  HInstruction* cls = MakeLoadClass(entry);
  HInstruction* new_inst = MakeNewInstance(entry, cls);
  MakeGoto(entry);
  ManuallyBuildEnvFor(cls, {});
  new_inst->CopyEnvironmentFrom(cls->GetEnvironment());

  MakeIf(entry_post, bool_value);

  HInstruction* write_left_pre = MakeIFieldSet(left_pre, new_inst, c1, MemberOffset(32));
  MakeGoto(left_pre);

  HInstruction* suspend_left_loop = MakeSuspendCheck(left_loop);
  HInstruction* call_left_loop = MakeInvokeStatic(left_loop, DataType::Type::kBool, {new_inst});
  MakeIf(left_loop, call_left_loop);
  suspend_left_loop->CopyEnvironmentFrom(cls->GetEnvironment());
  call_left_loop->CopyEnvironmentFrom(cls->GetEnvironment());

  HInstruction* write_left_loop = MakeIFieldSet(left_loop_post, new_inst, c3, MemberOffset(32));
  MakeGoto(left_loop_post);

  HInstruction* write_right = MakeIFieldSet(right, new_inst, c2, MemberOffset(32));
  MakeGoto(right);

  HInstruction* read_return =
      MakeIFieldGet(return_block, new_inst, DataType::Type::kInt32, MemberOffset(32));
  MakeReturn(return_block, read_return);

  MakeExit(exit);

  PerformLSE(blks);

  EXPECT_INS_RETAINED(write_left_pre) << *write_left_pre;
  EXPECT_INS_RETAINED(read_return) << *read_return;
  EXPECT_INS_RETAINED(write_right) << *write_right;
  EXPECT_INS_RETAINED(write_left_loop) << *write_left_loop;
  EXPECT_INS_RETAINED(call_left_loop) << *call_left_loop;
}

// // ENTRY
// obj = new Obj();
// if (parameter_value) {
//   // LEFT
//   // ELIMINATE (not visible since always overridden by obj.field = 3)
//   obj.field = 1;
//   while (true) {
//     bool stop = should_stop();
//     // DO NOT ELIMINATE (visible by read at end)
//     obj.field = 3;
//     if (stop) break;
//   }
// } else {
//   // RIGHT
//   // DO NOT ELIMINATE
//   obj.field = 2;
//   escape(obj);
// }
// // DO NOT ELIMINATE
// return obj.field;
// EXIT
// Disabled due to b/205813546.
TEST_F(LoadStoreEliminationTest, DISABLED_PartialLoadPreserved4) {
  ScopedObjectAccess soa(Thread::Current());
  VariableSizedHandleScope vshs(soa.Self());
  CreateGraph(&vshs);
  AdjacencyListGraph blks(SetupFromAdjacencyList("entry",
                                                 "exit",
                                                 {{"entry", "entry_post"},
                                                  {"entry_post", "right"},
                                                  {"right", "return_block"},
                                                  {"entry_post", "left_pre"},
                                                  {"left_pre", "left_loop"},
                                                  {"left_loop", "left_loop"},
                                                  {"left_loop", "return_block"},
                                                  {"return_block", "exit"}}));
#define GET_BLOCK(name) HBasicBlock* name = blks.Get(#name)
  GET_BLOCK(entry);
  GET_BLOCK(entry_post);
  GET_BLOCK(exit);
  GET_BLOCK(return_block);
  GET_BLOCK(left_pre);
  GET_BLOCK(left_loop);
  GET_BLOCK(right);
#undef GET_BLOCK
  // Left-loops first successor is the break.
  if (left_loop->GetSuccessors()[0] != return_block) {
    left_loop->SwapSuccessors();
  }
  HInstruction* bool_value = MakeParam(DataType::Type::kBool);
  HInstruction* c1 = graph_->GetIntConstant(1);
  HInstruction* c2 = graph_->GetIntConstant(2);
  HInstruction* c3 = graph_->GetIntConstant(3);

  HInstruction* cls = MakeLoadClass(entry);
  HInstruction* new_inst = MakeNewInstance(entry, cls);
  MakeGoto(entry);
  ManuallyBuildEnvFor(cls, {});
  new_inst->CopyEnvironmentFrom(cls->GetEnvironment());

  MakeIf(entry_post, bool_value);

  HInstruction* write_left_pre = MakeIFieldSet(left_pre, new_inst, c1, MemberOffset(32));
  MakeGoto(left_pre);

  HInstruction* suspend_left_loop = MakeSuspendCheck(left_loop);
  HInstruction* call_left_loop = MakeInvokeStatic(left_loop, DataType::Type::kBool, {});
  HInstruction* write_left_loop = MakeIFieldSet(left_loop, new_inst, c3, MemberOffset(32));
  MakeIf(left_loop, call_left_loop);
  suspend_left_loop->CopyEnvironmentFrom(cls->GetEnvironment());
  call_left_loop->CopyEnvironmentFrom(cls->GetEnvironment());

  HInstruction* write_right = MakeIFieldSet(right, new_inst, c2, MemberOffset(32));
  HInstruction* call_right = MakeInvokeStatic(right, DataType::Type::kBool, {new_inst});
  MakeGoto(right);
  call_right->CopyEnvironmentFrom(cls->GetEnvironment());

  HInstruction* read_return =
      MakeIFieldGet(return_block, new_inst, DataType::Type::kInt32, MemberOffset(32));
  MakeReturn(return_block, read_return);

  MakeExit(exit);

  PerformLSE(blks);

  EXPECT_INS_RETAINED(read_return);
  EXPECT_INS_RETAINED(write_right);
  EXPECT_INS_RETAINED(write_left_loop);
  EXPECT_INS_RETAINED(call_left_loop);
  EXPECT_INS_REMOVED(write_left_pre);
  EXPECT_INS_RETAINED(call_right);
}

// // ENTRY
// obj = new Obj();
// if (parameter_value) {
//   // LEFT
//   // DO NOT ELIMINATE
//   escape(obj);
//   obj.field = 1;
//   // obj has already escaped so can't use field = 1 for value
//   noescape();
// } else {
//   // RIGHT
//   // obj is needed for read since we don't know what the left value is
//   // DO NOT ELIMINATE
//   obj.field = 2;
//   noescape();
// }
// EXIT
// ELIMINATE
// return obj.field
TEST_F(LoadStoreEliminationTest, PartialLoadPreserved5) {
  ScopedObjectAccess soa(Thread::Current());
  VariableSizedHandleScope vshs(soa.Self());
  CreateGraph(&vshs);
  AdjacencyListGraph blks(SetupFromAdjacencyList("entry",
                                                 "exit",
                                                 {{"entry", "left"},
                                                  {"entry", "right"},
                                                  {"left", "breturn"},
                                                  {"right", "breturn"},
                                                  {"breturn", "exit"}}));
#define GET_BLOCK(name) HBasicBlock* name = blks.Get(#name)
  GET_BLOCK(entry);
  GET_BLOCK(exit);
  GET_BLOCK(breturn);
  GET_BLOCK(left);
  GET_BLOCK(right);
#undef GET_BLOCK
  HInstruction* bool_value = MakeParam(DataType::Type::kBool);
  HInstruction* c1 = graph_->GetIntConstant(1);
  HInstruction* c2 = graph_->GetIntConstant(2);

  HInstruction* cls = MakeLoadClass(entry);
  HInstruction* new_inst = MakeNewInstance(entry, cls);
  MakeIf(entry, bool_value);
  ManuallyBuildEnvFor(cls, {});
  new_inst->CopyEnvironmentFrom(cls->GetEnvironment());

  HInstruction* call_left = MakeInvokeStatic(left, DataType::Type::kVoid, { new_inst });
  HInstruction* write_left = MakeIFieldSet(left, new_inst, c1, MemberOffset(32));
  HInstruction* call2_left = MakeInvokeStatic(left, DataType::Type::kVoid, {});
  MakeGoto(left);
  call_left->CopyEnvironmentFrom(cls->GetEnvironment());
  call2_left->CopyEnvironmentFrom(cls->GetEnvironment());

  HInstruction* write_right = MakeIFieldSet(right, new_inst, c2, MemberOffset(32));
  HInstruction* call_right = MakeInvokeStatic(right, DataType::Type::kVoid, {});
  MakeGoto(right);
  call_right->CopyEnvironmentFrom(cls->GetEnvironment());

  HInstruction* read_bottom =
      MakeIFieldGet(breturn, new_inst, DataType::Type::kInt32, MemberOffset(32));
  MakeReturn(breturn, read_bottom);

  MakeExit(exit);

  PerformLSE(blks);

  EXPECT_INS_RETAINED(read_bottom);
  EXPECT_INS_RETAINED(write_right);
  EXPECT_INS_RETAINED(write_left);
  EXPECT_INS_RETAINED(call_left);
  EXPECT_INS_RETAINED(call_right);
}

// // ENTRY
// obj = new Obj();
// DO NOT ELIMINATE. Kept by escape.
// obj.field = 3;
// noescape();
// if (parameter_value) {
//   // LEFT
//   // DO NOT ELIMINATE
//   escape(obj);
//   obj.field = 1;
// } else {
//   // RIGHT
//   // ELIMINATE
//   obj.field = 2;
// }
// EXIT
// ELIMINATE
// return obj.field
// Disabled due to b/205813546.
TEST_F(LoadStoreEliminationTest, DISABLED_PartialLoadPreserved6) {
  CreateGraph();
  AdjacencyListGraph blks(SetupFromAdjacencyList("entry",
                                                 "exit",
                                                 {{"entry", "left"},
                                                  {"entry", "right"},
                                                  {"left", "breturn"},
                                                  {"right", "breturn"},
                                                  {"breturn", "exit"}}));
#define GET_BLOCK(name) HBasicBlock* name = blks.Get(#name)
  GET_BLOCK(entry);
  GET_BLOCK(exit);
  GET_BLOCK(breturn);
  GET_BLOCK(left);
  GET_BLOCK(right);
#undef GET_BLOCK
  HInstruction* bool_value = MakeParam(DataType::Type::kBool);
  HInstruction* c1 = graph_->GetIntConstant(1);
  HInstruction* c2 = graph_->GetIntConstant(2);
  HInstruction* c3 = graph_->GetIntConstant(3);

  HInstruction* cls = MakeLoadClass(entry);
  HInstruction* new_inst = MakeNewInstance(entry, cls);
  HInstruction* write_entry = MakeIFieldSet(entry, new_inst, c3, MemberOffset(32));
  HInstruction* call_entry = MakeInvokeStatic(entry, DataType::Type::kVoid, {});
  MakeIf(entry, bool_value);
  ManuallyBuildEnvFor(cls, {});
  new_inst->CopyEnvironmentFrom(cls->GetEnvironment());
  call_entry->CopyEnvironmentFrom(cls->GetEnvironment());

  HInstruction* call_left = MakeInvokeStatic(left, DataType::Type::kVoid, { new_inst });
  HInstruction* write_left = MakeIFieldSet(left, new_inst, c1, MemberOffset(32));
  MakeGoto(left);
  call_left->CopyEnvironmentFrom(cls->GetEnvironment());

  HInstruction* write_right = MakeIFieldSet(right, new_inst, c2, MemberOffset(32));
  MakeGoto(right);

  HInstruction* read_bottom =
      MakeIFieldGet(breturn, new_inst, DataType::Type::kInt32, MemberOffset(32));
  MakeReturn(breturn, read_bottom);

  MakeExit(exit);

  PerformLSE(blks);

  EXPECT_INS_REMOVED(read_bottom);
  EXPECT_INS_REMOVED(write_right);
  EXPECT_INS_RETAINED(write_entry);
  EXPECT_INS_RETAINED(write_left);
  EXPECT_INS_RETAINED(call_left);
  EXPECT_INS_RETAINED(call_entry);
}
}  // namespace art
