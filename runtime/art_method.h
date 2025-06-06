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

#ifndef ART_RUNTIME_ART_METHOD_H_
#define ART_RUNTIME_ART_METHOD_H_

#include <cstddef>
#include <limits>

#include <android-base/logging.h>
#include <jni.h>

#include "base/array_ref.h"
#include "base/bit_utils.h"
#include "base/casts.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/pointer_size.h"
#include "base/runtime_debug.h"
#include "dex/dex_file_structs.h"
#include "dex/dex_file_types.h"
#include "dex/modifiers.h"
#include "dex/primitive.h"
#include "interpreter/mterp/nterp.h"
#include "gc_root.h"
#include "intrinsics_enum.h"
#include "obj_ptr.h"
#include "offsets.h"
#include "read_barrier_option.h"

namespace art HIDDEN {

class CodeItemDataAccessor;
class CodeItemDebugInfoAccessor;
class CodeItemInstructionAccessor;
class DexFile;
template<class T> class Handle;
class ImtConflictTable;
enum InvokeType : uint32_t;
union JValue;
template<typename T> class LengthPrefixedArray;
class OatQuickMethodHeader;
class ProfilingInfo;
class ScopedObjectAccessAlreadyRunnable;
class ShadowFrame;
class Signature;

namespace mirror {
class Array;
class Class;
class ClassLoader;
class DexCache;
class IfTable;
class Object;
template <typename MirrorType> class ObjectArray;
class PointerArray;
class String;
}  // namespace mirror

namespace detail {
template <char Shorty> struct ShortyTraits;
template <> struct ShortyTraits<'V'>;
template <> struct ShortyTraits<'Z'>;
template <> struct ShortyTraits<'B'>;
template <> struct ShortyTraits<'C'>;
template <> struct ShortyTraits<'S'>;
template <> struct ShortyTraits<'I'>;
template <> struct ShortyTraits<'J'>;
template <> struct ShortyTraits<'F'>;
template <> struct ShortyTraits<'D'>;
template <> struct ShortyTraits<'L'>;
template <char Shorty> struct HandleShortyTraits;
template <> struct HandleShortyTraits<'L'>;
}  // namespace detail

class EXPORT ArtMethod final {
 public:
  // Should the class state be checked on sensitive operations?
  DECLARE_RUNTIME_DEBUG_FLAG(kCheckDeclaringClassState);

  // The runtime dex_method_index is kDexNoIndex. To lower dependencies, we use this
  // constexpr, and ensure that the value is correct in art_method.cc.
  static constexpr uint32_t kRuntimeMethodDexMethodIndex = 0xFFFFFFFF;

  ArtMethod() : access_flags_(0), dex_method_index_(0),
      method_index_(0), hotness_count_(0) { }

  ArtMethod(ArtMethod* src, PointerSize image_pointer_size) {
    CopyFrom(src, image_pointer_size);
  }

  static ArtMethod* FromReflectedMethod(const ScopedObjectAccessAlreadyRunnable& soa,
                                        jobject jlr_method)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Visit the declaring class in 'method' if it is within [start_boundary, end_boundary).
  template<typename RootVisitorType>
  static void VisitRoots(RootVisitorType& visitor,
                         uint8_t* start_boundary,
                         uint8_t* end_boundary,
                         ArtMethod* method)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Visit declaring classes of all the art-methods in 'array' that reside
  // in [start_boundary, end_boundary).
  template<PointerSize kPointerSize, typename RootVisitorType>
  static void VisitArrayRoots(RootVisitorType& visitor,
                              uint8_t* start_boundary,
                              uint8_t* end_boundary,
                              LengthPrefixedArray<ArtMethod>* array)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template <ReadBarrierOption kReadBarrierOption = kWithReadBarrier>
  ALWAYS_INLINE ObjPtr<mirror::Class> GetDeclaringClass() REQUIRES_SHARED(Locks::mutator_lock_);

  template <ReadBarrierOption kReadBarrierOption = kWithReadBarrier>
  ALWAYS_INLINE ObjPtr<mirror::Class> GetDeclaringClassUnchecked()
      REQUIRES_SHARED(Locks::mutator_lock_);

  mirror::CompressedReference<mirror::Object>* GetDeclaringClassAddressWithoutBarrier() {
    return declaring_class_.AddressWithoutBarrier();
  }

  void SetDeclaringClass(ObjPtr<mirror::Class> new_declaring_class)
      REQUIRES_SHARED(Locks::mutator_lock_);

  bool CASDeclaringClass(ObjPtr<mirror::Class> expected_class, ObjPtr<mirror::Class> desired_class)
      REQUIRES_SHARED(Locks::mutator_lock_);

  static constexpr MemberOffset DeclaringClassOffset() {
    return MemberOffset(OFFSETOF_MEMBER(ArtMethod, declaring_class_));
  }

  uint32_t GetAccessFlags() const {
    return access_flags_.load(std::memory_order_relaxed);
  }

  // This version should only be called when it's certain there is no
  // concurrency so there is no need to guarantee atomicity. For example,
  // before the method is linked.
  void SetAccessFlags(uint32_t new_access_flags) REQUIRES_SHARED(Locks::mutator_lock_) {
    // The following check ensures that we do not set `Intrinsics::kNone` (see b/228049006).
    DCHECK_IMPLIES((new_access_flags & kAccIntrinsic) != 0,
                   (new_access_flags & kAccIntrinsicBits) != 0);
    access_flags_.store(new_access_flags, std::memory_order_relaxed);
  }

  static constexpr MemberOffset AccessFlagsOffset() {
    return MemberOffset(OFFSETOF_MEMBER(ArtMethod, access_flags_));
  }

  // Approximate what kind of method call would be used for this method.
  InvokeType GetInvokeType() REQUIRES_SHARED(Locks::mutator_lock_);

  // Returns true if the method is declared public.
  bool IsPublic() const {
    return IsPublic(GetAccessFlags());
  }

  static bool IsPublic(uint32_t access_flags) {
    return (access_flags & kAccPublic) != 0;
  }

  // Returns true if the method is declared private.
  bool IsPrivate() const {
    return IsPrivate(GetAccessFlags());
  }

  static bool IsPrivate(uint32_t access_flags) {
    return (access_flags & kAccPrivate) != 0;
  }

  // Returns true if the method is declared static.
  bool IsStatic() const {
    return IsStatic(GetAccessFlags());
  }

  static bool IsStatic(uint32_t access_flags) {
    return (access_flags & kAccStatic) != 0;
  }

  // Returns true if the method is a constructor according to access flags.
  bool IsConstructor() const {
    return IsConstructor(GetAccessFlags());
  }

