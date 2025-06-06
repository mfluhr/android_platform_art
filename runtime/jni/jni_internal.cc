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

#include "jni_internal.h"

#include <log/log.h>

#include <cstdarg>
#include <memory>
#include <utility>

#include "art_field-inl.h"
#include "art_method-alloc-inl.h"
#include "base/allocator.h"
#include "base/atomic.h"
#include "base/casts.h"
#include "base/file_utils.h"
#include "base/logging.h"  // For VLOG.
#include "base/mutex.h"
#include "base/pointer_size.h"
#include "base/safe_map.h"
#include "base/stl_util.h"
#include "class_linker-inl.h"
#include "class_root-inl.h"
#include "dex/dex_file-inl.h"
#include "dex/utf-inl.h"
#include "fault_handler.h"
#include "gc/accounting/card_table-inl.h"
#include "gc_root.h"
#include "handle_scope.h"
#include "hidden_api.h"
#include "indirect_reference_table-inl.h"
#include "instrumentation.h"
#include "interpreter/interpreter.h"
#include "java_vm_ext.h"
#include "jni_env_ext.h"
#include "jvalue-inl.h"
#include "mirror/class-alloc-inl.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mirror/dex_cache-inl.h"
#include "mirror/field.h"
#include "mirror/method.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-alloc-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/string-alloc-inl.h"
#include "mirror/string-inl.h"
#include "mirror/throwable.h"
#include "nativebridge/native_bridge.h"
#include "nativehelper/scoped_local_ref.h"
#include "nativeloader/native_loader.h"
#include "parsed_options.h"
#include "reflection.h"
#include "runtime.h"
#include "scoped_thread_state_change-inl.h"
#include "thread.h"
#include "well_known_classes-inl.h"

namespace art HIDDEN {

namespace {

// Frees the given va_list upon destruction.
// This also guards the returns from inside of the CHECK_NON_NULL_ARGUMENTs.
struct ScopedVAArgs {
  explicit ScopedVAArgs(va_list* args): args(args) {}
  ScopedVAArgs(const ScopedVAArgs&) = delete;
  ScopedVAArgs(ScopedVAArgs&&) = delete;
  ~ScopedVAArgs() { va_end(*args); }

 private:
  va_list* args;
};

constexpr char kBadUtf8ReplacementChar = '?';

// This is a modified version of `CountModifiedUtf8Chars()` from utf.cc,
// with extra checks and different output options.
//
// The `good` functor can process valid characters.
// The `bad` functor is called when we find an invalid character.
//
// Returns the number of UTF-16 characters.
template <typename GoodFunc, typename BadFunc>
size_t VisitUtf8Chars(const char* utf8, size_t byte_count, GoodFunc good, BadFunc bad) {
  DCHECK_LE(byte_count, strlen(utf8));
  size_t len = 0;
  const char* end = utf8 + byte_count;
  while (utf8 != end) {
    int ic = *utf8;
    if (LIKELY((ic & 0x80) == 0)) {
      // One-byte encoding.
      good(utf8, 1u);
      utf8 += 1u;
      len += 1u;
      continue;
    }
    // Note: We do not check whether the bit 0x40 is correctly set in the leading byte of
    // a multi-byte sequence. Nor do we verify the top two bits of continuation characters.
    if ((ic & 0x20) == 0) {
      // Two-byte encoding.
      if (static_cast<size_t>(end - utf8) < 2u) {
        bad();
        return len + 1u;  // Reached end of sequence.
      }
      good(utf8, 2u);
      utf8 += 2u;
      len += 1u;
      continue;
    }
    if ((ic & 0x10) == 0) {
      // Three-byte encoding.
      if (static_cast<size_t>(end - utf8) < 3u) {
        bad();
        return len + 1u;  // Reached end of sequence
      }
      good(utf8, 3u);
      utf8 += 3u;
      len += 1u;
      continue;
    }

    // Four-byte encoding: needs to be converted into a surrogate pair.
    if (static_cast<size_t>(end - utf8) < 4u) {
      bad();
      return len + 1u;  // Reached end of sequence.
    }
    good(utf8, 4u);
    utf8 += 4u;
    len += 2u;
  }
  return len;
}

ALWAYS_INLINE
static inline uint16_t DecodeModifiedUtf8Character(const char* ptr, size_t length) {
  switch (length) {
    case 1:
      return ptr[0];
    case 2:
      return ((ptr[0] & 0x1fu) << 6) | (ptr[1] & 0x3fu);
    case 3:
      return ((ptr[0] & 0x0fu) << 12) | ((ptr[1] & 0x3fu) << 6) | (ptr[2] & 0x3fu);
    default:
      LOG(FATAL) << "UNREACHABLE";  // 4-byte sequences are not valid Modified UTF-8.
      UNREACHABLE();
  }
}

class NewStringUTFVisitor {
 public:
  NewStringUTFVisitor(const char* utf, size_t utf8_length, int32_t count, bool has_bad_char)
      : utf_(utf), utf8_length_(utf8_length), count_(count), has_bad_char_(has_bad_char) {}

  void operator()(ObjPtr<mirror::Object> obj, [[maybe_unused]] size_t usable_size) const
      REQUIRES_SHARED(Locks::mutator_lock_) {
    // Avoid AsString as object is not yet in live bitmap or allocation stack.
    ObjPtr<mirror::String> string = ObjPtr<mirror::String>::DownCast(obj);
    string->SetCount(count_);
    DCHECK_IMPLIES(string->IsCompressed(), mirror::kUseStringCompression);
    if (string->IsCompressed()) {
      uint8_t* value_compressed = string->GetValueCompressed();
      auto good = [&](const char* ptr, size_t length) {
        uint16_t c = DecodeModifiedUtf8Character(ptr, length);
        DCHECK(mirror::String::IsASCII(c));
        *value_compressed++ = dchecked_integral_cast<uint8_t>(c);
      };
      auto bad = [&]() {
        DCHECK(has_bad_char_);
        *value_compressed++ = kBadUtf8ReplacementChar;
      };
      VisitUtf8Chars(utf_, utf8_length_, good, bad);
    } else {
      // Uncompressed.
      uint16_t* value = string->GetValue();
      auto good = [&](const char* ptr, size_t length) {
        if (length != 4u) {
          *value++ = DecodeModifiedUtf8Character(ptr, length);
        } else {
          const uint32_t code_point = ((ptr[0] & 0x0fu) << 18) |
                                      ((ptr[1] & 0x3fu) << 12) |
                                      ((ptr[2] & 0x3fu) << 6) |
                                      (ptr[3] & 0x3fu);
          // TODO: What do we do about values outside the range [U+10000, U+10FFFF]?
          // The spec says they're invalid but nobody appears to check for them.
          const uint32_t code_point_bits = code_point - 0x10000u;
          *value++ = 0xd800u | ((code_point_bits >> 10) & 0x3ffu);
          *value++ = 0xdc00u | (code_point_bits & 0x3ffu);
        }
      };
      auto bad = [&]() {
        DCHECK(has_bad_char_);
        *value++ = kBadUtf8ReplacementChar;
      };
      VisitUtf8Chars(utf_, utf8_length_, good, bad);
      DCHECK_IMPLIES(mirror::kUseStringCompression,
                     !mirror::String::AllASCII(string->GetValue(), string->GetLength()));
    }
  }

 private:
  const char* utf_;
  size_t utf8_length_;
  const int32_t count_;
  bool has_bad_char_;
};

// The JNI specification says that `GetStringUTFLength()`, `GetStringUTFChars()`
// and `GetStringUTFRegion()` should emit the Modified UTF-8 encoding.
// However, we have been emitting 4-byte UTF-8 sequences for several years now
// and changing that would risk breaking a lot of binary interfaces.
constexpr bool kUtfUseShortZero = false;
constexpr bool kUtfUse4ByteSequence = true;  // This is against the JNI spec.
constexpr bool kUtfReplaceBadSurrogates = false;

jsize GetUncompressedStringUTFLength(const uint16_t* chars, size_t length) {
  jsize byte_count = 0;
  ConvertUtf16ToUtf8<kUtfUseShortZero, kUtfUse4ByteSequence, kUtfReplaceBadSurrogates>(
      chars, length, [&]([[maybe_unused]] char c) { ++byte_count; });
  return byte_count;
}

char* GetUncompressedStringUTFChars(const uint16_t* chars, size_t length, char* dest) {
  ConvertUtf16ToUtf8<kUtfUseShortZero, kUtfUse4ByteSequence, kUtfReplaceBadSurrogates>(
      chars, length, [&](char c) { *dest++ = c; });
  return dest;
}

}  // namespace

// Consider turning this on when there is errors which could be related to JNI array copies such as
// things not rendering correctly. E.g. b/16858794
static constexpr bool kWarnJniAbort = false;

static hiddenapi::AccessContext GetJniAccessContext(Thread* self)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  // Construct AccessContext from the first calling class on stack.
  // If the calling class cannot be determined, e.g. unattached threads,
  // we conservatively assume the caller is trusted.
  ObjPtr<mirror::Class> caller = GetCallingClass(self, /* num_frames= */ 1);
  return caller.IsNull() ? hiddenapi::AccessContext(/* is_trusted= */ true)
                         : hiddenapi::AccessContext(caller);
}

template<typename T>
ALWAYS_INLINE static bool ShouldDenyAccessToMember(
    T* member,
    Thread* self,
    hiddenapi::AccessMethod access_kind = hiddenapi::AccessMethod::kJNI)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  return hiddenapi::ShouldDenyAccessToMember(
      member,
      [self]() REQUIRES_SHARED(Locks::mutator_lock_) { return GetJniAccessContext(self); },
      access_kind);
}

// Helpers to call instrumentation functions for fields. These take jobjects so we don't need to set
// up handles for the rare case where these actually do something. Once these functions return it is
// possible there will be a pending exception if the instrumentation happens to throw one.
static void NotifySetObjectField(ArtField* field, jobject obj, jobject jval)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  DCHECK_EQ(field->GetTypeAsPrimitiveType(), Primitive::kPrimNot);
  instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
  if (UNLIKELY(instrumentation->HasFieldWriteListeners())) {
    Thread* self = Thread::Current();
    ArtMethod* cur_method = self->GetCurrentMethod(/*dex_pc=*/ nullptr,
                                                   /*check_suspended=*/ true,
                                                   /*abort_on_error=*/ false);

    if (cur_method == nullptr) {
      // Set/Get Fields can be issued without a method during runtime startup/teardown. Ignore all
      // of these changes.
      return;
    }
    DCHECK(cur_method->IsNative());
    JValue val;
    val.SetL(self->DecodeJObject(jval));
    instrumentation->FieldWriteEvent(self,
                                     self->DecodeJObject(obj),
                                     cur_method,
                                     0,  // dex_pc is always 0 since this is a native method.
                                     field,
                                     val);
  }
}

static void NotifySetPrimitiveField(ArtField* field, jobject obj, JValue val)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  DCHECK_NE(field->GetTypeAsPrimitiveType(), Primitive::kPrimNot);
  instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
  if (UNLIKELY(instrumentation->HasFieldWriteListeners())) {
    Thread* self = Thread::Current();
    ArtMethod* cur_method = self->GetCurrentMethod(/*dex_pc=*/ nullptr,
                                                   /*check_suspended=*/ true,
                                                   /*abort_on_error=*/ false);

    if (cur_method == nullptr) {
      // Set/Get Fields can be issued without a method during runtime startup/teardown. Ignore all
      // of these changes.
      return;
    }
    DCHECK(cur_method->IsNative());
    instrumentation->FieldWriteEvent(self,
                                     self->DecodeJObject(obj),
                                     cur_method,
                                     0,  // dex_pc is always 0 since this is a native method.
                                     field,
                                     val);
  }
}

static void NotifyGetField(ArtField* field, jobject obj)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
  if (UNLIKELY(instrumentation->HasFieldReadListeners())) {
    Thread* self = Thread::Current();
    ArtMethod* cur_method = self->GetCurrentMethod(/*dex_pc=*/ nullptr,
                                                   /*check_suspended=*/ true,
                                                   /*abort_on_error=*/ false);

    if (cur_method == nullptr) {
      // Set/Get Fields can be issued without a method during runtime startup/teardown. Ignore all
      // of these changes.
      return;
    }
    DCHECK(cur_method->IsNative());
    instrumentation->FieldReadEvent(self,
                                    self->DecodeJObject(obj),
                                    cur_method,
                                    0,  // dex_pc is always 0 since this is a native method.
                                    field);
  }
}

// Section 12.3.2 of the JNI spec describes JNI class descriptors. They're
// separated with slashes but aren't wrapped with "L;" like regular descriptors
// (i.e. "a/b/C" rather than "La/b/C;"). Arrays of reference types are an
// exception; there the "L;" must be present ("[La/b/C;"). Historically we've
// supported names with dots too (such as "a.b.C").
static std::string NormalizeJniClassDescriptor(const char* name) {
  std::string result;
  // Add the missing "L;" if necessary.
  if (name[0] == '[') {
    result = name;
  } else {
    result += 'L';
    result += name;
    result += ';';
  }
  // Rewrite '.' as '/' for backwards compatibility.
  if (result.find('.') != std::string::npos) {
    LOG(WARNING) << "Call to JNI FindClass with dots in name: "
                 << "\"" << name << "\"";
    std::replace(result.begin(), result.end(), '.', '/');
  }
  return result;
}

static void ReportInvalidJNINativeMethod(const ScopedObjectAccess& soa,
                                         ObjPtr<mirror::Class> c,
                                         const char* kind,
                                         jint idx)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  LOG(ERROR)
      << "Failed to register native method in " << c->PrettyDescriptor()
      << " in " << c->GetDexCache()->GetLocation()->ToModifiedUtf8()
      << ": " << kind << " is null at index " << idx;
  soa.Self()->ThrowNewExceptionF("Ljava/lang/NoSuchMethodError;",
                                 "%s is null at index %d",
                                 kind,
                                 idx);
}

template<bool kEnableIndexIds>
static jmethodID FindMethodID(ScopedObjectAccess& soa, jclass jni_class,
                              const char* name, const char* sig, bool is_static)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  return jni::EncodeArtMethod<kEnableIndexIds>(FindMethodJNI(soa, jni_class, name, sig, is_static));
}

template<bool kEnableIndexIds>
static ObjPtr<mirror::ClassLoader> GetClassLoader(const ScopedObjectAccess& soa)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ArtMethod* method = soa.Self()->GetCurrentMethod(nullptr);
  // If we are running Runtime.nativeLoad, use the overriding ClassLoader it set.
  if (method == WellKnownClasses::java_lang_Runtime_nativeLoad) {
    return soa.Decode<mirror::ClassLoader>(soa.Self()->GetClassLoaderOverride());
  }
  // If we have a method, use its ClassLoader for context.
  if (method != nullptr) {
    return method->GetDeclaringClass()->GetClassLoader();
  }
  // We don't have a method, so try to use the system ClassLoader.
  ObjPtr<mirror::ClassLoader> class_loader =
      soa.Decode<mirror::ClassLoader>(Runtime::Current()->GetSystemClassLoader());
  if (class_loader != nullptr) {
    return class_loader;
  }
  // See if the override ClassLoader is set for gtests.
  class_loader = soa.Decode<mirror::ClassLoader>(soa.Self()->GetClassLoaderOverride());
  if (class_loader != nullptr) {
    // If so, CommonCompilerTest should have marked the runtime as a compiler not compiling an
    // image.
    CHECK(Runtime::Current()->IsAotCompiler());
    CHECK(!Runtime::Current()->IsCompilingBootImage());
    return class_loader;
  }
  // Use the BOOTCLASSPATH.
  return nullptr;
}

template<bool kEnableIndexIds>
static jfieldID FindFieldID(const ScopedObjectAccess& soa, jclass jni_class, const char* name,
                            const char* sig, bool is_static)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  return jni::EncodeArtField<kEnableIndexIds>(FindFieldJNI(soa, jni_class, name, sig, is_static));
}

static void ThrowAIOOBE(ScopedObjectAccess& soa,
                        ObjPtr<mirror::Array> array,
                        jsize start,
                        jsize length,
                        const char* identifier)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  std::string type(array->PrettyTypeOf());
  soa.Self()->ThrowNewExceptionF("Ljava/lang/ArrayIndexOutOfBoundsException;",
                                 "%s offset=%d length=%d %s.length=%d",
                                 type.c_str(), start, length, identifier, array->GetLength());
}

static void ThrowSIOOBE(ScopedObjectAccess& soa, jsize start, jsize length,
                        jsize array_length)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  soa.Self()->ThrowNewExceptionF("Ljava/lang/StringIndexOutOfBoundsException;",
                                 "offset=%d length=%d string.length()=%d", start, length,
                                 array_length);
}

static void ThrowNoSuchMethodError(const ScopedObjectAccess& soa,
                                   ObjPtr<mirror::Class> c,
                                   const char* name,
                                   const char* sig,
                                   const char* kind)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  std::string temp;
  soa.Self()->ThrowNewExceptionF("Ljava/lang/NoSuchMethodError;",
                                 "no %s method \"%s.%s%s\"",
                                 kind,
                                 c->GetDescriptor(&temp),
                                 name,
                                 sig);
}

static ObjPtr<mirror::Class> EnsureInitialized(Thread* self, ObjPtr<mirror::Class> klass)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (LIKELY(klass->IsInitialized())) {
    return klass;
  }
  StackHandleScope<1> hs(self);
  Handle<mirror::Class> h_klass(hs.NewHandle(klass));
  if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(self, h_klass, true, true)) {
    return nullptr;
  }
  return h_klass.Get();
}

