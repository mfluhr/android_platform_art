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

#include "art_method.h"

#include <algorithm>
#include <cstddef>

#include "android-base/stringprintf.h"

#include "arch/context.h"
#include "art_method-inl.h"
#include "base/pointer_size.h"
#include "base/stl_util.h"
#include "class_linker-inl.h"
#include "class_root-inl.h"
#include "debugger.h"
#include "dex/class_accessor-inl.h"
#include "dex/descriptors_names.h"
#include "dex/dex_file-inl.h"
#include "dex/dex_file_exception_helpers.h"
#include "dex/dex_instruction.h"
#include "dex/signature-inl.h"
#include "entrypoints/runtime_asm_entrypoints.h"
#include "gc/accounting/card_table-inl.h"
#include "hidden_api.h"
#include "interpreter/interpreter.h"
#include "intrinsics_enum.h"
#include "jit/jit.h"
#include "jit/jit_code_cache.h"
#include "jit/profiling_info.h"
#include "jni/jni_internal.h"
#include "mirror/class-inl.h"
#include "mirror/class_ext-inl.h"
#include "mirror/executable.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/string.h"
#include "oat/oat_file-inl.h"
#include "runtime_callbacks.h"
#include "scoped_thread_state_change-inl.h"
#include "vdex_file.h"

