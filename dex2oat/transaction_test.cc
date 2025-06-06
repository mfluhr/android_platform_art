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

#include "transaction.h"

#include "art_field-inl.h"
#include "art_method-inl.h"
#include "class_linker-inl.h"
#include "common_transaction_test.h"
#include "common_throws.h"
#include "dex/dex_file.h"
#include "mirror/array-alloc-inl.h"
#include "mirror/class-alloc-inl.h"
#include "scoped_thread_state_change-inl.h"

namespace art HIDDEN {

class TransactionTest : public CommonTransactionTest {
 protected:
  TransactionTest() {
    this->use_boot_image_ = true;  // We need the boot image for this test.
  }

  // Tests failing class initialization due to native call with transaction rollback.
  void testTransactionAbort(const char* tested_class_signature) {
    ScopedObjectAccess soa(Thread::Current());
    jobject jclass_loader = LoadDex("Transaction");
    StackHandleScope<2> hs(soa.Self());
    Handle<mirror::ClassLoader> class_loader(
        hs.NewHandle(soa.Decode<mirror::ClassLoader>(jclass_loader)));
    ASSERT_TRUE(class_loader != nullptr);

    // Load and initialize java.lang.ExceptionInInitializerError and the exception class used
    // to abort transaction so they can be thrown during class initialization if the transaction
    // aborts.
    MutableHandle<mirror::Class> h_klass(
        hs.NewHandle(class_linker_->FindSystemClass(soa.Self(),
                                                    "Ljava/lang/ExceptionInInitializerError;")));
    ASSERT_TRUE(h_klass != nullptr);
    class_linker_->EnsureInitialized(soa.Self(), h_klass, true, true);
    ASSERT_TRUE(h_klass->IsInitialized());

    h_klass.Assign(class_linker_->FindSystemClass(soa.Self(), kTransactionAbortErrorDescriptor));
    ASSERT_TRUE(h_klass != nullptr);
    class_linker_->EnsureInitialized(soa.Self(), h_klass, true, true);
    ASSERT_TRUE(h_klass->IsInitialized());

    // Load and verify utility class.
    h_klass.Assign(FindClass("LTransaction$AbortHelperClass;", class_loader));
    ASSERT_TRUE(h_klass != nullptr);
    class_linker_->VerifyClass(soa.Self(), /* verifier_deps= */ nullptr, h_klass);
    ASSERT_TRUE(h_klass->IsVerified());

    // Load and verify tested class.
    h_klass.Assign(FindClass(tested_class_signature, class_loader));
    ASSERT_TRUE(h_klass != nullptr);
    class_linker_->VerifyClass(soa.Self(), /* verifier_deps= */ nullptr, h_klass);
    ASSERT_TRUE(h_klass->IsVerified());

    ClassStatus old_status = h_klass->GetStatus();
    LockWord old_lock_word = h_klass->GetLockWord(false);

    EnterTransactionMode();
    bool success = class_linker_->EnsureInitialized(soa.Self(), h_klass, true, true);
    ASSERT_TRUE(IsTransactionAborted());
    ASSERT_FALSE(success);
    ASSERT_TRUE(h_klass->IsErroneous());
    ASSERT_TRUE(soa.Self()->IsExceptionPending());

    // Check class's monitor get back to its original state without rolling back changes.
    LockWord new_lock_word = h_klass->GetLockWord(false);
    EXPECT_TRUE(LockWord::Equal<false>(old_lock_word, new_lock_word));

    // Check class status is rolled back properly.
    soa.Self()->ClearException();
    RollbackAndExitTransactionMode();
    ASSERT_EQ(old_status, h_klass->GetStatus());
  }
};

// Tests object's class is preserved after transaction rollback.
TEST_F(TransactionTest, Object_class) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::Class> h_klass(
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Object;")));
  ASSERT_TRUE(h_klass != nullptr);

  EnterTransactionMode();
  Handle<mirror::Object> h_obj(hs.NewHandle(h_klass->AllocObject(soa.Self())));
  ASSERT_TRUE(h_obj != nullptr);
  ASSERT_OBJ_PTR_EQ(h_obj->GetClass(), h_klass.Get());
  // Rolling back transaction's changes must not clear the Object::class field.
  RollbackAndExitTransactionMode();
  EXPECT_OBJ_PTR_EQ(h_obj->GetClass(), h_klass.Get());
}

// Tests object's monitor state is preserved after transaction rollback.
TEST_F(TransactionTest, Object_monitor) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::Class> h_klass(
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Object;")));
  ASSERT_TRUE(h_klass != nullptr);
  Handle<mirror::Object> h_obj(hs.NewHandle(h_klass->AllocObject(soa.Self())));
  ASSERT_TRUE(h_obj != nullptr);
  ASSERT_OBJ_PTR_EQ(h_obj->GetClass(), h_klass.Get());

  // Lock object's monitor outside the transaction.
  h_obj->MonitorEnter(soa.Self());
  LockWord old_lock_word = h_obj->GetLockWord(false);

  EnterTransactionMode();
  // Unlock object's monitor inside the transaction.
  h_obj->MonitorExit(soa.Self());
  LockWord new_lock_word = h_obj->GetLockWord(false);
  // Rolling back transaction's changes must not change monitor's state.
  RollbackAndExitTransactionMode();

  LockWord aborted_lock_word = h_obj->GetLockWord(false);
  EXPECT_FALSE(LockWord::Equal<false>(old_lock_word, new_lock_word));
  EXPECT_TRUE(LockWord::Equal<false>(aborted_lock_word, new_lock_word));
}

// Tests array's length is preserved after transaction rollback.
TEST_F(TransactionTest, Array_length) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::Class> h_klass(
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "[Ljava/lang/Object;")));
  ASSERT_TRUE(h_klass != nullptr);

  constexpr int32_t kArraySize = 2;

  EnterTransactionMode();

  // Allocate an array during transaction.
  Handle<mirror::Array> h_obj = hs.NewHandle(
      mirror::Array::Alloc(soa.Self(),
                           h_klass.Get(),
                           kArraySize,
                           h_klass->GetComponentSizeShift(),
                           Runtime::Current()->GetHeap()->GetCurrentAllocator()));
  ASSERT_TRUE(h_obj != nullptr);
  ASSERT_OBJ_PTR_EQ(h_obj->GetClass(), h_klass.Get());
  RollbackAndExitTransactionMode();

  // Rolling back transaction's changes must not reset array's length.
  EXPECT_EQ(h_obj->GetLength(), kArraySize);
}

