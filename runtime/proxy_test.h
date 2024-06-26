/*
 * Copyright (C) 2018 The Android Open Source Project
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

#ifndef ART_RUNTIME_PROXY_TEST_H_
#define ART_RUNTIME_PROXY_TEST_H_

#include <jni.h>
#include <vector>

#include "art_method-inl.h"
#include "base/macros.h"
#include "class_linker-inl.h"
#include "class_root-inl.h"
#include "mirror/class-inl.h"
#include "mirror/method.h"
#include "obj_ptr-inl.h"

namespace art HIDDEN {
namespace proxy_test {

// Generate a proxy class with the given name and interfaces. This is a simplification from what
// libcore does to fit to our test needs. We do not check for duplicated interfaces or methods and
// we do not declare exceptions.
inline ObjPtr<mirror::Class> GenerateProxyClass(ScopedObjectAccess& soa,
                                                jobject jclass_loader,
                                                ClassLinker* class_linker,
                                                const char* className,
                                                const std::vector<Handle<mirror::Class>>& interfaces)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::Class> javaLangObject = hs.NewHandle(GetClassRoot<mirror::Object>());
  CHECK(javaLangObject != nullptr);

  jclass javaLangClass = soa.AddLocalReference<jclass>(GetClassRoot<mirror::Class>());

  // Builds the interfaces array.
  jobjectArray proxyClassInterfaces =
      soa.Env()->NewObjectArray(interfaces.size(), javaLangClass, /* initialElement= */ nullptr);
  soa.Self()->AssertNoPendingException();
  for (size_t i = 0; i < interfaces.size(); ++i) {
    soa.Env()->SetObjectArrayElement(proxyClassInterfaces, i,
                                     soa.AddLocalReference<jclass>(interfaces[i].Get()));
  }

  // Builds the method array.
  jsize methods_count = 3;  // Object.equals, Object.hashCode and Object.toString.
  for (Handle<mirror::Class> interface : interfaces) {
    methods_count += interface->NumVirtualMethods();
  }
  jobjectArray proxyClassMethods = soa.Env()->NewObjectArray(
      methods_count,
      soa.AddLocalReference<jclass>(GetClassRoot<mirror::Method>()),
      /* initialElement= */ nullptr);
  soa.Self()->AssertNoPendingException();

  jsize array_index = 0;
  // Fill the method array
  DCHECK_EQ(Runtime::Current()->GetClassLinker()->GetImagePointerSize(), kRuntimePointerSize);
  ArtMethod* method = javaLangObject->FindClassMethod(
      "equals", "(Ljava/lang/Object;)Z", kRuntimePointerSize);
  CHECK(method != nullptr);
  CHECK(!method->IsDirect());
  CHECK(method->GetDeclaringClass() == javaLangObject.Get());
  DCHECK(!Runtime::Current()->IsActiveTransaction());
  soa.Env()->SetObjectArrayElement(
      proxyClassMethods, array_index++, soa.AddLocalReference<jobject>(
          mirror::Method::CreateFromArtMethod<kRuntimePointerSize>(soa.Self(), method)));
  method = javaLangObject->FindClassMethod("hashCode", "()I", kRuntimePointerSize);
  CHECK(method != nullptr);
  CHECK(!method->IsDirect());
  CHECK(method->GetDeclaringClass() == javaLangObject.Get());
  soa.Env()->SetObjectArrayElement(
      proxyClassMethods, array_index++, soa.AddLocalReference<jobject>(
          mirror::Method::CreateFromArtMethod<kRuntimePointerSize>(soa.Self(), method)));
  method = javaLangObject->FindClassMethod(
      "toString", "()Ljava/lang/String;", kRuntimePointerSize);
  CHECK(method != nullptr);
  CHECK(!method->IsDirect());
  CHECK(method->GetDeclaringClass() == javaLangObject.Get());
  soa.Env()->SetObjectArrayElement(
      proxyClassMethods, array_index++, soa.AddLocalReference<jobject>(
          mirror::Method::CreateFromArtMethod<kRuntimePointerSize>(soa.Self(), method)));
  // Now adds all interfaces virtual methods.
  for (Handle<mirror::Class> interface : interfaces) {
    for (auto& m : interface->GetDeclaredVirtualMethods(kRuntimePointerSize)) {
      soa.Env()->SetObjectArrayElement(
          proxyClassMethods, array_index++, soa.AddLocalReference<jobject>(
              mirror::Method::CreateFromArtMethod<kRuntimePointerSize>(soa.Self(), &m)));
    }
  }
  CHECK_EQ(array_index, methods_count);

  // Builds an empty exception array.
  jobjectArray proxyClassThrows = soa.Env()->NewObjectArray(0, javaLangClass, nullptr);
  soa.Self()->AssertNoPendingException();

  ObjPtr<mirror::Class> proxyClass = class_linker->CreateProxyClass(
      soa,
      soa.Env()->NewStringUTF(className),
      proxyClassInterfaces,
      jclass_loader,
      proxyClassMethods,
      proxyClassThrows);
  soa.Self()->AssertNoPendingException();
  return proxyClass;
}

}  // namespace proxy_test
}  // namespace art

#endif  // ART_RUNTIME_PROXY_TEST_H_
