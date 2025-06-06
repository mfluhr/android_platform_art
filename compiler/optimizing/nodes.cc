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
#include "nodes.h"

#include <algorithm>
#include <cfloat>
#include <functional>
#include <optional>

#include "art_method-inl.h"
#include "base/arena_allocator.h"
#include "base/arena_bit_vector.h"
#include "base/bit_utils.h"
#include "base/bit_vector-inl.h"
#include "base/bit_vector.h"
#include "base/iteration_range.h"
#include "base/logging.h"
#include "base/malloc_arena_pool.h"
#include "base/scoped_arena_allocator.h"
#include "base/scoped_arena_containers.h"
#include "base/stl_util.h"
#include "class_linker-inl.h"
#include "class_root-inl.h"
#include "code_generator.h"
#include "common_dominator.h"
#include "intrinsic_objects.h"
#include "intrinsics.h"
#include "intrinsics_list.h"
#include "mirror/class-inl.h"
#include "scoped_thread_state_change-inl.h"
#include "ssa_builder.h"

namespace art HIDDEN {

// Enable floating-point static evaluation during constant folding
// only if all floating-point operations and constants evaluate in the
// range and precision of the type used (i.e., 32-bit float, 64-bit
// double).
static constexpr bool kEnableFloatingPointStaticEvaluation = (FLT_EVAL_METHOD == 0);

void HGraph::AddBlock(HBasicBlock* block) {
  block->SetBlockId(blocks_.size());
  blocks_.push_back(block);
}

inline int32_t HGraph::AllocateInstructionId() {
  CHECK_NE(current_instruction_id_, INT32_MAX);
  return current_instruction_id_++;
}

void HGraph::FindBackEdges(/*out*/ BitVectorView<size_t> visited) {
  // "visited" must be empty on entry, it's an output argument for all visited (i.e. live) blocks.
  DCHECK(!visited.IsAnyBitSet());

  // Allocate memory from local ScopedArenaAllocator.
  ScopedArenaAllocator allocator(GetArenaStack());
  // Nodes that we're currently visiting, indexed by block id.
  BitVectorView<size_t> visiting =
      ArenaBitVector::CreateFixedSize(&allocator, blocks_.size(), kArenaAllocGraphBuilder);
  // Number of successors visited from a given node, indexed by block id.
  ScopedArenaVector<size_t> successors_visited(blocks_.size(),
                                               0u,
                                               allocator.Adapter(kArenaAllocGraphBuilder));
  // Stack of nodes that we're currently visiting (same as marked in "visiting" above).
  ScopedArenaVector<HBasicBlock*> worklist(allocator.Adapter(kArenaAllocGraphBuilder));
  constexpr size_t kDefaultWorklistSize = 8;
  worklist.reserve(kDefaultWorklistSize);
  visited.SetBit(entry_block_->GetBlockId());
  visiting.SetBit(entry_block_->GetBlockId());
  worklist.push_back(entry_block_);

  while (!worklist.empty()) {
    HBasicBlock* current = worklist.back();
    uint32_t current_id = current->GetBlockId();
    if (successors_visited[current_id] == current->GetSuccessors().size()) {
      visiting.ClearBit(current_id);
      worklist.pop_back();
    } else {
      HBasicBlock* successor = current->GetSuccessors()[successors_visited[current_id]++];
      uint32_t successor_id = successor->GetBlockId();
      if (visiting.IsBitSet(successor_id)) {
        DCHECK(ContainsElement(worklist, successor));
        successor->AddBackEdge(current);
      } else if (!visited.IsBitSet(successor_id)) {
        visited.SetBit(successor_id);
        visiting.SetBit(successor_id);
        worklist.push_back(successor);
      }
    }
  }
}

// Remove the environment use records of the instruction for users.
void RemoveEnvironmentUses(HInstruction* instruction) {
  for (HEnvironment* environment = instruction->GetEnvironment();
       environment != nullptr;
       environment = environment->GetParent()) {
    for (size_t i = 0, e = environment->Size(); i < e; ++i) {
      if (environment->GetInstructionAt(i) != nullptr) {
        environment->RemoveAsUserOfInput(i);
      }
    }
  }
}

// Return whether the instruction has an environment and it's used by others.
bool HasEnvironmentUsedByOthers(HInstruction* instruction) {
  for (HEnvironment* environment = instruction->GetEnvironment();
       environment != nullptr;
       environment = environment->GetParent()) {
    for (size_t i = 0, e = environment->Size(); i < e; ++i) {
      HInstruction* user = environment->GetInstructionAt(i);
      if (user != nullptr) {
        return true;
      }
    }
  }
  return false;
}

// Reset environment records of the instruction itself.
void ResetEnvironmentInputRecords(HInstruction* instruction) {
  for (HEnvironment* environment = instruction->GetEnvironment();
       environment != nullptr;
       environment = environment->GetParent()) {
    for (size_t i = 0, e = environment->Size(); i < e; ++i) {
      DCHECK(environment->GetHolder() == instruction);
      if (environment->GetInstructionAt(i) != nullptr) {
        environment->SetRawEnvAt(i, nullptr);
      }
    }
  }
}

static void RemoveAsUser(HInstruction* instruction) {
  instruction->RemoveAsUserOfAllInputs();
  RemoveEnvironmentUses(instruction);
}

void HGraph::RemoveDeadBlocksInstructionsAsUsersAndDisconnect(
    BitVectorView<const size_t> visited) const {
  for (size_t i = 0; i < blocks_.size(); ++i) {
    if (!visited.IsBitSet(i)) {
      HBasicBlock* block = blocks_[i];
      if (block == nullptr) continue;

      // Remove as user.
      for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
        RemoveAsUser(it.Current());
      }
      for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
        RemoveAsUser(it.Current());
      }

      // Remove non-catch phi uses, and disconnect the block.
      block->DisconnectFromSuccessors(visited);
    }
  }
}

// This method assumes `insn` has been removed from all users with the exception of catch
// phis because of missing exceptional edges in the graph. It removes the
// instruction from catch phi uses, together with inputs of other catch phis in
// the catch block at the same index, as these must be dead too.
static void RemoveCatchPhiUsesOfDeadInstruction(HInstruction* insn) {
  DCHECK(!insn->HasEnvironmentUses());
  while (insn->HasNonEnvironmentUses()) {
    const HUseListNode<HInstruction*>& use = insn->GetUses().front();
    size_t use_index = use.GetIndex();
    HBasicBlock* user_block = use.GetUser()->GetBlock();
    DCHECK(use.GetUser()->IsPhi());
    DCHECK(user_block->IsCatchBlock());
    for (HInstructionIterator phi_it(user_block->GetPhis()); !phi_it.Done(); phi_it.Advance()) {
      phi_it.Current()->AsPhi()->RemoveInputAt(use_index);
    }
  }
}

void HGraph::RemoveDeadBlocks(BitVectorView<const size_t> visited) {
  DCHECK(reverse_post_order_.empty()) << "We shouldn't have dominance information.";
  for (size_t i = 0; i < blocks_.size(); ++i) {
    if (!visited.IsBitSet(i)) {
      HBasicBlock* block = blocks_[i];
      if (block == nullptr) continue;

      // Remove all remaining uses (which should be only catch phi uses), and the instructions.
      block->RemoveCatchPhiUsesAndInstruction(/* building_dominator_tree = */ true);

      // Remove the block from the list of blocks, so that further analyses
      // never see it.
      blocks_[i] = nullptr;
      if (block->IsExitBlock()) {
        SetExitBlock(nullptr);
      }
      // Mark the block as removed. This is used by the HGraphBuilder to discard
      // the block as a branch target.
      block->SetGraph(nullptr);
    }
  }
}

GraphAnalysisResult HGraph::BuildDominatorTree() {
  // Allocate memory from local ScopedArenaAllocator.
  ScopedArenaAllocator allocator(GetArenaStack());

  BitVectorView<size_t> visited =
      ArenaBitVector::CreateFixedSize(&allocator, blocks_.size(), kArenaAllocGraphBuilder);

  // (1) Find the back edges in the graph doing a DFS traversal.
  FindBackEdges(visited);

  // (2) Remove instructions and phis from blocks not visited during
  //     the initial DFS as users from other instructions, so that
  //     users can be safely removed before uses later.
  //     Also disconnect the block from its successors, updating the successor's phis if needed.
  RemoveDeadBlocksInstructionsAsUsersAndDisconnect(visited);

  // (3) Remove blocks not visited during the initial DFS.
  //     Step (5) requires dead blocks to be removed from the
  //     predecessors list of live blocks.
  RemoveDeadBlocks(visited);

  // (4) Simplify the CFG now, so that we don't need to recompute
  //     dominators and the reverse post order.
  SimplifyCFG();

  // (5) Compute the dominance information and the reverse post order.
  ComputeDominanceInformation();

  // (6) Analyze loops discovered through back edge analysis, and
  //     set the loop information on each block.
  GraphAnalysisResult result = AnalyzeLoops();
  if (result != kAnalysisSuccess) {
    return result;
  }

  // (7) Precompute per-block try membership before entering the SSA builder,
  //     which needs the information to build catch block phis from values of
  //     locals at throwing instructions inside try blocks.
  ComputeTryBlockInformation();

  return kAnalysisSuccess;
}

GraphAnalysisResult HGraph::RecomputeDominatorTree() {
  DCHECK(!HasIrreducibleLoops()) << "Recomputing loop information in graphs with irreducible loops "
                                 << "is unsupported, as it could lead to loop header changes";
  ClearLoopInformation();
  ClearDominanceInformation();
  return BuildDominatorTree();
}

void HGraph::ClearDominanceInformation() {
  for (HBasicBlock* block : GetActiveBlocks()) {
    block->ClearDominanceInformation();
  }
  reverse_post_order_.clear();
}

void HGraph::ClearLoopInformation() {
  SetHasLoops(false);
  SetHasIrreducibleLoops(false);
  for (HBasicBlock* block : GetActiveBlocks()) {
    block->SetLoopInformation(nullptr);
  }
}

void HBasicBlock::ClearDominanceInformation() {
  dominated_blocks_.clear();
  dominator_ = nullptr;
}

HInstruction* HBasicBlock::GetFirstInstructionDisregardMoves() const {
  HInstruction* instruction = GetFirstInstruction();
  while (instruction->IsParallelMove()) {
    instruction = instruction->GetNext();
  }
  return instruction;
}

static bool UpdateDominatorOfSuccessor(HBasicBlock* block, HBasicBlock* successor) {
  DCHECK(ContainsElement(block->GetSuccessors(), successor));

  HBasicBlock* old_dominator = successor->GetDominator();
  HBasicBlock* new_dominator =
      (old_dominator == nullptr) ? block
                                 : CommonDominator::ForPair(old_dominator, block);

  if (old_dominator == new_dominator) {
    return false;
  } else {
    successor->SetDominator(new_dominator);
    return true;
  }
}

void HGraph::ComputeDominanceInformation() {
  DCHECK(reverse_post_order_.empty());
  reverse_post_order_.reserve(blocks_.size());
  reverse_post_order_.push_back(entry_block_);

  // Allocate memory from local ScopedArenaAllocator.
  ScopedArenaAllocator allocator(GetArenaStack());
  // Number of visits of a given node, indexed by block id.
  ScopedArenaVector<size_t> visits(blocks_.size(), 0u, allocator.Adapter(kArenaAllocGraphBuilder));
  // Number of successors visited from a given node, indexed by block id.
  ScopedArenaVector<size_t> successors_visited(blocks_.size(),
                                               0u,
                                               allocator.Adapter(kArenaAllocGraphBuilder));
  // Nodes for which we need to visit successors.
  ScopedArenaVector<HBasicBlock*> worklist(allocator.Adapter(kArenaAllocGraphBuilder));
  constexpr size_t kDefaultWorklistSize = 8;
  worklist.reserve(kDefaultWorklistSize);
  worklist.push_back(entry_block_);

  while (!worklist.empty()) {
    HBasicBlock* current = worklist.back();
    uint32_t current_id = current->GetBlockId();
    if (successors_visited[current_id] == current->GetSuccessors().size()) {
      worklist.pop_back();
    } else {
      HBasicBlock* successor = current->GetSuccessors()[successors_visited[current_id]++];
      UpdateDominatorOfSuccessor(current, successor);

      // Once all the forward edges have been visited, we know the immediate
      // dominator of the block. We can then start visiting its successors.
      if (++visits[successor->GetBlockId()] ==
          successor->GetPredecessors().size() - successor->NumberOfBackEdges()) {
        reverse_post_order_.push_back(successor);
        worklist.push_back(successor);
      }
    }
  }

  // Check if the graph has back edges not dominated by their respective headers.
  // If so, we need to update the dominators of those headers and recursively of
  // their successors. We do that with a fix-point iteration over all blocks.
  // The algorithm is guaranteed to terminate because it loops only if the sum
  // of all dominator chains has decreased in the current iteration.
  bool must_run_fix_point = false;
  for (HBasicBlock* block : blocks_) {
    if (block != nullptr &&
        block->IsLoopHeader() &&
        block->GetLoopInformation()->HasBackEdgeNotDominatedByHeader()) {
      must_run_fix_point = true;
      break;
    }
  }
  if (must_run_fix_point) {
    bool update_occurred = true;
    while (update_occurred) {
      update_occurred = false;
      for (HBasicBlock* block : GetReversePostOrder()) {
        for (HBasicBlock* successor : block->GetSuccessors()) {
          update_occurred |= UpdateDominatorOfSuccessor(block, successor);
        }
      }
    }
  }

  // Make sure that there are no remaining blocks whose dominator information
  // needs to be updated.
  if (kIsDebugBuild) {
    for (HBasicBlock* block : GetReversePostOrder()) {
      for (HBasicBlock* successor : block->GetSuccessors()) {
        DCHECK(!UpdateDominatorOfSuccessor(block, successor));
      }
    }
  }

  // Populate `dominated_blocks_` information after computing all dominators.
  // The potential presence of irreducible loops requires to do it after.
  for (HBasicBlock* block : GetReversePostOrder()) {
    if (!block->IsEntryBlock()) {
      block->GetDominator()->AddDominatedBlock(block);
    }
  }
}

HBasicBlock* HGraph::SplitEdge(HBasicBlock* block, HBasicBlock* successor) {
  HBasicBlock* new_block = new (allocator_) HBasicBlock(this, successor->GetDexPc());
  AddBlock(new_block);
  // Use `InsertBetween` to ensure the predecessor index and successor index of
  // `block` and `successor` are preserved.
  new_block->InsertBetween(block, successor);
  return new_block;
}

void HGraph::SplitCriticalEdge(HBasicBlock* block, HBasicBlock* successor) {
  // Insert a new node between `block` and `successor` to split the
  // critical edge.
  HBasicBlock* new_block = SplitEdge(block, successor);
  new_block->AddInstruction(new (allocator_) HGoto(successor->GetDexPc()));
  if (successor->IsLoopHeader()) {
    // If we split at a back edge boundary, make the new block the back edge.
    HLoopInformation* info = successor->GetLoopInformation();
    if (info->IsBackEdge(*block)) {
      info->RemoveBackEdge(block);
      info->AddBackEdge(new_block);
    }
  }
}

HBasicBlock* HGraph::SplitEdgeAndUpdateRPO(HBasicBlock* block, HBasicBlock* successor) {
  HBasicBlock* new_block = SplitEdge(block, successor);
  // In the RPO we have {... , block, ... , successor}. We want to insert `new_block` right after
  // `block` to have a consistent RPO without recomputing the whole graph's RPO.
  reverse_post_order_.insert(
      reverse_post_order_.begin() + IndexOfElement(reverse_post_order_, block) + 1, new_block);
  return new_block;
}

// Reorder phi inputs to match reordering of the block's predecessors.
static void FixPhisAfterPredecessorsReodering(HBasicBlock* block, size_t first, size_t second) {
  for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
    HPhi* phi = it.Current()->AsPhi();
    HInstruction* first_instr = phi->InputAt(first);
    HInstruction* second_instr = phi->InputAt(second);
    phi->ReplaceInput(first_instr, second);
    phi->ReplaceInput(second_instr, first);
  }
}

// Make sure that the first predecessor of a loop header is the incoming block.
void HGraph::OrderLoopHeaderPredecessors(HBasicBlock* header) {
  DCHECK(header->IsLoopHeader());
  HLoopInformation* info = header->GetLoopInformation();
  if (info->IsBackEdge(*header->GetPredecessors()[0])) {
    HBasicBlock* to_swap = header->GetPredecessors()[0];
    for (size_t pred = 1, e = header->GetPredecessors().size(); pred < e; ++pred) {
      HBasicBlock* predecessor = header->GetPredecessors()[pred];
      if (!info->IsBackEdge(*predecessor)) {
        header->predecessors_[pred] = to_swap;
        header->predecessors_[0] = predecessor;
        FixPhisAfterPredecessorsReodering(header, 0, pred);
        break;
      }
    }
  }
}

// Transform control flow of the loop to a single preheader format (don't touch the data flow).
// New_preheader can be already among the header predecessors - this situation will be correctly
// processed.
static void FixControlForNewSinglePreheader(HBasicBlock* header, HBasicBlock* new_preheader) {
  HLoopInformation* loop_info = header->GetLoopInformation();
  for (size_t pred = 0; pred < header->GetPredecessors().size(); ++pred) {
    HBasicBlock* predecessor = header->GetPredecessors()[pred];
    if (!loop_info->IsBackEdge(*predecessor) && predecessor != new_preheader) {
      predecessor->ReplaceSuccessor(header, new_preheader);
      pred--;
    }
  }
}