// Tests static fields are reset to their default value after transaction rollback.
TEST_F(TransactionTest, StaticFieldsTest) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<4> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader>(LoadDex("Transaction"))));
  ASSERT_TRUE(class_loader != nullptr);

  Handle<mirror::Class> h_klass = hs.NewHandle(FindClass("LStaticFieldsTest;", class_loader));
  ASSERT_TRUE(h_klass != nullptr);
  bool success = class_linker_->EnsureInitialized(soa.Self(), h_klass, true, true);
  ASSERT_TRUE(success);
  ASSERT_TRUE(h_klass->IsInitialized());
  ASSERT_FALSE(soa.Self()->IsExceptionPending());

  // Lookup fields.
  ArtField* booleanField = h_klass->FindDeclaredStaticField("booleanField", "Z");
  ASSERT_TRUE(booleanField != nullptr);
  ASSERT_EQ(booleanField->GetTypeAsPrimitiveType(), Primitive::kPrimBoolean);
  ASSERT_EQ(booleanField->GetBoolean(h_klass.Get()), false);

  ArtField* byteField = h_klass->FindDeclaredStaticField("byteField", "B");
  ASSERT_TRUE(byteField != nullptr);
  ASSERT_EQ(byteField->GetTypeAsPrimitiveType(), Primitive::kPrimByte);
  ASSERT_EQ(byteField->GetByte(h_klass.Get()), 0);

  ArtField* charField = h_klass->FindDeclaredStaticField("charField", "C");
  ASSERT_TRUE(charField != nullptr);
  ASSERT_EQ(charField->GetTypeAsPrimitiveType(), Primitive::kPrimChar);
  ASSERT_EQ(charField->GetChar(h_klass.Get()), 0u);

  ArtField* shortField = h_klass->FindDeclaredStaticField("shortField", "S");
  ASSERT_TRUE(shortField != nullptr);
  ASSERT_EQ(shortField->GetTypeAsPrimitiveType(), Primitive::kPrimShort);
  ASSERT_EQ(shortField->GetShort(h_klass.Get()), 0);

  ArtField* intField = h_klass->FindDeclaredStaticField("intField", "I");
  ASSERT_TRUE(intField != nullptr);
  ASSERT_EQ(intField->GetTypeAsPrimitiveType(), Primitive::kPrimInt);
  ASSERT_EQ(intField->GetInt(h_klass.Get()), 0);

  ArtField* longField = h_klass->FindDeclaredStaticField("longField", "J");
  ASSERT_TRUE(longField != nullptr);
  ASSERT_EQ(longField->GetTypeAsPrimitiveType(), Primitive::kPrimLong);
  ASSERT_EQ(longField->GetLong(h_klass.Get()), static_cast<int64_t>(0));

  ArtField* floatField = h_klass->FindDeclaredStaticField("floatField", "F");
  ASSERT_TRUE(floatField != nullptr);
  ASSERT_EQ(floatField->GetTypeAsPrimitiveType(), Primitive::kPrimFloat);
  ASSERT_FLOAT_EQ(floatField->GetFloat(h_klass.Get()), static_cast<float>(0.0f));

  ArtField* doubleField = h_klass->FindDeclaredStaticField("doubleField", "D");
  ASSERT_TRUE(doubleField != nullptr);
  ASSERT_EQ(doubleField->GetTypeAsPrimitiveType(), Primitive::kPrimDouble);
  ASSERT_DOUBLE_EQ(doubleField->GetDouble(h_klass.Get()), static_cast<double>(0.0));

  ArtField* objectField = h_klass->FindDeclaredStaticField("objectField",
                                                                   "Ljava/lang/Object;");
  ASSERT_TRUE(objectField != nullptr);
  ASSERT_EQ(objectField->GetTypeAsPrimitiveType(), Primitive::kPrimNot);
  ASSERT_EQ(objectField->GetObject(h_klass.Get()), nullptr);

  // Create a java.lang.Object instance to set objectField.
  Handle<mirror::Class> object_klass(
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Object;")));
  ASSERT_TRUE(object_klass != nullptr);
  Handle<mirror::Object> h_obj(hs.NewHandle(h_klass->AllocObject(soa.Self())));
  ASSERT_TRUE(h_obj != nullptr);
  ASSERT_OBJ_PTR_EQ(h_obj->GetClass(), h_klass.Get());

  // Modify fields inside transaction then rollback changes.
  EnterTransactionMode();
  booleanField->SetBoolean<true>(h_klass.Get(), true);
  byteField->SetByte<true>(h_klass.Get(), 1);
  charField->SetChar<true>(h_klass.Get(), 1u);
  shortField->SetShort<true>(h_klass.Get(), 1);
  intField->SetInt<true>(h_klass.Get(), 1);
  longField->SetLong<true>(h_klass.Get(), 1);
  floatField->SetFloat<true>(h_klass.Get(), 1.0);
  doubleField->SetDouble<true>(h_klass.Get(), 1.0);
  objectField->SetObject<true>(h_klass.Get(), h_obj.Get());
  RollbackAndExitTransactionMode();

  // Check values have properly been restored to their original (default) value.
  EXPECT_EQ(booleanField->GetBoolean(h_klass.Get()), false);
  EXPECT_EQ(byteField->GetByte(h_klass.Get()), 0);
  EXPECT_EQ(charField->GetChar(h_klass.Get()), 0u);
  EXPECT_EQ(shortField->GetShort(h_klass.Get()), 0);
  EXPECT_EQ(intField->GetInt(h_klass.Get()), 0);
  EXPECT_EQ(longField->GetLong(h_klass.Get()), static_cast<int64_t>(0));
  EXPECT_FLOAT_EQ(floatField->GetFloat(h_klass.Get()), static_cast<float>(0.0f));
  EXPECT_DOUBLE_EQ(doubleField->GetDouble(h_klass.Get()), static_cast<double>(0.0));
  EXPECT_EQ(objectField->GetObject(h_klass.Get()), nullptr);
}

