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

#include "object.h"

#include <stdint.h>
#include <stdio.h>
#include <memory>

#include "array-alloc-inl.h"
#include "array-inl.h"
#include "art_field-inl.h"
#include "art_method-inl.h"
#include "asm_support.h"
#include "base/pointer_size.h"
#include "class-alloc-inl.h"
#include "class-inl.h"
#include "class_linker-inl.h"
#include "class_linker.h"
#include "class_root-inl.h"
#include "common_runtime_test.h"
#include "dex/dex_file.h"
#include "entrypoints/entrypoint_utils-inl.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/heap.h"
#include "handle_scope-inl.h"
#include "iftable-inl.h"
#include "obj_ptr.h"
#include "object-inl.h"
#include "object_array-alloc-inl.h"
#include "object_array-inl.h"
#include "scoped_thread_state_change-inl.h"
#include "string-inl.h"

namespace art HIDDEN {
namespace mirror {

class ObjectTest : public CommonRuntimeTest {
 protected:
  ObjectTest() {
    use_boot_image_ = true;  // Make the Runtime creation cheaper.
  }

  void AssertString(int32_t expected_utf16_length,
                    const char* utf8_in,
                    const char* utf16_expected_le,
                    int32_t expected_hash)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    std::unique_ptr<uint16_t[]> utf16_expected(new uint16_t[expected_utf16_length]);
    for (int32_t i = 0; i < expected_utf16_length; i++) {
      uint16_t ch = (((utf16_expected_le[i*2 + 0] & 0xff) << 8) |
                     ((utf16_expected_le[i*2 + 1] & 0xff) << 0));
      utf16_expected[i] = ch;
    }

    Thread* self = Thread::Current();
    StackHandleScope<1> hs(self);
    Handle<String> string(
        hs.NewHandle(String::AllocFromModifiedUtf8(self, expected_utf16_length, utf8_in)));
    ASSERT_EQ(expected_utf16_length, string->GetLength());
    ASSERT_EQ(string->IsValueNull(), false);
    // strlen is necessary because the 1-character string "\x00\x00" is interpreted as ""
    ASSERT_TRUE(string->Equals(utf8_in) || (expected_utf16_length == 1 && strlen(utf8_in) == 0));
    for (int32_t i = 0; i < expected_utf16_length; i++) {
      EXPECT_EQ(utf16_expected[i], string->CharAt(i));
    }
    EXPECT_EQ(expected_hash, string->GetHashCode());
  }

  template <class T>
  ObjPtr<mirror::ObjectArray<T>> AllocObjectArray(Thread* self, size_t length)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    return mirror::ObjectArray<T>::Alloc(
        self, GetClassRoot(ClassRoot::kObjectArrayClass, class_linker_), length);
  }
};

// Keep constants in sync.
TEST_F(ObjectTest, Constants) {
  EXPECT_EQ(kObjectReferenceSize, sizeof(HeapReference<Object>));
  EXPECT_EQ(kObjectHeaderSize, sizeof(Object));
  EXPECT_EQ(ART_METHOD_QUICK_CODE_OFFSET_32,
            ArtMethod::EntryPointFromQuickCompiledCodeOffset(PointerSize::k32).
                Int32Value());
  EXPECT_EQ(ART_METHOD_QUICK_CODE_OFFSET_64,
            ArtMethod::EntryPointFromQuickCompiledCodeOffset(PointerSize::k64).
                Int32Value());
}

TEST_F(ObjectTest, IsInSamePackage) {
  // Matches
  EXPECT_TRUE(Class::IsInSamePackage("Ljava/lang/Object;", "Ljava/lang/Class;"));
  EXPECT_TRUE(Class::IsInSamePackage("LFoo;", "LBar;"));

  // Mismatches
  EXPECT_FALSE(Class::IsInSamePackage("Ljava/lang/Object;", "Ljava/io/File;"));
  EXPECT_FALSE(Class::IsInSamePackage("Ljava/lang/Object;", "Ljava/lang/reflect/Method;"));
}

TEST_F(ObjectTest, Clone) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<2> hs(soa.Self());
  Handle<ObjectArray<Object>> a1(hs.NewHandle(AllocObjectArray<Object>(soa.Self(), 256)));
  size_t s1 = a1->SizeOf();
  ObjPtr<Object> clone = Object::Clone(a1, soa.Self());
  EXPECT_EQ(s1, clone->SizeOf());
  EXPECT_TRUE(clone->GetClass() == a1->GetClass());
}

TEST_F(ObjectTest, AllocObjectArray) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<3> hs(soa.Self());
  Handle<ObjectArray<Object>> oa(hs.NewHandle(AllocObjectArray<Object>(soa.Self(), 2)));
  EXPECT_EQ(2, oa->GetLength());
  EXPECT_TRUE(oa->Get(0) == nullptr);
  EXPECT_TRUE(oa->Get(1) == nullptr);
  oa->Set<false>(0, oa.Get());
  EXPECT_TRUE(oa->Get(0) == oa.Get());
  EXPECT_TRUE(oa->Get(1) == nullptr);
  oa->Set<false>(1, oa.Get());
  EXPECT_TRUE(oa->Get(0) == oa.Get());
  EXPECT_TRUE(oa->Get(1) == oa.Get());

  Handle<Class> aioobe = hs.NewHandle(
      class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/ArrayIndexOutOfBoundsException;"));

  EXPECT_TRUE(oa->Get(-1) == nullptr);
  EXPECT_TRUE(soa.Self()->IsExceptionPending());
  EXPECT_OBJ_PTR_EQ(aioobe.Get(), soa.Self()->GetException()->GetClass());
  soa.Self()->ClearException();

  EXPECT_TRUE(oa->Get(2) == nullptr);
  EXPECT_TRUE(soa.Self()->IsExceptionPending());
  EXPECT_OBJ_PTR_EQ(aioobe.Get(), soa.Self()->GetException()->GetClass());
  soa.Self()->ClearException();

  ASSERT_TRUE(oa->GetClass() != nullptr);
  Handle<mirror::Class> klass(hs.NewHandle(oa->GetClass()));
  ASSERT_EQ(2U, klass->NumDirectInterfaces());
  EXPECT_OBJ_PTR_EQ(class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Cloneable;"),
                    klass->GetDirectInterface(0));
  EXPECT_OBJ_PTR_EQ(class_linker_->FindSystemClass(soa.Self(), "Ljava/io/Serializable;"),
                    klass->GetDirectInterface(1));
}

