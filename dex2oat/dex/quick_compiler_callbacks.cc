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

#include "quick_compiler_callbacks.h"

#include "aot_class_linker.h"
#include "dex/verification_results.h"
#include "driver/compiler_driver.h"
#include "mirror/class-inl.h"

namespace art {

ClassLinker* QuickCompilerCallbacks::CreateAotClassLinker(InternTable* intern_table) {
  return new AotClassLinker(intern_table);
}

void QuickCompilerCallbacks::AddUncompilableMethod(MethodReference ref) {
  if (verification_results_ != nullptr) {
    verification_results_->AddUncompilableMethod(ref);
  }
}

void QuickCompilerCallbacks::AddUncompilableClass(ClassReference ref) {
  if (verification_results_ != nullptr) {
    verification_results_->AddUncompilableClass(ref);
  }
}

void QuickCompilerCallbacks::ClassRejected(ClassReference ref) {
  if (verification_results_ != nullptr) {
    verification_results_->AddRejectedClass(ref);
  }
}

bool QuickCompilerCallbacks::IsUncompilableMethod(MethodReference ref) {
  if (verification_results_ != nullptr) {
    return verification_results_->IsUncompilableMethod(ref);
  }
  return false;
}

ClassStatus QuickCompilerCallbacks::GetPreviousClassState(ClassReference ref) {
  // If we don't have class unloading enabled in the compiler, we will never see class that were
  // previously verified. Return false to avoid overhead from the lookup in the compiler driver.
  if (!does_class_unloading_) {
    return ClassStatus::kNotReady;
  }
  DCHECK(compiler_driver_ != nullptr);
  // In the case of the verify filter, avoiding verifiying twice.
  return compiler_driver_->GetClassStatus(ref);
}

void QuickCompilerCallbacks::UpdateClassState(ClassReference ref, ClassStatus status) {
  // Driver is null when bootstrapping the runtime.
  if (compiler_driver_ != nullptr) {
    compiler_driver_->RecordClassStatus(ref, status);
  }
}

bool QuickCompilerCallbacks::CanUseOatStatusForVerification(mirror::Class* klass) {
  // No dex files: conservatively false.
  if (dex_files_ == nullptr) {
    return false;
  }

  // If the class isn't from one of the dex files, accept oat file data.
  const DexFile* dex_file = &klass->GetDexFile();
  return std::find(dex_files_->begin(), dex_files_->end(), dex_file) == dex_files_->end();
}

}  // namespace art