// Tests instance fields are reset to their default value after transaction rollback.
TEST_F(TransactionTest, InstanceFieldsTest) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<5> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader>(LoadDex("Transaction"))));
  ASSERT_TRUE(class_loader != nullptr);

  Handle<mirror::Class> h_klass = hs.NewHandle(FindClass("LInstanceFieldsTest;", class_loader));
  ASSERT_TRUE(h_klass != nullptr);
  bool success = class_linker_->EnsureInitialized(soa.Self(), h_klass, true, true);
  ASSERT_TRUE(success);
  ASSERT_TRUE(h_klass->IsInitialized());
  ASSERT_FALSE(soa.Self()->IsExceptionPending());

  // Allocate an InstanceFieldTest object.
  Handle<mirror::Object> h_instance(hs.NewHandle(h_klass->AllocObject(soa.Self())));
  ASSERT_TRUE(h_instance != nullptr);

  // Lookup fields.
  ArtField* booleanField = h_klass->FindDeclaredInstanceField("booleanField", "Z");
  ASSERT_TRUE(booleanField != nullptr);
  ASSERT_EQ(booleanField->GetTypeAsPrimitiveType(), Primitive::kPrimBoolean);
  ASSERT_EQ(booleanField->GetBoolean(h_instance.Get()), false);

  ArtField* byteField = h_klass->FindDeclaredInstanceField("byteField", "B");
  ASSERT_TRUE(byteField != nullptr);
  ASSERT_EQ(byteField->GetTypeAsPrimitiveType(), Primitive::kPrimByte);
  ASSERT_EQ(byteField->GetByte(h_instance.Get()), 0);

  ArtField* charField = h_klass->FindDeclaredInstanceField("charField", "C");
  ASSERT_TRUE(charField != nullptr);
  ASSERT_EQ(charField->GetTypeAsPrimitiveType(), Primitive::kPrimChar);
  ASSERT_EQ(charField->GetChar(h_instance.Get()), 0u);

  ArtField* shortField = h_klass->FindDeclaredInstanceField("shortField", "S");
  ASSERT_TRUE(shortField != nullptr);
  ASSERT_EQ(shortField->GetTypeAsPrimitiveType(), Primitive::kPrimShort);
  ASSERT_EQ(shortField->GetShort(h_instance.Get()), 0);

  ArtField* intField = h_klass->FindDeclaredInstanceField("intField", "I");
  ASSERT_TRUE(intField != nullptr);
  ASSERT_EQ(intField->GetTypeAsPrimitiveType(), Primitive::kPrimInt);
  ASSERT_EQ(intField->GetInt(h_instance.Get()), 0);

  ArtField* longField = h_klass->FindDeclaredInstanceField("longField", "J");
  ASSERT_TRUE(longField != nullptr);
  ASSERT_EQ(longField->GetTypeAsPrimitiveType(), Primitive::kPrimLong);
  ASSERT_EQ(longField->GetLong(h_instance.Get()), static_cast<int64_t>(0));

  ArtField* floatField = h_klass->FindDeclaredInstanceField("floatField", "F");
  ASSERT_TRUE(floatField != nullptr);
  ASSERT_EQ(floatField->GetTypeAsPrimitiveType(), Primitive::kPrimFloat);
  ASSERT_FLOAT_EQ(floatField->GetFloat(h_instance.Get()), static_cast<float>(0.0f));

  ArtField* doubleField = h_klass->FindDeclaredInstanceField("doubleField", "D");
  ASSERT_TRUE(doubleField != nullptr);
  ASSERT_EQ(doubleField->GetTypeAsPrimitiveType(), Primitive::kPrimDouble);
  ASSERT_DOUBLE_EQ(doubleField->GetDouble(h_instance.Get()), static_cast<double>(0.0));

  ArtField* objectField = h_klass->FindDeclaredInstanceField("objectField",
                                                                        "Ljava/lang/Object;");
  ASSERT_TRUE(objectField != nullptr);
  ASSERT_EQ(objectField->GetTypeAsPrimitiveType(), Primitive::kPrimNot);
  ASSERT_EQ(objectField->GetObject(h_instance.Get()), nullptr);

  // Create a java.lang.Object instance to set objectField.
  Handle<mirror::Class> object_klass(
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Object;")));
  ASSERT_TRUE(object_klass != nullptr);
  Handle<mirror::Object> h_obj(hs.NewHandle(h_klass->AllocObject(soa.Self())));
  ASSERT_TRUE(h_obj != nullptr);
  ASSERT_OBJ_PTR_EQ(h_obj->GetClass(), h_klass.Get());

  // Modify fields inside transaction then rollback changes.
  EnterTransactionMode();
  booleanField->SetBoolean<true>(h_instance.Get(), true);
  byteField->SetByte<true>(h_instance.Get(), 1);
  charField->SetChar<true>(h_instance.Get(), 1u);
  shortField->SetShort<true>(h_instance.Get(), 1);
  intField->SetInt<true>(h_instance.Get(), 1);
  longField->SetLong<true>(h_instance.Get(), 1);
  floatField->SetFloat<true>(h_instance.Get(), 1.0);
  doubleField->SetDouble<true>(h_instance.Get(), 1.0);
  objectField->SetObject<true>(h_instance.Get(), h_obj.Get());
  RollbackAndExitTransactionMode();

  // Check values have properly been restored to their original (default) value.
  EXPECT_EQ(booleanField->GetBoolean(h_instance.Get()), false);
  EXPECT_EQ(byteField->GetByte(h_instance.Get()), 0);
  EXPECT_EQ(charField->GetChar(h_instance.Get()), 0u);
  EXPECT_EQ(shortField->GetShort(h_instance.Get()), 0);
  EXPECT_EQ(intField->GetInt(h_instance.Get()), 0);
  EXPECT_EQ(longField->GetLong(h_instance.Get()), static_cast<int64_t>(0));
  EXPECT_FLOAT_EQ(floatField->GetFloat(h_instance.Get()), static_cast<float>(0.0f));
  EXPECT_DOUBLE_EQ(doubleField->GetDouble(h_instance.Get()), static_cast<double>(0.0));
  EXPECT_EQ(objectField->GetObject(h_instance.Get()), nullptr);

  // Fail to modify fields with strong CAS inside transaction, then rollback changes.
  EnterTransactionMode();
  bool cas_success = h_instance->CasField32</*kTransactionActive=*/ true>(
      intField->GetOffset(),
      /*old_value=*/ 1,
      /*new_value=*/ 2,
      CASMode::kStrong,
      std::memory_order_seq_cst);
  EXPECT_FALSE(cas_success);
  cas_success = h_instance->CasFieldStrongSequentiallyConsistent64</*kTransactionActive=*/ true>(
      longField->GetOffset(), /*old_value=*/ INT64_C(1), /*new_value=*/ INT64_C(2));
  EXPECT_FALSE(cas_success);
  cas_success = h_instance->CasFieldObject</*kTransactionActive=*/ true>(
      objectField->GetOffset(),
      /*old_value=*/ h_instance.Get(),
      /*new_value=*/ nullptr,
      CASMode::kStrong,
      std::memory_order_seq_cst);
  EXPECT_FALSE(cas_success);
  RollbackAndExitTransactionMode();

  // Check values have properly been restored to their original (default) value.
  EXPECT_EQ(intField->GetInt(h_instance.Get()), 0);
  EXPECT_EQ(longField->GetLong(h_instance.Get()), static_cast<int64_t>(0));
  EXPECT_EQ(objectField->GetObject(h_instance.Get()), nullptr);

  // Fail to modify fields with weak CAS inside transaction, then rollback changes.
  EnterTransactionMode();
  cas_success = h_instance->CasField32</*kTransactionActive=*/ true>(
      intField->GetOffset(),
      /*old_value=*/ 3,
      /*new_value=*/ 4,
      CASMode::kWeak,
      std::memory_order_seq_cst);
  EXPECT_FALSE(cas_success);
  cas_success = h_instance->CasFieldWeakSequentiallyConsistent64</*kTransactionActive=*/ true>(
      longField->GetOffset(), /*old_value=*/ INT64_C(3), /*new_value=*/ INT64_C(4));
  EXPECT_FALSE(cas_success);
  cas_success = h_instance->CasFieldObject</*kTransactionActive=*/ true>(
      objectField->GetOffset(),
      /*old_value=*/ h_klass.Get(),
      /*new_value=*/ nullptr,
      CASMode::kWeak,
      std::memory_order_seq_cst);
  EXPECT_FALSE(cas_success);
  RollbackAndExitTransactionMode();

  // Check values have properly been restored to their original (default) value.
  EXPECT_EQ(intField->GetInt(h_instance.Get()), 0);
  EXPECT_EQ(longField->GetLong(h_instance.Get()), static_cast<int64_t>(0));
  EXPECT_EQ(objectField->GetObject(h_instance.Get()), nullptr);
}

