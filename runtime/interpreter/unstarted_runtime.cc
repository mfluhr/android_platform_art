/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "unstarted_runtime.h"

#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>

#include <atomic>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <locale>

#include "art_method-inl.h"
#include "base/casts.h"
#include "base/hash_map.h"
#include "base/macros.h"
#include "base/os.h"
#include "base/pointer_size.h"
#include "base/quasi_atomic.h"
#include "base/unix_file/fd_file.h"
#include "base/zip_archive.h"
#include "class_linker.h"
#include "common_throws.h"
#include "dex/descriptors_names.h"
#include "entrypoints/entrypoint_utils-inl.h"
#include "gc/reference_processor.h"
#include "handle_scope-inl.h"
#include "hidden_api.h"
#include "interpreter/interpreter_common.h"
#include "jvalue-inl.h"
#include "mirror/array-alloc-inl.h"
#include "mirror/array-inl.h"
#include "mirror/class-alloc-inl.h"
#include "mirror/class.h"
#include "mirror/executable-inl.h"
#include "mirror/field.h"
#include "mirror/method.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-alloc-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/string-alloc-inl.h"
#include "mirror/string-inl.h"
#include "nativehelper/scoped_local_ref.h"
#include "nth_caller_visitor.h"
#include "reflection.h"
#include "thread-inl.h"
#include "unstarted_runtime_list.h"
#include "well_known_classes-inl.h"

namespace art HIDDEN {
namespace interpreter {

using android::base::StringAppendV;
using android::base::StringPrintf;

static void AbortTransactionOrFail(Thread* self, const char* fmt, ...)
    __attribute__((__format__(__printf__, 2, 3)))
    REQUIRES_SHARED(Locks::mutator_lock_);

static void AbortTransactionOrFail(Thread* self, const char* fmt, ...) {
  va_list args;
  Runtime* runtime = Runtime::Current();
  if (runtime->IsActiveTransaction()) {
    va_start(args, fmt);
    runtime->GetClassLinker()->AbortTransactionV(self, fmt, args);
    va_end(args);
  } else {
    va_start(args, fmt);
    std::string msg;
    StringAppendV(&msg, fmt, args);
    va_end(args);
    LOG(FATAL) << "Trying to abort, but not in transaction mode: " << msg;
    UNREACHABLE();
  }
}

// Restricted support for character upper case / lower case. Only support ASCII, where
// it's easy. Abort the transaction otherwise.
static void CharacterLowerUpper(Thread* self,
                                ShadowFrame* shadow_frame,
                                JValue* result,
                                size_t arg_offset,
                                bool to_lower_case) REQUIRES_SHARED(Locks::mutator_lock_) {
  int32_t int_value = shadow_frame->GetVReg(arg_offset);

  // Only ASCII (7-bit).
  if (!isascii(int_value)) {
    AbortTransactionOrFail(self,
                           "Only support ASCII characters for toLowerCase/toUpperCase: %u",
                           int_value);
    return;
  }

  // Constructing a `std::locale("C")` is slow. Use an explicit calculation, compare in debug mode.
  int32_t masked_value = int_value & ~0x20;  // Clear bit distinguishing `A`..`Z` from `a`..`z`.
  bool is_ascii_letter = ('A' <= masked_value) && (masked_value <= 'Z');
  int32_t result_value = is_ascii_letter ? (masked_value | (to_lower_case ? 0x20 : 0)) : int_value;
  DCHECK_EQ(result_value,
            to_lower_case
                ? std::tolower(dchecked_integral_cast<char>(int_value), std::locale("C"))
                : std::toupper(dchecked_integral_cast<char>(int_value), std::locale("C")))
      << std::boolalpha << to_lower_case;
  result->SetI(result_value);
}

void UnstartedRuntime::UnstartedCharacterToLowerCase(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  CharacterLowerUpper(self, shadow_frame, result, arg_offset, true);
}

void UnstartedRuntime::UnstartedCharacterToUpperCase(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  CharacterLowerUpper(self, shadow_frame, result, arg_offset, false);
}

// Helper function to deal with class loading in an unstarted runtime.
static void UnstartedRuntimeFindClass(Thread* self,
                                      Handle<mirror::String> className,
                                      Handle<mirror::ClassLoader> class_loader,
                                      JValue* result,
                                      bool initialize_class)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  CHECK(className != nullptr);
  std::string descriptor = DotToDescriptor(className->ToModifiedUtf8());
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();

  ObjPtr<mirror::Class> found =
      class_linker->FindClass(self, descriptor.c_str(), descriptor.length(), class_loader);
  if (found != nullptr && !found->CheckIsVisibleWithTargetSdk(self)) {
    CHECK(self->IsExceptionPending());
    return;
  }
  if (found != nullptr && initialize_class) {
    StackHandleScope<1> hs(self);
    HandleWrapperObjPtr<mirror::Class> h_class = hs.NewHandleWrapper(&found);
    if (!class_linker->EnsureInitialized(self, h_class, true, true)) {
      CHECK(self->IsExceptionPending());
      return;
    }
  }
  result->SetL(found);
}

static inline bool PendingExceptionHasAbortDescriptor(Thread* self)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  DCHECK(self->IsExceptionPending());
  return self->GetException()->GetClass()->DescriptorEquals(kTransactionAbortErrorDescriptor);
}

// Common helper for class-loading cutouts in an unstarted runtime. We call Runtime methods that
// rely on Java code to wrap errors in the correct exception class (i.e., NoClassDefFoundError into
// ClassNotFoundException), so need to do the same. The only exception is if the exception is
// actually the transaction abort exception. This must not be wrapped, as it signals an
// initialization abort.
static void CheckExceptionGenerateClassNotFound(Thread* self)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (self->IsExceptionPending()) {
    Runtime* runtime = Runtime::Current();
    if (runtime->IsActiveTransaction()) {
      // The boot class path at run time may contain additional dex files with
      // the required class definition(s). We cannot throw a normal exception at
      // compile time because a class initializer could catch it and successfully
      // initialize a class differently than when executing at run time.
      // If we're not aborting the transaction yet, abort now. b/183691501
      if (!runtime->GetClassLinker()->IsTransactionAborted()) {
        DCHECK(!PendingExceptionHasAbortDescriptor(self));
        runtime->GetClassLinker()->AbortTransactionF(self, "ClassNotFoundException");
      } else {
        DCHECK(PendingExceptionHasAbortDescriptor(self))
            << self->GetException()->GetClass()->PrettyDescriptor();
      }
    } else {
      // If not in a transaction, it cannot be the transaction abort exception. Wrap it.
      DCHECK(!PendingExceptionHasAbortDescriptor(self));
      self->ThrowNewWrappedException("Ljava/lang/ClassNotFoundException;",
                                     "ClassNotFoundException");
    }
  }
}

static ObjPtr<mirror::String> GetClassName(Thread* self,
                                           ShadowFrame* shadow_frame,
                                           size_t arg_offset)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  mirror::Object* param = shadow_frame->GetVRegReference(arg_offset);
  if (param == nullptr) {
    AbortTransactionOrFail(self, "Null-pointer in Class.forName.");
    return nullptr;
  }
  return param->AsString();
}

static std::function<hiddenapi::AccessContext()> GetHiddenapiAccessContextFunction(
    ShadowFrame* frame) {
  return [=]() REQUIRES_SHARED(Locks::mutator_lock_) {
    return hiddenapi::AccessContext(frame->GetMethod()->GetDeclaringClass());
  };
}

template<typename T>
static ALWAYS_INLINE bool ShouldDenyAccessToMember(T* member, ShadowFrame* frame)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  // All uses in this file are from reflection
  constexpr hiddenapi::AccessMethod kAccessMethod = hiddenapi::AccessMethod::kReflection;
  return hiddenapi::ShouldDenyAccessToMember(member,
                                             GetHiddenapiAccessContextFunction(frame),
                                             kAccessMethod);
}

void UnstartedRuntime::UnstartedClassForNameCommon(Thread* self,
                                                   ShadowFrame* shadow_frame,
                                                   JValue* result,
                                                   size_t arg_offset,
                                                   bool long_form) {
  ObjPtr<mirror::String> class_name = GetClassName(self, shadow_frame, arg_offset);
  if (class_name == nullptr) {
    return;
  }
  bool initialize_class;
  ObjPtr<mirror::ClassLoader> class_loader;
  if (long_form) {
    initialize_class = shadow_frame->GetVReg(arg_offset + 1) != 0;
    class_loader =
        ObjPtr<mirror::ClassLoader>::DownCast(shadow_frame->GetVRegReference(arg_offset + 2));
  } else {
    initialize_class = true;
    // TODO: This is really only correct for the boot classpath, and for robustness we should
    //       check the caller.
    class_loader = nullptr;
  }

  if (class_loader != nullptr && !ClassLinker::IsBootClassLoader(class_loader)) {
    AbortTransactionOrFail(self,
                           "Only the boot classloader is supported: %s",
                           mirror::Object::PrettyTypeOf(class_loader).c_str());
    return;
  }

  StackHandleScope<1> hs(self);
  Handle<mirror::String> h_class_name(hs.NewHandle(class_name));
  UnstartedRuntimeFindClass(self,
                            h_class_name,
                            ScopedNullHandle<mirror::ClassLoader>(),
                            result,
                            initialize_class);
  CheckExceptionGenerateClassNotFound(self);
}

void UnstartedRuntime::UnstartedClassForName(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  UnstartedClassForNameCommon(self, shadow_frame, result, arg_offset, /*long_form=*/ false);
}

void UnstartedRuntime::UnstartedClassForNameLong(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  UnstartedClassForNameCommon(self, shadow_frame, result, arg_offset, /*long_form=*/ true);
}

void UnstartedRuntime::UnstartedClassGetPrimitiveClass(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  ObjPtr<mirror::String> class_name = GetClassName(self, shadow_frame, arg_offset);
  ObjPtr<mirror::Class> klass = mirror::Class::GetPrimitiveClass(class_name);
  if (UNLIKELY(klass == nullptr)) {
    DCHECK(self->IsExceptionPending());
    AbortTransactionOrFail(self,
                           "Class.getPrimitiveClass() failed: %s",
                           self->GetException()->GetDetailMessage()->ToModifiedUtf8().c_str());
    return;
  }
  result->SetL(klass);
}

void UnstartedRuntime::UnstartedClassClassForName(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  UnstartedClassForNameCommon(self, shadow_frame, result, arg_offset, /*long_form=*/ true);
}

void UnstartedRuntime::UnstartedClassNewInstance(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  StackHandleScope<2> hs(self);  // Class, constructor, object.
  mirror::Object* param = shadow_frame->GetVRegReference(arg_offset);
  if (param == nullptr) {
    AbortTransactionOrFail(self, "Null-pointer in Class.newInstance.");
    return;
  }
  Handle<mirror::Class> h_klass(hs.NewHandle(param->AsClass()));

  // Check that it's not null.
  if (h_klass == nullptr) {
    AbortTransactionOrFail(self, "Class reference is null for newInstance");
    return;
  }

  // If we're in a transaction, class must not be finalizable (it or a superclass has a finalizer).
  Runtime* runtime = Runtime::Current();
  if (runtime->IsActiveTransaction() &&
      runtime->GetClassLinker()->TransactionAllocationConstraint(self, h_klass.Get())) {
    DCHECK(self->IsExceptionPending());
    return;
  }

  // There are two situations in which we'll abort this run.
  //  1) If the class isn't yet initialized and initialization fails.
  //  2) If we can't find the default constructor. We'll postpone the exception to runtime.
  // Note that 2) could likely be handled here, but for safety abort the transaction.
  bool ok = false;
  auto* cl = runtime->GetClassLinker();
  if (cl->EnsureInitialized(self, h_klass, true, true)) {
    ArtMethod* cons = h_klass->FindConstructor("()V", cl->GetImagePointerSize());
    if (cons != nullptr && ShouldDenyAccessToMember(cons, shadow_frame)) {
      cons = nullptr;
    }
    if (cons != nullptr) {
      Handle<mirror::Object> h_obj(hs.NewHandle(h_klass->AllocObject(self)));
      CHECK(h_obj != nullptr);  // We don't expect OOM at compile-time.
      EnterInterpreterFromInvoke(self, cons, h_obj.Get(), nullptr, nullptr);
      if (!self->IsExceptionPending()) {
        result->SetL(h_obj.Get());
        ok = true;
      }
    } else {
      self->ThrowNewExceptionF("Ljava/lang/InternalError;",
                               "Could not find default constructor for '%s'",
                               h_klass->PrettyClass().c_str());
    }
  }
  if (!ok) {
    AbortTransactionOrFail(self, "Failed in Class.newInstance for '%s' with %s",
                           h_klass->PrettyClass().c_str(),
                           mirror::Object::PrettyTypeOf(self->GetException()).c_str());
  }
}

