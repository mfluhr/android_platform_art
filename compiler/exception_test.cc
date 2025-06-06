/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include <android-base/test_utils.h>

#include <memory>
#include <type_traits>

#include "art_method-inl.h"
#include "base/arena_allocator.h"
#include "base/callee_save_type.h"
#include "base/leb128.h"
#include "base/macros.h"
#include "base/malloc_arena_pool.h"
#include "base/pointer_size.h"
#include "class_linker.h"
#include "common_runtime_test.h"
#include "dex/code_item_accessors-inl.h"
#include "dex/dex_file-inl.h"
#include "dex/dex_file.h"
#include "dex/dex_file_exception_helpers.h"
#include "gtest/gtest.h"
#include "handle_scope-inl.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/stack_trace_element-inl.h"
#include "oat/oat_quick_method_header.h"
#include "obj_ptr-inl.h"
#include "optimizing/stack_map_stream.h"
#include "runtime-inl.h"
#include "scoped_thread_state_change-inl.h"
#include "thread.h"

namespace art HIDDEN {

class ExceptionTest : public CommonRuntimeTest {
 protected:
  // Since various dexers may differ in bytecode layout, we play
  // it safe and simply set the dex pc to the start of the method,
  // which always points to the first source statement.
  static constexpr const uint32_t kDexPc = 0;

  void SetUp() override {
    CommonRuntimeTest::SetUp();

    ScopedObjectAccess soa(Thread::Current());
    StackHandleScope<2> hs(soa.Self());
    Handle<mirror::ClassLoader> class_loader(
        hs.NewHandle(soa.Decode<mirror::ClassLoader>(LoadDex("ExceptionHandle"))));
    my_klass_ = FindClass("LExceptionHandle;", class_loader);
    ASSERT_TRUE(my_klass_ != nullptr);
    Handle<mirror::Class> klass(hs.NewHandle(my_klass_));
    class_linker_->EnsureInitialized(soa.Self(), klass, true, true);
    my_klass_ = klass.Get();

    dex_ = my_klass_->GetDexCache()->GetDexFile();

    std::vector<uint8_t> fake_code;
    uint32_t code_size = 12;
    for (size_t i = 0 ; i < code_size; i++) {
      fake_code.push_back(0x70 | i);
    }

    const uint32_t native_pc_offset = 4u;
    CHECK_ALIGNED_PARAM(native_pc_offset, GetInstructionSetInstructionAlignment(kRuntimeISA));

    MallocArenaPool pool;
    ArenaStack arena_stack(&pool);
    ScopedArenaAllocator allocator(&arena_stack);
    StackMapStream stack_maps(&allocator, kRuntimeISA);
    stack_maps.BeginMethod(/* frame_size_in_bytes= */ 4 * sizeof(void*),
                           /* core_spill_mask= */ 0u,
                           /* fp_spill_mask= */ 0u,
                           /* num_dex_registers= */ 0u,
                           /* baseline= */ false,
                           /* debuggable= */ false);
    stack_maps.BeginStackMapEntry(kDexPc, native_pc_offset);
    stack_maps.EndStackMapEntry();
    stack_maps.EndMethod(code_size);
    ScopedArenaVector<uint8_t> stack_map = stack_maps.Encode();

    const size_t stack_maps_size = stack_map.size();
    const size_t header_size = sizeof(OatQuickMethodHeader);
    const size_t code_alignment = GetInstructionSetCodeAlignment(kRuntimeISA);

    fake_header_code_and_maps_size_ = stack_maps_size + header_size + code_size + code_alignment;
    // Use mmap to make sure we get untagged memory here. Real code gets allocated using
    // mspace_memalign which is never tagged.
    fake_header_code_and_maps_ = static_cast<uint8_t*>(mmap(nullptr,
                                                            fake_header_code_and_maps_size_,
                                                            PROT_READ | PROT_WRITE,
                                                            MAP_PRIVATE | MAP_ANONYMOUS,
                                                            -1,
                                                            0));
    uint8_t* code_ptr =
      AlignUp(&fake_header_code_and_maps_[stack_maps_size + header_size], code_alignment);

    memcpy(&fake_header_code_and_maps_[0], stack_map.data(), stack_maps_size);
    OatQuickMethodHeader method_header(code_ptr - fake_header_code_and_maps_);
    static_assert(std::is_trivially_copyable<OatQuickMethodHeader>::value, "Cannot use memcpy");
    memcpy(code_ptr - header_size, &method_header, header_size);
    memcpy(code_ptr, fake_code.data(), fake_code.size());

    if (kRuntimeISA == InstructionSet::kArm) {
      // Check that the Thumb2 adjustment will be a NOP, see EntryPointToCodePointer().
      CHECK_ALIGNED(code_ptr, 2);
    }

    method_f_ = my_klass_->FindClassMethod("f", "()I", kRuntimePointerSize);
    ASSERT_TRUE(method_f_ != nullptr);
    ASSERT_FALSE(method_f_->IsDirect());
    method_f_->SetEntryPointFromQuickCompiledCode(code_ptr);

    method_g_ = my_klass_->FindClassMethod("g", "(I)V", kRuntimePointerSize);
    ASSERT_TRUE(method_g_ != nullptr);
    ASSERT_FALSE(method_g_->IsDirect());
    method_g_->SetEntryPointFromQuickCompiledCode(code_ptr);
  }

