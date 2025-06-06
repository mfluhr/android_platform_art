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

#ifndef ART_RUNTIME_MIRROR_OBJECT_H_
#define ART_RUNTIME_MIRROR_OBJECT_H_

#include "base/atomic.h"
#include "base/casts.h"
#include "base/macros.h"
#include "base/pointer_size.h"
#include "dex/primitive.h"
#include "obj_ptr.h"
#include "object_reference.h"
#include "offsets.h"
#include "read_barrier_config.h"
#include "read_barrier_option.h"
#include "runtime_globals.h"
#include "verify_object.h"

namespace art HIDDEN {

class ArtField;
class ArtMethod;
template <class T> class Handle;
class LockWord;
class Monitor;
struct ObjectOffsets;
class Thread;
class VoidFunctor;

namespace mirror {

class Array;
class Class;
class ClassLoader;
class DexCache;
class FinalizerReference;
template<class T> class ObjectArray;
template<class T> class PrimitiveArray;
using BooleanArray = PrimitiveArray<uint8_t>;
using ByteArray = PrimitiveArray<int8_t>;
using CharArray = PrimitiveArray<uint16_t>;
using DoubleArray = PrimitiveArray<double>;
using FloatArray = PrimitiveArray<float>;
using IntArray = PrimitiveArray<int32_t>;
using LongArray = PrimitiveArray<int64_t>;
using ShortArray = PrimitiveArray<int16_t>;
class Reference;
class String;
class Throwable;

// Fields within mirror objects aren't accessed directly so that the appropriate amount of
// handshaking is done with GC (for example, read and write barriers). This macro is used to
// compute an offset for the Set/Get methods defined in Object that can safely access fields.
#define OFFSET_OF_OBJECT_MEMBER(type, field) \
    MemberOffset(OFFSETOF_MEMBER(type, field))

// Checks that we don't do field assignments which violate the typing system.
static constexpr bool kCheckFieldAssignments = false;

// Size of Object.
static constexpr uint32_t kObjectHeaderSize = 8;

// C++ mirror of java.lang.Object
class EXPORT MANAGED LOCKABLE Object {
 public:
  MIRROR_CLASS("Ljava/lang/Object;");

  // The number of vtable entries in java.lang.Object.
  static constexpr size_t kVTableLength = 11;

  // The size of the java.lang.Class representing a java.lang.Object.
  static uint32_t ClassSize(PointerSize pointer_size);

  // Size of an instance of java.lang.Object.
  static constexpr uint32_t InstanceSize() {
    return sizeof(Object);
  }

  static constexpr MemberOffset ClassOffset() {
    return OFFSET_OF_OBJECT_MEMBER(Object, klass_);
  }

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags,
           ReadBarrierOption kReadBarrierOption = kWithReadBarrier>
  ALWAYS_INLINE Class* GetClass() REQUIRES_SHARED(Locks::mutator_lock_);

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  void SetClass(ObjPtr<Class> new_klass) REQUIRES_SHARED(Locks::mutator_lock_);

  // Get the read barrier state with a fake address dependency.
  // '*fake_address_dependency' will be set to 0.
  ALWAYS_INLINE uint32_t GetReadBarrierState(uintptr_t* fake_address_dependency)
      REQUIRES_SHARED(Locks::mutator_lock_);
  // This version does not offer any special mechanism to prevent load-load reordering.
  ALWAYS_INLINE uint32_t GetReadBarrierState() REQUIRES_SHARED(Locks::mutator_lock_);
  // Get the read barrier state with a load-acquire.
  ALWAYS_INLINE uint32_t GetReadBarrierStateAcquire() REQUIRES_SHARED(Locks::mutator_lock_);

  ALWAYS_INLINE void SetReadBarrierState(uint32_t rb_state) REQUIRES_SHARED(Locks::mutator_lock_);

  ALWAYS_INLINE bool AtomicSetReadBarrierState(uint32_t expected_rb_state,
                                               uint32_t rb_state,
                                               std::memory_order order = std::memory_order_relaxed)
      REQUIRES_SHARED(Locks::mutator_lock_);