void UnstartedRuntime::UnstartedClassGetDeclaredField(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  // Special managed code cut-out to allow field lookup in a un-started runtime that'd fail
  // going the reflective Dex way.
  ObjPtr<mirror::Class> klass = shadow_frame->GetVRegReference(arg_offset)->AsClass();
  ObjPtr<mirror::String> name2 = shadow_frame->GetVRegReference(arg_offset + 1)->AsString();
  ArtField* found = nullptr;
  for (ArtField& field : klass->GetFields()) {
    if (name2->Equals(field.GetName())) {
      found = &field;
      break;
    }
  }
  if (found != nullptr && ShouldDenyAccessToMember(found, shadow_frame)) {
    found = nullptr;
  }
  if (found == nullptr) {
    AbortTransactionOrFail(self, "Failed to find field in Class.getDeclaredField in un-started "
                           " runtime. name=%s class=%s", name2->ToModifiedUtf8().c_str(),
                           klass->PrettyDescriptor().c_str());
    return;
  }
  ObjPtr<mirror::Field> field = mirror::Field::CreateFromArtField(self, found, true);
  result->SetL(field);
}

void UnstartedRuntime::UnstartedClassGetDeclaredFields(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  // Special managed code cut-out to allow field lookup in a un-started runtime that'd fail
  // going the reflective Dex way.
  ObjPtr<mirror::Class> klass = shadow_frame->GetVRegReference(arg_offset)->AsClass();
  auto object_array = klass->GetDeclaredFields(self,
                                               /*public_only=*/ false,
                                               /*force_resolve=*/ true);
  if (object_array != nullptr) {
    result->SetL(object_array);
  }
}

void UnstartedRuntime::UnstartedClassGetPublicDeclaredFields(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  ObjPtr<mirror::Class> klass = shadow_frame->GetVRegReference(arg_offset)->AsClass();
  auto object_array = klass->GetDeclaredFields(self,
                                               /*public_only=*/ true,
                                               /*force_resolve=*/ true);
  if (object_array != nullptr) {
    result->SetL(object_array);
  }
}

// This is required for Enum(Set) code, as that uses reflection to inspect enum classes.
void UnstartedRuntime::UnstartedClassGetDeclaredMethod(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  // Special managed code cut-out to allow method lookup in a un-started runtime.
  ObjPtr<mirror::Class> klass = shadow_frame->GetVRegReference(arg_offset)->AsClass();
  if (klass == nullptr) {
    ThrowNullPointerExceptionForMethodAccess(shadow_frame->GetMethod(), InvokeType::kVirtual);
    return;
  }
  ObjPtr<mirror::String> name = shadow_frame->GetVRegReference(arg_offset + 1)->AsString();
  ObjPtr<mirror::ObjectArray<mirror::Class>> args =
      shadow_frame->GetVRegReference(arg_offset + 2)->AsObjectArray<mirror::Class>();
  PointerSize pointer_size = Runtime::Current()->GetClassLinker()->GetImagePointerSize();
  auto fn_hiddenapi_access_context = GetHiddenapiAccessContextFunction(shadow_frame);
  ObjPtr<mirror::Method> method = (pointer_size == PointerSize::k64)
      ? mirror::Class::GetDeclaredMethodInternal<PointerSize::k64>(
            self, klass, name, args, fn_hiddenapi_access_context)
      : mirror::Class::GetDeclaredMethodInternal<PointerSize::k32>(
            self, klass, name, args, fn_hiddenapi_access_context);
  if (method != nullptr && ShouldDenyAccessToMember(method->GetArtMethod(), shadow_frame)) {
    method = nullptr;
  }
  result->SetL(method);
}

// Special managed code cut-out to allow constructor lookup in a un-started runtime.
void UnstartedRuntime::UnstartedClassGetDeclaredConstructor(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  ObjPtr<mirror::Class> klass = shadow_frame->GetVRegReference(arg_offset)->AsClass();
  if (klass == nullptr) {
    ThrowNullPointerExceptionForMethodAccess(shadow_frame->GetMethod(), InvokeType::kVirtual);
    return;
  }
  ObjPtr<mirror::ObjectArray<mirror::Class>> args =
      shadow_frame->GetVRegReference(arg_offset + 1)->AsObjectArray<mirror::Class>();
  PointerSize pointer_size = Runtime::Current()->GetClassLinker()->GetImagePointerSize();
  ObjPtr<mirror::Constructor> constructor = (pointer_size == PointerSize::k64)
      ? mirror::Class::GetDeclaredConstructorInternal<PointerSize::k64>(self, klass, args)
      : mirror::Class::GetDeclaredConstructorInternal<PointerSize::k32>(self, klass, args);
  if (constructor != nullptr &&
      ShouldDenyAccessToMember(constructor->GetArtMethod(), shadow_frame)) {
    constructor = nullptr;
  }
  result->SetL(constructor);
}

void UnstartedRuntime::UnstartedClassGetDeclaringClass(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  StackHandleScope<1> hs(self);
  Handle<mirror::Class> klass(hs.NewHandle(
      reinterpret_cast<mirror::Class*>(shadow_frame->GetVRegReference(arg_offset))));
  if (klass->IsProxyClass() || klass->GetDexCache() == nullptr) {
    result->SetL(nullptr);
    return;
  }
  // Return null for anonymous classes.
  JValue is_anon_result;
  UnstartedClassIsAnonymousClass(self, shadow_frame, &is_anon_result, arg_offset);
  if (is_anon_result.GetZ() != 0) {
    result->SetL(nullptr);
    return;
  }
  result->SetL(annotations::GetDeclaringClass(klass));
}

void UnstartedRuntime::UnstartedClassGetEnclosingClass(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  StackHandleScope<1> hs(self);
  Handle<mirror::Class> klass(hs.NewHandle(shadow_frame->GetVRegReference(arg_offset)->AsClass()));
  if (klass->IsProxyClass() || klass->GetDexCache() == nullptr) {
    result->SetL(nullptr);
    return;
  }
  result->SetL(annotations::GetEnclosingClass(klass));
}

void UnstartedRuntime::UnstartedClassGetInnerClassFlags(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  StackHandleScope<1> hs(self);
  Handle<mirror::Class> klass(hs.NewHandle(
      reinterpret_cast<mirror::Class*>(shadow_frame->GetVRegReference(arg_offset))));
  const int32_t default_value = shadow_frame->GetVReg(arg_offset + 1);
  result->SetI(mirror::Class::GetInnerClassFlags(klass, default_value));
}

void UnstartedRuntime::UnstartedClassGetSignatureAnnotation(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  StackHandleScope<1> hs(self);
  Handle<mirror::Class> klass(hs.NewHandle(
      reinterpret_cast<mirror::Class*>(shadow_frame->GetVRegReference(arg_offset))));

  if (klass->IsProxyClass() || klass->GetDexCache() == nullptr) {
    result->SetL(nullptr);
    return;
  }

  result->SetL(annotations::GetSignatureAnnotationForClass(klass));
}

void UnstartedRuntime::UnstartedClassIsAnonymousClass(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  StackHandleScope<1> hs(self);
  Handle<mirror::Class> klass(hs.NewHandle(
      reinterpret_cast<mirror::Class*>(shadow_frame->GetVRegReference(arg_offset))));
  if (klass->IsProxyClass() || klass->GetDexCache() == nullptr) {
    result->SetZ(false);
    return;
  }
  ObjPtr<mirror::String> class_name = nullptr;
  if (!annotations::GetInnerClass(klass, &class_name)) {
    result->SetZ(false);
    return;
  }
  result->SetZ(class_name == nullptr);
}

static MemMap FindAndExtractEntry(const std::string& bcp_jar_file,
                                  int jar_fd,
                                  const char* entry_name,
                                  size_t* size,
                                  std::string* error_msg) {
  CHECK(size != nullptr);

  std::unique_ptr<ZipArchive> zip_archive;
  if (jar_fd >= 0) {
    zip_archive.reset(ZipArchive::OpenFromOwnedFd(jar_fd, bcp_jar_file.c_str(), error_msg));
  } else {
    zip_archive.reset(ZipArchive::Open(bcp_jar_file.c_str(), error_msg));
  }
  if (zip_archive == nullptr) {
    return MemMap::Invalid();
  }
  std::unique_ptr<ZipEntry> zip_entry(zip_archive->Find(entry_name, error_msg));
  if (zip_entry == nullptr) {
    return MemMap::Invalid();
  }
  MemMap tmp_map = zip_entry->ExtractToMemMap(bcp_jar_file.c_str(), entry_name, error_msg);
  if (!tmp_map.IsValid()) {
    return MemMap::Invalid();
  }

  // OK, from here everything seems fine.
  *size = zip_entry->GetUncompressedLength();
  return tmp_map;
}

static void GetResourceAsStream(Thread* self,
                                ShadowFrame* shadow_frame,
                                JValue* result,
                                size_t arg_offset) REQUIRES_SHARED(Locks::mutator_lock_) {
  mirror::Object* resource_obj = shadow_frame->GetVRegReference(arg_offset + 1);
  if (resource_obj == nullptr) {
    AbortTransactionOrFail(self, "null name for getResourceAsStream");
    return;
  }
  CHECK(resource_obj->IsString());
  ObjPtr<mirror::String> resource_name = resource_obj->AsString();

  std::string resource_name_str = resource_name->ToModifiedUtf8();
  if (resource_name_str.empty() || resource_name_str == "/") {
    AbortTransactionOrFail(self,
                           "Unsupported name %s for getResourceAsStream",
                           resource_name_str.c_str());
    return;
  }
  const char* resource_cstr = resource_name_str.c_str();
  if (resource_cstr[0] == '/') {
    resource_cstr++;
  }

  Runtime* runtime = Runtime::Current();

  const std::vector<std::string>& boot_class_path = Runtime::Current()->GetBootClassPath();
  if (boot_class_path.empty()) {
    AbortTransactionOrFail(self, "Boot classpath not set");
    return;
  }

  ArrayRef<File> boot_class_path_files = Runtime::Current()->GetBootClassPathFiles();
  DCHECK(boot_class_path_files.empty() || boot_class_path_files.size() == boot_class_path.size());

  MemMap mem_map;
  size_t map_size;
  std::string last_error_msg;  // Only store the last message (we could concatenate).

  bool has_bcp_fds = !boot_class_path_files.empty();
  for (size_t i = 0; i < boot_class_path.size(); ++i) {
    const std::string& jar_file = boot_class_path[i];
    const int jar_fd = has_bcp_fds ? boot_class_path_files[i].Fd() : -1;
    mem_map = FindAndExtractEntry(jar_file, jar_fd, resource_cstr, &map_size, &last_error_msg);
    if (mem_map.IsValid()) {
      break;
    }
  }

  if (!mem_map.IsValid()) {
    // Didn't find it. There's a good chance this will be the same at runtime, but still
    // conservatively abort the transaction here.
    AbortTransactionOrFail(self,
                           "Could not find resource %s. Last error was %s.",
                           resource_name_str.c_str(),
                           last_error_msg.c_str());
    return;
  }

  StackHandleScope<3> hs(self);

  // Create byte array for content.
  Handle<mirror::ByteArray> h_array(hs.NewHandle(mirror::ByteArray::Alloc(self, map_size)));
  if (h_array == nullptr) {
    AbortTransactionOrFail(self, "Could not find/create byte array class");
    return;
  }
  // Copy in content.
  memcpy(h_array->GetData(), mem_map.Begin(), map_size);
  // Be proactive releasing memory.
  mem_map.Reset();

  // Create a ByteArrayInputStream.
  Handle<mirror::Class> h_class(hs.NewHandle(
      runtime->GetClassLinker()->FindSystemClass(self, "Ljava/io/ByteArrayInputStream;")));
  if (h_class == nullptr) {
    AbortTransactionOrFail(self, "Could not find ByteArrayInputStream class");
    return;
  }
  if (!runtime->GetClassLinker()->EnsureInitialized(self, h_class, true, true)) {
    AbortTransactionOrFail(self, "Could not initialize ByteArrayInputStream class");
    return;
  }

  Handle<mirror::Object> h_obj(hs.NewHandle(h_class->AllocObject(self)));
  if (h_obj == nullptr) {
    AbortTransactionOrFail(self, "Could not allocate ByteArrayInputStream object");
    return;
  }

  auto* cl = Runtime::Current()->GetClassLinker();
  ArtMethod* constructor = h_class->FindConstructor("([B)V", cl->GetImagePointerSize());
  if (constructor == nullptr) {
    AbortTransactionOrFail(self, "Could not find ByteArrayInputStream constructor");
    return;
  }

  uint32_t args[1];
  args[0] = reinterpret_cast32<uint32_t>(h_array.Get());
  EnterInterpreterFromInvoke(self, constructor, h_obj.Get(), args, nullptr);

  if (self->IsExceptionPending()) {
    AbortTransactionOrFail(self, "Could not run ByteArrayInputStream constructor");
    return;
  }

  result->SetL(h_obj.Get());
}

void UnstartedRuntime::UnstartedClassLoaderGetResourceAsStream(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  {
    mirror::Object* this_obj = shadow_frame->GetVRegReference(arg_offset);
    CHECK(this_obj != nullptr);
    CHECK(this_obj->IsClassLoader());

    StackHandleScope<1> hs(self);
    Handle<mirror::Class> this_classloader_class(hs.NewHandle(this_obj->GetClass()));

    if (WellKnownClasses::java_lang_BootClassLoader != this_classloader_class.Get()) {
      AbortTransactionOrFail(self,
                             "Unsupported classloader type %s for getResourceAsStream",
                             mirror::Class::PrettyClass(this_classloader_class.Get()).c_str());
      return;
    }
  }

  GetResourceAsStream(self, shadow_frame, result, arg_offset);
}