//             == Before ==                                               == After ==
//      _________         _________                               _________         _________
//     | B0      |       | B1      |      (old preheaders)       | B0      |       | B1      |
//     |=========|       |=========|                             |=========|       |=========|
//     | i0 = .. |       | i1 = .. |                             | i0 = .. |       | i1 = .. |
//     |_________|       |_________|                             |_________|       |_________|
//           \               /                                         \              /
//            \             /                                        ___v____________v___
//             \           /               (new preheader)          | B20 <- B0, B1      |
//              |         |                                         |====================|
//              |         |                                         | i20 = phi(i0, i1)  |
//              |         |                                         |____________________|
//              |         |                                                   |
//    /\        |         |        /\                           /\            |              /\
//   /  v_______v_________v_______v  \                         /  v___________v_____________v  \
//  |  | B10 <- B0, B1, B2, B3     |  |                       |  | B10 <- B20, B2, B3        |  |
//  |  |===========================|  |       (header)        |  |===========================|  |
//  |  | i10 = phi(i0, i1, i2, i3) |  |                       |  | i10 = phi(i20, i2, i3)    |  |
//  |  |___________________________|  |                       |  |___________________________|  |
//  |        /               \        |                       |        /               \        |
//  |      ...              ...       |                       |      ...              ...       |
//  |   _________         _________   |                       |   _________         _________   |
//  |  | B2      |       | B3      |  |                       |  | B2      |       | B3      |  |
//  |  |=========|       |=========|  |     (back edges)      |  |=========|       |=========|  |
//  |  | i2 = .. |       | i3 = .. |  |                       |  | i2 = .. |       | i3 = .. |  |
//  |  |_________|       |_________|  |                       |  |_________|       |_________|  |
//   \     /                   \     /                         \     /                   \     /
//    \___/                     \___/                           \___/                     \___/
//
void HGraph::TransformLoopToSinglePreheaderFormat(HBasicBlock* header) {
  HLoopInformation* loop_info = header->GetLoopInformation();

  HBasicBlock* preheader = new (allocator_) HBasicBlock(this, header->GetDexPc());
  AddBlock(preheader);
  preheader->AddInstruction(new (allocator_) HGoto(header->GetDexPc()));

  // If the old header has no Phis then we only need to fix the control flow.
  if (header->GetPhis().IsEmpty()) {
    FixControlForNewSinglePreheader(header, preheader);
    preheader->AddSuccessor(header);
    return;
  }

  // Find the first non-back edge block in the header's predecessors list.
  size_t first_nonbackedge_pred_pos = 0;
  bool found = false;
  for (size_t pred = 0; pred < header->GetPredecessors().size(); ++pred) {
    HBasicBlock* predecessor = header->GetPredecessors()[pred];
    if (!loop_info->IsBackEdge(*predecessor)) {
      first_nonbackedge_pred_pos = pred;
      found = true;
      break;
    }
  }

  DCHECK(found);

  // Fix the data-flow.
  for (HInstructionIterator it(header->GetPhis()); !it.Done(); it.Advance()) {
    HPhi* header_phi = it.Current()->AsPhi();

    HPhi* preheader_phi = new (GetAllocator()) HPhi(GetAllocator(),
                                                    header_phi->GetRegNumber(),
                                                    0,
                                                    header_phi->GetType());
    if (header_phi->GetType() == DataType::Type::kReference) {
      preheader_phi->SetReferenceTypeInfoIfValid(header_phi->GetReferenceTypeInfo());
    }
    preheader->AddPhi(preheader_phi);

    HInstruction* orig_input = header_phi->InputAt(first_nonbackedge_pred_pos);
    header_phi->ReplaceInput(preheader_phi, first_nonbackedge_pred_pos);
    preheader_phi->AddInput(orig_input);

    for (size_t input_pos = first_nonbackedge_pred_pos + 1;
         input_pos < header_phi->InputCount();
         input_pos++) {
      HInstruction* input = header_phi->InputAt(input_pos);
      HBasicBlock* pred_block = header->GetPredecessors()[input_pos];

      if (loop_info->Contains(*pred_block)) {
        DCHECK(loop_info->IsBackEdge(*pred_block));
      } else {
        preheader_phi->AddInput(input);
        header_phi->RemoveInputAt(input_pos);
        input_pos--;
      }
    }
  }

  // Fix the control-flow.
  HBasicBlock* first_pred = header->GetPredecessors()[first_nonbackedge_pred_pos];
  preheader->InsertBetween(first_pred, header);

  FixControlForNewSinglePreheader(header, preheader);
}

void HGraph::SimplifyLoop(HBasicBlock* header) {
  HLoopInformation* info = header->GetLoopInformation();

  // Make sure the loop has only one pre header. This simplifies SSA building by having
  // to just look at the pre header to know which locals are initialized at entry of the
  // loop. Also, don't allow the entry block to be a pre header: this simplifies inlining
  // this graph.
  size_t number_of_incomings = header->GetPredecessors().size() - info->NumberOfBackEdges();
  if (number_of_incomings != 1 || (GetEntryBlock()->GetSingleSuccessor() == header)) {
    TransformLoopToSinglePreheaderFormat(header);
  }

  OrderLoopHeaderPredecessors(header);

  HInstruction* first_instruction = header->GetFirstInstruction();
  if (first_instruction != nullptr && first_instruction->IsSuspendCheck()) {
    // Called from DeadBlockElimination. Update SuspendCheck pointer.
    info->SetSuspendCheck(first_instruction->AsSuspendCheck());
  }
}

void HGraph::ComputeTryBlockInformation() {
  // Iterate in reverse post order to propagate try membership information from
  // predecessors to their successors.
  bool graph_has_try_catch = false;

  for (HBasicBlock* block : GetReversePostOrder()) {
    if (block->IsEntryBlock() || block->IsCatchBlock()) {
      // Catch blocks after simplification have only exceptional predecessors
      // and hence are never in tries.
      continue;
    }

    // Infer try membership from the first predecessor. Having simplified loops,
    // the first predecessor can never be a back edge and therefore it must have
    // been visited already and had its try membership set.
    HBasicBlock* first_predecessor = block->GetPredecessors()[0];
    DCHECK_IMPLIES(block->IsLoopHeader(),
                   !block->GetLoopInformation()->IsBackEdge(*first_predecessor));
    const HTryBoundary* try_entry = first_predecessor->ComputeTryEntryOfSuccessors();
    graph_has_try_catch |= try_entry != nullptr;
    if (try_entry != nullptr &&
        (block->GetTryCatchInformation() == nullptr ||
         try_entry != &block->GetTryCatchInformation()->GetTryEntry())) {
      // We are either setting try block membership for the first time or it
      // has changed.
      block->SetTryCatchInformation(new (allocator_) TryCatchInformation(*try_entry));
    }
  }

  SetHasTryCatch(graph_has_try_catch);
}

void HGraph::SimplifyCFG() {
// Simplify the CFG for future analysis, and code generation:
  // (1): Split critical edges.
  // (2): Simplify loops by having only one preheader.
  // NOTE: We're appending new blocks inside the loop, so we need to use index because iterators
  // can be invalidated. We remember the initial size to avoid iterating over the new blocks.
  for (size_t block_id = 0u, end = blocks_.size(); block_id != end; ++block_id) {
    HBasicBlock* block = blocks_[block_id];
    if (block == nullptr) continue;
    if (block->GetSuccessors().size() > 1) {
      // Only split normal-flow edges. We cannot split exceptional edges as they
      // are synthesized (approximate real control flow), and we do not need to
      // anyway. Moves that would be inserted there are performed by the runtime.
      ArrayRef<HBasicBlock* const> normal_successors = block->GetNormalSuccessors();
      for (size_t j = 0, e = normal_successors.size(); j < e; ++j) {
        HBasicBlock* successor = normal_successors[j];
        DCHECK(!successor->IsCatchBlock());
        if (successor == exit_block_) {
          // (Throw/Return/ReturnVoid)->TryBoundary->Exit. Special case which we
          // do not want to split because Goto->Exit is not allowed.
          DCHECK(block->IsSingleTryBoundary());
        } else if (successor->GetPredecessors().size() > 1) {
          SplitCriticalEdge(block, successor);
          // SplitCriticalEdge could have invalidated the `normal_successors`
          // ArrayRef. We must re-acquire it.
          normal_successors = block->GetNormalSuccessors();
          DCHECK_EQ(normal_successors[j]->GetSingleSuccessor(), successor);
          DCHECK_EQ(e, normal_successors.size());
        }
      }
    }
    if (block->IsLoopHeader()) {
      SimplifyLoop(block);
    } else if (!block->IsEntryBlock() &&
               block->GetFirstInstruction() != nullptr &&
               block->GetFirstInstruction()->IsSuspendCheck()) {
      // We are being called by the dead code elimiation pass, and what used to be
      // a loop got dismantled. Just remove the suspend check.
      block->RemoveInstruction(block->GetFirstInstruction());
    }
  }
}

GraphAnalysisResult HGraph::AnalyzeLoops() const {
  // We iterate post order to ensure we visit inner loops before outer loops.
  // `PopulateRecursive` needs this guarantee to know whether a natural loop
  // contains an irreducible loop.
  for (HBasicBlock* block : GetPostOrder()) {
    if (block->IsLoopHeader()) {
      if (block->IsCatchBlock()) {
        // TODO: Dealing with exceptional back edges could be tricky because
        //       they only approximate the real control flow. Bail out for now.
        VLOG(compiler) << "Not compiled: Exceptional back edges";
        return kAnalysisFailThrowCatchLoop;
      }
      block->GetLoopInformation()->Populate();
    }
  }
  return kAnalysisSuccess;
}

void HLoopInformation::Dump(std::ostream& os) {
  os << "header: " << header_->GetBlockId() << std::endl;
  os << "pre header: " << GetPreHeader()->GetBlockId() << std::endl;
  for (HBasicBlock* block : back_edges_) {
    os << "back edge: " << block->GetBlockId() << std::endl;
  }
  for (HBasicBlock* block : header_->GetPredecessors()) {
    os << "predecessor: " << block->GetBlockId() << std::endl;
  }
  for (uint32_t idx : blocks_.Indexes()) {
    os << "  in loop: " << idx << std::endl;
  }
}

template <class InstructionType, typename ValueType>
InstructionType* HGraph::CreateConstant(ValueType value,
                                        ArenaSafeMap<ValueType, InstructionType*>* cache) {
  // Try to find an existing constant of the given value.
  InstructionType* constant = nullptr;
  auto cached_constant = cache->find(value);
  if (cached_constant != cache->end()) {
    constant = cached_constant->second;
  }

  // If not found or previously deleted, create and cache a new instruction.
  // Don't bother reviving a previously deleted instruction, for simplicity.
  if (constant == nullptr || constant->GetBlock() == nullptr) {
    constant = new (allocator_) InstructionType(value);
    cache->Overwrite(value, constant);
    InsertConstant(constant);
  }
  return constant;
}

void HGraph::InsertConstant(HConstant* constant) {
  // New constants are inserted before the SuspendCheck at the bottom of the
  // entry block. Note that this method can be called from the graph builder and
  // the entry block therefore may not end with SuspendCheck->Goto yet.
  HInstruction* insert_before = nullptr;

  HInstruction* gota = entry_block_->GetLastInstruction();
  if (gota != nullptr && gota->IsGoto()) {
    HInstruction* suspend_check = gota->GetPrevious();
    if (suspend_check != nullptr && suspend_check->IsSuspendCheck()) {
      insert_before = suspend_check;
    } else {
      insert_before = gota;
    }
  }

  if (insert_before == nullptr) {
    entry_block_->AddInstruction(constant);
  } else {
    entry_block_->InsertInstructionBefore(constant, insert_before);
  }
}

HNullConstant* HGraph::GetNullConstant() {
  // For simplicity, don't bother reviving the cached null constant if it is
  // not null and not in a block. Otherwise, we need to clear the instruction
  // id and/or any invariants the graph is assuming when adding new instructions.
  if ((cached_null_constant_ == nullptr) || (cached_null_constant_->GetBlock() == nullptr)) {
    cached_null_constant_ = new (allocator_) HNullConstant();
    cached_null_constant_->SetReferenceTypeInfo(GetInexactObjectRti());
    InsertConstant(cached_null_constant_);
  }
  if (kIsDebugBuild) {
    ScopedObjectAccess soa(Thread::Current());
    DCHECK(cached_null_constant_->GetReferenceTypeInfo().IsValid());
  }
  return cached_null_constant_;
}

HIntConstant* HGraph::GetIntConstant(int32_t value) {
  return CreateConstant(value, &cached_int_constants_);
}

HLongConstant* HGraph::GetLongConstant(int64_t value) {
  return CreateConstant(value, &cached_long_constants_);
}

HFloatConstant* HGraph::GetFloatConstant(float value) {
  return CreateConstant(bit_cast<int32_t, float>(value), &cached_float_constants_);
}

HDoubleConstant* HGraph::GetDoubleConstant(double value) {
  return CreateConstant(bit_cast<int64_t, double>(value), &cached_double_constants_);
}

HCurrentMethod* HGraph::GetCurrentMethod() {
  // For simplicity, don't bother reviving the cached current method if it is
  // not null and not in a block. Otherwise, we need to clear the instruction
  // id and/or any invariants the graph is assuming when adding new instructions.
  if ((cached_current_method_ == nullptr) || (cached_current_method_->GetBlock() == nullptr)) {
    cached_current_method_ = new (allocator_) HCurrentMethod(
        Is64BitInstructionSet(instruction_set_) ? DataType::Type::kInt64 : DataType::Type::kInt32,
        entry_block_->GetDexPc());
    if (entry_block_->GetFirstInstruction() == nullptr) {
      entry_block_->AddInstruction(cached_current_method_);
    } else {
      entry_block_->InsertInstructionBefore(
          cached_current_method_, entry_block_->GetFirstInstruction());
    }
  }
  return cached_current_method_;
}

const char* HGraph::GetMethodName() const {
  const dex::MethodId& method_id = dex_file_.GetMethodId(method_idx_);
  return dex_file_.GetMethodName(method_id);
}

std::string HGraph::PrettyMethod(bool with_signature) const {
  return dex_file_.PrettyMethod(method_idx_, with_signature);
}

HConstant* HGraph::GetConstant(DataType::Type type, int64_t value) {
  switch (type) {
    case DataType::Type::kBool:
      DCHECK(IsUint<1>(value));
      FALLTHROUGH_INTENDED;
    case DataType::Type::kUint8:
    case DataType::Type::kInt8:
    case DataType::Type::kUint16:
    case DataType::Type::kInt16:
    case DataType::Type::kInt32:
      DCHECK(IsInt(DataType::Size(type) * kBitsPerByte, value));
      return GetIntConstant(static_cast<int32_t>(value));

    case DataType::Type::kInt64:
      return GetLongConstant(value);

    default:
      LOG(FATAL) << "Unsupported constant type";
      UNREACHABLE();
  }
}

void HGraph::CacheFloatConstant(HFloatConstant* constant) {
  int32_t value = bit_cast<int32_t, float>(constant->GetValue());
  DCHECK(cached_float_constants_.find(value) == cached_float_constants_.end());
  cached_float_constants_.Overwrite(value, constant);
}

void HGraph::CacheDoubleConstant(HDoubleConstant* constant) {
  int64_t value = bit_cast<int64_t, double>(constant->GetValue());
  DCHECK(cached_double_constants_.find(value) == cached_double_constants_.end());
  cached_double_constants_.Overwrite(value, constant);
}

void HLoopInformation::Add(HBasicBlock* block) {
  blocks_.SetBit(block->GetBlockId());
}

void HLoopInformation::Remove(HBasicBlock* block) {
  blocks_.ClearBit(block->GetBlockId());
}

void HLoopInformation::PopulateRecursive(HBasicBlock* block) {
  if (blocks_.IsBitSet(block->GetBlockId())) {
    return;
  }

  blocks_.SetBit(block->GetBlockId());
  block->SetInLoop(this);
  if (block->IsLoopHeader()) {
    // We're visiting loops in post-order, so inner loops must have been
    // populated already.
    DCHECK(block->GetLoopInformation()->IsPopulated());
    if (block->GetLoopInformation()->IsIrreducible()) {
      contains_irreducible_loop_ = true;
    }
  }
  for (HBasicBlock* predecessor : block->GetPredecessors()) {
    PopulateRecursive(predecessor);
  }
}

void HLoopInformation::PopulateIrreducibleRecursive(HBasicBlock* block, ArenaBitVector* finalized) {
  size_t block_id = block->GetBlockId();

  // If `block` is in `finalized`, we know its membership in the loop has been
  // decided and it does not need to be revisited.
  if (finalized->IsBitSet(block_id)) {
    return;
  }

  bool is_finalized = false;
  if (block->IsLoopHeader()) {
    // If we hit a loop header in an irreducible loop, we first check if the
    // pre header of that loop belongs to the currently analyzed loop. If it does,
    // then we visit the back edges.
    // Note that we cannot use GetPreHeader, as the loop may have not been populated
    // yet.
    HBasicBlock* pre_header = block->GetPredecessors()[0];
    PopulateIrreducibleRecursive(pre_header, finalized);
    if (blocks_.IsBitSet(pre_header->GetBlockId())) {
      block->SetInLoop(this);
      blocks_.SetBit(block_id);
      finalized->SetBit(block_id);
      is_finalized = true;

      HLoopInformation* info = block->GetLoopInformation();
      for (HBasicBlock* back_edge : info->GetBackEdges()) {
        PopulateIrreducibleRecursive(back_edge, finalized);
      }
    }
  } else {
    // Visit all predecessors. If one predecessor is part of the loop, this
    // block is also part of this loop.
    for (HBasicBlock* predecessor : block->GetPredecessors()) {
      PopulateIrreducibleRecursive(predecessor, finalized);
      if (!is_finalized && blocks_.IsBitSet(predecessor->GetBlockId())) {
        block->SetInLoop(this);
        blocks_.SetBit(block_id);
        finalized->SetBit(block_id);
        is_finalized = true;
      }
    }
  }

  // All predecessors have been recursively visited. Mark finalized if not marked yet.
  if (!is_finalized) {
    finalized->SetBit(block_id);
  }
}