namespace art HIDDEN {

using android::base::StringPrintf;

extern "C" void art_quick_invoke_stub(ArtMethod*, uint32_t*, uint32_t, Thread*, JValue*,
                                      const char*);
extern "C" void art_quick_invoke_static_stub(ArtMethod*, uint32_t*, uint32_t, Thread*, JValue*,
                                             const char*);

// Enforce that we have the right index for runtime methods.
static_assert(ArtMethod::kRuntimeMethodDexMethodIndex == dex::kDexNoIndex,
              "Wrong runtime-method dex method index");

ArtMethod* ArtMethod::GetCanonicalMethod(PointerSize pointer_size) {
  if (LIKELY(!IsCopied())) {
    return this;
  } else {
    ObjPtr<mirror::Class> declaring_class = GetDeclaringClass();
    DCHECK(declaring_class->IsInterface());
    ArtMethod* ret = declaring_class->FindInterfaceMethod(GetDexCache(),
                                                          GetDexMethodIndex(),
                                                          pointer_size);
    DCHECK(ret != nullptr);
    return ret;
  }
}

ArtMethod* ArtMethod::GetNonObsoleteMethod() {
  if (LIKELY(!IsObsolete())) {
    return this;
  }
  DCHECK_EQ(kRuntimePointerSize, Runtime::Current()->GetClassLinker()->GetImagePointerSize());
  if (IsDirect()) {
    return &GetDeclaringClass()->GetDirectMethodsSlice(kRuntimePointerSize)[GetMethodIndex()];
  } else {
    return GetDeclaringClass()->GetVTableEntry(GetMethodIndex(), kRuntimePointerSize);
  }
}

ArtMethod* ArtMethod::GetSingleImplementation(PointerSize pointer_size) {
  if (IsInvokable()) {
    // An invokable method single implementation is itself.
    return this;
  }
  DCHECK(!IsDefaultConflicting());
  ArtMethod* m = reinterpret_cast<ArtMethod*>(GetDataPtrSize(pointer_size));
  CHECK(m == nullptr || !m->IsDefaultConflicting());
  return m;
}

ArtMethod* ArtMethod::FromReflectedMethod(const ScopedObjectAccessAlreadyRunnable& soa,
                                          jobject jlr_method) {
  ObjPtr<mirror::Executable> executable = soa.Decode<mirror::Executable>(jlr_method);
  DCHECK(executable != nullptr);
  return executable->GetArtMethod();
}

template <ReadBarrierOption kReadBarrierOption>
ObjPtr<mirror::DexCache> ArtMethod::GetObsoleteDexCache() {
  // Note: The class redefinition happens with GC disabled, so at the point where we
  // create obsolete methods, the `ClassExt` and its obsolete methods and dex caches
  // members are reachable without a read barrier. If we start a GC later, and we
  // look at these objects without read barriers (`kWithoutReadBarrier`), the method
  // pointers shall be the same in from-space array as in to-space array (if these
  // arrays are different) and the dex cache array entry can point to from-space or
  // to-space `DexCache` but either is a valid result for `kWithoutReadBarrier`.
  ScopedAssertNoThreadSuspension ants(__FUNCTION__);
  std::optional<ScopedDebugDisallowReadBarriers> sddrb(std::nullopt);
  if (kIsDebugBuild && kReadBarrierOption == kWithoutReadBarrier) {
    sddrb.emplace(Thread::Current());
  }
  PointerSize pointer_size = kRuntimePointerSize;
  DCHECK(!Runtime::Current()->IsAotCompiler()) << PrettyMethod();
  DCHECK(IsObsolete());
  ObjPtr<mirror::Class> declaring_class = GetDeclaringClass<kReadBarrierOption>();
  ObjPtr<mirror::ClassExt> ext =
      declaring_class->GetExtData<kDefaultVerifyFlags, kReadBarrierOption>();
  ObjPtr<mirror::PointerArray> obsolete_methods(
      ext.IsNull() ? nullptr : ext->GetObsoleteMethods<kDefaultVerifyFlags, kReadBarrierOption>());
  int32_t len = 0;
  ObjPtr<mirror::ObjectArray<mirror::DexCache>> obsolete_dex_caches = nullptr;
  if (!obsolete_methods.IsNull()) {
    len = obsolete_methods->GetLength();
    obsolete_dex_caches = ext->GetObsoleteDexCaches<kDefaultVerifyFlags, kReadBarrierOption>();
    // FIXME: `ClassExt::SetObsoleteArrays()` is not atomic, so one of the arrays we see here
    // could be extended for a new class redefinition while the other may be shorter.
    // Furthermore, there is no synchronization to ensure that copied contents of an old
    // obsolete array are visible to a thread reading the new array.
    DCHECK_EQ(len, obsolete_dex_caches->GetLength())
        << " ext->GetObsoleteDexCaches()=" << obsolete_dex_caches;
  }
  // Using kRuntimePointerSize (instead of using the image's pointer size) is fine since images
  // should never have obsolete methods in them so they should always be the same.
  DCHECK_EQ(pointer_size, Runtime::Current()->GetClassLinker()->GetImagePointerSize());
  for (int32_t i = 0; i < len; i++) {
    if (this == obsolete_methods->GetElementPtrSize<ArtMethod*>(i, pointer_size)) {
      return obsolete_dex_caches->GetWithoutChecks<kDefaultVerifyFlags, kReadBarrierOption>(i);
    }
  }
  CHECK(declaring_class->IsObsoleteObject())
      << "This non-structurally obsolete method does not appear in the obsolete map of its class: "
      << declaring_class->PrettyClass() << " Searched " << len << " caches.";
  CHECK_EQ(this,
           std::clamp(this,
                      &(*declaring_class->GetMethods(pointer_size).begin()),
                      &(*declaring_class->GetMethods(pointer_size).end())))
      << "class is marked as structurally obsolete method but not found in normal obsolete-map "
      << "despite not being the original method pointer for " << GetDeclaringClass()->PrettyClass();
  return declaring_class->template GetDexCache<kDefaultVerifyFlags, kReadBarrierOption>();
}

template ObjPtr<mirror::DexCache> ArtMethod::GetObsoleteDexCache<kWithReadBarrier>();
template ObjPtr<mirror::DexCache> ArtMethod::GetObsoleteDexCache<kWithoutReadBarrier>();

uint16_t ArtMethod::FindObsoleteDexClassDefIndex() {
  DCHECK(!Runtime::Current()->IsAotCompiler()) << PrettyMethod();
  DCHECK(IsObsolete());
  const DexFile* dex_file = GetDexFile();
  const dex::TypeIndex declaring_class_type = dex_file->GetMethodId(GetDexMethodIndex()).class_idx_;
  const dex::ClassDef* class_def = dex_file->FindClassDef(declaring_class_type);
  CHECK(class_def != nullptr);
  return dex_file->GetIndexForClassDef(*class_def);
}

void ArtMethod::ThrowInvocationTimeError(ObjPtr<mirror::Object> receiver) {
  DCHECK(!IsInvokable());
  if (IsDefaultConflicting()) {
    ThrowIncompatibleClassChangeErrorForMethodConflict(this);
  } else if (GetDeclaringClass()->IsInterface() && receiver != nullptr) {
    // If this was an interface call, check whether there is a method in the
    // superclass chain that isn't public. In this situation, we should throw an
    // IllegalAccessError.
    DCHECK(IsAbstract());
    ObjPtr<mirror::Class> current = receiver->GetClass();
    std::string_view name = GetNameView();
    Signature signature = GetSignature();
    while (current != nullptr) {
      for (ArtMethod& method : current->GetDeclaredMethodsSlice(kRuntimePointerSize)) {
        ArtMethod* np_method = method.GetInterfaceMethodIfProxy(kRuntimePointerSize);
        if (!np_method->IsStatic() &&
            np_method->GetNameView() == name &&
            np_method->GetSignature() == signature) {
          if (!np_method->IsPublic()) {
            ThrowIllegalAccessErrorForImplementingMethod(receiver->GetClass(), np_method, this);
            return;
          } else if (np_method->IsAbstract()) {
            ThrowAbstractMethodError(this, receiver);
            return;
          }
        }
      }
      current = current->GetSuperClass();
    }
    ThrowAbstractMethodError(this, receiver);
  } else {
    DCHECK(IsAbstract());
    ThrowAbstractMethodError(this, receiver);
  }
}

InvokeType ArtMethod::GetInvokeType() {
  // TODO: kSuper?
  if (IsStatic()) {
    return kStatic;
  } else if (GetDeclaringClass()->IsInterface()) {
    return kInterface;
  } else if (IsDirect()) {
    return kDirect;
  } else if (IsSignaturePolymorphic()) {
    return kPolymorphic;
  } else {
    return kVirtual;
  }
}

size_t ArtMethod::NumArgRegisters(std::string_view shorty) {
  CHECK(!shorty.empty());
  uint32_t num_registers = 0;
  for (char c : shorty.substr(1u)) {
    if (c == 'D' || c == 'J') {
      num_registers += 2;
    } else {
      num_registers += 1;
    }
  }
  return num_registers;
}

bool ArtMethod::HasSameNameAndSignature(ArtMethod* other) {
  ScopedAssertNoThreadSuspension ants("HasSameNameAndSignature");
  const DexFile* dex_file = GetDexFile();
  const dex::MethodId& mid = dex_file->GetMethodId(GetDexMethodIndex());
  if (GetDexCache() == other->GetDexCache()) {
    const dex::MethodId& mid2 = dex_file->GetMethodId(other->GetDexMethodIndex());
    return mid.name_idx_ == mid2.name_idx_ && mid.proto_idx_ == mid2.proto_idx_;
  }
  const DexFile* dex_file2 = other->GetDexFile();
  const dex::MethodId& mid2 = dex_file2->GetMethodId(other->GetDexMethodIndex());
  if (!DexFile::StringEquals(dex_file, mid.name_idx_, dex_file2, mid2.name_idx_)) {
    return false;  // Name mismatch.
  }
  return dex_file->GetMethodSignature(mid) == dex_file2->GetMethodSignature(mid2);
}

ArtMethod* ArtMethod::FindOverriddenMethod(PointerSize pointer_size) {
  if (IsStatic()) {
    return nullptr;
  }
  ObjPtr<mirror::Class> declaring_class = GetDeclaringClass();
  ObjPtr<mirror::Class> super_class = declaring_class->GetSuperClass();
  uint16_t method_index = GetMethodIndex();
  ArtMethod* result = nullptr;
  // Did this method override a super class method? If so load the result from the super class'
  // vtable
  if (super_class->HasVTable() && method_index < super_class->GetVTableLength()) {
    result = super_class->GetVTableEntry(method_index, pointer_size);
  } else {
    // Method didn't override superclass method so search interfaces
    if (IsProxyMethod()) {
      result = GetInterfaceMethodIfProxy(pointer_size);
      DCHECK(result != nullptr);
    } else {
      ObjPtr<mirror::IfTable> iftable = GetDeclaringClass()->GetIfTable();
      for (size_t i = 0; i < iftable->Count() && result == nullptr; i++) {
        ObjPtr<mirror::Class> interface = iftable->GetInterface(i);
        for (ArtMethod& interface_method : interface->GetVirtualMethods(pointer_size)) {
          if (HasSameNameAndSignature(interface_method.GetInterfaceMethodIfProxy(pointer_size))) {
            result = &interface_method;
            break;
          }
        }
      }
    }
  }
  DCHECK(result == nullptr ||
         GetInterfaceMethodIfProxy(pointer_size)->HasSameNameAndSignature(
             result->GetInterfaceMethodIfProxy(pointer_size)));
  return result;
}

uint32_t ArtMethod::FindDexMethodIndexInOtherDexFile(const DexFile& other_dexfile,
                                                     uint32_t name_and_signature_idx) {
  const DexFile* dexfile = GetDexFile();
  const uint32_t dex_method_idx = GetDexMethodIndex();
  const dex::MethodId& mid = dexfile->GetMethodId(dex_method_idx);
  const dex::MethodId& name_and_sig_mid = other_dexfile.GetMethodId(name_and_signature_idx);
  DCHECK_STREQ(dexfile->GetMethodName(mid), other_dexfile.GetMethodName(name_and_sig_mid));
  DCHECK_EQ(dexfile->GetMethodSignature(mid), other_dexfile.GetMethodSignature(name_and_sig_mid));
  if (dexfile == &other_dexfile) {
    return dex_method_idx;
  }
  std::string_view mid_declaring_class_descriptor = dexfile->GetTypeDescriptorView(mid.class_idx_);
  const dex::TypeId* other_type_id = other_dexfile.FindTypeId(mid_declaring_class_descriptor);
  if (other_type_id != nullptr) {
    const dex::MethodId* other_mid = other_dexfile.FindMethodId(
        *other_type_id, other_dexfile.GetStringId(name_and_sig_mid.name_idx_),
        other_dexfile.GetProtoId(name_and_sig_mid.proto_idx_));
    if (other_mid != nullptr) {
      return other_dexfile.GetIndexForMethodId(*other_mid);
    }
  }
  return dex::kDexNoIndex;
}

uint32_t ArtMethod::FindCatchBlock(Handle<mirror::Class> exception_type,
                                   uint32_t dex_pc, bool* has_no_move_exception) {
  // Set aside the exception while we resolve its type.
  Thread* self = Thread::Current();
  StackHandleScope<1> hs(self);
  Handle<mirror::Throwable> exception(hs.NewHandle(self->GetException()));
  self->ClearException();
  // Default to handler not found.
  uint32_t found_dex_pc = dex::kDexNoIndex;
  // Iterate over the catch handlers associated with dex_pc.
  CodeItemDataAccessor accessor(DexInstructionData());
  for (CatchHandlerIterator it(accessor, dex_pc); it.HasNext(); it.Next()) {
    dex::TypeIndex iter_type_idx = it.GetHandlerTypeIndex();
    // Catch all case
    if (!iter_type_idx.IsValid()) {
      found_dex_pc = it.GetHandlerAddress();
      break;
    }
    // Does this catch exception type apply?
    ObjPtr<mirror::Class> iter_exception_type = ResolveClassFromTypeIndex(iter_type_idx);
    if (UNLIKELY(iter_exception_type == nullptr)) {
      // Now have a NoClassDefFoundError as exception. Ignore in case the exception class was
      // removed by a pro-guard like tool.
      // Note: this is not RI behavior. RI would have failed when loading the class.
      self->ClearException();
      LOG(WARNING) << "Unresolved exception class when finding catch block: "
        << DescriptorToDot(GetTypeDescriptorFromTypeIdx(iter_type_idx));
    } else if (iter_exception_type->IsAssignableFrom(exception_type.Get())) {
      found_dex_pc = it.GetHandlerAddress();
      break;
    }
  }
  if (found_dex_pc != dex::kDexNoIndex) {
    const Instruction& first_catch_instr = accessor.InstructionAt(found_dex_pc);
    *has_no_move_exception = (first_catch_instr.Opcode() != Instruction::MOVE_EXCEPTION);
  }
  // Put the exception back.
  if (exception != nullptr) {
    self->SetException(exception.Get());
  }
  return found_dex_pc;
}

NO_STACK_PROTECTOR
void ArtMethod::Invoke(Thread* self, uint32_t* args, uint32_t args_size, JValue* result,
                       const char* shorty) {
  if (UNLIKELY(__builtin_frame_address(0) < self->GetStackEnd<kNativeStackType>())) {
    ThrowStackOverflowError<kNativeStackType>(self);
    return;
  }

  if (kIsDebugBuild) {
    self->AssertThreadSuspensionIsAllowable();
    CHECK_EQ(ThreadState::kRunnable, self->GetState());
    CHECK_STREQ(GetInterfaceMethodIfProxy(kRuntimePointerSize)->GetShorty(), shorty);
  }

  // Push a transition back into managed code onto the linked list in thread.
  ManagedStack fragment;
  self->PushManagedStackFragment(&fragment);

  Runtime* runtime = Runtime::Current();
  // Call the invoke stub, passing everything as arguments.
  // If the runtime is not yet started or it is required by the debugger, then perform the
  // Invocation by the interpreter, explicitly forcing interpretation over JIT to prevent
  // cycling around the various JIT/Interpreter methods that handle method invocation.
  if (UNLIKELY(!runtime->IsStarted() ||
               (self->IsForceInterpreter() && !IsNative() && !IsProxyMethod() && IsInvokable()))) {
    if (IsStatic()) {
      art::interpreter::EnterInterpreterFromInvoke(
          self, this, nullptr, args, result, /*stay_in_interpreter=*/ true);
    } else {
      mirror::Object* receiver =
          reinterpret_cast<StackReference<mirror::Object>*>(&args[0])->AsMirrorPtr();
      art::interpreter::EnterInterpreterFromInvoke(
          self, this, receiver, args + 1, result, /*stay_in_interpreter=*/ true);
    }
  } else {
    DCHECK_EQ(runtime->GetClassLinker()->GetImagePointerSize(), kRuntimePointerSize);

    constexpr bool kLogInvocationStartAndReturn = false;
    bool have_quick_code = GetEntryPointFromQuickCompiledCode() != nullptr;
    if (LIKELY(have_quick_code)) {
      if (kLogInvocationStartAndReturn) {
        LOG(INFO) << StringPrintf(
            "Invoking '%s' quick code=%p static=%d", PrettyMethod().c_str(),
            GetEntryPointFromQuickCompiledCode(), static_cast<int>(IsStatic() ? 1 : 0));
      }

      // Ensure that we won't be accidentally calling quick compiled code when -Xint.
      if (kIsDebugBuild && runtime->GetInstrumentation()->IsForcedInterpretOnly()) {
        CHECK(!runtime->UseJitCompilation());
        const void* oat_quick_code =
            (IsNative() || !IsInvokable() || IsProxyMethod() || IsObsolete())
            ? nullptr
            : GetOatMethodQuickCode(runtime->GetClassLinker()->GetImagePointerSize());
        CHECK(oat_quick_code == nullptr || oat_quick_code != GetEntryPointFromQuickCompiledCode())
            << "Don't call compiled code when -Xint " << PrettyMethod();
      }

      if (!IsStatic()) {
        (*art_quick_invoke_stub)(this, args, args_size, self, result, shorty);
      } else {
        (*art_quick_invoke_static_stub)(this, args, args_size, self, result, shorty);
      }
      if (UNLIKELY(self->GetException() == Thread::GetDeoptimizationException())) {
        // Unusual case where we were running generated code and an
        // exception was thrown to force the activations to be removed from the
        // stack. Continue execution in the interpreter.
        self->DeoptimizeWithDeoptimizationException(result);
      }
      if (kLogInvocationStartAndReturn) {
        LOG(INFO) << StringPrintf("Returned '%s' quick code=%p", PrettyMethod().c_str(),
                                  GetEntryPointFromQuickCompiledCode());
      }
    } else {
      LOG(INFO) << "Not invoking '" << PrettyMethod() << "' code=null";
      if (result != nullptr) {
        result->SetJ(0);
      }
    }
  }

  // Pop transition.
  self->PopManagedStackFragment(fragment);
}

bool ArtMethod::IsSignaturePolymorphic() {
  // Methods with a polymorphic signature have constraints that they
  // are native and varargs and belong to either MethodHandle or VarHandle.
  if (!IsNative() || !IsVarargs()) {
    return false;
  }
  ObjPtr<mirror::ObjectArray<mirror::Class>> class_roots =
      Runtime::Current()->GetClassLinker()->GetClassRoots();
  ObjPtr<mirror::Class> cls = GetDeclaringClass();
  return (cls == GetClassRoot<mirror::MethodHandle>(class_roots) ||
          cls == GetClassRoot<mirror::VarHandle>(class_roots));
}

static uint32_t GetOatMethodIndexFromMethodIndex(const DexFile& dex_file,
                                                 uint16_t class_def_idx,
                                                 uint32_t method_idx) {
  ClassAccessor accessor(dex_file, class_def_idx);
  uint32_t class_def_method_index = 0u;
  for (const ClassAccessor::Method& method : accessor.GetMethods()) {
    if (method.GetIndex() == method_idx) {
      return class_def_method_index;
    }
    class_def_method_index++;
  }
  LOG(FATAL) << "Failed to find method index " << method_idx << " in " << dex_file.GetLocation();
  UNREACHABLE();
}

// We use the method's DexFile and declaring class name to find the OatMethod for an obsolete
// method.  This is extremely slow but we need it if we want to be able to have obsolete native
// methods since we need this to find the size of its stack frames.
//
// NB We could (potentially) do this differently and rely on the way the transformation is applied
// in order to use the entrypoint to find this information. However, for debugging reasons (most
// notably making sure that new invokes of obsolete methods fail) we choose to instead get the data
// directly from the dex file.
static const OatFile::OatMethod FindOatMethodFromDexFileFor(ArtMethod* method, bool* found)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  DCHECK(method->IsObsolete() && method->IsNative());
  const DexFile* dex_file = method->GetDexFile();

