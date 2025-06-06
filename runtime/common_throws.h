/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef ART_RUNTIME_COMMON_THROWS_H_
#define ART_RUNTIME_COMMON_THROWS_H_

#include <string_view>

#include "base/locks.h"
#include "base/macros.h"
#include "obj_ptr.h"

namespace art HIDDEN {
namespace mirror {
class Class;
class Object;
class MethodType;
}  // namespace mirror
class ArtField;
class ArtMethod;
class DexFile;
enum InvokeType : uint32_t;
class Signature;
enum class StackType;

// The descriptor of the transaction abort exception.
constexpr const char kTransactionAbortErrorDescriptor[] = "Ldalvik/system/TransactionAbortError;";

// AbstractMethodError

void ThrowAbstractMethodError(ArtMethod* method, ObjPtr<mirror::Object> receiver)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

void ThrowAbstractMethodError(uint32_t method_idx,
                              const DexFile& dex_file,
                              ObjPtr<mirror::Object> receiver)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

// ArithmeticException

EXPORT void ThrowArithmeticExceptionDivideByZero() REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

// ArrayIndexOutOfBoundsException

void ThrowArrayIndexOutOfBoundsException(int index, int length)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

// ArrayStoreException

void ThrowArrayStoreException(ObjPtr<mirror::Class> element_class,
                              ObjPtr<mirror::Class> array_class)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

// BootstrapMethodError

void ThrowBootstrapMethodError(const char* fmt, ...)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

void ThrowWrappedBootstrapMethodError(const char* fmt, ...)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

// ClassCircularityError

void ThrowClassCircularityError(ObjPtr<mirror::Class> c)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

void ThrowClassCircularityError(ObjPtr<mirror::Class> c, const char* fmt, ...)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

// ClassCastException

EXPORT void ThrowClassCastException(ObjPtr<mirror::Class> dest_type, ObjPtr<mirror::Class> src_type)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

void ThrowClassCastException(const char* msg)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

// ClassFormatError

EXPORT void ThrowClassFormatError(ObjPtr<mirror::Class> referrer, const char* fmt, ...)
    __attribute__((__format__(__printf__, 2, 3))) REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

// IllegalAccessError

EXPORT void ThrowIllegalAccessErrorClass(ObjPtr<mirror::Class> referrer,
                                         ObjPtr<mirror::Class> accessed)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

void ThrowIllegalAccessErrorClassForMethodDispatch(ObjPtr<mirror::Class> referrer,
                                                   ObjPtr<mirror::Class> accessed,
                                                   ArtMethod* called,
                                                   InvokeType type)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

void ThrowIllegalAccessErrorMethod(ObjPtr<mirror::Class> referrer, ArtMethod* accessed)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

EXPORT void ThrowIllegalAccessErrorField(ObjPtr<mirror::Class> referrer, ArtField* accessed)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

EXPORT void ThrowIllegalAccessErrorFinalField(ArtMethod* referrer, ArtField* accessed)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

EXPORT void ThrowIllegalAccessError(ObjPtr<mirror::Class> referrer, const char* fmt, ...)
    __attribute__((__format__(__printf__, 2, 3)))
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

void ThrowIllegalAccessErrorForImplementingMethod(ObjPtr<mirror::Class> klass,
                                                  ArtMethod* implementation_method,
                                                  ArtMethod* interface_method)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

// IllegalAccessException

void ThrowIllegalAccessException(const char* msg)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

// IllegalArgumentException

void ThrowIllegalArgumentException(const char* msg)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

// IllegalAccessException

EXPORT void ThrowIllegalStateException(const char* msg)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

// IncompatibleClassChangeError

void ThrowIncompatibleClassChangeError(InvokeType expected_type,
                                       InvokeType found_type,
                                       ArtMethod* method,
                                       ArtMethod* referrer)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

EXPORT void ThrowIncompatibleClassChangeErrorClassForInterfaceDispatch(
    ArtMethod* interface_method, ObjPtr<mirror::Object> this_object, ArtMethod* referrer)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

EXPORT void ThrowIncompatibleClassChangeErrorField(
    ArtField* resolved_field, bool is_static, ArtMethod* referrer)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

void ThrowIncompatibleClassChangeError(ObjPtr<mirror::Class> referrer, const char* fmt, ...)
    __attribute__((__format__(__printf__, 2, 3)))
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

void ThrowIncompatibleClassChangeErrorForMethodConflict(ArtMethod* method)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

// IndexOutOfBoundsException

void ThrowIndexOutOfBoundsException(int index, int length)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

// InternalError

void ThrowInternalError(const char* fmt, ...)
    __attribute__((__format__(__printf__, 1, 2)))
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

// IOException

void ThrowIOException(const char* fmt, ...) __attribute__((__format__(__printf__, 1, 2)))
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

void ThrowWrappedIOException(const char* fmt, ...) __attribute__((__format__(__printf__, 1, 2)))
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

// LinkageError

void ThrowLinkageError(ObjPtr<mirror::Class> referrer, const char* fmt, ...)
    __attribute__((__format__(__printf__, 2, 3)))
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

void ThrowWrappedLinkageError(ObjPtr<mirror::Class> referrer, const char* fmt, ...)
    __attribute__((__format__(__printf__, 2, 3)))
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

// NegativeArraySizeException

EXPORT void ThrowNegativeArraySizeException(int size)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

void ThrowNegativeArraySizeException(const char* msg)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;


// NoSuchFieldError

EXPORT void ThrowNoSuchFieldError(std::string_view scope,
                                  ObjPtr<mirror::Class> c,
                                  std::string_view type,
                                  std::string_view name)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

void ThrowNoSuchFieldException(ObjPtr<mirror::Class> c, std::string_view name)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

// NoSuchMethodError

void ThrowNoSuchMethodError(InvokeType type,
                            ObjPtr<mirror::Class> c,
                            std::string_view name,
                            const Signature& signature)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

void ThrowNoSuchMethodError(ObjPtr<mirror::Class> c,
                            std::string_view name,
                            const Signature& signature)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

// NullPointerException
EXPORT
void ThrowNullPointerExceptionForFieldAccess(ArtField* field, ArtMethod* method, bool is_read)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

EXPORT void ThrowNullPointerExceptionForMethodAccess(uint32_t method_idx, InvokeType type)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

void ThrowNullPointerExceptionForMethodAccess(ArtMethod* method, InvokeType type)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

void ThrowNullPointerExceptionFromDexPC(bool check_address = false, uintptr_t addr = 0)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

EXPORT void ThrowNullPointerException(const char* msg)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

EXPORT void ThrowNullPointerException()
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

// ReadOnlyBufferException

void ThrowReadOnlyBufferException() REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

// RuntimeException

void ThrowRuntimeException(const char* fmt, ...)
    __attribute__((__format__(__printf__, 1, 2)))
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

// SecurityException

void ThrowSecurityException(const char* fmt, ...)
    __attribute__((__format__(__printf__, 1, 2)))
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

// Stack overflow.

template <StackType stack_type>
void ThrowStackOverflowError(Thread* self) REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

// StringIndexOutOfBoundsException

void ThrowStringIndexOutOfBoundsException(int index, int length)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

// UnsupportedOperationException

void ThrowUnsupportedOperationException() REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

// VerifyError

void ThrowVerifyError(ObjPtr<mirror::Class> referrer, const char* fmt, ...)
    __attribute__((__format__(__printf__, 2, 3)))
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

// WrongMethodTypeException

void ThrowWrongMethodTypeException(ObjPtr<mirror::MethodType> callee_type,
                                   ObjPtr<mirror::MethodType> callsite_type)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

void ThrowWrongMethodTypeException(const std::string& expected_descriptor,
                                   const std::string& actual_descriptor)
    REQUIRES_SHARED(Locks::mutator_lock_) COLD_ATTR;

}  // namespace art

#endif  // ART_RUNTIME_COMMON_THROWS_H_