  ALWAYS_INLINE uint32_t GetMarkBit() REQUIRES_SHARED(Locks::mutator_lock_);

  ALWAYS_INLINE bool AtomicSetMarkBit(uint32_t expected_mark_bit, uint32_t mark_bit)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Assert that the read barrier state is in the default (white, i.e. non-gray) state.
  ALWAYS_INLINE void AssertReadBarrierState() const REQUIRES_SHARED(Locks::mutator_lock_);

  // The verifier treats all interfaces as java.lang.Object and relies on runtime checks in
  // invoke-interface to detect incompatible interface types.
  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  bool VerifierInstanceOf(ObjPtr<Class> klass) REQUIRES_SHARED(Locks::mutator_lock_);
  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  ALWAYS_INLINE bool InstanceOf(ObjPtr<Class> klass) REQUIRES_SHARED(Locks::mutator_lock_);

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  size_t SizeOf() REQUIRES_SHARED(Locks::mutator_lock_);

  static ObjPtr<Object> Clone(Handle<Object> h_this, Thread* self)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Roles::uninterruptible_);

  // Returns a nonzero value that fits into lockword slot.
  int32_t IdentityHashCode()
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::thread_list_lock_,
               !Locks::thread_suspend_count_lock_);

  // Identical to the above, but returns 0 if monitor inflation would otherwise be needed.
  int32_t IdentityHashCodeNoInflation() REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::thread_list_lock_, !Locks::thread_suspend_count_lock_);

  static constexpr MemberOffset MonitorOffset() {
    return OFFSET_OF_OBJECT_MEMBER(Object, monitor_);
  }

  // As_volatile can be false if the mutators are suspended. This is an optimization since it
  // avoids the barriers.
  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  LockWord GetLockWord(bool as_volatile) REQUIRES_SHARED(Locks::mutator_lock_);
  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  void SetLockWord(LockWord new_val, bool as_volatile) REQUIRES_SHARED(Locks::mutator_lock_);
  bool CasLockWord(LockWord old_val, LockWord new_val, CASMode mode, std::memory_order memory_order)
      REQUIRES_SHARED(Locks::mutator_lock_);
  uint32_t GetLockOwnerThreadId() REQUIRES_SHARED(Locks::mutator_lock_);

  // Try to enter the monitor, returns non null if we succeeded.
  ObjPtr<mirror::Object> MonitorTryEnter(Thread* self)
      EXCLUSIVE_LOCK_FUNCTION()
      REQUIRES(!Roles::uninterruptible_)
      REQUIRES_SHARED(Locks::mutator_lock_);
  ObjPtr<mirror::Object> MonitorEnter(Thread* self)
      EXCLUSIVE_LOCK_FUNCTION()
      REQUIRES(!Roles::uninterruptible_)
      REQUIRES_SHARED(Locks::mutator_lock_);
  bool MonitorExit(Thread* self)
      REQUIRES(!Roles::uninterruptible_)
      REQUIRES_SHARED(Locks::mutator_lock_)
      UNLOCK_FUNCTION();
  void Notify(Thread* self) REQUIRES_SHARED(Locks::mutator_lock_);
  void NotifyAll(Thread* self) REQUIRES_SHARED(Locks::mutator_lock_);
  void Wait(Thread* self, int64_t timeout, int32_t nanos) REQUIRES_SHARED(Locks::mutator_lock_);

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  bool IsClass() REQUIRES_SHARED(Locks::mutator_lock_);
  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  ObjPtr<Class> AsClass() REQUIRES_SHARED(Locks::mutator_lock_);

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  bool IsObjectArray() REQUIRES_SHARED(Locks::mutator_lock_);
  template<class T, VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  ObjPtr<ObjectArray<T>> AsObjectArray() REQUIRES_SHARED(Locks::mutator_lock_);

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags,
           ReadBarrierOption kReadBarrierOption = kWithReadBarrier>
  bool IsClassLoader() REQUIRES_SHARED(Locks::mutator_lock_);
  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags,
           ReadBarrierOption kReadBarrierOption = kWithReadBarrier>
  ObjPtr<ClassLoader> AsClassLoader() REQUIRES_SHARED(Locks::mutator_lock_);

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags,
           ReadBarrierOption kReadBarrierOption = kWithReadBarrier>
  bool IsDexCache() REQUIRES_SHARED(Locks::mutator_lock_);
  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags,
           ReadBarrierOption kReadBarrierOption = kWithReadBarrier>
  ObjPtr<DexCache> AsDexCache() REQUIRES_SHARED(Locks::mutator_lock_);

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  bool IsArrayInstance() REQUIRES_SHARED(Locks::mutator_lock_);
  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  ObjPtr<Array> AsArray() REQUIRES_SHARED(Locks::mutator_lock_);

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  bool IsBooleanArray() REQUIRES_SHARED(Locks::mutator_lock_);
  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  ObjPtr<BooleanArray> AsBooleanArray() REQUIRES_SHARED(Locks::mutator_lock_);

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  bool IsByteArray() REQUIRES_SHARED(Locks::mutator_lock_);
  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  ObjPtr<ByteArray> AsByteArray() REQUIRES_SHARED(Locks::mutator_lock_);

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  bool IsCharArray() REQUIRES_SHARED(Locks::mutator_lock_);
  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  ObjPtr<CharArray> AsCharArray() REQUIRES_SHARED(Locks::mutator_lock_);

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  bool IsShortArray() REQUIRES_SHARED(Locks::mutator_lock_);
  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  ObjPtr<ShortArray> AsShortArray() REQUIRES_SHARED(Locks::mutator_lock_);

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  bool IsIntArray() REQUIRES_SHARED(Locks::mutator_lock_);
  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  ObjPtr<IntArray> AsIntArray() REQUIRES_SHARED(Locks::mutator_lock_);
  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  ObjPtr<IntArray> AsIntArrayUnchecked() REQUIRES_SHARED(Locks::mutator_lock_);

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  bool IsLongArray() REQUIRES_SHARED(Locks::mutator_lock_);
  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  ObjPtr<LongArray> AsLongArray() REQUIRES_SHARED(Locks::mutator_lock_);
  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  ObjPtr<LongArray> AsLongArrayUnchecked() REQUIRES_SHARED(Locks::mutator_lock_);

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  bool IsFloatArray() REQUIRES_SHARED(Locks::mutator_lock_);
  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  ObjPtr<FloatArray> AsFloatArray() REQUIRES_SHARED(Locks::mutator_lock_);

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  bool IsDoubleArray() REQUIRES_SHARED(Locks::mutator_lock_);
  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  ObjPtr<DoubleArray> AsDoubleArray() REQUIRES_SHARED(Locks::mutator_lock_);

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  bool IsString() REQUIRES_SHARED(Locks::mutator_lock_);

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  ObjPtr<String> AsString() REQUIRES_SHARED(Locks::mutator_lock_);

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  ObjPtr<Throwable> AsThrowable() REQUIRES_SHARED(Locks::mutator_lock_);

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags,
           ReadBarrierOption kReadBarrierOption = kWithReadBarrier>
  bool IsReferenceInstance() REQUIRES_SHARED(Locks::mutator_lock_);
  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags,
           ReadBarrierOption kReadBarrierOption = kWithReadBarrier>
  ObjPtr<Reference> AsReference() REQUIRES_SHARED(Locks::mutator_lock_);
  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  bool IsWeakReferenceInstance() REQUIRES_SHARED(Locks::mutator_lock_);
  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  bool IsSoftReferenceInstance() REQUIRES_SHARED(Locks::mutator_lock_);
  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  bool IsFinalizerReferenceInstance() REQUIRES_SHARED(Locks::mutator_lock_);
  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  ObjPtr<FinalizerReference> AsFinalizerReference() REQUIRES_SHARED(Locks::mutator_lock_);
  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  bool IsPhantomReferenceInstance() REQUIRES_SHARED(Locks::mutator_lock_);

  // Accessor for Java type fields.
  template<class T,
           VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags,
           ReadBarrierOption kReadBarrierOption = kWithReadBarrier,
           bool kIsVolatile = false>
  ALWAYS_INLINE T* GetFieldObject(MemberOffset field_offset)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<class T,
           VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags,
           ReadBarrierOption kReadBarrierOption = kWithReadBarrier>
  ALWAYS_INLINE T* GetFieldObjectVolatile(MemberOffset field_offset)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<bool kTransactionActive,
           bool kCheckTransaction = true,
           VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags,
           bool kIsVolatile = false>
  ALWAYS_INLINE void SetFieldObjectWithoutWriteBarrier(MemberOffset field_offset,
                                                       ObjPtr<Object> new_value)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<bool kTransactionActive,
           bool kCheckTransaction = true,
           VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags,
           bool kIsVolatile = false>
  ALWAYS_INLINE void SetFieldObject(MemberOffset field_offset, ObjPtr<Object> new_value)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<bool kTransactionActive,
           bool kCheckTransaction = true,
           VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  ALWAYS_INLINE void SetFieldObjectVolatile(MemberOffset field_offset, ObjPtr<Object> new_value)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<bool kCheckTransaction = true,
           VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags,
           bool kIsVolatile = false>
  ALWAYS_INLINE void SetFieldObjectTransaction(MemberOffset field_offset, ObjPtr<Object> new_value)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<bool kTransactionActive,
           bool kCheckTransaction = true,
           VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  ALWAYS_INLINE bool CasFieldObject(MemberOffset field_offset,
                                    ObjPtr<Object> old_value,
                                    ObjPtr<Object> new_value,
                                    CASMode mode,
                                    std::memory_order memory_order)
      REQUIRES_SHARED(Locks::mutator_lock_);
  template<bool kTransactionActive,
           bool kCheckTransaction = true,
           VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  ALWAYS_INLINE bool CasFieldObjectWithoutWriteBarrier(MemberOffset field_offset,
                                                       ObjPtr<Object> old_value,
                                                       ObjPtr<Object> new_value,
                                                       CASMode mode,
                                                       std::memory_order memory_order)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<bool kTransactionActive,
           bool kCheckTransaction = true,
           VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  ObjPtr<Object> CompareAndExchangeFieldObject(MemberOffset field_offset,
                                               ObjPtr<Object> old_value,
                                               ObjPtr<Object> new_value)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<bool kTransactionActive,
           bool kCheckTransaction = true,
           VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  ObjPtr<Object> ExchangeFieldObject(MemberOffset field_offset, ObjPtr<Object> new_value)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  HeapReference<Object>* GetFieldObjectReferenceAddr(MemberOffset field_offset)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<typename kType, bool kIsVolatile>
  ALWAYS_INLINE void SetFieldPrimitive(MemberOffset field_offset, kType new_value)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    uint8_t* raw_addr = reinterpret_cast<uint8_t*>(this) + field_offset.Int32Value();
    kType* addr = reinterpret_cast<kType*>(raw_addr);
    if (kIsVolatile) {
      reinterpret_cast<Atomic<kType>*>(addr)->store(new_value, std::memory_order_seq_cst);
    } else {
      reinterpret_cast<Atomic<kType>*>(addr)->StoreJavaData(new_value);
    }
  }

  template<typename kType, bool kIsVolatile>
  ALWAYS_INLINE kType GetFieldPrimitive(MemberOffset field_offset)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    const uint8_t* raw_addr = reinterpret_cast<const uint8_t*>(this) + field_offset.Int32Value();
    const kType* addr = reinterpret_cast<const kType*>(raw_addr);
    if (kIsVolatile) {
      return reinterpret_cast<const Atomic<kType>*>(addr)->load(std::memory_order_seq_cst);
    } else {
      return reinterpret_cast<const Atomic<kType>*>(addr)->LoadJavaData();
    }
  }

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags, bool kIsVolatile = false>
  ALWAYS_INLINE uint8_t GetFieldBoolean(MemberOffset field_offset)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    Verify<kVerifyFlags>();
    return GetFieldPrimitive<uint8_t, kIsVolatile>(field_offset);
  }

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags, bool kIsVolatile = false>
  ALWAYS_INLINE int8_t GetFieldByte(MemberOffset field_offset)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  ALWAYS_INLINE uint8_t GetFieldBooleanVolatile(MemberOffset field_offset)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  ALWAYS_INLINE int8_t GetFieldByteVolatile(MemberOffset field_offset)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<bool kTransactionActive,
           bool kCheckTransaction = true,
           VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags,
           bool kIsVolatile = false>
  ALWAYS_INLINE void SetFieldBoolean(MemberOffset field_offset, uint8_t new_value)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<bool kTransactionActive,
           bool kCheckTransaction = true,
           VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags,
           bool kIsVolatile = false>
  ALWAYS_INLINE void SetFieldByte(MemberOffset field_offset, int8_t new_value)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<bool kTransactionActive,
           bool kCheckTransaction = true,
           VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  ALWAYS_INLINE void SetFieldBooleanVolatile(MemberOffset field_offset, uint8_t new_value)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<bool kTransactionActive,
           bool kCheckTransaction = true,
           VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  ALWAYS_INLINE void SetFieldByteVolatile(MemberOffset field_offset, int8_t new_value)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags, bool kIsVolatile = false>
  ALWAYS_INLINE uint16_t GetFieldChar(MemberOffset field_offset)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags, bool kIsVolatile = false>
  ALWAYS_INLINE int16_t GetFieldShort(MemberOffset field_offset)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  ALWAYS_INLINE uint16_t GetFieldCharVolatile(MemberOffset field_offset)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  ALWAYS_INLINE int16_t GetFieldShortVolatile(MemberOffset field_offset)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<bool kTransactionActive,
           bool kCheckTransaction = true,
           VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags,
           bool kIsVolatile = false>
  ALWAYS_INLINE void SetFieldChar(MemberOffset field_offset, uint16_t new_value)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<bool kTransactionActive,
           bool kCheckTransaction = true,
           VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags,
           bool kIsVolatile = false>
  ALWAYS_INLINE void SetFieldShort(MemberOffset field_offset, int16_t new_value)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<bool kTransactionActive,
           bool kCheckTransaction = true,
           VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  ALWAYS_INLINE void SetFieldCharVolatile(MemberOffset field_offset, uint16_t new_value)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<bool kTransactionActive,
           bool kCheckTransaction = true,
           VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  ALWAYS_INLINE void SetFieldShortVolatile(MemberOffset field_offset, int16_t new_value)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags, bool kIsVolatile = false>
  ALWAYS_INLINE int32_t GetField32(MemberOffset field_offset)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    Verify<kVerifyFlags>();
    return GetFieldPrimitive<int32_t, kIsVolatile>(field_offset);
  }

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  ALWAYS_INLINE int32_t GetField32Volatile(MemberOffset field_offset)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    return GetField32<kVerifyFlags, true>(field_offset);
  }

  template<bool kTransactionActive,
           bool kCheckTransaction = true,
           VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags,
           bool kIsVolatile = false>
  ALWAYS_INLINE void SetField32(MemberOffset field_offset, int32_t new_value)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<bool kTransactionActive,
           bool kCheckTransaction = true,
           VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  ALWAYS_INLINE void SetField32Volatile(MemberOffset field_offset, int32_t new_value)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<bool kCheckTransaction = true,
           VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags,
           bool kIsVolatile = false>
  ALWAYS_INLINE void SetField32Transaction(MemberOffset field_offset, int32_t new_value)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<bool kTransactionActive,
           bool kCheckTransaction = true,
           VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  ALWAYS_INLINE bool CasField32(MemberOffset field_offset,
                                int32_t old_value,
                                int32_t new_value,
                                CASMode mode,
                                std::memory_order memory_order)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags, bool kIsVolatile = false>
  ALWAYS_INLINE int64_t GetField64(MemberOffset field_offset)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    Verify<kVerifyFlags>();
    return GetFieldPrimitive<int64_t, kIsVolatile>(field_offset);
  }

  template<VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  ALWAYS_INLINE int64_t GetField64Volatile(MemberOffset field_offset)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    return GetField64<kVerifyFlags, true>(field_offset);
  }

  template<bool kTransactionActive,
           bool kCheckTransaction = true,
           VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags,
           bool kIsVolatile = false>
  ALWAYS_INLINE void SetField64(MemberOffset field_offset, int64_t new_value)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<bool kTransactionActive,
           bool kCheckTransaction = true,
           VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  ALWAYS_INLINE void SetField64Volatile(MemberOffset field_offset, int64_t new_value)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<bool kCheckTransaction = true,
           VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags,
           bool kIsVolatile = false>
  ALWAYS_INLINE void SetField64Transaction(MemberOffset field_offset, int32_t new_value)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<bool kTransactionActive,
           bool kCheckTransaction = true,
           VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  bool CasFieldWeakSequentiallyConsistent64(MemberOffset field_offset,
                                            int64_t old_value,
                                            int64_t new_value)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template <bool kTransactionActive,
            bool kCheckTransaction = true,
            VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  bool CasFieldStrongSequentiallyConsistent64(MemberOffset field_offset,
                                              int64_t old_value,
                                              int64_t new_value)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template <bool kTransactionActive,
            bool kCheckTransaction = true,
            VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  int64_t CaeFieldStrongSequentiallyConsistent64(MemberOffset field_offset,
                                                 int64_t old_value,
                                                 int64_t new_value)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<bool kTransactionActive,
           bool kCheckTransaction = true,
           VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags,
           typename T>
  void SetFieldPtr(MemberOffset field_offset, T new_value)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    SetFieldPtrWithSize<kTransactionActive, kCheckTransaction, kVerifyFlags>(
        field_offset, new_value, kRuntimePointerSize);
  }
  template<bool kTransactionActive,
           bool kCheckTransaction = true,
           VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags,
           typename T>
  void SetFieldPtr64(MemberOffset field_offset, T new_value)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    SetFieldPtrWithSize<kTransactionActive, kCheckTransaction, kVerifyFlags>(
        field_offset, new_value, PointerSize::k64);
  }

  template<bool kTransactionActive,
           bool kCheckTransaction = true,
           VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags,
           typename T>
  ALWAYS_INLINE void SetFieldPtrWithSize(MemberOffset field_offset,
                                         T new_value,
                                         PointerSize pointer_size)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (pointer_size == PointerSize::k32) {
      SetField32<kTransactionActive, kCheckTransaction, kVerifyFlags>(
          field_offset, reinterpret_cast32<int32_t>(new_value));
    } else {
      SetField64<kTransactionActive, kCheckTransaction, kVerifyFlags>(
          field_offset, reinterpret_cast64<int64_t>(new_value));
    }
  }

  // Base class for accessors used to describe accesses performed by VarHandle methods.
  template <typename T>
  class Accessor {
   public:
    virtual ~Accessor() {
      static_assert(std::is_arithmetic<T>::value, "unsupported type");
    }
    virtual void Access(T* field_address) = 0;
  };

  // Getter method that exposes the raw address of a primitive value-type field to an Accessor
  // instance. This are used by VarHandle accessor methods to read fields with a wider range of
  // memory orderings than usually required.
  template<typename T, VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  void GetPrimitiveFieldViaAccessor(MemberOffset field_offset, Accessor<T>* accessor)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Update methods that expose the raw address of a primitive value-type to an Accessor instance
  // that will attempt to update the field. These are used by VarHandle accessor methods to
  // atomically update fields with a wider range of memory orderings than usually required.
  template<bool kTransactionActive,
           bool kCheckTransaction = true,
           VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  void UpdateFieldBooleanViaAccessor(MemberOffset field_offset, Accessor<uint8_t>* accessor)
      REQUIRES_SHARED(Locks::mutator_lock_);
  template<bool kTransactionActive,
           bool kCheckTransaction = true,
           VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  void UpdateFieldByteViaAccessor(MemberOffset field_offset, Accessor<int8_t>* accessor)
      REQUIRES_SHARED(Locks::mutator_lock_);
  template<bool kTransactionActive,
           bool kCheckTransaction = true,
           VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  void UpdateFieldCharViaAccessor(MemberOffset field_offset, Accessor<uint16_t>* accessor)
      REQUIRES_SHARED(Locks::mutator_lock_);
  template<bool kTransactionActive,
           bool kCheckTransaction = true,
           VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  void UpdateFieldShortViaAccessor(MemberOffset field_offset, Accessor<int16_t>* accessor)
      REQUIRES_SHARED(Locks::mutator_lock_);
  template<bool kTransactionActive,
           bool kCheckTransaction = true,
           VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  void UpdateField32ViaAccessor(MemberOffset field_offset, Accessor<int32_t>* accessor)
      REQUIRES_SHARED(Locks::mutator_lock_);
  template<bool kTransactionActive,
           bool kCheckTransaction = true,
           VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags>
  void UpdateField64ViaAccessor(MemberOffset field_offset, Accessor<int64_t>* accessor)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // TODO fix thread safety analysis broken by the use of template. This should be
  // REQUIRES_SHARED(Locks::mutator_lock_).
  template <bool kVisitNativeRoots = true,
            VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags,
            ReadBarrierOption kReadBarrierOption = kWithReadBarrier,
            typename Visitor,
            typename JavaLangRefVisitor = VoidFunctor>
  void VisitReferences(const Visitor& visitor, const JavaLangRefVisitor& ref_visitor)
      NO_THREAD_SAFETY_ANALYSIS;
  // VisitReferences version for compaction. It is invoked with from-space
  // object so that portions of the object, like klass and length (for arrays),
  // can be accessed without causing cascading faults.
  template <bool kFetchObjSize = true,
            bool kVisitNativeRoots = false,
            VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags,
            ReadBarrierOption kReadBarrierOption = kWithFromSpaceBarrier,
            typename Visitor>
  size_t VisitRefsForCompaction(const Visitor& visitor,
                                MemberOffset begin,
                                MemberOffset end) NO_THREAD_SAFETY_ANALYSIS;

  ArtField* FindFieldByOffset(MemberOffset offset) REQUIRES_SHARED(Locks::mutator_lock_);

  // Used by object_test.
  static void SetHashCodeSeed(uint32_t new_seed);
  // Generate an identity hash code. Public for object test.
  static uint32_t GenerateIdentityHashCode();

  // Returns a human-readable form of the name of the *class* of the given object.
  // So given an instance of java.lang.String, the output would
  // be "java.lang.String". Given an array of int, the output would be "int[]".
  // Given String.class, the output would be "java.lang.Class<java.lang.String>".
  static std::string PrettyTypeOf(ObjPtr<mirror::Object> obj)
      REQUIRES_SHARED(Locks::mutator_lock_);
  std::string PrettyTypeOf()
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Dump non-null references and their type.
  template <bool kDumpNativeRoots>
  void DumpReferences(std::ostream& osi, bool dump_type_of);
  // A utility function that does a raw copy of `src`'s data into the buffer `dst_bytes`.
  // Skips the object header.
  static void CopyRawObjectData(uint8_t* dst_bytes,
                                ObjPtr<mirror::Object> src,
                                size_t num_bytes)
      REQUIRES_SHARED(Locks::mutator_lock_);

 protected:
  // Accessors for non-Java type fields
  template<class T, VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags, bool kIsVolatile = false>
  T GetFieldPtr(MemberOffset field_offset)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    return GetFieldPtrWithSize<T, kVerifyFlags, kIsVolatile>(field_offset, kRuntimePointerSize);
  }
  template<class T, VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags, bool kIsVolatile = false>
  T GetFieldPtr64(MemberOffset field_offset)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    return GetFieldPtrWithSize<T, kVerifyFlags, kIsVolatile>(field_offset, PointerSize::k64);
  }

  template<class T, VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags, bool kIsVolatile = false>
  ALWAYS_INLINE T GetFieldPtrWithSize(MemberOffset field_offset, PointerSize pointer_size)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (pointer_size == PointerSize::k32) {
      int32_t v = GetField32<kVerifyFlags, kIsVolatile>(field_offset);
      return reinterpret_cast32<T>(v);
    } else {
      int64_t v = GetField64<kVerifyFlags, kIsVolatile>(field_offset);
      return reinterpret_cast64<T>(v);
    }
  }

  template <VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags,
            ReadBarrierOption kReadBarrierOption = kWithReadBarrier,
            typename Visitor>
  void VisitInstanceFieldsReferences(ObjPtr<mirror::Class> klass, const Visitor& visitor) HOT_ATTR
      REQUIRES_SHARED(Locks::mutator_lock_);

 private:
  template <bool kAllowInflation>
  int32_t IdentityHashCodeHelper() REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::thread_list_lock_, !Locks::thread_suspend_count_lock_);

  // Get a field with acquire semantics.
  template<typename kSize>
  ALWAYS_INLINE kSize GetFieldAcquire(MemberOffset field_offset)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Verify the type correctness of stores to fields.
  // TODO: This can cause thread suspension and isn't moving GC safe.
  void CheckFieldAssignmentImpl(MemberOffset field_offset, ObjPtr<Object> new_value)
      REQUIRES_SHARED(Locks::mutator_lock_);
  void CheckFieldAssignment(MemberOffset field_offset, ObjPtr<Object>new_value)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (kCheckFieldAssignments) {
      CheckFieldAssignmentImpl(field_offset, new_value);
    }
  }

  template<VerifyObjectFlags kVerifyFlags>
  ALWAYS_INLINE void Verify() REQUIRES_SHARED(Locks::mutator_lock_) {
    if (kVerifyFlags & kVerifyThis) {
      VerifyObject(this);
    }
  }

  // Not ObjPtr since the values may be unaligned for logic in verification.cc.
  template<VerifyObjectFlags kVerifyFlags, typename Reference>
  ALWAYS_INLINE static void VerifyRead(Reference value) REQUIRES_SHARED(Locks::mutator_lock_) {
    if (kVerifyFlags & kVerifyReads) {
      VerifyObject(value);
    }
  }

  template<VerifyObjectFlags kVerifyFlags>
  ALWAYS_INLINE static void VerifyWrite(ObjPtr<mirror::Object> value)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (kVerifyFlags & kVerifyWrites) {
      VerifyObject(value);
    }
  }

  template<VerifyObjectFlags kVerifyFlags>
  ALWAYS_INLINE void VerifyCAS(ObjPtr<mirror::Object> new_value, ObjPtr<mirror::Object> old_value)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    Verify<kVerifyFlags>();
    VerifyRead<kVerifyFlags>(old_value);
    VerifyWrite<kVerifyFlags>(new_value);
  }

  // Verify transaction is active (if required).
  template<bool kTransactionActive, bool kCheckTransaction>
  ALWAYS_INLINE void VerifyTransaction();

  // A utility function that copies an object in a read barrier and write barrier-aware way.
  // This is internally used by Clone() and Class::CopyOf(). If the object is finalizable,
  // it is the callers job to call Heap::AddFinalizerReference.
  static ObjPtr<Object> CopyObject(ObjPtr<mirror::Object> dest,
                                   ObjPtr<mirror::Object> src,
                                   size_t num_bytes)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<VerifyObjectFlags kVerifyFlags, Primitive::Type kType>
  bool IsSpecificPrimitiveArray() REQUIRES_SHARED(Locks::mutator_lock_);

  static Atomic<uint32_t> hash_code_seed;

  // The Class representing the type of the object.
  HeapReference<Class> klass_;
  // Monitor and hash code information.
  uint32_t monitor_;

  class DumpRefsVisitor;

  friend class art::Monitor;
  friend struct art::ObjectOffsets;  // for verifying offset information
  friend class CopyObjectVisitor;  // for CopyObject().
  friend class CopyClassVisitor;   // for CopyObject().
  DISALLOW_ALLOCATION();
  DISALLOW_IMPLICIT_CONSTRUCTORS(Object);
};

}  // namespace mirror
}  // namespace art

#endif  // ART_RUNTIME_MIRROR_OBJECT_H_