ArtMethod* FindMethodJNI(const ScopedObjectAccess& soa,
                         jclass jni_class,
                         const char* name,
                         const char* sig,
                         bool is_static) {
  ObjPtr<mirror::Class> c = EnsureInitialized(soa.Self(), soa.Decode<mirror::Class>(jni_class));
  if (c == nullptr) {
    return nullptr;
  }
  ArtMethod* method = nullptr;
  auto pointer_size = Runtime::Current()->GetClassLinker()->GetImagePointerSize();
  if (c->IsInterface()) {
    method = c->FindInterfaceMethod(name, sig, pointer_size);
  } else {
    method = c->FindClassMethod(name, sig, pointer_size);
  }
  if (method != nullptr &&
      ShouldDenyAccessToMember(method, soa.Self(), hiddenapi::AccessMethod::kCheckWithPolicy)) {
    // The resolved method that we have found cannot be accessed due to
    // hiddenapi (typically it is declared up the hierarchy and is not an SDK
    // method). Try to find an interface method from the implemented interfaces which is
    // accessible.
    ArtMethod* itf_method = c->FindAccessibleInterfaceMethod(method, pointer_size);
    if (itf_method == nullptr) {
      // No interface method. Call ShouldDenyAccessToMember again but this time
      // with AccessMethod::kJNI to ensure that an appropriate warning is
      // logged.
      ShouldDenyAccessToMember(method, soa.Self(), hiddenapi::AccessMethod::kJNI);
      method = nullptr;
    } else {
      // We found an interface method that is accessible, continue with the resolved method.
    }
  }
  if (method == nullptr || method->IsStatic() != is_static) {
    ThrowNoSuchMethodError(soa, c, name, sig, is_static ? "static" : "non-static");
    return nullptr;
  }
  return method;
}

ArtField* FindFieldJNI(const ScopedObjectAccess& soa,
                       jclass jni_class,
                       const char* name,
                       const char* sig,
                       bool is_static) {
  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::Class> c(
      hs.NewHandle(EnsureInitialized(soa.Self(), soa.Decode<mirror::Class>(jni_class))));
  if (c == nullptr) {
    return nullptr;
  }
  ArtField* field = nullptr;
  ObjPtr<mirror::Class> field_type;
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  if (UNLIKELY(sig[0] == '\0')) {
    DCHECK(field == nullptr);
  } else if (sig[1] != '\0') {
    Handle<mirror::ClassLoader> class_loader(hs.NewHandle(c->GetClassLoader()));
    field_type = class_linker->FindClass(soa.Self(), sig, strlen(sig), class_loader);
  } else {
    field_type = class_linker->FindPrimitiveClass(*sig);
  }
  if (field_type == nullptr) {
    // Failed to find type from the signature of the field.
    DCHECK(sig[0] == '\0' || soa.Self()->IsExceptionPending());
    StackHandleScope<1> hs2(soa.Self());
    Handle<mirror::Throwable> cause(hs2.NewHandle(soa.Self()->GetException()));
    soa.Self()->ClearException();
    std::string temp;
    soa.Self()->ThrowNewExceptionF("Ljava/lang/NoSuchFieldError;",
                                   "no type \"%s\" found and so no field \"%s\" "
                                   "could be found in class \"%s\" or its superclasses", sig, name,
                                   c->GetDescriptor(&temp));
    if (cause != nullptr) {
      soa.Self()->GetException()->SetCause(cause.Get());
    }
    return nullptr;
  }
  std::string temp;
  if (is_static) {
    field = c->FindStaticField(name, field_type->GetDescriptor(&temp));
  } else {
    field = c->FindInstanceField(name, field_type->GetDescriptor(&temp));
  }
  if (field != nullptr && ShouldDenyAccessToMember(field, soa.Self())) {
    field = nullptr;
  }
  if (field == nullptr) {
    soa.Self()->ThrowNewExceptionF("Ljava/lang/NoSuchFieldError;",
                                   "no \"%s\" field \"%s\" in class \"%s\" or its superclasses",
                                   sig, name, c->GetDescriptor(&temp));
    return nullptr;
  }
  return field;
}

int ThrowNewException(JNIEnv* env, jclass exception_class, const char* msg, jobject cause)
    REQUIRES(!Locks::mutator_lock_) {
  // Turn the const char* into a java.lang.String.
  ScopedLocalRef<jstring> s(env, env->NewStringUTF(msg));
  if (msg != nullptr && s.get() == nullptr) {
    return JNI_ERR;
  }

  // Choose an appropriate constructor and set up the arguments.
  jvalue args[2];
  const char* signature;
  if (msg == nullptr && cause == nullptr) {
    signature = "()V";
  } else if (msg != nullptr && cause == nullptr) {
    signature = "(Ljava/lang/String;)V";
    args[0].l = s.get();
  } else if (msg == nullptr && cause != nullptr) {
    signature = "(Ljava/lang/Throwable;)V";
    args[0].l = cause;
  } else {
    signature = "(Ljava/lang/String;Ljava/lang/Throwable;)V";
    args[0].l = s.get();
    args[1].l = cause;
  }
  jmethodID mid = env->GetMethodID(exception_class, "<init>", signature);
  if (mid == nullptr) {
    ScopedObjectAccess soa(env);
    LOG(ERROR) << "No <init>" << signature << " in "
        << mirror::Class::PrettyClass(soa.Decode<mirror::Class>(exception_class));
    return JNI_ERR;
  }

  ScopedLocalRef<jthrowable> exception(
      env, reinterpret_cast<jthrowable>(env->NewObjectA(exception_class, mid, args)));
  if (exception.get() == nullptr) {
    return JNI_ERR;
  }
  ScopedObjectAccess soa(env);
  soa.Self()->SetException(soa.Decode<mirror::Throwable>(exception.get()));
  return JNI_OK;
}

static JavaVMExt* JavaVmExtFromEnv(JNIEnv* env) {
  return reinterpret_cast<JNIEnvExt*>(env)->GetVm();
}

#define CHECK_NON_NULL_ARGUMENT(value) \
    CHECK_NON_NULL_ARGUMENT_FN_NAME(__FUNCTION__, value, nullptr)

#define CHECK_NON_NULL_ARGUMENT_RETURN_VOID(value) \
    CHECK_NON_NULL_ARGUMENT_FN_NAME(__FUNCTION__, value, )

#define CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(value) \
    CHECK_NON_NULL_ARGUMENT_FN_NAME(__FUNCTION__, value, 0)

#define CHECK_NON_NULL_ARGUMENT_RETURN(value, return_val) \
    CHECK_NON_NULL_ARGUMENT_FN_NAME(__FUNCTION__, value, return_val)