void UnstartedRuntime::UnstartedConstructorNewInstance0(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  // This is a cutdown version of java_lang_reflect_Constructor.cc's implementation.
  StackHandleScope<4> hs(self);
  Handle<mirror::Constructor> m = hs.NewHandle(
      reinterpret_cast<mirror::Constructor*>(shadow_frame->GetVRegReference(arg_offset)));
  Handle<mirror::ObjectArray<mirror::Object>> args = hs.NewHandle(
      reinterpret_cast<mirror::ObjectArray<mirror::Object>*>(
          shadow_frame->GetVRegReference(arg_offset + 1)));
  Handle<mirror::Class> c(hs.NewHandle(m->GetDeclaringClass()));
  if (UNLIKELY(c->IsAbstract())) {
    AbortTransactionOrFail(self, "Cannot handle abstract classes");
    return;
  }
  // Verify that we can access the class.
  if (!m->IsAccessible() && !c->IsPublic()) {
    // Go 2 frames back, this method is always called from newInstance0, which is called from
    // Constructor.newInstance(Object... args).
    ObjPtr<mirror::Class> caller = GetCallingClass(self, 2);
    // If caller is null, then we called from JNI, just avoid the check since JNI avoids most
    // access checks anyways. TODO: Investigate if this the correct behavior.
    if (caller != nullptr && !caller->CanAccess(c.Get())) {
      AbortTransactionOrFail(self, "Cannot access class");
      return;
    }
  }
  if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(self, c, true, true)) {
    DCHECK(self->IsExceptionPending());
    return;
  }
  if (c->IsClassClass()) {
    AbortTransactionOrFail(self, "new Class() is not supported");
    return;
  }

  // String constructor is replaced by a StringFactory method in InvokeMethod.
  if (c->IsStringClass()) {
    // We don't support strings.
    AbortTransactionOrFail(self, "String construction is not supported");
    return;
  }

  Handle<mirror::Object> receiver = hs.NewHandle(c->AllocObject(self));
  if (receiver == nullptr) {
    AbortTransactionOrFail(self, "Could not allocate");
    return;
  }

  // It's easier to use reflection to make the call, than create the uint32_t array.
  {
    ScopedObjectAccessUnchecked soa(self);
    ScopedLocalRef<jobject> method_ref(self->GetJniEnv(),
                                       soa.AddLocalReference<jobject>(m.Get()));
    ScopedLocalRef<jobject> object_ref(self->GetJniEnv(),
                                       soa.AddLocalReference<jobject>(receiver.Get()));
    ScopedLocalRef<jobject> args_ref(self->GetJniEnv(),
                                     soa.AddLocalReference<jobject>(args.Get()));
    PointerSize pointer_size = Runtime::Current()->GetClassLinker()->GetImagePointerSize();
    if (pointer_size == PointerSize::k64) {
      InvokeMethod<PointerSize::k64>(soa, method_ref.get(), object_ref.get(), args_ref.get(), 2);
    } else {
      InvokeMethod<PointerSize::k32>(soa, method_ref.get(), object_ref.get(), args_ref.get(), 2);
    }
  }
  if (self->IsExceptionPending()) {
    AbortTransactionOrFail(self, "Failed running constructor");
  } else {
    result->SetL(receiver.Get());
  }
}

void UnstartedRuntime::UnstartedJNIExecutableGetParameterTypesInternal(
    Thread* self, ArtMethod*, mirror::Object* receiver, uint32_t*, JValue* result) {
  StackHandleScope<3> hs(self);
  ScopedObjectAccessUnchecked soa(self);
  Handle<mirror::Executable> executable(hs.NewHandle(
      reinterpret_cast<mirror::Executable*>(receiver)));
  if (executable == nullptr) {
    AbortTransactionOrFail(self, "Receiver can't be null in GetParameterTypesInternal");
  }

  ArtMethod* method = executable->GetArtMethod();
  const dex::TypeList* params = method->GetParameterTypeList();
  if (params == nullptr) {
    result->SetL(nullptr);
    return;
  }

  const uint32_t num_params = params->Size();

  ObjPtr<mirror::Class> class_array_class = GetClassRoot<mirror::ObjectArray<mirror::Class>>();
  Handle<mirror::ObjectArray<mirror::Class>> ptypes = hs.NewHandle(
      mirror::ObjectArray<mirror::Class>::Alloc(soa.Self(), class_array_class, num_params));
  if (ptypes.IsNull()) {
    AbortTransactionOrFail(self, "Could not allocate array of mirror::Class");
    return;
  }

  MutableHandle<mirror::Class> param(hs.NewHandle<mirror::Class>(nullptr));
  for (uint32_t i = 0; i < num_params; ++i) {
    const dex::TypeIndex type_idx = params->GetTypeItem(i).type_idx_;
    param.Assign(Runtime::Current()->GetClassLinker()->ResolveType(type_idx, method));
    if (param.Get() == nullptr) {
      AbortTransactionOrFail(self, "Could not resolve type");
      return;
    }
    ptypes->SetWithoutChecks<false>(i, param.Get());
  }

  result->SetL(ptypes.Get());
}

void UnstartedRuntime::UnstartedVmClassLoaderFindLoadedClass(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  ObjPtr<mirror::String> class_name = shadow_frame->GetVRegReference(arg_offset + 1)->AsString();
  ObjPtr<mirror::ClassLoader> class_loader =
      ObjPtr<mirror::ClassLoader>::DownCast(shadow_frame->GetVRegReference(arg_offset));
  StackHandleScope<2> hs(self);
  Handle<mirror::String> h_class_name(hs.NewHandle(class_name));
  Handle<mirror::ClassLoader> h_class_loader(hs.NewHandle(class_loader));
  UnstartedRuntimeFindClass(self,
                            h_class_name,
                            h_class_loader,
                            result,
                            /*initialize_class=*/ false);
  // This might have an error pending. But semantics are to just return null.
  if (self->IsExceptionPending()) {
    Runtime* runtime = Runtime::Current();
    if (runtime->IsActiveTransaction()) {
      // If we're not aborting the transaction yet, abort now. b/183691501
      // See CheckExceptionGenerateClassNotFound() for more detailed explanation.
      if (!runtime->GetClassLinker()->IsTransactionAborted()) {
        DCHECK(!PendingExceptionHasAbortDescriptor(self));
        runtime->GetClassLinker()->AbortTransactionF(self, "ClassNotFoundException");
      } else {
        DCHECK(PendingExceptionHasAbortDescriptor(self))
            << self->GetException()->GetClass()->PrettyDescriptor();
      }
    } else {
      // If not in a transaction, it cannot be the transaction abort exception. Clear it.
      DCHECK(!PendingExceptionHasAbortDescriptor(self));
      self->ClearException();
    }
  }
}

// Arraycopy emulation.
// Note: we can't use any fast copy functions, as they are not available under transaction.

template <typename T>
static void PrimitiveArrayCopy(Thread* self,
                               ObjPtr<mirror::Array> src_array,
                               int32_t src_pos,
                               ObjPtr<mirror::Array> dst_array,
                               int32_t dst_pos,
                               int32_t length)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (src_array->GetClass()->GetComponentType() != dst_array->GetClass()->GetComponentType()) {
    AbortTransactionOrFail(self,
                           "Types mismatched in arraycopy: %s vs %s.",
                           mirror::Class::PrettyDescriptor(
                               src_array->GetClass()->GetComponentType()).c_str(),
                           mirror::Class::PrettyDescriptor(
                               dst_array->GetClass()->GetComponentType()).c_str());
    return;
  }
  ObjPtr<mirror::PrimitiveArray<T>> src = ObjPtr<mirror::PrimitiveArray<T>>::DownCast(src_array);
  ObjPtr<mirror::PrimitiveArray<T>> dst = ObjPtr<mirror::PrimitiveArray<T>>::DownCast(dst_array);
  const bool copy_forward = (dst_pos < src_pos) || (dst_pos - src_pos >= length);
  if (copy_forward) {
    for (int32_t i = 0; i < length; ++i) {
      dst->Set(dst_pos + i, src->Get(src_pos + i));
    }
  } else {
    for (int32_t i = 1; i <= length; ++i) {
      dst->Set(dst_pos + length - i, src->Get(src_pos + length - i));
    }
  }
}

void UnstartedRuntime::UnstartedSystemArraycopy(Thread* self,
                                                ShadowFrame* shadow_frame,
                                                [[maybe_unused]] JValue* result,
                                                size_t arg_offset) {
  // Special case array copying without initializing System.
  jint src_pos = shadow_frame->GetVReg(arg_offset + 1);
  jint dst_pos = shadow_frame->GetVReg(arg_offset + 3);
  jint length = shadow_frame->GetVReg(arg_offset + 4);

  mirror::Object* src_obj = shadow_frame->GetVRegReference(arg_offset);
  mirror::Object* dst_obj = shadow_frame->GetVRegReference(arg_offset + 2);
  // Null checking. For simplicity, abort transaction.
  if (src_obj == nullptr) {
    AbortTransactionOrFail(self, "src is null in arraycopy.");
    return;
  }
  if (dst_obj == nullptr) {
    AbortTransactionOrFail(self, "dst is null in arraycopy.");
    return;
  }
  // Test for arrayness. Throw ArrayStoreException.
  if (!src_obj->IsArrayInstance() || !dst_obj->IsArrayInstance()) {
    self->ThrowNewException("Ljava/lang/ArrayStoreException;", "src or trg is not an array");
    return;
  }

  ObjPtr<mirror::Array> src_array = src_obj->AsArray();
  ObjPtr<mirror::Array> dst_array = dst_obj->AsArray();

  // Bounds checking. Throw IndexOutOfBoundsException.
  if (UNLIKELY(src_pos < 0) || UNLIKELY(dst_pos < 0) || UNLIKELY(length < 0) ||
      UNLIKELY(src_pos > src_array->GetLength() - length) ||
      UNLIKELY(dst_pos > dst_array->GetLength() - length)) {
    self->ThrowNewExceptionF("Ljava/lang/IndexOutOfBoundsException;",
                             "src.length=%d srcPos=%d dst.length=%d dstPos=%d length=%d",
                             src_array->GetLength(), src_pos, dst_array->GetLength(), dst_pos,
                             length);
    return;
  }

  Runtime* runtime = Runtime::Current();
  if (runtime->IsActiveTransaction() &&
      runtime->GetClassLinker()->TransactionWriteConstraint(self, dst_obj)) {
    DCHECK(self->IsExceptionPending());
    return;
  }

  // Type checking.
  ObjPtr<mirror::Class> src_type = shadow_frame->GetVRegReference(arg_offset)->GetClass()->
      GetComponentType();

  if (!src_type->IsPrimitive()) {
    // Check that the second type is not primitive.
    ObjPtr<mirror::Class> trg_type = shadow_frame->GetVRegReference(arg_offset + 2)->GetClass()->
        GetComponentType();
    if (trg_type->IsPrimitiveInt()) {
      AbortTransactionOrFail(self, "Type mismatch in arraycopy: %s vs %s",
                             mirror::Class::PrettyDescriptor(
                                 src_array->GetClass()->GetComponentType()).c_str(),
                             mirror::Class::PrettyDescriptor(
                                 dst_array->GetClass()->GetComponentType()).c_str());
      return;
    }

    ObjPtr<mirror::ObjectArray<mirror::Object>> src = src_array->AsObjectArray<mirror::Object>();
    ObjPtr<mirror::ObjectArray<mirror::Object>> dst = dst_array->AsObjectArray<mirror::Object>();
    if (src == dst) {
      // Can overlap, but not have type mismatches.
      // We cannot use ObjectArray::MemMove here, as it doesn't support transactions.
      const bool copy_forward = (dst_pos < src_pos) || (dst_pos - src_pos >= length);
      if (copy_forward) {
        for (int32_t i = 0; i < length; ++i) {
          dst->Set(dst_pos + i, src->Get(src_pos + i));
        }
      } else {
        for (int32_t i = 1; i <= length; ++i) {
          dst->Set(dst_pos + length - i, src->Get(src_pos + length - i));
        }
      }
    } else {
      // We're being lazy here. Optimally this could be a memcpy (if component types are
      // assignable), but the ObjectArray implementation doesn't support transactions. The
      // checking version, however, does.
      if (Runtime::Current()->IsActiveTransaction()) {
        dst->AssignableCheckingMemcpy<true>(
            dst_pos, src, src_pos, length, /* throw_exception= */ true);
      } else {
        dst->AssignableCheckingMemcpy<false>(
            dst_pos, src, src_pos, length, /* throw_exception= */ true);
      }
    }
  } else if (src_type->IsPrimitiveByte()) {
    PrimitiveArrayCopy<uint8_t>(self, src_array, src_pos, dst_array, dst_pos, length);
  } else if (src_type->IsPrimitiveChar()) {
    PrimitiveArrayCopy<uint16_t>(self, src_array, src_pos, dst_array, dst_pos, length);
  } else if (src_type->IsPrimitiveInt()) {
    PrimitiveArrayCopy<int32_t>(self, src_array, src_pos, dst_array, dst_pos, length);
  } else {
    AbortTransactionOrFail(self, "Unimplemented System.arraycopy for type '%s'",
                           src_type->PrettyDescriptor().c_str());
  }
}

void UnstartedRuntime::UnstartedSystemArraycopyByte(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  // Just forward.
  UnstartedRuntime::UnstartedSystemArraycopy(self, shadow_frame, result, arg_offset);
}