  void TearDown() override { munmap(fake_header_code_and_maps_, fake_header_code_and_maps_size_); }

  const DexFile* dex_;

  size_t fake_header_code_and_maps_size_;
  uint8_t* fake_header_code_and_maps_;

  ArtMethod* method_f_;
  ArtMethod* method_g_;

 private:
  ObjPtr<mirror::Class> my_klass_;
};

TEST_F(ExceptionTest, FindCatchHandler) {
  ScopedObjectAccess soa(Thread::Current());
  CodeItemDataAccessor accessor(*dex_, method_f_->GetCodeItem());

  ASSERT_TRUE(accessor.HasCodeItem());

  ASSERT_EQ(2u, accessor.TriesSize());
  ASSERT_NE(0u, accessor.InsnsSizeInCodeUnits());

  const dex::TryItem& t0 = accessor.TryItems().begin()[0];
  const dex::TryItem& t1 = accessor.TryItems().begin()[1];
  EXPECT_LE(t0.start_addr_, t1.start_addr_);
  {
    CatchHandlerIterator iter(accessor, 4 /* Dex PC in the first try block */);
    EXPECT_STREQ("Ljava/io/IOException;", dex_->GetTypeDescriptor(iter.GetHandlerTypeIndex()));
    ASSERT_TRUE(iter.HasNext());
    iter.Next();
    EXPECT_STREQ("Ljava/lang/Exception;", dex_->GetTypeDescriptor(iter.GetHandlerTypeIndex()));
    ASSERT_TRUE(iter.HasNext());
    iter.Next();
    EXPECT_FALSE(iter.HasNext());
  }
  {
    CatchHandlerIterator iter(accessor, 8 /* Dex PC in the second try block */);
    EXPECT_STREQ("Ljava/io/IOException;", dex_->GetTypeDescriptor(iter.GetHandlerTypeIndex()));
    ASSERT_TRUE(iter.HasNext());
    iter.Next();
    EXPECT_FALSE(iter.HasNext());
  }
  {
    CatchHandlerIterator iter(accessor, 11 /* Dex PC not in any try block */);
    EXPECT_FALSE(iter.HasNext());
  }
}

TEST_F(ExceptionTest, StackTraceElement) {
  Thread* thread = Thread::Current();
  thread->TransitionFromSuspendedToRunnable();
  bool started = runtime_->Start();
  CHECK(started);
  JNIEnv* env = thread->GetJniEnv();
  ScopedObjectAccess soa(env);

  std::vector<uintptr_t> fake_stack;
  Runtime* r = Runtime::Current();
  r->SetInstructionSet(kRuntimeISA);
  ArtMethod* save_method = r->CreateCalleeSaveMethod();
  r->SetCalleeSaveMethod(save_method, CalleeSaveType::kSaveAllCalleeSaves);
  QuickMethodFrameInfo frame_info = r->GetRuntimeMethodFrameInfo(save_method);

  ASSERT_EQ(kStackAlignment, 16U);
  // ASSERT_EQ(sizeof(uintptr_t), sizeof(uint32_t));

  // Create the stack frame for the callee save method, expected by the runtime.
  fake_stack.push_back(reinterpret_cast<uintptr_t>(save_method));
  for (size_t i = 0; i < frame_info.FrameSizeInBytes() - 2 * sizeof(uintptr_t);
       i += sizeof(uintptr_t)) {
    fake_stack.push_back(0);
  }

  OatQuickMethodHeader* header = OatQuickMethodHeader::FromEntryPoint(
      method_g_->GetEntryPointFromQuickCompiledCode());
  // Untag native pc when running with hwasan since the pcs on the stack aren't tagged and we use
  // this to create a fake stack. See OatQuickMethodHeader::Contains where we untag code pointers
  // before comparing it with the PC from the stack.
  uintptr_t native_pc = header->ToNativeQuickPc(method_g_, kDexPc);
  if (running_with_hwasan()) {
    // TODO(228989263): Use HWASanUntag once we have a hwasan target for tests too. HWASanUntag
    // uses static checks which won't work if we don't have a dedicated target.
    native_pc = (native_pc & ((1ULL << 56) - 1));
  }
  fake_stack.push_back(native_pc);  // return pc

  // Create/push fake 16byte stack frame for method g
  fake_stack.push_back(reinterpret_cast<uintptr_t>(method_g_));
  fake_stack.push_back(0);
  fake_stack.push_back(0);
  fake_stack.push_back(native_pc);  // return pc.

  // Create/push fake 16byte stack frame for method f
  fake_stack.push_back(reinterpret_cast<uintptr_t>(method_f_));
  fake_stack.push_back(0);
  fake_stack.push_back(0);
  fake_stack.push_back(0xEBAD6070);  // return pc

  // Push Method* of null to terminate the trace
  fake_stack.push_back(0);

  // Push null values which will become null incoming arguments.
  fake_stack.push_back(0);
  fake_stack.push_back(0);
  fake_stack.push_back(0);

  // Set up thread to appear as if we called out of method_g_ at given pc dex.
  thread->SetTopOfStack(reinterpret_cast<ArtMethod**>(&fake_stack[0]));

  jobject internal = soa.AddLocalReference<jobject>(thread->CreateInternalStackTrace(soa));
  ASSERT_TRUE(internal != nullptr);
  jobjectArray ste_array = Thread::InternalStackTraceToStackTraceElementArray(soa, internal);
  ASSERT_TRUE(ste_array != nullptr);
  auto trace_array = soa.Decode<mirror::ObjectArray<mirror::StackTraceElement>>(ste_array);

  ASSERT_TRUE(trace_array != nullptr);
  ASSERT_TRUE(trace_array->Get(0) != nullptr);
  EXPECT_STREQ("ExceptionHandle",
               trace_array->Get(0)->GetDeclaringClass()->ToModifiedUtf8().c_str());
  EXPECT_STREQ("ExceptionHandle.java",
               trace_array->Get(0)->GetFileName()->ToModifiedUtf8().c_str());
  EXPECT_STREQ("g", trace_array->Get(0)->GetMethodName()->ToModifiedUtf8().c_str());
  EXPECT_EQ(36, trace_array->Get(0)->GetLineNumber());

  ASSERT_TRUE(trace_array->Get(1) != nullptr);
  EXPECT_STREQ("ExceptionHandle",
               trace_array->Get(1)->GetDeclaringClass()->ToModifiedUtf8().c_str());
  EXPECT_STREQ("ExceptionHandle.java",
               trace_array->Get(1)->GetFileName()->ToModifiedUtf8().c_str());
  EXPECT_STREQ("f", trace_array->Get(1)->GetMethodName()->ToModifiedUtf8().c_str());
  EXPECT_EQ(22, trace_array->Get(1)->GetLineNumber());

  thread->SetTopOfStack(nullptr);  // Disarm the assertion that no code is running when we detach.
}

}  // namespace art