  // recreate the class_def_index from the descriptor.
  const dex::TypeId* declaring_class_type_id =
      dex_file->FindTypeId(method->GetDeclaringClassDescriptorView());
  CHECK(declaring_class_type_id != nullptr);
  dex::TypeIndex declaring_class_type_index = dex_file->GetIndexForTypeId(*declaring_class_type_id);
  const dex::ClassDef* declaring_class_type_def =
      dex_file->FindClassDef(declaring_class_type_index);
  CHECK(declaring_class_type_def != nullptr);
  uint16_t declaring_class_def_index = dex_file->GetIndexForClassDef(*declaring_class_type_def);

  size_t oat_method_index = GetOatMethodIndexFromMethodIndex(*dex_file,
                                                             declaring_class_def_index,
                                                             method->GetDexMethodIndex());

  OatFile::OatClass oat_class = OatFile::FindOatClass(*dex_file,
                                                      declaring_class_def_index,
                                                      found);
  if (!(*found)) {
    return OatFile::OatMethod::Invalid();
  }
  return oat_class.GetOatMethod(oat_method_index);
}

static const OatFile::OatMethod FindOatMethodFor(ArtMethod* method,
                                                 PointerSize pointer_size,
                                                 bool* found)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (UNLIKELY(method->IsObsolete())) {
    // We shouldn't be calling this with obsolete methods except for native obsolete methods for
    // which we need to use the oat method to figure out how large the quick frame is.
    DCHECK(method->IsNative()) << "We should only be finding the OatMethod of obsolete methods in "
                               << "order to allow stack walking. Other obsolete methods should "
                               << "never need to access this information.";
    DCHECK_EQ(pointer_size, kRuntimePointerSize) << "Obsolete method in compiler!";
    return FindOatMethodFromDexFileFor(method, found);
  }
  // Although we overwrite the trampoline of non-static methods, we may get here via the resolution
  // method for direct methods (or virtual methods made direct).
  ObjPtr<mirror::Class> declaring_class = method->GetDeclaringClass();
  size_t oat_method_index;
  if (method->IsStatic() || method->IsDirect()) {
    // Simple case where the oat method index was stashed at load time.
    oat_method_index = method->GetMethodIndex();
  } else {
    // Compute the oat_method_index by search for its position in the declared virtual methods.
    oat_method_index = declaring_class->NumDirectMethods();
    bool found_virtual = false;
    for (ArtMethod& art_method : declaring_class->GetVirtualMethods(pointer_size)) {
      // Check method index instead of identity in case of duplicate method definitions.
      if (method->GetDexMethodIndex() == art_method.GetDexMethodIndex()) {
        found_virtual = true;
        break;
      }
      oat_method_index++;
    }
    CHECK(found_virtual) << "Didn't find oat method index for virtual method: "
                         << method->PrettyMethod();
  }
  DCHECK_EQ(oat_method_index,
            GetOatMethodIndexFromMethodIndex(declaring_class->GetDexFile(),
                                             method->GetDeclaringClass()->GetDexClassDefIndex(),
                                             method->GetDexMethodIndex()));
  OatFile::OatClass oat_class = OatFile::FindOatClass(declaring_class->GetDexFile(),
                                                      declaring_class->GetDexClassDefIndex(),
                                                      found);
  if (!(*found)) {
    return OatFile::OatMethod::Invalid();
  }
  return oat_class.GetOatMethod(oat_method_index);
}