void HLoopInformation::Populate() {
  DCHECK_EQ(blocks_.NumSetBits(), 0u) << "Loop information has already been populated";
  // Populate this loop: starting with the back edge, recursively add predecessors
  // that are not already part of that loop. Set the header as part of the loop
  // to end the recursion.
  // This is a recursive implementation of the algorithm described in
  // "Advanced Compiler Design & Implementation" (Muchnick) p192.
  HGraph* graph = header_->GetGraph();
  blocks_.SetBit(header_->GetBlockId());
  header_->SetInLoop(this);

  bool is_irreducible_loop = HasBackEdgeNotDominatedByHeader();

  if (is_irreducible_loop) {
    // Allocate memory from local ScopedArenaAllocator.
    ScopedArenaAllocator allocator(graph->GetArenaStack());
    ArenaBitVector visited(&allocator,
                           graph->GetBlocks().size(),
                           /* expandable= */ false,
                           kArenaAllocGraphBuilder);
    // Stop marking blocks at the loop header.
    visited.SetBit(header_->GetBlockId());

    for (HBasicBlock* back_edge : GetBackEdges()) {
      PopulateIrreducibleRecursive(back_edge, &visited);
    }
  } else {
    for (HBasicBlock* back_edge : GetBackEdges()) {
      PopulateRecursive(back_edge);
    }
  }

  if (!is_irreducible_loop && graph->IsCompilingOsr()) {
    // When compiling in OSR mode, all loops in the compiled method may be entered
    // from the interpreter. We treat this OSR entry point just like an extra entry
    // to an irreducible loop, so we need to mark the method's loops as irreducible.
    // This does not apply to inlined loops which do not act as OSR entry points.
    if (suspend_check_ == nullptr) {
      // Just building the graph in OSR mode, this loop is not inlined. We never build an
      // inner graph in OSR mode as we can do OSR transition only from the outer method.
      is_irreducible_loop = true;
    } else {
      // Look at the suspend check's environment to determine if the loop was inlined.
      DCHECK(suspend_check_->HasEnvironment());
      if (!suspend_check_->GetEnvironment()->IsFromInlinedInvoke()) {
        is_irreducible_loop = true;
      }
    }
  }
  if (is_irreducible_loop) {
    irreducible_ = true;
    contains_irreducible_loop_ = true;
    graph->SetHasIrreducibleLoops(true);
  }
  graph->SetHasLoops(true);
}

void HLoopInformation::PopulateInnerLoopUpwards(HLoopInformation* inner_loop) {
  DCHECK(inner_loop->GetPreHeader()->GetLoopInformation() == this);
  blocks_.Union(&inner_loop->blocks_);
  HLoopInformation* outer_loop = GetPreHeader()->GetLoopInformation();
  if (outer_loop != nullptr) {
    outer_loop->PopulateInnerLoopUpwards(this);
  }
}

HBasicBlock* HLoopInformation::GetPreHeader() const {
  HBasicBlock* block = header_->GetPredecessors()[0];
  DCHECK(irreducible_ || (block == header_->GetDominator()));
  return block;
}

bool HLoopInformation::Contains(const HBasicBlock& block) const {
  return blocks_.IsBitSet(block.GetBlockId());
}

bool HLoopInformation::IsIn(const HLoopInformation& other) const {
  return other.blocks_.IsBitSet(header_->GetBlockId());
}

bool HLoopInformation::IsDefinedOutOfTheLoop(HInstruction* instruction) const {
  return !blocks_.IsBitSet(instruction->GetBlock()->GetBlockId());
}

size_t HLoopInformation::GetLifetimeEnd() const {
  size_t last_position = 0;
  for (HBasicBlock* back_edge : GetBackEdges()) {
    last_position = std::max(back_edge->GetLifetimeEnd(), last_position);
  }
  return last_position;
}

bool HLoopInformation::HasBackEdgeNotDominatedByHeader() const {
  for (HBasicBlock* back_edge : GetBackEdges()) {
    DCHECK(back_edge->GetDominator() != nullptr);
    if (!header_->Dominates(back_edge)) {
      return true;
    }
  }
  return false;
}

bool HLoopInformation::DominatesAllBackEdges(HBasicBlock* block) {
  for (HBasicBlock* back_edge : GetBackEdges()) {
    if (!block->Dominates(back_edge)) {
      return false;
    }
  }
  return true;
}


bool HLoopInformation::HasExitEdge() const {
  // Determine if this loop has at least one exit edge.
  HBlocksInLoopReversePostOrderIterator it_loop(*this);
  for (; !it_loop.Done(); it_loop.Advance()) {
    for (HBasicBlock* successor : it_loop.Current()->GetSuccessors()) {
      if (!Contains(*successor)) {
        return true;
      }
    }
  }
  return false;
}

bool HBasicBlock::Dominates(const HBasicBlock* other) const {
  // Walk up the dominator tree from `other`, to find out if `this`
  // is an ancestor.
  const HBasicBlock* current = other;
  while (current != nullptr) {
    if (current == this) {
      return true;
    }
    current = current->GetDominator();
  }
  return false;
}

static void UpdateInputsUsers(HGraph* graph, HInstruction* instruction) {
  HInputsRef inputs = instruction->GetInputs();
  if (inputs.size() != 0u) {
    ArenaAllocator* allocator = graph->GetAllocator();
    for (size_t i = 0; i < inputs.size(); ++i) {
      inputs[i]->AddUseAt(allocator, instruction, i);
    }
  }
  // Environment should be created later.
  DCHECK(!instruction->HasEnvironment());
}

void HBasicBlock::ReplaceAndRemovePhiWith(HPhi* initial, HPhi* replacement) {
  DCHECK(initial->GetBlock() == this);
  InsertPhiAfter(replacement, initial);
  initial->ReplaceWith(replacement);
  RemovePhi(initial);
}

void HBasicBlock::ReplaceAndRemoveInstructionWith(HInstruction* initial,
                                                  HInstruction* replacement) {
  DCHECK(initial->GetBlock() == this);
  if (initial->IsControlFlow()) {
    // We can only replace a control flow instruction with another control flow instruction.
    DCHECK(replacement->IsControlFlow());
    DCHECK_EQ(replacement->GetId(), -1);
    DCHECK_EQ(replacement->GetType(), DataType::Type::kVoid);
    DCHECK_EQ(initial->GetBlock(), this);
    DCHECK_EQ(initial->GetType(), DataType::Type::kVoid);
    DCHECK(initial->GetUses().empty());
    DCHECK(initial->GetEnvUses().empty());
    replacement->SetBlock(this);
    HGraph* graph = GetGraph();
    replacement->SetId(graph->AllocateInstructionId());
    instructions_.InsertInstructionBefore(replacement, initial);
    UpdateInputsUsers(graph, replacement);
  } else {
    InsertInstructionBefore(replacement, initial);
    initial->ReplaceWith(replacement);
  }
  RemoveInstruction(initial);
}

static void Add(HInstructionList* instruction_list,
                HBasicBlock* block,
                HInstruction* instruction) {
  DCHECK(instruction->GetBlock() == nullptr);
  DCHECK_EQ(instruction->GetId(), -1);
  instruction->SetBlock(block);
  HGraph* graph = block->GetGraph();
  instruction->SetId(graph->AllocateInstructionId());
  UpdateInputsUsers(graph, instruction);
  instruction_list->AddInstruction(instruction);
}

void HBasicBlock::AddInstruction(HInstruction* instruction) {
  Add(&instructions_, this, instruction);
}

void HBasicBlock::AddPhi(HPhi* phi) {
  Add(&phis_, this, phi);
}

void HBasicBlock::InsertInstructionBefore(HInstruction* instruction, HInstruction* cursor) {
  DCHECK(!cursor->IsPhi());
  DCHECK(!instruction->IsPhi());
  DCHECK_EQ(instruction->GetId(), -1);
  DCHECK_NE(cursor->GetId(), -1);
  DCHECK_EQ(cursor->GetBlock(), this);
  DCHECK(!instruction->IsControlFlow());
  instruction->SetBlock(this);
  HGraph* graph = GetGraph();
  instruction->SetId(graph->AllocateInstructionId());
  UpdateInputsUsers(graph, instruction);
  instructions_.InsertInstructionBefore(instruction, cursor);
}

void HBasicBlock::InsertInstructionAfter(HInstruction* instruction, HInstruction* cursor) {
  DCHECK(!cursor->IsPhi());
  DCHECK(!instruction->IsPhi());
  DCHECK_EQ(instruction->GetId(), -1);
  DCHECK_NE(cursor->GetId(), -1);
  DCHECK_EQ(cursor->GetBlock(), this);
  DCHECK(!instruction->IsControlFlow());
  DCHECK(!cursor->IsControlFlow());
  instruction->SetBlock(this);
  HGraph* graph = GetGraph();
  instruction->SetId(graph->AllocateInstructionId());
  UpdateInputsUsers(graph, instruction);
  instructions_.InsertInstructionAfter(instruction, cursor);
}

void HBasicBlock::InsertPhiAfter(HPhi* phi, HPhi* cursor) {
  DCHECK_EQ(phi->GetId(), -1);
  DCHECK_NE(cursor->GetId(), -1);
  DCHECK_EQ(cursor->GetBlock(), this);
  phi->SetBlock(this);
  HGraph* graph = GetGraph();
  phi->SetId(graph->AllocateInstructionId());
  UpdateInputsUsers(graph, phi);
  phis_.InsertInstructionAfter(phi, cursor);
}

static void Remove(HInstructionList* instruction_list,
                   HBasicBlock* block,
                   HInstruction* instruction,
                   bool ensure_safety) {
  DCHECK_EQ(block, instruction->GetBlock());
  instruction->SetBlock(nullptr);
  instruction_list->RemoveInstruction(instruction);
  if (ensure_safety) {
    DCHECK(instruction->GetUses().empty());
    DCHECK(instruction->GetEnvUses().empty());
    RemoveAsUser(instruction);
  }
}

void HBasicBlock::RemoveInstruction(HInstruction* instruction, bool ensure_safety) {
  DCHECK(!instruction->IsPhi());
  Remove(&instructions_, this, instruction, ensure_safety);
}

void HBasicBlock::RemovePhi(HPhi* phi, bool ensure_safety) {
  Remove(&phis_, this, phi, ensure_safety);
}

void HBasicBlock::RemoveInstructionOrPhi(HInstruction* instruction, bool ensure_safety) {
  if (instruction->IsPhi()) {
    RemovePhi(instruction->AsPhi(), ensure_safety);
  } else {
    RemoveInstruction(instruction, ensure_safety);
  }
}

void HEnvironment::CopyFrom(ArenaAllocator* allocator, ArrayRef<HInstruction* const> locals) {
  DCHECK_EQ(locals.size(), Size());
  for (size_t i = 0; i < locals.size(); i++) {
    HInstruction* instruction = locals[i];
    SetRawEnvAt(i, instruction);
    if (instruction != nullptr) {
      instruction->AddEnvUseAt(allocator, this, i);
    }
  }
}

void HEnvironment::CopyFrom(ArenaAllocator* allocator, const HEnvironment* env) {
  DCHECK_EQ(env->Size(), Size());
  for (size_t i = 0; i < env->Size(); i++) {
    HInstruction* instruction = env->GetInstructionAt(i);
    SetRawEnvAt(i, instruction);
    if (instruction != nullptr) {
      instruction->AddEnvUseAt(allocator, this, i);
    }
  }
}

void HEnvironment::CopyFromWithLoopPhiAdjustment(ArenaAllocator* allocator,
                                                 HEnvironment* env,
                                                 HBasicBlock* loop_header) {
  DCHECK(loop_header->IsLoopHeader());
  for (size_t i = 0; i < env->Size(); i++) {
    HInstruction* instruction = env->GetInstructionAt(i);
    SetRawEnvAt(i, instruction);
    if (instruction == nullptr) {
      continue;
    }
    if (instruction->IsLoopHeaderPhi() && (instruction->GetBlock() == loop_header)) {
      // At the end of the loop pre-header, the corresponding value for instruction
      // is the first input of the phi.
      HInstruction* initial = instruction->AsPhi()->InputAt(0);
      SetRawEnvAt(i, initial);
      initial->AddEnvUseAt(allocator, this, i);
    } else {
      instruction->AddEnvUseAt(allocator, this, i);
    }
  }
}

void HEnvironment::RemoveAsUserOfInput(size_t index) const {
  const HUserRecord<HEnvironment*>& env_use = GetVRegs()[index];
  HInstruction* user = env_use.GetInstruction();
  auto before_env_use_node = env_use.GetBeforeUseNode();
  user->env_uses_.erase_after(before_env_use_node);
  user->FixUpUserRecordsAfterEnvUseRemoval(before_env_use_node);
}

void HEnvironment::ReplaceInput(HInstruction* replacement, size_t index) {
  const HUserRecord<HEnvironment*>& env_use_record = GetVRegs()[index];
  HInstruction* orig_instr = env_use_record.GetInstruction();

  DCHECK(orig_instr != replacement);

  HUseList<HEnvironment*>::iterator before_use_node = env_use_record.GetBeforeUseNode();
  // Note: fixup_end remains valid across splice_after().
  auto fixup_end = replacement->env_uses_.empty() ? replacement->env_uses_.begin()
                                                  : ++replacement->env_uses_.begin();
  replacement->env_uses_.splice_after(replacement->env_uses_.before_begin(),
                                      env_use_record.GetInstruction()->env_uses_,
                                      before_use_node);
  replacement->FixUpUserRecordsAfterEnvUseInsertion(fixup_end);
  orig_instr->FixUpUserRecordsAfterEnvUseRemoval(before_use_node);
}

std::ostream& HInstruction::Dump(std::ostream& os, bool dump_args) {
  // Note: Handle the case where the instruction has been removed from
  // the graph to support debugging output for failed gtests.
  HGraph* graph = (GetBlock() != nullptr) ? GetBlock()->GetGraph() : nullptr;
  HGraphVisualizer::DumpInstruction(&os, graph, this);
  if (dump_args) {
    // Allocate memory from local ScopedArenaAllocator.
    std::optional<MallocArenaPool> local_arena_pool;
    std::optional<ArenaStack> local_arena_stack;
    if (UNLIKELY(graph == nullptr)) {
      local_arena_pool.emplace();
      local_arena_stack.emplace(&local_arena_pool.value());
    }
    ScopedArenaAllocator allocator(
        graph != nullptr ? graph->GetArenaStack() : &local_arena_stack.value());
    // Instructions that we already visited. We print each instruction only once.
    ArenaBitVector visited(&allocator,
                           (graph != nullptr) ? graph->GetCurrentInstructionId() : 0u,
                           /* expandable= */ (graph == nullptr),
                           kArenaAllocMisc);
    visited.SetBit(GetId());
    // Keep a queue of instructions with their indentations.
    ScopedArenaDeque<std::pair<HInstruction*, size_t>> queue(allocator.Adapter(kArenaAllocMisc));
    auto add_args = [&queue](HInstruction* instruction, size_t indentation) {
      for (HInstruction* arg : ReverseRange(instruction->GetInputs())) {
        queue.emplace_front(arg, indentation);
      }
    };
    add_args(this, /*indentation=*/ 1u);
    while (!queue.empty()) {
      HInstruction* instruction;
      size_t indentation;
      std::tie(instruction, indentation) = queue.front();
      queue.pop_front();
      if (!visited.IsBitSet(instruction->GetId())) {
        visited.SetBit(instruction->GetId());
        os << '\n';
        for (size_t i = 0; i != indentation; ++i) {
          os << "  ";
        }
        HGraphVisualizer::DumpInstruction(&os, graph, instruction);
        add_args(instruction, indentation + 1u);
      }
    }
  }
  return os;
}

HInstruction* HInstruction::GetNextDisregardingMoves() const {
  HInstruction* next = GetNext();
  while (next != nullptr && next->IsParallelMove()) {
    next = next->GetNext();
  }
  return next;
}

HInstruction* HInstruction::GetPreviousDisregardingMoves() const {
  HInstruction* previous = GetPrevious();
  while (previous != nullptr && previous->IsParallelMove()) {
    previous = previous->GetPrevious();
  }
  return previous;
}

void HInstructionList::AddInstruction(HInstruction* instruction) {
  if (first_instruction_ == nullptr) {
    DCHECK(last_instruction_ == nullptr);
    first_instruction_ = last_instruction_ = instruction;
  } else {
    DCHECK(last_instruction_ != nullptr);
    last_instruction_->next_ = instruction;
    instruction->previous_ = last_instruction_;
    last_instruction_ = instruction;
  }
}

void HInstructionList::InsertInstructionBefore(HInstruction* instruction, HInstruction* cursor) {
  DCHECK(Contains(cursor));
  if (cursor == first_instruction_) {
    cursor->previous_ = instruction;
    instruction->next_ = cursor;
    first_instruction_ = instruction;
  } else {
    instruction->previous_ = cursor->previous_;
    instruction->next_ = cursor;
    cursor->previous_ = instruction;
    instruction->previous_->next_ = instruction;
  }
}

void HInstructionList::InsertInstructionAfter(HInstruction* instruction, HInstruction* cursor) {
  DCHECK(Contains(cursor));
  if (cursor == last_instruction_) {
    cursor->next_ = instruction;
    instruction->previous_ = cursor;
    last_instruction_ = instruction;
  } else {
    instruction->next_ = cursor->next_;
    instruction->previous_ = cursor;
    cursor->next_ = instruction;
    instruction->next_->previous_ = instruction;
  }
}

void HInstructionList::RemoveInstruction(HInstruction* instruction) {
  DCHECK_EQ(instruction->previous_ == nullptr, instruction == first_instruction_);
  DCHECK_EQ(instruction->next_ == nullptr, instruction == last_instruction_);

  if (instruction == first_instruction_) {
    first_instruction_ = instruction->next_;
  } else {
    instruction->previous_->next_ = instruction->next_;
  }

  if (instruction == last_instruction_) {
    last_instruction_ = instruction->previous_;
  } else {
    instruction->next_->previous_ = instruction->previous_;
  }
}