void UnstartedRuntime::UnstartedSystemArraycopyChar(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  // Just forward.
  UnstartedRuntime::UnstartedSystemArraycopy(self, shadow_frame, result, arg_offset);
}

void UnstartedRuntime::UnstartedSystemArraycopyInt(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  // Just forward.
  UnstartedRuntime::UnstartedSystemArraycopy(self, shadow_frame, result, arg_offset);
}

void UnstartedRuntime::UnstartedSystemGetSecurityManager([[maybe_unused]] Thread* self,
                                                         [[maybe_unused]] ShadowFrame* shadow_frame,
                                                         JValue* result,
                                                         [[maybe_unused]] size_t arg_offset) {
  result->SetL(nullptr);
}

static constexpr const char* kAndroidHardcodedSystemPropertiesFieldName = "STATIC_PROPERTIES";

static void GetSystemProperty(Thread* self,
                              ShadowFrame* shadow_frame,
                              JValue* result,
                              size_t arg_offset,
                              bool is_default_version)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  StackHandleScope<4> hs(self);
  Handle<mirror::String> h_key(
      hs.NewHandle(reinterpret_cast<mirror::String*>(shadow_frame->GetVRegReference(arg_offset))));
  if (h_key == nullptr) {
    AbortTransactionOrFail(self, "getProperty key was null");
    return;
  }

  // This is overall inefficient, but reflecting the values here is not great, either. So
  // for simplicity, and with the assumption that the number of getProperty calls is not
  // too great, just iterate each time.

  // Get the storage class.
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Handle<mirror::Class> h_props_class(hs.NewHandle(
      class_linker->FindSystemClass(self, "Ljava/lang/AndroidHardcodedSystemProperties;")));
  if (h_props_class == nullptr) {
    AbortTransactionOrFail(self, "Could not find AndroidHardcodedSystemProperties");
    return;
  }
  if (!class_linker->EnsureInitialized(self, h_props_class, true, true)) {
    AbortTransactionOrFail(self, "Could not initialize AndroidHardcodedSystemProperties");
    return;
  }

  // Get the storage array.
  ArtField* static_properties =
      h_props_class->FindDeclaredStaticField(kAndroidHardcodedSystemPropertiesFieldName,
                                             "[[Ljava/lang/String;");
  if (static_properties == nullptr) {
    AbortTransactionOrFail(self,
                           "Could not find %s field",
                           kAndroidHardcodedSystemPropertiesFieldName);
    return;
  }
  ObjPtr<mirror::Object> props = static_properties->GetObject(h_props_class.Get());
  Handle<mirror::ObjectArray<mirror::ObjectArray<mirror::String>>> h_2string_array(hs.NewHandle(
      props->AsObjectArray<mirror::ObjectArray<mirror::String>>()));
  if (h_2string_array == nullptr) {
    AbortTransactionOrFail(self, "Field %s is null", kAndroidHardcodedSystemPropertiesFieldName);
    return;
  }

  // Iterate over it.
  const int32_t prop_count = h_2string_array->GetLength();
  // Use the third handle as mutable.
  MutableHandle<mirror::ObjectArray<mirror::String>> h_string_array(
      hs.NewHandle<mirror::ObjectArray<mirror::String>>(nullptr));
  for (int32_t i = 0; i < prop_count; ++i) {
    h_string_array.Assign(h_2string_array->Get(i));
    if (h_string_array == nullptr ||
        h_string_array->GetLength() != 2 ||
        h_string_array->Get(0) == nullptr) {
      AbortTransactionOrFail(self,
                             "Unexpected content of %s",
                             kAndroidHardcodedSystemPropertiesFieldName);
      return;
    }
    if (h_key->Equals(h_string_array->Get(0))) {
      // Found a value.
      if (h_string_array->Get(1) == nullptr && is_default_version) {
        // Null is being delegated to the default map, and then resolved to the given default value.
        // As there's no default map, return the given value.
        result->SetL(shadow_frame->GetVRegReference(arg_offset + 1));
      } else {
        result->SetL(h_string_array->Get(1));
      }
      return;
    }
  }

  // Key is not supported.
  AbortTransactionOrFail(self, "getProperty key %s not supported", h_key->ToModifiedUtf8().c_str());
}

void UnstartedRuntime::UnstartedSystemGetProperty(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  GetSystemProperty(self, shadow_frame, result, arg_offset, false);
}

void UnstartedRuntime::UnstartedSystemGetPropertyWithDefault(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  GetSystemProperty(self, shadow_frame, result, arg_offset, true);
}

void UnstartedRuntime::UnstartedSystemNanoTime(Thread* self, ShadowFrame*, JValue*, size_t) {
  // We don't want `System.nanoTime` to be called at compile time because `java.util.Random`'s
  // default constructor uses `nanoTime` to initialize seed and having it set during compile time
  // makes that `java.util.Random` instance deterministic for given system image.
  AbortTransactionOrFail(self, "Should not be called by UnstartedRuntime");
}

static std::string GetImmediateCaller(ShadowFrame* shadow_frame)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (shadow_frame->GetLink() == nullptr) {
    return "<no caller>";
  }
  return ArtMethod::PrettyMethod(shadow_frame->GetLink()->GetMethod());
}

static bool CheckCallers(ShadowFrame* shadow_frame,
                         std::initializer_list<std::string> allowed_call_stack)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  for (const std::string& allowed_caller : allowed_call_stack) {
    if (shadow_frame->GetLink() == nullptr) {
      return false;
    }

    std::string found_caller = ArtMethod::PrettyMethod(shadow_frame->GetLink()->GetMethod());
    if (allowed_caller != found_caller) {
      return false;
    }

    shadow_frame = shadow_frame->GetLink();
  }
  return true;
}

static ObjPtr<mirror::Object> CreateInstanceOf(Thread* self, const char* class_descriptor)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  // Find the requested class.
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  ObjPtr<mirror::Class> klass = class_linker->FindSystemClass(self, class_descriptor);
  if (klass == nullptr) {
    AbortTransactionOrFail(self, "Could not load class %s", class_descriptor);
    return nullptr;
  }

  StackHandleScope<2> hs(self);
  Handle<mirror::Class> h_class(hs.NewHandle(klass));
  Handle<mirror::Object> h_obj(hs.NewHandle(h_class->AllocObject(self)));
  if (h_obj != nullptr) {
    ArtMethod* init_method = h_class->FindConstructor("()V", class_linker->GetImagePointerSize());
    if (init_method == nullptr) {
      AbortTransactionOrFail(self, "Could not find <init> for %s", class_descriptor);
      return nullptr;
    } else {
      JValue invoke_result;
      EnterInterpreterFromInvoke(self, init_method, h_obj.Get(), nullptr, nullptr);
      if (!self->IsExceptionPending()) {
        return h_obj.Get();
      }
      AbortTransactionOrFail(self, "Could not run <init> for %s", class_descriptor);
    }
  }
  AbortTransactionOrFail(self, "Could not allocate instance of %s", class_descriptor);
  return nullptr;
}

void UnstartedRuntime::UnstartedThreadLocalGet(Thread* self,
                                               ShadowFrame* shadow_frame,
                                               JValue* result,
                                               [[maybe_unused]] size_t arg_offset) {
  if (CheckCallers(shadow_frame,
                   { "jdk.internal.math.FloatingDecimal$BinaryToASCIIBuffer "
                         "jdk.internal.math.FloatingDecimal.getBinaryToASCIIBuffer()" })) {
    result->SetL(CreateInstanceOf(self, "Ljdk/internal/math/FloatingDecimal$BinaryToASCIIBuffer;"));
  } else {
    AbortTransactionOrFail(self,
                           "ThreadLocal.get() does not support %s",
                           GetImmediateCaller(shadow_frame).c_str());
  }
}

void UnstartedRuntime::UnstartedThreadCurrentThread(Thread* self,
                                                    ShadowFrame* shadow_frame,
                                                    JValue* result,
                                                    [[maybe_unused]] size_t arg_offset) {
  if (CheckCallers(shadow_frame,
                   { "void java.lang.Thread.<init>(java.lang.ThreadGroup, java.lang.Runnable, "
                         "java.lang.String, long, java.security.AccessControlContext, boolean)",
                     "void java.lang.Thread.<init>(java.lang.ThreadGroup, java.lang.Runnable, "
                         "java.lang.String, long)",
                     "void java.lang.Thread.<init>()",
                     "void java.util.logging.LogManager$Cleaner.<init>("
                         "java.util.logging.LogManager)" })) {
    // Allow list LogManager$Cleaner, which is an unstarted Thread (for a shutdown hook). The
    // Thread constructor only asks for the current thread to set up defaults and add the
    // thread as unstarted to the ThreadGroup. A faked-up main thread peer is good enough for
    // these purposes.
    Runtime::Current()->InitThreadGroups(self);
    ObjPtr<mirror::Object> main_peer = self->CreateCompileTimePeer(
        "main", /*as_daemon=*/ false, Runtime::Current()->GetMainThreadGroup());
    if (main_peer == nullptr) {
      AbortTransactionOrFail(self, "Failed allocating peer");
      return;
    }

    result->SetL(main_peer);
  } else {
    AbortTransactionOrFail(self,
                           "Thread.currentThread() does not support %s",
                           GetImmediateCaller(shadow_frame).c_str());
  }
}

void UnstartedRuntime::UnstartedThreadGetNativeState(Thread* self,
                                                     ShadowFrame* shadow_frame,
                                                     JValue* result,
                                                     [[maybe_unused]] size_t arg_offset) {
  if (CheckCallers(shadow_frame,
                   { "java.lang.Thread$State java.lang.Thread.getState()",
                     "java.lang.ThreadGroup java.lang.Thread.getThreadGroup()",
                     "void java.lang.Thread.<init>(java.lang.ThreadGroup, java.lang.Runnable, "
                         "java.lang.String, long, java.security.AccessControlContext, boolean)",
                     "void java.lang.Thread.<init>(java.lang.ThreadGroup, java.lang.Runnable, "
                         "java.lang.String, long)",
                     "void java.lang.Thread.<init>()",
                     "void java.util.logging.LogManager$Cleaner.<init>("
                         "java.util.logging.LogManager)" })) {
    // Allow list reading the state of the "main" thread when creating another (unstarted) thread
    // for LogManager. Report the thread as "new" (it really only counts that it isn't terminated).
    constexpr int32_t kJavaRunnable = 1;
    result->SetI(kJavaRunnable);
  } else {
    AbortTransactionOrFail(self,
                           "Thread.getNativeState() does not support %s",
                           GetImmediateCaller(shadow_frame).c_str());
  }
}

void UnstartedRuntime::UnstartedMathCeil([[maybe_unused]] Thread* self,
                                         ShadowFrame* shadow_frame,
                                         JValue* result,
                                         size_t arg_offset) {
  result->SetD(ceil(shadow_frame->GetVRegDouble(arg_offset)));
}

void UnstartedRuntime::UnstartedMathFloor([[maybe_unused]] Thread* self,
                                          ShadowFrame* shadow_frame,
                                          JValue* result,
                                          size_t arg_offset) {
  result->SetD(floor(shadow_frame->GetVRegDouble(arg_offset)));
}

void UnstartedRuntime::UnstartedMathSin([[maybe_unused]] Thread* self,
                                        ShadowFrame* shadow_frame,
                                        JValue* result,
                                        size_t arg_offset) {
  result->SetD(sin(shadow_frame->GetVRegDouble(arg_offset)));
}

void UnstartedRuntime::UnstartedMathCos([[maybe_unused]] Thread* self,
                                        ShadowFrame* shadow_frame,
                                        JValue* result,
                                        size_t arg_offset) {
  result->SetD(cos(shadow_frame->GetVRegDouble(arg_offset)));
}

void UnstartedRuntime::UnstartedMathPow([[maybe_unused]] Thread* self,
                                        ShadowFrame* shadow_frame,
                                        JValue* result,
                                        size_t arg_offset) {
  result->SetD(pow(shadow_frame->GetVRegDouble(arg_offset),
                   shadow_frame->GetVRegDouble(arg_offset + 2)));
}

void UnstartedRuntime::UnstartedMathTan([[maybe_unused]] Thread* self,
                                        ShadowFrame* shadow_frame,
                                        JValue* result,
                                        size_t arg_offset) {
  result->SetD(tan(shadow_frame->GetVRegDouble(arg_offset)));
}

void UnstartedRuntime::UnstartedObjectHashCode([[maybe_unused]] Thread* self,
                                               ShadowFrame* shadow_frame,
                                               JValue* result,
                                               size_t arg_offset) {
  mirror::Object* obj = shadow_frame->GetVRegReference(arg_offset);
  result->SetI(obj->IdentityHashCode());
}

void UnstartedRuntime::UnstartedDoubleDoubleToRawLongBits([[maybe_unused]] Thread* self,
                                                          ShadowFrame* shadow_frame,
                                                          JValue* result,
                                                          size_t arg_offset) {
  double in = shadow_frame->GetVRegDouble(arg_offset);
  result->SetJ(bit_cast<int64_t, double>(in));
}