bool ArtMethod::EqualParameters(Handle<mirror::ObjectArray<mirror::Class>> params) {
  const DexFile* dex_file = GetDexFile();
  const auto& method_id = dex_file->GetMethodId(GetDexMethodIndex());
  const auto& proto_id = dex_file->GetMethodPrototype(method_id);
  const dex::TypeList* proto_params = dex_file->GetProtoParameters(proto_id);
  auto count = proto_params != nullptr ? proto_params->Size() : 0u;
  auto param_len = params != nullptr ? params->GetLength() : 0u;
  if (param_len != count) {
    return false;
  }
  auto* cl = Runtime::Current()->GetClassLinker();
  for (size_t i = 0; i < count; ++i) {
    dex::TypeIndex type_idx = proto_params->GetTypeItem(i).type_idx_;
    ObjPtr<mirror::Class> type = cl->ResolveType(type_idx, this);
    if (type == nullptr) {
      Thread::Current()->AssertPendingException();
      return false;
    }
    if (type != params->GetWithoutChecks(i)) {
      return false;
    }
  }
  return true;
}

const OatQuickMethodHeader* ArtMethod::GetOatQuickMethodHeader(uintptr_t pc) {
  if (IsRuntimeMethod()) {
    return nullptr;
  }

  Runtime* runtime = Runtime::Current();
  const void* existing_entry_point = GetEntryPointFromQuickCompiledCode();
  CHECK(existing_entry_point != nullptr) << PrettyMethod() << "@" << this;
  ClassLinker* class_linker = runtime->GetClassLinker();

  if (existing_entry_point == GetQuickProxyInvokeHandler()) {
    DCHECK(IsProxyMethod() && !IsConstructor());
    // The proxy entry point does not have any method header.
    return nullptr;
  }

  // We should not reach here with a pc of 0. pc can be 0 for downcalls when walking the stack.
  // For native methods this case is handled by the caller by checking the quick frame tag. See
  // StackVisitor::WalkStack for more details. For non-native methods pc can be 0 only for runtime
  // methods or proxy invoke handlers which are handled earlier.
  DCHECK_NE(pc, 0u) << "PC 0 for " << PrettyMethod();

  // Check whether the current entry point contains this pc. We need to manually
  // check some entrypoints in case they are trampolines in the oat file.
  if (!class_linker->IsQuickGenericJniStub(existing_entry_point) &&
      !class_linker->IsQuickResolutionStub(existing_entry_point) &&
      !class_linker->IsQuickToInterpreterBridge(existing_entry_point) &&
      !OatQuickMethodHeader::IsStub(
          reinterpret_cast<const uint8_t*>(existing_entry_point)).value_or(true)) {
    OatQuickMethodHeader* method_header =
        OatQuickMethodHeader::FromEntryPoint(existing_entry_point);

    if (method_header->Contains(pc)) {
      return method_header;
    }
  }

  if (OatQuickMethodHeader::IsNterpPc(pc)) {
    return OatQuickMethodHeader::NterpMethodHeader;
  }

  // Check whether the pc is in the JIT code cache.
  jit::Jit* jit = runtime->GetJit();
  if (jit != nullptr) {
    jit::JitCodeCache* code_cache = jit->GetCodeCache();
    OatQuickMethodHeader* method_header = code_cache->LookupMethodHeader(pc, this);
    if (method_header != nullptr) {
      DCHECK(method_header->Contains(pc));
      return method_header;
    } else {
      if (kIsDebugBuild && code_cache->ContainsPc(reinterpret_cast<const void*>(pc))) {
        code_cache->DumpAllCompiledMethods(LOG_STREAM(FATAL_WITHOUT_ABORT));
        LOG(FATAL)
            << PrettyMethod()
            << ", pc=" << std::hex << pc
            << ", entry_point=" << std::hex << reinterpret_cast<uintptr_t>(existing_entry_point)
            << ", copy=" << std::boolalpha << IsCopied()
            << ", proxy=" << std::boolalpha << IsProxyMethod()
            << ", is_native=" << std::boolalpha << IsNative();
      }
    }
  }

  // The code has to be in an oat file.
  bool found;
  OatFile::OatMethod oat_method =
      FindOatMethodFor(this, class_linker->GetImagePointerSize(), &found);
  if (!found) {
    if (!IsNative()) {
      PrintFileToLog("/proc/self/maps", LogSeverity::FATAL_WITHOUT_ABORT);
      MemMap::DumpMaps(LOG_STREAM(FATAL_WITHOUT_ABORT), /* terse= */ true);
      LOG(FATAL)
          << PrettyMethod()
          << " pc=" << pc
          << ", entrypoint= " << std::hex << reinterpret_cast<uintptr_t>(existing_entry_point)
          << ", jit= " << jit;
    }
    // We are running the GenericJNI stub. The entrypoint may point
    // to different entrypoints, to a JIT-compiled JNI stub, or to a shared boot
    // image stub.
    DCHECK(class_linker->IsQuickGenericJniStub(existing_entry_point) ||
           class_linker->IsQuickResolutionStub(existing_entry_point) ||
           (jit != nullptr && jit->GetCodeCache()->ContainsPc(existing_entry_point)) ||
           (class_linker->FindBootJniStub(this) != nullptr))
        << " method: " << PrettyMethod()
        << " entrypoint: " << existing_entry_point
        << " size: " << OatQuickMethodHeader::FromEntryPoint(existing_entry_point)->GetCodeSize()
        << " pc: " << reinterpret_cast<const void*>(pc);
    return nullptr;
  }
  const void* oat_entry_point = oat_method.GetQuickCode();
  if (oat_entry_point == nullptr || class_linker->IsQuickGenericJniStub(oat_entry_point)) {
    if (kIsDebugBuild && !IsNative()) {
      PrintFileToLog("/proc/self/maps", LogSeverity::FATAL_WITHOUT_ABORT);
      MemMap::DumpMaps(LOG_STREAM(FATAL_WITHOUT_ABORT), /* terse= */ true);
      LOG(FATAL)
          << PrettyMethod()
          << std::hex
          << " pc=" << pc
          << ", entrypoint= " << reinterpret_cast<uintptr_t>(existing_entry_point)
          << ", jit= " << jit
          << ", nterp_start= "
          << reinterpret_cast<uintptr_t>(OatQuickMethodHeader::NterpImpl.data())
          << ", nterp_end= "
          << reinterpret_cast<uintptr_t>(
                 OatQuickMethodHeader::NterpImpl.data() + OatQuickMethodHeader::NterpImpl.size());
    }
    return nullptr;
  }

  OatQuickMethodHeader* method_header = OatQuickMethodHeader::FromEntryPoint(oat_entry_point);
  // We could have existing Oat code for native methods but we may not use it if the runtime is java
  // debuggable or when profiling boot class path. There is no easy way to check if the pc
  // corresponds to QuickGenericJniStub. Since we have eliminated all the other cases, if the pc
  // doesn't correspond to the AOT code then we must be running QuickGenericJniStub.
  if (IsNative() && !method_header->Contains(pc)) {
    DCHECK_NE(pc, 0u) << "PC 0 for " << PrettyMethod();
    return nullptr;
  }

  DCHECK(method_header->Contains(pc))
      << PrettyMethod()
      << " " << std::hex << pc << " " << oat_entry_point
      << " " << (uintptr_t)(method_header->GetCode() + method_header->GetCodeSize());
  return method_header;
}