TEST_F(ObjectTest, AllocArray) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<2> hs(soa.Self());
  MutableHandle<Class> c = hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "[I"));
  gc::AllocatorType allocator_type = Runtime::Current()->GetHeap()->GetCurrentAllocator();
  MutableHandle<Array> a = hs.NewHandle(
      Array::Alloc(soa.Self(), c.Get(), 1, c->GetComponentSizeShift(), allocator_type));
  EXPECT_TRUE(c.Get() == a->GetClass());
  EXPECT_EQ(1, a->GetLength());

  c.Assign(class_linker_->FindSystemClass(soa.Self(), "[Ljava/lang/Object;"));
  a.Assign(Array::Alloc(soa.Self(), c.Get(), 1, c->GetComponentSizeShift(), allocator_type));
  EXPECT_TRUE(c.Get() == a->GetClass());
  EXPECT_EQ(1, a->GetLength());

  c.Assign(class_linker_->FindSystemClass(soa.Self(), "[[Ljava/lang/Object;"));
  a.Assign(Array::Alloc(soa.Self(), c.Get(), 1, c->GetComponentSizeShift(), allocator_type));
  EXPECT_TRUE(c.Get() == a->GetClass());
  EXPECT_EQ(1, a->GetLength());
}

TEST_F(ObjectTest, AllocArray_FillUsable) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<2> hs(soa.Self());
  MutableHandle<Class> c = hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "[B"));
  gc::AllocatorType allocator_type = Runtime::Current()->GetHeap()->GetCurrentAllocator();
  MutableHandle<Array> a = hs.NewHandle(
      Array::Alloc</*kIsInstrumented=*/ true, /*kFillUsable=*/ true>(
          soa.Self(), c.Get(), 1, c->GetComponentSizeShift(), allocator_type));
  EXPECT_TRUE(c.Get() == a->GetClass());
  EXPECT_LE(1, a->GetLength());

  c.Assign(class_linker_->FindSystemClass(soa.Self(), "[I"));
  a.Assign(Array::Alloc</*kIsInstrumented=*/ true, /*kFillUsable=*/ true>(
      soa.Self(), c.Get(), 2, c->GetComponentSizeShift(), allocator_type));
  EXPECT_TRUE(c.Get() == a->GetClass());
  EXPECT_LE(2, a->GetLength());

  c.Assign(class_linker_->FindSystemClass(soa.Self(), "[Ljava/lang/Object;"));
  a.Assign(Array::Alloc</*kIsInstrumented=*/ true, /*kFillUsable=*/ true>(
      soa.Self(), c.Get(), 2, c->GetComponentSizeShift(), allocator_type));
  EXPECT_TRUE(c.Get() == a->GetClass());
  EXPECT_LE(2, a->GetLength());

  c.Assign(class_linker_->FindSystemClass(soa.Self(), "[[Ljava/lang/Object;"));
  a.Assign(Array::Alloc</*kIsInstrumented=*/ true, /*kFillUsable=*/ true>(
      soa.Self(), c.Get(), 2, c->GetComponentSizeShift(), allocator_type));
  EXPECT_TRUE(c.Get() == a->GetClass());
  EXPECT_LE(2, a->GetLength());
}

template<typename ArrayT>
void TestPrimitiveArray(ClassLinker* cl) {
  ScopedObjectAccess soa(Thread::Current());
  using T = typename ArrayT::ElementType;

  StackHandleScope<2> hs(soa.Self());
  Handle<ArrayT> a = hs.NewHandle(ArrayT::Alloc(soa.Self(), 2));
  EXPECT_EQ(2, a->GetLength());
  EXPECT_EQ(0, a->Get(0));
  EXPECT_EQ(0, a->Get(1));
  a->Set(0, T(123));
  EXPECT_EQ(T(123), a->Get(0));
  EXPECT_EQ(0, a->Get(1));
  a->Set(1, T(321));
  EXPECT_EQ(T(123), a->Get(0));
  EXPECT_EQ(T(321), a->Get(1));

  Handle<Class> aioobe = hs.NewHandle(
      cl->FindSystemClass(soa.Self(), "Ljava/lang/ArrayIndexOutOfBoundsException;"));

  EXPECT_EQ(0, a->Get(-1));
  EXPECT_TRUE(soa.Self()->IsExceptionPending());
  EXPECT_OBJ_PTR_EQ(aioobe.Get(), soa.Self()->GetException()->GetClass());
  soa.Self()->ClearException();

  EXPECT_EQ(0, a->Get(2));
  EXPECT_TRUE(soa.Self()->IsExceptionPending());
  EXPECT_OBJ_PTR_EQ(aioobe.Get(), soa.Self()->GetException()->GetClass());
  soa.Self()->ClearException();
}