  static bool IsConstructor(uint32_t access_flags) {
    return (access_flags & kAccConstructor) != 0;
  }

  // Returns true if the method is a class initializer according to access flags.
  bool IsClassInitializer() const {
    return IsClassInitializer(GetAccessFlags());
  }

  static bool IsClassInitializer(uint32_t access_flags) {
    return IsConstructor(access_flags) && IsStatic(access_flags);
  }

  // Returns true if the method is static, private, or a constructor.
  bool IsDirect() const {
    return IsDirect(GetAccessFlags());
  }

  static bool IsDirect(uint32_t access_flags) {
    constexpr uint32_t direct = kAccStatic | kAccPrivate | kAccConstructor;
    return (access_flags & direct) != 0;
  }

  // Returns true if the method is declared synchronized.
  bool IsSynchronized() const {
    return IsSynchronized(GetAccessFlags());
  }

  static bool IsSynchronized(uint32_t access_flags) {
    constexpr uint32_t synchonized = kAccSynchronized | kAccDeclaredSynchronized;
    return (access_flags & synchonized) != 0;
  }

  // Returns true if the method is declared final.
  bool IsFinal() const {
    return IsFinal(GetAccessFlags());
  }

  static bool IsFinal(uint32_t access_flags) {
    return (access_flags & kAccFinal) != 0;
  }

  // Returns true if the method is an intrinsic.
  bool IsIntrinsic() const {
    return IsIntrinsic(GetAccessFlags());
  }

  static bool IsIntrinsic(uint32_t access_flags) {
    return (access_flags & kAccIntrinsic) != 0;
  }

  ALWAYS_INLINE void SetIntrinsic(Intrinsics intrinsic) REQUIRES_SHARED(Locks::mutator_lock_);

  Intrinsics GetIntrinsic() const {
    static const int kAccFlagsShift = CTZ(kAccIntrinsicBits);
    static_assert(IsPowerOfTwo((kAccIntrinsicBits >> kAccFlagsShift) + 1),
                  "kAccIntrinsicBits are not continuous");
    static_assert((kAccIntrinsic & kAccIntrinsicBits) == 0,
                  "kAccIntrinsic overlaps kAccIntrinsicBits");
    DCHECK(IsIntrinsic());
    return static_cast<Intrinsics>((GetAccessFlags() & kAccIntrinsicBits) >> kAccFlagsShift);
  }

  void SetNotIntrinsic() REQUIRES_SHARED(Locks::mutator_lock_);

  // Returns true if the method is a copied method.
  bool IsCopied() const {
    return IsCopied(GetAccessFlags());
  }

  static bool IsCopied(uint32_t access_flags) {
    // We do not have intrinsics for any default methods and therefore intrinsics are never copied.
    // So we are using a flag from the intrinsic flags range and need to check `kAccIntrinsic` too.
    static_assert((kAccCopied & kAccIntrinsicBits) != 0,
                  "kAccCopied deliberately overlaps intrinsic bits");
    const bool copied = (access_flags & (kAccIntrinsic | kAccCopied)) == kAccCopied;
    // (IsMiranda() || IsDefaultConflicting()) implies copied
    DCHECK(!(IsMiranda(access_flags) || IsDefaultConflicting(access_flags)) || copied)
        << "Miranda or default-conflict methods must always be copied.";
    return copied;
  }

  bool IsMiranda() const {
    return IsMiranda(GetAccessFlags());
  }

  static bool IsMiranda(uint32_t access_flags) {
    // Miranda methods are marked as copied and abstract but not default.
    // We need to check the kAccIntrinsic too, see `IsCopied()`.
    static constexpr uint32_t kMask = kAccIntrinsic | kAccCopied | kAccAbstract | kAccDefault;
    static constexpr uint32_t kValue = kAccCopied | kAccAbstract;
    return (access_flags & kMask) == kValue;
  }

  // A default conflict method is a special sentinel method that stands for a conflict between
  // multiple default methods. It cannot be invoked, throwing an IncompatibleClassChangeError
  // if one attempts to do so.
  bool IsDefaultConflicting() const {
    return IsDefaultConflicting(GetAccessFlags());
  }

  static bool IsDefaultConflicting(uint32_t access_flags) {
    // Default conflct methods are marked as copied, abstract and default.
    // We need to check the kAccIntrinsic too, see `IsCopied()`.
    static constexpr uint32_t kMask = kAccIntrinsic | kAccCopied | kAccAbstract | kAccDefault;
    static constexpr uint32_t kValue = kAccCopied | kAccAbstract | kAccDefault;
    return (access_flags & kMask) == kValue;
  }

  // Returns true if invoking this method will not throw an AbstractMethodError or
  // IncompatibleClassChangeError.
  bool IsInvokable() const {
    return IsInvokable(GetAccessFlags());
  }

  static bool IsInvokable(uint32_t access_flags) {
    // Default conflicting methods are marked with `kAccAbstract` (as well as `kAccCopied`
    // and `kAccDefault`) but they are not considered abstract, see `IsAbstract()`.
    DCHECK_EQ((access_flags & kAccAbstract) == 0,
              !IsDefaultConflicting(access_flags) && !IsAbstract(access_flags));
    return (access_flags & kAccAbstract) == 0;
  }

  // Returns true if the method is marked as pre-compiled.
  bool IsPreCompiled() const {
    return IsPreCompiled(GetAccessFlags());
  }

  static bool IsPreCompiled(uint32_t access_flags) {
    // kAccCompileDontBother and kAccPreCompiled overlap with kAccIntrinsicBits.
    static_assert((kAccCompileDontBother & kAccIntrinsicBits) != 0);
    static_assert((kAccPreCompiled & kAccIntrinsicBits) != 0);
    static constexpr uint32_t kMask = kAccIntrinsic | kAccCompileDontBother | kAccPreCompiled;
    static constexpr uint32_t kValue = kAccCompileDontBother | kAccPreCompiled;
    return (access_flags & kMask) == kValue;
  }

  void SetPreCompiled() REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK(IsInvokable());
    DCHECK(IsCompilable());
    // kAccPreCompiled and kAccCompileDontBother overlaps with kAccIntrinsicBits.
    // We don't mark the intrinsics as precompiled, which means in JIT zygote
    // mode, compiled code for intrinsics will not be shared, and apps will
    // compile intrinsics themselves if needed.
    if (IsIntrinsic()) {
      return;
    }
    AddAccessFlags(kAccPreCompiled | kAccCompileDontBother);
  }

  void ClearPreCompiled() REQUIRES_SHARED(Locks::mutator_lock_) {
    ClearAccessFlags(kAccPreCompiled | kAccCompileDontBother);
  }