#define CHECK_NON_NULL_ARGUMENT_FN_NAME(name, value, return_val) \
  if (UNLIKELY((value) == nullptr)) { \
    JavaVmExtFromEnv(env)->JniAbort(name, #value " == null"); \
    return return_val; \
  }

#define CHECK_NON_NULL_MEMCPY_ARGUMENT(length, value) \
  if (UNLIKELY((length) != 0 && (value) == nullptr)) { \
    JavaVmExtFromEnv(env)->JniAbort(__FUNCTION__, #value " == null"); \
    return; \
  }

template <bool kNative>
static ArtMethod* FindMethod(ObjPtr<mirror::Class> c,
                             std::string_view name,
                             std::string_view sig)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  auto pointer_size = Runtime::Current()->GetClassLinker()->GetImagePointerSize();
  for (auto& method : c->GetMethods(pointer_size)) {
    if (kNative == method.IsNative() && name == method.GetName() && method.GetSignature() == sig) {
      return &method;
    }
  }
  return nullptr;
}

template <bool kEnableIndexIds>
class JNI {
 public:
  static jint GetVersion(JNIEnv*) {
    return JNI_VERSION_1_6;
  }

  static jclass DefineClass(JNIEnv*, const char*, jobject, const jbyte*, jsize) {
    LOG(WARNING) << "JNI DefineClass is not supported";
    return nullptr;
  }

  static jclass FindClass(JNIEnv* env, const char* name) {
    CHECK_NON_NULL_ARGUMENT(name);
    Runtime* runtime = Runtime::Current();
    ClassLinker* class_linker = runtime->GetClassLinker();
    std::string descriptor(NormalizeJniClassDescriptor(name));
    ScopedObjectAccess soa(env);
    StackHandleScope<1> hs(soa.Self());
    Handle<mirror::ClassLoader> class_loader = hs.NewHandle(
        runtime->IsStarted() ? GetClassLoader<kEnableIndexIds>(soa) : nullptr);
    ObjPtr<mirror::Class> c = class_linker->FindClass(
        soa.Self(), descriptor.c_str(), descriptor.length(), class_loader);
    return soa.AddLocalReference<jclass>(c);
  }

  static jmethodID FromReflectedMethod(JNIEnv* env, jobject jlr_method) {
    CHECK_NON_NULL_ARGUMENT(jlr_method);
    ScopedObjectAccess soa(env);
    return jni::EncodeArtMethod<kEnableIndexIds>(ArtMethod::FromReflectedMethod(soa, jlr_method));
  }

  static jfieldID FromReflectedField(JNIEnv* env, jobject jlr_field) {
    CHECK_NON_NULL_ARGUMENT(jlr_field);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Object> obj_field = soa.Decode<mirror::Object>(jlr_field);
    if (obj_field->GetClass() != GetClassRoot<mirror::Field>()) {
      // Not even a java.lang.reflect.Field, return null. TODO, is this check necessary?
      return nullptr;
    }
    ObjPtr<mirror::Field> field = ObjPtr<mirror::Field>::DownCast(obj_field);
    return jni::EncodeArtField<kEnableIndexIds>(field->GetArtField());
  }

  static jobject ToReflectedMethod(JNIEnv* env, jclass, jmethodID mid, jboolean) {
    CHECK_NON_NULL_ARGUMENT(mid);
    ScopedObjectAccess soa(env);
    ArtMethod* m = jni::DecodeArtMethod(mid);
    ObjPtr<mirror::Executable> method;
    DCHECK_EQ(Runtime::Current()->GetClassLinker()->GetImagePointerSize(), kRuntimePointerSize);
    if (m->IsConstructor()) {
      method = mirror::Constructor::CreateFromArtMethod<kRuntimePointerSize>(soa.Self(), m);
    } else {
      method = mirror::Method::CreateFromArtMethod<kRuntimePointerSize>(soa.Self(), m);
    }
    return soa.AddLocalReference<jobject>(method);
  }

  static jobject ToReflectedField(JNIEnv* env, jclass, jfieldID fid, jboolean) {
    CHECK_NON_NULL_ARGUMENT(fid);
    ScopedObjectAccess soa(env);
    ArtField* f = jni::DecodeArtField(fid);
    return soa.AddLocalReference<jobject>(
        mirror::Field::CreateFromArtField(soa.Self(), f, true));
  }

  static jclass GetObjectClass(JNIEnv* env, jobject java_object) {
    CHECK_NON_NULL_ARGUMENT(java_object);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Object> o = soa.Decode<mirror::Object>(java_object);
    return soa.AddLocalReference<jclass>(o->GetClass());
  }

  static jclass GetSuperclass(JNIEnv* env, jclass java_class) {
    CHECK_NON_NULL_ARGUMENT(java_class);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Class> c = soa.Decode<mirror::Class>(java_class);
    return soa.AddLocalReference<jclass>(c->IsInterface() ? nullptr : c->GetSuperClass());
  }

  // Note: java_class1 should be safely castable to java_class2, and
  // not the other way around.
  static jboolean IsAssignableFrom(JNIEnv* env, jclass java_class1, jclass java_class2) {
    CHECK_NON_NULL_ARGUMENT_RETURN(java_class1, JNI_FALSE);
    CHECK_NON_NULL_ARGUMENT_RETURN(java_class2, JNI_FALSE);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Class> c1 = soa.Decode<mirror::Class>(java_class1);
    ObjPtr<mirror::Class> c2 = soa.Decode<mirror::Class>(java_class2);
    return c2->IsAssignableFrom(c1) ? JNI_TRUE : JNI_FALSE;
  }

  static jboolean IsInstanceOf(JNIEnv* env, jobject jobj, jclass java_class) {
    CHECK_NON_NULL_ARGUMENT_RETURN(java_class, JNI_FALSE);
    if (jobj == nullptr) {
      // Note: JNI is different from regular Java instanceof in this respect
      return JNI_TRUE;
    } else {
      ScopedObjectAccess soa(env);
      ObjPtr<mirror::Object> obj = soa.Decode<mirror::Object>(jobj);
      ObjPtr<mirror::Class> c = soa.Decode<mirror::Class>(java_class);
      return obj->InstanceOf(c) ? JNI_TRUE : JNI_FALSE;
    }
  }

  static jint Throw(JNIEnv* env, jthrowable java_exception) {
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Throwable> exception = soa.Decode<mirror::Throwable>(java_exception);
    if (exception == nullptr) {
      return JNI_ERR;
    }
    soa.Self()->SetException(exception);
    return JNI_OK;
  }

  static jint ThrowNew(JNIEnv* env, jclass c, const char* msg) {
    CHECK_NON_NULL_ARGUMENT_RETURN(c, JNI_ERR);
    return ThrowNewException(env, c, msg, nullptr);
  }

  static jboolean ExceptionCheck(JNIEnv* env) {
    return static_cast<JNIEnvExt*>(env)->self_->IsExceptionPending() ? JNI_TRUE : JNI_FALSE;
  }

  static void ExceptionClear(JNIEnv* env) {
    ScopedObjectAccess soa(env);
    soa.Self()->ClearException();
  }

  static void ExceptionDescribe(JNIEnv* env) {
    ScopedObjectAccess soa(env);

    // If we have no exception to describe, pass through.
    if (!soa.Self()->GetException()) {
      return;
    }

    StackHandleScope<1> hs(soa.Self());
    Handle<mirror::Throwable> old_exception(
        hs.NewHandle<mirror::Throwable>(soa.Self()->GetException()));
    soa.Self()->ClearException();
    ScopedLocalRef<jthrowable> exception(env,
                                         soa.AddLocalReference<jthrowable>(old_exception.Get()));
    ScopedLocalRef<jclass> exception_class(env, env->GetObjectClass(exception.get()));
    jmethodID mid = env->GetMethodID(exception_class.get(), "printStackTrace", "()V");
    if (mid == nullptr) {
      LOG(WARNING) << "JNI WARNING: no printStackTrace()V in "
                   << mirror::Object::PrettyTypeOf(old_exception.Get());
    } else {
      env->CallVoidMethod(exception.get(), mid);
      if (soa.Self()->IsExceptionPending()) {
        LOG(WARNING) << "JNI WARNING: " << mirror::Object::PrettyTypeOf(soa.Self()->GetException())
                     << " thrown while calling printStackTrace";
        soa.Self()->ClearException();
      }
    }
    soa.Self()->SetException(old_exception.Get());
  }

  static jthrowable ExceptionOccurred(JNIEnv* env) {
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Object> exception = soa.Self()->GetException();
    return soa.AddLocalReference<jthrowable>(exception);
  }

  static void FatalError(JNIEnv*, const char* msg) {
    LOG(FATAL) << "JNI FatalError called: " << msg;
  }

  static jint PushLocalFrame(JNIEnv* env, jint capacity) {
    // TODO: SOA may not be necessary but I do it to please lock annotations.
    ScopedObjectAccess soa(env);
    if (EnsureLocalCapacityInternal(soa, capacity, "PushLocalFrame") != JNI_OK) {
      return JNI_ERR;
    }
    down_cast<JNIEnvExt*>(env)->PushFrame(capacity);
    return JNI_OK;
  }

  static jobject PopLocalFrame(JNIEnv* env, jobject java_survivor) {
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Object> survivor = soa.Decode<mirror::Object>(java_survivor);
    soa.Env()->PopFrame();
    return soa.AddLocalReference<jobject>(survivor);
  }

  static jint EnsureLocalCapacity(JNIEnv* env, jint desired_capacity) {
    // TODO: SOA may not be necessary but I do it to please lock annotations.
    ScopedObjectAccess soa(env);
    return EnsureLocalCapacityInternal(soa, desired_capacity, "EnsureLocalCapacity");
  }

  static jobject NewGlobalRef(JNIEnv* env, jobject obj) {
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Object> decoded_obj = soa.Decode<mirror::Object>(obj);
    return soa.Vm()->AddGlobalRef(soa.Self(), decoded_obj);
  }

  static void DeleteGlobalRef(JNIEnv* env, jobject obj) {
    JavaVMExt* vm = down_cast<JNIEnvExt*>(env)->GetVm();
    Thread* self = down_cast<JNIEnvExt*>(env)->self_;
    vm->DeleteGlobalRef(self, obj);
  }

  static jweak NewWeakGlobalRef(JNIEnv* env, jobject obj) {
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Object> decoded_obj = soa.Decode<mirror::Object>(obj);
    return soa.Vm()->AddWeakGlobalRef(soa.Self(), decoded_obj);
  }

  static void DeleteWeakGlobalRef(JNIEnv* env, jweak obj) {
    JavaVMExt* vm = down_cast<JNIEnvExt*>(env)->GetVm();
    Thread* self = down_cast<JNIEnvExt*>(env)->self_;
    vm->DeleteWeakGlobalRef(self, obj);
  }

  static jobject NewLocalRef(JNIEnv* env, jobject obj) {
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Object> decoded_obj = soa.Decode<mirror::Object>(obj);
    // Check for null after decoding the object to handle cleared weak globals.
    if (decoded_obj == nullptr) {
      return nullptr;
    }
    return soa.AddLocalReference<jobject>(decoded_obj);
  }

  static void DeleteLocalRef(JNIEnv* env, jobject obj) {
    if (obj == nullptr) {
      return;
    }
    // SOA is only necessary to have exclusion between GC root marking and removing.
    // We don't want to have the GC attempt to mark a null root if we just removed
    // it. b/22119403
    ScopedObjectAccess soa(env);
    auto* ext_env = down_cast<JNIEnvExt*>(env);
    if (!ext_env->locals_.Remove(obj)) {
      // Attempting to delete a local reference that is not in the
      // topmost local reference frame is a no-op.  DeleteLocalRef returns
      // void and doesn't throw any exceptions, but we should probably
      // complain about it so the user will notice that things aren't
      // going quite the way they expect.
      LOG(WARNING) << "JNI WARNING: DeleteLocalRef(" << obj << ") "
                   << "failed to find entry";
      // Investigating b/228295454: Scudo ERROR: internal map failure (NO MEMORY).
      soa.Self()->DumpJavaStack(LOG_STREAM(WARNING));
    }
  }

  static jboolean IsSameObject(JNIEnv* env, jobject obj1, jobject obj2) {
    if (obj1 == obj2) {
      return JNI_TRUE;
    } else {
      ScopedObjectAccess soa(env);
      return (soa.Decode<mirror::Object>(obj1) == soa.Decode<mirror::Object>(obj2))
              ? JNI_TRUE : JNI_FALSE;
    }
  }

  static jobject AllocObject(JNIEnv* env, jclass java_class) {
    CHECK_NON_NULL_ARGUMENT(java_class);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Class> c = EnsureInitialized(soa.Self(), soa.Decode<mirror::Class>(java_class));
    if (c == nullptr) {
      return nullptr;
    }
    if (c->IsStringClass()) {
      gc::AllocatorType allocator_type = Runtime::Current()->GetHeap()->GetCurrentAllocator();
      return soa.AddLocalReference<jobject>(
          mirror::String::AllocEmptyString(soa.Self(), allocator_type));
    }
    return soa.AddLocalReference<jobject>(c->AllocObject(soa.Self()));
  }

  static jobject NewObject(JNIEnv* env, jclass java_class, jmethodID mid, ...) {
    va_list args;
    va_start(args, mid);
    ScopedVAArgs free_args_later(&args);
    CHECK_NON_NULL_ARGUMENT(java_class);
    CHECK_NON_NULL_ARGUMENT(mid);
    jobject result = NewObjectV(env, java_class, mid, args);
    return result;
  }

  static jobject NewObjectV(JNIEnv* env, jclass java_class, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT(java_class);
    CHECK_NON_NULL_ARGUMENT(mid);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Class> c = EnsureInitialized(soa.Self(),
                                                soa.Decode<mirror::Class>(java_class));
    if (c == nullptr) {
      return nullptr;
    }
    if (c->IsStringClass()) {
      // Replace calls to String.<init> with equivalent StringFactory call.
      jmethodID sf_mid = jni::EncodeArtMethod<kEnableIndexIds>(
          WellKnownClasses::StringInitToStringFactory(jni::DecodeArtMethod(mid)));
      return CallStaticObjectMethodV(env, WellKnownClasses::java_lang_StringFactory, sf_mid, args);
    }
    ScopedLocalRef<jobject> result(env, soa.AddLocalReference<jobject>(c->AllocObject(soa.Self())));
    if (result == nullptr) {
      return nullptr;
    }
    CallNonvirtualVoidMethodV(env, result.get(), java_class, mid, args);
    if (soa.Self()->IsExceptionPending()) {
      return nullptr;
    }
    return result.release();
  }

  static jobject NewObjectA(JNIEnv* env, jclass java_class, jmethodID mid, const jvalue* args) {
    CHECK_NON_NULL_ARGUMENT(java_class);
    CHECK_NON_NULL_ARGUMENT(mid);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Class> c = EnsureInitialized(soa.Self(),
                                                soa.Decode<mirror::Class>(java_class));
    if (c == nullptr) {
      return nullptr;
    }
    if (c->IsStringClass()) {
      // Replace calls to String.<init> with equivalent StringFactory call.
      jmethodID sf_mid = jni::EncodeArtMethod<kEnableIndexIds>(
          WellKnownClasses::StringInitToStringFactory(jni::DecodeArtMethod(mid)));
      return CallStaticObjectMethodA(env, WellKnownClasses::java_lang_StringFactory, sf_mid, args);
    }
    ScopedLocalRef<jobject> result(env, soa.AddLocalReference<jobject>(c->AllocObject(soa.Self())));
    if (result == nullptr) {
      return nullptr;
    }
    CallNonvirtualVoidMethodA(env, result.get(), java_class, mid, args);
    if (soa.Self()->IsExceptionPending()) {
      return nullptr;
    }
    return result.release();
  }

  static jmethodID GetMethodID(JNIEnv* env, jclass java_class, const char* name, const char* sig) {
    CHECK_NON_NULL_ARGUMENT(java_class);
    CHECK_NON_NULL_ARGUMENT(name);
    CHECK_NON_NULL_ARGUMENT(sig);
    ScopedObjectAccess soa(env);
    return FindMethodID<kEnableIndexIds>(soa, java_class, name, sig, false);
  }

  static jmethodID GetStaticMethodID(JNIEnv* env, jclass java_class, const char* name,
                                     const char* sig) {
    CHECK_NON_NULL_ARGUMENT(java_class);
    CHECK_NON_NULL_ARGUMENT(name);
    CHECK_NON_NULL_ARGUMENT(sig);
    ScopedObjectAccess soa(env);
    return FindMethodID<kEnableIndexIds>(soa, java_class, name, sig, true);
  }

  static jobject CallObjectMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    ScopedVAArgs free_args_later(&ap);
    CHECK_NON_NULL_ARGUMENT(obj);
    CHECK_NON_NULL_ARGUMENT(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, ap));
    return soa.AddLocalReference<jobject>(result.GetL());
  }

  static jobject CallObjectMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT(obj);
    CHECK_NON_NULL_ARGUMENT(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, args));
    return soa.AddLocalReference<jobject>(result.GetL());
  }

  static jobject CallObjectMethodA(JNIEnv* env, jobject obj, jmethodID mid, const jvalue* args) {
    CHECK_NON_NULL_ARGUMENT(obj);
    CHECK_NON_NULL_ARGUMENT(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeVirtualOrInterfaceWithJValues(soa, obj, mid, args));
    return soa.AddLocalReference<jobject>(result.GetL());
  }

  static jboolean CallBooleanMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    ScopedVAArgs free_args_later(&ap);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, ap));
    return result.GetZ();
  }

  static jboolean CallBooleanMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, args).GetZ();
  }

  static jboolean CallBooleanMethodA(JNIEnv* env, jobject obj, jmethodID mid, const jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithJValues(soa, obj, mid, args).GetZ();
  }

  static jbyte CallByteMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    ScopedVAArgs free_args_later(&ap);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, ap));
    return result.GetB();
  }

  static jbyte CallByteMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, args).GetB();
  }

  static jbyte CallByteMethodA(JNIEnv* env, jobject obj, jmethodID mid, const jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithJValues(soa, obj, mid, args).GetB();
  }

  static jchar CallCharMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    ScopedVAArgs free_args_later(&ap);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, ap));
    return result.GetC();
  }

  static jchar CallCharMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, args).GetC();
  }

  static jchar CallCharMethodA(JNIEnv* env, jobject obj, jmethodID mid, const jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithJValues(soa, obj, mid, args).GetC();
  }

  static jdouble CallDoubleMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    ScopedVAArgs free_args_later(&ap);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, ap));
    return result.GetD();
  }

  static jdouble CallDoubleMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, args).GetD();
  }

  static jdouble CallDoubleMethodA(JNIEnv* env, jobject obj, jmethodID mid, const jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithJValues(soa, obj, mid, args).GetD();
  }

  static jfloat CallFloatMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    ScopedVAArgs free_args_later(&ap);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, ap));
    return result.GetF();
  }

  static jfloat CallFloatMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, args).GetF();
  }

  static jfloat CallFloatMethodA(JNIEnv* env, jobject obj, jmethodID mid, const jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithJValues(soa, obj, mid, args).GetF();
  }

  static jint CallIntMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    ScopedVAArgs free_args_later(&ap);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, ap));
    return result.GetI();
  }

  static jint CallIntMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, args).GetI();
  }

  static jint CallIntMethodA(JNIEnv* env, jobject obj, jmethodID mid, const jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithJValues(soa, obj, mid, args).GetI();
  }

  static jlong CallLongMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    ScopedVAArgs free_args_later(&ap);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, ap));
    return result.GetJ();
  }

  static jlong CallLongMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, args).GetJ();
  }

  static jlong CallLongMethodA(JNIEnv* env, jobject obj, jmethodID mid, const jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithJValues(soa, obj, mid, args).GetJ();
  }

  static jshort CallShortMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    ScopedVAArgs free_args_later(&ap);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, ap));
    return result.GetS();
  }

  static jshort CallShortMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, args).GetS();
  }

  static jshort CallShortMethodA(JNIEnv* env, jobject obj, jmethodID mid, const jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeVirtualOrInterfaceWithJValues(soa, obj, mid, args).GetS();
  }

  static void CallVoidMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    ScopedVAArgs free_args_later(&ap);
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(mid);
    ScopedObjectAccess soa(env);
    InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, ap);
  }

  static void CallVoidMethodV(JNIEnv* env, jobject obj, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(mid);
    ScopedObjectAccess soa(env);
    InvokeVirtualOrInterfaceWithVarArgs(soa, obj, mid, args);
  }

  static void CallVoidMethodA(JNIEnv* env, jobject obj, jmethodID mid, const jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(mid);
    ScopedObjectAccess soa(env);
    InvokeVirtualOrInterfaceWithJValues(soa, obj, mid, args);
  }

  static jobject CallNonvirtualObjectMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    ScopedVAArgs free_args_later(&ap);
    CHECK_NON_NULL_ARGUMENT(obj);
    CHECK_NON_NULL_ARGUMENT(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, obj, mid, ap));
    return soa.AddLocalReference<jobject>(result.GetL());
  }

  static jobject CallNonvirtualObjectMethodV(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                             va_list args) {
    CHECK_NON_NULL_ARGUMENT(obj);
    CHECK_NON_NULL_ARGUMENT(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, obj, mid, args));
    return soa.AddLocalReference<jobject>(result.GetL());
  }

  static jobject CallNonvirtualObjectMethodA(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                             const jvalue* args) {
    CHECK_NON_NULL_ARGUMENT(obj);
    CHECK_NON_NULL_ARGUMENT(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithJValues(soa, obj, mid, args));
    return soa.AddLocalReference<jobject>(result.GetL());
  }

  static jboolean CallNonvirtualBooleanMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                              ...) {
    va_list ap;
    va_start(ap, mid);
    ScopedVAArgs free_args_later(&ap);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, obj, mid, ap));
    return result.GetZ();
  }

  static jboolean CallNonvirtualBooleanMethodV(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                               va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, obj, mid, args).GetZ();
  }

  static jboolean CallNonvirtualBooleanMethodA(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                               const jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, obj, mid, args).GetZ();
  }

  static jbyte CallNonvirtualByteMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    ScopedVAArgs free_args_later(&ap);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, obj, mid, ap));
    return result.GetB();
  }

  static jbyte CallNonvirtualByteMethodV(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                         va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, obj, mid, args).GetB();
  }

  static jbyte CallNonvirtualByteMethodA(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                         const jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, obj, mid, args).GetB();
  }

  static jchar CallNonvirtualCharMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    ScopedVAArgs free_args_later(&ap);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, obj, mid, ap));
    return result.GetC();
  }

  static jchar CallNonvirtualCharMethodV(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                         va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, obj, mid, args).GetC();
  }

  static jchar CallNonvirtualCharMethodA(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                         const jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, obj, mid, args).GetC();
  }

  static jshort CallNonvirtualShortMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    ScopedVAArgs free_args_later(&ap);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, obj, mid, ap));
    return result.GetS();
  }

  static jshort CallNonvirtualShortMethodV(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                           va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, obj, mid, args).GetS();
  }

  static jshort CallNonvirtualShortMethodA(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                           const jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, obj, mid, args).GetS();
  }

  static jint CallNonvirtualIntMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    ScopedVAArgs free_args_later(&ap);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, obj, mid, ap));
    return result.GetI();
  }

  static jint CallNonvirtualIntMethodV(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                       va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, obj, mid, args).GetI();
  }

  static jint CallNonvirtualIntMethodA(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                       const jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, obj, mid, args).GetI();
  }

  static jlong CallNonvirtualLongMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    ScopedVAArgs free_args_later(&ap);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, obj, mid, ap));
    return result.GetJ();
  }

  static jlong CallNonvirtualLongMethodV(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                         va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, obj, mid, args).GetJ();
  }

  static jlong CallNonvirtualLongMethodA(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                         const jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, obj, mid, args).GetJ();
  }

  static jfloat CallNonvirtualFloatMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    ScopedVAArgs free_args_later(&ap);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, obj, mid, ap));
    return result.GetF();
  }

  static jfloat CallNonvirtualFloatMethodV(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                           va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, obj, mid, args).GetF();
  }

  static jfloat CallNonvirtualFloatMethodA(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                           const jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, obj, mid, args).GetF();
  }

  static jdouble CallNonvirtualDoubleMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    ScopedVAArgs free_args_later(&ap);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, obj, mid, ap));
    return result.GetD();
  }

  static jdouble CallNonvirtualDoubleMethodV(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                             va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, obj, mid, args).GetD();
  }

  static jdouble CallNonvirtualDoubleMethodA(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                             const jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, obj, mid, args).GetD();
  }

  static void CallNonvirtualVoidMethod(JNIEnv* env, jobject obj, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    ScopedVAArgs free_args_later(&ap);
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(mid);
    ScopedObjectAccess soa(env);
    InvokeWithVarArgs(soa, obj, mid, ap);
  }

  static void CallNonvirtualVoidMethodV(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                        va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(mid);
    ScopedObjectAccess soa(env);
    InvokeWithVarArgs(soa, obj, mid, args);
  }

  static void CallNonvirtualVoidMethodA(JNIEnv* env, jobject obj, jclass, jmethodID mid,
                                        const jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(obj);
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(mid);
    ScopedObjectAccess soa(env);
    InvokeWithJValues(soa, obj, mid, args);
  }

  static jfieldID GetFieldID(JNIEnv* env, jclass java_class, const char* name, const char* sig) {
    CHECK_NON_NULL_ARGUMENT(java_class);
    CHECK_NON_NULL_ARGUMENT(name);
    CHECK_NON_NULL_ARGUMENT(sig);
    ScopedObjectAccess soa(env);
    return FindFieldID<kEnableIndexIds>(soa, java_class, name, sig, false);
  }

  static jfieldID GetStaticFieldID(JNIEnv* env, jclass java_class, const char* name,
                                   const char* sig) {
    CHECK_NON_NULL_ARGUMENT(java_class);
    CHECK_NON_NULL_ARGUMENT(name);
    CHECK_NON_NULL_ARGUMENT(sig);
    ScopedObjectAccess soa(env);
    return FindFieldID<kEnableIndexIds>(soa, java_class, name, sig, true);
  }

  static jobject GetObjectField(JNIEnv* env, jobject obj, jfieldID fid) {
    CHECK_NON_NULL_ARGUMENT(obj);
    CHECK_NON_NULL_ARGUMENT(fid);
    ScopedObjectAccess soa(env);
    ArtField* f = jni::DecodeArtField<kEnableIndexIds>(fid);
    NotifyGetField(f, obj);
    ObjPtr<mirror::Object> o = soa.Decode<mirror::Object>(obj);
    return soa.AddLocalReference<jobject>(f->GetObject(o));
  }

  static jobject GetStaticObjectField(JNIEnv* env, jclass, jfieldID fid) {
    CHECK_NON_NULL_ARGUMENT(fid);
    ScopedObjectAccess soa(env);
    ArtField* f = jni::DecodeArtField<kEnableIndexIds>(fid);
    NotifyGetField(f, nullptr);
    return soa.AddLocalReference<jobject>(f->GetObject(f->GetDeclaringClass()));
  }

  static void SetObjectField(JNIEnv* env, jobject java_object, jfieldID fid, jobject java_value) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(java_object);
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(fid);
    ScopedObjectAccess soa(env);
    ArtField* f = jni::DecodeArtField<kEnableIndexIds>(fid);
    NotifySetObjectField(f, java_object, java_value);
    ObjPtr<mirror::Object> o = soa.Decode<mirror::Object>(java_object);
    ObjPtr<mirror::Object> v = soa.Decode<mirror::Object>(java_value);
    f->SetObject<false>(o, v);
  }

  static void SetStaticObjectField(JNIEnv* env, jclass, jfieldID fid, jobject java_value) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(fid);
    ScopedObjectAccess soa(env);
    ArtField* f = jni::DecodeArtField<kEnableIndexIds>(fid);
    NotifySetObjectField(f, nullptr, java_value);
    ObjPtr<mirror::Object> v = soa.Decode<mirror::Object>(java_value);
    f->SetObject<false>(f->GetDeclaringClass(), v);
  }

#define GET_PRIMITIVE_FIELD(fn, instance) \
  CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(instance); \
  CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(fid); \
  ScopedObjectAccess soa(env); \
  ArtField* f = jni::DecodeArtField<kEnableIndexIds>(fid); \
  NotifyGetField(f, instance); \
  ObjPtr<mirror::Object> o = soa.Decode<mirror::Object>(instance); \
  return f->Get ##fn (o)

#define GET_STATIC_PRIMITIVE_FIELD(fn) \
  CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(fid); \
  ScopedObjectAccess soa(env); \
  ArtField* f = jni::DecodeArtField<kEnableIndexIds>(fid); \
  NotifyGetField(f, nullptr); \
  return f->Get ##fn (f->GetDeclaringClass())

#define SET_PRIMITIVE_FIELD(fn, instance, value) \
  CHECK_NON_NULL_ARGUMENT_RETURN_VOID(instance); \
  CHECK_NON_NULL_ARGUMENT_RETURN_VOID(fid); \
  ScopedObjectAccess soa(env); \
  ArtField* f = jni::DecodeArtField<kEnableIndexIds>(fid); \
  NotifySetPrimitiveField(f, instance, JValue::FromPrimitive<decltype(value)>(value)); \
  ObjPtr<mirror::Object> o = soa.Decode<mirror::Object>(instance); \
  f->Set ##fn <false>(o, value)