TEST_F(ObjectTest, PrimitiveArray_Boolean_Alloc) {
  TestPrimitiveArray<BooleanArray>(class_linker_);
}
TEST_F(ObjectTest, PrimitiveArray_Byte_Alloc) {
  TestPrimitiveArray<ByteArray>(class_linker_);
}
TEST_F(ObjectTest, PrimitiveArray_Char_Alloc) {
  TestPrimitiveArray<CharArray>(class_linker_);
}
TEST_F(ObjectTest, PrimitiveArray_Int_Alloc) {
  TestPrimitiveArray<IntArray>(class_linker_);
}
TEST_F(ObjectTest, PrimitiveArray_Long_Alloc) {
  TestPrimitiveArray<LongArray>(class_linker_);
}
TEST_F(ObjectTest, PrimitiveArray_Short_Alloc) {
  TestPrimitiveArray<ShortArray>(class_linker_);
}

TEST_F(ObjectTest, PointerArrayWriteRead) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<2> hs(soa.Self());

  Handle<PointerArray> a32 =
      hs.NewHandle(ObjPtr<PointerArray>::DownCast<Array>(IntArray::Alloc(soa.Self(), 1)));
  ASSERT_TRUE(a32 != nullptr);
  ASSERT_EQ(1, a32->GetLength());
  EXPECT_EQ(0u, (a32->GetElementPtrSize<uint32_t, PointerSize::k32>(0u)));
  EXPECT_EQ(0u, (a32->GetElementPtrSizeUnchecked<uint32_t, PointerSize::k32>(0u)));
  for (uint32_t value : { 0u, 1u, 0x7fffffffu, 0x80000000u, 0xffffffffu }) {
    a32->SetElementPtrSize(0u, value, PointerSize::k32);
    EXPECT_EQ(value, (a32->GetElementPtrSize<uint32_t, PointerSize::k32>(0u)));
    EXPECT_EQ(value, (a32->GetElementPtrSizeUnchecked<uint32_t, PointerSize::k32>(0u)));
    // Check that the value matches also when retrieved as `uint64_t`.
    // This is a regression test for unintended sign-extension. b/155780442
    // (Using `uint64_t` rather than `uintptr_t`, so that the 32-bit test checks this too.)
    EXPECT_EQ(value, (a32->GetElementPtrSize<uint64_t, PointerSize::k32>(0u)));
    EXPECT_EQ(value, (a32->GetElementPtrSizeUnchecked<uint64_t, PointerSize::k32>(0u)));
  }

  Handle<PointerArray> a64 =
      hs.NewHandle(ObjPtr<PointerArray>::DownCast<Array>(LongArray::Alloc(soa.Self(), 1)));
  ASSERT_TRUE(a64 != nullptr);
  ASSERT_EQ(1, a64->GetLength());
  EXPECT_EQ(0u, (a64->GetElementPtrSize<uint32_t, PointerSize::k64>(0u)));
  EXPECT_EQ(0u, (a64->GetElementPtrSizeUnchecked<uint32_t, PointerSize::k64>(0u)));
  for (uint64_t value : { UINT64_C(0),
                          UINT64_C(1),
                          UINT64_C(0x7fffffff),
                          UINT64_C(0x80000000),
                          UINT64_C(0xffffffff),
                          UINT64_C(0x100000000),
                          UINT64_C(0x7fffffffffffffff),
                          UINT64_C(0x8000000000000000),
                          UINT64_C(0xffffffffffffffff) }) {
    a64->SetElementPtrSize(0u, value, PointerSize::k64);
    EXPECT_EQ(value, (a64->GetElementPtrSize<uint64_t, PointerSize::k64>(0u)));
    EXPECT_EQ(value, (a64->GetElementPtrSizeUnchecked<uint64_t, PointerSize::k64>(0u)));
  }
}