static void UnstartedMemoryPeek(
    Primitive::Type type, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  int64_t address = shadow_frame->GetVRegLong(arg_offset);
  // TODO: Check that this is in the heap somewhere. Otherwise we will segfault instead of
  //       aborting the transaction.

  switch (type) {
    case Primitive::kPrimByte: {
      result->SetB(*reinterpret_cast<int8_t*>(static_cast<intptr_t>(address)));
      return;
    }

    case Primitive::kPrimShort: {
      using unaligned_short __attribute__((__aligned__(1))) = int16_t;
      result->SetS(*reinterpret_cast<unaligned_short*>(static_cast<intptr_t>(address)));
      return;
    }

    case Primitive::kPrimInt: {
      using unaligned_int __attribute__((__aligned__(1))) = int32_t;
      result->SetI(*reinterpret_cast<unaligned_int*>(static_cast<intptr_t>(address)));
      return;
    }

    case Primitive::kPrimLong: {
      using unaligned_long __attribute__((__aligned__(1))) = int64_t;
      result->SetJ(*reinterpret_cast<unaligned_long*>(static_cast<intptr_t>(address)));
      return;
    }

    case Primitive::kPrimBoolean:
    case Primitive::kPrimChar:
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
    case Primitive::kPrimVoid:
    case Primitive::kPrimNot:
      LOG(FATAL) << "Not in the Memory API: " << type;
      UNREACHABLE();
  }
  LOG(FATAL) << "Should not reach here";
  UNREACHABLE();
}

void UnstartedRuntime::UnstartedMemoryPeekByte([[maybe_unused]] Thread* self,
                                               ShadowFrame* shadow_frame,
                                               JValue* result,
                                               size_t arg_offset) {
  UnstartedMemoryPeek(Primitive::kPrimByte, shadow_frame, result, arg_offset);
}

void UnstartedRuntime::UnstartedMemoryPeekShort([[maybe_unused]] Thread* self,
                                                ShadowFrame* shadow_frame,
                                                JValue* result,
                                                size_t arg_offset) {
  UnstartedMemoryPeek(Primitive::kPrimShort, shadow_frame, result, arg_offset);
}

void UnstartedRuntime::UnstartedMemoryPeekInt([[maybe_unused]] Thread* self,
                                              ShadowFrame* shadow_frame,
                                              JValue* result,
                                              size_t arg_offset) {
  UnstartedMemoryPeek(Primitive::kPrimInt, shadow_frame, result, arg_offset);
}

void UnstartedRuntime::UnstartedMemoryPeekLong([[maybe_unused]] Thread* self,
                                               ShadowFrame* shadow_frame,
                                               JValue* result,
                                               size_t arg_offset) {
  UnstartedMemoryPeek(Primitive::kPrimLong, shadow_frame, result, arg_offset);
}

static void UnstartedMemoryPeekArray(
    Primitive::Type type, Thread* self, ShadowFrame* shadow_frame, size_t arg_offset)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  int64_t address_long = shadow_frame->GetVRegLong(arg_offset);
  mirror::Object* obj = shadow_frame->GetVRegReference(arg_offset + 2);
  if (obj == nullptr) {
    Runtime::Current()->GetClassLinker()->AbortTransactionF(self, "Null pointer in peekArray");
    return;
  }
  ObjPtr<mirror::Array> array = obj->AsArray();

  int offset = shadow_frame->GetVReg(arg_offset + 3);
  int count = shadow_frame->GetVReg(arg_offset + 4);
  if (offset < 0 || offset + count > array->GetLength()) {
    Runtime::Current()->GetClassLinker()->AbortTransactionF(
        self, "Array out of bounds in peekArray: %d/%d vs %d", offset, count, array->GetLength());
    return;
  }

  switch (type) {
    case Primitive::kPrimByte: {
      int8_t* address = reinterpret_cast<int8_t*>(static_cast<intptr_t>(address_long));
      ObjPtr<mirror::ByteArray> byte_array = array->AsByteArray();
      for (int32_t i = 0; i < count; ++i, ++address) {
        byte_array->SetWithoutChecks<true>(i + offset, *address);
      }
      return;
    }

    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimLong:
      LOG(FATAL) << "Type unimplemented for Memory Array API, should not reach here: " << type;
      UNREACHABLE();

    case Primitive::kPrimBoolean:
    case Primitive::kPrimChar:
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
    case Primitive::kPrimVoid:
    case Primitive::kPrimNot:
      LOG(FATAL) << "Not in the Memory API: " << type;
      UNREACHABLE();
  }
  LOG(FATAL) << "Should not reach here";
  UNREACHABLE();
}

void UnstartedRuntime::UnstartedMemoryPeekByteArray(Thread* self,
                                                    ShadowFrame* shadow_frame,
                                                    [[maybe_unused]] JValue* result,
                                                    size_t arg_offset) {
  UnstartedMemoryPeekArray(Primitive::kPrimByte, self, shadow_frame, arg_offset);
}

// This allows reading the new style of String objects during compilation.
void UnstartedRuntime::UnstartedStringGetCharsNoCheck(Thread* self,
                                                      ShadowFrame* shadow_frame,
                                                      [[maybe_unused]] JValue* result,
                                                      size_t arg_offset) {
  jint start = shadow_frame->GetVReg(arg_offset + 1);
  jint end = shadow_frame->GetVReg(arg_offset + 2);
  jint index = shadow_frame->GetVReg(arg_offset + 4);
  ObjPtr<mirror::String> string = shadow_frame->GetVRegReference(arg_offset)->AsString();
  if (string == nullptr) {
    AbortTransactionOrFail(self, "String.getCharsNoCheck with null object");
    return;
  }
  DCHECK_GE(start, 0);
  DCHECK_LE(start, end);
  DCHECK_LE(end, string->GetLength());
  StackHandleScope<1> hs(self);
  Handle<mirror::CharArray> h_char_array(
      hs.NewHandle(shadow_frame->GetVRegReference(arg_offset + 3)->AsCharArray()));
  DCHECK_GE(index, 0);
  DCHECK_LE(index, h_char_array->GetLength());
  DCHECK_LE(end - start, h_char_array->GetLength() - index);
  string->GetChars(start, end, h_char_array, index);
}

// This allows reading chars from the new style of String objects during compilation.
void UnstartedRuntime::UnstartedStringCharAt(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  jint index = shadow_frame->GetVReg(arg_offset + 1);
  ObjPtr<mirror::String> string = shadow_frame->GetVRegReference(arg_offset)->AsString();
  if (string == nullptr) {
    AbortTransactionOrFail(self, "String.charAt with null object");
    return;
  }
  result->SetC(string->CharAt(index));
}

// This allows creating String objects with replaced characters during compilation.
// String.doReplace(char, char) is called from String.replace(char, char) when there is a match.
void UnstartedRuntime::UnstartedStringDoReplace(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  jchar old_c = shadow_frame->GetVReg(arg_offset + 1);
  jchar new_c = shadow_frame->GetVReg(arg_offset + 2);
  StackHandleScope<1> hs(self);
  Handle<mirror::String> string =
      hs.NewHandle(shadow_frame->GetVRegReference(arg_offset)->AsString());
  if (string == nullptr) {
    AbortTransactionOrFail(self, "String.replaceWithMatch with null object");
    return;
  }
  result->SetL(mirror::String::DoReplace(self, string, old_c, new_c));
}

// This allows creating the new style of String objects during compilation.
void UnstartedRuntime::UnstartedStringFactoryNewStringFromBytes(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  jint high = shadow_frame->GetVReg(arg_offset + 1);
  jint offset = shadow_frame->GetVReg(arg_offset + 2);
  jint byte_count = shadow_frame->GetVReg(arg_offset + 3);
  DCHECK_GE(byte_count, 0);
  StackHandleScope<1> hs(self);
  Handle<mirror::ByteArray> h_byte_array(
      hs.NewHandle(shadow_frame->GetVRegReference(arg_offset)->AsByteArray()));
  Runtime* runtime = Runtime::Current();
  gc::AllocatorType allocator = runtime->GetHeap()->GetCurrentAllocator();
  result->SetL(
      mirror::String::AllocFromByteArray(self, byte_count, h_byte_array, offset, high, allocator));
}

// This allows creating the new style of String objects during compilation.
void UnstartedRuntime::UnstartedStringFactoryNewStringFromChars(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  jint offset = shadow_frame->GetVReg(arg_offset);
  jint char_count = shadow_frame->GetVReg(arg_offset + 1);
  DCHECK_GE(char_count, 0);
  StackHandleScope<1> hs(self);
  Handle<mirror::CharArray> h_char_array(
      hs.NewHandle(shadow_frame->GetVRegReference(arg_offset + 2)->AsCharArray()));
  Runtime* runtime = Runtime::Current();
  gc::AllocatorType allocator = runtime->GetHeap()->GetCurrentAllocator();
  result->SetL(
      mirror::String::AllocFromCharArray(self, char_count, h_char_array, offset, allocator));
}

// This allows creating the new style of String objects during compilation.
void UnstartedRuntime::UnstartedStringFactoryNewStringFromString(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  ObjPtr<mirror::String> to_copy = shadow_frame->GetVRegReference(arg_offset)->AsString();
  if (to_copy == nullptr) {
    AbortTransactionOrFail(self, "StringFactory.newStringFromString with null object");
    return;
  }
  StackHandleScope<1> hs(self);
  Handle<mirror::String> h_string(hs.NewHandle(to_copy));
  Runtime* runtime = Runtime::Current();
  gc::AllocatorType allocator = runtime->GetHeap()->GetCurrentAllocator();
  result->SetL(
      mirror::String::AllocFromString(self, h_string->GetLength(), h_string, 0, allocator));
}

void UnstartedRuntime::UnstartedStringFastSubstring(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  jint start = shadow_frame->GetVReg(arg_offset + 1);
  jint length = shadow_frame->GetVReg(arg_offset + 2);
  DCHECK_GE(start, 0);
  DCHECK_GE(length, 0);
  StackHandleScope<1> hs(self);
  Handle<mirror::String> h_string(
      hs.NewHandle(shadow_frame->GetVRegReference(arg_offset)->AsString()));
  DCHECK_LE(start, h_string->GetLength());
  DCHECK_LE(start + length, h_string->GetLength());
  Runtime* runtime = Runtime::Current();
  gc::AllocatorType allocator = runtime->GetHeap()->GetCurrentAllocator();
  result->SetL(mirror::String::AllocFromString(self, length, h_string, start, allocator));
}

// This allows getting the char array for new style of String objects during compilation.
void UnstartedRuntime::UnstartedStringToCharArray(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  StackHandleScope<1> hs(self);
  Handle<mirror::String> string =
      hs.NewHandle(shadow_frame->GetVRegReference(arg_offset)->AsString());
  if (string == nullptr) {
    AbortTransactionOrFail(self, "String.charAt with null object");
    return;
  }
  result->SetL(mirror::String::ToCharArray(string, self));
}

// This allows statically initializing ConcurrentHashMap and SynchronousQueue.
void UnstartedRuntime::UnstartedReferenceGetReferent(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  const ObjPtr<mirror::Reference> ref = ObjPtr<mirror::Reference>::DownCast(
      shadow_frame->GetVRegReference(arg_offset));
  if (ref == nullptr) {
    AbortTransactionOrFail(self, "Reference.getReferent() with null object");
    return;
  }
  const ObjPtr<mirror::Object> referent =
      Runtime::Current()->GetHeap()->GetReferenceProcessor()->GetReferent(self, ref);
  result->SetL(referent);
}

void UnstartedRuntime::UnstartedReferenceRefersTo(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  // Use the naive implementation that may block and needlessly extend the lifetime
  // of the referenced object.
  const ObjPtr<mirror::Reference> ref = ObjPtr<mirror::Reference>::DownCast(
      shadow_frame->GetVRegReference(arg_offset));
  if (ref == nullptr) {
    AbortTransactionOrFail(self, "Reference.refersTo() with null object");
    return;
  }
  const ObjPtr<mirror::Object> referent =
      Runtime::Current()->GetHeap()->GetReferenceProcessor()->GetReferent(self, ref);
  const ObjPtr<mirror::Object> o = shadow_frame->GetVRegReference(arg_offset + 1);
  result->SetZ(o == referent);
}

// This allows statically initializing ConcurrentHashMap and SynchronousQueue. We use a somewhat
// conservative upper bound. We restrict the callers to SynchronousQueue and ConcurrentHashMap,
// where we can predict the behavior (somewhat).
// Note: this is required (instead of lazy initialization) as these classes are used in the static
//       initialization of other classes, so will *use* the value.
void UnstartedRuntime::UnstartedRuntimeAvailableProcessors(Thread* self,
                                                           ShadowFrame* shadow_frame,
                                                           JValue* result,
                                                           [[maybe_unused]] size_t arg_offset) {
  if (CheckCallers(shadow_frame, { "void java.util.concurrent.SynchronousQueue.<clinit>()" })) {
    // SynchronousQueue really only separates between single- and multiprocessor case. Return
    // 8 as a conservative upper approximation.
    result->SetI(8);
  } else if (CheckCallers(shadow_frame,
                          { "void java.util.concurrent.ConcurrentHashMap.<clinit>()" })) {
    // ConcurrentHashMap uses it for striding. 8 still seems an OK general value, as it's likely
    // a good upper bound.
    // TODO: Consider resetting in the zygote?
    result->SetI(8);
  } else {
    // Not supported.
    AbortTransactionOrFail(self, "Accessing availableProcessors not allowed");
  }
}

// This allows accessing ConcurrentHashMap/SynchronousQueue.

