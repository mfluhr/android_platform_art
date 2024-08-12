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

#include "gvn.h"

#include "base/arena_allocator.h"
#include "base/macros.h"
#include "builder.h"
#include "nodes.h"
#include "optimizing_unit_test.h"
#include "side_effects_analysis.h"

namespace art HIDDEN {

class GVNTest : public OptimizingUnitTest {};

TEST_F(GVNTest, LocalFieldElimination) {
  HGraph* graph = CreateGraph();
  HBasicBlock* entry = new (GetAllocator()) HBasicBlock(graph);
  graph->AddBlock(entry);
  graph->SetEntryBlock(entry);
  HInstruction* parameter = MakeParam(DataType::Type::kReference);

  HBasicBlock* block = new (GetAllocator()) HBasicBlock(graph);
  graph->AddBlock(block);
  entry->AddSuccessor(block);

  MakeIFieldGet(block, parameter, DataType::Type::kReference, MemberOffset(42));
  HInstruction* to_remove =
      MakeIFieldGet(block, parameter, DataType::Type::kReference, MemberOffset(42));
  HInstruction* different_offset =
      MakeIFieldGet(block, parameter, DataType::Type::kReference, MemberOffset(43));
  // Kill the value.
  MakeIFieldSet(block, parameter, parameter, MemberOffset(42));
  HInstruction* use_after_kill =
      MakeIFieldGet(block, parameter, DataType::Type::kReference, MemberOffset(42));
  MakeExit(block);

  ASSERT_EQ(to_remove->GetBlock(), block);
  ASSERT_EQ(different_offset->GetBlock(), block);
  ASSERT_EQ(use_after_kill->GetBlock(), block);

  graph->BuildDominatorTree();
  SideEffectsAnalysis side_effects(graph);
  side_effects.Run();
  GVNOptimization(graph, side_effects).Run();

  ASSERT_TRUE(to_remove->GetBlock() == nullptr);
  ASSERT_EQ(different_offset->GetBlock(), block);
  ASSERT_EQ(use_after_kill->GetBlock(), block);
}

TEST_F(GVNTest, GlobalFieldElimination) {
  HGraph* graph = CreateGraph();
  HBasicBlock* entry = new (GetAllocator()) HBasicBlock(graph);
  graph->AddBlock(entry);
  graph->SetEntryBlock(entry);
  HInstruction* parameter = MakeParam(DataType::Type::kReference);

  HBasicBlock* block = new (GetAllocator()) HBasicBlock(graph);
  graph->AddBlock(block);
  entry->AddSuccessor(block);

  HInstruction* field_get =
      MakeIFieldGet(block, parameter, DataType::Type::kBool, MemberOffset(42));
  MakeIf(block, field_get);

  HBasicBlock* then = new (GetAllocator()) HBasicBlock(graph);
  HBasicBlock* else_ = new (GetAllocator()) HBasicBlock(graph);
  HBasicBlock* join = new (GetAllocator()) HBasicBlock(graph);
  graph->AddBlock(then);
  graph->AddBlock(else_);
  graph->AddBlock(join);

  block->AddSuccessor(then);
  block->AddSuccessor(else_);
  then->AddSuccessor(join);
  else_->AddSuccessor(join);

  MakeIFieldGet(then, parameter, DataType::Type::kBool, MemberOffset(42));
  MakeGoto(then);

  MakeIFieldGet(else_, parameter, DataType::Type::kBool, MemberOffset(42));
  MakeGoto(else_);

  MakeIFieldGet(join, parameter, DataType::Type::kBool, MemberOffset(42));
  MakeExit(join);

  graph->BuildDominatorTree();
  SideEffectsAnalysis side_effects(graph);
  side_effects.Run();
  GVNOptimization(graph, side_effects).Run();

  // Check that all field get instructions have been GVN'ed.
  ASSERT_TRUE(then->GetFirstInstruction()->IsGoto());
  ASSERT_TRUE(else_->GetFirstInstruction()->IsGoto());
  ASSERT_TRUE(join->GetFirstInstruction()->IsExit());
}

TEST_F(GVNTest, LoopFieldElimination) {
  HGraph* graph = CreateGraph();
  HBasicBlock* entry = new (GetAllocator()) HBasicBlock(graph);
  graph->AddBlock(entry);
  graph->SetEntryBlock(entry);

  HInstruction* parameter = MakeParam(DataType::Type::kReference);

  HBasicBlock* block = new (GetAllocator()) HBasicBlock(graph);
  graph->AddBlock(block);
  entry->AddSuccessor(block);
  MakeIFieldGet(block, parameter, DataType::Type::kBool, MemberOffset(42));
  MakeGoto(block);

  HBasicBlock* loop_header = new (GetAllocator()) HBasicBlock(graph);
  HBasicBlock* loop_body = new (GetAllocator()) HBasicBlock(graph);
  HBasicBlock* exit = new (GetAllocator()) HBasicBlock(graph);

  graph->AddBlock(loop_header);
  graph->AddBlock(loop_body);
  graph->AddBlock(exit);
  block->AddSuccessor(loop_header);
  loop_header->AddSuccessor(loop_body);
  loop_header->AddSuccessor(exit);
  loop_body->AddSuccessor(loop_header);

  HInstruction* field_get_in_loop_header =
      MakeIFieldGet(loop_header, parameter, DataType::Type::kBool, MemberOffset(42));
  MakeIf(loop_header, field_get_in_loop_header);

  // Kill inside the loop body to prevent field gets inside the loop header
  // and the body to be GVN'ed.
  HInstruction* field_set =
      MakeIFieldSet(loop_body, parameter, parameter, DataType::Type::kBool, MemberOffset(42));
  HInstruction* field_get_in_loop_body =
      MakeIFieldGet(loop_body, parameter, DataType::Type::kBool, MemberOffset(42));
  MakeGoto(loop_body);

  HInstruction* field_get_in_exit =
      MakeIFieldGet(exit, parameter, DataType::Type::kBool, MemberOffset(42));
  MakeExit(exit);

  ASSERT_EQ(field_get_in_loop_header->GetBlock(), loop_header);
  ASSERT_EQ(field_get_in_loop_body->GetBlock(), loop_body);
  ASSERT_EQ(field_get_in_exit->GetBlock(), exit);

  graph->BuildDominatorTree();
  {
    SideEffectsAnalysis side_effects(graph);
    side_effects.Run();
    GVNOptimization(graph, side_effects).Run();
  }

  // Check that all field get instructions are still there.
  ASSERT_EQ(field_get_in_loop_header->GetBlock(), loop_header);
  ASSERT_EQ(field_get_in_loop_body->GetBlock(), loop_body);
  // The exit block is dominated by the loop header, whose field get
  // does not get killed by the loop flags.
  ASSERT_TRUE(field_get_in_exit->GetBlock() == nullptr);

  // Now remove the field set, and check that all field get instructions have been GVN'ed.
  loop_body->RemoveInstruction(field_set);
  {
    SideEffectsAnalysis side_effects(graph);
    side_effects.Run();
    GVNOptimization(graph, side_effects).Run();
  }

  ASSERT_TRUE(field_get_in_loop_header->GetBlock() == nullptr);
  ASSERT_TRUE(field_get_in_loop_body->GetBlock() == nullptr);
  ASSERT_TRUE(field_get_in_exit->GetBlock() == nullptr);
}

// Test that inner loops affect the side effects of the outer loop.
TEST_F(GVNTest, LoopSideEffects) {
  static const SideEffects kCanTriggerGC = SideEffects::CanTriggerGC();

  HGraph* graph = CreateGraph();
  HBasicBlock* entry = new (GetAllocator()) HBasicBlock(graph);
  graph->AddBlock(entry);
  graph->SetEntryBlock(entry);

  HBasicBlock* outer_loop_header = new (GetAllocator()) HBasicBlock(graph);
  HBasicBlock* outer_loop_body = new (GetAllocator()) HBasicBlock(graph);
  HBasicBlock* outer_loop_exit = new (GetAllocator()) HBasicBlock(graph);
  HBasicBlock* inner_loop_header = new (GetAllocator()) HBasicBlock(graph);
  HBasicBlock* inner_loop_body = new (GetAllocator()) HBasicBlock(graph);
  HBasicBlock* inner_loop_exit = new (GetAllocator()) HBasicBlock(graph);

  graph->AddBlock(outer_loop_header);
  graph->AddBlock(outer_loop_body);
  graph->AddBlock(outer_loop_exit);
  graph->AddBlock(inner_loop_header);
  graph->AddBlock(inner_loop_body);
  graph->AddBlock(inner_loop_exit);

  entry->AddSuccessor(outer_loop_header);
  outer_loop_header->AddSuccessor(outer_loop_body);
  outer_loop_header->AddSuccessor(outer_loop_exit);
  outer_loop_body->AddSuccessor(inner_loop_header);
  inner_loop_header->AddSuccessor(inner_loop_body);
  inner_loop_header->AddSuccessor(inner_loop_exit);
  inner_loop_body->AddSuccessor(inner_loop_header);
  inner_loop_exit->AddSuccessor(outer_loop_header);

  HInstruction* parameter = MakeParam(DataType::Type::kBool);
  MakeGoto(entry);
  MakeSuspendCheck(outer_loop_header);
  MakeIf(outer_loop_header, parameter);
  MakeGoto(outer_loop_body);
  MakeSuspendCheck(inner_loop_header);
  MakeIf(inner_loop_header, parameter);
  MakeGoto(inner_loop_body);
  MakeGoto(inner_loop_exit);
  MakeExit(outer_loop_exit);

  graph->BuildDominatorTree();

  ASSERT_TRUE(inner_loop_header->GetLoopInformation()->IsIn(
      *outer_loop_header->GetLoopInformation()));

  // Check that the only side effect of loops is to potentially trigger GC.
  {
    // Make one block with a side effect.
    MakeIFieldSet(entry, parameter, parameter, DataType::Type::kReference, MemberOffset(42));

    SideEffectsAnalysis side_effects(graph);
    side_effects.Run();

    ASSERT_TRUE(side_effects.GetBlockEffects(entry).DoesAnyWrite());
    ASSERT_FALSE(side_effects.GetBlockEffects(outer_loop_body).DoesAnyWrite());
    ASSERT_FALSE(side_effects.GetLoopEffects(outer_loop_header).DoesAnyWrite());
    ASSERT_FALSE(side_effects.GetLoopEffects(inner_loop_header).DoesAnyWrite());
    ASSERT_TRUE(side_effects.GetLoopEffects(outer_loop_header).Equals(kCanTriggerGC));
    ASSERT_TRUE(side_effects.GetLoopEffects(inner_loop_header).Equals(kCanTriggerGC));
  }

  // Check that the side effects of the outer loop does not affect the inner loop.
  {
    MakeIFieldSet(
        outer_loop_body, parameter, parameter, DataType::Type::kReference, MemberOffset(42));

    SideEffectsAnalysis side_effects(graph);
    side_effects.Run();

    ASSERT_TRUE(side_effects.GetBlockEffects(entry).DoesAnyWrite());
    ASSERT_TRUE(side_effects.GetBlockEffects(outer_loop_body).DoesAnyWrite());
    ASSERT_TRUE(side_effects.GetLoopEffects(outer_loop_header).DoesAnyWrite());
    ASSERT_FALSE(side_effects.GetLoopEffects(inner_loop_header).DoesAnyWrite());
    ASSERT_TRUE(side_effects.GetLoopEffects(inner_loop_header).Equals(kCanTriggerGC));
  }

  // Check that the side effects of the inner loop affects the outer loop.
  {
    outer_loop_body->RemoveInstruction(outer_loop_body->GetFirstInstruction());
    MakeIFieldSet(
        inner_loop_body, parameter, parameter, DataType::Type::kReference, MemberOffset(42));

    SideEffectsAnalysis side_effects(graph);
    side_effects.Run();

    ASSERT_TRUE(side_effects.GetBlockEffects(entry).DoesAnyWrite());
    ASSERT_FALSE(side_effects.GetBlockEffects(outer_loop_body).DoesAnyWrite());
    ASSERT_TRUE(side_effects.GetLoopEffects(outer_loop_header).DoesAnyWrite());
    ASSERT_TRUE(side_effects.GetLoopEffects(inner_loop_header).DoesAnyWrite());
  }
}
}  // namespace art