// Tests static array fields are reset to their default value after transaction rollback.
TEST_F(TransactionTest, StaticArrayFieldsTest) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<13> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader>(LoadDex("Transaction"))));
  ASSERT_TRUE(class_loader != nullptr);

  Handle<mirror::Class> h_klass = hs.NewHandle(FindClass("LStaticArrayFieldsTest;", class_loader));
  ASSERT_TRUE(h_klass != nullptr);
  bool success = class_linker_->EnsureInitialized(soa.Self(), h_klass, true, true);
  ASSERT_TRUE(success);
  ASSERT_TRUE(h_klass->IsInitialized());
  ASSERT_FALSE(soa.Self()->IsExceptionPending());

  // Lookup fields.
  ArtField* booleanArrayField = h_klass->FindDeclaredStaticField("booleanArrayField", "[Z");
  ASSERT_TRUE(booleanArrayField != nullptr);
  Handle<mirror::BooleanArray> booleanArray = hs.NewHandle(
      booleanArrayField->GetObject(h_klass.Get())->AsBooleanArray());
  ASSERT_TRUE(booleanArray != nullptr);
  ASSERT_EQ(booleanArray->GetLength(), 1);
  ASSERT_EQ(booleanArray->GetWithoutChecks(0), false);

  ArtField* byteArrayField = h_klass->FindDeclaredStaticField("byteArrayField", "[B");
  ASSERT_TRUE(byteArrayField != nullptr);
  Handle<mirror::ByteArray> byteArray =
      hs.NewHandle(byteArrayField->GetObject(h_klass.Get())->AsByteArray());
  ASSERT_TRUE(byteArray != nullptr);
  ASSERT_EQ(byteArray->GetLength(), 1);
  ASSERT_EQ(byteArray->GetWithoutChecks(0), 0);

  ArtField* charArrayField = h_klass->FindDeclaredStaticField("charArrayField", "[C");
  ASSERT_TRUE(charArrayField != nullptr);
  Handle<mirror::CharArray> charArray =
      hs.NewHandle(charArrayField->GetObject(h_klass.Get())->AsCharArray());
  ASSERT_TRUE(charArray != nullptr);
  ASSERT_EQ(charArray->GetLength(), 1);
  ASSERT_EQ(charArray->GetWithoutChecks(0), 0u);

  ArtField* shortArrayField = h_klass->FindDeclaredStaticField("shortArrayField", "[S");
  ASSERT_TRUE(shortArrayField != nullptr);
  Handle<mirror::ShortArray> shortArray =
      hs.NewHandle(shortArrayField->GetObject(h_klass.Get())->AsShortArray());
  ASSERT_TRUE(shortArray != nullptr);
  ASSERT_EQ(shortArray->GetLength(), 1);
  ASSERT_EQ(shortArray->GetWithoutChecks(0), 0);

  ArtField* intArrayField = h_klass->FindDeclaredStaticField("intArrayField", "[I");
  ASSERT_TRUE(intArrayField != nullptr);
  Handle<mirror::IntArray> intArray =
      hs.NewHandle(intArrayField->GetObject(h_klass.Get())->AsIntArray());
  ASSERT_TRUE(intArray != nullptr);
  ASSERT_EQ(intArray->GetLength(), 1);
  ASSERT_EQ(intArray->GetWithoutChecks(0), 0);

  ArtField* longArrayField = h_klass->FindDeclaredStaticField("longArrayField", "[J");
  ASSERT_TRUE(longArrayField != nullptr);
  Handle<mirror::LongArray> longArray =
      hs.NewHandle(longArrayField->GetObject(h_klass.Get())->AsLongArray());
  ASSERT_TRUE(longArray != nullptr);
  ASSERT_EQ(longArray->GetLength(), 1);
  ASSERT_EQ(longArray->GetWithoutChecks(0), static_cast<int64_t>(0));

  ArtField* floatArrayField = h_klass->FindDeclaredStaticField("floatArrayField", "[F");
  ASSERT_TRUE(floatArrayField != nullptr);
  Handle<mirror::FloatArray> floatArray =
      hs.NewHandle(floatArrayField->GetObject(h_klass.Get())->AsFloatArray());
  ASSERT_TRUE(floatArray != nullptr);
  ASSERT_EQ(floatArray->GetLength(), 1);
  ASSERT_FLOAT_EQ(floatArray->GetWithoutChecks(0), static_cast<float>(0.0f));

  ArtField* doubleArrayField = h_klass->FindDeclaredStaticField("doubleArrayField", "[D");
  ASSERT_TRUE(doubleArrayField != nullptr);
  Handle<mirror::DoubleArray> doubleArray =
      hs.NewHandle(doubleArrayField->GetObject(h_klass.Get())->AsDoubleArray());
  ASSERT_TRUE(doubleArray != nullptr);
  ASSERT_EQ(doubleArray->GetLength(), 1);
  ASSERT_DOUBLE_EQ(doubleArray->GetWithoutChecks(0), static_cast<double>(0.0f));

  ArtField* objectArrayField =
      h_klass->FindDeclaredStaticField("objectArrayField", "[Ljava/lang/Object;");
  ASSERT_TRUE(objectArrayField != nullptr);
  Handle<mirror::ObjectArray<mirror::Object>> objectArray =
      hs.NewHandle(objectArrayField->GetObject(h_klass.Get())->AsObjectArray<mirror::Object>());
  ASSERT_TRUE(objectArray != nullptr);
  ASSERT_EQ(objectArray->GetLength(), 1);
  ASSERT_EQ(objectArray->GetWithoutChecks(0), nullptr);

  // Create a java.lang.Object instance to set objectField.
  Handle<mirror::Class> object_klass(
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Object;")));
  ASSERT_TRUE(object_klass != nullptr);
  Handle<mirror::Object> h_obj(hs.NewHandle(h_klass->AllocObject(soa.Self())));
  ASSERT_TRUE(h_obj != nullptr);
  ASSERT_OBJ_PTR_EQ(h_obj->GetClass(), h_klass.Get());

  // Modify fields inside transaction then rollback changes.
  EnterTransactionMode();
  booleanArray->SetWithoutChecks<true>(0, true);
  byteArray->SetWithoutChecks<true>(0, 1);
  charArray->SetWithoutChecks<true>(0, 1u);
  shortArray->SetWithoutChecks<true>(0, 1);
  intArray->SetWithoutChecks<true>(0, 1);
  longArray->SetWithoutChecks<true>(0, 1);
  floatArray->SetWithoutChecks<true>(0, 1.0);
  doubleArray->SetWithoutChecks<true>(0, 1.0);
  objectArray->SetWithoutChecks<true>(0, h_obj.Get());
  RollbackAndExitTransactionMode();

  // Check values have properly been restored to their original (default) value.
  EXPECT_EQ(booleanArray->GetWithoutChecks(0), false);
  EXPECT_EQ(byteArray->GetWithoutChecks(0), 0);
  EXPECT_EQ(charArray->GetWithoutChecks(0), 0u);
  EXPECT_EQ(shortArray->GetWithoutChecks(0), 0);
  EXPECT_EQ(intArray->GetWithoutChecks(0), 0);
  EXPECT_EQ(longArray->GetWithoutChecks(0), static_cast<int64_t>(0));
  EXPECT_FLOAT_EQ(floatArray->GetWithoutChecks(0), static_cast<float>(0.0f));
  EXPECT_DOUBLE_EQ(doubleArray->GetWithoutChecks(0), static_cast<double>(0.0f));
  EXPECT_EQ(objectArray->GetWithoutChecks(0), nullptr);
}