bool HInstructionList::Contains(HInstruction* instruction) const {
  for (HInstructionIterator it(*this); !it.Done(); it.Advance()) {
    if (it.Current() == instruction) {
      return true;
    }
  }
  return false;
}

bool HInstructionList::FoundBefore(const HInstruction* instruction1,
                                   const HInstruction* instruction2) const {
  DCHECK_EQ(instruction1->GetBlock(), instruction2->GetBlock());
  for (HInstructionIterator it(*this); !it.Done(); it.Advance()) {
    if (it.Current() == instruction2) {
      return false;
    }
    if (it.Current() == instruction1) {
      return true;
    }
  }
  LOG(FATAL) << "Did not find an order between two instructions of the same block.";
  UNREACHABLE();
}

bool HInstruction::Dominates(HInstruction* other_instruction) const {
  return other_instruction == this || StrictlyDominates(other_instruction);
}

bool HInstruction::StrictlyDominates(HInstruction* other_instruction) const {
  if (other_instruction == this) {
    // An instruction does not strictly dominate itself.
    return false;
  }
  HBasicBlock* block = GetBlock();
  HBasicBlock* other_block = other_instruction->GetBlock();
  if (block != other_block) {
    return GetBlock()->Dominates(other_instruction->GetBlock());
  } else {
    // If both instructions are in the same block, ensure this
    // instruction comes before `other_instruction`.
    if (IsPhi()) {
      if (!other_instruction->IsPhi()) {
        // Phis appear before non phi-instructions so this instruction
        // dominates `other_instruction`.
        return true;
      } else {
        // There is no order among phis.
        LOG(FATAL) << "There is no dominance between phis of a same block.";
        UNREACHABLE();
      }
    } else {
      // `this` is not a phi.
      if (other_instruction->IsPhi()) {
        // Phis appear before non phi-instructions so this instruction
        // does not dominate `other_instruction`.
        return false;
      } else {
        // Check whether this instruction comes before
        // `other_instruction` in the instruction list.
        return block->GetInstructions().FoundBefore(this, other_instruction);
      }
    }
  }
}

void HInstruction::RemoveEnvironment() {
  RemoveEnvironmentUses(this);
  environment_ = nullptr;
}

void HInstruction::ReplaceWith(HInstruction* other) {
  DCHECK(other != nullptr);
  // Note: fixup_end remains valid across splice_after().
  auto fixup_end = other->uses_.empty() ? other->uses_.begin() : ++other->uses_.begin();
  other->uses_.splice_after(other->uses_.before_begin(), uses_);
  other->FixUpUserRecordsAfterUseInsertion(fixup_end);

  // Note: env_fixup_end remains valid across splice_after().
  auto env_fixup_end =
      other->env_uses_.empty() ? other->env_uses_.begin() : ++other->env_uses_.begin();
  other->env_uses_.splice_after(other->env_uses_.before_begin(), env_uses_);
  other->FixUpUserRecordsAfterEnvUseInsertion(env_fixup_end);

  DCHECK(uses_.empty());
  DCHECK(env_uses_.empty());
}

void HInstruction::ReplaceUsesDominatedBy(HInstruction* dominator,
                                          HInstruction* replacement,
                                          bool strictly_dominated) {
  HBasicBlock* dominator_block = dominator->GetBlock();
  BitVectorView<size_t> visited_blocks;

  // Lazily compute the dominated blocks to faster calculation of domination afterwards.
  auto maybe_generate_visited_blocks = [&visited_blocks, this, dominator_block]() {
    if (visited_blocks.SizeInBits() != 0u) {
      DCHECK_EQ(visited_blocks.SizeInBits(), GetBlock()->GetGraph()->GetBlocks().size());
      return;
    }
    HGraph* graph = GetBlock()->GetGraph();
    visited_blocks = ArenaBitVector::CreateFixedSize(
        graph->GetAllocator(), graph->GetBlocks().size(), kArenaAllocMisc);
    ScopedArenaAllocator allocator(graph->GetArenaStack());
    ScopedArenaQueue<const HBasicBlock*> worklist(allocator.Adapter(kArenaAllocMisc));
    worklist.push(dominator_block);

    while (!worklist.empty()) {
      const HBasicBlock* current = worklist.front();
      worklist.pop();
      visited_blocks.SetBit(current->GetBlockId());
      for (HBasicBlock* dominated : current->GetDominatedBlocks()) {
        if (visited_blocks.IsBitSet(dominated->GetBlockId())) {
          continue;
        }
        worklist.push(dominated);
      }
    }
  };

  const HUseList<HInstruction*>& uses = GetUses();
  for (auto it = uses.begin(), end = uses.end(); it != end; /* ++it below */) {
    HInstruction* user = it->GetUser();
    HBasicBlock* block = user->GetBlock();
    size_t index = it->GetIndex();
    // Increment `it` now because `*it` may disappear thanks to user->ReplaceInput().
    ++it;
    bool dominated = false;
    if (dominator_block == block) {
      // Trickier case, call the other methods.
      dominated =
          strictly_dominated ? dominator->StrictlyDominates(user) : dominator->Dominates(user);
    } else {
      // Block domination.
      maybe_generate_visited_blocks();
      dominated = visited_blocks.IsBitSet(block->GetBlockId());
    }

    if (dominated) {
      user->ReplaceInput(replacement, index);
    } else if (user->IsPhi() && !user->AsPhi()->IsCatchPhi()) {
      // If the input flows from a block dominated by `dominator`, we can replace it.
      // We do not perform this for catch phis as we don't have control flow support
      // for their inputs.
      HBasicBlock* predecessor = block->GetPredecessors()[index];
      maybe_generate_visited_blocks();
      if (visited_blocks.IsBitSet(predecessor->GetBlockId())) {
        user->ReplaceInput(replacement, index);
      }
    }
  }
}

void HInstruction::ReplaceEnvUsesDominatedBy(HInstruction* dominator, HInstruction* replacement) {
  const HUseList<HEnvironment*>& uses = GetEnvUses();
  for (auto it = uses.begin(), end = uses.end(); it != end; /* ++it below */) {
    HEnvironment* user = it->GetUser();
    size_t index = it->GetIndex();
    // Increment `it` now because `*it` may disappear thanks to user->ReplaceInput().
    ++it;
    if (dominator->StrictlyDominates(user->GetHolder())) {
      user->ReplaceInput(replacement, index);
    }
  }
}

void HInstruction::ReplaceInput(HInstruction* replacement, size_t index) {
  HUserRecord<HInstruction*> input_use = InputRecordAt(index);
  if (input_use.GetInstruction() == replacement) {
    // Nothing to do.
    return;
  }
  HUseList<HInstruction*>::iterator before_use_node = input_use.GetBeforeUseNode();
  // Note: fixup_end remains valid across splice_after().
  auto fixup_end =
      replacement->uses_.empty() ? replacement->uses_.begin() : ++replacement->uses_.begin();
  replacement->uses_.splice_after(replacement->uses_.before_begin(),
                                  input_use.GetInstruction()->uses_,
                                  before_use_node);
  replacement->FixUpUserRecordsAfterUseInsertion(fixup_end);
  input_use.GetInstruction()->FixUpUserRecordsAfterUseRemoval(before_use_node);
}

size_t HInstruction::EnvironmentSize() const {
  return HasEnvironment() ? environment_->Size() : 0;
}

void HVariableInputSizeInstruction::AddInput(HInstruction* input) {
  DCHECK(input->GetBlock() != nullptr);
  inputs_.push_back(HUserRecord<HInstruction*>(input));
  input->AddUseAt(GetBlock()->GetGraph()->GetAllocator(), this, inputs_.size() - 1);
}

void HVariableInputSizeInstruction::InsertInputAt(size_t index, HInstruction* input) {
  inputs_.insert(inputs_.begin() + index, HUserRecord<HInstruction*>(input));
  // Update indexes in use nodes of inputs that have been pushed further back by the insert().
  for (size_t i = index + 1u, e = inputs_.size(); i < e; ++i) {
    DCHECK_EQ(inputs_[i].GetUseNode()->GetIndex(), i - 1u);
    inputs_[i].GetUseNode()->SetIndex(i);
  }
  // Add the use after updating the indexes. If the `input` is already used by `this`,
  // the fixup after use insertion can use those indexes.
  input->AddUseAt(GetBlock()->GetGraph()->GetAllocator(), this, index);
}

void HVariableInputSizeInstruction::RemoveInputAt(size_t index) {
  RemoveAsUserOfInput(index);
  inputs_.erase(inputs_.begin() + index);
  // Update indexes in use nodes of inputs that have been pulled forward by the erase().
  for (size_t i = index, e = inputs_.size(); i < e; ++i) {
    DCHECK_EQ(inputs_[i].GetUseNode()->GetIndex(), i + 1u);
    inputs_[i].GetUseNode()->SetIndex(i);
  }
}

void HVariableInputSizeInstruction::RemoveAllInputs() {
  RemoveAsUserOfAllInputs();
  DCHECK(!HasNonEnvironmentUses());

  inputs_.clear();
  DCHECK_EQ(0u, InputCount());
}

size_t HConstructorFence::RemoveConstructorFences(HInstruction* instruction) {
  DCHECK(instruction->GetBlock() != nullptr);
  // Removing constructor fences only makes sense for instructions with an object return type.
  DCHECK_EQ(DataType::Type::kReference, instruction->GetType());

  // Return how many instructions were removed for statistic purposes.
  size_t remove_count = 0;

  // Efficient implementation that simultaneously (in one pass):
  // * Scans the uses list for all constructor fences.
  // * Deletes that constructor fence from the uses list of `instruction`.
  // * Deletes `instruction` from the constructor fence's inputs.
  // * Deletes the constructor fence if it now has 0 inputs.

  const HUseList<HInstruction*>& uses = instruction->GetUses();
  // Warning: Although this is "const", we might mutate the list when calling RemoveInputAt.
  for (auto it = uses.begin(), end = uses.end(); it != end; ) {
    const HUseListNode<HInstruction*>& use_node = *it;
    HInstruction* const use_instruction = use_node.GetUser();

    // Advance the iterator immediately once we fetch the use_node.
    // Warning: If the input is removed, the current iterator becomes invalid.
    ++it;

    if (use_instruction->IsConstructorFence()) {
      HConstructorFence* ctor_fence = use_instruction->AsConstructorFence();
      size_t input_index = use_node.GetIndex();

      // Process the candidate instruction for removal
      // from the graph.

      // Constructor fence instructions are never
      // used by other instructions.
      //
      // If we wanted to make this more generic, it
      // could be a runtime if statement.
      DCHECK(!ctor_fence->HasUses());

      // A constructor fence's return type is "kPrimVoid"
      // and therefore it can't have any environment uses.
      DCHECK(!ctor_fence->HasEnvironmentUses());

      // Remove the inputs first, otherwise removing the instruction
      // will try to remove its uses while we are already removing uses
      // and this operation will fail.
      DCHECK_EQ(instruction, ctor_fence->InputAt(input_index));

      // Removing the input will also remove the `use_node`.
      // (Do not look at `use_node` after this, it will be a dangling reference).
      ctor_fence->RemoveInputAt(input_index);

      // Once all inputs are removed, the fence is considered dead and
      // is removed.
      if (ctor_fence->InputCount() == 0u) {
        ctor_fence->GetBlock()->RemoveInstruction(ctor_fence);
        ++remove_count;
      }
    }
  }

  if (kIsDebugBuild) {
    // Post-condition checks:
    // * None of the uses of `instruction` are a constructor fence.
    // * The `instruction` itself did not get removed from a block.
    for (const HUseListNode<HInstruction*>& use_node : instruction->GetUses()) {
      CHECK(!use_node.GetUser()->IsConstructorFence());
    }
    CHECK(instruction->GetBlock() != nullptr);
  }

  return remove_count;
}

void HConstructorFence::Merge(HConstructorFence* other) {
  // Do not delete yourself from the graph.
  DCHECK(this != other);
  // Don't try to merge with an instruction not associated with a block.
  DCHECK(other->GetBlock() != nullptr);
  // A constructor fence's return type is "kPrimVoid"
  // and therefore it cannot have any environment uses.
  DCHECK(!other->HasEnvironmentUses());

  auto has_input = [](HInstruction* haystack, HInstruction* needle) {
    // Check if `haystack` has `needle` as any of its inputs.
    for (size_t input_count = 0; input_count < haystack->InputCount(); ++input_count) {
      if (haystack->InputAt(input_count) == needle) {
        return true;
      }
    }
    return false;
  };

  // Add any inputs from `other` into `this` if it wasn't already an input.
  for (size_t input_count = 0; input_count < other->InputCount(); ++input_count) {
    HInstruction* other_input = other->InputAt(input_count);
    if (!has_input(this, other_input)) {
      AddInput(other_input);
    }
  }

  other->GetBlock()->RemoveInstruction(other);
}

HInstruction* HConstructorFence::GetAssociatedAllocation(bool ignore_inputs) {
  HInstruction* new_instance_inst = GetPrevious();
  // Check if the immediately preceding instruction is a new-instance/new-array.
  // Otherwise this fence is for protecting final fields.
  if (new_instance_inst != nullptr &&
      (new_instance_inst->IsNewInstance() || new_instance_inst->IsNewArray())) {
    if (ignore_inputs) {
      // If inputs are ignored, simply check if the predecessor is
      // *any* HNewInstance/HNewArray.
      //
      // Inputs are normally only ignored for prepare_for_register_allocation,
      // at which point *any* prior HNewInstance/Array can be considered
      // associated.
      return new_instance_inst;
    } else {
      // Normal case: There must be exactly 1 input and the previous instruction
      // must be that input.
      if (InputCount() == 1u && InputAt(0) == new_instance_inst) {
        return new_instance_inst;
      }
    }
  }
  return nullptr;
}

#define DEFINE_ACCEPT(name, super)                                             \
void H##name::Accept(HGraphVisitor* visitor) {                                 \
  visitor->Visit##name(this);                                                  \
}

FOR_EACH_CONCRETE_INSTRUCTION(DEFINE_ACCEPT)

#undef DEFINE_ACCEPT

void HGraphVisitor::VisitInsertionOrder() {
  for (HBasicBlock* block : graph_->GetActiveBlocks()) {
    VisitBasicBlock(block);
  }
}

void HGraphVisitor::VisitReversePostOrder() {
  for (HBasicBlock* block : graph_->GetReversePostOrder()) {
    VisitBasicBlock(block);
  }
}

void HGraphVisitor::VisitBasicBlock(HBasicBlock* block) {
  VisitPhis(block);
  VisitNonPhiInstructions(block);
}

void HGraphVisitor::VisitPhis(HBasicBlock* block) {
  for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
    DCHECK(it.Current()->IsPhi());
    VisitPhi(it.Current()->AsPhi());
  }
}

void HGraphVisitor::VisitNonPhiInstructions(HBasicBlock* block) {
  for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
    DCHECK(!it.Current()->IsPhi());
    it.Current()->Accept(this);
  }
}

void HGraphVisitor::VisitNonPhiInstructionsHandleChanges(HBasicBlock* block) {
  for (HInstructionIteratorHandleChanges it(block->GetInstructions()); !it.Done(); it.Advance()) {
    DCHECK(!it.Current()->IsPhi());
    it.Current()->Accept(this);
  }
}

HConstant* HTypeConversion::TryStaticEvaluation() const { return TryStaticEvaluation(GetInput()); }

HConstant* HTypeConversion::TryStaticEvaluation(HInstruction* input) const {
  HGraph* graph = input->GetBlock()->GetGraph();
  if (input->IsIntConstant()) {
    int32_t value = input->AsIntConstant()->GetValue();
    switch (GetResultType()) {
      case DataType::Type::kInt8:
        return graph->GetIntConstant(static_cast<int8_t>(value));
      case DataType::Type::kUint8:
        return graph->GetIntConstant(static_cast<uint8_t>(value));
      case DataType::Type::kInt16:
        return graph->GetIntConstant(static_cast<int16_t>(value));
      case DataType::Type::kUint16:
        return graph->GetIntConstant(static_cast<uint16_t>(value));
      case DataType::Type::kInt64:
        return graph->GetLongConstant(static_cast<int64_t>(value));
      case DataType::Type::kFloat32:
        return graph->GetFloatConstant(static_cast<float>(value));
      case DataType::Type::kFloat64:
        return graph->GetDoubleConstant(static_cast<double>(value));
      default:
        return nullptr;
    }
  } else if (input->IsLongConstant()) {
    int64_t value = input->AsLongConstant()->GetValue();
    switch (GetResultType()) {
      case DataType::Type::kInt8:
        return graph->GetIntConstant(static_cast<int8_t>(value));
      case DataType::Type::kUint8:
        return graph->GetIntConstant(static_cast<uint8_t>(value));
      case DataType::Type::kInt16:
        return graph->GetIntConstant(static_cast<int16_t>(value));
      case DataType::Type::kUint16:
        return graph->GetIntConstant(static_cast<uint16_t>(value));
      case DataType::Type::kInt32:
        return graph->GetIntConstant(static_cast<int32_t>(value));
      case DataType::Type::kFloat32:
        return graph->GetFloatConstant(static_cast<float>(value));
      case DataType::Type::kFloat64:
        return graph->GetDoubleConstant(static_cast<double>(value));
      default:
        return nullptr;
    }
  } else if (input->IsFloatConstant()) {
    float value = input->AsFloatConstant()->GetValue();
    switch (GetResultType()) {
      case DataType::Type::kInt32:
        if (std::isnan(value))
          return graph->GetIntConstant(0);
        if (value >= static_cast<float>(kPrimIntMax))
          return graph->GetIntConstant(kPrimIntMax);
        if (value <= kPrimIntMin)
          return graph->GetIntConstant(kPrimIntMin);
        return graph->GetIntConstant(static_cast<int32_t>(value));
      case DataType::Type::kInt64:
        if (std::isnan(value))
          return graph->GetLongConstant(0);
        if (value >= static_cast<float>(kPrimLongMax))
          return graph->GetLongConstant(kPrimLongMax);
        if (value <= kPrimLongMin)
          return graph->GetLongConstant(kPrimLongMin);
        return graph->GetLongConstant(static_cast<int64_t>(value));
      case DataType::Type::kFloat64:
        return graph->GetDoubleConstant(static_cast<double>(value));
      default:
        return nullptr;
    }
  } else if (input->IsDoubleConstant()) {
    double value = input->AsDoubleConstant()->GetValue();
    switch (GetResultType()) {
      case DataType::Type::kInt32:
        if (std::isnan(value))
          return graph->GetIntConstant(0);
        if (value >= kPrimIntMax)
          return graph->GetIntConstant(kPrimIntMax);
        if (value <= kPrimLongMin)
          return graph->GetIntConstant(kPrimIntMin);
        return graph->GetIntConstant(static_cast<int32_t>(value));
      case DataType::Type::kInt64:
        if (std::isnan(value))
          return graph->GetLongConstant(0);
        if (value >= static_cast<double>(kPrimLongMax))
          return graph->GetLongConstant(kPrimLongMax);
        if (value <= kPrimLongMin)
          return graph->GetLongConstant(kPrimLongMin);
        return graph->GetLongConstant(static_cast<int64_t>(value));
      case DataType::Type::kFloat32:
        return graph->GetFloatConstant(static_cast<float>(value));
      default:
        return nullptr;
    }
  }
  return nullptr;
}