const void* ArtMethod::GetOatMethodQuickCode(PointerSize pointer_size) {
  bool found;
  OatFile::OatMethod oat_method = FindOatMethodFor(this, pointer_size, &found);
  if (found) {
    return oat_method.GetQuickCode();
  }
  return nullptr;
}

void ArtMethod::SetIntrinsic(Intrinsics intrinsic) {
  // Currently we only do intrinsics for static/final methods or methods of final
  // classes. We don't set kHasSingleImplementation for those methods.
  DCHECK(IsStatic() || IsFinal() || GetDeclaringClass()->IsFinal()) <<
      "Potential conflict with kAccSingleImplementation";
  static constexpr int kAccFlagsShift = CTZ(kAccIntrinsicBits);
  uint32_t intrinsic_u32 = enum_cast<uint32_t>(intrinsic);
  DCHECK_LE(intrinsic_u32, kAccIntrinsicBits >> kAccFlagsShift);
  uint32_t intrinsic_bits = intrinsic_u32 << kAccFlagsShift;
  uint32_t new_value = (GetAccessFlags() & ~kAccIntrinsicBits) | kAccIntrinsic | intrinsic_bits;

  // These flags shouldn't be overridden by setting the intrinsic.
  uint32_t java_flags = (GetAccessFlags() & kAccJavaFlagsMask);
  bool is_constructor = IsConstructor();
  bool is_synchronized = IsSynchronized();
  bool skip_access_checks = SkipAccessChecks();
  bool is_fast_native = IsFastNative();
  bool is_critical_native = IsCriticalNative();
  bool is_copied = IsCopied();
  bool is_miranda = IsMiranda();
  bool is_default = IsDefault();
  bool is_default_conflict = IsDefaultConflicting();
  bool is_compilable = IsCompilable();
  bool must_count_locks = MustCountLocks();

#ifdef ART_TARGET_ANDROID
  // Recompute flags instead of getting them from the current access flags because
  // access flags may have been changed to deduplicate warning messages (b/129063331).
  // For host builds, the flags from the api list (i.e. hiddenapi::CreateRuntimeFlags) might not
  // have the right value.
  uint32_t hiddenapi_flags = hiddenapi::CreateRuntimeFlags(this);
#endif

  SetAccessFlags(new_value);
  // Intrinsics are considered hot from the first call.
  SetHotCounter();

  // DCHECK that the flags weren't overridden.
  DCHECK_EQ(java_flags, (GetAccessFlags() & kAccJavaFlagsMask));
  DCHECK_EQ(is_constructor, IsConstructor());
  DCHECK_EQ(is_synchronized, IsSynchronized());
  DCHECK_EQ(skip_access_checks, SkipAccessChecks());
  DCHECK_EQ(is_fast_native, IsFastNative());
  DCHECK_EQ(is_critical_native, IsCriticalNative());
  DCHECK_EQ(is_copied, IsCopied());
  DCHECK_EQ(is_miranda, IsMiranda());
  DCHECK_EQ(is_default, IsDefault());
  DCHECK_EQ(is_default_conflict, IsDefaultConflicting());
  DCHECK_EQ(is_compilable, IsCompilable());
  DCHECK_EQ(must_count_locks, MustCountLocks());

#ifdef ART_TARGET_ANDROID
  DCHECK_EQ(hiddenapi_flags, hiddenapi::GetRuntimeFlags(this)) << PrettyMethod();
#endif
}