// Tests rolling back interned strings and resolved strings.
TEST_F(TransactionTest, ResolveString) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<3> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader>(LoadDex("Transaction"))));
  ASSERT_TRUE(class_loader != nullptr);

  Handle<mirror::Class> h_klass =
      hs.NewHandle(FindClass("LTransaction$ResolveString;", class_loader));
  ASSERT_TRUE(h_klass != nullptr);

  Handle<mirror::DexCache> h_dex_cache(hs.NewHandle(h_klass->GetDexCache()));
  ASSERT_TRUE(h_dex_cache != nullptr);
  const DexFile* const dex_file = h_dex_cache->GetDexFile();
  ASSERT_TRUE(dex_file != nullptr);

  // Go search the dex file to find the string id of our string.
  static const char* kResolvedString = "ResolvedString";
  const dex::StringId* string_id = dex_file->FindStringId(kResolvedString);
  ASSERT_TRUE(string_id != nullptr);
  dex::StringIndex string_idx = dex_file->GetIndexForStringId(*string_id);
  ASSERT_TRUE(string_idx.IsValid());
  // String should only get resolved by the initializer.
  EXPECT_TRUE(class_linker_->LookupString(string_idx, h_dex_cache.Get()) == nullptr);
  EXPECT_TRUE(h_dex_cache->GetResolvedString(string_idx) == nullptr);
  // Do the transaction, then roll back.
  EnterTransactionMode();
  bool success = class_linker_->EnsureInitialized(soa.Self(), h_klass, true, true);
  ASSERT_TRUE(success);
  ASSERT_TRUE(h_klass->IsInitialized());
  // Make sure the string got resolved by the transaction.
  {
    ObjPtr<mirror::String> s =
        class_linker_->LookupString(string_idx, h_dex_cache.Get());
    ASSERT_TRUE(s != nullptr);
    EXPECT_STREQ(s->ToModifiedUtf8().c_str(), kResolvedString);
    EXPECT_OBJ_PTR_EQ(s, h_dex_cache->GetResolvedString(string_idx));
  }
  RollbackAndExitTransactionMode();
  // Check that the string did not stay resolved.
  EXPECT_TRUE(class_linker_->LookupString(string_idx, h_dex_cache.Get()) == nullptr);
  EXPECT_TRUE(h_dex_cache->GetResolvedString(string_idx) == nullptr);
  ASSERT_FALSE(h_klass->IsInitialized());
  ASSERT_FALSE(soa.Self()->IsExceptionPending());
}