TEST_F(ObjectTest, PrimitiveArray_Double_Alloc) {
  using ArrayT = DoubleArray;
  ScopedObjectAccess soa(Thread::Current());
  using T = typename ArrayT::ElementType;

  StackHandleScope<2> hs(soa.Self());
  Handle<ArrayT> a = hs.NewHandle(ArrayT::Alloc(soa.Self(), 2));
  EXPECT_EQ(2, a->GetLength());
  EXPECT_DOUBLE_EQ(0, a->Get(0));
  EXPECT_DOUBLE_EQ(0, a->Get(1));
  a->Set(0, T(123));
  EXPECT_DOUBLE_EQ(T(123), a->Get(0));
  EXPECT_DOUBLE_EQ(0, a->Get(1));
  a->Set(1, T(321));
  EXPECT_DOUBLE_EQ(T(123), a->Get(0));
  EXPECT_DOUBLE_EQ(T(321), a->Get(1));

  Handle<Class> aioobe = hs.NewHandle(
      class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/ArrayIndexOutOfBoundsException;"));

  EXPECT_DOUBLE_EQ(0, a->Get(-1));
  EXPECT_TRUE(soa.Self()->IsExceptionPending());
  EXPECT_OBJ_PTR_EQ(aioobe.Get(), soa.Self()->GetException()->GetClass());
  soa.Self()->ClearException();

  EXPECT_DOUBLE_EQ(0, a->Get(2));
  EXPECT_TRUE(soa.Self()->IsExceptionPending());
  EXPECT_OBJ_PTR_EQ(aioobe.Get(), soa.Self()->GetException()->GetClass());
  soa.Self()->ClearException();
}

TEST_F(ObjectTest, PrimitiveArray_Float_Alloc) {
  using ArrayT = FloatArray;
  ScopedObjectAccess soa(Thread::Current());
  using T = typename ArrayT::ElementType;

  StackHandleScope<2> hs(soa.Self());
  Handle<ArrayT> a = hs.NewHandle(ArrayT::Alloc(soa.Self(), 2));
  EXPECT_FLOAT_EQ(2, a->GetLength());
  EXPECT_FLOAT_EQ(0, a->Get(0));
  EXPECT_FLOAT_EQ(0, a->Get(1));
  a->Set(0, T(123));
  EXPECT_FLOAT_EQ(T(123), a->Get(0));
  EXPECT_FLOAT_EQ(0, a->Get(1));
  a->Set(1, T(321));
  EXPECT_FLOAT_EQ(T(123), a->Get(0));
  EXPECT_FLOAT_EQ(T(321), a->Get(1));

  Handle<Class> aioobe = hs.NewHandle(
      class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/ArrayIndexOutOfBoundsException;"));

  EXPECT_FLOAT_EQ(0, a->Get(-1));
  EXPECT_TRUE(soa.Self()->IsExceptionPending());
  EXPECT_OBJ_PTR_EQ(aioobe.Get(), soa.Self()->GetException()->GetClass());
  soa.Self()->ClearException();

  EXPECT_FLOAT_EQ(0, a->Get(2));
  EXPECT_TRUE(soa.Self()->IsExceptionPending());
  EXPECT_OBJ_PTR_EQ(aioobe.Get(), soa.Self()->GetException()->GetClass());
  soa.Self()->ClearException();
}


TEST_F(ObjectTest, CreateMultiArray) {
  ScopedObjectAccess soa(Thread::Current());

  StackHandleScope<4> hs(soa.Self());
  Handle<Class> int_class(hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "I")));
  Handle<Class> int_array_class = hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "[I"));
  MutableHandle<IntArray> dims(hs.NewHandle(IntArray::Alloc(soa.Self(), 1)));
  dims->Set<false>(0, 1);
  MutableHandle<Array> multi = hs.NewHandle(Array::CreateMultiArray(soa.Self(), int_class, dims));
  EXPECT_OBJ_PTR_EQ(int_array_class.Get(), multi->GetClass());
  EXPECT_EQ(1, multi->GetLength());

  dims->Set<false>(0, -1);
  multi.Assign(Array::CreateMultiArray(soa.Self(), int_class, dims));
  EXPECT_TRUE(soa.Self()->IsExceptionPending());
  EXPECT_EQ(mirror::Class::PrettyDescriptor(soa.Self()->GetException()->GetClass()),
            "java.lang.NegativeArraySizeException");
  soa.Self()->ClearException();

  dims.Assign(IntArray::Alloc(soa.Self(), 2));
  for (int i = 1; i < 20; ++i) {
    for (int j = 0; j < 20; ++j) {
      dims->Set<false>(0, i);
      dims->Set<false>(1, j);
      multi.Assign(Array::CreateMultiArray(soa.Self(), int_class, dims));
      ObjPtr<mirror::Class> expected_class = class_linker_->FindSystemClass(soa.Self(), "[[I");
      EXPECT_OBJ_PTR_EQ(multi->GetClass(), expected_class);
      EXPECT_EQ(i, multi->GetLength());
      for (int k = 0; k < i; ++k) {
        ObjPtr<Array> outer = multi->AsObjectArray<Array>()->Get(k);
        EXPECT_OBJ_PTR_EQ(int_array_class.Get(), outer->GetClass());
        EXPECT_EQ(j, outer->GetLength());
      }
    }
  }
}

TEST_F(ObjectTest, StaticFieldFromCode) {
  // pretend we are trying to access 'Static.s0' from StaticsFromCode.<clinit>
  ScopedObjectAccess soa(Thread::Current());
  jobject class_loader = LoadDex("StaticsFromCode");
  const DexFile* dex_file = GetFirstDexFile(class_loader);

  StackHandleScope<3> hs(soa.Self());
  Handle<mirror::ClassLoader> loader(hs.NewHandle(soa.Decode<ClassLoader>(class_loader)));
  Handle<Class> klass = hs.NewHandle(FindClass("LStaticsFromCode;", loader));
  ArtMethod* clinit = klass->FindClassInitializer(kRuntimePointerSize);
  const dex::TypeId* klass_type_id = dex_file->FindTypeId("LStaticsFromCode;");
  ASSERT_TRUE(klass_type_id != nullptr);

  const dex::TypeId* type_type_id = dex_file->FindTypeId("Ljava/lang/Object;");
  ASSERT_TRUE(type_type_id != nullptr);

  const dex::StringId* name_str_id = dex_file->FindStringId("s0");
  ASSERT_TRUE(name_str_id != nullptr);

  const dex::FieldId* field_id = dex_file->FindFieldId(
      *klass_type_id, *name_str_id, *type_type_id);
  ASSERT_TRUE(field_id != nullptr);
  uint32_t field_idx = dex_file->GetIndexForFieldId(*field_id);

  ArtField* field = FindFieldFromCode<StaticObjectRead>(field_idx,
                                                        clinit,
                                                        Thread::Current(),
                                                        sizeof(HeapReference<Object>));
  ObjPtr<Object> s0 = field->GetObj(klass.Get());
  EXPECT_TRUE(s0 != nullptr) << field->PrettyField();

  Handle<CharArray> char_array(hs.NewHandle(CharArray::Alloc(soa.Self(), 0)));
  field->SetObj<false>(field->GetDeclaringClass(), char_array.Get());
  EXPECT_OBJ_PTR_EQ(char_array.Get(), field->GetObj(klass.Get()));

  field->SetObj<false>(field->GetDeclaringClass(), nullptr);
  EXPECT_EQ(nullptr, field->GetObj(klass.Get()));

  // TODO: more exhaustive tests of all 6 cases of ArtField::*FromCode
}