void ArtMethod::SetNotIntrinsic() {
  if (!IsIntrinsic()) {
    return;
  }

  // Read the existing hiddenapi flags.
  uint32_t hiddenapi_runtime_flags = hiddenapi::GetRuntimeFlags(this);

  // Clear intrinsic-related access flags.
  ClearAccessFlags(kAccIntrinsic | kAccIntrinsicBits);

  // Re-apply hidden API access flags now that the method is not an intrinsic.
  SetAccessFlags(GetAccessFlags() | hiddenapi_runtime_flags);
  DCHECK_EQ(hiddenapi_runtime_flags, hiddenapi::GetRuntimeFlags(this));
}

void ArtMethod::CopyFrom(ArtMethod* src, PointerSize image_pointer_size) {
  memcpy(reinterpret_cast<void*>(this), reinterpret_cast<const void*>(src),
         Size(image_pointer_size));
  declaring_class_ = GcRoot<mirror::Class>(const_cast<ArtMethod*>(src)->GetDeclaringClass());

  // If the entry point of the method we are copying from is from JIT code, we just
  // put the entry point of the new method to interpreter or GenericJNI. We could set
  // the entry point to the JIT code, but this would require taking the JIT code cache
  // lock to notify it, which we do not want at this level.
  Runtime* runtime = Runtime::Current();
  const void* entry_point = GetEntryPointFromQuickCompiledCodePtrSize(image_pointer_size);
  if (runtime->UseJitCompilation()) {
    if (runtime->GetJit()->GetCodeCache()->ContainsPc(entry_point)) {
      SetNativePointer(EntryPointFromQuickCompiledCodeOffset(image_pointer_size),
                       src->IsNative() ? GetQuickGenericJniStub() : GetQuickToInterpreterBridge(),
                       image_pointer_size);
    }
  }
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  if (interpreter::IsNterpSupported() && class_linker->IsNterpEntryPoint(entry_point)) {
    // If the entrypoint is nterp, it's too early to check if the new method
    // will support it. So for simplicity, use the interpreter bridge.
    SetNativePointer(EntryPointFromQuickCompiledCodeOffset(image_pointer_size),
                     GetQuickToInterpreterBridge(),
                     image_pointer_size);
  }

  // Clear the data pointer, it will be set if needed by the caller.
  if (!src->HasCodeItem() && !src->IsNative()) {
    SetDataPtrSize(nullptr, image_pointer_size);
  }
  // Clear hotness to let the JIT properly decide when to compile this method.
  ResetCounter(runtime->GetJITOptions()->GetWarmupThreshold());
}

