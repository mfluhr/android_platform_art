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

#include "dex_cache.h"

#include <stdio.h>

#include "art_method-inl.h"
#include "class_linker.h"
#include "common_runtime_test.h"
#include "handle_scope-inl.h"
#include "linear_alloc.h"
#include "mirror/class_loader-inl.h"
#include "mirror/dex_cache-inl.h"
#include "scoped_thread_state_change-inl.h"

namespace art HIDDEN {
namespace mirror {

class DexCacheTest : public CommonRuntimeTest {
 protected:
  DexCacheTest() {
    this->use_boot_image_ = true;  // Make the Runtime creation cheaper.
  }
};

class DexCacheMethodHandlesTest : public DexCacheTest {
 protected:
  void SetUpRuntimeOptions(RuntimeOptions* options) override {
    CommonRuntimeTest::SetUpRuntimeOptions(options);
  }
};

TEST_F(DexCacheTest, Open) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<1> hs(soa.Self());
  ASSERT_TRUE(java_lang_dex_file_ != nullptr);
  Handle<DexCache> dex_cache(
      hs.NewHandle(class_linker_->AllocAndInitializeDexCache(
          soa.Self(), *java_lang_dex_file_, /*class_loader=*/nullptr)));
  ASSERT_TRUE(dex_cache != nullptr);

  // The cache is initially empty.
  EXPECT_EQ(0u, dex_cache->NumStrings());
  EXPECT_EQ(0u, dex_cache->NumResolvedTypes());
  EXPECT_EQ(0u, dex_cache->NumResolvedMethods());
  EXPECT_EQ(0u, dex_cache->NumResolvedFields());
  EXPECT_EQ(0u, dex_cache->NumResolvedMethodTypes());
}

TEST_F(DexCacheMethodHandlesTest, Open) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<1> hs(soa.Self());
  ASSERT_TRUE(java_lang_dex_file_ != nullptr);
  Handle<DexCache> dex_cache(
      hs.NewHandle(class_linker_->AllocAndInitializeDexCache(
          soa.Self(), *java_lang_dex_file_, /*class_loader=*/nullptr)));

  EXPECT_EQ(0u, dex_cache->NumResolvedMethodTypes());
}

TEST_F(DexCacheTest, TestResolvedFieldAccess) {
  ScopedObjectAccess soa(Thread::Current());
  jobject jclass_loader(LoadDex("Packages"));
  ASSERT_TRUE(jclass_loader != nullptr);
  StackHandleScope<3> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(
      soa.Decode<mirror::ClassLoader>(jclass_loader)));
  Handle<mirror::Class> klass1 = hs.NewHandle(FindClass("Lpackage1/Package1;", class_loader));
  ASSERT_TRUE(klass1 != nullptr);
  Handle<mirror::Class> klass2 = hs.NewHandle(FindClass("Lpackage2/Package2;", class_loader));
  ASSERT_TRUE(klass2 != nullptr);
  EXPECT_OBJ_PTR_EQ(klass1->GetDexCache(), klass2->GetDexCache());

  EXPECT_NE(klass1->ComputeNumStaticFields(), 0u);
  for (ArtField& field : klass2->GetFields()) {
    if (field.IsStatic()) {
      EXPECT_FALSE(
          klass1->ResolvedFieldAccessTest</*throw_on_failure=*/ false>(
              klass2.Get(),
              &field,
              klass1->GetDexCache(),
              field.GetDexFieldIndex()));
    }
  }
}

TEST_F(DexCacheMethodHandlesTest, TestResolvedMethodTypes) {
  ScopedObjectAccess soa(Thread::Current());
  jobject jclass_loader(LoadDex("MethodTypes"));
  ASSERT_TRUE(jclass_loader != nullptr);

  StackHandleScope<5> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(
      soa.Decode<mirror::ClassLoader>(jclass_loader)));

  Handle<mirror::Class> method_types = hs.NewHandle(FindClass("LMethodTypes;", class_loader));
  class_linker_->EnsureInitialized(soa.Self(), method_types, true, true);

  ArtMethod* method1 = method_types->FindClassMethod(
      "method1",
      "(Ljava/lang/String;)Ljava/lang/String;",
      kRuntimePointerSize);
  ASSERT_TRUE(method1 != nullptr);
  ASSERT_FALSE(method1->IsDirect());
  ArtMethod* method2 = method_types->FindClassMethod(
      "method2",
      "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
      kRuntimePointerSize);
  ASSERT_TRUE(method2 != nullptr);
  ASSERT_FALSE(method2->IsDirect());

  const DexFile& dex_file = *(method1->GetDexFile());
  Handle<mirror::DexCache> dex_cache = hs.NewHandle(
      class_linker_->FindDexCache(Thread::Current(), dex_file));

  const dex::MethodId& method1_id = dex_file.GetMethodId(method1->GetDexMethodIndex());
  const dex::MethodId& method2_id = dex_file.GetMethodId(method2->GetDexMethodIndex());
  Handle<mirror::MethodType> method1_type = hs.NewHandle(
      class_linker_->ResolveMethodType(soa.Self(),
                                       method1_id.proto_idx_,
                                       dex_cache,
                                       class_loader));
  Handle<mirror::MethodType> method2_type = hs.NewHandle(
      class_linker_->ResolveMethodType(soa.Self(),
                                       method2_id.proto_idx_,
                                       dex_cache,
                                       class_loader));
  EXPECT_EQ(method1_type.Get(), dex_cache->GetResolvedMethodType(method1_id.proto_idx_));
  EXPECT_EQ(method2_type.Get(), dex_cache->GetResolvedMethodType(method2_id.proto_idx_));

  // The MethodTypes dex file contains a single interface with two abstract
  // methods. It must therefore contain precisely two method IDs.
  ASSERT_EQ(2u, dex_file.NumProtoIds());
  ASSERT_EQ(dex_file.NumProtoIds(), dex_cache->NumResolvedMethodTypesArray());
  ASSERT_EQ(0u, dex_cache->NumResolvedMethodTypes());
  auto* method_types_cache = dex_cache->GetResolvedMethodTypesArray();

  for (size_t i = 0; i < dex_file.NumProtoIds(); ++i) {
    auto* method_type = method_types_cache->Get(i);
    if (dex::ProtoIndex(i) == method1_id.proto_idx_) {
      ASSERT_EQ(method1_type.Get(), method_type);
    } else if (dex::ProtoIndex(i) == method2_id.proto_idx_) {
      ASSERT_EQ(method2_type.Get(), method_type);
    } else {
      ASSERT_TRUE(false);
    }
  }
}

}  // namespace mirror
}  // namespace art