class MethodTypeTransactionTest : public TransactionTest {
 protected:
  MethodTypeTransactionTest() {
    // java.lang.invoke.MethodType factory methods and mirror::MethodType::Create
    // are backed by the same cache, which is in the primary boot image. As as a
    // result, MethodType creation can lead to writes to the map under a
    // transaction, which is forbidden.
    this->use_boot_image_ = false;
  }
};

// Tests rolling back resolved method types in dex cache.
TEST_F(MethodTypeTransactionTest, ResolveMethodType) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<3> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader>(LoadDex("Transaction"))));
  ASSERT_TRUE(class_loader != nullptr);

  MutableHandle<mirror::Class> h_klass(hs.NewHandle(
      class_linker_->FindSystemClass(soa.Self(), "Ljava/util/concurrent/ConcurrentHashMap$Node;")));
  ASSERT_TRUE(h_klass != nullptr);

  class_linker_->EnsureInitialized(soa.Self(), h_klass, true, true);
  ASSERT_TRUE(h_klass->IsInitialized());

  h_klass.Assign(FindClass("LTransaction;", class_loader));
  ASSERT_TRUE(h_klass != nullptr);

  Handle<mirror::DexCache> h_dex_cache(hs.NewHandle(h_klass->GetDexCache()));
  ASSERT_TRUE(h_dex_cache != nullptr);
  const DexFile* const dex_file = h_dex_cache->GetDexFile();
  ASSERT_TRUE(dex_file != nullptr);

  ASSERT_NE(dex_file->NumProtoIds(), 0u);
  dex::ProtoIndex proto_index(0u);
  ASSERT_TRUE(h_dex_cache->GetResolvedMethodType(proto_index) == nullptr);

  // Do the transaction, then roll back.
  EnterTransactionMode();
  ObjPtr<mirror::MethodType> method_type =
      class_linker_->ResolveMethodType(soa.Self(), proto_index, h_dex_cache, class_loader);
  ASSERT_TRUE(method_type != nullptr);
  // Make sure the method type was recorded in the dex cache.
  ASSERT_TRUE(h_dex_cache->GetResolvedMethodType(proto_index) == method_type);
  RollbackAndExitTransactionMode();
  // Check that the method type was removed from the dex cache.
  ASSERT_TRUE(h_dex_cache->GetResolvedMethodType(proto_index) == nullptr);
}

// Tests successful class initialization without class initializer.
TEST_F(TransactionTest, EmptyClass) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader>(LoadDex("Transaction"))));
  ASSERT_TRUE(class_loader != nullptr);

  Handle<mirror::Class> h_klass =
      hs.NewHandle(FindClass("LTransaction$EmptyStatic;", class_loader));
  ASSERT_TRUE(h_klass != nullptr);
  class_linker_->VerifyClass(soa.Self(), /* verifier_deps= */ nullptr, h_klass);
  ASSERT_TRUE(h_klass->IsVerified());

  EnterTransactionMode();
  bool success = class_linker_->EnsureInitialized(soa.Self(), h_klass, true, true);
  ExitTransactionMode();
  ASSERT_TRUE(success);
  ASSERT_TRUE(h_klass->IsInitialized());
  ASSERT_FALSE(soa.Self()->IsExceptionPending());
}