TEST_F(ObjectTest, String) {
  ScopedObjectAccess soa(Thread::Current());
  // Test the empty string.
  AssertString(0, "",     "", 0);

  // Test one-byte characters.
  AssertString(1, " ",    "\x00\x20",         0x20);
  AssertString(1, "",     "\x00\x00",         0);
  AssertString(1, "\x7f", "\x00\x7f",         0x7f);
  AssertString(2, "hi",   "\x00\x68\x00\x69", (31 * 0x68) + 0x69);

  // Test two-byte characters.
  AssertString(1, "\xc2\x80",   "\x00\x80",                 0x80);
  AssertString(1, "\xd9\xa6",   "\x06\x66",                 0x0666);
  AssertString(1, "\xdf\xbf",   "\x07\xff",                 0x07ff);
  AssertString(3, "h\xd9\xa6i", "\x00\x68\x06\x66\x00\x69",
               (31 * ((31 * 0x68) + 0x0666)) + 0x69);

  // Test three-byte characters.
  AssertString(1, "\xe0\xa0\x80",   "\x08\x00",                 0x0800);
  AssertString(1, "\xe1\x88\xb4",   "\x12\x34",                 0x1234);
  AssertString(1, "\xef\xbf\xbf",   "\xff\xff",                 0xffff);
  AssertString(3, "h\xe1\x88\xb4i", "\x00\x68\x12\x34\x00\x69",
               (31 * ((31 * 0x68) + 0x1234)) + 0x69);

  // Test four-byte characters.
  AssertString(2, "\xf0\x9f\x8f\xa0",  "\xd8\x3c\xdf\xe0", (31 * 0xd83c) + 0xdfe0);
  AssertString(2, "\xf0\x9f\x9a\x80",  "\xd8\x3d\xde\x80", (31 * 0xd83d) + 0xde80);
  AssertString(4, "h\xf0\x9f\x9a\x80i", "\x00\x68\xd8\x3d\xde\x80\x00\x69",
               (31 * (31 * (31 * 0x68 +  0xd83d) + 0xde80) + 0x69));
}

TEST_F(ObjectTest, StringEqualsUtf8) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<2> hs(soa.Self());
  Handle<String> string(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "android")));
  EXPECT_TRUE(string->Equals("android"));
  EXPECT_FALSE(string->Equals("Android"));
  EXPECT_FALSE(string->Equals("ANDROID"));
  EXPECT_FALSE(string->Equals(""));
  EXPECT_FALSE(string->Equals("and"));
  EXPECT_FALSE(string->Equals("androids"));

  Handle<String> empty(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "")));
  EXPECT_TRUE(empty->Equals(""));
  EXPECT_FALSE(empty->Equals("a"));
}

TEST_F(ObjectTest, StringEquals) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<3> hs(soa.Self());
  Handle<String> string(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "android")));
  Handle<String> string_2(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "android")));
  EXPECT_TRUE(string->Equals(string_2.Get()));
  EXPECT_FALSE(string->Equals("Android"));
  EXPECT_FALSE(string->Equals("ANDROID"));
  EXPECT_FALSE(string->Equals(""));
  EXPECT_FALSE(string->Equals("and"));
  EXPECT_FALSE(string->Equals("androids"));

  Handle<String> empty(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "")));
  EXPECT_TRUE(empty->Equals(""));
  EXPECT_FALSE(empty->Equals("a"));
}

TEST_F(ObjectTest, StringCompareTo) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<5> hs(soa.Self());
  Handle<String> string(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "android")));
  Handle<String> string_2(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "android")));
  Handle<String> string_3(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "Android")));
  Handle<String> string_4(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "and")));
  Handle<String> string_5(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "")));
  EXPECT_EQ(0, string->CompareTo(string_2.Get()));
  EXPECT_LT(0, string->CompareTo(string_3.Get()));
  EXPECT_GT(0, string_3->CompareTo(string.Get()));
  EXPECT_LT(0, string->CompareTo(string_4.Get()));
  EXPECT_GT(0, string_4->CompareTo(string.Get()));
  EXPECT_LT(0, string->CompareTo(string_5.Get()));
  EXPECT_GT(0, string_5->CompareTo(string.Get()));
}

TEST_F(ObjectTest, StringLength) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<1> hs(soa.Self());
  Handle<String> string(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "android")));
  EXPECT_EQ(string->GetLength(), 7);
  EXPECT_EQ(string->GetModifiedUtf8Length(), 7);
}