HConstant* HUnaryOperation::TryStaticEvaluation() const { return TryStaticEvaluation(GetInput()); }

HConstant* HUnaryOperation::TryStaticEvaluation(HInstruction* input) const {
  if (input->IsIntConstant()) {
    return Evaluate(input->AsIntConstant());
  } else if (input->IsLongConstant()) {
    return Evaluate(input->AsLongConstant());
  } else if (kEnableFloatingPointStaticEvaluation) {
    if (input->IsFloatConstant()) {
      return Evaluate(input->AsFloatConstant());
    } else if (input->IsDoubleConstant()) {
      return Evaluate(input->AsDoubleConstant());
    }
  }
  return nullptr;
}

HConstant* HBinaryOperation::TryStaticEvaluation() const {
  return TryStaticEvaluation(GetLeft(), GetRight());
}

HConstant* HBinaryOperation::TryStaticEvaluation(HInstruction* left, HInstruction* right) const {
  if (left->IsIntConstant() && right->IsIntConstant()) {
    return Evaluate(left->AsIntConstant(), right->AsIntConstant());
  } else if (left->IsLongConstant()) {
    if (right->IsIntConstant()) {
      // The binop(long, int) case is only valid for shifts and rotations.
      DCHECK(IsShl() || IsShr() || IsUShr() || IsRol() || IsRor()) << DebugName();
      return Evaluate(left->AsLongConstant(), right->AsIntConstant());
    } else if (right->IsLongConstant()) {
      return Evaluate(left->AsLongConstant(), right->AsLongConstant());
    }
  } else if (left->IsNullConstant() && right->IsNullConstant()) {
    // The binop(null, null) case is only valid for equal and not-equal conditions.
    DCHECK(IsEqual() || IsNotEqual()) << DebugName();
    return Evaluate(left->AsNullConstant(), right->AsNullConstant());
  } else if (kEnableFloatingPointStaticEvaluation) {
    if (left->IsFloatConstant() && right->IsFloatConstant()) {
      return Evaluate(left->AsFloatConstant(), right->AsFloatConstant());
    } else if (left->IsDoubleConstant() && right->IsDoubleConstant()) {
      return Evaluate(left->AsDoubleConstant(), right->AsDoubleConstant());
    }
  }
  return nullptr;
}

HConstant* HBinaryOperation::GetConstantRight() const {
  if (GetRight()->IsConstant()) {
    return GetRight()->AsConstant();
  } else if (IsCommutative() && GetLeft()->IsConstant()) {
    return GetLeft()->AsConstant();
  } else {
    return nullptr;
  }
}

// If `GetConstantRight()` returns one of the input, this returns the other
// one. Otherwise it returns null.
HInstruction* HBinaryOperation::GetLeastConstantLeft() const {
  HInstruction* most_constant_right = GetConstantRight();
  if (most_constant_right == nullptr) {
    return nullptr;
  } else if (most_constant_right == GetLeft()) {
    return GetRight();
  } else {
    return GetLeft();
  }
}

std::ostream& operator<<(std::ostream& os, ComparisonBias rhs) {
  // TODO: Replace with auto-generated operator<<.
  switch (rhs) {
    case ComparisonBias::kNoBias:
      return os << "none";
    case ComparisonBias::kGtBias:
      return os << "gt";
    case ComparisonBias::kLtBias:
      return os << "lt";
  }
}

HCondition* HCondition::Create(HGraph* graph,
                               IfCondition cond,
                               HInstruction* lhs,
                               HInstruction* rhs,
                               uint32_t dex_pc) {
  ArenaAllocator* allocator = graph->GetAllocator();
  switch (cond) {
    case kCondEQ: return new (allocator) HEqual(lhs, rhs, dex_pc);
    case kCondNE: return new (allocator) HNotEqual(lhs, rhs, dex_pc);
    case kCondLT: return new (allocator) HLessThan(lhs, rhs, dex_pc);
    case kCondLE: return new (allocator) HLessThanOrEqual(lhs, rhs, dex_pc);
    case kCondGT: return new (allocator) HGreaterThan(lhs, rhs, dex_pc);
    case kCondGE: return new (allocator) HGreaterThanOrEqual(lhs, rhs, dex_pc);
    case kCondB:  return new (allocator) HBelow(lhs, rhs, dex_pc);
    case kCondBE: return new (allocator) HBelowOrEqual(lhs, rhs, dex_pc);
    case kCondA:  return new (allocator) HAbove(lhs, rhs, dex_pc);
    case kCondAE: return new (allocator) HAboveOrEqual(lhs, rhs, dex_pc);
    default:
      LOG(FATAL) << "Unexpected condition " << cond;
      UNREACHABLE();
  }
}

bool HCondition::IsBeforeWhenDisregardMoves(HInstruction* instruction) const {
  return this == instruction->GetPreviousDisregardingMoves();
}

bool HInstruction::Equals(const HInstruction* other) const {
  if (GetKind() != other->GetKind()) return false;
  if (GetType() != other->GetType()) return false;
  if (!InstructionDataEquals(other)) return false;
  HConstInputsRef inputs = GetInputs();
  HConstInputsRef other_inputs = other->GetInputs();
  if (inputs.size() != other_inputs.size()) return false;
  for (size_t i = 0; i != inputs.size(); ++i) {
    if (inputs[i] != other_inputs[i]) return false;
  }

  DCHECK_EQ(ComputeHashCode(), other->ComputeHashCode());
  return true;
}

std::ostream& operator<<(std::ostream& os, HInstruction::InstructionKind rhs) {
#define DECLARE_CASE(type, super) case HInstruction::k##type: os << #type; break;
  switch (rhs) {
    FOR_EACH_CONCRETE_INSTRUCTION(DECLARE_CASE)
    default:
      os << "Unknown instruction kind " << static_cast<int>(rhs);
      break;
  }
#undef DECLARE_CASE
  return os;
}

std::ostream& operator<<(std::ostream& os, const HInstruction::NoArgsDump rhs) {
  // TODO Really this should be const but that would require const-ifying
  // graph-visualizer and HGraphVisitor which are tangled up everywhere.
  return const_cast<HInstruction*>(rhs.ins)->Dump(os, /* dump_args= */ false);
}

std::ostream& operator<<(std::ostream& os, const HInstruction::ArgsDump rhs) {
  // TODO Really this should be const but that would require const-ifying
  // graph-visualizer and HGraphVisitor which are tangled up everywhere.
  return const_cast<HInstruction*>(rhs.ins)->Dump(os, /* dump_args= */ true);
}

std::ostream& operator<<(std::ostream& os, const HInstruction& rhs) {
  return os << rhs.DumpWithoutArgs();
}

std::ostream& operator<<(std::ostream& os, const HUseList<HInstruction*>& lst) {
  os << "Instructions[";
  bool first = true;
  for (const auto& hi : lst) {
    if (!first) {
      os << ", ";
    }
    first = false;
    os << hi.GetUser()->DebugName() << "[id: " << hi.GetUser()->GetId()
       << ", blk: " << hi.GetUser()->GetBlock()->GetBlockId() << "]@" << hi.GetIndex();
  }
  os << "]";
  return os;
}

std::ostream& operator<<(std::ostream& os, const HUseList<HEnvironment*>& lst) {
  os << "Environments[";
  bool first = true;
  for (const auto& hi : lst) {
    if (!first) {
      os << ", ";
    }
    first = false;
    os << *hi.GetUser()->GetHolder() << "@" << hi.GetIndex();
  }
  os << "]";
  return os;
}

std::ostream& HGraph::Dump(std::ostream& os,
                           CodeGenerator* codegen,
                           std::optional<std::reference_wrapper<const BlockNamer>> namer) {
  HGraphVisualizer vis(&os, this, codegen, namer);
  vis.DumpGraphDebug();
  return os;
}

void HInstruction::MoveBefore(HInstruction* cursor, bool do_checks) {
  if (do_checks) {
    DCHECK(!IsPhi());
    DCHECK(!IsControlFlow());
    DCHECK(CanBeMoved() ||
           // HShouldDeoptimizeFlag can only be moved by CHAGuardOptimization.
           IsShouldDeoptimizeFlag());
    DCHECK(!cursor->IsPhi());
  }

  next_->previous_ = previous_;
  if (previous_ != nullptr) {
    previous_->next_ = next_;
  }
  if (block_->instructions_.first_instruction_ == this) {
    block_->instructions_.first_instruction_ = next_;
  }
  DCHECK_NE(block_->instructions_.last_instruction_, this);

  previous_ = cursor->previous_;
  if (previous_ != nullptr) {
    previous_->next_ = this;
  }
  next_ = cursor;
  cursor->previous_ = this;
  block_ = cursor->block_;

  if (block_->instructions_.first_instruction_ == cursor) {
    block_->instructions_.first_instruction_ = this;
  }
}

void HInstruction::MoveBeforeFirstUserAndOutOfLoops() {
  DCHECK(!CanThrow());
  DCHECK(!HasSideEffects());
  DCHECK(!HasEnvironmentUses());
  DCHECK(HasNonEnvironmentUses());
  DCHECK(!IsPhi());  // Makes no sense for Phi.
  DCHECK_EQ(InputCount(), 0u);

  // Find the target block.
  auto uses_it = GetUses().begin();
  auto uses_end = GetUses().end();
  HBasicBlock* target_block = uses_it->GetUser()->GetBlock();
  ++uses_it;
  while (uses_it != uses_end && uses_it->GetUser()->GetBlock() == target_block) {
    ++uses_it;
  }
  if (uses_it != uses_end) {
    // This instruction has uses in two or more blocks. Find the common dominator.
    CommonDominator finder(target_block);
    for (; uses_it != uses_end; ++uses_it) {
      finder.Update(uses_it->GetUser()->GetBlock());
    }
    target_block = finder.Get();
    DCHECK(target_block != nullptr);
  }
  // Move to the first dominator not in a loop.
  while (target_block->IsInLoop()) {
    target_block = target_block->GetDominator();
    DCHECK(target_block != nullptr);
  }

  // Find insertion position.
  HInstruction* insert_pos = nullptr;
  for (const HUseListNode<HInstruction*>& use : GetUses()) {
    if (use.GetUser()->GetBlock() == target_block &&
        (insert_pos == nullptr || use.GetUser()->StrictlyDominates(insert_pos))) {
      insert_pos = use.GetUser();
    }
  }
  if (insert_pos == nullptr) {
    // No user in `target_block`, insert before the control flow instruction.
    insert_pos = target_block->GetLastInstruction();
    DCHECK(insert_pos->IsControlFlow());
    // Avoid splitting HCondition from HIf to prevent unnecessary materialization.
    if (insert_pos->IsIf()) {
      HInstruction* if_input = insert_pos->AsIf()->InputAt(0);
      if (if_input == insert_pos->GetPrevious()) {
        insert_pos = if_input;
      }
    }
  }
  MoveBefore(insert_pos);
}

HBasicBlock* HBasicBlock::SplitBefore(HInstruction* cursor, bool require_graph_not_in_ssa_form) {
  DCHECK_IMPLIES(require_graph_not_in_ssa_form, !graph_->IsInSsaForm())
      << "Support for SSA form not implemented.";
  DCHECK_EQ(cursor->GetBlock(), this);

  HBasicBlock* new_block =
      new (GetGraph()->GetAllocator()) HBasicBlock(GetGraph(), cursor->GetDexPc());
  new_block->instructions_.first_instruction_ = cursor;
  new_block->instructions_.last_instruction_ = instructions_.last_instruction_;
  instructions_.last_instruction_ = cursor->previous_;
  if (cursor->previous_ == nullptr) {
    instructions_.first_instruction_ = nullptr;
  } else {
    cursor->previous_->next_ = nullptr;
    cursor->previous_ = nullptr;
  }

  new_block->instructions_.SetBlockOfInstructions(new_block);
  AddInstruction(new (GetGraph()->GetAllocator()) HGoto(new_block->GetDexPc()));

  for (HBasicBlock* successor : GetSuccessors()) {
    successor->predecessors_[successor->GetPredecessorIndexOf(this)] = new_block;
  }
  new_block->successors_.swap(successors_);
  DCHECK(successors_.empty());
  AddSuccessor(new_block);

  GetGraph()->AddBlock(new_block);
  return new_block;
}

HBasicBlock* HBasicBlock::CreateImmediateDominator() {
  DCHECK(!graph_->IsInSsaForm()) << "Support for SSA form not implemented.";
  DCHECK(!IsCatchBlock()) << "Support for updating try/catch information not implemented.";

  HBasicBlock* new_block = new (GetGraph()->GetAllocator()) HBasicBlock(GetGraph(), GetDexPc());

  for (HBasicBlock* predecessor : GetPredecessors()) {
    predecessor->successors_[predecessor->GetSuccessorIndexOf(this)] = new_block;
  }
  new_block->predecessors_.swap(predecessors_);
  DCHECK(predecessors_.empty());
  AddPredecessor(new_block);

  GetGraph()->AddBlock(new_block);
  return new_block;
}

HBasicBlock* HBasicBlock::SplitBeforeForInlining(HInstruction* cursor) {
  DCHECK_EQ(cursor->GetBlock(), this);

  HBasicBlock* new_block =
      new (GetGraph()->GetAllocator()) HBasicBlock(GetGraph(), cursor->GetDexPc());
  new_block->instructions_.first_instruction_ = cursor;
  new_block->instructions_.last_instruction_ = instructions_.last_instruction_;
  instructions_.last_instruction_ = cursor->previous_;
  if (cursor->previous_ == nullptr) {
    instructions_.first_instruction_ = nullptr;
  } else {
    cursor->previous_->next_ = nullptr;
    cursor->previous_ = nullptr;
  }

  new_block->instructions_.SetBlockOfInstructions(new_block);

  for (HBasicBlock* successor : GetSuccessors()) {
    successor->predecessors_[successor->GetPredecessorIndexOf(this)] = new_block;
  }
  new_block->successors_.swap(successors_);
  DCHECK(successors_.empty());

  for (HBasicBlock* dominated : GetDominatedBlocks()) {
    dominated->dominator_ = new_block;
  }
  new_block->dominated_blocks_.swap(dominated_blocks_);
  DCHECK(dominated_blocks_.empty());
  return new_block;
}

HBasicBlock* HBasicBlock::SplitAfterForInlining(HInstruction* cursor) {
  DCHECK(!cursor->IsControlFlow());
  DCHECK_NE(instructions_.last_instruction_, cursor);
  DCHECK_EQ(cursor->GetBlock(), this);

  HBasicBlock* new_block = new (GetGraph()->GetAllocator()) HBasicBlock(GetGraph(), GetDexPc());
  new_block->instructions_.first_instruction_ = cursor->GetNext();
  new_block->instructions_.last_instruction_ = instructions_.last_instruction_;
  cursor->next_->previous_ = nullptr;
  cursor->next_ = nullptr;
  instructions_.last_instruction_ = cursor;

  new_block->instructions_.SetBlockOfInstructions(new_block);
  for (HBasicBlock* successor : GetSuccessors()) {
    successor->predecessors_[successor->GetPredecessorIndexOf(this)] = new_block;
  }
  new_block->successors_.swap(successors_);
  DCHECK(successors_.empty());

  for (HBasicBlock* dominated : GetDominatedBlocks()) {
    dominated->dominator_ = new_block;
  }
  new_block->dominated_blocks_.swap(dominated_blocks_);
  DCHECK(dominated_blocks_.empty());
  return new_block;
}

const HTryBoundary* HBasicBlock::ComputeTryEntryOfSuccessors() const {
  if (EndsWithTryBoundary()) {
    HTryBoundary* try_boundary = GetLastInstruction()->AsTryBoundary();
    if (try_boundary->IsEntry()) {
      DCHECK(!IsTryBlock());
      return try_boundary;
    } else {
      DCHECK(IsTryBlock());
      DCHECK(try_catch_information_->GetTryEntry().HasSameExceptionHandlersAs(*try_boundary));
      return nullptr;
    }
  } else if (IsTryBlock()) {
    return &try_catch_information_->GetTryEntry();
  } else {
    return nullptr;
  }
}

bool HBasicBlock::HasThrowingInstructions() const {
  for (HInstructionIterator it(GetInstructions()); !it.Done(); it.Advance()) {
    if (it.Current()->CanThrow()) {
      return true;
    }
  }
  return false;
}