void UnstartedRuntime::UnstartedUnsafeCompareAndSwapLong(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  UnstartedJdkUnsafeCompareAndSwapLong(self, shadow_frame, result, arg_offset);
}

void UnstartedRuntime::UnstartedUnsafeCompareAndSwapObject(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  UnstartedJdkUnsafeCompareAndSwapObject(self, shadow_frame, result, arg_offset);
}

void UnstartedRuntime::UnstartedUnsafeGetObjectVolatile(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  UnstartedJdkUnsafeGetReferenceVolatile(self, shadow_frame, result, arg_offset);
}

void UnstartedRuntime::UnstartedUnsafePutObjectVolatile(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  UnstartedJdkUnsafePutReferenceVolatile(self, shadow_frame, result, arg_offset);
}

void UnstartedRuntime::UnstartedUnsafePutOrderedObject(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  UnstartedJdkUnsafePutOrderedObject(self, shadow_frame, result, arg_offset);
}

void UnstartedRuntime::UnstartedJdkUnsafeCompareAndSetLong(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  UnstartedJdkUnsafeCompareAndSwapLong(self, shadow_frame, result, arg_offset);
}

void UnstartedRuntime::UnstartedJdkUnsafeCompareAndSetReference(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  UnstartedJdkUnsafeCompareAndSwapObject(self, shadow_frame, result, arg_offset);
}

void UnstartedRuntime::UnstartedJdkUnsafeCompareAndSwapLong(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  // Argument 0 is the Unsafe instance, skip.
  mirror::Object* obj = shadow_frame->GetVRegReference(arg_offset + 1);
  if (obj == nullptr) {
    AbortTransactionOrFail(self, "Cannot access null object, retry at runtime.");
    return;
  }
  int64_t offset = shadow_frame->GetVRegLong(arg_offset + 2);
  int64_t expectedValue = shadow_frame->GetVRegLong(arg_offset + 4);
  int64_t newValue = shadow_frame->GetVRegLong(arg_offset + 6);
  bool success;
  // Check whether we're in a transaction, call accordingly.
  Runtime* runtime = Runtime::Current();
  if (runtime->IsActiveTransaction()) {
    if (runtime->GetClassLinker()->TransactionWriteConstraint(self, obj)) {
      DCHECK(self->IsExceptionPending());
      return;
    }
    success = obj->CasFieldStrongSequentiallyConsistent64<true>(MemberOffset(offset),
                                                                expectedValue,
                                                                newValue);
  } else {
    success = obj->CasFieldStrongSequentiallyConsistent64<false>(MemberOffset(offset),
                                                                 expectedValue,
                                                                 newValue);
  }
  result->SetZ(success ? 1 : 0);
}

void UnstartedRuntime::UnstartedJdkUnsafeCompareAndSwapObject(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  // Argument 0 is the Unsafe instance, skip.
  mirror::Object* obj = shadow_frame->GetVRegReference(arg_offset + 1);
  if (obj == nullptr) {
    AbortTransactionOrFail(self, "Cannot access null object, retry at runtime.");
    return;
  }
  int64_t offset = shadow_frame->GetVRegLong(arg_offset + 2);
  mirror::Object* expected_value = shadow_frame->GetVRegReference(arg_offset + 4);
  mirror::Object* new_value = shadow_frame->GetVRegReference(arg_offset + 5);

  // Must use non transactional mode.
  if (gUseReadBarrier) {
    // Need to make sure the reference stored in the field is a to-space one before attempting the
    // CAS or the CAS could fail incorrectly.
    mirror::HeapReference<mirror::Object>* field_addr =
        reinterpret_cast<mirror::HeapReference<mirror::Object>*>(
            reinterpret_cast<uint8_t*>(obj) + static_cast<size_t>(offset));
    ReadBarrier::Barrier<
        mirror::Object,
        /* kIsVolatile= */ false,
        kWithReadBarrier,
        /* kAlwaysUpdateField= */ true>(
        obj,
        MemberOffset(offset),
        field_addr);
  }
  bool success;
  // Check whether we're in a transaction, call accordingly.
  Runtime* runtime = Runtime::Current();
  if (runtime->IsActiveTransaction()) {
    if (runtime->GetClassLinker()->TransactionWriteConstraint(self, obj) ||
        runtime->GetClassLinker()->TransactionWriteValueConstraint(self, new_value)) {
      DCHECK(self->IsExceptionPending());
      return;
    }
    success = obj->CasFieldObject<true>(MemberOffset(offset),
                                        expected_value,
                                        new_value,
                                        CASMode::kStrong,
                                        std::memory_order_seq_cst);
  } else {
    success = obj->CasFieldObject<false>(MemberOffset(offset),
                                         expected_value,
                                         new_value,
                                         CASMode::kStrong,
                                         std::memory_order_seq_cst);
  }
  result->SetZ(success ? 1 : 0);
}

void UnstartedRuntime::UnstartedJdkUnsafeGetReferenceVolatile(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  // Argument 0 is the Unsafe instance, skip.
  mirror::Object* obj = shadow_frame->GetVRegReference(arg_offset + 1);
  if (obj == nullptr) {
    AbortTransactionOrFail(self, "Cannot access null object, retry at runtime.");
    return;
  }
  int64_t offset = shadow_frame->GetVRegLong(arg_offset + 2);
  ObjPtr<mirror::Object> value = obj->GetFieldObjectVolatile<mirror::Object>(MemberOffset(offset));
  result->SetL(value);
}

void UnstartedRuntime::UnstartedJdkUnsafePutReferenceVolatile(Thread* self,
                                                              ShadowFrame* shadow_frame,
                                                              [[maybe_unused]] JValue* result,
                                                              size_t arg_offset)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  // Argument 0 is the Unsafe instance, skip.
  mirror::Object* obj = shadow_frame->GetVRegReference(arg_offset + 1);
  if (obj == nullptr) {
    AbortTransactionOrFail(self, "Cannot access null object, retry at runtime.");
    return;
  }
  int64_t offset = shadow_frame->GetVRegLong(arg_offset + 2);
  mirror::Object* value = shadow_frame->GetVRegReference(arg_offset + 4);
  Runtime* runtime = Runtime::Current();
  if (runtime->IsActiveTransaction()) {
    if (runtime->GetClassLinker()->TransactionWriteConstraint(self, obj) ||
        runtime->GetClassLinker()->TransactionWriteValueConstraint(self, value)) {
      DCHECK(self->IsExceptionPending());
      return;
    }
    obj->SetFieldObjectVolatile<true>(MemberOffset(offset), value);
  } else {
    obj->SetFieldObjectVolatile<false>(MemberOffset(offset), value);
  }
}

void UnstartedRuntime::UnstartedJdkUnsafePutOrderedObject(Thread* self,
                                                          ShadowFrame* shadow_frame,
                                                          [[maybe_unused]] JValue* result,
                                                          size_t arg_offset)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  // Argument 0 is the Unsafe instance, skip.
  mirror::Object* obj = shadow_frame->GetVRegReference(arg_offset + 1);
  if (obj == nullptr) {
    AbortTransactionOrFail(self, "Cannot access null object, retry at runtime.");
    return;
  }
  int64_t offset = shadow_frame->GetVRegLong(arg_offset + 2);
  mirror::Object* new_value = shadow_frame->GetVRegReference(arg_offset + 4);
  std::atomic_thread_fence(std::memory_order_release);
  Runtime* runtime = Runtime::Current();
  if (runtime->IsActiveTransaction()) {
    if (runtime->GetClassLinker()->TransactionWriteConstraint(self, obj) ||
        runtime->GetClassLinker()->TransactionWriteValueConstraint(self, new_value)) {
      DCHECK(self->IsExceptionPending());
      return;
    }
    obj->SetFieldObject<true>(MemberOffset(offset), new_value);
  } else {
    obj->SetFieldObject<false>(MemberOffset(offset), new_value);
  }
}

// A cutout for Integer.parseInt(String). Note: this code is conservative and will bail instead
// of correctly handling the corner cases.
void UnstartedRuntime::UnstartedIntegerParseInt(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  mirror::Object* obj = shadow_frame->GetVRegReference(arg_offset);
  if (obj == nullptr) {
    AbortTransactionOrFail(self, "Cannot parse null string, retry at runtime.");
    return;
  }

  std::string string_value = obj->AsString()->ToModifiedUtf8();
  if (string_value.empty()) {
    AbortTransactionOrFail(self, "Cannot parse empty string, retry at runtime.");
    return;
  }

  const char* c_str = string_value.c_str();
  char *end;
  // Can we set errno to 0? Is this always a variable, and not a macro?
  // Worst case, we'll incorrectly fail a transaction. Seems OK.
  int64_t l = strtol(c_str, &end, 10);

  if ((errno == ERANGE && l == LONG_MAX) || l > std::numeric_limits<int32_t>::max() ||
      (errno == ERANGE && l == LONG_MIN) || l < std::numeric_limits<int32_t>::min()) {
    AbortTransactionOrFail(self, "Cannot parse string %s, retry at runtime.", c_str);
    return;
  }
  if (l == 0) {
    // Check whether the string wasn't exactly zero.
    if (string_value != "0") {
      AbortTransactionOrFail(self, "Cannot parse string %s, retry at runtime.", c_str);
      return;
    }
  } else if (*end != '\0') {
    AbortTransactionOrFail(self, "Cannot parse string %s, retry at runtime.", c_str);
    return;
  }

  result->SetI(static_cast<int32_t>(l));
}

// A cutout for Long.parseLong.
//
// Note: for now use code equivalent to Integer.parseInt, as the full range may not be supported
//       well.
void UnstartedRuntime::UnstartedLongParseLong(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  mirror::Object* obj = shadow_frame->GetVRegReference(arg_offset);
  if (obj == nullptr) {
    AbortTransactionOrFail(self, "Cannot parse null string, retry at runtime.");
    return;
  }

  std::string string_value = obj->AsString()->ToModifiedUtf8();
  if (string_value.empty()) {
    AbortTransactionOrFail(self, "Cannot parse empty string, retry at runtime.");
    return;
  }

  const char* c_str = string_value.c_str();
  char *end;
  // Can we set errno to 0? Is this always a variable, and not a macro?
  // Worst case, we'll incorrectly fail a transaction. Seems OK.
  int64_t l = strtol(c_str, &end, 10);

  // Note: comparing against int32_t min/max is intentional here.
  if ((errno == ERANGE && l == LONG_MAX) || l > std::numeric_limits<int32_t>::max() ||
      (errno == ERANGE && l == LONG_MIN) || l < std::numeric_limits<int32_t>::min()) {
    AbortTransactionOrFail(self, "Cannot parse string %s, retry at runtime.", c_str);
    return;
  }
  if (l == 0) {
    // Check whether the string wasn't exactly zero.
    if (string_value != "0") {
      AbortTransactionOrFail(self, "Cannot parse string %s, retry at runtime.", c_str);
      return;
    }
  } else if (*end != '\0') {
    AbortTransactionOrFail(self, "Cannot parse string %s, retry at runtime.", c_str);
    return;
  }

  result->SetJ(l);
}

void UnstartedRuntime::UnstartedMethodInvoke(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  JNIEnvExt* env = self->GetJniEnv();
  ScopedObjectAccessUnchecked soa(self);

  ObjPtr<mirror::Object> java_method_obj = shadow_frame->GetVRegReference(arg_offset);
  ScopedLocalRef<jobject> java_method(env,
      java_method_obj == nullptr ? nullptr : env->AddLocalReference<jobject>(java_method_obj));

  ObjPtr<mirror::Object> java_receiver_obj = shadow_frame->GetVRegReference(arg_offset + 1);
  ScopedLocalRef<jobject> java_receiver(env,
      java_receiver_obj == nullptr ? nullptr : env->AddLocalReference<jobject>(java_receiver_obj));

  ObjPtr<mirror::Object> java_args_obj = shadow_frame->GetVRegReference(arg_offset + 2);
  ScopedLocalRef<jobject> java_args(env,
      java_args_obj == nullptr ? nullptr : env->AddLocalReference<jobject>(java_args_obj));

  PointerSize pointer_size = Runtime::Current()->GetClassLinker()->GetImagePointerSize();
  ScopedLocalRef<jobject> result_jobj(env,
      (pointer_size == PointerSize::k64)
          ? InvokeMethod<PointerSize::k64>(soa,
                                           java_method.get(),
                                           java_receiver.get(),
                                           java_args.get())
          : InvokeMethod<PointerSize::k32>(soa,
                                           java_method.get(),
                                           java_receiver.get(),
                                           java_args.get()));

  result->SetL(self->DecodeJObject(result_jobj.get()));

  // Conservatively flag all exceptions as transaction aborts. This way we don't need to unwrap
  // InvocationTargetExceptions.
  if (self->IsExceptionPending()) {
    AbortTransactionOrFail(self, "Failed Method.invoke");
  }
}

void UnstartedRuntime::UnstartedSystemIdentityHashCode([[maybe_unused]] Thread* self,
                                                       ShadowFrame* shadow_frame,
                                                       JValue* result,
                                                       size_t arg_offset)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  mirror::Object* obj = shadow_frame->GetVRegReference(arg_offset);
  result->SetI((obj != nullptr) ? obj->IdentityHashCode() : 0);
}

