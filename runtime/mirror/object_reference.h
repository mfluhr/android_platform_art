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

#ifndef ART_RUNTIME_MIRROR_OBJECT_REFERENCE_H_
#define ART_RUNTIME_MIRROR_OBJECT_REFERENCE_H_

#include <array>
#include <string_view>

#include "base/atomic.h"
#include "base/casts.h"
#include "base/locks.h"  // For Locks::mutator_lock_.
#include "base/macros.h"
#include "heap_poisoning.h"
#include "obj_ptr.h"
#include "runtime_globals.h"

namespace art HIDDEN {
namespace mirror {

class Object;

// Classes shared with the managed side of the world need to be packed so that they don't have
// extra platform specific padding.
#define MANAGED PACKED(4)
#define MIRROR_CLASS(desc) \
  static_assert(::art::mirror::IsMirroredDescriptor(desc), \
                desc " is not a known mirror class. Please update" \
                " IsMirroredDescriptor to include it!")

constexpr bool IsMirroredDescriptor(std::string_view desc) {
  if (desc[0] != 'L') {
    // All primitives and arrays are mirrored
    return true;
  }
#define MIRROR_DESCRIPTORS(vis)                       \
    vis("Ljava/lang/Class;")                          \
    vis("Ljava/lang/ClassLoader;")                    \
    vis("Ljava/lang/ClassNotFoundException;")         \
    vis("Ljava/lang/DexCache;")                       \
    vis("Ljava/lang/Object;")                         \
    vis("Ljava/lang/StackFrameInfo;")                 \
    vis("Ljava/lang/StackTraceElement;")              \
    vis("Ljava/lang/String;")                         \
    vis("Ljava/lang/Throwable;")                      \
    vis("Ljava/lang/invoke/ArrayElementVarHandle;")   \
    vis("Ljava/lang/invoke/ByteArrayViewVarHandle;")  \
    vis("Ljava/lang/invoke/ByteBufferViewVarHandle;") \
    vis("Ljava/lang/invoke/CallSite;")                \
    vis("Ljava/lang/invoke/FieldVarHandle;")          \
    vis("Ljava/lang/invoke/StaticFieldVarHandle;")    \
    vis("Ljava/lang/invoke/MethodHandle;")            \
    vis("Ljava/lang/invoke/MethodHandleImpl;")        \
    vis("Ljava/lang/invoke/MethodHandles$Lookup;")    \
    vis("Ljava/lang/invoke/MethodType;")              \
    vis("Ljava/lang/invoke/VarHandle;")               \
    vis("Ljava/lang/ref/FinalizerReference;")         \
    vis("Ljava/lang/ref/Reference;")                  \
    vis("Ljava/lang/reflect/AccessibleObject;")       \
    vis("Ljava/lang/reflect/Constructor;")            \
    vis("Ljava/lang/reflect/Executable;")             \
    vis("Ljava/lang/reflect/Field;")                  \
    vis("Ljava/lang/reflect/Method;")                 \
    vis("Ljava/lang/reflect/Proxy;")                  \
    vis("Ldalvik/system/ClassExt;")                   \
    vis("Ldalvik/system/EmulatedStackFrame;")
  // TODO: Once we are C++ 20 we can just have a constexpr array and std::find.
  // constexpr std::array<std::string_view, 28> kMirrorTypes{
  //    // Fill in
  // };
  // return std::find(kMirrorTypes.begin(), kMirrorTypes.end(), desc) != kMirrorTypes.end();
#define CHECK_DESCRIPTOR(descriptor)          \
  if (std::string_view(descriptor) == desc) { \
    return true;                              \
  }
  MIRROR_DESCRIPTORS(CHECK_DESCRIPTOR)
#undef CHECK_DESCRIPTOR
  return false;
#undef MIRROR_DESCRIPTORS
}

template<bool kPoisonReferences, class MirrorType>
class PtrCompression {
 public:
  // Compress reference to its bit representation.
  static uint32_t Compress(MirrorType* mirror_ptr) {
    uint32_t as_bits = reinterpret_cast32<uint32_t>(mirror_ptr);
    return kPoisonReferences ? -as_bits : as_bits;
  }

  // Uncompress an encoded reference from its bit representation.
  static MirrorType* Decompress(uint32_t ref) {
    uint32_t as_bits = kPoisonReferences ? -ref : ref;
    return reinterpret_cast32<MirrorType*>(as_bits);
  }

  // Convert an ObjPtr to a compressed reference.
  static uint32_t Compress(ObjPtr<MirrorType> ptr) REQUIRES_SHARED(Locks::mutator_lock_);
};

// Value type representing a reference to a mirror::Object of type MirrorType.
template<bool kPoisonReferences, class MirrorType>
class MANAGED ObjectReference {
 private:
  using Compression = PtrCompression<kPoisonReferences, MirrorType>;

