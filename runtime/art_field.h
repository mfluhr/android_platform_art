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

#ifndef ART_RUNTIME_ART_FIELD_H_
#define ART_RUNTIME_ART_FIELD_H_

#include "base/macros.h"
#include "dex/modifiers.h"
#include "dex/primitive.h"
#include "gc_root.h"
#include "obj_ptr.h"
#include "offsets.h"
#include "read_barrier_option.h"
#include "verify_object.h"

namespace art HIDDEN {

class DexFile;
template<typename T> class LengthPrefixedArray;
class ScopedObjectAccessAlreadyRunnable;

namespace mirror {
class Class;
class ClassLoader;
class DexCache;
class Object;
class String;
}  // namespace mirror

class EXPORT ArtField final {
 public:
  // Visit declaring classes of all the art-fields in 'array' that reside
  // in [start_boundary, end_boundary).
  template<typename RootVisitorType>
  static void VisitArrayRoots(RootVisitorType& visitor,
                              uint8_t* start_boundary,
                              uint8_t* end_boundary,
                              LengthPrefixedArray<ArtField>* array)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<ReadBarrierOption kReadBarrierOption = kWithReadBarrier>
  ObjPtr<mirror::Class> GetDeclaringClass() REQUIRES_SHARED(Locks::mutator_lock_);

  ObjPtr<mirror::ClassLoader> GetClassLoader() REQUIRES_SHARED(Locks::mutator_lock_);

  void SetDeclaringClass(ObjPtr<mirror::Class> new_declaring_class)
      REQUIRES_SHARED(Locks::mutator_lock_);

  mirror::CompressedReference<mirror::Object>* GetDeclaringClassAddressWithoutBarrier() {
    return declaring_class_.AddressWithoutBarrier();
  }

  uint32_t GetAccessFlags() {
    return access_flags_;
  }

  void SetAccessFlags(uint32_t new_access_flags) {
    // Not called within a transaction.
    access_flags_ = new_access_flags;
  }

  bool IsPublic() {
    return (GetAccessFlags() & kAccPublic) != 0;
  }

  bool IsStatic() {
    return (GetAccessFlags() & kAccStatic) != 0;
  }

  bool IsFinal() {
    return (GetAccessFlags() & kAccFinal) != 0;
  }

  bool IsPrivate() {
    return (GetAccessFlags() & kAccPrivate) != 0;
  }

  uint32_t GetDexFieldIndex() {
    return field_dex_idx_;
  }

  void SetDexFieldIndex(uint32_t new_idx) {
    // Not called within a transaction.
    field_dex_idx_ = new_idx;
  }

  // Offset to field within an Object.
  MemberOffset GetOffset() {
    return MemberOffset(offset_);
  }

  static constexpr MemberOffset OffsetOffset() {
    return MemberOffset(OFFSETOF_MEMBER(ArtField, offset_));
  }

  static constexpr MemberOffset DeclaringClassOffset() {
    return MemberOffset(OFFSETOF_MEMBER(ArtField, declaring_class_));
  }

  MemberOffset GetOffsetDuringLinking() REQUIRES_SHARED(Locks::mutator_lock_);

  void SetOffset(MemberOffset num_bytes) REQUIRES_SHARED(Locks::mutator_lock_);

  // field access, null object for static fields
  uint8_t GetBoolean(ObjPtr<mirror::Object> object) REQUIRES_SHARED(Locks::mutator_lock_);

  template<bool kTransactionActive>
  void SetBoolean(ObjPtr<mirror::Object> object, uint8_t z) REQUIRES_SHARED(Locks::mutator_lock_);

  int8_t GetByte(ObjPtr<mirror::Object> object) REQUIRES_SHARED(Locks::mutator_lock_);

  template<bool kTransactionActive>
  void SetByte(ObjPtr<mirror::Object> object, int8_t b) REQUIRES_SHARED(Locks::mutator_lock_);

  uint16_t GetChar(ObjPtr<mirror::Object> object) REQUIRES_SHARED(Locks::mutator_lock_);
  uint16_t GetCharacter(ObjPtr<mirror::Object> object) REQUIRES_SHARED(Locks::mutator_lock_);

  template<bool kTransactionActive>
  void SetChar(ObjPtr<mirror::Object> object, uint16_t c) REQUIRES_SHARED(Locks::mutator_lock_);

  int16_t GetShort(ObjPtr<mirror::Object> object) REQUIRES_SHARED(Locks::mutator_lock_);

  template<bool kTransactionActive>
  void SetShort(ObjPtr<mirror::Object> object, int16_t s) REQUIRES_SHARED(Locks::mutator_lock_);

  int32_t GetInt(ObjPtr<mirror::Object> object) REQUIRES_SHARED(Locks::mutator_lock_);
  int32_t GetInteger(ObjPtr<mirror::Object> object) REQUIRES_SHARED(Locks::mutator_lock_);

  template<bool kTransactionActive>
  void SetInt(ObjPtr<mirror::Object> object, int32_t i) REQUIRES_SHARED(Locks::mutator_lock_);

  int64_t GetLong(ObjPtr<mirror::Object> object) REQUIRES_SHARED(Locks::mutator_lock_);

  template<bool kTransactionActive>
  void SetLong(ObjPtr<mirror::Object> object, int64_t j) REQUIRES_SHARED(Locks::mutator_lock_);

  float GetFloat(ObjPtr<mirror::Object> object) REQUIRES_SHARED(Locks::mutator_lock_);

  template<bool kTransactionActive>
  void SetFloat(ObjPtr<mirror::Object> object, float f) REQUIRES_SHARED(Locks::mutator_lock_);

