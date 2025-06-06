/*
 * Copyright (C) 2016 The Android Open Source Project
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

#ifndef ART_RUNTIME_MIRROR_METHOD_HANDLE_IMPL_H_
#define ART_RUNTIME_MIRROR_METHOD_HANDLE_IMPL_H_

#include "art_field.h"
#include "art_method.h"
#include "base/macros.h"
#include "class.h"
#include "method_type.h"
#include "mirror/field.h"
#include "obj_ptr.h"
#include "object.h"

namespace art HIDDEN {

struct MethodHandleOffsets;
struct MethodHandleImplOffsets;
class ReflectiveValueVisitor;

namespace mirror {

// C++ mirror of java.lang.invoke.MethodHandle
class MANAGED MethodHandle : public Object {
 public:
  MIRROR_CLASS("Ljava/lang/invoke/MethodHandle;");

  // Defines the behaviour of a given method handle. The behaviour
  // of a handle of a given kind is identical to the dex bytecode behaviour
  // of the equivalent instruction.
  //
  // NOTE: These must be kept in sync with the constants defined in
  // java.lang.invoke.MethodHandle.
  enum Kind {
    kInvokeVirtual = 0,
    kInvokeSuper,
    kInvokeDirect,
    kInvokeStatic,
    kInvokeInterface,
    kInvokeTransform,
    kInvokeVarHandle,
    kInvokeVarHandleExact,
    kInstanceGet,
    kInstancePut,
    kStaticGet,
    kStaticPut,
    kLastValidKind = kStaticPut,
    kFirstAccessorKind = kInstanceGet,
    kLastAccessorKind = kStaticPut,
    kLastInvokeKind = kInvokeVarHandleExact
  };

  Kind GetHandleKind() REQUIRES_SHARED(Locks::mutator_lock_);

  ObjPtr<mirror::MethodType> GetMethodType() REQUIRES_SHARED(Locks::mutator_lock_);

  ObjPtr<mirror::MethodHandle> GetAsTypeCache() REQUIRES_SHARED(Locks::mutator_lock_);

  ArtField* GetTargetField() REQUIRES_SHARED(Locks::mutator_lock_);

  ArtMethod* GetTargetMethod() REQUIRES_SHARED(Locks::mutator_lock_);

  // Gets the return type for a named invoke method, or nullptr if the invoke method is not
  // supported.
  static const char* GetReturnTypeDescriptor(const char* invoke_method_name);

  // Used when classes become structurally obsolete to change the MethodHandle to refer to the new
  // method or field.
  void VisitTarget(ReflectiveValueVisitor* v) REQUIRES(Locks::mutator_lock_);

  static MemberOffset ArtFieldOrMethodOffset() {
    return MemberOffset(OFFSETOF_MEMBER(MethodHandle, art_field_or_method_));
  }

  static MemberOffset HandleKindOffset() {
    return MemberOffset(OFFSETOF_MEMBER(MethodHandle, handle_kind_));
  }

  static MemberOffset MethodTypeOffset() {
    return MemberOffset(OFFSETOF_MEMBER(MethodHandle, method_type_));
  }

 protected:
  void Initialize(uintptr_t art_field_or_method, Kind kind, Handle<MethodType> method_type)
      REQUIRES_SHARED(Locks::mutator_lock_);

 private:
  HeapReference<mirror::MethodHandle> as_type_cache_;
  HeapReference<mirror::MethodHandle> cached_spread_invoker_;
  HeapReference<mirror::MethodType> method_type_;
  uint32_t handle_kind_;
  uint64_t art_field_or_method_;

 private:
  static MemberOffset CachedSpreadInvokerOffset() {
    return MemberOffset(OFFSETOF_MEMBER(MethodHandle, cached_spread_invoker_));
  }
  static MemberOffset AsTypeCacheOffset() {
    return MemberOffset(OFFSETOF_MEMBER(MethodHandle, as_type_cache_));
  }

  friend struct art::MethodHandleOffsets;  // for verifying offset information
  DISALLOW_IMPLICIT_CONSTRUCTORS(MethodHandle);
};

// C++ mirror of java.lang.invoke.MethodHandleImpl
class MANAGED MethodHandleImpl : public MethodHandle {
 public:
  MIRROR_CLASS("Ljava/lang/invoke/MethodHandleImpl;");

  EXPORT static ObjPtr<mirror::MethodHandleImpl> Create(Thread* const self,
                                                        uintptr_t art_field_or_method,
                                                        MethodHandle::Kind kind,
                                                        Handle<MethodType> method_type)
      REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(!Roles::uninterruptible_);

  EXPORT static ObjPtr<mirror::MethodHandleImpl> Create(Thread* const self,
                                                        Handle<Field> field,
                                                        MethodHandle::Kind kind,
                                                        Handle<MethodType> method_type)
      REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(!Roles::uninterruptible_);

  static MemberOffset TargetOffset() {
    return MemberOffset(OFFSETOF_MEMBER(MethodHandleImpl, target_));
  }

 private:
  HeapReference<mirror::Field> field_;
  HeapReference<mirror::Object> target_class_or_info_;  // Unused by the runtime.
  uint64_t target_;

  friend struct art::MethodHandleImplOffsets;  // for verifying offset information
  DISALLOW_IMPLICIT_CONSTRUCTORS(MethodHandleImpl);
};

}  // namespace mirror
}  // namespace art

#endif  // ART_RUNTIME_MIRROR_METHOD_HANDLE_IMPL_H_