TEST_F(ObjectTest, DescriptorCompare) {
  // Two classloaders conflicts in compile_time_class_paths_.
  ScopedObjectAccess soa(Thread::Current());

  jobject jclass_loader_1 = LoadDex("ProtoCompare");
  jobject jclass_loader_2 = LoadDex("ProtoCompare2");
  StackHandleScope<4> hs(soa.Self());
  Handle<ClassLoader> class_loader_1(hs.NewHandle(soa.Decode<ClassLoader>(jclass_loader_1)));
  Handle<ClassLoader> class_loader_2(hs.NewHandle(soa.Decode<ClassLoader>(jclass_loader_2)));

  Handle<Class> klass1 = hs.NewHandle(FindClass("LProtoCompare;", class_loader_1));
  ASSERT_TRUE(klass1 != nullptr);
  Handle<Class> klass2 = hs.NewHandle(FindClass("LProtoCompare2;", class_loader_2));
  ASSERT_TRUE(klass2 != nullptr);

  ArtMethod* m1_1 = klass1->GetVirtualMethod(0, kRuntimePointerSize);
  EXPECT_STREQ(m1_1->GetName(), "m1");
  ArtMethod* m2_1 = klass1->GetVirtualMethod(1, kRuntimePointerSize);
  EXPECT_STREQ(m2_1->GetName(), "m2");
  ArtMethod* m3_1 = klass1->GetVirtualMethod(2, kRuntimePointerSize);
  EXPECT_STREQ(m3_1->GetName(), "m3");
  ArtMethod* m4_1 = klass1->GetVirtualMethod(3, kRuntimePointerSize);
  EXPECT_STREQ(m4_1->GetName(), "m4");

  ArtMethod* m1_2 = klass2->GetVirtualMethod(0, kRuntimePointerSize);
  EXPECT_STREQ(m1_2->GetName(), "m1");
  ArtMethod* m2_2 = klass2->GetVirtualMethod(1, kRuntimePointerSize);
  EXPECT_STREQ(m2_2->GetName(), "m2");
  ArtMethod* m3_2 = klass2->GetVirtualMethod(2, kRuntimePointerSize);
  EXPECT_STREQ(m3_2->GetName(), "m3");
  ArtMethod* m4_2 = klass2->GetVirtualMethod(3, kRuntimePointerSize);
  EXPECT_STREQ(m4_2->GetName(), "m4");
}

TEST_F(ObjectTest, StringHashCode) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<3> hs(soa.Self());
  Handle<String> empty(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "")));
  Handle<String> A(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "A")));
  Handle<String> ABC(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "ABC")));

  EXPECT_EQ(0, empty->GetHashCode());
  EXPECT_EQ(65, A->GetHashCode());
  EXPECT_EQ(64578, ABC->GetHashCode());
}

TEST_F(ObjectTest, InstanceOf) {
  ScopedObjectAccess soa(Thread::Current());
  jobject jclass_loader = LoadDex("XandY");
  StackHandleScope<10> hs(soa.Self());
  Handle<ClassLoader> class_loader(hs.NewHandle(soa.Decode<ClassLoader>(jclass_loader)));

  Handle<Class> X = hs.NewHandle(FindClass("LX;", class_loader));
  Handle<Class> Y = hs.NewHandle(FindClass("LY;", class_loader));
  ASSERT_TRUE(X != nullptr);
  ASSERT_TRUE(Y != nullptr);

  Handle<Object> x(hs.NewHandle(X->AllocObject(soa.Self())));
  Handle<Object> y(hs.NewHandle(Y->AllocObject(soa.Self())));
  ASSERT_TRUE(x != nullptr);
  ASSERT_TRUE(y != nullptr);

  EXPECT_TRUE(x->InstanceOf(X.Get()));
  EXPECT_FALSE(x->InstanceOf(Y.Get()));
  EXPECT_TRUE(y->InstanceOf(X.Get()));
  EXPECT_TRUE(y->InstanceOf(Y.Get()));

  Handle<Class> java_lang_Class =
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Class;"));
  Handle<Class> Object_array_class =
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "[Ljava/lang/Object;"));

  EXPECT_FALSE(java_lang_Class->InstanceOf(Object_array_class.Get()));
  EXPECT_TRUE(Object_array_class->InstanceOf(java_lang_Class.Get()));

  // All array classes implement Cloneable and Serializable.
  Handle<Object> array =
      hs.NewHandle<Object>(ObjectArray<Object>::Alloc(soa.Self(), Object_array_class.Get(), 1));
  Handle<Class> java_lang_Cloneable =
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Cloneable;"));
  Handle<Class> java_io_Serializable =
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "Ljava/io/Serializable;"));
  EXPECT_TRUE(array->InstanceOf(java_lang_Cloneable.Get()));
  EXPECT_TRUE(array->InstanceOf(java_io_Serializable.Get()));
}

TEST_F(ObjectTest, IsAssignableFrom) {
  ScopedObjectAccess soa(Thread::Current());
  jobject jclass_loader = LoadDex("XandY");
  StackHandleScope<5> hs(soa.Self());
  Handle<ClassLoader> class_loader(hs.NewHandle(soa.Decode<ClassLoader>(jclass_loader)));
  Handle<Class> X = hs.NewHandle(FindClass("LX;", class_loader));
  Handle<Class> Y = hs.NewHandle(FindClass("LY;", class_loader));

  EXPECT_TRUE(X->IsAssignableFrom(X.Get()));
  EXPECT_TRUE(X->IsAssignableFrom(Y.Get()));
  EXPECT_FALSE(Y->IsAssignableFrom(X.Get()));
  EXPECT_TRUE(Y->IsAssignableFrom(Y.Get()));

  // class final String implements CharSequence, ..
  Handle<Class> string =
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/String;"));
  Handle<Class> charseq =
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/CharSequence;"));
  // Can String be assigned to CharSequence without a cast?
  EXPECT_TRUE(charseq->IsAssignableFrom(string.Get()));
  // Can CharSequence be assigned to String without a cast?
  EXPECT_FALSE(string->IsAssignableFrom(charseq.Get()));

  // Primitive types are only assignable to themselves
  const char* prims = "ZBCSIJFD";
  std::vector<ObjPtr<Class>> prim_types(strlen(prims));
  for (size_t i = 0; i < strlen(prims); i++) {
    prim_types[i] = class_linker_->FindPrimitiveClass(prims[i]);
  }
  for (size_t i = 0; i < strlen(prims); i++) {
    for (size_t j = 0; i < strlen(prims); i++) {
      if (i == j) {
        EXPECT_TRUE(prim_types[i]->IsAssignableFrom(prim_types[j]));
      } else {
        EXPECT_FALSE(prim_types[i]->IsAssignableFrom(prim_types[j]));
      }
    }
  }
}