// Tests successful class initialization with class initializer.
TEST_F(TransactionTest, StaticFieldClass) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader>(LoadDex("Transaction"))));
  ASSERT_TRUE(class_loader != nullptr);

  Handle<mirror::Class> h_klass =
      hs.NewHandle(FindClass("LTransaction$StaticFieldClass;", class_loader));
  ASSERT_TRUE(h_klass != nullptr);
  class_linker_->VerifyClass(soa.Self(), /* verifier_deps= */ nullptr, h_klass);
  ASSERT_TRUE(h_klass->IsVerified());

  EnterTransactionMode();
  bool success = class_linker_->EnsureInitialized(soa.Self(), h_klass, true, true);
  ExitTransactionMode();
  ASSERT_TRUE(success);
  ASSERT_TRUE(h_klass->IsInitialized());
  ASSERT_FALSE(soa.Self()->IsExceptionPending());
}

// Tests failing class initialization due to native call.
TEST_F(TransactionTest, NativeCallAbortClass) {
  testTransactionAbort("LTransaction$NativeCallAbortClass;");
}

// Tests failing class initialization due to native call in a "synchronized" statement
// (which must catch any exception, do the monitor-exit then re-throw the caught exception).
TEST_F(TransactionTest, SynchronizedNativeCallAbortClass) {
  testTransactionAbort("LTransaction$SynchronizedNativeCallAbortClass;");
}

// Tests failing class initialization due to native call, even if an "all" catch handler
// catches the exception thrown when aborting the transaction.
TEST_F(TransactionTest, CatchNativeCallAbortClass) {
  testTransactionAbort("LTransaction$CatchNativeCallAbortClass;");
}

// Tests failing class initialization with multiple transaction aborts.
TEST_F(TransactionTest, MultipleNativeCallAbortClass) {
  testTransactionAbort("LTransaction$MultipleNativeCallAbortClass;");
}

// Tests failing class initialization due to Class.forName() not finding the class,
// even if an "all" catch handler catches the exception thrown when aborting the transaction.
TEST_F(TransactionTest, CatchClassForNameAbortClass) {
  testTransactionAbort("LTransaction$CatchClassForNameAbortClass;");
}

// Same as CatchClassForNameAbortClass but the class initializer tries to do the work twice.
// This would trigger a DCHECK() if we continued executing bytecode with an aborted transaction.
TEST_F(TransactionTest, CatchClassForNameAbortClassTwice) {
  testTransactionAbort("LTransaction$CatchClassForNameAbortClassTwice;");
}

// Tests failing class initialization due to allocating instance of finalizable class.
TEST_F(TransactionTest, FinalizableAbortClass) {
  testTransactionAbort("LTransaction$FinalizableAbortClass;");
}