static bool HasOnlyOneInstruction(const HBasicBlock& block) {
  return block.GetPhis().IsEmpty()
      && !block.GetInstructions().IsEmpty()
      && block.GetFirstInstruction() == block.GetLastInstruction();
}

bool HBasicBlock::IsSingleGoto() const {
  return HasOnlyOneInstruction(*this) && GetLastInstruction()->IsGoto();
}

bool HBasicBlock::IsSingleReturn() const {
  return HasOnlyOneInstruction(*this) && GetLastInstruction()->IsReturn();
}

bool HBasicBlock::IsSingleReturnOrReturnVoidAllowingPhis() const {
  return (GetFirstInstruction() == GetLastInstruction()) &&
         (GetLastInstruction()->IsReturn() || GetLastInstruction()->IsReturnVoid());
}

bool HBasicBlock::IsSingleTryBoundary() const {
  return HasOnlyOneInstruction(*this) && GetLastInstruction()->IsTryBoundary();
}

bool HBasicBlock::EndsWithControlFlowInstruction() const {
  return !GetInstructions().IsEmpty() && GetLastInstruction()->IsControlFlow();
}

bool HBasicBlock::EndsWithReturn() const {
  return !GetInstructions().IsEmpty() &&
      (GetLastInstruction()->IsReturn() || GetLastInstruction()->IsReturnVoid());
}

bool HBasicBlock::EndsWithIf() const {
  return !GetInstructions().IsEmpty() && GetLastInstruction()->IsIf();
}

bool HBasicBlock::EndsWithTryBoundary() const {
  return !GetInstructions().IsEmpty() && GetLastInstruction()->IsTryBoundary();
}

bool HBasicBlock::HasSinglePhi() const {
  return !GetPhis().IsEmpty() && GetFirstPhi()->GetNext() == nullptr;
}

ArrayRef<HBasicBlock* const> HBasicBlock::GetNormalSuccessors() const {
  if (EndsWithTryBoundary()) {
    // The normal-flow successor of HTryBoundary is always stored at index zero.
    DCHECK_EQ(successors_[0], GetLastInstruction()->AsTryBoundary()->GetNormalFlowSuccessor());
    return ArrayRef<HBasicBlock* const>(successors_).SubArray(0u, 1u);
  } else {
    // All successors of blocks not ending with TryBoundary are normal.
    return ArrayRef<HBasicBlock* const>(successors_);
  }
}

ArrayRef<HBasicBlock* const> HBasicBlock::GetExceptionalSuccessors() const {
  if (EndsWithTryBoundary()) {
    return GetLastInstruction()->AsTryBoundary()->GetExceptionHandlers();
  } else {
    // Blocks not ending with TryBoundary do not have exceptional successors.
    return ArrayRef<HBasicBlock* const>();
  }
}

bool HTryBoundary::HasSameExceptionHandlersAs(const HTryBoundary& other) const {
  ArrayRef<HBasicBlock* const> handlers1 = GetExceptionHandlers();
  ArrayRef<HBasicBlock* const> handlers2 = other.GetExceptionHandlers();

  size_t length = handlers1.size();
  if (length != handlers2.size()) {
    return false;
  }

  // Exception handlers need to be stored in the same order.
  for (size_t i = 0; i < length; ++i) {
    if (handlers1[i] != handlers2[i]) {
      return false;
    }
  }
  return true;
}

size_t HInstructionList::CountSize() const {
  size_t size = 0;
  HInstruction* current = first_instruction_;
  for (; current != nullptr; current = current->GetNext()) {
    size++;
  }
  return size;
}

void HInstructionList::SetBlockOfInstructions(HBasicBlock* block) const {
  for (HInstruction* current = first_instruction_;
       current != nullptr;
       current = current->GetNext()) {
    current->SetBlock(block);
  }
}

void HInstructionList::AddAfter(HInstruction* cursor, const HInstructionList& instruction_list) {
  DCHECK(Contains(cursor));
  if (!instruction_list.IsEmpty()) {
    if (cursor == last_instruction_) {
      last_instruction_ = instruction_list.last_instruction_;
    } else {
      cursor->next_->previous_ = instruction_list.last_instruction_;
    }
    instruction_list.last_instruction_->next_ = cursor->next_;
    cursor->next_ = instruction_list.first_instruction_;
    instruction_list.first_instruction_->previous_ = cursor;
  }
}

void HInstructionList::AddBefore(HInstruction* cursor, const HInstructionList& instruction_list) {
  DCHECK(Contains(cursor));
  if (!instruction_list.IsEmpty()) {
    if (cursor == first_instruction_) {
      first_instruction_ = instruction_list.first_instruction_;
    } else {
      cursor->previous_->next_ = instruction_list.first_instruction_;
    }
    instruction_list.last_instruction_->next_ = cursor;
    instruction_list.first_instruction_->previous_ = cursor->previous_;
    cursor->previous_ = instruction_list.last_instruction_;
  }
}

void HInstructionList::Add(const HInstructionList& instruction_list) {
  if (IsEmpty()) {
    first_instruction_ = instruction_list.first_instruction_;
    last_instruction_ = instruction_list.last_instruction_;
  } else {
    AddAfter(last_instruction_, instruction_list);
  }
}

void HBasicBlock::DisconnectAndDelete() {
  // Dominators must be removed after all the blocks they dominate. This way
  // a loop header is removed last, a requirement for correct loop information
  // iteration.
  DCHECK(dominated_blocks_.empty());

  // The following steps gradually remove the block from all its dependants in
  // post order (b/27683071).

  // (1) Store a basic block that we'll use in step (5) to find loops to be updated.
  //     We need to do this before step (4) which destroys the predecessor list.
  HBasicBlock* loop_update_start = this;
  if (IsLoopHeader()) {
    HLoopInformation* loop_info = GetLoopInformation();
    // All other blocks in this loop should have been removed because the header
    // was their dominator.
    // Note that we do not remove `this` from `loop_info` as it is unreachable.
    DCHECK(!loop_info->IsIrreducible());
    DCHECK_EQ(loop_info->GetBlocks().NumSetBits(), 1u);
    DCHECK_EQ(static_cast<uint32_t>(loop_info->GetBlocks().GetHighestBitSet()), GetBlockId());
    loop_update_start = loop_info->GetPreHeader();
  }

  // (2) Disconnect the block from its successors and update their phis.
  DisconnectFromSuccessors();

  // (3) Remove instructions and phis. Instructions should have no remaining uses
  //     except in catch phis. If an instruction is used by a catch phi at `index`,
  //     remove `index`-th input of all phis in the catch block since they are
  //     guaranteed dead. Note that we may miss dead inputs this way but the
  //     graph will always remain consistent.
  RemoveCatchPhiUsesAndInstruction(/* building_dominator_tree = */ false);

  // (4) Disconnect the block from its predecessors and update their
  //     control-flow instructions.
  for (HBasicBlock* predecessor : predecessors_) {
    // We should not see any back edges as they would have been removed by step (3).
    DCHECK_IMPLIES(IsInLoop(), !GetLoopInformation()->IsBackEdge(*predecessor));

    HInstruction* last_instruction = predecessor->GetLastInstruction();
    if (last_instruction->IsTryBoundary() && !IsCatchBlock()) {
      // This block is the only normal-flow successor of the TryBoundary which
      // makes `predecessor` dead. Since DCE removes blocks in post order,
      // exception handlers of this TryBoundary were already visited and any
      // remaining handlers therefore must be live. We remove `predecessor` from
      // their list of predecessors.
      DCHECK_EQ(last_instruction->AsTryBoundary()->GetNormalFlowSuccessor(), this);
      while (predecessor->GetSuccessors().size() > 1) {
        HBasicBlock* handler = predecessor->GetSuccessors()[1];
        DCHECK(handler->IsCatchBlock());
        predecessor->RemoveSuccessor(handler);
        handler->RemovePredecessor(predecessor);
      }
    }

    predecessor->RemoveSuccessor(this);
    uint32_t num_pred_successors = predecessor->GetSuccessors().size();
    if (num_pred_successors == 1u) {
      // If we have one successor after removing one, then we must have
      // had an HIf, HPackedSwitch or HTryBoundary, as they have more than one
      // successor. Replace those with a HGoto.
      DCHECK(last_instruction->IsIf() ||
             last_instruction->IsPackedSwitch() ||
             (last_instruction->IsTryBoundary() && IsCatchBlock()));
      predecessor->RemoveInstruction(last_instruction);
      predecessor->AddInstruction(new (graph_->GetAllocator()) HGoto(last_instruction->GetDexPc()));
    } else if (num_pred_successors == 0u) {
      // The predecessor has no remaining successors and therefore must be dead.
      // We deliberately leave it without a control-flow instruction so that the
      // GraphChecker fails unless it is not removed during the pass too.
      predecessor->RemoveInstruction(last_instruction);
    } else {
      // There are multiple successors left. The removed block might be a successor
      // of a PackedSwitch which will be completely removed (perhaps replaced with
      // a Goto), or we are deleting a catch block from a TryBoundary. In either
      // case, leave `last_instruction` as is for now.
      DCHECK(last_instruction->IsPackedSwitch() ||
             (last_instruction->IsTryBoundary() && IsCatchBlock()));
    }
  }
  predecessors_.clear();

  // (5) Remove the block from all loops it is included in. Skip the inner-most
  //     loop if this is the loop header (see definition of `loop_update_start`)
  //     because the loop header's predecessor list has been destroyed in step (4).
  for (HLoopInformationOutwardIterator it(*loop_update_start); !it.Done(); it.Advance()) {
    HLoopInformation* loop_info = it.Current();
    loop_info->Remove(this);
    if (loop_info->IsBackEdge(*this)) {
      // If this was the last back edge of the loop, we deliberately leave the
      // loop in an inconsistent state and will fail GraphChecker unless the
      // entire loop is removed during the pass.
      loop_info->RemoveBackEdge(this);
    }
  }

  // (6) Disconnect from the dominator.
  dominator_->RemoveDominatedBlock(this);
  SetDominator(nullptr);

  // (7) Delete from the graph, update reverse post order.
  graph_->DeleteDeadEmptyBlock(this);
}

void HBasicBlock::DisconnectFromSuccessors(BitVectorView<const size_t> visited) {
  DCHECK_IMPLIES(visited.SizeInBits() != 0u, visited.SizeInBits() == graph_->GetBlocks().size());
  for (HBasicBlock* successor : successors_) {
    // Delete this block from the list of predecessors.
    size_t this_index = successor->GetPredecessorIndexOf(this);
    successor->predecessors_.erase(successor->predecessors_.begin() + this_index);

    if (visited.SizeInBits() != 0u && !visited.IsBitSet(successor->GetBlockId())) {
      // `successor` itself is dead. Therefore, there is no need to update its phis.
      continue;
    }

    DCHECK(!successor->predecessors_.empty());

    // Remove this block's entries in the successor's phis. Skips exceptional
    // successors because catch phi inputs do not correspond to predecessor
    // blocks but throwing instructions. They are removed in `RemoveCatchPhiUses`.
    if (!successor->IsCatchBlock()) {
      if (successor->predecessors_.size() == 1u) {
        // The successor has just one predecessor left. Replace phis with the only
        // remaining input.
        for (HInstructionIterator phi_it(successor->GetPhis()); !phi_it.Done(); phi_it.Advance()) {
          HPhi* phi = phi_it.Current()->AsPhi();
          phi->ReplaceWith(phi->InputAt(1 - this_index));
          successor->RemovePhi(phi);
        }
      } else {
        for (HInstructionIterator phi_it(successor->GetPhis()); !phi_it.Done(); phi_it.Advance()) {
          phi_it.Current()->AsPhi()->RemoveInputAt(this_index);
        }
      }
    }
  }
  successors_.clear();
}

void HBasicBlock::RemoveCatchPhiUsesAndInstruction(bool building_dominator_tree) {
  for (HBackwardInstructionIterator it(GetInstructions()); !it.Done(); it.Advance()) {
    HInstruction* insn = it.Current();
    RemoveCatchPhiUsesOfDeadInstruction(insn);

    // If we are building the dominator tree, we removed all input records previously.
    // `RemoveInstruction` will try to remove them again but that's not something we support and we
    // will crash. We check here since we won't be checking that in RemoveInstruction.
    if (building_dominator_tree) {
      DCHECK(insn->GetUses().empty());
      DCHECK(insn->GetEnvUses().empty());
    }
    RemoveInstruction(insn, /* ensure_safety= */ !building_dominator_tree);
  }
  for (HInstructionIterator it(GetPhis()); !it.Done(); it.Advance()) {
    HPhi* insn = it.Current()->AsPhi();
    RemoveCatchPhiUsesOfDeadInstruction(insn);

    // If we are building the dominator tree, we removed all input records previously.
    // `RemovePhi` will try to remove them again but that's not something we support and we
    // will crash. We check here since we won't be checking that in RemovePhi.
    if (building_dominator_tree) {
      DCHECK(insn->GetUses().empty());
      DCHECK(insn->GetEnvUses().empty());
    }
    RemovePhi(insn, /* ensure_safety= */ !building_dominator_tree);
  }
}

void HBasicBlock::MergeInstructionsWith(HBasicBlock* other) {
  DCHECK(EndsWithControlFlowInstruction());
  RemoveInstruction(GetLastInstruction());
  instructions_.Add(other->GetInstructions());
  other->instructions_.SetBlockOfInstructions(this);
  other->instructions_.Clear();
}

void HBasicBlock::MergeWith(HBasicBlock* other) {
  DCHECK_EQ(GetGraph(), other->GetGraph());
  DCHECK(ContainsElement(dominated_blocks_, other));
  DCHECK_EQ(GetSingleSuccessor(), other);
  DCHECK_EQ(other->GetSinglePredecessor(), this);
  DCHECK(other->GetPhis().IsEmpty());

  // Move instructions from `other` to `this`.
  MergeInstructionsWith(other);

  // Remove `other` from the loops it is included in.
  for (HLoopInformationOutwardIterator it(*other); !it.Done(); it.Advance()) {
    HLoopInformation* loop_info = it.Current();
    loop_info->Remove(other);
    if (loop_info->IsBackEdge(*other)) {
      loop_info->ReplaceBackEdge(other, this);
    }
  }

  // Update links to the successors of `other`.
  successors_.clear();
  for (HBasicBlock* successor : other->GetSuccessors()) {
    successor->predecessors_[successor->GetPredecessorIndexOf(other)] = this;
  }
  successors_.swap(other->successors_);
  DCHECK(other->successors_.empty());

  // Update the dominator tree.
  RemoveDominatedBlock(other);
  for (HBasicBlock* dominated : other->GetDominatedBlocks()) {
    dominated->SetDominator(this);
  }
  dominated_blocks_.insert(
      dominated_blocks_.end(), other->dominated_blocks_.begin(), other->dominated_blocks_.end());
  other->dominated_blocks_.clear();
  other->dominator_ = nullptr;

  // Clear the list of predecessors of `other` in preparation of deleting it.
  other->predecessors_.clear();

  // Delete `other` from the graph. The function updates reverse post order.
  graph_->DeleteDeadEmptyBlock(other);
}

void HBasicBlock::MergeWithInlined(HBasicBlock* other) {
  DCHECK_NE(GetGraph(), other->GetGraph());
  DCHECK(GetDominatedBlocks().empty());
  DCHECK(GetSuccessors().empty());
  DCHECK(!EndsWithControlFlowInstruction());
  DCHECK(other->GetSinglePredecessor()->IsEntryBlock());
  DCHECK(other->GetPhis().IsEmpty());
  DCHECK(!other->IsInLoop());

  // Move instructions from `other` to `this`.
  instructions_.Add(other->GetInstructions());
  other->instructions_.SetBlockOfInstructions(this);

  // Update links to the successors of `other`.
  successors_.clear();
  for (HBasicBlock* successor : other->GetSuccessors()) {
    successor->predecessors_[successor->GetPredecessorIndexOf(other)] = this;
  }
  successors_.swap(other->successors_);
  DCHECK(other->successors_.empty());

  // Update the dominator tree.
  for (HBasicBlock* dominated : other->GetDominatedBlocks()) {
    dominated->SetDominator(this);
  }
  dominated_blocks_.insert(
      dominated_blocks_.end(), other->dominated_blocks_.begin(), other->dominated_blocks_.end());
  other->dominated_blocks_.clear();
  other->dominator_ = nullptr;
  other->graph_ = nullptr;
}

void HBasicBlock::ReplaceWith(HBasicBlock* other) {
  while (!GetPredecessors().empty()) {
    HBasicBlock* predecessor = GetPredecessors()[0];
    predecessor->ReplaceSuccessor(this, other);
  }
  while (!GetSuccessors().empty()) {
    HBasicBlock* successor = GetSuccessors()[0];
    successor->ReplacePredecessor(this, other);
  }
  for (HBasicBlock* dominated : GetDominatedBlocks()) {
    other->AddDominatedBlock(dominated);
  }
  GetDominator()->ReplaceDominatedBlock(this, other);
  other->SetDominator(GetDominator());
  dominator_ = nullptr;
  graph_ = nullptr;
}

void HGraph::DeleteDeadEmptyBlock(HBasicBlock* block) {
  DCHECK_EQ(block->GetGraph(), this);
  DCHECK(block->GetSuccessors().empty());
  DCHECK(block->GetPredecessors().empty());
  DCHECK(block->GetDominatedBlocks().empty());
  DCHECK(block->GetDominator() == nullptr);
  DCHECK(block->GetInstructions().IsEmpty());
  DCHECK(block->GetPhis().IsEmpty());

  if (block->IsExitBlock()) {
    SetExitBlock(nullptr);
  }

  RemoveElement(reverse_post_order_, block);
  blocks_[block->GetBlockId()] = nullptr;
  block->SetGraph(nullptr);
}