 public:
  /*
   * Returns a pointer to the mirror of the managed object this reference is for.
   *
   * This does NOT return the current object (which isn't derived from, and
   * therefor cannot be a mirror::Object) as a mirror pointer.  Instead, this
   * returns a pointer to the mirror of the managed object this refers to.
   *
   * TODO (chriswailes): Rename to GetPtr().
   */
  MirrorType* AsMirrorPtr() const {
    return Compression::Decompress(reference_);
  }

  void Assign(MirrorType* other) {
    reference_ = Compression::Compress(other);
  }

  void Assign(ObjPtr<MirrorType> ptr) REQUIRES_SHARED(Locks::mutator_lock_);

  void Clear() {
    reference_ = 0;
    DCHECK(IsNull());
  }

  bool IsNull() const {
    return reference_ == 0;
  }

  static ObjectReference<kPoisonReferences, MirrorType> FromMirrorPtr(MirrorType* mirror_ptr)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    return ObjectReference<kPoisonReferences, MirrorType>(mirror_ptr);
  }

 protected:
  explicit ObjectReference(MirrorType* mirror_ptr) REQUIRES_SHARED(Locks::mutator_lock_)
      : reference_(Compression::Compress(mirror_ptr)) {
  }
  ObjectReference() : reference_(0u) {
    DCHECK(IsNull());
  }

  // The encoded reference to a mirror::Object.
  uint32_t reference_;
};

// References between objects within the managed heap.
// Similar API to ObjectReference, but not a value type. Supports atomic access.
template<class MirrorType>
class MANAGED HeapReference {
 private:
  using Compression = PtrCompression<kPoisonHeapReferences, MirrorType>;

 public:
  HeapReference() REQUIRES_SHARED(Locks::mutator_lock_) : HeapReference(nullptr) {}

  template <bool kIsVolatile = false>
  MirrorType* AsMirrorPtr() const REQUIRES_SHARED(Locks::mutator_lock_) {
    return Compression::Decompress(
        kIsVolatile ? reference_.load(std::memory_order_seq_cst) : reference_.LoadJavaData());
  }

  template <bool kIsVolatile = false>
  void Assign(MirrorType* other) REQUIRES_SHARED(Locks::mutator_lock_) {
    if (kIsVolatile) {
      reference_.store(Compression::Compress(other), std::memory_order_seq_cst);
    } else {
      reference_.StoreJavaData(Compression::Compress(other));
    }
  }

  template <bool kIsVolatile = false>
  void Assign(ObjPtr<MirrorType> ptr) REQUIRES_SHARED(Locks::mutator_lock_);

  void Clear() {
    reference_.StoreJavaData(0);
    DCHECK(IsNull());
  }

  bool IsNull() const {
    return reference_.LoadJavaData() == 0;
  }

  static HeapReference<MirrorType> FromMirrorPtr(MirrorType* mirror_ptr)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    return HeapReference<MirrorType>(mirror_ptr);
  }

  bool CasWeakRelaxed(MirrorType* old_ptr, MirrorType* new_ptr)
      REQUIRES_SHARED(Locks::mutator_lock_);

 private:
  explicit HeapReference(MirrorType* mirror_ptr) REQUIRES_SHARED(Locks::mutator_lock_)
      : reference_(Compression::Compress(mirror_ptr)) {}

  // The encoded reference to a mirror::Object. Atomically updateable.
  Atomic<uint32_t> reference_;
};

static_assert(sizeof(mirror::HeapReference<mirror::Object>) == kHeapReferenceSize,
              "heap reference size does not match");

// Standard compressed reference used in the runtime. Used for StackReference and GC roots.
template<class MirrorType>
class MANAGED CompressedReference : public mirror::ObjectReference<false, MirrorType> {
 public:
  CompressedReference<MirrorType>()
      : mirror::ObjectReference<false, MirrorType>() {}

  static CompressedReference<MirrorType> FromMirrorPtr(MirrorType* p)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    return CompressedReference<MirrorType>(p);
  }

  static CompressedReference<MirrorType> FromVRegValue(uint32_t vreg_value) {
    CompressedReference<MirrorType> result;
    result.reference_ = vreg_value;
    return result;
  }

  uint32_t AsVRegValue() const {
    return this->reference_;
  }

 private:
  explicit CompressedReference(MirrorType* p) REQUIRES_SHARED(Locks::mutator_lock_)
      : ObjectReference<false, MirrorType>(p) {}
};

}  // namespace mirror
}  // namespace art

#endif  // ART_RUNTIME_MIRROR_OBJECT_REFERENCE_H_