TEST_F(ObjectTest, IsAssignableFromArray) {
  ScopedObjectAccess soa(Thread::Current());
  jobject jclass_loader = LoadDex("XandY");
  StackHandleScope<14> hs(soa.Self());
  Handle<ClassLoader> class_loader(hs.NewHandle(soa.Decode<ClassLoader>(jclass_loader)));
  Handle<Class> X = hs.NewHandle(FindClass("LX;", class_loader));
  Handle<Class> Y = hs.NewHandle(FindClass("LY;", class_loader));
  ASSERT_TRUE(X != nullptr);
  ASSERT_TRUE(Y != nullptr);

  Handle<Class> YA = hs.NewHandle(FindClass("[LY;", class_loader));
  Handle<Class> YAA = hs.NewHandle(FindClass("[[LY;", class_loader));
  ASSERT_TRUE(YA != nullptr);
  ASSERT_TRUE(YAA != nullptr);

  Handle<Class> XAA = hs.NewHandle(FindClass("[[LX;", class_loader));
  ASSERT_TRUE(XAA != nullptr);

  Handle<Class> O = hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Object;"));
  Handle<Class> OA =
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "[Ljava/lang/Object;"));
  Handle<Class> OAA =
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "[[Ljava/lang/Object;"));
  Handle<Class> OAAA =
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "[[[Ljava/lang/Object;"));
  ASSERT_TRUE(O != nullptr);
  ASSERT_TRUE(OA != nullptr);
  ASSERT_TRUE(OAA != nullptr);
  ASSERT_TRUE(OAAA != nullptr);

  Handle<Class> S =
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "Ljava/io/Serializable;"));
  Handle<Class> SA =
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "[Ljava/io/Serializable;"));
  Handle<Class> SAA =
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "[[Ljava/io/Serializable;"));
  ASSERT_TRUE(S != nullptr);
  ASSERT_TRUE(SA != nullptr);
  ASSERT_TRUE(SAA != nullptr);

  Handle<Class> IA = hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "[I"));
  ASSERT_TRUE(IA != nullptr);

  EXPECT_TRUE(YAA->IsAssignableFrom(YAA.Get()));  // identity
  EXPECT_TRUE(XAA->IsAssignableFrom(YAA.Get()));  // element superclass
  EXPECT_FALSE(YAA->IsAssignableFrom(XAA.Get()));
  EXPECT_FALSE(Y->IsAssignableFrom(YAA.Get()));
  EXPECT_FALSE(YA->IsAssignableFrom(YAA.Get()));
  EXPECT_TRUE(O->IsAssignableFrom(YAA.Get()));  // everything is an Object
  EXPECT_TRUE(OA->IsAssignableFrom(YAA.Get()));
  EXPECT_TRUE(OAA->IsAssignableFrom(YAA.Get()));
  EXPECT_TRUE(S->IsAssignableFrom(YAA.Get()));  // all arrays are Serializable
  EXPECT_TRUE(SA->IsAssignableFrom(YAA.Get()));
  EXPECT_FALSE(SAA->IsAssignableFrom(YAA.Get()));  // unless Y was Serializable

  EXPECT_FALSE(IA->IsAssignableFrom(OA.Get()));
  EXPECT_FALSE(OA->IsAssignableFrom(IA.Get()));
  EXPECT_TRUE(O->IsAssignableFrom(IA.Get()));
}

TEST_F(ObjectTest, FindInstanceField) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<1> hs(soa.Self());
  Handle<String> s(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "ABC")));
  ASSERT_TRUE(s != nullptr);
  ObjPtr<Class> c = s->GetClass();
  ASSERT_TRUE(c != nullptr);

  // Wrong type.
  EXPECT_TRUE(c->FindDeclaredInstanceField("count", "J") == nullptr);
  EXPECT_TRUE(c->FindInstanceField("count", "J") == nullptr);

  // Wrong name.
  EXPECT_TRUE(c->FindDeclaredInstanceField("Count", "I") == nullptr);
  EXPECT_TRUE(c->FindInstanceField("Count", "I") == nullptr);

  // Right name and type.
  ArtField* f1 = c->FindDeclaredInstanceField("count", "I");
  ArtField* f2 = c->FindInstanceField("count", "I");
  EXPECT_TRUE(f1 != nullptr);
  EXPECT_TRUE(f2 != nullptr);
  EXPECT_EQ(f1, f2);

  // TODO: check that s.count == 3.

  // Ensure that we handle superclass fields correctly...
  c = class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/StringBuilder;");
  ASSERT_TRUE(c != nullptr);
  // No StringBuilder.count...
  EXPECT_TRUE(c->FindDeclaredInstanceField("count", "I") == nullptr);
  // ...but there is an AbstractStringBuilder.count.
  EXPECT_TRUE(c->FindInstanceField("count", "I") != nullptr);
}