#define SET_STATIC_PRIMITIVE_FIELD(fn, value) \
  CHECK_NON_NULL_ARGUMENT_RETURN_VOID(fid); \
  ScopedObjectAccess soa(env); \
  ArtField* f = jni::DecodeArtField<kEnableIndexIds>(fid); \
  NotifySetPrimitiveField(f, nullptr, JValue::FromPrimitive<decltype(value)>(value)); \
  f->Set ##fn <false>(f->GetDeclaringClass(), value)

  static jboolean GetBooleanField(JNIEnv* env, jobject obj, jfieldID fid) {
    GET_PRIMITIVE_FIELD(Boolean, obj);
  }

  static jbyte GetByteField(JNIEnv* env, jobject obj, jfieldID fid) {
    GET_PRIMITIVE_FIELD(Byte, obj);
  }

  static jchar GetCharField(JNIEnv* env, jobject obj, jfieldID fid) {
    GET_PRIMITIVE_FIELD(Char, obj);
  }

  static jshort GetShortField(JNIEnv* env, jobject obj, jfieldID fid) {
    GET_PRIMITIVE_FIELD(Short, obj);
  }

  static jint GetIntField(JNIEnv* env, jobject obj, jfieldID fid) {
    GET_PRIMITIVE_FIELD(Int, obj);
  }

  static jlong GetLongField(JNIEnv* env, jobject obj, jfieldID fid) {
    GET_PRIMITIVE_FIELD(Long, obj);
  }

  static jfloat GetFloatField(JNIEnv* env, jobject obj, jfieldID fid) {
    GET_PRIMITIVE_FIELD(Float, obj);
  }

  static jdouble GetDoubleField(JNIEnv* env, jobject obj, jfieldID fid) {
    GET_PRIMITIVE_FIELD(Double, obj);
  }

  static jboolean GetStaticBooleanField(JNIEnv* env, jclass, jfieldID fid) {
    GET_STATIC_PRIMITIVE_FIELD(Boolean);
  }

  static jbyte GetStaticByteField(JNIEnv* env, jclass, jfieldID fid) {
    GET_STATIC_PRIMITIVE_FIELD(Byte);
  }

  static jchar GetStaticCharField(JNIEnv* env, jclass, jfieldID fid) {
    GET_STATIC_PRIMITIVE_FIELD(Char);
  }

  static jshort GetStaticShortField(JNIEnv* env, jclass, jfieldID fid) {
    GET_STATIC_PRIMITIVE_FIELD(Short);
  }

  static jint GetStaticIntField(JNIEnv* env, jclass, jfieldID fid) {
    GET_STATIC_PRIMITIVE_FIELD(Int);
  }

  static jlong GetStaticLongField(JNIEnv* env, jclass, jfieldID fid) {
    GET_STATIC_PRIMITIVE_FIELD(Long);
  }

  static jfloat GetStaticFloatField(JNIEnv* env, jclass, jfieldID fid) {
    GET_STATIC_PRIMITIVE_FIELD(Float);
  }

  static jdouble GetStaticDoubleField(JNIEnv* env, jclass, jfieldID fid) {
    GET_STATIC_PRIMITIVE_FIELD(Double);
  }

  static void SetBooleanField(JNIEnv* env, jobject obj, jfieldID fid, jboolean v) {
    SET_PRIMITIVE_FIELD(Boolean, obj, v);
  }

  static void SetByteField(JNIEnv* env, jobject obj, jfieldID fid, jbyte v) {
    SET_PRIMITIVE_FIELD(Byte, obj, v);
  }

  static void SetCharField(JNIEnv* env, jobject obj, jfieldID fid, jchar v) {
    SET_PRIMITIVE_FIELD(Char, obj, v);
  }

  static void SetFloatField(JNIEnv* env, jobject obj, jfieldID fid, jfloat v) {
    SET_PRIMITIVE_FIELD(Float, obj, v);
  }

  static void SetDoubleField(JNIEnv* env, jobject obj, jfieldID fid, jdouble v) {
    SET_PRIMITIVE_FIELD(Double, obj, v);
  }

  static void SetIntField(JNIEnv* env, jobject obj, jfieldID fid, jint v) {
    SET_PRIMITIVE_FIELD(Int, obj, v);
  }

  static void SetLongField(JNIEnv* env, jobject obj, jfieldID fid, jlong v) {
    SET_PRIMITIVE_FIELD(Long, obj, v);
  }

  static void SetShortField(JNIEnv* env, jobject obj, jfieldID fid, jshort v) {
    SET_PRIMITIVE_FIELD(Short, obj, v);
  }

  static void SetStaticBooleanField(JNIEnv* env, jclass, jfieldID fid, jboolean v) {
    SET_STATIC_PRIMITIVE_FIELD(Boolean, v);
  }

  static void SetStaticByteField(JNIEnv* env, jclass, jfieldID fid, jbyte v) {
    SET_STATIC_PRIMITIVE_FIELD(Byte, v);
  }

  static void SetStaticCharField(JNIEnv* env, jclass, jfieldID fid, jchar v) {
    SET_STATIC_PRIMITIVE_FIELD(Char, v);
  }

  static void SetStaticFloatField(JNIEnv* env, jclass, jfieldID fid, jfloat v) {
    SET_STATIC_PRIMITIVE_FIELD(Float, v);
  }

  static void SetStaticDoubleField(JNIEnv* env, jclass, jfieldID fid, jdouble v) {
    SET_STATIC_PRIMITIVE_FIELD(Double, v);
  }

  static void SetStaticIntField(JNIEnv* env, jclass, jfieldID fid, jint v) {
    SET_STATIC_PRIMITIVE_FIELD(Int, v);
  }

  static void SetStaticLongField(JNIEnv* env, jclass, jfieldID fid, jlong v) {
    SET_STATIC_PRIMITIVE_FIELD(Long, v);
  }

  static void SetStaticShortField(JNIEnv* env, jclass, jfieldID fid, jshort v) {
    SET_STATIC_PRIMITIVE_FIELD(Short, v);
  }

  static jobject CallStaticObjectMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    ScopedVAArgs free_args_later(&ap);
    CHECK_NON_NULL_ARGUMENT(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, nullptr, mid, ap));
    return soa.AddLocalReference<jobject>(result.GetL());
  }

  static jobject CallStaticObjectMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, nullptr, mid, args));
    return soa.AddLocalReference<jobject>(result.GetL());
  }

  static jobject CallStaticObjectMethodA(JNIEnv* env, jclass, jmethodID mid, const jvalue* args) {
    CHECK_NON_NULL_ARGUMENT(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithJValues(soa, nullptr, mid, args));
    return soa.AddLocalReference<jobject>(result.GetL());
  }

  static jboolean CallStaticBooleanMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    ScopedVAArgs free_args_later(&ap);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, nullptr, mid, ap));
    return result.GetZ();
  }

  static jboolean CallStaticBooleanMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, nullptr, mid, args).GetZ();
  }

  static jboolean CallStaticBooleanMethodA(JNIEnv* env, jclass, jmethodID mid, const jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, nullptr, mid, args).GetZ();
  }

  static jbyte CallStaticByteMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    ScopedVAArgs free_args_later(&ap);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, nullptr, mid, ap));
    return result.GetB();
  }

  static jbyte CallStaticByteMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, nullptr, mid, args).GetB();
  }

  static jbyte CallStaticByteMethodA(JNIEnv* env, jclass, jmethodID mid, const jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, nullptr, mid, args).GetB();
  }

  static jchar CallStaticCharMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    ScopedVAArgs free_args_later(&ap);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, nullptr, mid, ap));
    return result.GetC();
  }

  static jchar CallStaticCharMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, nullptr, mid, args).GetC();
  }

  static jchar CallStaticCharMethodA(JNIEnv* env, jclass, jmethodID mid, const jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, nullptr, mid, args).GetC();
  }

  static jshort CallStaticShortMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    ScopedVAArgs free_args_later(&ap);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, nullptr, mid, ap));
    return result.GetS();
  }

  static jshort CallStaticShortMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, nullptr, mid, args).GetS();
  }

  static jshort CallStaticShortMethodA(JNIEnv* env, jclass, jmethodID mid, const jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, nullptr, mid, args).GetS();
  }

  static jint CallStaticIntMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    ScopedVAArgs free_args_later(&ap);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, nullptr, mid, ap));
    return result.GetI();
  }

  static jint CallStaticIntMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, nullptr, mid, args).GetI();
  }

  static jint CallStaticIntMethodA(JNIEnv* env, jclass, jmethodID mid, const jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, nullptr, mid, args).GetI();
  }

  static jlong CallStaticLongMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    ScopedVAArgs free_args_later(&ap);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, nullptr, mid, ap));
    return result.GetJ();
  }

  static jlong CallStaticLongMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, nullptr, mid, args).GetJ();
  }

  static jlong CallStaticLongMethodA(JNIEnv* env, jclass, jmethodID mid, const jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, nullptr, mid, args).GetJ();
  }

  static jfloat CallStaticFloatMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    ScopedVAArgs free_args_later(&ap);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, nullptr, mid, ap));
    return result.GetF();
  }

  static jfloat CallStaticFloatMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, nullptr, mid, args).GetF();
  }

  static jfloat CallStaticFloatMethodA(JNIEnv* env, jclass, jmethodID mid, const jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, nullptr, mid, args).GetF();
  }

  static jdouble CallStaticDoubleMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    ScopedVAArgs free_args_later(&ap);
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    JValue result(InvokeWithVarArgs(soa, nullptr, mid, ap));
    return result.GetD();
  }

  static jdouble CallStaticDoubleMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithVarArgs(soa, nullptr, mid, args).GetD();
  }

  static jdouble CallStaticDoubleMethodA(JNIEnv* env, jclass, jmethodID mid, const jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(mid);
    ScopedObjectAccess soa(env);
    return InvokeWithJValues(soa, nullptr, mid, args).GetD();
  }

  NO_STACK_PROTECTOR
  static void CallStaticVoidMethod(JNIEnv* env, jclass, jmethodID mid, ...) {
    va_list ap;
    va_start(ap, mid);
    ScopedVAArgs free_args_later(&ap);
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(mid);
    ScopedObjectAccess soa(env);
    InvokeWithVarArgs(soa, nullptr, mid, ap);
  }

  NO_STACK_PROTECTOR
  static void CallStaticVoidMethodV(JNIEnv* env, jclass, jmethodID mid, va_list args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(mid);
    ScopedObjectAccess soa(env);
    InvokeWithVarArgs(soa, nullptr, mid, args);
  }

  static void CallStaticVoidMethodA(JNIEnv* env, jclass, jmethodID mid, const jvalue* args) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(mid);
    ScopedObjectAccess soa(env);
    InvokeWithJValues(soa, nullptr, mid, args);
  }

  static jstring NewString(JNIEnv* env, const jchar* chars, jsize char_count) {
    if (UNLIKELY(char_count < 0)) {
      JavaVmExtFromEnv(env)->JniAbortF("NewString", "char_count < 0: %d", char_count);
      return nullptr;
    }
    if (UNLIKELY(chars == nullptr && char_count > 0)) {
      JavaVmExtFromEnv(env)->JniAbortF("NewString", "chars == null && char_count > 0");
      return nullptr;
    }
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::String> result = mirror::String::AllocFromUtf16(soa.Self(), char_count, chars);
    return soa.AddLocalReference<jstring>(result);
  }

  // For historical reasons, NewStringUTF() accepts 4-byte UTF-8
  // sequences which are not valid Modified UTF-8. This can be
  // considered an extension of the JNI specification.
  static jstring NewStringUTF(JNIEnv* env, const char* utf) {
    if (utf == nullptr) {
      return nullptr;
    }

    // The input may come from an untrusted source, so we need to validate it.
    // We do not perform full validation, only as much as necessary to avoid reading
    // beyond the terminating null character. CheckJNI performs stronger validation.
    size_t utf8_length = strlen(utf);
    bool compressible = mirror::kUseStringCompression;
    bool has_bad_char = false;
    size_t utf16_length = VisitUtf8Chars(
        utf,
        utf8_length,
        /*good=*/ [&compressible](const char* ptr, size_t length) {
          if (mirror::kUseStringCompression) {
            switch (length) {
              case 1:
                DCHECK(mirror::String::IsASCII(*ptr));
                break;
              case 2:
              case 3:
                if (!mirror::String::IsASCII(DecodeModifiedUtf8Character(ptr, length))) {
                  compressible = false;
                }
                break;
              default:
                // 4-byte sequences lead to uncompressible surroate pairs.
                DCHECK_EQ(length, 4u);
                compressible = false;
                break;
            }
          }
        },
        /*bad=*/ [&has_bad_char]() {
          static_assert(mirror::String::IsASCII(kBadUtf8ReplacementChar));  // Compressible.
          has_bad_char = true;
        });
    if (UNLIKELY(utf16_length > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))) {
      // Converting the utf16_length to int32_t would overflow. Explicitly throw an OOME.
      std::string error =
          android::base::StringPrintf("NewStringUTF input has 2^31 or more characters: %zu",
                                      utf16_length);
      ScopedObjectAccess soa(env);
      soa.Self()->ThrowOutOfMemoryError(error.c_str());
      return nullptr;
    }
    if (UNLIKELY(has_bad_char)) {
      // VisitUtf8Chars() found a bad character.
      android_errorWriteLog(0x534e4554, "172655291");  // Report to SafetyNet.
      // Report the error to logcat but avoid too much spam.
      static const uint64_t kMinDelay = UINT64_C(10000000000);  // 10s
      static std::atomic<uint64_t> prev_bad_input_time(UINT64_C(0));
      uint64_t prev_time = prev_bad_input_time.load(std::memory_order_relaxed);
      uint64_t now = NanoTime();
      if ((prev_time == 0u || now - prev_time >= kMinDelay) &&
          prev_bad_input_time.compare_exchange_strong(prev_time, now, std::memory_order_relaxed)) {
        LOG(ERROR) << "Invalid UTF-8 input to JNI::NewStringUTF()";
      }
    }
    const int32_t length_with_flag = mirror::String::GetFlaggedCount(utf16_length, compressible);
    NewStringUTFVisitor visitor(utf, utf8_length, length_with_flag, has_bad_char);

    ScopedObjectAccess soa(env);
    gc::AllocatorType allocator_type = Runtime::Current()->GetHeap()->GetCurrentAllocator();
    ObjPtr<mirror::String> result =
        mirror::String::Alloc(soa.Self(), length_with_flag, allocator_type, visitor);
    return soa.AddLocalReference<jstring>(result);
  }

  static jsize GetStringLength(JNIEnv* env, jstring java_string) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(java_string);
    ScopedObjectAccess soa(env);
    return soa.Decode<mirror::String>(java_string)->GetLength();
  }

  static jsize GetStringUTFLength(JNIEnv* env, jstring java_string) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(java_string);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::String> str = soa.Decode<mirror::String>(java_string);
    return str->IsCompressed()
        ? str->GetLength()
        : GetUncompressedStringUTFLength(str->GetValue(), str->GetLength());
  }

  static void GetStringRegion(JNIEnv* env, jstring java_string, jsize start, jsize length,
                              jchar* buf) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(java_string);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::String> s = soa.Decode<mirror::String>(java_string);
    if (start < 0 || length < 0 || length > s->GetLength() - start) {
      ThrowSIOOBE(soa, start, length, s->GetLength());
    } else {
      CHECK_NON_NULL_MEMCPY_ARGUMENT(length, buf);
      if (s->IsCompressed()) {
        const uint8_t* src = s->GetValueCompressed() + start;
        for (int i = 0; i < length; ++i) {
          buf[i] = static_cast<jchar>(src[i]);
        }
      } else {
        const jchar* chars = static_cast<jchar*>(s->GetValue());
        memcpy(buf, chars + start, length * sizeof(jchar));
      }
    }
  }

  static void GetStringUTFRegion(JNIEnv* env, jstring java_string, jsize start, jsize length,
                                 char* buf) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(java_string);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::String> s = soa.Decode<mirror::String>(java_string);
    if (start < 0 || length < 0 || length > s->GetLength() - start) {
      ThrowSIOOBE(soa, start, length, s->GetLength());
    } else {
      CHECK_NON_NULL_MEMCPY_ARGUMENT(length, buf);
      if (length == 0 && buf == nullptr) {
        // Don't touch anything when length is 0 and null buffer.
        return;
      }
      if (s->IsCompressed()) {
        const uint8_t* src = s->GetValueCompressed() + start;
        for (int i = 0; i < length; ++i) {
          buf[i] = static_cast<jchar>(src[i]);
        }
        buf[length] = '\0';
      } else {
        char* end = GetUncompressedStringUTFChars(s->GetValue() + start, length, buf);
        *end = '\0';
      }
    }
  }

  static const jchar* GetStringChars(JNIEnv* env, jstring java_string, jboolean* is_copy) {
    CHECK_NON_NULL_ARGUMENT(java_string);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::String> s = soa.Decode<mirror::String>(java_string);
    gc::Heap* heap = Runtime::Current()->GetHeap();
    if (heap->IsMovableObject(s) || s->IsCompressed()) {
      jchar* chars = new jchar[s->GetLength()];
      if (s->IsCompressed()) {
        int32_t length = s->GetLength();
        const uint8_t* src = s->GetValueCompressed();
        for (int i = 0; i < length; ++i) {
          chars[i] = static_cast<jchar>(src[i]);
        }
      } else {
        memcpy(chars, s->GetValue(), sizeof(jchar) * s->GetLength());
      }
      if (is_copy != nullptr) {
        *is_copy = JNI_TRUE;
      }
      return chars;
    }
    if (is_copy != nullptr) {
      *is_copy = JNI_FALSE;
    }
    return static_cast<jchar*>(s->GetValue());
  }

  static void ReleaseStringChars(JNIEnv* env, jstring java_string, const jchar* chars) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(java_string);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::String> s = soa.Decode<mirror::String>(java_string);
    if (s->IsCompressed() || (s->IsCompressed() == false && chars != s->GetValue())) {
      delete[] chars;
    }
  }

  static const jchar* GetStringCritical(JNIEnv* env, jstring java_string, jboolean* is_copy) {
    CHECK_NON_NULL_ARGUMENT(java_string);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::String> s = soa.Decode<mirror::String>(java_string);
    gc::Heap* heap = Runtime::Current()->GetHeap();
    if (s->IsCompressed()) {
      if (is_copy != nullptr) {
        *is_copy = JNI_TRUE;
      }
      int32_t length = s->GetLength();
      const uint8_t* src = s->GetValueCompressed();
      jchar* chars = new jchar[length];
      for (int i = 0; i < length; ++i) {
        chars[i] = static_cast<jchar>(src[i]);
      }
      return chars;
    } else {
      if (heap->IsMovableObject(s)) {
        StackHandleScope<1> hs(soa.Self());
        HandleWrapperObjPtr<mirror::String> h(hs.NewHandleWrapper(&s));
        if (!gUseReadBarrier && !gUseUserfaultfd) {
          heap->IncrementDisableMovingGC(soa.Self());
        } else {
          // For the CC and CMC collector, we only need to wait for the thread flip rather
          // than the whole GC to occur thanks to the to-space invariant.
          heap->IncrementDisableThreadFlip(soa.Self());
        }
      }
      // Ensure that the string doesn't cause userfaults in case passed on to
      // the kernel.
      heap->EnsureObjectUserfaulted(s);
      if (is_copy != nullptr) {
        *is_copy = JNI_FALSE;
      }
      return static_cast<jchar*>(s->GetValue());
    }
  }

  static void ReleaseStringCritical(JNIEnv* env,
                                    jstring java_string,
                                    const jchar* chars) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(java_string);
    ScopedObjectAccess soa(env);
    gc::Heap* heap = Runtime::Current()->GetHeap();
    ObjPtr<mirror::String> s = soa.Decode<mirror::String>(java_string);
    if (!s->IsCompressed() && heap->IsMovableObject(s)) {
      if (!gUseReadBarrier && !gUseUserfaultfd) {
        heap->DecrementDisableMovingGC(soa.Self());
      } else {
        heap->DecrementDisableThreadFlip(soa.Self());
      }
    }
    // TODO: For uncompressed strings GetStringCritical() always returns `s->GetValue()`.
    // Should we report an error if the user passes a different `chars`?
    if (s->IsCompressed() || (!s->IsCompressed() && s->GetValue() != chars)) {
      delete[] chars;
    }
  }

  static const char* GetStringUTFChars(JNIEnv* env, jstring java_string, jboolean* is_copy) {
    if (java_string == nullptr) {
      return nullptr;
    }
    if (is_copy != nullptr) {
      *is_copy = JNI_TRUE;
    }

    ScopedObjectAccess soa(env);
    ObjPtr<mirror::String> s = soa.Decode<mirror::String>(java_string);
    size_t length = s->GetLength();
    size_t byte_count =
        s->IsCompressed() ? length : GetUncompressedStringUTFLength(s->GetValue(), length);
    char* bytes = new char[byte_count + 1];
    CHECK(bytes != nullptr);  // bionic aborts anyway.
    if (s->IsCompressed()) {
      const uint8_t* src = s->GetValueCompressed();
      for (size_t i = 0; i < byte_count; ++i) {
        bytes[i] = src[i];
      }
    } else {
      char* end = GetUncompressedStringUTFChars(s->GetValue(), length, bytes);
      DCHECK_EQ(byte_count, static_cast<size_t>(end - bytes));
    }
    bytes[byte_count] = '\0';
    return bytes;
  }

  static void ReleaseStringUTFChars(JNIEnv*, jstring, const char* chars) {
    delete[] chars;
  }

  static jsize GetArrayLength(JNIEnv* env, jarray java_array) {
    CHECK_NON_NULL_ARGUMENT_RETURN_ZERO(java_array);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Object> obj = soa.Decode<mirror::Object>(java_array);
    if (UNLIKELY(!obj->IsArrayInstance())) {
      soa.Vm()->JniAbortF("GetArrayLength", "not an array: %s", obj->PrettyTypeOf().c_str());
      return 0;
    }
    ObjPtr<mirror::Array> array = obj->AsArray();
    return array->GetLength();
  }

  static jobject GetObjectArrayElement(JNIEnv* env, jobjectArray java_array, jsize index) {
    CHECK_NON_NULL_ARGUMENT(java_array);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::ObjectArray<mirror::Object>> array =
        soa.Decode<mirror::ObjectArray<mirror::Object>>(java_array);
    return soa.AddLocalReference<jobject>(array->Get(index));
  }

  static void SetObjectArrayElement(JNIEnv* env, jobjectArray java_array, jsize index,
                                    jobject java_value) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(java_array);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::ObjectArray<mirror::Object>> array =
        soa.Decode<mirror::ObjectArray<mirror::Object>>(java_array);
    ObjPtr<mirror::Object> value = soa.Decode<mirror::Object>(java_value);
    array->Set<false>(index, value);
  }

  static jbooleanArray NewBooleanArray(JNIEnv* env, jsize length) {
    return NewPrimitiveArray<jbooleanArray, mirror::BooleanArray>(env, length);
  }

  static jbyteArray NewByteArray(JNIEnv* env, jsize length) {
    return NewPrimitiveArray<jbyteArray, mirror::ByteArray>(env, length);
  }

  static jcharArray NewCharArray(JNIEnv* env, jsize length) {
    return NewPrimitiveArray<jcharArray, mirror::CharArray>(env, length);
  }

  static jdoubleArray NewDoubleArray(JNIEnv* env, jsize length) {
    return NewPrimitiveArray<jdoubleArray, mirror::DoubleArray>(env, length);
  }

  static jfloatArray NewFloatArray(JNIEnv* env, jsize length) {
    return NewPrimitiveArray<jfloatArray, mirror::FloatArray>(env, length);
  }

  static jintArray NewIntArray(JNIEnv* env, jsize length) {
    return NewPrimitiveArray<jintArray, mirror::IntArray>(env, length);
  }

  static jlongArray NewLongArray(JNIEnv* env, jsize length) {
    return NewPrimitiveArray<jlongArray, mirror::LongArray>(env, length);
  }

  static jobjectArray NewObjectArray(JNIEnv* env, jsize length, jclass element_jclass,
                                     jobject initial_element) {
    if (UNLIKELY(length < 0)) {
      JavaVmExtFromEnv(env)->JniAbortF("NewObjectArray", "negative array length: %d", length);
      return nullptr;
    }
    CHECK_NON_NULL_ARGUMENT(element_jclass);

    // Compute the array class corresponding to the given element class.
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Class> array_class;
    {
      ObjPtr<mirror::Class> element_class = soa.Decode<mirror::Class>(element_jclass);
      if (UNLIKELY(element_class->IsPrimitive())) {
        soa.Vm()->JniAbortF("NewObjectArray",
                            "not an object type: %s",
                            element_class->PrettyDescriptor().c_str());
        return nullptr;
      }
      ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
      array_class = class_linker->FindArrayClass(soa.Self(), element_class);
      if (UNLIKELY(array_class == nullptr)) {
        return nullptr;
      }
    }

    // Allocate and initialize if necessary.
    ObjPtr<mirror::ObjectArray<mirror::Object>> result =
        mirror::ObjectArray<mirror::Object>::Alloc(soa.Self(), array_class, length);
    if (result != nullptr && initial_element != nullptr) {
      ObjPtr<mirror::Object> initial_object = soa.Decode<mirror::Object>(initial_element);
      if (initial_object != nullptr) {
        ObjPtr<mirror::Class> element_class = result->GetClass()->GetComponentType();
        if (UNLIKELY(!element_class->IsAssignableFrom(initial_object->GetClass()))) {
          soa.Vm()->JniAbortF("NewObjectArray", "cannot assign object of type '%s' to array with "
                              "element type of '%s'",
                              mirror::Class::PrettyDescriptor(initial_object->GetClass()).c_str(),
                              element_class->PrettyDescriptor().c_str());
          return nullptr;
        } else {
          for (jsize i = 0; i < length; ++i) {
            result->SetWithoutChecks<false>(i, initial_object);
          }
        }
      }
    }
    return soa.AddLocalReference<jobjectArray>(result);
  }

  static jshortArray NewShortArray(JNIEnv* env, jsize length) {
    return NewPrimitiveArray<jshortArray, mirror::ShortArray>(env, length);
  }

  static void* GetPrimitiveArrayCritical(JNIEnv* env, jarray java_array, jboolean* is_copy) {
    CHECK_NON_NULL_ARGUMENT(java_array);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Array> array = soa.Decode<mirror::Array>(java_array);
    if (UNLIKELY(!array->GetClass()->IsPrimitiveArray())) {
      soa.Vm()->JniAbortF("GetPrimitiveArrayCritical", "expected primitive array, given %s",
                          array->GetClass()->PrettyDescriptor().c_str());
      return nullptr;
    }
    gc::Heap* heap = Runtime::Current()->GetHeap();
    if (heap->IsMovableObject(array)) {
      if (!gUseReadBarrier && !gUseUserfaultfd) {
        heap->IncrementDisableMovingGC(soa.Self());
      } else {
        // For the CC and CMC collector, we only need to wait for the thread flip rather
        // than the whole GC to occur thanks to the to-space invariant.
        heap->IncrementDisableThreadFlip(soa.Self());
      }
      // Re-decode in case the object moved since IncrementDisableGC waits for GC to complete.
      array = soa.Decode<mirror::Array>(java_array);
    }
    // Ensure that the array doesn't cause userfaults in case passed on to the kernel.
    heap->EnsureObjectUserfaulted(array);
    if (is_copy != nullptr) {
      *is_copy = JNI_FALSE;
    }
    return array->GetRawData(array->GetClass()->GetComponentSize(), 0);
  }

  static void ReleasePrimitiveArrayCritical(JNIEnv* env, jarray java_array, void* elements,
                                            jint mode) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(java_array);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Array> array = soa.Decode<mirror::Array>(java_array);
    if (UNLIKELY(!array->GetClass()->IsPrimitiveArray())) {
      soa.Vm()->JniAbortF("ReleasePrimitiveArrayCritical", "expected primitive array, given %s",
                          array->GetClass()->PrettyDescriptor().c_str());
      return;
    }
    const size_t component_size = array->GetClass()->GetComponentSize();
    ReleasePrimitiveArray(soa, array, component_size, elements, mode);
  }

  static jboolean* GetBooleanArrayElements(JNIEnv* env, jbooleanArray array, jboolean* is_copy) {
    return GetPrimitiveArray<jbooleanArray, jboolean, mirror::BooleanArray>(env, array, is_copy);
  }

  static jbyte* GetByteArrayElements(JNIEnv* env, jbyteArray array, jboolean* is_copy) {
    return GetPrimitiveArray<jbyteArray, jbyte, mirror::ByteArray>(env, array, is_copy);
  }

  static jchar* GetCharArrayElements(JNIEnv* env, jcharArray array, jboolean* is_copy) {
    return GetPrimitiveArray<jcharArray, jchar, mirror::CharArray>(env, array, is_copy);
  }

  static jdouble* GetDoubleArrayElements(JNIEnv* env, jdoubleArray array, jboolean* is_copy) {
    return GetPrimitiveArray<jdoubleArray, jdouble, mirror::DoubleArray>(env, array, is_copy);
  }

  static jfloat* GetFloatArrayElements(JNIEnv* env, jfloatArray array, jboolean* is_copy) {
    return GetPrimitiveArray<jfloatArray, jfloat, mirror::FloatArray>(env, array, is_copy);
  }

  static jint* GetIntArrayElements(JNIEnv* env, jintArray array, jboolean* is_copy) {
    return GetPrimitiveArray<jintArray, jint, mirror::IntArray>(env, array, is_copy);
  }

  static jlong* GetLongArrayElements(JNIEnv* env, jlongArray array, jboolean* is_copy) {
    return GetPrimitiveArray<jlongArray, jlong, mirror::LongArray>(env, array, is_copy);
  }

  static jshort* GetShortArrayElements(JNIEnv* env, jshortArray array, jboolean* is_copy) {
    return GetPrimitiveArray<jshortArray, jshort, mirror::ShortArray>(env, array, is_copy);
  }

  static void ReleaseBooleanArrayElements(JNIEnv* env, jbooleanArray array, jboolean* elements,
                                          jint mode) {
    ReleasePrimitiveArray<jbooleanArray, jboolean, mirror::BooleanArray>(env, array, elements,
                                                                         mode);
  }

  static void ReleaseByteArrayElements(JNIEnv* env, jbyteArray array, jbyte* elements, jint mode) {
    ReleasePrimitiveArray<jbyteArray, jbyte, mirror::ByteArray>(env, array, elements, mode);
  }

  static void ReleaseCharArrayElements(JNIEnv* env, jcharArray array, jchar* elements, jint mode) {
    ReleasePrimitiveArray<jcharArray, jchar, mirror::CharArray>(env, array, elements, mode);
  }

  static void ReleaseDoubleArrayElements(JNIEnv* env, jdoubleArray array, jdouble* elements,
                                         jint mode) {
    ReleasePrimitiveArray<jdoubleArray, jdouble, mirror::DoubleArray>(env, array, elements, mode);
  }

  static void ReleaseFloatArrayElements(JNIEnv* env, jfloatArray array, jfloat* elements,
                                        jint mode) {
    ReleasePrimitiveArray<jfloatArray, jfloat, mirror::FloatArray>(env, array, elements, mode);
  }

  static void ReleaseIntArrayElements(JNIEnv* env, jintArray array, jint* elements, jint mode) {
    ReleasePrimitiveArray<jintArray, jint, mirror::IntArray>(env, array, elements, mode);
  }

  static void ReleaseLongArrayElements(JNIEnv* env, jlongArray array, jlong* elements, jint mode) {
    ReleasePrimitiveArray<jlongArray, jlong, mirror::LongArray>(env, array, elements, mode);
  }

  static void ReleaseShortArrayElements(JNIEnv* env, jshortArray array, jshort* elements,
                                        jint mode) {
    ReleasePrimitiveArray<jshortArray, jshort, mirror::ShortArray>(env, array, elements, mode);
  }

  static void GetBooleanArrayRegion(JNIEnv* env, jbooleanArray array, jsize start, jsize length,
                                    jboolean* buf) {
    GetPrimitiveArrayRegion<jbooleanArray, jboolean, mirror::BooleanArray>(env, array, start,
                                                                           length, buf);
  }

  static void GetByteArrayRegion(JNIEnv* env, jbyteArray array, jsize start, jsize length,
                                 jbyte* buf) {
    GetPrimitiveArrayRegion<jbyteArray, jbyte, mirror::ByteArray>(env, array, start, length, buf);
  }

  static void GetCharArrayRegion(JNIEnv* env, jcharArray array, jsize start, jsize length,
                                 jchar* buf) {
    GetPrimitiveArrayRegion<jcharArray, jchar, mirror::CharArray>(env, array, start, length, buf);
  }

  static void GetDoubleArrayRegion(JNIEnv* env, jdoubleArray array, jsize start, jsize length,
                                   jdouble* buf) {
    GetPrimitiveArrayRegion<jdoubleArray, jdouble, mirror::DoubleArray>(env, array, start, length,
                                                                        buf);
  }

  static void GetFloatArrayRegion(JNIEnv* env, jfloatArray array, jsize start, jsize length,
                                  jfloat* buf) {
    GetPrimitiveArrayRegion<jfloatArray, jfloat, mirror::FloatArray>(env, array, start, length,
                                                                     buf);
  }

  static void GetIntArrayRegion(JNIEnv* env, jintArray array, jsize start, jsize length,
                                jint* buf) {
    GetPrimitiveArrayRegion<jintArray, jint, mirror::IntArray>(env, array, start, length, buf);
  }

  static void GetLongArrayRegion(JNIEnv* env, jlongArray array, jsize start, jsize length,
                                 jlong* buf) {
    GetPrimitiveArrayRegion<jlongArray, jlong, mirror::LongArray>(env, array, start, length, buf);
  }

  static void GetShortArrayRegion(JNIEnv* env, jshortArray array, jsize start, jsize length,
                                  jshort* buf) {
    GetPrimitiveArrayRegion<jshortArray, jshort, mirror::ShortArray>(env, array, start, length,
                                                                     buf);
  }

  static void SetBooleanArrayRegion(JNIEnv* env, jbooleanArray array, jsize start, jsize length,
                                    const jboolean* buf) {
    SetPrimitiveArrayRegion<jbooleanArray, jboolean, mirror::BooleanArray>(env, array, start,
                                                                           length, buf);
  }

  static void SetByteArrayRegion(JNIEnv* env, jbyteArray array, jsize start, jsize length,
                                 const jbyte* buf) {
    SetPrimitiveArrayRegion<jbyteArray, jbyte, mirror::ByteArray>(env, array, start, length, buf);
  }

  static void SetCharArrayRegion(JNIEnv* env, jcharArray array, jsize start, jsize length,
                                 const jchar* buf) {
    SetPrimitiveArrayRegion<jcharArray, jchar, mirror::CharArray>(env, array, start, length, buf);
  }

  static void SetDoubleArrayRegion(JNIEnv* env, jdoubleArray array, jsize start, jsize length,
                                   const jdouble* buf) {
    SetPrimitiveArrayRegion<jdoubleArray, jdouble, mirror::DoubleArray>(env, array, start, length,
                                                                        buf);
  }

  static void SetFloatArrayRegion(JNIEnv* env, jfloatArray array, jsize start, jsize length,
                                  const jfloat* buf) {
    SetPrimitiveArrayRegion<jfloatArray, jfloat, mirror::FloatArray>(env, array, start, length,
                                                                     buf);
  }

  static void SetIntArrayRegion(JNIEnv* env, jintArray array, jsize start, jsize length,
                                const jint* buf) {
    SetPrimitiveArrayRegion<jintArray, jint, mirror::IntArray>(env, array, start, length, buf);
  }

  static void SetLongArrayRegion(JNIEnv* env, jlongArray array, jsize start, jsize length,
                                 const jlong* buf) {
    SetPrimitiveArrayRegion<jlongArray, jlong, mirror::LongArray>(env, array, start, length, buf);
  }

  static void SetShortArrayRegion(JNIEnv* env, jshortArray array, jsize start, jsize length,
                                  const jshort* buf) {
    SetPrimitiveArrayRegion<jshortArray, jshort, mirror::ShortArray>(env, array, start, length,
                                                                     buf);
  }

  static jint RegisterNatives(JNIEnv* env,
                              jclass java_class,
                              const JNINativeMethod* methods,
                              jint method_count) {
    if (UNLIKELY(method_count < 0)) {
      JavaVmExtFromEnv(env)->JniAbortF("RegisterNatives", "negative method count: %d",
                                       method_count);
      return JNI_ERR;  // Not reached except in unit tests.
    }
    CHECK_NON_NULL_ARGUMENT_FN_NAME("RegisterNatives", java_class, JNI_ERR);
    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
    ScopedObjectAccess soa(env);
    StackHandleScope<1> hs(soa.Self());
    Handle<mirror::Class> c = hs.NewHandle(soa.Decode<mirror::Class>(java_class));
    if (UNLIKELY(method_count == 0)) {
      LOG(WARNING) << "JNI RegisterNativeMethods: attempt to register 0 native methods for "
                   << c->PrettyDescriptor();
      return JNI_OK;
    }
    ScopedLocalRef<jobject> jclass_loader(env, nullptr);
    if (c->GetClassLoader() != nullptr) {
      jclass_loader.reset(soa.Env()->AddLocalReference<jobject>(c->GetClassLoader()));
    }

    bool is_class_loader_namespace_natively_bridged = false;
    {
      // Making sure to release mutator_lock_ before proceeding.
      // FindNativeLoaderNamespaceByClassLoader eventually acquires lock on g_namespaces_mutex
      // which may cause a deadlock if another thread is waiting for mutator_lock_
      // for IsSameObject call in libnativeloader's CreateClassLoaderNamespace (which happens
      // under g_namespace_mutex lock)
      ScopedThreadSuspension sts(soa.Self(), ThreadState::kNative);

      is_class_loader_namespace_natively_bridged =
          IsClassLoaderNamespaceNativelyBridged(env, jclass_loader.get());
    }

    CHECK_NON_NULL_ARGUMENT_FN_NAME("RegisterNatives", methods, JNI_ERR);
    for (jint i = 0; i < method_count; ++i) {
      const char* name = methods[i].name;
      const char* sig = methods[i].signature;
      const void* fnPtr = methods[i].fnPtr;
      if (UNLIKELY(name == nullptr)) {
        ReportInvalidJNINativeMethod(soa, c.Get(), "method name", i);
        return JNI_ERR;
      } else if (UNLIKELY(sig == nullptr)) {
        ReportInvalidJNINativeMethod(soa, c.Get(), "method signature", i);
        return JNI_ERR;
      } else if (UNLIKELY(fnPtr == nullptr)) {
        ReportInvalidJNINativeMethod(soa, c.Get(), "native function", i);
        return JNI_ERR;
      }
      bool is_fast = false;
      // Notes about fast JNI calls:
      //
      // On a normal JNI call, the calling thread usually transitions
      // from the kRunnable state to the kNative state. But if the
      // called native function needs to access any Java object, it
      // will have to transition back to the kRunnable state.
      //
      // There is a cost to this double transition. For a JNI call
      // that should be quick, this cost may dominate the call cost.
      //
      // On a fast JNI call, the calling thread avoids this double
      // transition by not transitioning from kRunnable to kNative and
      // stays in the kRunnable state.
      //
      // There are risks to using a fast JNI call because it can delay
      // a response to a thread suspension request which is typically
      // used for a GC root scanning, etc. If a fast JNI call takes a
      // long time, it could cause longer thread suspension latency
      // and GC pauses.
      //
      // Thus, fast JNI should be used with care. It should be used
      // for a JNI call that takes a short amount of time (eg. no
      // long-running loop) and does not block (eg. no locks, I/O,
      // etc.)
      //
      // A '!' prefix in the signature in the JNINativeMethod
      // indicates that it's a fast JNI call and the runtime omits the
      // thread state transition from kRunnable to kNative at the
      // entry.
      if (*sig == '!') {
        is_fast = true;
        ++sig;
      }

      // Note: the right order is to try to find the method locally
      // first, either as a direct or a virtual method. Then move to
      // the parent.
      ArtMethod* m = nullptr;
      bool warn_on_going_to_parent = down_cast<JNIEnvExt*>(env)->GetVm()->IsCheckJniEnabled();
      for (ObjPtr<mirror::Class> current_class = c.Get();
           current_class != nullptr;
           current_class = current_class->GetSuperClass()) {
        // Search first only comparing methods which are native.
        m = FindMethod<true>(current_class, name, sig);
        if (m != nullptr) {
          break;
        }

        // Search again comparing to all methods, to find non-native methods that match.
        m = FindMethod<false>(current_class, name, sig);
        if (m != nullptr) {
          break;
        }

        if (warn_on_going_to_parent) {
          LOG(WARNING) << "CheckJNI: method to register \"" << name << "\" not in the given class. "
                       << "This is slow, consider changing your RegisterNatives calls.";
          warn_on_going_to_parent = false;
        }
      }

      if (m == nullptr) {
        c->DumpClass(LOG_STREAM(ERROR), mirror::Class::kDumpClassFullDetail);
        LOG(ERROR)
            << "Failed to register native method "
            << c->PrettyDescriptor() << "." << name << sig << " in "
            << c->GetDexCache()->GetLocation()->ToModifiedUtf8();
        ThrowNoSuchMethodError(soa, c.Get(), name, sig, "static or non-static");
        return JNI_ERR;
      } else if (!m->IsNative()) {
        LOG(ERROR)
            << "Failed to register non-native method "
            << c->PrettyDescriptor() << "." << name << sig
            << " as native";
        ThrowNoSuchMethodError(soa, c.Get(), name, sig, "native");
        return JNI_ERR;
      }

      VLOG(jni) << "[Registering JNI native method " << m->PrettyMethod() << "]";

      if (UNLIKELY(is_fast)) {
        // There are a few reasons to switch:
        // 1) We don't support !bang JNI anymore, it will turn to a hard error later.
        // 2) @FastNative is actually faster. At least 1.5x faster than !bang JNI.
        //    and switching is super easy, remove ! in C code, add annotation in .java code.
        // 3) Good chance of hitting DCHECK failures in ScopedFastNativeObjectAccess
        //    since that checks for presence of @FastNative and not for ! in the descriptor.
        LOG(WARNING) << "!bang JNI is deprecated. Switch to @FastNative for " << m->PrettyMethod();
        is_fast = false;
        // TODO: make this a hard register error in the future.
      }

      // It is possible to link a class with native methods from a library loaded by
      // a different classloader. In this case IsClassLoaderNamespaceNativelyBridged
      // fails detect if native bridge is enabled and may return false.
      // For this reason we always check method with native bridge (see b/393035780
      // for details).
      if (is_class_loader_namespace_natively_bridged ||
          android::NativeBridgeIsNativeBridgeFunctionPointer(fnPtr)) {
        fnPtr = GenerateNativeBridgeTrampoline(fnPtr, m);
      }
      const void* final_function_ptr = class_linker->RegisterNative(soa.Self(), m, fnPtr);
      UNUSED(final_function_ptr);
    }
    return JNI_OK;
  }

  static jint UnregisterNatives(JNIEnv* env, jclass java_class) {
    CHECK_NON_NULL_ARGUMENT_RETURN(java_class, JNI_ERR);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Class> c = soa.Decode<mirror::Class>(java_class);

    VLOG(jni) << "[Unregistering JNI native methods for " << mirror::Class::PrettyClass(c) << "]";

    size_t unregistered_count = 0;
    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
    auto pointer_size = class_linker->GetImagePointerSize();
    for (auto& m : c->GetMethods(pointer_size)) {
      if (m.IsNative()) {
        class_linker->UnregisterNative(soa.Self(), &m);
        unregistered_count++;
      }
    }

    if (unregistered_count == 0) {
      LOG(WARNING) << "JNI UnregisterNatives: attempt to unregister native methods of class '"
          << mirror::Class::PrettyDescriptor(c) << "' that contains no native methods";
    }
    return JNI_OK;
  }

  static jint MonitorEnter(JNIEnv* env, jobject java_object) NO_THREAD_SAFETY_ANALYSIS {
    CHECK_NON_NULL_ARGUMENT_RETURN(java_object, JNI_ERR);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Object> o = soa.Decode<mirror::Object>(java_object);
    o = o->MonitorEnter(soa.Self());
    if (soa.Self()->HoldsLock(o)) {
      soa.Env()->monitors_.Add(o);
    }
    if (soa.Self()->IsExceptionPending()) {
      return JNI_ERR;
    }
    return JNI_OK;
  }

  static jint MonitorExit(JNIEnv* env, jobject java_object) NO_THREAD_SAFETY_ANALYSIS {
    CHECK_NON_NULL_ARGUMENT_RETURN(java_object, JNI_ERR);
    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Object> o = soa.Decode<mirror::Object>(java_object);
    bool remove_mon = soa.Self()->HoldsLock(o);
    o->MonitorExit(soa.Self());
    if (remove_mon) {
      soa.Env()->monitors_.Remove(o);
    }
    if (soa.Self()->IsExceptionPending()) {
      return JNI_ERR;
    }
    return JNI_OK;
  }

  static jint GetJavaVM(JNIEnv* env, JavaVM** vm) {
    CHECK_NON_NULL_ARGUMENT_RETURN(vm, JNI_ERR);
    Runtime* runtime = Runtime::Current();
    if (runtime != nullptr) {
      *vm = runtime->GetJavaVM();
    } else {
      *vm = nullptr;
    }
    return (*vm != nullptr) ? JNI_OK : JNI_ERR;
  }

  static jobject NewDirectByteBuffer(JNIEnv* env, void* address, jlong capacity) {
    if (capacity < 0) {
      JavaVmExtFromEnv(env)->JniAbortF("NewDirectByteBuffer", "negative buffer capacity: %" PRId64,
                                       capacity);
      return nullptr;
    }
    if (address == nullptr && capacity != 0) {
      JavaVmExtFromEnv(env)->JniAbortF("NewDirectByteBuffer",
                                       "non-zero capacity for nullptr pointer: %" PRId64, capacity);
      return nullptr;
    }

    // At the moment, the capacity of DirectByteBuffer is limited to a signed int.
    if (capacity > INT_MAX) {
      JavaVmExtFromEnv(env)->JniAbortF("NewDirectByteBuffer",
                                       "buffer capacity greater than maximum jint: %" PRId64,
                                       capacity);
      return nullptr;
    }
    jlong address_arg = reinterpret_cast<jlong>(address);
    jint capacity_arg = static_cast<jint>(capacity);

    ScopedObjectAccess soa(env);
    return soa.AddLocalReference<jobject>(
        WellKnownClasses::java_nio_DirectByteBuffer_init->NewObject<'J', 'I'>(
            soa.Self(), address_arg, capacity_arg));
  }

  static void* GetDirectBufferAddress(JNIEnv* env, jobject java_buffer) {
    // Return null if |java_buffer| is not defined.
    if (java_buffer == nullptr) {
      return nullptr;
    }

    ScopedObjectAccess soa(env);
    ObjPtr<mirror::Object> buffer = soa.Decode<mirror::Object>(java_buffer);

    // Return null if |java_buffer| is not a java.nio.Buffer instance.
    if (!buffer->InstanceOf(WellKnownClasses::java_nio_Buffer.Get())) {
      return nullptr;
    }

    // Buffer.address is non-null when the |java_buffer| is direct.
    return reinterpret_cast<void*>(WellKnownClasses::java_nio_Buffer_address->GetLong(buffer));
  }

  static jlong GetDirectBufferCapacity(JNIEnv* env, jobject java_buffer) {
    if (java_buffer == nullptr) {
      return -1;
    }

    ScopedObjectAccess soa(env);
    StackHandleScope<1u> hs(soa.Self());
    Handle<mirror::Object> buffer = hs.NewHandle(soa.Decode<mirror::Object>(java_buffer));
    if (!buffer->InstanceOf(WellKnownClasses::java_nio_Buffer.Get())) {
      return -1;
    }

    // When checking the buffer capacity, it's important to note that a zero-sized direct buffer
    // may have a null address field which means we can't tell whether it is direct or not.
    // We therefore call Buffer.isDirect(). One path that creates such a buffer is
    // FileChannel.map() if the file size is zero.
    //
    // NB GetDirectBufferAddress() does not need to call `Buffer.isDirect()` since it is only
    // able return a valid address if the Buffer address field is not-null.
    //
    // Note: We can hit a `StackOverflowError` during the invocation but `Buffer.isDirect()`
    // implementations should not otherwise throw any exceptions.
    bool direct = WellKnownClasses::java_nio_Buffer_isDirect->InvokeVirtual<'Z'>(
        soa.Self(), buffer.Get());
    if (UNLIKELY(soa.Self()->IsExceptionPending()) || !direct) {
      return -1;
    }

    return static_cast<jlong>(WellKnownClasses::java_nio_Buffer_capacity->GetInt(buffer.Get()));
  }

  static jobjectRefType GetObjectRefType([[maybe_unused]] JNIEnv* env, jobject java_object) {
    if (java_object == nullptr) {
      return JNIInvalidRefType;
    }

    // Do we definitely know what kind of reference this is?
    IndirectRef ref = reinterpret_cast<IndirectRef>(java_object);
    IndirectRefKind kind = IndirectReferenceTable::GetIndirectRefKind(ref);
    switch (kind) {
    case kLocal:
      return JNILocalRefType;
    case kGlobal:
      return JNIGlobalRefType;
    case kWeakGlobal:
      return JNIWeakGlobalRefType;
    case kJniTransition:
      // Assume value is in a JNI transition frame.
      return JNILocalRefType;
    }
    LOG(FATAL) << "IndirectRefKind[" << kind << "]";
    UNREACHABLE();
  }

 private:
  static jint EnsureLocalCapacityInternal(ScopedObjectAccess& soa, jint desired_capacity,
                                          const char* caller)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (desired_capacity > 0) {
      std::string error_msg;
      if (!soa.Env()->locals_.EnsureFreeCapacity(static_cast<size_t>(desired_capacity),
                                                 &error_msg)) {
        std::string caller_error = android::base::StringPrintf("%s: %s", caller,
                                                               error_msg.c_str());
        soa.Self()->ThrowOutOfMemoryError(caller_error.c_str());
        return JNI_ERR;
      }
    } else if (desired_capacity < 0) {
      LOG(ERROR) << "Invalid capacity given to " << caller << ": " << desired_capacity;
      return JNI_ERR;
    }  // The zero case is a no-op.
    return JNI_OK;
  }

  template<typename JniT, typename ArtT>
  static JniT NewPrimitiveArray(JNIEnv* env, jsize length) {
    ScopedObjectAccess soa(env);
    if (UNLIKELY(length < 0)) {
      soa.Vm()->JniAbortF("NewPrimitiveArray", "negative array length: %d", length);
      return nullptr;
    }
    ObjPtr<ArtT> result = ArtT::Alloc(soa.Self(), length);
    return soa.AddLocalReference<JniT>(result);
  }

  template <typename JArrayT, typename ElementT, typename ArtArrayT>
  static ObjPtr<ArtArrayT> DecodeAndCheckArrayType(ScopedObjectAccess& soa,
                                                   JArrayT java_array,
                                                   const char* fn_name,
                                                   const char* operation)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    ObjPtr<ArtArrayT> array = soa.Decode<ArtArrayT>(java_array);
    ObjPtr<mirror::Class> expected_array_class = GetClassRoot<ArtArrayT>();
    if (UNLIKELY(expected_array_class != array->GetClass())) {
      soa.Vm()->JniAbortF(fn_name,
                          "attempt to %s %s primitive array elements with an object of type %s",
                          operation,
                          mirror::Class::PrettyDescriptor(
                              expected_array_class->GetComponentType()).c_str(),
                          mirror::Class::PrettyDescriptor(array->GetClass()).c_str());
      return nullptr;
    }
    DCHECK_EQ(sizeof(ElementT), array->GetClass()->GetComponentSize());
    return array;
  }

  static bool IsClassLoaderNamespaceNativelyBridged(JNIEnv* env, jobject jclass_loader) {
#if defined(ART_TARGET_ANDROID)
    android::NativeLoaderNamespace* ns =
        android::FindNativeLoaderNamespaceByClassLoader(env, jclass_loader);
    return ns != nullptr && android::IsNamespaceNativeBridged(ns);
#else
    UNUSED(env, jclass_loader);
    return false;
#endif
  }

  static const void* GenerateNativeBridgeTrampoline(const void* fn_ptr, ArtMethod* method)
      REQUIRES_SHARED(Locks::mutator_lock_) {
#if defined(ART_TARGET_ANDROID)
    uint32_t shorty_length;
    const char* shorty = method->GetShorty(&shorty_length);
    android::JNICallType jni_call_type = method->IsCriticalNative() ?
                                             android::JNICallType::kJNICallTypeCriticalNative :
                                             android::JNICallType::kJNICallTypeRegular;
    return NativeBridgeGetTrampolineForFunctionPointer(
        fn_ptr, shorty, shorty_length, jni_call_type);
#else
    UNUSED(method);
    return fn_ptr;
#endif
  }

  template <typename ArrayT, typename ElementT, typename ArtArrayT>
  static ElementT* GetPrimitiveArray(JNIEnv* env, ArrayT java_array, jboolean* is_copy) {
    CHECK_NON_NULL_ARGUMENT(java_array);
    ScopedObjectAccess soa(env);
    ObjPtr<ArtArrayT> array = DecodeAndCheckArrayType<ArrayT, ElementT, ArtArrayT>(
        soa, java_array, "GetArrayElements", "get");
    if (UNLIKELY(array == nullptr)) {
      return nullptr;
    }
    // Only make a copy if necessary.
    if (Runtime::Current()->GetHeap()->IsMovableObject(array)) {
      if (is_copy != nullptr) {
        *is_copy = JNI_TRUE;
      }
      const size_t component_size = sizeof(ElementT);
      size_t size = array->GetLength() * component_size;
      void* data = new uint64_t[RoundUp(size, 8) / 8];
      memcpy(data, array->GetData(), size);
      return reinterpret_cast<ElementT*>(data);
    } else {
      if (is_copy != nullptr) {
        *is_copy = JNI_FALSE;
      }
      return reinterpret_cast<ElementT*>(array->GetData());
    }
  }

  template <typename ArrayT, typename ElementT, typename ArtArrayT>
  static void ReleasePrimitiveArray(JNIEnv* env, ArrayT java_array, ElementT* elements, jint mode) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(java_array);
    ScopedObjectAccess soa(env);
    ObjPtr<ArtArrayT> array = DecodeAndCheckArrayType<ArrayT, ElementT, ArtArrayT>(
        soa, java_array, "ReleaseArrayElements", "release");
    if (array == nullptr) {
      return;
    }
    ReleasePrimitiveArray(soa, array, sizeof(ElementT), elements, mode);
  }

  static void ReleasePrimitiveArray(ScopedObjectAccess& soa,
                                    ObjPtr<mirror::Array> array,
                                    size_t component_size,
                                    void* elements,
                                    jint mode)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    void* array_data = array->GetRawData(component_size, 0);
    gc::Heap* heap = Runtime::Current()->GetHeap();
    bool is_copy = array_data != elements;
    size_t bytes = array->GetLength() * component_size;
    if (is_copy) {
      // Integrity check: If elements is not the same as the java array's data, it better not be a
      // heap address. TODO: This might be slow to check, may be worth keeping track of which
      // copies we make?
      if (heap->IsNonDiscontinuousSpaceHeapAddress(elements)) {
        soa.Vm()->JniAbortF("ReleaseArrayElements",
                            "invalid element pointer %p, array elements are %p",
                            reinterpret_cast<void*>(elements), array_data);
        return;
      }
      if (mode != JNI_ABORT) {
        memcpy(array_data, elements, bytes);
      } else if (kWarnJniAbort && memcmp(array_data, elements, bytes) != 0) {
        // Warn if we have JNI_ABORT and the arrays don't match since this is usually an error.
        LOG(WARNING) << "Possible incorrect JNI_ABORT in Release*ArrayElements";
        soa.Self()->DumpJavaStack(LOG_STREAM(WARNING));
      }
    }
    if (mode != JNI_COMMIT) {
      if (is_copy) {
        delete[] reinterpret_cast<uint64_t*>(elements);
      } else if (heap->IsMovableObject(array)) {
        // Non copy to a movable object must means that we had disabled the moving GC.
        if (!gUseReadBarrier && !gUseUserfaultfd) {
          heap->DecrementDisableMovingGC(soa.Self());
        } else {
          heap->DecrementDisableThreadFlip(soa.Self());
        }
      }
    }
  }

  template <typename JArrayT, typename ElementT, typename ArtArrayT>
  static void GetPrimitiveArrayRegion(JNIEnv* env, JArrayT java_array,
                                      jsize start, jsize length, ElementT* buf) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(java_array);
    ScopedObjectAccess soa(env);
    ObjPtr<ArtArrayT> array = DecodeAndCheckArrayType<JArrayT, ElementT, ArtArrayT>(
        soa, java_array, "GetPrimitiveArrayRegion", "get region of");
    if (array != nullptr) {
      if (start < 0 || length < 0 || length > array->GetLength() - start) {
        ThrowAIOOBE(soa, array, start, length, "src");
      } else {
        CHECK_NON_NULL_MEMCPY_ARGUMENT(length, buf);
        ElementT* data = array->GetData();
        memcpy(buf, data + start, length * sizeof(ElementT));
      }
    }
  }

  template <typename JArrayT, typename ElementT, typename ArtArrayT>
  static void SetPrimitiveArrayRegion(JNIEnv* env, JArrayT java_array,
                                      jsize start, jsize length, const ElementT* buf) {
    CHECK_NON_NULL_ARGUMENT_RETURN_VOID(java_array);
    ScopedObjectAccess soa(env);
    ObjPtr<ArtArrayT> array = DecodeAndCheckArrayType<JArrayT, ElementT, ArtArrayT>(
        soa, java_array, "SetPrimitiveArrayRegion", "set region of");
    if (array != nullptr) {
      if (start < 0 || length < 0 || length > array->GetLength() - start) {
        ThrowAIOOBE(soa, array, start, length, "dst");
      } else {
        CHECK_NON_NULL_MEMCPY_ARGUMENT(length, buf);
        ElementT* data = array->GetData();
        memcpy(data + start, buf, length * sizeof(ElementT));
      }
    }
  }
};