// Checks whether the runtime is s64-bit. This is needed for the clinit of
// java.lang.invoke.VarHandle clinit. The clinit determines sets of
// available VarHandle accessors and these differ based on machine
// word size.
void UnstartedRuntime::UnstartedJNIVMRuntimeIs64Bit([[maybe_unused]] Thread* self,
                                                    [[maybe_unused]] ArtMethod* method,
                                                    [[maybe_unused]] mirror::Object* receiver,
                                                    [[maybe_unused]] uint32_t* args,
                                                    JValue* result) {
  PointerSize pointer_size = Runtime::Current()->GetClassLinker()->GetImagePointerSize();
  jboolean is64bit = (pointer_size == PointerSize::k64) ? JNI_TRUE : JNI_FALSE;
  result->SetZ(is64bit);
}

void UnstartedRuntime::UnstartedJNIVMRuntimeNewUnpaddedArray(
    Thread* self,
    [[maybe_unused]] ArtMethod* method,
    [[maybe_unused]] mirror::Object* receiver,
    uint32_t* args,
    JValue* result) {
  int32_t length = args[1];
  DCHECK_GE(length, 0);
  ObjPtr<mirror::Object> element_class = reinterpret_cast32<mirror::Object*>(args[0])->AsClass();
  if (element_class == nullptr) {
    AbortTransactionOrFail(self, "VMRuntime.newUnpaddedArray with null element_class.");
    return;
  }
  Runtime* runtime = Runtime::Current();
  ObjPtr<mirror::Class> array_class =
      runtime->GetClassLinker()->FindArrayClass(self, element_class->AsClass());
  DCHECK(array_class != nullptr);
  gc::AllocatorType allocator = runtime->GetHeap()->GetCurrentAllocator();
  result->SetL(mirror::Array::Alloc</*kIsInstrumented=*/ true, /*kFillUsable=*/ true>(
      self, array_class, length, array_class->GetComponentSizeShift(), allocator));
}

void UnstartedRuntime::UnstartedJNIVMStackGetCallingClassLoader(
    [[maybe_unused]] Thread* self,
    [[maybe_unused]] ArtMethod* method,
    [[maybe_unused]] mirror::Object* receiver,
    [[maybe_unused]] uint32_t* args,
    JValue* result) {
  result->SetL(nullptr);
}

void UnstartedRuntime::UnstartedJNIVMStackGetStackClass2(Thread* self,
                                                         [[maybe_unused]] ArtMethod* method,
                                                         [[maybe_unused]] mirror::Object* receiver,
                                                         [[maybe_unused]] uint32_t* args,
                                                         JValue* result) {
  NthCallerVisitor visitor(self, 3);
  visitor.WalkStack();
  if (visitor.caller != nullptr) {
    result->SetL(visitor.caller->GetDeclaringClass());
  }
}

void UnstartedRuntime::UnstartedJNIMathLog([[maybe_unused]] Thread* self,
                                           [[maybe_unused]] ArtMethod* method,
                                           [[maybe_unused]] mirror::Object* receiver,
                                           uint32_t* args,
                                           JValue* result) {
  JValue value;
  value.SetJ((static_cast<uint64_t>(args[1]) << 32) | args[0]);
  result->SetD(log(value.GetD()));
}

void UnstartedRuntime::UnstartedJNIMathExp([[maybe_unused]] Thread* self,
                                           [[maybe_unused]] ArtMethod* method,
                                           [[maybe_unused]] mirror::Object* receiver,
                                           uint32_t* args,
                                           JValue* result) {
  JValue value;
  value.SetJ((static_cast<uint64_t>(args[1]) << 32) | args[0]);
  result->SetD(exp(value.GetD()));
}

void UnstartedRuntime::UnstartedJNIAtomicLongVMSupportsCS8(
    [[maybe_unused]] Thread* self,
    [[maybe_unused]] ArtMethod* method,
    [[maybe_unused]] mirror::Object* receiver,
    [[maybe_unused]] uint32_t* args,
    JValue* result) {
  result->SetZ(QuasiAtomic::LongAtomicsUseMutexes(Runtime::Current()->GetInstructionSet())
                   ? 0
                   : 1);
}

void UnstartedRuntime::UnstartedJNIClassGetNameNative(Thread* self,
                                                      [[maybe_unused]] ArtMethod* method,
                                                      mirror::Object* receiver,
                                                      [[maybe_unused]] uint32_t* args,
                                                      JValue* result) {
  StackHandleScope<1> hs(self);
  result->SetL(mirror::Class::ComputeName(hs.NewHandle(receiver->AsClass())));
}

void UnstartedRuntime::UnstartedJNIDoubleLongBitsToDouble([[maybe_unused]] Thread* self,
                                                          [[maybe_unused]] ArtMethod* method,
                                                          [[maybe_unused]] mirror::Object* receiver,
                                                          uint32_t* args,
                                                          JValue* result) {
  uint64_t long_input = args[0] | (static_cast<uint64_t>(args[1]) << 32);
  result->SetD(bit_cast<double>(long_input));
}

void UnstartedRuntime::UnstartedJNIFloatFloatToRawIntBits([[maybe_unused]] Thread* self,
                                                          [[maybe_unused]] ArtMethod* method,
                                                          [[maybe_unused]] mirror::Object* receiver,
                                                          uint32_t* args,
                                                          JValue* result) {
  result->SetI(args[0]);
}

void UnstartedRuntime::UnstartedJNIFloatIntBitsToFloat([[maybe_unused]] Thread* self,
                                                       [[maybe_unused]] ArtMethod* method,
                                                       [[maybe_unused]] mirror::Object* receiver,
                                                       uint32_t* args,
                                                       JValue* result) {
  result->SetI(args[0]);
}

void UnstartedRuntime::UnstartedJNIObjectInternalClone(Thread* self,
                                                       [[maybe_unused]] ArtMethod* method,
                                                       mirror::Object* receiver,
                                                       [[maybe_unused]] uint32_t* args,
                                                       JValue* result) {
  StackHandleScope<1> hs(self);
  Handle<mirror::Object> h_receiver = hs.NewHandle(receiver);
  result->SetL(mirror::Object::Clone(h_receiver, self));
}

void UnstartedRuntime::UnstartedJNIObjectNotifyAll(Thread* self,
                                                   [[maybe_unused]] ArtMethod* method,
                                                   mirror::Object* receiver,
                                                   [[maybe_unused]] uint32_t* args,
                                                   [[maybe_unused]] JValue* result) {
  receiver->NotifyAll(self);
}

void UnstartedRuntime::UnstartedJNIStringCompareTo(Thread* self,
                                                   [[maybe_unused]] ArtMethod* method,
                                                   mirror::Object* receiver,
                                                   uint32_t* args,
                                                   JValue* result) {
  ObjPtr<mirror::Object> rhs = reinterpret_cast32<mirror::Object*>(args[0]);
  if (rhs == nullptr) {
    AbortTransactionOrFail(self, "String.compareTo with null object.");
    return;
  }
  result->SetI(receiver->AsString()->CompareTo(rhs->AsString()));
}

void UnstartedRuntime::UnstartedJNIStringFillBytesLatin1(Thread* self,
                                                         [[maybe_unused]] ArtMethod* method,
                                                         mirror::Object* receiver,
                                                         uint32_t* args,
                                                         [[maybe_unused]] JValue*) {
  StackHandleScope<2> hs(self);
  Handle<mirror::String> h_receiver(hs.NewHandle(
      reinterpret_cast<mirror::String*>(receiver)->AsString()));
  Handle<mirror::ByteArray> h_buffer(hs.NewHandle(
      reinterpret_cast<mirror::ByteArray*>(args[0])->AsByteArray()));
  int32_t index = static_cast<int32_t>(args[1]);
  h_receiver->FillBytesLatin1(h_buffer, index);
}

void UnstartedRuntime::UnstartedJNIStringFillBytesUTF16(Thread* self,
                                                        [[maybe_unused]] ArtMethod* method,
                                                        mirror::Object* receiver,
                                                        uint32_t* args,
                                                        [[maybe_unused]] JValue*) {
  StackHandleScope<2> hs(self);
  Handle<mirror::String> h_receiver(hs.NewHandle(
      reinterpret_cast<mirror::String*>(receiver)->AsString()));
  Handle<mirror::ByteArray> h_buffer(hs.NewHandle(
      reinterpret_cast<mirror::ByteArray*>(args[0])->AsByteArray()));
  int32_t index = static_cast<int32_t>(args[1]);
  h_receiver->FillBytesUTF16(h_buffer, index);
}

void UnstartedRuntime::UnstartedJNIStringIntern([[maybe_unused]] Thread* self,
                                                [[maybe_unused]] ArtMethod* method,
                                                mirror::Object* receiver,
                                                [[maybe_unused]] uint32_t* args,
                                                JValue* result) {
  result->SetL(receiver->AsString()->Intern());
}

void UnstartedRuntime::UnstartedJNIArrayCreateMultiArray(Thread* self,
                                                         [[maybe_unused]] ArtMethod* method,
                                                         [[maybe_unused]] mirror::Object* receiver,
                                                         uint32_t* args,
                                                         JValue* result) {
  StackHandleScope<2> hs(self);
  auto h_class(hs.NewHandle(reinterpret_cast<mirror::Class*>(args[0])->AsClass()));
  auto h_dimensions(hs.NewHandle(reinterpret_cast<mirror::IntArray*>(args[1])->AsIntArray()));
  result->SetL(mirror::Array::CreateMultiArray(self, h_class, h_dimensions));
}

void UnstartedRuntime::UnstartedJNIArrayCreateObjectArray(Thread* self,
                                                          [[maybe_unused]] ArtMethod* method,
                                                          [[maybe_unused]] mirror::Object* receiver,
                                                          uint32_t* args,
                                                          JValue* result) {
  int32_t length = static_cast<int32_t>(args[1]);
  if (length < 0) {
    ThrowNegativeArraySizeException(length);
    return;
  }
  ObjPtr<mirror::Class> element_class = reinterpret_cast<mirror::Class*>(args[0])->AsClass();
  Runtime* runtime = Runtime::Current();
  ClassLinker* class_linker = runtime->GetClassLinker();
  ObjPtr<mirror::Class> array_class = class_linker->FindArrayClass(self, element_class);
  if (UNLIKELY(array_class == nullptr)) {
    CHECK(self->IsExceptionPending());
    return;
  }
  DCHECK(array_class->IsObjectArrayClass());
  ObjPtr<mirror::Array> new_array = mirror::ObjectArray<mirror::Object>::Alloc(
      self, array_class, length, runtime->GetHeap()->GetCurrentAllocator());
  result->SetL(new_array);
}

void UnstartedRuntime::UnstartedJNIThrowableNativeFillInStackTrace(
    Thread* self,
    [[maybe_unused]] ArtMethod* method,
    [[maybe_unused]] mirror::Object* receiver,
    [[maybe_unused]] uint32_t* args,
    JValue* result) {
  ScopedObjectAccessUnchecked soa(self);
  result->SetL(self->CreateInternalStackTrace(soa));
}

void UnstartedRuntime::UnstartedJNIUnsafeCompareAndSwapInt(
    Thread* self,
    ArtMethod* method,
    mirror::Object* receiver,
    uint32_t* args,
    JValue* result) {
  UnstartedJNIJdkUnsafeCompareAndSwapInt(self, method, receiver, args, result);
}

void UnstartedRuntime::UnstartedJNIUnsafeGetIntVolatile(Thread* self,
                                                        ArtMethod* method,
                                                        mirror::Object* receiver,
                                                        uint32_t* args,
                                                        JValue* result) {
  UnstartedJNIJdkUnsafeGetIntVolatile(self, method, receiver, args, result);
}

void UnstartedRuntime::UnstartedJNIUnsafePutObject(Thread* self,
                                                   ArtMethod* method,
                                                   mirror::Object* receiver,
                                                   uint32_t* args,
                                                   JValue* result) {
  UnstartedJNIJdkUnsafePutReference(self, method, receiver, args, result);
}

void UnstartedRuntime::UnstartedJNIUnsafeGetArrayBaseOffsetForComponentType(
    Thread* self,
    ArtMethod* method,
    mirror::Object* receiver,
    uint32_t* args,
    JValue* result) {
  UnstartedJNIJdkUnsafeGetArrayBaseOffsetForComponentType(self, method, receiver, args, result);
}

void UnstartedRuntime::UnstartedJNIUnsafeGetArrayIndexScaleForComponentType(
    Thread* self,
    ArtMethod* method,
    mirror::Object* receiver,
    uint32_t* args,
    JValue* result) {
  UnstartedJNIJdkUnsafeGetArrayIndexScaleForComponentType(self, method, receiver, args, result);
}

void UnstartedRuntime::UnstartedJNIJdkUnsafeAddressSize([[maybe_unused]] Thread* self,
                                                        [[maybe_unused]] ArtMethod* method,
                                                        [[maybe_unused]] mirror::Object* receiver,
                                                        [[maybe_unused]] uint32_t* args,
                                                        JValue* result) {
  result->SetI(sizeof(void*));
}

