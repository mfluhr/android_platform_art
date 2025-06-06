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

#ifndef ART_RUNTIME_ART_FIELD_INL_H_
#define ART_RUNTIME_ART_FIELD_INL_H_

#include "art_field.h"

#include <android-base/logging.h>

#include "class_linker-inl.h"
#include "dex/dex_file-inl.h"
#include "dex/primitive.h"
#include "gc/accounting/card_table-inl.h"
#include "gc_root-inl.h"
#include "jvalue.h"
#include "mirror/dex_cache-inl.h"
#include "mirror/object-inl.h"
#include "obj_ptr-inl.h"
#include "thread-current-inl.h"

namespace art HIDDEN {

inline bool ArtField::IsProxyField() {
  // No read barrier needed, we're reading the constant declaring class only to read
  // the constant proxy flag. See ReadBarrierOption.
  return GetDeclaringClass<kWithoutReadBarrier>()->IsProxyClass<kVerifyNone>();
}

// We are only ever allowed to set our own final fields
inline bool ArtField::CanBeChangedBy(ArtMethod* method) {
  ObjPtr<mirror::Class> declaring_class(GetDeclaringClass());
  ObjPtr<mirror::Class> referring_class(method->GetDeclaringClass());
  return !IsFinal() || (declaring_class == referring_class);
}

template<ReadBarrierOption kReadBarrierOption>
inline ObjPtr<mirror::Class> ArtField::GetDeclaringClass() {
  GcRootSource gc_root_source(this);
  ObjPtr<mirror::Class> result = declaring_class_.Read<kReadBarrierOption>(&gc_root_source);
  DCHECK(result != nullptr);
  DCHECK(result->IsIdxLoaded() || result->IsErroneous()) << result->GetStatus();
  return result;
}

inline void ArtField::SetDeclaringClass(ObjPtr<mirror::Class> new_declaring_class) {
  declaring_class_ = GcRoot<mirror::Class>(new_declaring_class);
}

template<typename RootVisitorType>
void ArtField::VisitArrayRoots(RootVisitorType& visitor,
                               uint8_t* start_boundary,
                               uint8_t* end_boundary,
                               LengthPrefixedArray<ArtField>* array) {
  DCHECK_LE(start_boundary, end_boundary);
  DCHECK_NE(array->size(), 0u);
  ArtField* first_field = &array->At(0);
  end_boundary = std::min(end_boundary, reinterpret_cast<uint8_t*>(first_field + array->size()));
  static constexpr size_t kFieldSize = sizeof(ArtField);
  // Confirm the assumption that ArtField size is power of two. It's important
  // as we assume so below (RoundUp).
  static_assert(IsPowerOfTwo(kFieldSize));
  uint8_t* declaring_class =
      reinterpret_cast<uint8_t*>(first_field) + DeclaringClassOffset().Int32Value();
  // Jump to the first class to visit.
  if (declaring_class < start_boundary) {
    declaring_class += RoundUp(start_boundary - declaring_class, kFieldSize);
  }
  while (declaring_class < end_boundary) {
    visitor.VisitRoot(
        reinterpret_cast<mirror::CompressedReference<mirror::Object>*>(declaring_class));
    declaring_class += kFieldSize;
  }
}

inline MemberOffset ArtField::GetOffsetDuringLinking() {
  DCHECK(GetDeclaringClass()->IsLoaded() || GetDeclaringClass()->IsErroneous());
  return MemberOffset(offset_);
}

inline uint32_t ArtField::Get32(ObjPtr<mirror::Object> object) {
  DCHECK(object != nullptr) << PrettyField();
  DCHECK(!IsStatic() || (object == GetDeclaringClass()) || !Runtime::Current()->IsStarted());
  if (UNLIKELY(IsVolatile())) {
    return object->GetField32Volatile(GetOffset());
  }
  return object->GetField32(GetOffset());
}

template<bool kTransactionActive>
inline void ArtField::Set32(ObjPtr<mirror::Object> object, uint32_t new_value) {
  DCHECK(object != nullptr) << PrettyField();
  DCHECK(!IsStatic() || (object == GetDeclaringClass()) || !Runtime::Current()->IsStarted());
  if (UNLIKELY(IsVolatile())) {
    object->SetField32Volatile<kTransactionActive>(GetOffset(), new_value);
  } else {
    object->SetField32<kTransactionActive>(GetOffset(), new_value);
  }
}

inline uint64_t ArtField::Get64(ObjPtr<mirror::Object> object) {
  DCHECK(object != nullptr) << PrettyField();
  DCHECK(!IsStatic() || (object == GetDeclaringClass()) || !Runtime::Current()->IsStarted());
  if (UNLIKELY(IsVolatile())) {
    return object->GetField64Volatile(GetOffset());
  }
  return object->GetField64(GetOffset());
}

template<bool kTransactionActive>
inline void ArtField::Set64(ObjPtr<mirror::Object> object, uint64_t new_value) {
  DCHECK(object != nullptr) << PrettyField();
  DCHECK(!IsStatic() || (object == GetDeclaringClass()) || !Runtime::Current()->IsStarted());
  if (UNLIKELY(IsVolatile())) {
    object->SetField64Volatile<kTransactionActive>(GetOffset(), new_value);
  } else {
    object->SetField64<kTransactionActive>(GetOffset(), new_value);
  }
}

template<class MirrorType, ReadBarrierOption kReadBarrierOption>
inline ObjPtr<MirrorType> ArtField::GetObj(ObjPtr<mirror::Object> object) {
  DCHECK(object != nullptr) << PrettyField();
  DCHECK(!IsStatic() ||
         (object == GetDeclaringClass<kReadBarrierOption>()) ||
         !Runtime::Current()->IsStarted());
  if (UNLIKELY(IsVolatile())) {
    return object->GetFieldObjectVolatile<MirrorType, kDefaultVerifyFlags, kReadBarrierOption>(
        GetOffset());
  }
  return object->GetFieldObject<MirrorType, kDefaultVerifyFlags, kReadBarrierOption>(GetOffset());
}

template<bool kTransactionActive>
inline void ArtField::SetObj(ObjPtr<mirror::Object> object, ObjPtr<mirror::Object> new_value) {
  DCHECK(object != nullptr) << PrettyField();
  DCHECK(!IsStatic() || (object == GetDeclaringClass()) || !Runtime::Current()->IsStarted());
  if (UNLIKELY(IsVolatile())) {
    object->SetFieldObjectVolatile<kTransactionActive>(GetOffset(), new_value);
  } else {
    object->SetFieldObject<kTransactionActive>(GetOffset(), new_value);
  }
}

#define FIELD_GET(object, type) \
  DCHECK_EQ(Primitive::kPrim ## type, GetTypeAsPrimitiveType()) << PrettyField(); \
  DCHECK((object) != nullptr) << PrettyField(); \
  DCHECK(!IsStatic() || ((object) == GetDeclaringClass()) || !Runtime::Current()->IsStarted()); \
  if (UNLIKELY(IsVolatile())) { \
    return (object)->GetField ## type ## Volatile(GetOffset()); \
  } \
  return (object)->GetField ## type(GetOffset());

#define FIELD_SET(object, type, value) \
  DCHECK((object) != nullptr) << PrettyField(); \
  DCHECK(!IsStatic() || ((object) == GetDeclaringClass()) || !Runtime::Current()->IsStarted()); \
  if (UNLIKELY(IsVolatile())) { \
    (object)->SetField ## type ## Volatile<kTransactionActive>(GetOffset(), value); \
  } else { \
    (object)->SetField ## type<kTransactionActive>(GetOffset(), value); \
  }

inline uint8_t ArtField::GetBoolean(ObjPtr<mirror::Object> object) {
  FIELD_GET(object, Boolean);
}

template<bool kTransactionActive>
inline void ArtField::SetBoolean(ObjPtr<mirror::Object> object, uint8_t z) {
  if (kIsDebugBuild) {
    // For simplicity, this method is being called by the compiler entrypoint for
    // both boolean and byte fields.
    Primitive::Type type = GetTypeAsPrimitiveType();
    DCHECK(type == Primitive::kPrimBoolean || type == Primitive::kPrimByte) << PrettyField();
  }
  FIELD_SET(object, Boolean, z);
}

inline int8_t ArtField::GetByte(ObjPtr<mirror::Object> object) {
  FIELD_GET(object, Byte);
}

template<bool kTransactionActive>
inline void ArtField::SetByte(ObjPtr<mirror::Object> object, int8_t b) {
  DCHECK_EQ(Primitive::kPrimByte, GetTypeAsPrimitiveType()) << PrettyField();
  FIELD_SET(object, Byte, b);
}

inline uint16_t ArtField::GetChar(ObjPtr<mirror::Object> object) {
  FIELD_GET(object, Char);
}

inline uint16_t ArtField::GetCharacter(ObjPtr<mirror::Object> object) {
  return GetChar(object);
}

template<bool kTransactionActive>
inline void ArtField::SetChar(ObjPtr<mirror::Object> object, uint16_t c) {
  if (kIsDebugBuild) {
    // For simplicity, this method is being called by the compiler entrypoint for
    // both char and short fields.
    Primitive::Type type = GetTypeAsPrimitiveType();
    DCHECK(type == Primitive::kPrimChar || type == Primitive::kPrimShort) << PrettyField();
  }
  FIELD_SET(object, Char, c);
}

inline int16_t ArtField::GetShort(ObjPtr<mirror::Object> object) {
  FIELD_GET(object, Short);
}

template<bool kTransactionActive>
inline void ArtField::SetShort(ObjPtr<mirror::Object> object, int16_t s) {
  DCHECK_EQ(Primitive::kPrimShort, GetTypeAsPrimitiveType()) << PrettyField();
  FIELD_SET(object, Short, s);
}

#undef FIELD_GET
#undef FIELD_SET

inline int32_t ArtField::GetInt(ObjPtr<mirror::Object> object) {
  if (kIsDebugBuild) {
    // For simplicity, this method is being called by the compiler entrypoint for
    // both int and float fields.
    Primitive::Type type = GetTypeAsPrimitiveType();
    CHECK(type == Primitive::kPrimInt || type == Primitive::kPrimFloat) << PrettyField();
  }
  return Get32(object);
}

inline int32_t ArtField::GetInteger(ObjPtr<mirror::Object> object) {
  return GetInt(object);
}

template<bool kTransactionActive>
inline void ArtField::SetInt(ObjPtr<mirror::Object> object, int32_t i) {
  if (kIsDebugBuild) {
    // For simplicity, this method is being called by the compiler entrypoint for
    // both int and float fields.
    Primitive::Type type = GetTypeAsPrimitiveType();
    CHECK(type == Primitive::kPrimInt || type == Primitive::kPrimFloat) << PrettyField();
  }
  Set32<kTransactionActive>(object, i);
}

inline int64_t ArtField::GetLong(ObjPtr<mirror::Object> object) {
  if (kIsDebugBuild) {
    // For simplicity, this method is being called by the compiler entrypoint for
    // both long and double fields.
    Primitive::Type type = GetTypeAsPrimitiveType();
    CHECK(type == Primitive::kPrimLong || type == Primitive::kPrimDouble) << PrettyField();
  }
  return Get64(object);
}

template<bool kTransactionActive>
inline void ArtField::SetLong(ObjPtr<mirror::Object> object, int64_t j) {
  if (kIsDebugBuild) {
    // For simplicity, this method is being called by the compiler entrypoint for
    // both long and double fields.
    Primitive::Type type = GetTypeAsPrimitiveType();
    CHECK(type == Primitive::kPrimLong || type == Primitive::kPrimDouble) << PrettyField();
  }
  Set64<kTransactionActive>(object, j);
}

inline float ArtField::GetFloat(ObjPtr<mirror::Object> object) {
  DCHECK_EQ(Primitive::kPrimFloat, GetTypeAsPrimitiveType()) << PrettyField();
  JValue bits;
  bits.SetI(Get32(object));
  return bits.GetF();
}

template<bool kTransactionActive>
inline void ArtField::SetFloat(ObjPtr<mirror::Object> object, float f) {
  DCHECK_EQ(Primitive::kPrimFloat, GetTypeAsPrimitiveType()) << PrettyField();
  JValue bits;
  bits.SetF(f);
  Set32<kTransactionActive>(object, bits.GetI());
}

inline double ArtField::GetDouble(ObjPtr<mirror::Object> object) {
  DCHECK_EQ(Primitive::kPrimDouble, GetTypeAsPrimitiveType()) << PrettyField();
  JValue bits;
  bits.SetJ(Get64(object));
  return bits.GetD();
}

template<bool kTransactionActive>
inline void ArtField::SetDouble(ObjPtr<mirror::Object> object, double d) {
  DCHECK_EQ(Primitive::kPrimDouble, GetTypeAsPrimitiveType()) << PrettyField();
  JValue bits;
  bits.SetD(d);
  Set64<kTransactionActive>(object, bits.GetJ());
}

template<ReadBarrierOption kReadBarrierOption>
inline ObjPtr<mirror::Object> ArtField::GetObject(ObjPtr<mirror::Object> object) {
  DCHECK_EQ(Primitive::kPrimNot, GetTypeAsPrimitiveType()) << PrettyField();
  return GetObj<mirror::Object, kReadBarrierOption>(object);
}

template<bool kTransactionActive>
inline void ArtField::SetObject(ObjPtr<mirror::Object> object, ObjPtr<mirror::Object> l) {
  DCHECK_EQ(Primitive::kPrimNot, GetTypeAsPrimitiveType()) << PrettyField();
  SetObj<kTransactionActive>(object, l);
}

inline const char* ArtField::GetName() {
  uint32_t field_index = GetDexFieldIndex();
  if (UNLIKELY(IsProxyField())) {
    DCHECK(IsStatic());
    DCHECK_LT(field_index, 2U);
    return field_index == 0 ? "interfaces" : "throws";
  }
  return GetDexFile()->GetFieldName(field_index);
}

inline std::string_view ArtField::GetNameView() {
  uint32_t field_index = GetDexFieldIndex();
  if (UNLIKELY(IsProxyField())) {
    DCHECK(IsStatic());
    DCHECK_LT(field_index, 2U);
    return field_index == 0 ? "interfaces" : "throws";
  }
  return GetDexFile()->GetFieldNameView(field_index);
}

inline const char* ArtField::GetTypeDescriptor() {
  uint32_t field_index = GetDexFieldIndex();
  if (UNLIKELY(IsProxyField())) {
    DCHECK(IsStatic());
    DCHECK_LT(field_index, 2U);
    // 0 == Class[] interfaces; 1 == Class[][] throws;
    return field_index == 0 ? "[Ljava/lang/Class;" : "[[Ljava/lang/Class;";
  }
  return GetDexFile()->GetFieldTypeDescriptor(field_index);
}

inline std::string_view ArtField::GetTypeDescriptorView() {
  uint32_t field_index = GetDexFieldIndex();
  if (UNLIKELY(IsProxyField())) {
    DCHECK(IsStatic());
    DCHECK_LT(field_index, 2U);
    // 0 == Class[] interfaces; 1 == Class[][] throws;
    return field_index == 0 ? "[Ljava/lang/Class;" : "[[Ljava/lang/Class;";
  }
  return GetDexFile()->GetFieldTypeDescriptorView(field_index);
}

inline Primitive::Type ArtField::GetTypeAsPrimitiveType() {
  return Primitive::GetType(GetTypeDescriptor()[0]);
}

inline bool ArtField::IsPrimitiveType() {
  return GetTypeAsPrimitiveType() != Primitive::kPrimNot;
}

inline ObjPtr<mirror::Class> ArtField::LookupResolvedType() {
  ScopedAssertNoThreadSuspension ants(__FUNCTION__);
  if (UNLIKELY(IsProxyField())) {
    return ProxyFindSystemClass(GetTypeDescriptorView());
  }
  ObjPtr<mirror::Class> type = Runtime::Current()->GetClassLinker()->LookupResolvedType(
      GetDexFile()->GetFieldId(GetDexFieldIndex()).type_idx_, this);
  DCHECK(!Thread::Current()->IsExceptionPending());
  return type;
}

inline ObjPtr<mirror::Class> ArtField::ResolveType() {
  if (UNLIKELY(IsProxyField())) {
    return ProxyFindSystemClass(GetTypeDescriptorView());
  }
  ObjPtr<mirror::Class> type = Runtime::Current()->GetClassLinker()->ResolveType(
      GetDexFile()->GetFieldId(GetDexFieldIndex()).type_idx_, this);
  DCHECK_EQ(type == nullptr, Thread::Current()->IsExceptionPending());
  return type;
}

inline size_t ArtField::FieldSize() {
  return Primitive::ComponentSize(GetTypeAsPrimitiveType());
}

template <ReadBarrierOption kReadBarrierOption>
inline ObjPtr<mirror::DexCache> ArtField::GetDexCache() {
  ObjPtr<mirror::Class> klass = GetDeclaringClass<kReadBarrierOption>();
  return klass->GetDexCache<kDefaultVerifyFlags, kReadBarrierOption>();
}

inline const DexFile* ArtField::GetDexFile() {
  return GetDexCache<kWithoutReadBarrier>()->GetDexFile();
}

inline const char* ArtField::GetDeclaringClassDescriptor() {
  DCHECK(!IsProxyField());
  return GetDexFile()->GetFieldDeclaringClassDescriptor(GetDexFieldIndex());
}

inline std::string_view ArtField::GetDeclaringClassDescriptorView() {
  DCHECK(!IsProxyField());
  return GetDexFile()->GetFieldDeclaringClassDescriptorView(GetDexFieldIndex());
}

inline ObjPtr<mirror::String> ArtField::ResolveNameString() {
  uint32_t dex_field_index = GetDexFieldIndex();
  CHECK_NE(dex_field_index, dex::kDexNoIndex);
  const dex::FieldId& field_id = GetDexFile()->GetFieldId(dex_field_index);
  return Runtime::Current()->GetClassLinker()->ResolveString(field_id.name_idx_, this);
}

// If kExactOffset is true then we only find the matching offset, not the field containing the
// offset.
template <bool kExactOffset>
static inline ArtField* FindFieldWithOffset(
    const IterationRange<StrideIterator<ArtField>>& fields,
    uint32_t field_offset,
    bool is_static) REQUIRES_SHARED(Locks::mutator_lock_) {
  for (ArtField& field : fields) {
    if (field.IsStatic() == is_static) {
      if (kExactOffset && field.GetOffset().Uint32Value() == field_offset) {
        return &field;
      } else {
        const uint32_t offset = field.GetOffset().Uint32Value();
        Primitive::Type type = field.GetTypeAsPrimitiveType();
        const size_t field_size = Primitive::ComponentSize(type);
        DCHECK_GT(field_size, 0u);
        if (offset <= field_offset && field_offset < offset + field_size) {
          return &field;
        }
      }
    }
  }
  return nullptr;
}

template <bool kExactOffset, VerifyObjectFlags kVerifyFlags, ReadBarrierOption kReadBarrierOption>
inline ArtField* ArtField::FindInstanceFieldWithOffset(ObjPtr<mirror::Class> klass,
                                                       uint32_t field_offset) {
  DCHECK(klass != nullptr);
  ArtField* field = FindFieldWithOffset<kExactOffset>(
      klass->GetFields(), field_offset, /* is_static= */ false);
  if (field != nullptr) {
    return field;
  }
  // We did not find field in the class: look into superclass.
  ObjPtr<mirror::Class> super_class = klass->GetSuperClass<kVerifyFlags, kReadBarrierOption>();
  return (super_class != nullptr)
      ? FindInstanceFieldWithOffset<kExactOffset, kVerifyFlags, kReadBarrierOption>(
          super_class, field_offset) :
      nullptr;
}

template <bool kExactOffset>
inline ArtField* ArtField::FindStaticFieldWithOffset(ObjPtr<mirror::Class> klass,
                                                     uint32_t field_offset) {
  DCHECK(klass != nullptr);
  return FindFieldWithOffset<kExactOffset>(klass->GetFields(), field_offset, /* is_static= */ true);
}

inline ObjPtr<mirror::ClassLoader> ArtField::GetClassLoader() {
  return GetDeclaringClass()->GetClassLoader();
}

}  // namespace art

#endif  // ART_RUNTIME_ART_FIELD_INL_H_