TEST_F(TransactionTest, Constraints) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<11> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader>(LoadDex("Transaction"))));

  gc::Heap* heap = Runtime::Current()->GetHeap();
  Handle<mirror::Class> boolean_class =
      hs.NewHandle(FindClass("Ljava/lang/Boolean;", class_loader));
  ASSERT_TRUE(boolean_class != nullptr);
  ASSERT_TRUE(heap->ObjectIsInBootImageSpace(boolean_class.Get()));
  ArtField* true_field = boolean_class->FindDeclaredStaticField("TRUE", "Ljava/lang/Boolean;");
  ASSERT_TRUE(true_field != nullptr);
  ASSERT_TRUE(true_field->IsStatic());
  Handle<mirror::Object> true_value = hs.NewHandle(true_field->GetObject(boolean_class.Get()));
  ASSERT_TRUE(true_value != nullptr);
  ASSERT_TRUE(heap->ObjectIsInBootImageSpace(true_value.Get()));
  ArtField* value_field = boolean_class->FindDeclaredInstanceField("value", "Z");
  ASSERT_TRUE(value_field != nullptr);
  ASSERT_FALSE(value_field->IsStatic());

  Handle<mirror::Class> static_field_class =
      hs.NewHandle(FindClass("LTransaction$StaticFieldClass;", class_loader));
  ASSERT_TRUE(static_field_class != nullptr);
  ASSERT_FALSE(heap->ObjectIsInBootImageSpace(static_field_class.Get()));
  ArtField* int_field = static_field_class->FindDeclaredStaticField("intField", "I");
  ASSERT_TRUE(int_field != nullptr);

  Handle<mirror::Class> static_fields_test_class =
      hs.NewHandle(FindClass("LStaticFieldsTest;", class_loader));
  ASSERT_TRUE(static_fields_test_class != nullptr);
  ASSERT_FALSE(heap->ObjectIsInBootImageSpace(static_fields_test_class.Get()));
  ArtField* static_fields_test_int_field =
      static_fields_test_class->FindDeclaredStaticField("intField", "I");
  ASSERT_TRUE(static_fields_test_int_field != nullptr);

  Handle<mirror::Class> instance_fields_test_class =
      hs.NewHandle(FindClass("LInstanceFieldsTest;", class_loader));
  ASSERT_TRUE(instance_fields_test_class != nullptr);
  ASSERT_FALSE(heap->ObjectIsInBootImageSpace(instance_fields_test_class.Get()));
  ArtField* instance_fields_test_int_field =
      instance_fields_test_class->FindDeclaredInstanceField("intField", "I");
  ASSERT_TRUE(instance_fields_test_int_field != nullptr);
  Handle<mirror::Object> instance_fields_test_object = hs.NewHandle(
      instance_fields_test_class->Alloc(soa.Self(), heap->GetCurrentAllocator()));
  ASSERT_TRUE(instance_fields_test_object != nullptr);
  ASSERT_FALSE(heap->ObjectIsInBootImageSpace(instance_fields_test_object.Get()));

  // The `long[].class` should be in the boot image but `long[][][].class` should not.
  // (We have seen `long[][].class` both present and missing from the boot image,
  // depending on the libcore code, so we do not use it for this test.)
  Handle<mirror::Class> long_array_dim3_class = hs.NewHandle(FindClass("[[[J", class_loader));
  ASSERT_TRUE(long_array_dim3_class != nullptr);
  ASSERT_FALSE(heap->ObjectIsInBootImageSpace(long_array_dim3_class.Get()));
  ASSERT_TRUE(heap->ObjectIsInBootImageSpace(
      long_array_dim3_class->GetComponentType()->GetComponentType()));
  Handle<mirror::Array> long_array_dim3 = hs.NewHandle(mirror::Array::Alloc(
      soa.Self(),
      long_array_dim3_class.Get(),
      /*component_count=*/ 1,
      long_array_dim3_class->GetComponentSizeShift(),
      heap->GetCurrentAllocator()));
  ASSERT_TRUE(long_array_dim3 != nullptr);
  ASSERT_FALSE(heap->ObjectIsInBootImageSpace(long_array_dim3.Get()));
  Handle<mirror::Array> long_array = hs.NewHandle(mirror::Array::Alloc(
      soa.Self(),
      long_array_dim3_class->GetComponentType()->GetComponentType(),
      /*component_count=*/ 1,
      long_array_dim3_class->GetComponentType()->GetComponentType()->GetComponentSizeShift(),
      heap->GetCurrentAllocator()));
  ASSERT_TRUE(long_array != nullptr);
  ASSERT_FALSE(heap->ObjectIsInBootImageSpace(long_array.Get()));

  // Use the Array's IfTable as an array from the boot image.
  Handle<mirror::ObjectArray<mirror::Object>> array_iftable =
      hs.NewHandle(long_array_dim3_class->GetIfTable());
  ASSERT_TRUE(array_iftable != nullptr);
  ASSERT_TRUE(heap->ObjectIsInBootImageSpace(array_iftable.Get()));

  // Test non-strict transaction.
  ArenaPool* arena_pool = Runtime::Current()->GetArenaPool();
  Transaction transaction(
      /*strict=*/ false, /*root=*/ nullptr, /*arena_stack=*/ nullptr, arena_pool);
  // Static field in boot image.
  EXPECT_TRUE(transaction.WriteConstraint(boolean_class.Get()));
  EXPECT_FALSE(transaction.ReadConstraint(boolean_class.Get()));
  // Instance field or array element in boot image.
  // Do not check ReadConstraint(), it expects only static fields (checks for class object).
  EXPECT_TRUE(transaction.WriteConstraint(true_value.Get()));
  EXPECT_TRUE(transaction.WriteConstraint(array_iftable.Get()));
  // Static field not in boot image.
  EXPECT_FALSE(transaction.WriteConstraint(static_fields_test_class.Get()));
  EXPECT_FALSE(transaction.ReadConstraint(static_fields_test_class.Get()));
  // Instance field or array element not in boot image.
  // Do not check ReadConstraint(), it expects only static fields (checks for class object).
  EXPECT_FALSE(transaction.WriteConstraint(instance_fields_test_object.Get()));
  EXPECT_FALSE(transaction.WriteConstraint(long_array_dim3.Get()));
  // Write value constraints.
  EXPECT_FALSE(transaction.WriteValueConstraint(static_fields_test_class.Get()));
  EXPECT_FALSE(transaction.WriteValueConstraint(instance_fields_test_object.Get()));
  EXPECT_TRUE(transaction.WriteValueConstraint(long_array_dim3->GetClass()));
  EXPECT_TRUE(transaction.WriteValueConstraint(long_array_dim3.Get()));
  EXPECT_FALSE(transaction.WriteValueConstraint(long_array->GetClass()));
  EXPECT_FALSE(transaction.WriteValueConstraint(long_array.Get()));

  // Test strict transaction.
  Transaction strict_transaction(
      /*strict=*/ true, /*root=*/ static_field_class.Get(), /*arena_stack=*/ nullptr, arena_pool);
  // Static field in boot image.
  EXPECT_TRUE(strict_transaction.WriteConstraint(boolean_class.Get()));
  EXPECT_TRUE(strict_transaction.ReadConstraint(boolean_class.Get()));
  // Instance field or array element in boot image.
  // Do not check ReadConstraint(), it expects only static fields (checks for class object).
  EXPECT_TRUE(strict_transaction.WriteConstraint(true_value.Get()));
  EXPECT_TRUE(strict_transaction.WriteConstraint(array_iftable.Get()));
  // Static field in another class not in boot image.
  EXPECT_TRUE(strict_transaction.WriteConstraint(static_fields_test_class.Get()));
  EXPECT_TRUE(strict_transaction.ReadConstraint(static_fields_test_class.Get()));
  // Instance field or array element not in boot image.
  // Do not check ReadConstraint(), it expects only static fields (checks for class object).
  EXPECT_FALSE(strict_transaction.WriteConstraint(instance_fields_test_object.Get()));
  EXPECT_FALSE(strict_transaction.WriteConstraint(long_array_dim3.Get()));
  // Static field in the same class.
  EXPECT_FALSE(strict_transaction.WriteConstraint(static_field_class.Get()));
  EXPECT_FALSE(strict_transaction.ReadConstraint(static_field_class.Get()));
  // Write value constraints.
  EXPECT_FALSE(strict_transaction.WriteValueConstraint(static_fields_test_class.Get()));
  EXPECT_FALSE(strict_transaction.WriteValueConstraint(instance_fields_test_object.Get()));
  // TODO: The following may be revised, see a TODO in Transaction::WriteValueConstraint().
  EXPECT_FALSE(strict_transaction.WriteValueConstraint(long_array_dim3->GetClass()));
  EXPECT_FALSE(strict_transaction.WriteValueConstraint(long_array_dim3.Get()));
  EXPECT_FALSE(strict_transaction.WriteValueConstraint(long_array->GetClass()));
  EXPECT_FALSE(strict_transaction.WriteValueConstraint(long_array.Get()));
}

}  // namespace art