void HGraph::UpdateLoopAndTryInformationOfNewBlock(HBasicBlock* block,
                                                   HBasicBlock* reference,
                                                   bool replace_if_back_edge,
                                                   bool has_more_specific_try_catch_info) {
  if (block->IsLoopHeader()) {
    // Clear the information of which blocks are contained in that loop. Since the
    // information is stored as a bit vector based on block ids, we have to update
    // it, as those block ids were specific to the callee graph and we are now adding
    // these blocks to the caller graph.
    block->GetLoopInformation()->ClearAllBlocks();
  }

  // If not already in a loop, update the loop information.
  if (!block->IsInLoop()) {
    block->SetLoopInformation(reference->GetLoopInformation());
  }

  // If the block is in a loop, update all its outward loops.
  HLoopInformation* loop_info = block->GetLoopInformation();
  if (loop_info != nullptr) {
    for (HLoopInformationOutwardIterator loop_it(*block);
         !loop_it.Done();
         loop_it.Advance()) {
      loop_it.Current()->Add(block);
    }
    if (replace_if_back_edge && loop_info->IsBackEdge(*reference)) {
      loop_info->ReplaceBackEdge(reference, block);
    }
  }

  DCHECK_IMPLIES(has_more_specific_try_catch_info, !reference->IsTryBlock())
      << "We don't allow to inline try catches inside of other try blocks.";

  // Update the TryCatchInformation, if we are not inlining a try catch.
  if (!has_more_specific_try_catch_info) {
    // Copy TryCatchInformation if `reference` is a try block, not if it is a catch block.
    TryCatchInformation* try_catch_info =
        reference->IsTryBlock() ? reference->GetTryCatchInformation() : nullptr;
    block->SetTryCatchInformation(try_catch_info);
  }
}

HInstruction* HGraph::InlineInto(HGraph* outer_graph, HInvoke* invoke) {
  DCHECK(HasExitBlock()) << "Unimplemented scenario";
  // Update the environments in this graph to have the invoke's environment
  // as parent.
  {
    // Skip the entry block, we do not need to update the entry's suspend check.
    for (HBasicBlock* block : GetReversePostOrderSkipEntryBlock()) {
      for (HInstructionIterator instr_it(block->GetInstructions());
           !instr_it.Done();
           instr_it.Advance()) {
        HInstruction* current = instr_it.Current();
        if (current->NeedsEnvironment()) {
          DCHECK(current->HasEnvironment());
          current->GetEnvironment()->SetAndCopyParentChain(
              outer_graph->GetAllocator(), invoke->GetEnvironment());
        }
      }
    }
  }

  if (HasBoundsChecks()) {
    outer_graph->SetHasBoundsChecks(true);
  }
  if (HasLoops()) {
    outer_graph->SetHasLoops(true);
  }
  if (HasIrreducibleLoops()) {
    outer_graph->SetHasIrreducibleLoops(true);
  }
  if (HasDirectCriticalNativeCall()) {
    outer_graph->SetHasDirectCriticalNativeCall(true);
  }
  if (HasTryCatch()) {
    outer_graph->SetHasTryCatch(true);
  }
  if (HasMonitorOperations()) {
    outer_graph->SetHasMonitorOperations(true);
  }
  if (HasTraditionalSIMD()) {
    outer_graph->SetHasTraditionalSIMD(true);
  }
  if (HasPredicatedSIMD()) {
    outer_graph->SetHasPredicatedSIMD(true);
  }
  if (HasAlwaysThrowingInvokes()) {
    outer_graph->SetHasAlwaysThrowingInvokes(true);
  }

  HInstruction* return_value = nullptr;
  if (GetBlocks().size() == 3) {
    // Inliner already made sure we don't inline methods that always throw.
    DCHECK(!GetBlocks()[1]->GetLastInstruction()->IsThrow());
    // Simple case of an entry block, a body block, and an exit block.
    // Put the body block's instruction into `invoke`'s block.
    HBasicBlock* body = GetBlocks()[1];
    DCHECK(GetBlocks()[0]->IsEntryBlock());
    DCHECK(GetBlocks()[2]->IsExitBlock());
    DCHECK(!body->IsExitBlock());
    DCHECK(!body->IsInLoop());
    HInstruction* last = body->GetLastInstruction();

    // Note that we add instructions before the invoke only to simplify polymorphic inlining.
    invoke->GetBlock()->instructions_.AddBefore(invoke, body->GetInstructions());
    body->GetInstructions().SetBlockOfInstructions(invoke->GetBlock());

    // Replace the invoke with the return value of the inlined graph.
    if (last->IsReturn()) {
      return_value = last->InputAt(0);
    } else {
      DCHECK(last->IsReturnVoid());
    }

    invoke->GetBlock()->RemoveInstruction(last);
  } else {
    // Need to inline multiple blocks. We split `invoke`'s block
    // into two blocks, merge the first block of the inlined graph into
    // the first half, and replace the exit block of the inlined graph
    // with the second half.
    ArenaAllocator* allocator = outer_graph->GetAllocator();
    HBasicBlock* at = invoke->GetBlock();
    // Note that we split before the invoke only to simplify polymorphic inlining.
    HBasicBlock* to = at->SplitBeforeForInlining(invoke);

    HBasicBlock* first = entry_block_->GetSuccessors()[0];
    DCHECK(!first->IsInLoop());
    DCHECK(first->GetTryCatchInformation() == nullptr);
    at->MergeWithInlined(first);
    exit_block_->ReplaceWith(to);

    // Update the meta information surrounding blocks:
    // (1) the graph they are now in,
    // (2) the reverse post order of that graph,
    // (3) their potential loop information, inner and outer,
    // (4) try block membership.
    // Note that we do not need to update catch phi inputs because they
    // correspond to the register file of the outer method which the inlinee
    // cannot modify.

    // We don't add the entry block, the exit block, and the first block, which
    // has been merged with `at`.
    static constexpr int kNumberOfSkippedBlocksInCallee = 3;

    // We add the `to` block.
    static constexpr int kNumberOfNewBlocksInCaller = 1;
    size_t blocks_added = (reverse_post_order_.size() - kNumberOfSkippedBlocksInCallee)
        + kNumberOfNewBlocksInCaller;

    // Find the location of `at` in the outer graph's reverse post order. The new
    // blocks will be added after it.
    size_t index_of_at = IndexOfElement(outer_graph->reverse_post_order_, at);
    MakeRoomFor(&outer_graph->reverse_post_order_, blocks_added, index_of_at);

    // Do a reverse post order of the blocks in the callee and do (1), (2), (3)
    // and (4) to the blocks that apply.
    for (HBasicBlock* current : GetReversePostOrder()) {
      if (current != exit_block_ && current != entry_block_ && current != first) {
        DCHECK(current->GetGraph() == this);
        current->SetGraph(outer_graph);
        outer_graph->AddBlock(current);
        outer_graph->reverse_post_order_[++index_of_at] = current;
        UpdateLoopAndTryInformationOfNewBlock(current,
                                              at,
                                              /* replace_if_back_edge= */ false,
                                              current->GetTryCatchInformation() != nullptr);
      }
    }

    // Do (1), (2), (3) and (4) to `to`.
    to->SetGraph(outer_graph);
    outer_graph->AddBlock(to);
    outer_graph->reverse_post_order_[++index_of_at] = to;
    // Only `to` can become a back edge, as the inlined blocks
    // are predecessors of `to`.
    UpdateLoopAndTryInformationOfNewBlock(to, at, /* replace_if_back_edge= */ true);

    // Update all predecessors of the exit block (now the `to` block)
    // to not `HReturn` but `HGoto` instead. Special case throwing blocks
    // to now get the outer graph exit block as successor.
    HPhi* return_value_phi = nullptr;
    bool rerun_dominance = false;
    bool rerun_loop_analysis = false;
    for (size_t pred = 0; pred < to->GetPredecessors().size(); ++pred) {
      HBasicBlock* predecessor = to->GetPredecessors()[pred];
      HInstruction* last = predecessor->GetLastInstruction();

      // At this point we might either have:
      // A) Return/ReturnVoid/Throw as the last instruction, or
      // B) `Return/ReturnVoid/Throw->TryBoundary` as the last instruction chain

      const bool saw_try_boundary = last->IsTryBoundary();
      if (saw_try_boundary) {
        DCHECK(predecessor->IsSingleTryBoundary());
        DCHECK(!last->AsTryBoundary()->IsEntry());
        predecessor = predecessor->GetSinglePredecessor();
        last = predecessor->GetLastInstruction();
      }

      if (last->IsThrow()) {
        if (at->IsTryBlock()) {
          DCHECK(!saw_try_boundary) << "We don't support inlining of try blocks into try blocks.";
          // Create a TryBoundary of kind:exit and point it to the Exit block.
          HBasicBlock* new_block = outer_graph->SplitEdge(predecessor, to);
          new_block->AddInstruction(
              new (allocator) HTryBoundary(HTryBoundary::BoundaryKind::kExit, last->GetDexPc()));
          new_block->ReplaceSuccessor(to, outer_graph->GetExitBlock());

          // Copy information from the predecessor.
          new_block->SetLoopInformation(predecessor->GetLoopInformation());
          TryCatchInformation* try_catch_info = predecessor->GetTryCatchInformation();
          new_block->SetTryCatchInformation(try_catch_info);
          for (HBasicBlock* xhandler :
               try_catch_info->GetTryEntry().GetBlock()->GetExceptionalSuccessors()) {
            new_block->AddSuccessor(xhandler);
          }
          DCHECK(try_catch_info->GetTryEntry().HasSameExceptionHandlersAs(
              *new_block->GetLastInstruction()->AsTryBoundary()));
        } else {
          // We either have `Throw->TryBoundary` or `Throw`. We want to point the whole chain to the
          // exit, so we recompute `predecessor`
          predecessor = to->GetPredecessors()[pred];
          predecessor->ReplaceSuccessor(to, outer_graph->GetExitBlock());
        }

        --pred;
        // We need to re-run dominance information, as the exit block now has
        // a new predecessor and potential new dominator.
        // TODO(solanes): See if it's worth it to hand-modify the domination chain instead of
        // rerunning the dominance for the whole graph.
        rerun_dominance = true;
        if (predecessor->GetLoopInformation() != nullptr) {
          // The loop information might have changed e.g. `predecessor` might not be in a loop
          // anymore. We only do this if `predecessor` has loop information as it is impossible for
          // predecessor to end up in a loop if it wasn't in one before.
          rerun_loop_analysis = true;
        }
      } else {
        if (last->IsReturnVoid()) {
          DCHECK(return_value == nullptr);
          DCHECK(return_value_phi == nullptr);
        } else {
          DCHECK(last->IsReturn());
          if (return_value_phi != nullptr) {
            return_value_phi->AddInput(last->InputAt(0));
          } else if (return_value == nullptr) {
            return_value = last->InputAt(0);
          } else {
            // There will be multiple returns.
            return_value_phi = new (allocator) HPhi(
                allocator, kNoRegNumber, 0, HPhi::ToPhiType(invoke->GetType()), to->GetDexPc());
            to->AddPhi(return_value_phi);
            return_value_phi->AddInput(return_value);
            return_value_phi->AddInput(last->InputAt(0));
            return_value = return_value_phi;
          }
        }
        predecessor->AddInstruction(new (allocator) HGoto(last->GetDexPc()));
        predecessor->RemoveInstruction(last);

        if (saw_try_boundary) {
          predecessor = to->GetPredecessors()[pred];
          DCHECK(predecessor->EndsWithTryBoundary());
          DCHECK_EQ(predecessor->GetNormalSuccessors().size(), 1u);
          if (predecessor->GetSuccessors()[0]->GetPredecessors().size() > 1) {
            outer_graph->SplitCriticalEdge(predecessor, to);
            rerun_dominance = true;
            if (predecessor->GetLoopInformation() != nullptr) {
              rerun_loop_analysis = true;
            }
          }
        }
      }
    }
    if (rerun_loop_analysis) {
      outer_graph->RecomputeDominatorTree();
    } else if (rerun_dominance) {
      outer_graph->ClearDominanceInformation();
      outer_graph->ComputeDominanceInformation();
    }
  }

  // Walk over the entry block and:
  // - Move constants from the entry block to the outer_graph's entry block,
  // - Replace HParameterValue instructions with their real value.
  // - Remove suspend checks, that hold an environment.
  // We must do this after the other blocks have been inlined, otherwise ids of
  // constants could overlap with the inner graph.
  size_t parameter_index = 0;
  for (HInstructionIterator it(entry_block_->GetInstructions()); !it.Done(); it.Advance()) {
    HInstruction* current = it.Current();
    HInstruction* replacement = nullptr;
    if (current->IsNullConstant()) {
      replacement = outer_graph->GetNullConstant();
    } else if (current->IsIntConstant()) {
      replacement = outer_graph->GetIntConstant(current->AsIntConstant()->GetValue());
    } else if (current->IsLongConstant()) {
      replacement = outer_graph->GetLongConstant(current->AsLongConstant()->GetValue());
    } else if (current->IsFloatConstant()) {
      replacement = outer_graph->GetFloatConstant(current->AsFloatConstant()->GetValue());
    } else if (current->IsDoubleConstant()) {
      replacement = outer_graph->GetDoubleConstant(current->AsDoubleConstant()->GetValue());
    } else if (current->IsParameterValue()) {
      if (kIsDebugBuild &&
          invoke->IsInvokeStaticOrDirect() &&
          invoke->AsInvokeStaticOrDirect()->IsStaticWithExplicitClinitCheck()) {
        // Ensure we do not use the last input of `invoke`, as it
        // contains a clinit check which is not an actual argument.
        size_t last_input_index = invoke->InputCount() - 1;
        DCHECK(parameter_index != last_input_index);
      }
      replacement = invoke->InputAt(parameter_index++);
    } else if (current->IsCurrentMethod()) {
      replacement = outer_graph->GetCurrentMethod();
    } else {
      // It is OK to ignore MethodEntryHook for inlined functions.
      // In debug mode we don't inline and in release mode method
      // tracing is best effort so OK to ignore them.
      DCHECK(current->IsGoto() || current->IsSuspendCheck() || current->IsMethodEntryHook());
      entry_block_->RemoveInstruction(current);
    }
    if (replacement != nullptr) {
      current->ReplaceWith(replacement);
      // If the current is the return value then we need to update the latter.
      if (current == return_value) {
        DCHECK_EQ(entry_block_, return_value->GetBlock());
        return_value = replacement;
      }
    }
  }

  return return_value;
}

/*
 * Loop will be transformed to:
 *       old_pre_header
 *             |
 *          if_block
 *           /    \
 *  true_block   false_block
 *           \    /
 *       new_pre_header
 *             |
 *           header
 */
void HGraph::TransformLoopHeaderForBCE(HBasicBlock* header) {
  DCHECK(header->IsLoopHeader());
  HBasicBlock* old_pre_header = header->GetDominator();

  // Need extra block to avoid critical edge.
  HBasicBlock* if_block = new (allocator_) HBasicBlock(this, header->GetDexPc());
  HBasicBlock* true_block = new (allocator_) HBasicBlock(this, header->GetDexPc());
  HBasicBlock* false_block = new (allocator_) HBasicBlock(this, header->GetDexPc());
  HBasicBlock* new_pre_header = new (allocator_) HBasicBlock(this, header->GetDexPc());
  AddBlock(if_block);
  AddBlock(true_block);
  AddBlock(false_block);
  AddBlock(new_pre_header);

  header->ReplacePredecessor(old_pre_header, new_pre_header);
  old_pre_header->successors_.clear();
  old_pre_header->dominated_blocks_.clear();

  old_pre_header->AddSuccessor(if_block);
  if_block->AddSuccessor(true_block);  // True successor
  if_block->AddSuccessor(false_block);  // False successor
  true_block->AddSuccessor(new_pre_header);
  false_block->AddSuccessor(new_pre_header);

  old_pre_header->dominated_blocks_.push_back(if_block);
  if_block->SetDominator(old_pre_header);
  if_block->dominated_blocks_.push_back(true_block);
  true_block->SetDominator(if_block);
  if_block->dominated_blocks_.push_back(false_block);
  false_block->SetDominator(if_block);
  if_block->dominated_blocks_.push_back(new_pre_header);
  new_pre_header->SetDominator(if_block);
  new_pre_header->dominated_blocks_.push_back(header);
  header->SetDominator(new_pre_header);

  // Fix reverse post order.
  size_t index_of_header = IndexOfElement(reverse_post_order_, header);
  MakeRoomFor(&reverse_post_order_, 4, index_of_header - 1);
  reverse_post_order_[index_of_header++] = if_block;
  reverse_post_order_[index_of_header++] = true_block;
  reverse_post_order_[index_of_header++] = false_block;
  reverse_post_order_[index_of_header++] = new_pre_header;

  // The pre_header can never be a back edge of a loop.
  DCHECK((old_pre_header->GetLoopInformation() == nullptr) ||
         !old_pre_header->GetLoopInformation()->IsBackEdge(*old_pre_header));
  UpdateLoopAndTryInformationOfNewBlock(
      if_block, old_pre_header, /* replace_if_back_edge= */ false);
  UpdateLoopAndTryInformationOfNewBlock(
      true_block, old_pre_header, /* replace_if_back_edge= */ false);
  UpdateLoopAndTryInformationOfNewBlock(
      false_block, old_pre_header, /* replace_if_back_edge= */ false);
  UpdateLoopAndTryInformationOfNewBlock(
      new_pre_header, old_pre_header, /* replace_if_back_edge= */ false);
}