TEST_F(ObjectTest, FindStaticField) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<4> hs(soa.Self());
  Handle<String> s(hs.NewHandle(String::AllocFromModifiedUtf8(soa.Self(), "ABC")));
  ASSERT_TRUE(s != nullptr);
  Handle<Class> c(hs.NewHandle(s->GetClass()));
  ASSERT_TRUE(c != nullptr);

  // Wrong type.
  EXPECT_TRUE(c->FindDeclaredStaticField("CASE_INSENSITIVE_ORDER", "I") == nullptr);
  EXPECT_TRUE(c->FindStaticField("CASE_INSENSITIVE_ORDER", "I") == nullptr);

  // Wrong name.
  EXPECT_TRUE(c->FindDeclaredStaticField(
      "cASE_INSENSITIVE_ORDER", "Ljava/util/Comparator;") == nullptr);
  EXPECT_TRUE(c->FindStaticField("cASE_INSENSITIVE_ORDER", "Ljava/util/Comparator;") == nullptr);

  // Right name and type.
  ArtField* f1 = c->FindDeclaredStaticField("CASE_INSENSITIVE_ORDER", "Ljava/util/Comparator;");
  ArtField* f2 = c->FindStaticField("CASE_INSENSITIVE_ORDER", "Ljava/util/Comparator;");
  EXPECT_TRUE(f1 != nullptr);
  EXPECT_TRUE(f2 != nullptr);
  EXPECT_EQ(f1, f2);

  // TODO: test static fields via superclasses.
  // TODO: test static fields via interfaces.
  // TODO: test that interfaces trump superclasses.
}

TEST_F(ObjectTest, IdentityHashCode) {
  // Regression test for b/19046417 which had an infinite loop if the
  // (seed & LockWord::kHashMask) == 0. seed 0 triggered the infinite loop since we did the check
  // before the CAS which resulted in the same seed the next loop iteration.
  mirror::Object::SetHashCodeSeed(0);
  int32_t hash_code = mirror::Object::GenerateIdentityHashCode();
  EXPECT_NE(hash_code, 0);
}

TEST_F(ObjectTest, ObjectPointer) {
  ScopedObjectAccess soa(Thread::Current());
  jobject jclass_loader = LoadDex("XandY");
  StackHandleScope<2> hs(soa.Self());
  Handle<ClassLoader> class_loader(hs.NewHandle(soa.Decode<ClassLoader>(jclass_loader)));
  Handle<mirror::Class> h_X = hs.NewHandle(FindClass("LX;", class_loader));

  if (kObjPtrPoisoning) {
    ObjPtr<mirror::Object> null_ptr;
    EXPECT_TRUE(null_ptr.IsNull());
    EXPECT_TRUE(null_ptr.IsValid());
    EXPECT_TRUE(null_ptr.Ptr() == nullptr);
    EXPECT_TRUE(null_ptr == nullptr);
    EXPECT_TRUE(null_ptr == null_ptr);
    EXPECT_FALSE(null_ptr != null_ptr);
    EXPECT_FALSE(null_ptr != nullptr);
    null_ptr.AssertValid();
    ObjPtr<Class> X(h_X.Get());
    EXPECT_TRUE(!X.IsNull());
    EXPECT_TRUE(X.IsValid());
    EXPECT_TRUE(X.Ptr() != nullptr);
    EXPECT_OBJ_PTR_EQ(h_X.Get(), X);
    // FindClass may cause thread suspension, it should invalidate X.
    ObjPtr<Class> Y = FindClass("LY;", class_loader);
    EXPECT_TRUE(!Y.IsNull());
    EXPECT_TRUE(Y.IsValid());
    EXPECT_TRUE(Y.Ptr() != nullptr);

    // Should IsNull be safe to call on null ObjPtr? I'll allow it for now.
    EXPECT_TRUE(!X.IsNull());
    EXPECT_TRUE(!X.IsValid());
    // Make X valid again by copying out of handle.
    X.Assign(h_X.Get());
    EXPECT_TRUE(!X.IsNull());
    EXPECT_TRUE(X.IsValid());
    EXPECT_OBJ_PTR_EQ(h_X.Get(), X);

    // Allow thread suspension to invalidate Y.
    soa.Self()->AllowThreadSuspension();
    EXPECT_TRUE(!Y.IsNull());
    EXPECT_TRUE(!Y.IsValid());
  } else {
    // Test unpoisoned.
    ObjPtr<mirror::Object> unpoisoned;
    EXPECT_TRUE(unpoisoned.IsNull());
    EXPECT_TRUE(unpoisoned.IsValid());
    EXPECT_TRUE(unpoisoned.Ptr() == nullptr);
    EXPECT_TRUE(unpoisoned == nullptr);
    EXPECT_TRUE(unpoisoned == unpoisoned);
    EXPECT_FALSE(unpoisoned != unpoisoned);
    EXPECT_FALSE(unpoisoned != nullptr);

    unpoisoned = h_X.Get();
    EXPECT_FALSE(unpoisoned.IsNull());
    EXPECT_TRUE(unpoisoned == h_X.Get());
    EXPECT_OBJ_PTR_EQ(unpoisoned, h_X.Get());
  }
}

TEST_F(ObjectTest, PrettyTypeOf) {
  ScopedObjectAccess soa(Thread::Current());
  EXPECT_EQ("null", mirror::Object::PrettyTypeOf(nullptr));

  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::String> s(hs.NewHandle(mirror::String::AllocFromModifiedUtf8(soa.Self(), "")));
  EXPECT_EQ("java.lang.String", mirror::Object::PrettyTypeOf(s.Get()));

  Handle<mirror::ShortArray> a(hs.NewHandle(mirror::ShortArray::Alloc(soa.Self(), 2)));
  EXPECT_EQ("short[]", mirror::Object::PrettyTypeOf(a.Get()));

  ObjPtr<mirror::Class> c = class_linker_->FindSystemClass(soa.Self(), "[Ljava/lang/String;");
  ASSERT_TRUE(c != nullptr);
  ObjPtr<mirror::Object> o = mirror::ObjectArray<mirror::String>::Alloc(soa.Self(), c, 0);
  EXPECT_EQ("java.lang.String[]", mirror::Object::PrettyTypeOf(o));
  EXPECT_EQ("java.lang.Class<java.lang.String[]>", mirror::Object::PrettyTypeOf(o->GetClass()));
}

}  // namespace mirror
}  // namespace art
