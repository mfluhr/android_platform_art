/*
 * Copyright (C) 2017 The Android Open Source Project
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

#ifndef ART_RUNTIME_OAT_AOT_CLASS_LINKER_H_
#define ART_RUNTIME_OAT_AOT_CLASS_LINKER_H_

#include "base/macros.h"
#include "sdk_checker.h"
#include "class_linker.h"

namespace art HIDDEN {

namespace gc {
class Heap;
}  // namespace gc

// TODO: move to dex2oat/.
// AotClassLinker is only used for AOT compiler, which includes some logic for class initialization
// which will only be used in pre-compilation.
class AotClassLinker : public ClassLinker {
 public:
  explicit AotClassLinker(InternTable *intern_table);
  ~AotClassLinker();

  EXPORT static void SetAppImageDexFiles(const std::vector<const DexFile*>* app_image_dex_files);

  EXPORT static bool CanReferenceInBootImageExtensionOrAppImage(
      ObjPtr<mirror::Class> klass, gc::Heap* heap) REQUIRES_SHARED(Locks::mutator_lock_);

  EXPORT void SetSdkChecker(std::unique_ptr<SdkChecker>&& sdk_checker_);
  const SdkChecker* GetSdkChecker() const;

  bool DenyAccessBasedOnPublicSdk([[maybe_unused]] ArtMethod* art_method) const override
      REQUIRES_SHARED(Locks::mutator_lock_);
  bool DenyAccessBasedOnPublicSdk([[maybe_unused]] ArtField* art_field) const override
      REQUIRES_SHARED(Locks::mutator_lock_);
  bool DenyAccessBasedOnPublicSdk([[maybe_unused]] const char* type_descriptor) const override;
  void SetEnablePublicSdkChecks(bool enabled) override;

  // Transaction constraint checks for AOT compilation.
  bool TransactionWriteConstraint(Thread* self, ObjPtr<mirror::Object> obj) const override
      REQUIRES_SHARED(Locks::mutator_lock_);
  bool TransactionWriteValueConstraint(Thread* self, ObjPtr<mirror::Object> value) const override
      REQUIRES_SHARED(Locks::mutator_lock_);
  bool TransactionAllocationConstraint(Thread* self, ObjPtr<mirror::Class> klass) const override
      REQUIRES_SHARED(Locks::mutator_lock_);

 protected:
  // Overridden version of PerformClassVerification allows skipping verification if the class was
  // previously verified but unloaded.
  verifier::FailureKind PerformClassVerification(Thread* self,
                                                 verifier::VerifierDeps* verifier_deps,
                                                 Handle<mirror::Class> klass,
                                                 verifier::HardFailLogMode log_level,
                                                 std::string* error_msg)
      override
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Override AllocClass because aot compiler will need to perform a transaction check to determine
  // can we allocate class from heap.
  bool CanAllocClass()
      override
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Roles::uninterruptible_);

  bool InitializeClass(Thread *self,
                       Handle<mirror::Class> klass,
                       bool can_run_clinit,
                       bool can_init_parents)
      override
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_);

 private:
  std::unique_ptr<SdkChecker> sdk_checker_;
};

}  // namespace art

#endif  // ART_RUNTIME_OAT_AOT_CLASS_LINKER_H_