void UnstartedRuntime::UnstartedJNIJdkUnsafeCompareAndSwapInt(
    Thread* self,
    [[maybe_unused]] ArtMethod* method,
    [[maybe_unused]] mirror::Object* receiver,
    uint32_t* args,
    JValue* result) {
  ObjPtr<mirror::Object> obj = reinterpret_cast32<mirror::Object*>(args[0]);
  if (obj == nullptr) {
    AbortTransactionOrFail(self, "Cannot access null object, retry at runtime.");
    return;
  }
  jlong offset = (static_cast<uint64_t>(args[2]) << 32) | args[1];
  jint expectedValue = args[3];
  jint newValue = args[4];
  bool success;
  Runtime* runtime = Runtime::Current();
  if (runtime->IsActiveTransaction()) {
    if (runtime->GetClassLinker()->TransactionWriteConstraint(self, obj)) {
      DCHECK(self->IsExceptionPending());
      return;
    }
    success = obj->CasField32<true>(MemberOffset(offset),
                                    expectedValue,
                                    newValue,
                                    CASMode::kStrong,
                                    std::memory_order_seq_cst);
  } else {
    success = obj->CasField32<false>(MemberOffset(offset),
                                     expectedValue,
                                     newValue,
                                     CASMode::kStrong,
                                     std::memory_order_seq_cst);
  }
  result->SetZ(success ? JNI_TRUE : JNI_FALSE);
}

void UnstartedRuntime::UnstartedJNIJdkUnsafeCompareAndSetInt(
    Thread* self,
    ArtMethod* method,
    mirror::Object* receiver,
    uint32_t* args,
    JValue* result) {
  UnstartedJNIJdkUnsafeCompareAndSwapInt(self, method, receiver, args, result);
}

void UnstartedRuntime::UnstartedJNIJdkUnsafeGetIntVolatile(
    Thread* self,
    [[maybe_unused]] ArtMethod* method,
    [[maybe_unused]] mirror::Object* receiver,
    uint32_t* args,
    JValue* result) {
  ObjPtr<mirror::Object> obj = reinterpret_cast32<mirror::Object*>(args[0]);
  if (obj == nullptr) {
    AbortTransactionOrFail(self, "Unsafe.compareAndSwapIntVolatile with null object.");
    return;
  }

  jlong offset = (static_cast<uint64_t>(args[2]) << 32) | args[1];
  result->SetI(obj->GetField32Volatile(MemberOffset(offset)));
}

void UnstartedRuntime::UnstartedJNIJdkUnsafePutReference(Thread* self,
                                                         [[maybe_unused]] ArtMethod* method,
                                                         [[maybe_unused]] mirror::Object* receiver,
                                                         uint32_t* args,
                                                         [[maybe_unused]] JValue* result) {
  ObjPtr<mirror::Object> obj = reinterpret_cast32<mirror::Object*>(args[0]);
  if (obj == nullptr) {
    AbortTransactionOrFail(self, "Unsafe.putObject with null object.");
    return;
  }
  jlong offset = (static_cast<uint64_t>(args[2]) << 32) | args[1];
  ObjPtr<mirror::Object> new_value = reinterpret_cast32<mirror::Object*>(args[3]);
  Runtime* runtime = Runtime::Current();
  if (runtime->IsActiveTransaction()) {
    if (runtime->GetClassLinker()->TransactionWriteConstraint(self, obj) ||
        runtime->GetClassLinker()->TransactionWriteValueConstraint(self, new_value)) {
      DCHECK(self->IsExceptionPending());
      return;
    }
    obj->SetFieldObject<true>(MemberOffset(offset), new_value);
  } else {
    obj->SetFieldObject<false>(MemberOffset(offset), new_value);
  }
}

void UnstartedRuntime::UnstartedJNIJdkUnsafeStoreFence(Thread* self ATTRIBUTE_UNUSED,
                                                       ArtMethod* method ATTRIBUTE_UNUSED,
                                                       mirror::Object* receiver ATTRIBUTE_UNUSED,
                                                       uint32_t* args ATTRIBUTE_UNUSED,
                                                       JValue* result ATTRIBUTE_UNUSED) {
  std::atomic_thread_fence(std::memory_order_release);
}

void UnstartedRuntime::UnstartedJNIJdkUnsafeGetArrayBaseOffsetForComponentType(
    Thread* self,
    [[maybe_unused]] ArtMethod* method,
    [[maybe_unused]] mirror::Object* receiver,
    uint32_t* args,
    JValue* result) {
  ObjPtr<mirror::Object> component = reinterpret_cast32<mirror::Object*>(args[0]);
  if (component == nullptr) {
    AbortTransactionOrFail(self, "Unsafe.getArrayBaseOffsetForComponentType with null component.");
    return;
  }
  Primitive::Type primitive_type = component->AsClass()->GetPrimitiveType();
  result->SetI(mirror::Array::DataOffset(Primitive::ComponentSize(primitive_type)).Int32Value());
}

void UnstartedRuntime::UnstartedJNIJdkUnsafeGetArrayIndexScaleForComponentType(
    Thread* self,
    [[maybe_unused]] ArtMethod* method,
    [[maybe_unused]] mirror::Object* receiver,
    uint32_t* args,
    JValue* result) {
  ObjPtr<mirror::Object> component = reinterpret_cast32<mirror::Object*>(args[0]);
  if (component == nullptr) {
    AbortTransactionOrFail(self, "Unsafe.getArrayIndexScaleForComponentType with null component.");
    return;
  }
  Primitive::Type primitive_type = component->AsClass()->GetPrimitiveType();
  result->SetI(Primitive::ComponentSize(primitive_type));
}

void UnstartedRuntime::UnstartedJNIFieldGetArtField([[maybe_unused]] Thread* self,
                                                    [[maybe_unused]] ArtMethod* method,
                                                    mirror::Object* receiver,
                                                    [[maybe_unused]] uint32_t* args,
                                                    JValue* result) {
  ObjPtr<mirror::Field> field = ObjPtr<mirror::Field>::DownCast(receiver);
  ArtField* art_field = field->GetArtField();
  result->SetJ(reinterpret_cast<int64_t>(art_field));
}

void UnstartedRuntime::UnstartedJNIFieldGetNameInternal([[maybe_unused]] Thread* self,
                                                        [[maybe_unused]] ArtMethod* method,
                                                        mirror::Object* receiver,
                                                        [[maybe_unused]] uint32_t* args,
                                                        JValue* result) {
  ObjPtr<mirror::Field> field = ObjPtr<mirror::Field>::DownCast(receiver);
  ArtField* art_field = field->GetArtField();
  result->SetL(art_field->ResolveNameString());
}

using InvokeHandler = void(*)(Thread* self,
                              ShadowFrame* shadow_frame,
                              JValue* result,
                              size_t arg_size);

using JNIHandler = void(*)(Thread* self,
                           ArtMethod* method,
                           mirror::Object* receiver,
                           uint32_t* args,
                           JValue* result);

// NOLINTNEXTLINE
#define ONE_PLUS(ShortNameIgnored, DescriptorIgnored, NameIgnored, SignatureIgnored) 1 +
static constexpr size_t kInvokeHandlersSize = UNSTARTED_RUNTIME_DIRECT_LIST(ONE_PLUS) 0;
static constexpr size_t kJniHandlersSize = UNSTARTED_RUNTIME_JNI_LIST(ONE_PLUS) 0;
#undef ONE_PLUS

// The actual value of `kMinLoadFactor` is irrelevant because the HashMap<>s below
// are never resized, but we still need to pass a reasonable value to the constructor.
static constexpr double kMinLoadFactor = 0.5;
static constexpr double kMaxLoadFactor = 0.7;

constexpr size_t BufferSize(size_t size) {
  // Note: ceil() is not suitable for constexpr, so cast to size_t and adjust by 1 if needed.
  const size_t estimate = static_cast<size_t>(size / kMaxLoadFactor);
  return static_cast<size_t>(estimate * kMaxLoadFactor) == size ? estimate : estimate + 1u;
}

static constexpr size_t kInvokeHandlersBufferSize = BufferSize(kInvokeHandlersSize);
static_assert(
    static_cast<size_t>(kInvokeHandlersBufferSize * kMaxLoadFactor) == kInvokeHandlersSize);
static constexpr size_t kJniHandlersBufferSize = BufferSize(kJniHandlersSize);
static_assert(static_cast<size_t>(kJniHandlersBufferSize * kMaxLoadFactor) == kJniHandlersSize);

static bool tables_initialized_ = false;
static std::pair<ArtMethod*, InvokeHandler> kInvokeHandlersBuffer[kInvokeHandlersBufferSize];
static HashMap<ArtMethod*, InvokeHandler> invoke_handlers_(
    kMinLoadFactor, kMaxLoadFactor, kInvokeHandlersBuffer, kInvokeHandlersBufferSize);
static std::pair<ArtMethod*, JNIHandler> kJniHandlersBuffer[kJniHandlersBufferSize];
static HashMap<ArtMethod*, JNIHandler> jni_handlers_(
    kMinLoadFactor, kMaxLoadFactor, kJniHandlersBuffer, kJniHandlersBufferSize);

static ArtMethod* FindMethod(Thread* self,
                             ClassLinker* class_linker,
                             const char* descriptor,
                             std::string_view name,
                             std::string_view signature) REQUIRES_SHARED(Locks::mutator_lock_) {
  ObjPtr<mirror::Class> klass = class_linker->FindSystemClass(self, descriptor);
  DCHECK(klass != nullptr) << descriptor;
  ArtMethod* method = klass->FindClassMethod(name, signature, class_linker->GetImagePointerSize());
  DCHECK(method != nullptr) << descriptor << "." << name << signature;
  return method;
}

void UnstartedRuntime::InitializeInvokeHandlers(Thread* self) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
#define UNSTARTED_DIRECT(ShortName, Descriptor, Name, Signature) \
  { \
    ArtMethod* method = FindMethod(self, class_linker, Descriptor, Name, Signature); \
    invoke_handlers_.insert(std::make_pair(method, & UnstartedRuntime::Unstarted ## ShortName)); \
  }
  UNSTARTED_RUNTIME_DIRECT_LIST(UNSTARTED_DIRECT)
#undef UNSTARTED_DIRECT
  DCHECK_EQ(invoke_handlers_.NumBuckets(), kInvokeHandlersBufferSize);
}

void UnstartedRuntime::InitializeJNIHandlers(Thread* self) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
#define UNSTARTED_JNI(ShortName, Descriptor, Name, Signature) \
  { \
    ArtMethod* method = FindMethod(self, class_linker, Descriptor, Name, Signature); \
    jni_handlers_.insert(std::make_pair(method, & UnstartedRuntime::UnstartedJNI ## ShortName)); \
  }
  UNSTARTED_RUNTIME_JNI_LIST(UNSTARTED_JNI)
#undef UNSTARTED_JNI
  DCHECK_EQ(jni_handlers_.NumBuckets(), kJniHandlersBufferSize);
}

void UnstartedRuntime::Initialize() {
  CHECK(!tables_initialized_);

  ScopedObjectAccess soa(Thread::Current());
  InitializeInvokeHandlers(soa.Self());
  InitializeJNIHandlers(soa.Self());

  tables_initialized_ = true;
}

void UnstartedRuntime::Reinitialize() {
  CHECK(tables_initialized_);

  // Note: HashSet::clear() abandons the pre-allocated storage which we need to keep.
  while (!invoke_handlers_.empty()) {
    invoke_handlers_.erase(invoke_handlers_.begin());
  }
  while (!jni_handlers_.empty()) {
    jni_handlers_.erase(jni_handlers_.begin());
  }

  tables_initialized_ = false;
  Initialize();
}

void UnstartedRuntime::Invoke(Thread* self, const CodeItemDataAccessor& accessor,
                              ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  // In a runtime that's not started we intercept certain methods to avoid complicated dependency
  // problems in core libraries.
  CHECK(tables_initialized_);

  const auto& iter = invoke_handlers_.find(shadow_frame->GetMethod());
  if (iter != invoke_handlers_.end()) {
    // Note: When we special case the method, we do not ensure initialization.
    // This has been the behavior since implementation of this feature.

    // Clear out the result in case it's not zeroed out.
    result->SetL(nullptr);

    // Push the shadow frame. This is so the failing method can be seen in abort dumps.
    self->PushShadowFrame(shadow_frame);

    (*iter->second)(self, shadow_frame, result, arg_offset);

    self->PopShadowFrame();
  } else {
    if (!EnsureInitialized(self, shadow_frame)) {
      return;
    }
    // Not special, continue with regular interpreter execution.
    ArtInterpreterToInterpreterBridge(self, accessor, shadow_frame, result);
  }
}

// Hand select a number of methods to be run in a not yet started runtime without using JNI.
void UnstartedRuntime::Jni(Thread* self, ArtMethod* method, mirror::Object* receiver,
                           uint32_t* args, JValue* result) {
  const auto& iter = jni_handlers_.find(method);
  if (iter != jni_handlers_.end()) {
    // Clear out the result in case it's not zeroed out.
    result->SetL(nullptr);
    (*iter->second)(self, method, receiver, args, result);
  } else {
    Runtime* runtime = Runtime::Current();
    if (runtime->IsActiveTransaction()) {
      runtime->GetClassLinker()->AbortTransactionF(
          self,
          "Attempt to invoke native method in non-started runtime: %s",
          ArtMethod::PrettyMethod(method).c_str());
    } else {
      LOG(FATAL) << "Calling native method " << ArtMethod::PrettyMethod(method)
                 << " in an unstarted non-transactional runtime";
    }
  }
}

}  // namespace interpreter
}  // namespace art