  // Returns true if the method resides in shared memory.
  bool IsMemorySharedMethod() {
    return IsMemorySharedMethod(GetAccessFlags());
  }

  static bool IsMemorySharedMethod(uint32_t access_flags) {
    // There's an overlap with `kAccMemorySharedMethod` and `kAccIntrinsicBits` but that's OK as
    // intrinsics are always in the boot image and therefore memory shared.
    static_assert((kAccMemorySharedMethod & kAccIntrinsicBits) != 0,
                  "kAccMemorySharedMethod deliberately overlaps intrinsic bits");
    if (IsIntrinsic(access_flags)) {
      return true;
    }

    return (access_flags & kAccMemorySharedMethod) != 0;
  }

  void SetMemorySharedMethod() REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK(!IsIntrinsic());
    DCHECK(!IsAbstract());
    AddAccessFlags(kAccMemorySharedMethod);
  }

  static uint32_t SetMemorySharedMethod(uint32_t access_flags) {
    DCHECK(!IsIntrinsic(access_flags));
    DCHECK(!IsAbstract(access_flags));
    return access_flags | kAccMemorySharedMethod;
  }

  void ClearMemorySharedMethod() REQUIRES_SHARED(Locks::mutator_lock_) {
    uint32_t access_flags = GetAccessFlags();
    if (IsIntrinsic(access_flags) || IsAbstract(access_flags)) {
      return;
    }
    if (IsMemorySharedMethod(access_flags)) {
      ClearAccessFlags(kAccMemorySharedMethod);
    }
  }

  // Returns true if the method can be compiled.
  bool IsCompilable() const {
    return IsCompilable(GetAccessFlags());
  }

  static bool IsCompilable(uint32_t access_flags) {
    if (IsIntrinsic(access_flags)) {
      // kAccCompileDontBother overlaps with kAccIntrinsicBits.
      return true;
    }
    if (IsPreCompiled(access_flags)) {
      return true;
    }
    return (access_flags & kAccCompileDontBother) == 0;
  }

  void ClearDontCompile() REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK(!IsMiranda());
    ClearAccessFlags(kAccCompileDontBother);
  }

  void SetDontCompile() REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK(!IsMiranda());
    AddAccessFlags(kAccCompileDontBother);
  }

  // This is set by the class linker.
  bool IsDefault() const {
    return IsDefault(GetAccessFlags());
  }

  static bool IsDefault(uint32_t access_flags) {
    // The intrinsic bits use `kAccDefault`. However, we don't generate intrinsics for default
    // methods. Therefore, we check that both `kAccDefault` is set and `kAccIntrinsic` unset.
    static_assert((kAccDefault & kAccIntrinsicBits) != 0,
                  "kAccDefault deliberately overlaps intrinsic bits");
    static constexpr uint32_t kMask = kAccIntrinsic | kAccDefault;
    static constexpr uint32_t kValue = kAccDefault;
    return (access_flags & kMask) == kValue;
  }

  // Returns true if the method is obsolete.
  bool IsObsolete() const {
    return IsObsolete(GetAccessFlags());
  }

  static bool IsObsolete(uint32_t access_flags) {
    return (access_flags & kAccObsoleteMethod) != 0;
  }

  void SetIsObsolete() REQUIRES_SHARED(Locks::mutator_lock_) {
    AddAccessFlags(kAccObsoleteMethod);
  }

  // Returns true if the method is native.
  bool IsNative() const {
    return IsNative(GetAccessFlags());
  }

  static bool IsNative(uint32_t access_flags) {
    return (access_flags & kAccNative) != 0;
  }

  // Checks to see if the method was annotated with @dalvik.annotation.optimization.FastNative.
  bool IsFastNative() const {
    return IsFastNative(GetAccessFlags());
  }

  static bool IsFastNative(uint32_t access_flags) {
    // The presence of the annotation is checked by ClassLinker and recorded in access flags.
    // The kAccFastNative flag value is used with a different meaning for non-native methods,
    // so we need to check the kAccNative flag as well.
    constexpr uint32_t mask = kAccFastNative | kAccNative;
    return (access_flags & mask) == mask;
  }

  // Checks to see if the method was annotated with @dalvik.annotation.optimization.CriticalNative.
  bool IsCriticalNative() const {
    return IsCriticalNative(GetAccessFlags());
  }

  static bool IsCriticalNative([[maybe_unused]] uint32_t access_flags) {
#ifdef ART_USE_RESTRICTED_MODE
    // Return false to treat all critical native methods as normal native methods instead, i.e.:
    // will use the generic JNI trampoline instead.
    // TODO(Simulator): support critical native methods
    return false;
#else
    // The presence of the annotation is checked by ClassLinker and recorded in access flags.
    // The kAccCriticalNative flag value is used with a different meaning for non-native methods,
    // so we need to check the kAccNative flag as well.
    constexpr uint32_t mask = kAccCriticalNative | kAccNative;
    return (access_flags & mask) == mask;
#endif
  }

  // Returns true if the method is managed (not native).
  bool IsManaged() const {
    return IsManaged(GetAccessFlags());
  }

  static bool IsManaged(uint32_t access_flags) {
    return !IsNative(access_flags);
  }

  // Returns true if the method is managed (not native) and invokable.
  bool IsManagedAndInvokable() const {
    return IsManagedAndInvokable(GetAccessFlags());
  }

  static bool IsManagedAndInvokable(uint32_t access_flags) {
    return IsManaged(access_flags) && IsInvokable(access_flags);
  }

  // Returns true if the method is abstract.
  bool IsAbstract() const {
    return IsAbstract(GetAccessFlags());
  }

  static bool IsAbstract(uint32_t access_flags) {
    // Default confliciting methods have `kAccAbstract` set but they are not actually abstract.
    return (access_flags & kAccAbstract) != 0 && !IsDefaultConflicting(access_flags);
  }

  // Returns true if the method is declared synthetic.
  bool IsSynthetic() const {
    return IsSynthetic(GetAccessFlags());
  }

  static bool IsSynthetic(uint32_t access_flags) {
    return (access_flags & kAccSynthetic) != 0;
  }

  // Returns true if the method is declared varargs.
  bool IsVarargs() const {
    return IsVarargs(GetAccessFlags());
  }

  static bool IsVarargs(uint32_t access_flags) {
    return (access_flags & kAccVarargs) != 0;
  }

  bool IsProxyMethod() REQUIRES_SHARED(Locks::mutator_lock_);

  bool IsSignaturePolymorphic() REQUIRES_SHARED(Locks::mutator_lock_);

  bool SkipAccessChecks() const {
    // The kAccSkipAccessChecks flag value is used with a different meaning for native methods,
    // so we need to check the kAccNative flag as well.
    return (GetAccessFlags() & (kAccSkipAccessChecks | kAccNative)) == kAccSkipAccessChecks;
  }

  void SetSkipAccessChecks() REQUIRES_SHARED(Locks::mutator_lock_) {
    // SkipAccessChecks() is applicable only to non-native methods.
    DCHECK(!IsNative());
    AddAccessFlags(kAccSkipAccessChecks);
  }
  void ClearSkipAccessChecks() REQUIRES_SHARED(Locks::mutator_lock_) {
    // SkipAccessChecks() is applicable only to non-native methods.
    DCHECK(!IsNative());
    ClearAccessFlags(kAccSkipAccessChecks);
  }

  // Returns true if the method has previously been warm.
  bool PreviouslyWarm() const {
    return PreviouslyWarm(GetAccessFlags());
  }

  static bool PreviouslyWarm(uint32_t access_flags) {
    // kAccPreviouslyWarm overlaps with kAccIntrinsicBits. Return true for intrinsics.
    constexpr uint32_t mask = kAccPreviouslyWarm | kAccIntrinsic;
    return (access_flags & mask) != 0u;
  }

  void SetPreviouslyWarm() REQUIRES_SHARED(Locks::mutator_lock_) {
    if (IsIntrinsic()) {
      // kAccPreviouslyWarm overlaps with kAccIntrinsicBits.
      return;
    }
    AddAccessFlags(kAccPreviouslyWarm);
  }

  // Should this method be run in the interpreter and count locks (e.g., failed structured-
  // locking verification)?
  bool MustCountLocks() const {
    return MustCountLocks(GetAccessFlags());
  }

  static bool MustCountLocks(uint32_t access_flags) {
    if (IsIntrinsic(access_flags)) {
      return false;
    }
    return (access_flags & kAccMustCountLocks) != 0;
  }

  void ClearMustCountLocks() REQUIRES_SHARED(Locks::mutator_lock_) {
    ClearAccessFlags(kAccMustCountLocks);
  }

  void SetMustCountLocks() REQUIRES_SHARED(Locks::mutator_lock_) {
    ClearAccessFlags(kAccSkipAccessChecks);
    AddAccessFlags(kAccMustCountLocks);
  }

  // Returns true if the method is using the nterp entrypoint fast path.
  bool HasNterpEntryPointFastPathFlag() const {
    return HasNterpEntryPointFastPathFlag(GetAccessFlags());
  }

  static bool HasNterpEntryPointFastPathFlag(uint32_t access_flags) {
    constexpr uint32_t mask = kAccNative | kAccNterpEntryPointFastPathFlag;
    return (access_flags & mask) == kAccNterpEntryPointFastPathFlag;
  }

  void SetNterpEntryPointFastPathFlag() REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK(!IsNative());
    AddAccessFlags(kAccNterpEntryPointFastPathFlag);
  }

  void ClearNterpEntryPointFastPathFlag() REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK(!IsNative());
    ClearAccessFlags(kAccNterpEntryPointFastPathFlag);
  }

  void SetNterpInvokeFastPathFlag() REQUIRES_SHARED(Locks::mutator_lock_) {
    AddAccessFlags(kAccNterpInvokeFastPathFlag);
  }

  void ClearNterpInvokeFastPathFlag() REQUIRES_SHARED(Locks::mutator_lock_) {
    ClearAccessFlags(kAccNterpInvokeFastPathFlag);
  }

  static uint32_t ClearNterpFastPathFlags(uint32_t access_flags) {
    // `kAccNterpEntryPointFastPathFlag` has a different use for native methods.
    if (!IsNative(access_flags)) {
      access_flags &= ~kAccNterpEntryPointFastPathFlag;
    }
    access_flags &= ~kAccNterpInvokeFastPathFlag;
    return access_flags;
  }

  // Returns whether the method is a string constructor. The method must not
  // be a class initializer. (Class initializers are called from a different
  // context where we do not need to check for string constructors.)
  bool IsStringConstructor() REQUIRES_SHARED(Locks::mutator_lock_);

  // Returns true if this method could be overridden by a default method.
  bool IsOverridableByDefaultMethod() REQUIRES_SHARED(Locks::mutator_lock_);

  bool CheckIncompatibleClassChange(InvokeType type) REQUIRES_SHARED(Locks::mutator_lock_);

  // Throws the error that would result from trying to invoke this method (i.e.
  // IncompatibleClassChangeError, AbstractMethodError, or IllegalAccessError).
  // Only call if !IsInvokable();
  void ThrowInvocationTimeError(ObjPtr<mirror::Object> receiver)
      REQUIRES_SHARED(Locks::mutator_lock_);

  uint16_t GetMethodIndex() REQUIRES_SHARED(Locks::mutator_lock_);

  // Doesn't do erroneous / unresolved class checks.
  uint16_t GetMethodIndexDuringLinking() REQUIRES_SHARED(Locks::mutator_lock_);

  size_t GetVtableIndex() REQUIRES_SHARED(Locks::mutator_lock_) {
    return GetMethodIndex();
  }

  void SetMethodIndex(uint16_t new_method_index) REQUIRES_SHARED(Locks::mutator_lock_) {
    // Not called within a transaction.
    method_index_ = new_method_index;
  }

  static constexpr MemberOffset DexMethodIndexOffset() {
    return MemberOffset(OFFSETOF_MEMBER(ArtMethod, dex_method_index_));
  }

  static constexpr MemberOffset MethodIndexOffset() {
    return MemberOffset(OFFSETOF_MEMBER(ArtMethod, method_index_));
  }

  static constexpr MemberOffset ImtIndexOffset() {
    return MemberOffset(OFFSETOF_MEMBER(ArtMethod, imt_index_));
  }

  // Number of 32bit registers that would be required to hold all the arguments
  static size_t NumArgRegisters(std::string_view shorty);

  ALWAYS_INLINE uint32_t GetDexMethodIndex() const {
    return dex_method_index_;
  }

  void SetDexMethodIndex(uint32_t new_idx) REQUIRES_SHARED(Locks::mutator_lock_) {
    // Not called within a transaction.
    dex_method_index_ = new_idx;
  }

  // Lookup the Class from the type index into this method's dex cache.
  ObjPtr<mirror::Class> LookupResolvedClassFromTypeIndex(dex::TypeIndex type_idx)
      REQUIRES_SHARED(Locks::mutator_lock_);
  // Resolve the Class from the type index into this method's dex cache.
  ObjPtr<mirror::Class> ResolveClassFromTypeIndex(dex::TypeIndex type_idx)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Returns true if this method has the same name and signature of the other method.
  bool HasSameNameAndSignature(ArtMethod* other) REQUIRES_SHARED(Locks::mutator_lock_);

  // Find the method that this method overrides.
  ArtMethod* FindOverriddenMethod(PointerSize pointer_size)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Find the method index for this method within other_dexfile. If this method isn't present then
  // return dex::kDexNoIndex. The name_and_signature_idx MUST refer to a MethodId with the same
  // name and signature in the other_dexfile, such as the method index used to resolve this method
  // in the other_dexfile.
  uint32_t FindDexMethodIndexInOtherDexFile(const DexFile& other_dexfile,
                                            uint32_t name_and_signature_idx)
      REQUIRES_SHARED(Locks::mutator_lock_);

  void Invoke(Thread* self, uint32_t* args, uint32_t args_size, JValue* result, const char* shorty)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template <char ReturnType, char... ArgType>
  typename detail::ShortyTraits<ReturnType>::Type
  InvokeStatic(Thread* self, typename detail::ShortyTraits<ArgType>::Type... args)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template <char ReturnType, char... ArgType>
  typename detail::ShortyTraits<ReturnType>::Type
  InvokeInstance(Thread* self,
                 ObjPtr<mirror::Object> receiver,
                 typename detail::ShortyTraits<ArgType>::Type... args)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template <char ReturnType, char... ArgType>
  typename detail::ShortyTraits<ReturnType>::Type
  InvokeFinal(Thread* self,
              ObjPtr<mirror::Object> receiver,
              typename detail::ShortyTraits<ArgType>::Type... args)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template <char ReturnType, char... ArgType>
  typename detail::ShortyTraits<ReturnType>::Type
  InvokeVirtual(Thread* self,
                ObjPtr<mirror::Object> receiver,
                typename detail::ShortyTraits<ArgType>::Type... args)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template <char ReturnType, char... ArgType>
  typename detail::ShortyTraits<ReturnType>::Type
  InvokeInterface(Thread* self,
                  ObjPtr<mirror::Object> receiver,
                  typename detail::ShortyTraits<ArgType>::Type... args)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template <char... ArgType, typename HandleScopeType>
  Handle<mirror::Object> NewObject(HandleScopeType& hs,
                                   Thread* self,
                                   typename detail::HandleShortyTraits<ArgType>::Type... args)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template <char... ArgType>
  ObjPtr<mirror::Object> NewObject(Thread* self,
                                   typename detail::HandleShortyTraits<ArgType>::Type... args)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Returns true if the method needs a class initialization check according to access flags.
  // Only static methods other than the class initializer need this check.
  // The caller is responsible for performing the actual check.
  bool NeedsClinitCheckBeforeCall() const {
    return NeedsClinitCheckBeforeCall(GetAccessFlags());
  }

  static bool NeedsClinitCheckBeforeCall(uint32_t access_flags) {
    // The class initializer is special as it is invoked during initialization
    // and does not need the check.
    return IsStatic(access_flags) && !IsConstructor(access_flags);
  }

  // Check if the method needs a class initialization check before call
  // and its declaring class is not yet visibly initialized.
  // (The class needs to be visibly initialized before we can use entrypoints
  // to compiled code for static methods. See b/18161648 .)
  template <ReadBarrierOption kReadBarrierOption = kWithReadBarrier>
  bool StillNeedsClinitCheck() REQUIRES_SHARED(Locks::mutator_lock_);

  // Similar to `StillNeedsClinitCheck()` but the method's declaring class may
  // be dead but not yet reclaimed by the GC, so we cannot do a full read barrier
  // but we still want to check the class status in the to-space class if any.
  // Note: JIT can hold and use such methods during managed heap GC.
  bool StillNeedsClinitCheckMayBeDead() REQUIRES_SHARED(Locks::mutator_lock_);

  // Check if the declaring class has been verified and look at the to-space
  // class object, if any, as in `StillNeedsClinitCheckMayBeDead()`.
  bool IsDeclaringClassVerifiedMayBeDead() REQUIRES_SHARED(Locks::mutator_lock_);

  const void* GetEntryPointFromQuickCompiledCode() const {
    return GetEntryPointFromQuickCompiledCodePtrSize(kRuntimePointerSize);
  }
  ALWAYS_INLINE
  const void* GetEntryPointFromQuickCompiledCodePtrSize(PointerSize pointer_size) const {
    return GetNativePointer<const void*>(
        EntryPointFromQuickCompiledCodeOffset(pointer_size), pointer_size);
  }

  void SetEntryPointFromQuickCompiledCode(const void* entry_point_from_quick_compiled_code)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    SetEntryPointFromQuickCompiledCodePtrSize(entry_point_from_quick_compiled_code,
                                              kRuntimePointerSize);
  }
  ALWAYS_INLINE void SetEntryPointFromQuickCompiledCodePtrSize(
      const void* entry_point_from_quick_compiled_code, PointerSize pointer_size)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    SetNativePointer(EntryPointFromQuickCompiledCodeOffset(pointer_size),
                     entry_point_from_quick_compiled_code,
                     pointer_size);
  }

  static constexpr MemberOffset DataOffset(PointerSize pointer_size) {
    return MemberOffset(PtrSizedFieldsOffset(pointer_size) + OFFSETOF_MEMBER(
        PtrSizedFields, data_) / sizeof(void*) * static_cast<size_t>(pointer_size));
  }

  static constexpr MemberOffset EntryPointFromJniOffset(PointerSize pointer_size) {
    return DataOffset(pointer_size);
  }

  static constexpr MemberOffset EntryPointFromQuickCompiledCodeOffset(PointerSize pointer_size) {
    return MemberOffset(PtrSizedFieldsOffset(pointer_size) + OFFSETOF_MEMBER(
        PtrSizedFields, entry_point_from_quick_compiled_code_) / sizeof(void*)
            * static_cast<size_t>(pointer_size));
  }

  ImtConflictTable* GetImtConflictTable(PointerSize pointer_size) const {
    DCHECK(IsRuntimeMethod());
    return reinterpret_cast<ImtConflictTable*>(GetDataPtrSize(pointer_size));
  }

  ALWAYS_INLINE void SetImtConflictTable(ImtConflictTable* table, PointerSize pointer_size)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK(IsRuntimeMethod());
    SetDataPtrSize(table, pointer_size);
  }

  ALWAYS_INLINE bool HasSingleImplementation() REQUIRES_SHARED(Locks::mutator_lock_);

  ALWAYS_INLINE void SetHasSingleImplementation(bool single_impl)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK(!IsIntrinsic()) << "conflict with intrinsic bits";
    if (single_impl) {
      AddAccessFlags(kAccSingleImplementation);
    } else {
      ClearAccessFlags(kAccSingleImplementation);
    }
  }

  ALWAYS_INLINE bool HasSingleImplementationFlag() const {
    return (GetAccessFlags() & kAccSingleImplementation) != 0;
  }

  static uint32_t SetHasSingleImplementation(uint32_t access_flags, bool single_impl) {
    DCHECK(!IsIntrinsic(access_flags)) << "conflict with intrinsic bits";
    if (single_impl) {
      return access_flags | kAccSingleImplementation;
    } else {
      return access_flags & ~kAccSingleImplementation;
    }
  }

  // Takes a method and returns a 'canonical' one if the method is default (and therefore
  // potentially copied from some other class). For example, this ensures that the debugger does not
  // get confused as to which method we are in.
  ArtMethod* GetCanonicalMethod(PointerSize pointer_size = kRuntimePointerSize)
      REQUIRES_SHARED(Locks::mutator_lock_);

  ArtMethod* GetSingleImplementation(PointerSize pointer_size);

  ALWAYS_INLINE void SetSingleImplementation(ArtMethod* method, PointerSize pointer_size)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK(!IsNative());
    // Non-abstract method's single implementation is just itself.
    DCHECK(IsAbstract());
    DCHECK(method == nullptr || method->IsInvokable());
    SetDataPtrSize(method, pointer_size);
  }

  void* GetEntryPointFromJni() const {
    DCHECK(IsNative());
    return GetEntryPointFromJniPtrSize(kRuntimePointerSize);
  }

  ALWAYS_INLINE void* GetEntryPointFromJniPtrSize(PointerSize pointer_size) const {
    return GetDataPtrSize(pointer_size);
  }

  void SetEntryPointFromJni(const void* entrypoint)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    // The resolution method also has a JNI entrypoint for direct calls from
    // compiled code to the JNI dlsym lookup stub for @CriticalNative.
    DCHECK(IsNative() || IsRuntimeMethod());
    SetEntryPointFromJniPtrSize(entrypoint, kRuntimePointerSize);
  }

  ALWAYS_INLINE void SetEntryPointFromJniPtrSize(const void* entrypoint, PointerSize pointer_size)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    SetDataPtrSize(entrypoint, pointer_size);
  }

  ALWAYS_INLINE void* GetDataPtrSize(PointerSize pointer_size) const {
    DCHECK(IsImagePointerSize(pointer_size));
    return GetNativePointer<void*>(DataOffset(pointer_size), pointer_size);
  }

  ALWAYS_INLINE void SetDataPtrSize(const void* data, PointerSize pointer_size)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK(IsImagePointerSize(pointer_size));
    SetNativePointer(DataOffset(pointer_size), data, pointer_size);
  }

  // Is this a CalleSaveMethod or ResolutionMethod and therefore doesn't adhere to normal
  // conventions for a method of managed code. Returns false for Proxy methods.
  ALWAYS_INLINE bool IsRuntimeMethod() const {
    return dex_method_index_ == kRuntimeMethodDexMethodIndex;
  }

  bool HasCodeItem() REQUIRES_SHARED(Locks::mutator_lock_) {
    return NeedsCodeItem(GetAccessFlags()) && !IsRuntimeMethod() && !IsProxyMethod();
  }

  static bool NeedsCodeItem(uint32_t access_flags) {
    return !IsNative(access_flags) &&
           !IsAbstract(access_flags) &&
           !IsDefaultConflicting(access_flags);
  }

  void SetCodeItem(const dex::CodeItem* code_item)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK(HasCodeItem());
    SetDataPtrSize(code_item, kRuntimePointerSize);
  }

  // Is this a hand crafted method used for something like describing callee saves?
  bool IsCalleeSaveMethod() REQUIRES_SHARED(Locks::mutator_lock_);

  bool IsResolutionMethod() REQUIRES_SHARED(Locks::mutator_lock_);

  bool IsImtUnimplementedMethod() REQUIRES_SHARED(Locks::mutator_lock_);

  // Find the catch block for the given exception type and dex_pc. When a catch block is found,
  // indicates whether the found catch block is responsible for clearing the exception or whether
  // a move-exception instruction is present.
  uint32_t FindCatchBlock(Handle<mirror::Class> exception_type, uint32_t dex_pc,
                          bool* has_no_move_exception)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // NO_THREAD_SAFETY_ANALYSIS since we don't know what the callback requires.
  template<ReadBarrierOption kReadBarrierOption = kWithReadBarrier,
           bool kVisitProxyMethod = true,
           typename RootVisitorType>
  void VisitRoots(RootVisitorType& visitor, PointerSize pointer_size) NO_THREAD_SAFETY_ANALYSIS;

  const DexFile* GetDexFile() REQUIRES_SHARED(Locks::mutator_lock_);

  const char* GetDeclaringClassDescriptor() REQUIRES_SHARED(Locks::mutator_lock_);
  std::string_view GetDeclaringClassDescriptorView() REQUIRES_SHARED(Locks::mutator_lock_);

  ALWAYS_INLINE const char* GetShorty() REQUIRES_SHARED(Locks::mutator_lock_);

  const char* GetShorty(uint32_t* out_length) REQUIRES_SHARED(Locks::mutator_lock_);

  std::string_view GetShortyView() REQUIRES_SHARED(Locks::mutator_lock_);

  const Signature GetSignature() REQUIRES_SHARED(Locks::mutator_lock_);

  ALWAYS_INLINE const char* GetName() REQUIRES_SHARED(Locks::mutator_lock_);

  ALWAYS_INLINE std::string_view GetNameView() REQUIRES_SHARED(Locks::mutator_lock_);

  ObjPtr<mirror::String> ResolveNameString() REQUIRES_SHARED(Locks::mutator_lock_);

  bool NameEquals(ObjPtr<mirror::String> name) REQUIRES_SHARED(Locks::mutator_lock_);

  const dex::CodeItem* GetCodeItem() REQUIRES_SHARED(Locks::mutator_lock_);

  int32_t GetLineNumFromDexPC(uint32_t dex_pc) REQUIRES_SHARED(Locks::mutator_lock_);

  const dex::ProtoId& GetPrototype() REQUIRES_SHARED(Locks::mutator_lock_);

  const dex::ProtoIndex GetProtoIndex() REQUIRES_SHARED(Locks::mutator_lock_);

  const dex::TypeList* GetParameterTypeList() REQUIRES_SHARED(Locks::mutator_lock_);

  const char* GetDeclaringClassSourceFile() REQUIRES_SHARED(Locks::mutator_lock_);

  uint16_t GetClassDefIndex() REQUIRES_SHARED(Locks::mutator_lock_);

  const dex::ClassDef& GetClassDef() REQUIRES_SHARED(Locks::mutator_lock_);

  ALWAYS_INLINE size_t GetNumberOfParameters() REQUIRES_SHARED(Locks::mutator_lock_);

  const char* GetReturnTypeDescriptor() REQUIRES_SHARED(Locks::mutator_lock_);
  std::string_view GetReturnTypeDescriptorView() REQUIRES_SHARED(Locks::mutator_lock_);

  ALWAYS_INLINE Primitive::Type GetReturnTypePrimitive() REQUIRES_SHARED(Locks::mutator_lock_);

  const char* GetTypeDescriptorFromTypeIdx(dex::TypeIndex type_idx)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Lookup return type.
  ObjPtr<mirror::Class> LookupResolvedReturnType() REQUIRES_SHARED(Locks::mutator_lock_);
  // Resolve return type. May cause thread suspension due to GetClassFromTypeIdx
  // calling ResolveType this caused a large number of bugs at call sites.
  ObjPtr<mirror::Class> ResolveReturnType() REQUIRES_SHARED(Locks::mutator_lock_);

  ObjPtr<mirror::ClassLoader> GetClassLoader() REQUIRES_SHARED(Locks::mutator_lock_);

  template <ReadBarrierOption kReadBarrierOption = kWithReadBarrier>
  ObjPtr<mirror::DexCache> GetDexCache() REQUIRES_SHARED(Locks::mutator_lock_);
  template <ReadBarrierOption kReadBarrierOption>
  ObjPtr<mirror::DexCache> GetObsoleteDexCache() REQUIRES_SHARED(Locks::mutator_lock_);

  ALWAYS_INLINE ArtMethod* GetInterfaceMethodForProxyUnchecked(PointerSize pointer_size)
      REQUIRES_SHARED(Locks::mutator_lock_);
  ALWAYS_INLINE ArtMethod* GetInterfaceMethodIfProxy(PointerSize pointer_size)
      REQUIRES_SHARED(Locks::mutator_lock_);

  ArtMethod* GetNonObsoleteMethod() REQUIRES_SHARED(Locks::mutator_lock_);

  // May cause thread suspension due to class resolution.
  bool EqualParameters(Handle<mirror::ObjectArray<mirror::Class>> params)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Size of an instance of this native class.
  static constexpr size_t Size(PointerSize pointer_size) {
    return PtrSizedFieldsOffset(pointer_size) +
        (sizeof(PtrSizedFields) / sizeof(void*)) * static_cast<size_t>(pointer_size);
  }

  // Alignment of an instance of this native class.
  static constexpr size_t Alignment(PointerSize pointer_size) {
    // The ArtMethod alignment is the same as image pointer size. This differs from
    // alignof(ArtMethod) if cross-compiling with pointer_size != sizeof(void*).
    return static_cast<size_t>(pointer_size);
  }

  void CopyFrom(ArtMethod* src, PointerSize image_pointer_size)
      REQUIRES_SHARED(Locks::mutator_lock_);

  ALWAYS_INLINE void ResetCounter(uint16_t new_value);
  ALWAYS_INLINE void UpdateCounter(int32_t new_samples);
  ALWAYS_INLINE void SetHotCounter();
  ALWAYS_INLINE bool CounterIsHot();
  ALWAYS_INLINE uint16_t GetCounter();
  ALWAYS_INLINE bool CounterHasChanged(uint16_t threshold);

  ALWAYS_INLINE static constexpr uint16_t MaxCounter() {
    return std::numeric_limits<decltype(hotness_count_)>::max();
  }

  ALWAYS_INLINE uint32_t GetImtIndex() REQUIRES_SHARED(Locks::mutator_lock_);

  void SetImtIndex(uint16_t imt_index) REQUIRES_SHARED(Locks::mutator_lock_) {
    imt_index_ = imt_index;
  }

  void SetHotnessCount(uint16_t hotness_count) REQUIRES_SHARED(Locks::mutator_lock_) {
    hotness_count_ = hotness_count;
  }

  static constexpr MemberOffset HotnessCountOffset() {
    return MemberOffset(OFFSETOF_MEMBER(ArtMethod, hotness_count_));
  }

  // Returns the method header for the compiled code containing 'pc'. Note that runtime
  // methods will return null for this method, as they are not oat based.
  const OatQuickMethodHeader* GetOatQuickMethodHeader(uintptr_t pc)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Get compiled code for the method, return null if no code exists.
  const void* GetOatMethodQuickCode(PointerSize pointer_size)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Returns a human-readable signature for 'm'. Something like "a.b.C.m" or
  // "a.b.C.m(II)V" (depending on the value of 'with_signature').
  static std::string PrettyMethod(ArtMethod* m, bool with_signature = true)
      REQUIRES_SHARED(Locks::mutator_lock_);
  std::string PrettyMethod(bool with_signature = true)
      REQUIRES_SHARED(Locks::mutator_lock_);
  // Returns the JNI native function name for the non-overloaded method 'm'.
  std::string JniShortName()
      REQUIRES_SHARED(Locks::mutator_lock_);
  // Returns the JNI native function name for the overloaded method 'm'.
  std::string JniLongName()
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Visit the individual members of an ArtMethod.  Used by imgdiag.
  // As imgdiag does not support mixing instruction sets or pointer sizes (e.g., using imgdiag32
  // to inspect 64-bit images, etc.), we can go beneath the accessors directly to the class members.
  template <typename VisitorFunc>
  void VisitMembers(VisitorFunc& visitor) REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK(IsImagePointerSize(kRuntimePointerSize));
    visitor(this, &declaring_class_, "declaring_class_");
    visitor(this, &access_flags_, "access_flags_");
    visitor(this, &dex_method_index_, "dex_method_index_");
    visitor(this, &method_index_, "method_index_");
    visitor(this, &hotness_count_, "hotness_count_");
    visitor(this, &ptr_sized_fields_.data_, "ptr_sized_fields_.data_");
    visitor(this,
            &ptr_sized_fields_.entry_point_from_quick_compiled_code_,
            "ptr_sized_fields_.entry_point_from_quick_compiled_code_");
  }

  // Returns the dex instructions of the code item for the art method. Returns an empty array for
  // the null code item case.
  ALWAYS_INLINE CodeItemInstructionAccessor DexInstructions()
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Returns the dex code item data section of the DexFile for the art method.
  ALWAYS_INLINE CodeItemDataAccessor DexInstructionData()
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Returns the dex code item debug info section of the DexFile for the art method.
  ALWAYS_INLINE CodeItemDebugInfoAccessor DexInstructionDebugInfo()
      REQUIRES_SHARED(Locks::mutator_lock_);

  GcRoot<mirror::Class>& DeclaringClassRoot() {
    return declaring_class_;
  }

 protected:
  // Field order required by test "ValidateFieldOrderOfJavaCppUnionClasses".
  // The class we are a part of.
  GcRoot<mirror::Class> declaring_class_;

  // Access flags; low 16 bits are defined by spec.
  // Getting and setting this flag needs to be atomic when concurrency is
  // possible, e.g. after this method's class is linked. Such as when setting
  // verifier flags and single-implementation flag.
  std::atomic<std::uint32_t> access_flags_;

  /* Dex file fields. The defining dex file is available via declaring_class_->dex_cache_ */

  // Index into method_ids of the dex file associated with this method.
  uint32_t dex_method_index_;

  /* End of dex file fields. */

  // Entry within a dispatch table for this method. For static/direct methods the index is into
  // the declaringClass.directMethods, for virtual methods the vtable and for interface methods the
  // interface's method array in `IfTable`s of implementing classes.
  uint16_t method_index_;

  union {
    // Non-abstract methods: The hotness we measure for this method. Not atomic,
    // as we allow missing increments: if the method is hot, we will see it eventually.
    uint16_t hotness_count_;
    // Abstract interface methods: IMT index.
    // Abstract class (non-interface) methods: Unused (zero-initialized).
    uint16_t imt_index_;
  };

  // Fake padding field gets inserted here.

  // Must be the last fields in the method.
  struct PtrSizedFields {
    // Depending on the method type, the data is
    //   - native method: pointer to the JNI function registered to this method
    //                    or a function to resolve the JNI function,
    //   - resolution method: pointer to a function to resolve the method and
    //                        the JNI function for @CriticalNative.
    //   - conflict method: ImtConflictTable,
    //   - abstract/interface method: the single-implementation if any,
    //   - proxy method: the original interface method or constructor,
    //   - default conflict method: null
    //   - other methods: during AOT the code item offset, at runtime a pointer
    //                    to the code item.
    void* data_;

    // Method dispatch from quick compiled code invokes this pointer which may cause bridging into
    // the interpreter.
    void* entry_point_from_quick_compiled_code_;
  } ptr_sized_fields_;

 private:
  uint16_t FindObsoleteDexClassDefIndex() REQUIRES_SHARED(Locks::mutator_lock_);

  static constexpr size_t PtrSizedFieldsOffset(PointerSize pointer_size) {
    // Round up to pointer size for padding field. Tested in art_method.cc.
    return RoundUp(offsetof(ArtMethod, hotness_count_) + sizeof(hotness_count_),
                   static_cast<size_t>(pointer_size));
  }

  // Compare given pointer size to the image pointer size.
  static bool IsImagePointerSize(PointerSize pointer_size);

  dex::TypeIndex GetReturnTypeIndex() REQUIRES_SHARED(Locks::mutator_lock_);

  template<typename T>
  ALWAYS_INLINE T GetNativePointer(MemberOffset offset, PointerSize pointer_size) const {
    static_assert(std::is_pointer<T>::value, "T must be a pointer type");
    const auto addr = reinterpret_cast<uintptr_t>(this) + offset.Uint32Value();
    if (pointer_size == PointerSize::k32) {
      return reinterpret_cast<T>(*reinterpret_cast<const uint32_t*>(addr));
    } else {
      auto v = *reinterpret_cast<const uint64_t*>(addr);
      return reinterpret_cast<T>(dchecked_integral_cast<uintptr_t>(v));
    }
  }

  template<typename T>
  ALWAYS_INLINE void SetNativePointer(MemberOffset offset, T new_value, PointerSize pointer_size)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    static_assert(std::is_pointer<T>::value, "T must be a pointer type");
    const auto addr = reinterpret_cast<uintptr_t>(this) + offset.Uint32Value();
    if (pointer_size == PointerSize::k32) {
      uintptr_t ptr = reinterpret_cast<uintptr_t>(new_value);
      *reinterpret_cast<uint32_t*>(addr) = dchecked_integral_cast<uint32_t>(ptr);
    } else {
      *reinterpret_cast<uint64_t*>(addr) = reinterpret_cast<uintptr_t>(new_value);
    }
  }

  static inline bool IsValidIntrinsicUpdate(uint32_t modifier) {
    return (((modifier & kAccIntrinsic) == kAccIntrinsic) &&
            ((modifier & ~(kAccIntrinsic | kAccIntrinsicBits)) == 0) &&
            ((modifier & kAccIntrinsicBits) != 0));  // b/228049006: ensure intrinsic is not `kNone`
  }

  static inline bool OverlapsIntrinsicBits(uint32_t modifier) {
    return (modifier & kAccIntrinsicBits) != 0;
  }

  // This setter guarantees atomicity.
  void AddAccessFlags(uint32_t flag) REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK_IMPLIES(IsIntrinsic(), !OverlapsIntrinsicBits(flag) || IsValidIntrinsicUpdate(flag));
    // None of the readers rely ordering.
    access_flags_.fetch_or(flag, std::memory_order_relaxed);
  }

  // This setter guarantees atomicity.
  void ClearAccessFlags(uint32_t flag) REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK_IMPLIES(IsIntrinsic(), !OverlapsIntrinsicBits(flag) || IsValidIntrinsicUpdate(flag));
    access_flags_.fetch_and(~flag, std::memory_order_relaxed);
  }

  // Helper method for checking the class status of a possibly dead declaring class.
  // See `StillNeedsClinitCheckMayBeDead()` and `IsDeclaringClassVerifierMayBeDead()`.
  ObjPtr<mirror::Class> GetDeclaringClassMayBeDead() REQUIRES_SHARED(Locks::mutator_lock_);

  // Used by GetName and GetNameView to share common code.
  const char* GetRuntimeMethodName() REQUIRES_SHARED(Locks::mutator_lock_);

  DISALLOW_COPY_AND_ASSIGN(ArtMethod);  // Need to use CopyFrom to deal with 32 vs 64 bits.
};

class MethodCallback {
 public:
  virtual ~MethodCallback() {}

  virtual void RegisterNativeMethod(ArtMethod* method,
                                    const void* original_implementation,
                                    /*out*/void** new_implementation)
      REQUIRES_SHARED(Locks::mutator_lock_) = 0;
};

}  // namespace art

#endif  // ART_RUNTIME_ART_METHOD_H_
