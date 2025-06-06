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

#ifndef ART_RUNTIME_MIRROR_ARRAY_INL_H_
#define ART_RUNTIME_MIRROR_ARRAY_INL_H_

#include "array.h"

#include <android-base/logging.h>

#include "base/bit_utils.h"
#include "base/casts.h"
#include "class.h"
#include "class_linker.h"
#include "obj_ptr-inl.h"
#include "runtime.h"
#include "thread-current-inl.h"

namespace art HIDDEN {
namespace mirror {

inline uint32_t Array::ClassSize(PointerSize pointer_size) {
  uint32_t vtable_entries = Object::kVTableLength;
  return Class::ComputeClassSize(true, vtable_entries, 0, 0, 0, 0, 0, 0, pointer_size);
}

template <VerifyObjectFlags kVerifyFlags>
inline size_t Array::SizeOf(size_t component_size_shift) {
  int32_t component_count = GetLength<kVerifyFlags>();
  // This is safe from overflow because the array was already allocated.
  size_t header_size = DataOffset(1U << component_size_shift).SizeValue();
  size_t data_size = component_count << component_size_shift;
  return header_size + data_size;
}

template <VerifyObjectFlags kVerifyFlags, ReadBarrierOption kReadBarrierOption>
inline size_t Array::SizeOf() {
  size_t component_size_shift = GetClass<kVerifyFlags, kReadBarrierOption>()
                                    ->template GetComponentSizeShift<kReadBarrierOption>();
  // Don't need to check this since we already check this in GetClass.
  return SizeOf<static_cast<VerifyObjectFlags>(kVerifyFlags & ~kVerifyThis)>(component_size_shift);
}

template<VerifyObjectFlags kVerifyFlags>
inline bool Array::CheckIsValidIndex(int32_t index) {
  if (UNLIKELY(static_cast<uint32_t>(index) >=
               static_cast<uint32_t>(GetLength<kVerifyFlags>()))) {
    ThrowArrayIndexOutOfBoundsException(index);
    return false;
  }
  return true;
}

template<typename T>
inline T PrimitiveArray<T>::Get(int32_t i) {
  if (!CheckIsValidIndex(i)) {
    DCHECK(Thread::Current()->IsExceptionPending());
    return T(0);
  }
  return GetWithoutChecks(i);
}

template<typename T>
inline void PrimitiveArray<T>::Set(int32_t i, T value) {
  if (Runtime::Current()->IsActiveTransaction()) {
    Set<true>(i, value);
  } else {
    Set<false>(i, value);
  }
}

template<typename T>
template<bool kTransactionActive, bool kCheckTransaction>
inline void PrimitiveArray<T>::Set(int32_t i, T value) {
  if (CheckIsValidIndex(i)) {
    SetWithoutChecks<kTransactionActive, kCheckTransaction>(i, value);
  } else {
    DCHECK(Thread::Current()->IsExceptionPending());
  }
}

template<typename T>
template<bool kTransactionActive, bool kCheckTransaction, VerifyObjectFlags kVerifyFlags>
inline void PrimitiveArray<T>::SetWithoutChecks(int32_t i, T value) {
  if (kCheckTransaction) {
    DCHECK_EQ(kTransactionActive, Runtime::Current()->IsActiveTransaction());
  }
  if (kTransactionActive) {
    Runtime::Current()->GetClassLinker()->RecordWriteArray(this, i, GetWithoutChecks(i));
  }
  DCHECK(CheckIsValidIndex<kVerifyFlags>(i)) << i << " " << GetLength<kVerifyFlags>();
  GetData()[i] = value;
}
// Backward copy where elements are of aligned appropriately for T. Count is in T sized units.
// Copies are guaranteed not to tear when the sizeof T is less-than 64bit.
template<typename T>
static inline void ArrayBackwardCopy(T* d, const T* s, int32_t count) {
  d += count;
  s += count;
  for (int32_t i = 0; i < count; ++i) {
    d--;
    s--;
    *d = *s;
  }
}

// Forward copy where elements are of aligned appropriately for T. Count is in T sized units.
// Copies are guaranteed not to tear when the sizeof T is less-than 64bit.
template<typename T>
static inline void ArrayForwardCopy(T* d, const T* s, int32_t count) {
  for (int32_t i = 0; i < count; ++i) {
    *d = *s;
    d++;
    s++;
  }
}

template<class T>
inline void PrimitiveArray<T>::Memmove(int32_t dst_pos,
                                       ObjPtr<PrimitiveArray<T>> src,
                                       int32_t src_pos,
                                       int32_t count) {
  if (UNLIKELY(count == 0)) {
    return;
  }
  DCHECK_GE(dst_pos, 0);
  DCHECK_GE(src_pos, 0);
  DCHECK_GT(count, 0);
  DCHECK(src != nullptr);
  DCHECK_LT(dst_pos, GetLength());
  DCHECK_LE(dst_pos, GetLength() - count);
  DCHECK_LT(src_pos, src->GetLength());
  DCHECK_LE(src_pos, src->GetLength() - count);

  // Note for non-byte copies we can't rely on standard libc functions like memcpy(3) and memmove(3)
  // in our implementation, because they may copy byte-by-byte.
  if (LIKELY(src != this)) {
    // Memcpy ok for guaranteed non-overlapping distinct arrays.
    Memcpy(dst_pos, src, src_pos, count);
  } else {
    // Handle copies within the same array using the appropriate direction copy.
    void* dst_raw = GetRawData(sizeof(T), dst_pos);
    const void* src_raw = src->GetRawData(sizeof(T), src_pos);
    if (sizeof(T) == sizeof(uint8_t)) {
      uint8_t* d = reinterpret_cast<uint8_t*>(dst_raw);
      const uint8_t* s = reinterpret_cast<const uint8_t*>(src_raw);
      memmove(d, s, count);
    } else {
      const bool copy_forward = (dst_pos < src_pos) || (dst_pos - src_pos >= count);
      if (sizeof(T) == sizeof(uint16_t)) {
        uint16_t* d = reinterpret_cast<uint16_t*>(dst_raw);
        const uint16_t* s = reinterpret_cast<const uint16_t*>(src_raw);
        if (copy_forward) {
          ArrayForwardCopy<uint16_t>(d, s, count);
        } else {
          ArrayBackwardCopy<uint16_t>(d, s, count);
        }
      } else if (sizeof(T) == sizeof(uint32_t)) {
        uint32_t* d = reinterpret_cast<uint32_t*>(dst_raw);
        const uint32_t* s = reinterpret_cast<const uint32_t*>(src_raw);
        if (copy_forward) {
          ArrayForwardCopy<uint32_t>(d, s, count);
        } else {
          ArrayBackwardCopy<uint32_t>(d, s, count);
        }
      } else {
        DCHECK_EQ(sizeof(T), sizeof(uint64_t));
        uint64_t* d = reinterpret_cast<uint64_t*>(dst_raw);
        const uint64_t* s = reinterpret_cast<const uint64_t*>(src_raw);
        if (copy_forward) {
          ArrayForwardCopy<uint64_t>(d, s, count);
        } else {
          ArrayBackwardCopy<uint64_t>(d, s, count);
        }
      }
    }
  }
}

template<class T>
inline void PrimitiveArray<T>::Memcpy(int32_t dst_pos,
                                      ObjPtr<PrimitiveArray<T>> src,
                                      int32_t src_pos,
                                      int32_t count) {
  if (UNLIKELY(count == 0)) {
    return;
  }
  DCHECK_GE(dst_pos, 0);
  DCHECK_GE(src_pos, 0);
  DCHECK_GT(count, 0);
  DCHECK(src != nullptr);
  DCHECK_LT(dst_pos, GetLength());
  DCHECK_LE(dst_pos, GetLength() - count);
  DCHECK_LT(src_pos, src->GetLength());
  DCHECK_LE(src_pos, src->GetLength() - count);

  // Note for non-byte copies we can't rely on standard libc functions like memcpy(3) and memmove(3)
  // in our implementation, because they may copy byte-by-byte.
  static_assert(sizeof(T) == sizeof(uint8_t) || sizeof(T) == sizeof(uint16_t) ||
                sizeof(T) == sizeof(uint32_t) || sizeof(T) == sizeof(uint64_t));
  void* dst_raw = GetRawData(sizeof(T), dst_pos);
  const void* src_raw = src->GetRawData(sizeof(T), src_pos);
  if (sizeof(T) == sizeof(uint8_t)) {
    memcpy(dst_raw, src_raw, count);
  } else if (sizeof(T) == sizeof(uint32_t)) {
    // b/392789466 Avoids copy using float registers on aarch64 for better performance.
    uint32_t* d = reinterpret_cast<uint32_t*>(dst_raw);
    const uint32_t* s = reinterpret_cast<const uint32_t*>(src_raw);
    ArrayForwardCopy<uint32_t>(d, s, count);
  } else {
    T* d = reinterpret_cast<T*>(dst_raw);
    const T* s = reinterpret_cast<const T*>(src_raw);
    ArrayForwardCopy<T>(d, s, count);
  }
}

template<typename T, PointerSize kPointerSize, VerifyObjectFlags kVerifyFlags>
inline T PointerArray::GetElementPtrSize(uint32_t idx) {
  if (kPointerSize == PointerSize::k64) {
    DCHECK(IsLongArray<kVerifyFlags>());
  } else {
    DCHECK(IsIntArray<kVerifyFlags>());
  }
  return GetElementPtrSizeUnchecked<T, kPointerSize, kVerifyFlags>(idx);
}

template<typename T, PointerSize kPointerSize, VerifyObjectFlags kVerifyFlags>
inline T PointerArray::GetElementPtrSizeUnchecked(uint32_t idx) {
  // C style casts here since we sometimes have T be a pointer, or sometimes an integer
  // (for stack traces).
  using ConversionType = typename std::conditional_t<std::is_pointer_v<T>, uintptr_t, T>;
  // Note: we cast the array directly when unchecked as this code gets called by
  // runtime_image, which can pass a 64bit pointer and therefore cannot be held
  // by an ObjPtr.
  if (kPointerSize == PointerSize::k64) {
    uint64_t value =
        static_cast<uint64_t>(reinterpret_cast<LongArray*>(this)->GetWithoutChecks(idx));
    return (T) dchecked_integral_cast<ConversionType>(value);
  } else {
    uint32_t value =
        static_cast<uint32_t>(reinterpret_cast<IntArray*>(this)->GetWithoutChecks(idx));
    return (T) dchecked_integral_cast<ConversionType>(value);
  }
}

template<typename T, VerifyObjectFlags kVerifyFlags>
inline T PointerArray::GetElementPtrSize(uint32_t idx, PointerSize ptr_size) {
  if (ptr_size == PointerSize::k64) {
    return GetElementPtrSize<T, PointerSize::k64, kVerifyFlags>(idx);
  }
  return GetElementPtrSize<T, PointerSize::k32, kVerifyFlags>(idx);
}

template<bool kTransactionActive, bool kCheckTransaction, bool kUnchecked>
inline void PointerArray::SetElementPtrSize(uint32_t idx, uint64_t element, PointerSize ptr_size) {
  // Note: we cast the array directly when unchecked as this code gets called by
  // runtime_image, which can pass a 64bit pointer and therefore cannot be held
  // by an ObjPtr.
  if (ptr_size == PointerSize::k64) {
    (kUnchecked ? reinterpret_cast<LongArray*>(this) : AsLongArray().Ptr())->
        SetWithoutChecks<kTransactionActive, kCheckTransaction>(idx, element);
  } else {
    uint32_t element32 = dchecked_integral_cast<uint32_t>(element);
    (kUnchecked ? reinterpret_cast<IntArray*>(this) : AsIntArray().Ptr())
        ->SetWithoutChecks<kTransactionActive, kCheckTransaction>(idx, element32);
  }
}

template<bool kTransactionActive, bool kCheckTransaction, bool kUnchecked, typename T>
inline void PointerArray::SetElementPtrSize(uint32_t idx, T* element, PointerSize ptr_size) {
  SetElementPtrSize<kTransactionActive, kCheckTransaction, kUnchecked>(
      idx, reinterpret_cast<uintptr_t>(element), ptr_size);
}

template <VerifyObjectFlags kVerifyFlags, typename Visitor>
inline void PointerArray::Fixup(mirror::PointerArray* dest,
                                PointerSize pointer_size,
                                const Visitor& visitor) {
  for (size_t i = 0, count = GetLength(); i < count; ++i) {
    void* ptr = GetElementPtrSize<void*, kVerifyFlags>(i, pointer_size);
    void* new_ptr = visitor(ptr);
    if (ptr != new_ptr) {
      dest->SetElementPtrSize</*kActiveTransaction=*/ false,
                              /*kCheckTransaction=*/ true,
                              /*kUnchecked=*/ true>(i, new_ptr, pointer_size);
    }
  }
}

template<bool kUnchecked>
void PointerArray::Memcpy(int32_t dst_pos,
                          ObjPtr<PointerArray> src,
                          int32_t src_pos,
                          int32_t count,
                          PointerSize ptr_size) {
  DCHECK(!Runtime::Current()->IsActiveTransaction());
  DCHECK(!src.IsNull());
  if (ptr_size == PointerSize::k64) {
    ObjPtr<LongArray> l_this = (kUnchecked ? ObjPtr<LongArray>::DownCast(ObjPtr<Object>(this))
                                           : AsLongArray());
    ObjPtr<LongArray> l_src = (kUnchecked ? ObjPtr<LongArray>::DownCast(ObjPtr<Object>(src))
                                          : src->AsLongArray());
    l_this->Memcpy(dst_pos, l_src, src_pos, count);
  } else {
    ObjPtr<IntArray> i_this = (kUnchecked ? ObjPtr<IntArray>::DownCast(ObjPtr<Object>(this))
                                          : AsIntArray());
    ObjPtr<IntArray> i_src = (kUnchecked ? ObjPtr<IntArray>::DownCast(ObjPtr<Object>(src.Ptr()))
                                         : src->AsIntArray());
    i_this->Memcpy(dst_pos, i_src, src_pos, count);
  }
}

}  // namespace mirror
}  // namespace art

#endif  // ART_RUNTIME_MIRROR_ARRAY_INL_H_