  double GetDouble(ObjPtr<mirror::Object> object) REQUIRES_SHARED(Locks::mutator_lock_);

  template<bool kTransactionActive>
  void SetDouble(ObjPtr<mirror::Object> object, double d) REQUIRES_SHARED(Locks::mutator_lock_);

  template<ReadBarrierOption kReadBarrierOption = kWithReadBarrier>
  ObjPtr<mirror::Object> GetObject(ObjPtr<mirror::Object> object)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<bool kTransactionActive>
  void SetObject(ObjPtr<mirror::Object> object, ObjPtr<mirror::Object> l)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Raw field accesses.
  uint32_t Get32(ObjPtr<mirror::Object> object) REQUIRES_SHARED(Locks::mutator_lock_);

  template<bool kTransactionActive>
  void Set32(ObjPtr<mirror::Object> object, uint32_t new_value)
      REQUIRES_SHARED(Locks::mutator_lock_);

  uint64_t Get64(ObjPtr<mirror::Object> object) REQUIRES_SHARED(Locks::mutator_lock_);

  template<bool kTransactionActive>
  void Set64(ObjPtr<mirror::Object> object, uint64_t new_value)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<class MirrorType = mirror::Object,
           ReadBarrierOption kReadBarrierOption = kWithReadBarrier>
  ObjPtr<MirrorType> GetObj(ObjPtr<mirror::Object> object)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template<bool kTransactionActive>
  void SetObj(ObjPtr<mirror::Object> object, ObjPtr<mirror::Object> new_value)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // NO_THREAD_SAFETY_ANALYSIS since we don't know what the callback requires.
  template<typename RootVisitorType>
  ALWAYS_INLINE inline void VisitRoots(RootVisitorType& visitor) NO_THREAD_SAFETY_ANALYSIS {
    visitor.VisitRoot(declaring_class_.AddressWithoutBarrier());
  }

  bool IsVolatile() {
    return (GetAccessFlags() & kAccVolatile) != 0;
  }

  // Returns an instance field with this offset in the given class or null if not found.
  // If kExactOffset is true then we only find the matching offset, not the field containing the
  // offset.
  template <bool kExactOffset = true,
            VerifyObjectFlags kVerifyFlags = kDefaultVerifyFlags,
            ReadBarrierOption kReadBarrierOption = kWithReadBarrier>
  static ArtField* FindInstanceFieldWithOffset(ObjPtr<mirror::Class> klass, uint32_t field_offset)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Returns a static field with this offset in the given class or null if not found.
  // If kExactOffset is true then we only find the matching offset, not the field containing the
  // offset.
  template <bool kExactOffset = true>
  static ArtField* FindStaticFieldWithOffset(ObjPtr<mirror::Class> klass, uint32_t field_offset)
      REQUIRES_SHARED(Locks::mutator_lock_);

  const char* GetName() REQUIRES_SHARED(Locks::mutator_lock_);
  std::string_view GetNameView() REQUIRES_SHARED(Locks::mutator_lock_);

  // Resolves / returns the name from the dex cache.
  ObjPtr<mirror::String> ResolveNameString() REQUIRES_SHARED(Locks::mutator_lock_);

  const char* GetTypeDescriptor() REQUIRES_SHARED(Locks::mutator_lock_);
  std::string_view GetTypeDescriptorView() REQUIRES_SHARED(Locks::mutator_lock_);

  Primitive::Type GetTypeAsPrimitiveType() REQUIRES_SHARED(Locks::mutator_lock_);

  bool IsPrimitiveType() REQUIRES_SHARED(Locks::mutator_lock_);

  ObjPtr<mirror::Class> LookupResolvedType() REQUIRES_SHARED(Locks::mutator_lock_);
  ObjPtr<mirror::Class> ResolveType() REQUIRES_SHARED(Locks::mutator_lock_);

  size_t FieldSize() REQUIRES_SHARED(Locks::mutator_lock_);

  template <ReadBarrierOption kReadBarrierOption = kWithReadBarrier>
  ObjPtr<mirror::DexCache> GetDexCache() REQUIRES_SHARED(Locks::mutator_lock_);

  const DexFile* GetDexFile() REQUIRES_SHARED(Locks::mutator_lock_);

  const char* GetDeclaringClassDescriptor() REQUIRES_SHARED(Locks::mutator_lock_);
  std::string_view GetDeclaringClassDescriptorView() REQUIRES_SHARED(Locks::mutator_lock_);

  GcRoot<mirror::Class>& DeclaringClassRoot() {
    return declaring_class_;
  }

  // Returns a human-readable signature. Something like "a.b.C.f" or
  // "int a.b.C.f" (depending on the value of 'with_type').
  static std::string PrettyField(ArtField* f, bool with_type = true)
      REQUIRES_SHARED(Locks::mutator_lock_);
  std::string PrettyField(bool with_type = true)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Returns true if a set-* instruction in the given method is allowable.
  ALWAYS_INLINE inline bool CanBeChangedBy(ArtMethod* method) REQUIRES_SHARED(Locks::mutator_lock_);

 private:
  bool IsProxyField() REQUIRES_SHARED(Locks::mutator_lock_);

  ObjPtr<mirror::Class> ProxyFindSystemClass(std::string_view descriptor)
      REQUIRES_SHARED(Locks::mutator_lock_);

  GcRoot<mirror::Class> declaring_class_;

  uint32_t access_flags_ = 0;

  // Dex cache index of field id
  uint32_t field_dex_idx_ = 0;

  // Offset of field within an instance or in the Class' static fields
  uint32_t offset_ = 0;
};

}  // namespace art

#endif  // ART_RUNTIME_ART_FIELD_H_