// Creates a new two-basic-block loop and inserts it between original loop header and
// original loop exit; also adjusts dominators, post order and new LoopInformation.
HBasicBlock* HGraph::TransformLoopForVectorization(HBasicBlock* header,
                                                   HBasicBlock* body,
                                                   HBasicBlock* exit) {
  DCHECK(header->IsLoopHeader());
  HLoopInformation* loop = header->GetLoopInformation();

  // Add new loop blocks.
  HBasicBlock* new_pre_header = new (allocator_) HBasicBlock(this, header->GetDexPc());
  HBasicBlock* new_header = new (allocator_) HBasicBlock(this, header->GetDexPc());
  HBasicBlock* new_body = new (allocator_) HBasicBlock(this, header->GetDexPc());
  AddBlock(new_pre_header);
  AddBlock(new_header);
  AddBlock(new_body);

  // Set up control flow.
  header->ReplaceSuccessor(exit, new_pre_header);
  new_pre_header->AddSuccessor(new_header);
  new_header->AddSuccessor(exit);
  new_header->AddSuccessor(new_body);
  new_body->AddSuccessor(new_header);

  // Set up dominators.
  header->ReplaceDominatedBlock(exit, new_pre_header);
  new_pre_header->SetDominator(header);
  new_pre_header->dominated_blocks_.push_back(new_header);
  new_header->SetDominator(new_pre_header);
  new_header->dominated_blocks_.push_back(new_body);
  new_body->SetDominator(new_header);
  new_header->dominated_blocks_.push_back(exit);
  exit->SetDominator(new_header);

  // Fix reverse post order.
  size_t index_of_header = IndexOfElement(reverse_post_order_, header);
  MakeRoomFor(&reverse_post_order_, 2, index_of_header);
  reverse_post_order_[++index_of_header] = new_pre_header;
  reverse_post_order_[++index_of_header] = new_header;
  size_t index_of_body = IndexOfElement(reverse_post_order_, body);
  MakeRoomFor(&reverse_post_order_, 1, index_of_body - 1);
  reverse_post_order_[index_of_body] = new_body;

  // Add gotos and suspend check (client must add conditional in header).
  new_pre_header->AddInstruction(new (allocator_) HGoto());
  HSuspendCheck* suspend_check = new (allocator_) HSuspendCheck(header->GetDexPc());
  new_header->AddInstruction(suspend_check);
  new_body->AddInstruction(new (allocator_) HGoto());
  DCHECK(loop->GetSuspendCheck() != nullptr);
  suspend_check->CopyEnvironmentFromWithLoopPhiAdjustment(
      loop->GetSuspendCheck()->GetEnvironment(), header);

  // Update loop information.
  new_header->AddBackEdge(new_body);
  new_header->GetLoopInformation()->SetSuspendCheck(suspend_check);
  new_header->GetLoopInformation()->Populate();
  new_pre_header->SetLoopInformation(loop->GetPreHeader()->GetLoopInformation());  // outward
  HLoopInformationOutwardIterator it(*new_header);
  for (it.Advance(); !it.Done(); it.Advance()) {
    it.Current()->Add(new_pre_header);
    it.Current()->Add(new_header);
    it.Current()->Add(new_body);
  }
  return new_pre_header;
}

static void CheckAgainstUpperBound(ReferenceTypeInfo rti, ReferenceTypeInfo upper_bound_rti) {
  if (rti.IsValid()) {
    ScopedObjectAccess soa(Thread::Current());
    DCHECK(upper_bound_rti.IsSupertypeOf(rti))
        << " upper_bound_rti: " << upper_bound_rti
        << " rti: " << rti;
    DCHECK_IMPLIES(upper_bound_rti.GetTypeHandle()->CannotBeAssignedFromOtherTypes(), rti.IsExact())
        << " upper_bound_rti: " << upper_bound_rti
        << " rti: " << rti;
  }
}

void HInstruction::SetReferenceTypeInfo(ReferenceTypeInfo rti) {
  if (kIsDebugBuild) {
    DCHECK_EQ(GetType(), DataType::Type::kReference);
    DCHECK(rti.IsValid()) << "Invalid RTI for " << DebugName();
    if (IsBoundType()) {
      // Having the test here spares us from making the method virtual just for
      // the sake of a DCHECK.
      CheckAgainstUpperBound(rti, AsBoundType()->GetUpperBound());
    }
  }
  reference_type_handle_ = rti.GetTypeHandle();
  SetPackedFlag<kFlagReferenceTypeIsExact>(rti.IsExact());
}

void HInstruction::SetReferenceTypeInfoIfValid(ReferenceTypeInfo rti) {
  if (rti.IsValid()) {
    SetReferenceTypeInfo(rti);
  }
}

bool HBoundType::InstructionDataEquals(const HInstruction* other) const {
  const HBoundType* other_bt = other->AsBoundType();
  ScopedObjectAccess soa(Thread::Current());
  return GetUpperBound().IsEqual(other_bt->GetUpperBound()) &&
         GetUpperCanBeNull() == other_bt->GetUpperCanBeNull() &&
         CanBeNull() == other_bt->CanBeNull();
}

void HBoundType::SetUpperBound(const ReferenceTypeInfo& upper_bound, bool can_be_null) {
  if (kIsDebugBuild) {
    DCHECK(upper_bound.IsValid());
    DCHECK(!upper_bound_.IsValid()) << "Upper bound should only be set once.";
    CheckAgainstUpperBound(GetReferenceTypeInfo(), upper_bound);
  }
  upper_bound_ = upper_bound;
  SetPackedFlag<kFlagUpperCanBeNull>(can_be_null);
}

bool HInstruction::HasAnyEnvironmentUseBefore(HInstruction* other) {
  // For now, assume that instructions in different blocks may use the
  // environment.
  // TODO: Use the control flow to decide if this is true.
  if (GetBlock() != other->GetBlock()) {
    return true;
  }

  // We know that we are in the same block. Walk from 'this' to 'other',
  // checking to see if there is any instruction with an environment.
  HInstruction* current = this;
  for (; current != other && current != nullptr; current = current->GetNext()) {
    // This is a conservative check, as the instruction result may not be in
    // the referenced environment.
    if (current->HasEnvironment()) {
      return true;
    }
  }

  // We should have been called with 'this' before 'other' in the block.
  // Just confirm this.
  DCHECK(current != nullptr);
  return false;
}

void HInvoke::SetIntrinsic(Intrinsics intrinsic,
                           IntrinsicNeedsEnvironment needs_env,
                           IntrinsicSideEffects side_effects,
                           IntrinsicExceptions exceptions) {
  intrinsic_ = intrinsic;
  IntrinsicOptimizations opt(this);

  // Adjust method's side effects from intrinsic table.
  switch (side_effects) {
    case kNoSideEffects: SetSideEffects(SideEffects::None()); break;
    case kReadSideEffects: SetSideEffects(SideEffects::AllReads()); break;
    case kWriteSideEffects: SetSideEffects(SideEffects::AllWrites()); break;
    case kAllSideEffects: SetSideEffects(SideEffects::AllExceptGCDependency()); break;
  }

  if (needs_env == kNoEnvironment) {
    opt.SetDoesNotNeedEnvironment();
  } else {
    // If we need an environment, that means there will be a call, which can trigger GC.
    SetSideEffects(GetSideEffects().Union(SideEffects::CanTriggerGC()));
  }
  // Adjust method's exception status from intrinsic table.
  SetCanThrow(exceptions == kCanThrow);
}

bool HNewInstance::IsStringAlloc() const {
  return GetEntrypoint() == kQuickAllocStringObject;
}

bool HInvoke::NeedsEnvironment() const {
  if (!IsIntrinsic()) {
    return true;
  }
  IntrinsicOptimizations opt(*this);
  return !opt.GetDoesNotNeedEnvironment();
}

const DexFile& HInvokeStaticOrDirect::GetDexFileForPcRelativeDexCache() const {
  ArtMethod* caller = GetEnvironment()->GetMethod();
  ScopedObjectAccess soa(Thread::Current());
  // `caller` is null for a top-level graph representing a method whose declaring
  // class was not resolved.
  return caller == nullptr ? GetBlock()->GetGraph()->GetDexFile() : *caller->GetDexFile();
}

std::ostream& operator<<(std::ostream& os, HInvokeStaticOrDirect::ClinitCheckRequirement rhs) {
  switch (rhs) {
    case HInvokeStaticOrDirect::ClinitCheckRequirement::kExplicit:
      return os << "explicit";
    case HInvokeStaticOrDirect::ClinitCheckRequirement::kImplicit:
      return os << "implicit";
    case HInvokeStaticOrDirect::ClinitCheckRequirement::kNone:
      return os << "none";
  }
}

bool HInvokeStaticOrDirect::CanBeNull() const {
  if (IsStringInit()) {
    return false;
  }
  return HInvoke::CanBeNull();
}

bool HInvoke::CanBeNull() const {
  switch (GetIntrinsic()) {
    case Intrinsics::kThreadCurrentThread:
    case Intrinsics::kStringBufferAppend:
    case Intrinsics::kStringBufferToString:
    case Intrinsics::kStringBuilderAppendObject:
    case Intrinsics::kStringBuilderAppendString:
    case Intrinsics::kStringBuilderAppendCharSequence:
    case Intrinsics::kStringBuilderAppendCharArray:
    case Intrinsics::kStringBuilderAppendBoolean:
    case Intrinsics::kStringBuilderAppendChar:
    case Intrinsics::kStringBuilderAppendInt:
    case Intrinsics::kStringBuilderAppendLong:
    case Intrinsics::kStringBuilderAppendFloat:
    case Intrinsics::kStringBuilderAppendDouble:
    case Intrinsics::kStringBuilderToString:
#define DEFINE_BOXED_CASE(name, unused1, unused2, unused3, unused4) \
    case Intrinsics::k##name##ValueOf:
    BOXED_TYPES(DEFINE_BOXED_CASE)
#undef DEFINE_BOXED_CASE
      return false;
    default:
      return GetType() == DataType::Type::kReference;
  }
}

bool HInvokeVirtual::CanDoImplicitNullCheckOn(HInstruction* obj) const {
  if (obj != InputAt(0)) {
    return false;
  }
  switch (GetIntrinsic()) {
    case Intrinsics::kNone:
      return true;
    case Intrinsics::kReferenceRefersTo:
      return true;
    default:
      // TODO: Add implicit null checks in more intrinsics.
      return false;
  }
}

bool HLoadClass::InstructionDataEquals(const HInstruction* other) const {
  const HLoadClass* other_load_class = other->AsLoadClass();
  // TODO: To allow GVN for HLoadClass from different dex files, we should compare the type
  // names rather than type indexes. However, we shall also have to re-think the hash code.
  if (type_index_ != other_load_class->type_index_ ||
      GetPackedFields() != other_load_class->GetPackedFields()) {
    return false;
  }
  switch (GetLoadKind()) {
    case LoadKind::kBootImageRelRo:
    case LoadKind::kJitBootImageAddress:
    case LoadKind::kJitTableAddress: {
      ScopedObjectAccess soa(Thread::Current());
      return GetClass().Get() == other_load_class->GetClass().Get();
    }
    default:
      DCHECK(HasTypeReference(GetLoadKind()));
      return IsSameDexFile(GetDexFile(), other_load_class->GetDexFile());
  }
}

bool HLoadString::InstructionDataEquals(const HInstruction* other) const {
  const HLoadString* other_load_string = other->AsLoadString();
  // TODO: To allow GVN for HLoadString from different dex files, we should compare the strings
  // rather than their indexes. However, we shall also have to re-think the hash code.
  if (string_index_ != other_load_string->string_index_ ||
      GetPackedFields() != other_load_string->GetPackedFields()) {
    return false;
  }
  switch (GetLoadKind()) {
    case LoadKind::kBootImageRelRo:
    case LoadKind::kJitBootImageAddress:
    case LoadKind::kJitTableAddress: {
      ScopedObjectAccess soa(Thread::Current());
      return GetString().Get() == other_load_string->GetString().Get();
    }
    default:
      return IsSameDexFile(GetDexFile(), other_load_string->GetDexFile());
  }
}

void HInstruction::RemoveEnvironmentUsers() {
  for (const HUseListNode<HEnvironment*>& use : GetEnvUses()) {
    HEnvironment* user = use.GetUser();
    user->SetRawEnvAt(use.GetIndex(), nullptr);
  }
  env_uses_.clear();
}

HInstruction* ReplaceInstrOrPhiByClone(HInstruction* instr) {
  HInstruction* clone = instr->Clone(instr->GetBlock()->GetGraph()->GetAllocator());
  HBasicBlock* block = instr->GetBlock();

  if (instr->IsPhi()) {
    HPhi* phi = instr->AsPhi();
    DCHECK(!phi->HasEnvironment());
    HPhi* phi_clone = clone->AsPhi();
    block->ReplaceAndRemovePhiWith(phi, phi_clone);
  } else {
    block->ReplaceAndRemoveInstructionWith(instr, clone);
    if (instr->HasEnvironment()) {
      clone->CopyEnvironmentFrom(instr->GetEnvironment());
      HLoopInformation* loop_info = block->GetLoopInformation();
      if (instr->IsSuspendCheck() && loop_info != nullptr) {
        loop_info->SetSuspendCheck(clone->AsSuspendCheck());
      }
    }
  }
  return clone;
}

std::ostream& operator<<(std::ostream& os, const MoveOperands& rhs) {
  os << "["
     << " source=" << rhs.GetSource()
     << " destination=" << rhs.GetDestination()
     << " type=" << rhs.GetType()
     << " instruction=";
  if (rhs.GetInstruction() != nullptr) {
    os << rhs.GetInstruction()->DebugName() << ' ' << rhs.GetInstruction()->GetId();
  } else {
    os << "null";
  }
  os << " ]";
  return os;
}

std::ostream& operator<<(std::ostream& os, TypeCheckKind rhs) {
  switch (rhs) {
    case TypeCheckKind::kUnresolvedCheck:
      return os << "unresolved_check";
    case TypeCheckKind::kExactCheck:
      return os << "exact_check";
    case TypeCheckKind::kClassHierarchyCheck:
      return os << "class_hierarchy_check";
    case TypeCheckKind::kAbstractClassCheck:
      return os << "abstract_class_check";
    case TypeCheckKind::kInterfaceCheck:
      return os << "interface_check";
    case TypeCheckKind::kArrayObjectCheck:
      return os << "array_object_check";
    case TypeCheckKind::kArrayCheck:
      return os << "array_check";
    case TypeCheckKind::kBitstringCheck:
      return os << "bitstring_check";
  }
}

// Check that intrinsic enum values fit within space set aside in ArtMethod modifier flags.
#define CHECK_INTRINSICS_ENUM_VALUES(Name, InvokeType, _, SideEffects, Exceptions, ...) \
  static_assert( \
    static_cast<uint32_t>(Intrinsics::k ## Name) <= (kAccIntrinsicBits >> CTZ(kAccIntrinsicBits)), \
    "Intrinsics enumeration space overflow.");
  ART_INTRINSICS_LIST(CHECK_INTRINSICS_ENUM_VALUES)
#undef CHECK_INTRINSICS_ENUM_VALUES

// Function that returns whether an intrinsic needs an environment or not.
static inline IntrinsicNeedsEnvironment NeedsEnvironmentIntrinsic(Intrinsics i) {
  switch (i) {
    case Intrinsics::kNone:
      return kNeedsEnvironment;  // Non-sensical for intrinsic.
#define OPTIMIZING_INTRINSICS(Name, InvokeType, NeedsEnv, SideEffects, Exceptions, ...) \
    case Intrinsics::k ## Name: \
      return NeedsEnv;
      ART_INTRINSICS_LIST(OPTIMIZING_INTRINSICS)
#undef OPTIMIZING_INTRINSICS
  }
  return kNeedsEnvironment;
}

// Function that returns whether an intrinsic has side effects.
static inline IntrinsicSideEffects GetSideEffectsIntrinsic(Intrinsics i) {
  switch (i) {
    case Intrinsics::kNone:
      return kAllSideEffects;
#define OPTIMIZING_INTRINSICS(Name, InvokeType, NeedsEnv, SideEffects, Exceptions, ...) \
    case Intrinsics::k ## Name: \
      return SideEffects;
      ART_INTRINSICS_LIST(OPTIMIZING_INTRINSICS)
#undef OPTIMIZING_INTRINSICS
  }
  return kAllSideEffects;
}

// Function that returns whether an intrinsic can throw exceptions.
static inline IntrinsicExceptions GetExceptionsIntrinsic(Intrinsics i) {
  switch (i) {
    case Intrinsics::kNone:
      return kCanThrow;
#define OPTIMIZING_INTRINSICS(Name, InvokeType, NeedsEnv, SideEffects, Exceptions, ...) \
    case Intrinsics::k ## Name: \
      return Exceptions;
      ART_INTRINSICS_LIST(OPTIMIZING_INTRINSICS)
#undef OPTIMIZING_INTRINSICS
  }
  return kCanThrow;
}

void HInvoke::SetResolvedMethod(ArtMethod* method, bool enable_intrinsic_opt) {
  if (method != nullptr && method->IsIntrinsic() && enable_intrinsic_opt) {
    Intrinsics intrinsic = method->GetIntrinsic();
    SetIntrinsic(intrinsic,
                 NeedsEnvironmentIntrinsic(intrinsic),
                 GetSideEffectsIntrinsic(intrinsic),
                 GetExceptionsIntrinsic(intrinsic));
  }
  resolved_method_ = method;
}

bool IsGEZero(HInstruction* instruction) {
  DCHECK(instruction != nullptr);
  if (instruction->IsArrayLength()) {
    return true;
  } else if (instruction->IsMin()) {
    // Instruction MIN(>=0, >=0) is >= 0.
    return IsGEZero(instruction->InputAt(0)) &&
           IsGEZero(instruction->InputAt(1));
  } else if (instruction->IsAbs()) {
    // Instruction ABS(>=0) is >= 0.
    // NOTE: ABS(minint) = minint prevents assuming
    //       >= 0 without looking at the argument.
    return IsGEZero(instruction->InputAt(0));
  }
  int64_t value = -1;
  return IsInt64AndGet(instruction, &value) && value >= 0;
}

}  // namespace art