template<bool kEnableIndexIds>
struct JniNativeInterfaceFunctions {
  using JNIImpl = JNI<kEnableIndexIds>;
  static constexpr JNINativeInterface gJniNativeInterface = {
    nullptr,  // reserved0.
    nullptr,  // reserved1.
    nullptr,  // reserved2.
    nullptr,  // reserved3.
    JNIImpl::GetVersion,
    JNIImpl::DefineClass,
    JNIImpl::FindClass,
    JNIImpl::FromReflectedMethod,
    JNIImpl::FromReflectedField,
    JNIImpl::ToReflectedMethod,
    JNIImpl::GetSuperclass,
    JNIImpl::IsAssignableFrom,
    JNIImpl::ToReflectedField,
    JNIImpl::Throw,
    JNIImpl::ThrowNew,
    JNIImpl::ExceptionOccurred,
    JNIImpl::ExceptionDescribe,
    JNIImpl::ExceptionClear,
    JNIImpl::FatalError,
    JNIImpl::PushLocalFrame,
    JNIImpl::PopLocalFrame,
    JNIImpl::NewGlobalRef,
    JNIImpl::DeleteGlobalRef,
    JNIImpl::DeleteLocalRef,
    JNIImpl::IsSameObject,
    JNIImpl::NewLocalRef,
    JNIImpl::EnsureLocalCapacity,
    JNIImpl::AllocObject,
    JNIImpl::NewObject,
    JNIImpl::NewObjectV,
    JNIImpl::NewObjectA,
    JNIImpl::GetObjectClass,
    JNIImpl::IsInstanceOf,
    JNIImpl::GetMethodID,
    JNIImpl::CallObjectMethod,
    JNIImpl::CallObjectMethodV,
    JNIImpl::CallObjectMethodA,
    JNIImpl::CallBooleanMethod,
    JNIImpl::CallBooleanMethodV,
    JNIImpl::CallBooleanMethodA,
    JNIImpl::CallByteMethod,
    JNIImpl::CallByteMethodV,
    JNIImpl::CallByteMethodA,
    JNIImpl::CallCharMethod,
    JNIImpl::CallCharMethodV,
    JNIImpl::CallCharMethodA,
    JNIImpl::CallShortMethod,
    JNIImpl::CallShortMethodV,
    JNIImpl::CallShortMethodA,
    JNIImpl::CallIntMethod,
    JNIImpl::CallIntMethodV,
    JNIImpl::CallIntMethodA,
    JNIImpl::CallLongMethod,
    JNIImpl::CallLongMethodV,
    JNIImpl::CallLongMethodA,
    JNIImpl::CallFloatMethod,
    JNIImpl::CallFloatMethodV,
    JNIImpl::CallFloatMethodA,
    JNIImpl::CallDoubleMethod,
    JNIImpl::CallDoubleMethodV,
    JNIImpl::CallDoubleMethodA,
    JNIImpl::CallVoidMethod,
    JNIImpl::CallVoidMethodV,
    JNIImpl::CallVoidMethodA,
    JNIImpl::CallNonvirtualObjectMethod,
    JNIImpl::CallNonvirtualObjectMethodV,
    JNIImpl::CallNonvirtualObjectMethodA,
    JNIImpl::CallNonvirtualBooleanMethod,
    JNIImpl::CallNonvirtualBooleanMethodV,
    JNIImpl::CallNonvirtualBooleanMethodA,
    JNIImpl::CallNonvirtualByteMethod,
    JNIImpl::CallNonvirtualByteMethodV,
    JNIImpl::CallNonvirtualByteMethodA,
    JNIImpl::CallNonvirtualCharMethod,
    JNIImpl::CallNonvirtualCharMethodV,
    JNIImpl::CallNonvirtualCharMethodA,
    JNIImpl::CallNonvirtualShortMethod,
    JNIImpl::CallNonvirtualShortMethodV,
    JNIImpl::CallNonvirtualShortMethodA,
    JNIImpl::CallNonvirtualIntMethod,
    JNIImpl::CallNonvirtualIntMethodV,
    JNIImpl::CallNonvirtualIntMethodA,
    JNIImpl::CallNonvirtualLongMethod,
    JNIImpl::CallNonvirtualLongMethodV,
    JNIImpl::CallNonvirtualLongMethodA,
    JNIImpl::CallNonvirtualFloatMethod,
    JNIImpl::CallNonvirtualFloatMethodV,
    JNIImpl::CallNonvirtualFloatMethodA,
    JNIImpl::CallNonvirtualDoubleMethod,
    JNIImpl::CallNonvirtualDoubleMethodV,
    JNIImpl::CallNonvirtualDoubleMethodA,
    JNIImpl::CallNonvirtualVoidMethod,
    JNIImpl::CallNonvirtualVoidMethodV,
    JNIImpl::CallNonvirtualVoidMethodA,
    JNIImpl::GetFieldID,
    JNIImpl::GetObjectField,
    JNIImpl::GetBooleanField,
    JNIImpl::GetByteField,
    JNIImpl::GetCharField,
    JNIImpl::GetShortField,
    JNIImpl::GetIntField,
    JNIImpl::GetLongField,
    JNIImpl::GetFloatField,
    JNIImpl::GetDoubleField,
    JNIImpl::SetObjectField,
    JNIImpl::SetBooleanField,
    JNIImpl::SetByteField,
    JNIImpl::SetCharField,
    JNIImpl::SetShortField,
    JNIImpl::SetIntField,
    JNIImpl::SetLongField,
    JNIImpl::SetFloatField,
    JNIImpl::SetDoubleField,
    JNIImpl::GetStaticMethodID,
    JNIImpl::CallStaticObjectMethod,
    JNIImpl::CallStaticObjectMethodV,
    JNIImpl::CallStaticObjectMethodA,
    JNIImpl::CallStaticBooleanMethod,
    JNIImpl::CallStaticBooleanMethodV,
    JNIImpl::CallStaticBooleanMethodA,
    JNIImpl::CallStaticByteMethod,
    JNIImpl::CallStaticByteMethodV,
    JNIImpl::CallStaticByteMethodA,
    JNIImpl::CallStaticCharMethod,
    JNIImpl::CallStaticCharMethodV,
    JNIImpl::CallStaticCharMethodA,
    JNIImpl::CallStaticShortMethod,
    JNIImpl::CallStaticShortMethodV,
    JNIImpl::CallStaticShortMethodA,
    JNIImpl::CallStaticIntMethod,
    JNIImpl::CallStaticIntMethodV,
    JNIImpl::CallStaticIntMethodA,
    JNIImpl::CallStaticLongMethod,
    JNIImpl::CallStaticLongMethodV,
    JNIImpl::CallStaticLongMethodA,
    JNIImpl::CallStaticFloatMethod,
    JNIImpl::CallStaticFloatMethodV,
    JNIImpl::CallStaticFloatMethodA,
    JNIImpl::CallStaticDoubleMethod,
    JNIImpl::CallStaticDoubleMethodV,
    JNIImpl::CallStaticDoubleMethodA,
    JNIImpl::CallStaticVoidMethod,
    JNIImpl::CallStaticVoidMethodV,
    JNIImpl::CallStaticVoidMethodA,
    JNIImpl::GetStaticFieldID,
    JNIImpl::GetStaticObjectField,
    JNIImpl::GetStaticBooleanField,
    JNIImpl::GetStaticByteField,
    JNIImpl::GetStaticCharField,
    JNIImpl::GetStaticShortField,
    JNIImpl::GetStaticIntField,
    JNIImpl::GetStaticLongField,
    JNIImpl::GetStaticFloatField,
    JNIImpl::GetStaticDoubleField,
    JNIImpl::SetStaticObjectField,
    JNIImpl::SetStaticBooleanField,
    JNIImpl::SetStaticByteField,
    JNIImpl::SetStaticCharField,
    JNIImpl::SetStaticShortField,
    JNIImpl::SetStaticIntField,
    JNIImpl::SetStaticLongField,
    JNIImpl::SetStaticFloatField,
    JNIImpl::SetStaticDoubleField,
    JNIImpl::NewString,
    JNIImpl::GetStringLength,
    JNIImpl::GetStringChars,
    JNIImpl::ReleaseStringChars,
    JNIImpl::NewStringUTF,
    JNIImpl::GetStringUTFLength,
    JNIImpl::GetStringUTFChars,
    JNIImpl::ReleaseStringUTFChars,
    JNIImpl::GetArrayLength,
    JNIImpl::NewObjectArray,
    JNIImpl::GetObjectArrayElement,
    JNIImpl::SetObjectArrayElement,
    JNIImpl::NewBooleanArray,
    JNIImpl::NewByteArray,
    JNIImpl::NewCharArray,
    JNIImpl::NewShortArray,
    JNIImpl::NewIntArray,
    JNIImpl::NewLongArray,
    JNIImpl::NewFloatArray,
    JNIImpl::NewDoubleArray,
    JNIImpl::GetBooleanArrayElements,
    JNIImpl::GetByteArrayElements,
    JNIImpl::GetCharArrayElements,
    JNIImpl::GetShortArrayElements,
    JNIImpl::GetIntArrayElements,
    JNIImpl::GetLongArrayElements,
    JNIImpl::GetFloatArrayElements,
    JNIImpl::GetDoubleArrayElements,
    JNIImpl::ReleaseBooleanArrayElements,
    JNIImpl::ReleaseByteArrayElements,
    JNIImpl::ReleaseCharArrayElements,
    JNIImpl::ReleaseShortArrayElements,
    JNIImpl::ReleaseIntArrayElements,
    JNIImpl::ReleaseLongArrayElements,
    JNIImpl::ReleaseFloatArrayElements,
    JNIImpl::ReleaseDoubleArrayElements,
    JNIImpl::GetBooleanArrayRegion,
    JNIImpl::GetByteArrayRegion,
    JNIImpl::GetCharArrayRegion,
    JNIImpl::GetShortArrayRegion,
    JNIImpl::GetIntArrayRegion,
    JNIImpl::GetLongArrayRegion,
    JNIImpl::GetFloatArrayRegion,
    JNIImpl::GetDoubleArrayRegion,
    JNIImpl::SetBooleanArrayRegion,
    JNIImpl::SetByteArrayRegion,
    JNIImpl::SetCharArrayRegion,
    JNIImpl::SetShortArrayRegion,
    JNIImpl::SetIntArrayRegion,
    JNIImpl::SetLongArrayRegion,
    JNIImpl::SetFloatArrayRegion,
    JNIImpl::SetDoubleArrayRegion,
    JNIImpl::RegisterNatives,
    JNIImpl::UnregisterNatives,
    JNIImpl::MonitorEnter,
    JNIImpl::MonitorExit,
    JNIImpl::GetJavaVM,
    JNIImpl::GetStringRegion,
    JNIImpl::GetStringUTFRegion,
    JNIImpl::GetPrimitiveArrayCritical,
    JNIImpl::ReleasePrimitiveArrayCritical,
    JNIImpl::GetStringCritical,
    JNIImpl::ReleaseStringCritical,
    JNIImpl::NewWeakGlobalRef,
    JNIImpl::DeleteWeakGlobalRef,
    JNIImpl::ExceptionCheck,
    JNIImpl::NewDirectByteBuffer,
    JNIImpl::GetDirectBufferAddress,
    JNIImpl::GetDirectBufferCapacity,
    JNIImpl::GetObjectRefType,
  };
};