bool ArtMethod::IsImagePointerSize(PointerSize pointer_size) {
  // Hijack this function to get access to PtrSizedFieldsOffset.
  //
  // Ensure that PrtSizedFieldsOffset is correct. We rely here on usually having both 32-bit and
  // 64-bit builds.
  static_assert(std::is_standard_layout<ArtMethod>::value, "ArtMethod is not standard layout.");
  static_assert(
      (sizeof(void*) != 4) ||
          (offsetof(ArtMethod, ptr_sized_fields_) == PtrSizedFieldsOffset(PointerSize::k32)),
      "Unexpected 32-bit class layout.");
  static_assert(
      (sizeof(void*) != 8) ||
          (offsetof(ArtMethod, ptr_sized_fields_) == PtrSizedFieldsOffset(PointerSize::k64)),
      "Unexpected 64-bit class layout.");

  Runtime* runtime = Runtime::Current();
  if (runtime == nullptr) {
    return true;
  }
  return runtime->GetClassLinker()->GetImagePointerSize() == pointer_size;
}

std::string ArtMethod::PrettyMethod(ArtMethod* m, bool with_signature) {
  if (m == nullptr) {
    return "null";
  }
  return m->PrettyMethod(with_signature);
}

std::string ArtMethod::PrettyMethod(bool with_signature) {
  if (UNLIKELY(IsRuntimeMethod())) {
    std::string result = "<runtime method>.";
    result += GetName();
    // Do not add "<no signature>" even if `with_signature` is true.
    return result;
  }
  ArtMethod* m =
      GetInterfaceMethodIfProxy(Runtime::Current()->GetClassLinker()->GetImagePointerSize());
  std::string res(m->GetDexFile()->PrettyMethod(m->GetDexMethodIndex(), with_signature));
  if (with_signature && m->IsObsolete()) {
    return "<OBSOLETE> " + res;
  } else {
    return res;
  }
}