const JNINativeInterface* GetJniNativeInterface() {
  // The template argument is passed down through the Encode/DecodeArtMethod/Field calls so if
  // JniIdType is kPointer the calls will be a simple cast with no branches. This ensures that
  // the normal case is still fast.
  return Runtime::Current()->GetJniIdType() == JniIdType::kPointer
             ? &JniNativeInterfaceFunctions<false>::gJniNativeInterface
             : &JniNativeInterfaceFunctions<true>::gJniNativeInterface;
}

JNINativeInterface gJniSleepForeverStub = {
    nullptr,  // reserved0.
    nullptr,  // reserved1.
    nullptr,  // reserved2.
    nullptr,  // reserved3.
    reinterpret_cast<jint (*)(JNIEnv*)>(SleepForever),
    reinterpret_cast<jclass (*)(JNIEnv*, const char*, jobject, const jbyte*, jsize)>(SleepForever),
    reinterpret_cast<jclass (*)(JNIEnv*, const char*)>(SleepForever),
    reinterpret_cast<jmethodID (*)(JNIEnv*, jobject)>(SleepForever),
    reinterpret_cast<jfieldID (*)(JNIEnv*, jobject)>(SleepForever),
    reinterpret_cast<jobject (*)(JNIEnv*, jclass, jmethodID, jboolean)>(SleepForever),
    reinterpret_cast<jclass (*)(JNIEnv*, jclass)>(SleepForever),
    reinterpret_cast<jboolean (*)(JNIEnv*, jclass, jclass)>(SleepForever),
    reinterpret_cast<jobject (*)(JNIEnv*, jclass, jfieldID, jboolean)>(SleepForever),
    reinterpret_cast<jint (*)(JNIEnv*, jthrowable)>(SleepForever),
    reinterpret_cast<jint (*)(JNIEnv*, jclass, const char*)>(SleepForever),
    reinterpret_cast<jthrowable (*)(JNIEnv*)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, const char*)>(SleepForever),
    reinterpret_cast<jint (*)(JNIEnv*, jint)>(SleepForever),
    reinterpret_cast<jobject (*)(JNIEnv*, jobject)>(SleepForever),
    reinterpret_cast<jobject (*)(JNIEnv*, jobject)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jobject)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jobject)>(SleepForever),
    reinterpret_cast<jboolean (*)(JNIEnv*, jobject, jobject)>(SleepForever),
    reinterpret_cast<jobject (*)(JNIEnv*, jobject)>(SleepForever),
    reinterpret_cast<jint (*)(JNIEnv*, jint)>(SleepForever),
    reinterpret_cast<jobject (*)(JNIEnv*, jclass)>(SleepForever),
    reinterpret_cast<jobject (*)(JNIEnv*, jclass, jmethodID, ...)>(SleepForever),
    reinterpret_cast<jobject (*)(JNIEnv*, jclass, jmethodID, va_list)>(SleepForever),
    reinterpret_cast<jobject (*)(JNIEnv*, jclass, jmethodID, const jvalue*)>(SleepForever),
    reinterpret_cast<jclass (*)(JNIEnv*, jobject)>(SleepForever),
    reinterpret_cast<jboolean (*)(JNIEnv*, jobject, jclass)>(SleepForever),
    reinterpret_cast<jmethodID (*)(JNIEnv*, jclass, const char*, const char*)>(SleepForever),
    reinterpret_cast<jobject (*)(JNIEnv*, jobject, jmethodID, ...)>(SleepForever),
    reinterpret_cast<jobject (*)(JNIEnv*, jobject, jmethodID, va_list)>(SleepForever),
    reinterpret_cast<jobject (*)(JNIEnv*, jobject, jmethodID, const jvalue*)>(SleepForever),
    reinterpret_cast<jboolean (*)(JNIEnv*, jobject, jmethodID, ...)>(SleepForever),
    reinterpret_cast<jboolean (*)(JNIEnv*, jobject, jmethodID, va_list)>(SleepForever),
    reinterpret_cast<jboolean (*)(JNIEnv*, jobject, jmethodID, const jvalue*)>(SleepForever),
    reinterpret_cast<jbyte (*)(JNIEnv*, jobject, jmethodID, ...)>(SleepForever),
    reinterpret_cast<jbyte (*)(JNIEnv*, jobject, jmethodID, va_list)>(SleepForever),
    reinterpret_cast<jbyte (*)(JNIEnv*, jobject, jmethodID, const jvalue*)>(SleepForever),
    reinterpret_cast<jchar (*)(JNIEnv*, jobject, jmethodID, ...)>(SleepForever),
    reinterpret_cast<jchar (*)(JNIEnv*, jobject, jmethodID, va_list)>(SleepForever),
    reinterpret_cast<jchar (*)(JNIEnv*, jobject, jmethodID, const jvalue*)>(SleepForever),
    reinterpret_cast<jshort (*)(JNIEnv*, jobject, jmethodID, ...)>(SleepForever),
    reinterpret_cast<jshort (*)(JNIEnv*, jobject, jmethodID, va_list)>(SleepForever),
    reinterpret_cast<jshort (*)(JNIEnv*, jobject, jmethodID, const jvalue*)>(SleepForever),
    reinterpret_cast<jint (*)(JNIEnv*, jobject, jmethodID, ...)>(SleepForever),
    reinterpret_cast<jint (*)(JNIEnv*, jobject, jmethodID, va_list)>(SleepForever),
    reinterpret_cast<jint (*)(JNIEnv*, jobject, jmethodID, const jvalue*)>(SleepForever),
    reinterpret_cast<jlong (*)(JNIEnv*, jobject, jmethodID, ...)>(SleepForever),
    reinterpret_cast<jlong (*)(JNIEnv*, jobject, jmethodID, va_list)>(SleepForever),
    reinterpret_cast<jlong (*)(JNIEnv*, jobject, jmethodID, const jvalue*)>(SleepForever),
    reinterpret_cast<jfloat (*)(JNIEnv*, jobject, jmethodID, ...)>(SleepForever),
    reinterpret_cast<jfloat (*)(JNIEnv*, jobject, jmethodID, va_list)>(SleepForever),
    reinterpret_cast<jfloat (*)(JNIEnv*, jobject, jmethodID, const jvalue*)>(SleepForever),
    reinterpret_cast<jdouble (*)(JNIEnv*, jobject, jmethodID, ...)>(SleepForever),
    reinterpret_cast<jdouble (*)(JNIEnv*, jobject, jmethodID, va_list)>(SleepForever),
    reinterpret_cast<jdouble (*)(JNIEnv*, jobject, jmethodID, const jvalue*)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jobject, jmethodID, ...)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jobject, jmethodID, va_list)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jobject, jmethodID, const jvalue*)>(SleepForever),
    reinterpret_cast<jobject (*)(JNIEnv*, jobject, jclass, jmethodID, ...)>(SleepForever),
    reinterpret_cast<jobject (*)(JNIEnv*, jobject, jclass, jmethodID, va_list)>(SleepForever),
    reinterpret_cast<jobject (*)(JNIEnv*, jobject, jclass, jmethodID, const jvalue*)>(SleepForever),
    reinterpret_cast<jboolean (*)(JNIEnv*, jobject, jclass, jmethodID, ...)>(SleepForever),
    reinterpret_cast<jboolean (*)(JNIEnv*, jobject, jclass, jmethodID, va_list)>(SleepForever),
    reinterpret_cast<jboolean (*)(JNIEnv*, jobject, jclass, jmethodID, const jvalue*)>(
        SleepForever),
    reinterpret_cast<jbyte (*)(JNIEnv*, jobject, jclass, jmethodID, ...)>(SleepForever),
    reinterpret_cast<jbyte (*)(JNIEnv*, jobject, jclass, jmethodID, va_list)>(SleepForever),
    reinterpret_cast<jbyte (*)(JNIEnv*, jobject, jclass, jmethodID, const jvalue*)>(SleepForever),
    reinterpret_cast<jchar (*)(JNIEnv*, jobject, jclass, jmethodID, ...)>(SleepForever),
    reinterpret_cast<jchar (*)(JNIEnv*, jobject, jclass, jmethodID, va_list)>(SleepForever),
    reinterpret_cast<jchar (*)(JNIEnv*, jobject, jclass, jmethodID, const jvalue*)>(SleepForever),
    reinterpret_cast<jshort (*)(JNIEnv*, jobject, jclass, jmethodID, ...)>(SleepForever),
    reinterpret_cast<jshort (*)(JNIEnv*, jobject, jclass, jmethodID, va_list)>(SleepForever),
    reinterpret_cast<jshort (*)(JNIEnv*, jobject, jclass, jmethodID, const jvalue*)>(SleepForever),
    reinterpret_cast<jint (*)(JNIEnv*, jobject, jclass, jmethodID, ...)>(SleepForever),
    reinterpret_cast<jint (*)(JNIEnv*, jobject, jclass, jmethodID, va_list)>(SleepForever),
    reinterpret_cast<jint (*)(JNIEnv*, jobject, jclass, jmethodID, const jvalue*)>(SleepForever),
    reinterpret_cast<jlong (*)(JNIEnv*, jobject, jclass, jmethodID, ...)>(SleepForever),
    reinterpret_cast<jlong (*)(JNIEnv*, jobject, jclass, jmethodID, va_list)>(SleepForever),
    reinterpret_cast<jlong (*)(JNIEnv*, jobject, jclass, jmethodID, const jvalue*)>(SleepForever),
    reinterpret_cast<jfloat (*)(JNIEnv*, jobject, jclass, jmethodID, ...)>(SleepForever),
    reinterpret_cast<jfloat (*)(JNIEnv*, jobject, jclass, jmethodID, va_list)>(SleepForever),
    reinterpret_cast<jfloat (*)(JNIEnv*, jobject, jclass, jmethodID, const jvalue*)>(SleepForever),
    reinterpret_cast<jdouble (*)(JNIEnv*, jobject, jclass, jmethodID, ...)>(SleepForever),
    reinterpret_cast<jdouble (*)(JNIEnv*, jobject, jclass, jmethodID, va_list)>(SleepForever),
    reinterpret_cast<jdouble (*)(JNIEnv*, jobject, jclass, jmethodID, const jvalue*)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jobject, jclass, jmethodID, ...)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jobject, jclass, jmethodID, va_list)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jobject, jclass, jmethodID, const jvalue*)>(SleepForever),
    reinterpret_cast<jfieldID (*)(JNIEnv*, jclass, const char*, const char*)>(SleepForever),
    reinterpret_cast<jobject (*)(JNIEnv*, jobject, jfieldID)>(SleepForever),
    reinterpret_cast<jboolean (*)(JNIEnv*, jobject, jfieldID)>(SleepForever),
    reinterpret_cast<jbyte (*)(JNIEnv*, jobject, jfieldID)>(SleepForever),
    reinterpret_cast<jchar (*)(JNIEnv*, jobject, jfieldID)>(SleepForever),
    reinterpret_cast<jshort (*)(JNIEnv*, jobject, jfieldID)>(SleepForever),
    reinterpret_cast<jint (*)(JNIEnv*, jobject, jfieldID)>(SleepForever),
    reinterpret_cast<jlong (*)(JNIEnv*, jobject, jfieldID)>(SleepForever),
    reinterpret_cast<jfloat (*)(JNIEnv*, jobject, jfieldID)>(SleepForever),
    reinterpret_cast<jdouble (*)(JNIEnv*, jobject, jfieldID)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jobject, jfieldID, jobject)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jobject, jfieldID, jboolean)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jobject, jfieldID, jbyte)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jobject, jfieldID, jchar)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jobject, jfieldID, jshort)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jobject, jfieldID, jint)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jobject, jfieldID, jlong)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jobject, jfieldID, jfloat)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jobject, jfieldID, jdouble)>(SleepForever),
    reinterpret_cast<jmethodID (*)(JNIEnv*, jclass, const char*, const char*)>(SleepForever),
    reinterpret_cast<jobject (*)(JNIEnv*, jclass, jmethodID, ...)>(SleepForever),
    reinterpret_cast<jobject (*)(JNIEnv*, jclass, jmethodID, va_list)>(SleepForever),
    reinterpret_cast<jobject (*)(JNIEnv*, jclass, jmethodID, const jvalue*)>(SleepForever),
    reinterpret_cast<jboolean (*)(JNIEnv*, jclass, jmethodID, ...)>(SleepForever),
    reinterpret_cast<jboolean (*)(JNIEnv*, jclass, jmethodID, va_list)>(SleepForever),
    reinterpret_cast<jboolean (*)(JNIEnv*, jclass, jmethodID, const jvalue*)>(SleepForever),
    reinterpret_cast<jbyte (*)(JNIEnv*, jclass, jmethodID, ...)>(SleepForever),
    reinterpret_cast<jbyte (*)(JNIEnv*, jclass, jmethodID, va_list)>(SleepForever),
    reinterpret_cast<jbyte (*)(JNIEnv*, jclass, jmethodID, const jvalue*)>(SleepForever),
    reinterpret_cast<jchar (*)(JNIEnv*, jclass, jmethodID, ...)>(SleepForever),
    reinterpret_cast<jchar (*)(JNIEnv*, jclass, jmethodID, va_list)>(SleepForever),
    reinterpret_cast<jchar (*)(JNIEnv*, jclass, jmethodID, const jvalue*)>(SleepForever),
    reinterpret_cast<jshort (*)(JNIEnv*, jclass, jmethodID, ...)>(SleepForever),
    reinterpret_cast<jshort (*)(JNIEnv*, jclass, jmethodID, va_list)>(SleepForever),
    reinterpret_cast<jshort (*)(JNIEnv*, jclass, jmethodID, const jvalue*)>(SleepForever),
    reinterpret_cast<jint (*)(JNIEnv*, jclass, jmethodID, ...)>(SleepForever),
    reinterpret_cast<jint (*)(JNIEnv*, jclass, jmethodID, va_list)>(SleepForever),
    reinterpret_cast<jint (*)(JNIEnv*, jclass, jmethodID, const jvalue*)>(SleepForever),
    reinterpret_cast<jlong (*)(JNIEnv*, jclass, jmethodID, ...)>(SleepForever),
    reinterpret_cast<jlong (*)(JNIEnv*, jclass, jmethodID, va_list)>(SleepForever),
    reinterpret_cast<jlong (*)(JNIEnv*, jclass, jmethodID, const jvalue*)>(SleepForever),
    reinterpret_cast<jfloat (*)(JNIEnv*, jclass, jmethodID, ...)>(SleepForever),
    reinterpret_cast<jfloat (*)(JNIEnv*, jclass, jmethodID, va_list)>(SleepForever),
    reinterpret_cast<jfloat (*)(JNIEnv*, jclass, jmethodID, const jvalue*)>(SleepForever),
    reinterpret_cast<jdouble (*)(JNIEnv*, jclass, jmethodID, ...)>(SleepForever),
    reinterpret_cast<jdouble (*)(JNIEnv*, jclass, jmethodID, va_list)>(SleepForever),
    reinterpret_cast<jdouble (*)(JNIEnv*, jclass, jmethodID, const jvalue*)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jclass, jmethodID, ...)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jclass, jmethodID, va_list)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jclass, jmethodID, const jvalue*)>(SleepForever),
    reinterpret_cast<jfieldID (*)(JNIEnv*, jclass, const char*, const char*)>(SleepForever),
    reinterpret_cast<jobject (*)(JNIEnv*, jclass, jfieldID)>(SleepForever),
    reinterpret_cast<jboolean (*)(JNIEnv*, jclass, jfieldID)>(SleepForever),
    reinterpret_cast<jbyte (*)(JNIEnv*, jclass, jfieldID)>(SleepForever),
    reinterpret_cast<jchar (*)(JNIEnv*, jclass, jfieldID)>(SleepForever),
    reinterpret_cast<jshort (*)(JNIEnv*, jclass, jfieldID)>(SleepForever),
    reinterpret_cast<jint (*)(JNIEnv*, jclass, jfieldID)>(SleepForever),
    reinterpret_cast<jlong (*)(JNIEnv*, jclass, jfieldID)>(SleepForever),
    reinterpret_cast<jfloat (*)(JNIEnv*, jclass, jfieldID)>(SleepForever),
    reinterpret_cast<jdouble (*)(JNIEnv*, jclass, jfieldID)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jclass, jfieldID, jobject)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jclass, jfieldID, jboolean)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jclass, jfieldID, jbyte)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jclass, jfieldID, jchar)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jclass, jfieldID, jshort)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jclass, jfieldID, jint)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jclass, jfieldID, jlong)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jclass, jfieldID, jfloat)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jclass, jfieldID, jdouble)>(SleepForever),
    reinterpret_cast<jstring (*)(JNIEnv*, const jchar*, jsize)>(SleepForever),
    reinterpret_cast<jsize (*)(JNIEnv*, jstring)>(SleepForever),
    reinterpret_cast<const jchar* (*)(JNIEnv*, jstring, jboolean*)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jstring, const jchar*)>(SleepForever),
    reinterpret_cast<jstring (*)(JNIEnv*, const char*)>(SleepForever),
    reinterpret_cast<jsize (*)(JNIEnv*, jstring)>(SleepForever),
    reinterpret_cast<const char* (*)(JNIEnv*, jstring, jboolean*)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jstring, const char*)>(SleepForever),
    reinterpret_cast<jsize (*)(JNIEnv*, jarray)>(SleepForever),
    reinterpret_cast<jobjectArray (*)(JNIEnv*, jsize, jclass, jobject)>(SleepForever),
    reinterpret_cast<jobject (*)(JNIEnv*, jobjectArray, jsize)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jobjectArray, jsize, jobject)>(SleepForever),
    reinterpret_cast<jbooleanArray (*)(JNIEnv*, jsize)>(SleepForever),
    reinterpret_cast<jbyteArray (*)(JNIEnv*, jsize)>(SleepForever),
    reinterpret_cast<jcharArray (*)(JNIEnv*, jsize)>(SleepForever),
    reinterpret_cast<jshortArray (*)(JNIEnv*, jsize)>(SleepForever),
    reinterpret_cast<jintArray (*)(JNIEnv*, jsize)>(SleepForever),
    reinterpret_cast<jlongArray (*)(JNIEnv*, jsize)>(SleepForever),
    reinterpret_cast<jfloatArray (*)(JNIEnv*, jsize)>(SleepForever),
    reinterpret_cast<jdoubleArray (*)(JNIEnv*, jsize)>(SleepForever),
    reinterpret_cast<jboolean* (*)(JNIEnv*, jbooleanArray, jboolean*)>(SleepForever),
    reinterpret_cast<jbyte* (*)(JNIEnv*, jbyteArray, jboolean*)>(SleepForever),
    reinterpret_cast<jchar* (*)(JNIEnv*, jcharArray, jboolean*)>(SleepForever),
    reinterpret_cast<jshort* (*)(JNIEnv*, jshortArray, jboolean*)>(SleepForever),
    reinterpret_cast<jint* (*)(JNIEnv*, jintArray, jboolean*)>(SleepForever),
    reinterpret_cast<jlong* (*)(JNIEnv*, jlongArray, jboolean*)>(SleepForever),
    reinterpret_cast<jfloat* (*)(JNIEnv*, jfloatArray, jboolean*)>(SleepForever),
    reinterpret_cast<jdouble* (*)(JNIEnv*, jdoubleArray, jboolean*)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jbooleanArray, jboolean*, jint)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jbyteArray, jbyte*, jint)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jcharArray, jchar*, jint)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jshortArray, jshort*, jint)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jintArray, jint*, jint)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jlongArray, jlong*, jint)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jfloatArray, jfloat*, jint)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jdoubleArray, jdouble*, jint)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jbooleanArray, jsize, jsize, jboolean*)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jbyteArray, jsize, jsize, jbyte*)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jcharArray, jsize, jsize, jchar*)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jshortArray, jsize, jsize, jshort*)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jintArray, jsize, jsize, jint*)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jlongArray, jsize, jsize, jlong*)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jfloatArray, jsize, jsize, jfloat*)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jdoubleArray, jsize, jsize, jdouble*)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jbooleanArray, jsize, jsize, const jboolean*)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jbyteArray, jsize, jsize, const jbyte*)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jcharArray, jsize, jsize, const jchar*)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jshortArray, jsize, jsize, const jshort*)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jintArray, jsize, jsize, const jint*)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jlongArray, jsize, jsize, const jlong*)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jfloatArray, jsize, jsize, const jfloat*)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jdoubleArray, jsize, jsize, const jdouble*)>(SleepForever),
    reinterpret_cast<jint (*)(JNIEnv*, jclass, const JNINativeMethod*, jint)>(SleepForever),
    reinterpret_cast<jint (*)(JNIEnv*, jclass)>(SleepForever),
    reinterpret_cast<jint (*)(JNIEnv*, jobject)>(SleepForever),
    reinterpret_cast<jint (*)(JNIEnv*, jobject)>(SleepForever),
    reinterpret_cast<jint (*)(JNIEnv*, JavaVM**)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jstring, jsize, jsize, jchar*)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jstring, jsize, jsize, char*)>(SleepForever),
    reinterpret_cast<void* (*)(JNIEnv*, jarray, jboolean*)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jarray, void*, jint)>(SleepForever),
    reinterpret_cast<const jchar* (*)(JNIEnv*, jstring, jboolean*)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jstring, const jchar*)>(SleepForever),
    reinterpret_cast<jweak (*)(JNIEnv*, jobject)>(SleepForever),
    reinterpret_cast<void (*)(JNIEnv*, jweak)>(SleepForever),
    reinterpret_cast<jboolean (*)(JNIEnv*)>(SleepForever),
    reinterpret_cast<jobject (*)(JNIEnv*, void*, jlong)>(SleepForever),
    reinterpret_cast<void* (*)(JNIEnv*, jobject)>(SleepForever),
    reinterpret_cast<jlong (*)(JNIEnv*, jobject)>(SleepForever),
    reinterpret_cast<jobjectRefType (*)(JNIEnv*, jobject)>(SleepForever),
};

const JNINativeInterface* GetRuntimeShutdownNativeInterface() {
  return &gJniSleepForeverStub;
}

}  // namespace art

std::ostream& operator<<(std::ostream& os, const jobjectRefType& rhs) {
  switch (rhs) {
  case JNIInvalidRefType:
    os << "JNIInvalidRefType";
    return os;
  case JNILocalRefType:
    os << "JNILocalRefType";
    return os;
  case JNIGlobalRefType:
    os << "JNIGlobalRefType";
    return os;
  case JNIWeakGlobalRefType:
    os << "JNIWeakGlobalRefType";
    return os;
  default:
    LOG(FATAL) << "jobjectRefType[" << static_cast<int>(rhs) << "]";
    UNREACHABLE();
  }
}