std::string ArtMethod::JniShortName() {
  return GetJniShortName(GetDeclaringClassDescriptor(), GetName());
}

std::string ArtMethod::JniLongName() {
  std::string long_name;
  long_name += JniShortName();
  long_name += "__";

  std::string signature(GetSignature().ToString());
  signature.erase(0, 1);
  signature.erase(signature.begin() + signature.find(')'), signature.end());

  long_name += MangleForJni(signature);

  return long_name;
}

const char* ArtMethod::GetRuntimeMethodName() {
  Runtime* const runtime = Runtime::Current();
  if (this == runtime->GetResolutionMethod()) {
    return "<runtime internal resolution method>";
  } else if (this == runtime->GetImtConflictMethod()) {
    return "<runtime internal imt conflict method>";
  } else if (this == runtime->GetCalleeSaveMethod(CalleeSaveType::kSaveAllCalleeSaves)) {
    return "<runtime internal callee-save all registers method>";
  } else if (this == runtime->GetCalleeSaveMethod(CalleeSaveType::kSaveRefsOnly)) {
    return "<runtime internal callee-save reference registers method>";
  } else if (this == runtime->GetCalleeSaveMethod(CalleeSaveType::kSaveRefsAndArgs)) {
    return "<runtime internal callee-save reference and argument registers method>";
  } else if (this == runtime->GetCalleeSaveMethod(CalleeSaveType::kSaveEverything)) {
    return "<runtime internal save-every-register method>";
  } else if (this == runtime->GetCalleeSaveMethod(CalleeSaveType::kSaveEverythingForClinit)) {
    return "<runtime internal save-every-register method for clinit>";
  } else if (this == runtime->GetCalleeSaveMethod(CalleeSaveType::kSaveEverythingForSuspendCheck)) {
    return "<runtime internal save-every-register method for suspend check>";
  } else {
    return "<unknown runtime internal method>";
  }
}

// AssertSharedHeld doesn't work in GetAccessFlags, so use a NO_THREAD_SAFETY_ANALYSIS helper.
// TODO: Figure out why ASSERT_SHARED_CAPABILITY doesn't work.
template <ReadBarrierOption kReadBarrierOption>
ALWAYS_INLINE static inline void DoGetAccessFlagsHelper(ArtMethod* method)
    NO_THREAD_SAFETY_ANALYSIS {
  CHECK(method->IsRuntimeMethod() ||
        method->GetDeclaringClass<kReadBarrierOption>()->IsIdxLoaded() ||
        method->GetDeclaringClass<kReadBarrierOption>()->IsErroneous());
}

}  // namespace art
