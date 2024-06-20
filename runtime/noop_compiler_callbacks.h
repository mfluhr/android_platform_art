/*
 * Copyright (C) 2013 The Android Open Source Project
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

#ifndef ART_RUNTIME_NOOP_COMPILER_CALLBACKS_H_
#define ART_RUNTIME_NOOP_COMPILER_CALLBACKS_H_

#include "base/macros.h"
#include "class_linker.h"
#include "compiler_callbacks.h"

namespace art HIDDEN {

// Used for tests and some tools that pretend to be a compiler (say, oatdump).
class NoopCompilerCallbacks final : public CompilerCallbacks {
 public:
  NoopCompilerCallbacks() : CompilerCallbacks(CompilerCallbacks::CallbackMode::kCompileApp) {}
  ~NoopCompilerCallbacks() {}

  ClassLinker* CreateAotClassLinker(InternTable* intern_table) override {
    return new PermissiveClassLinker(intern_table);
  }

  void AddUncompilableMethod([[maybe_unused]] MethodReference ref) override {}
  void AddUncompilableClass([[maybe_unused]] ClassReference ref) override {}
  void ClassRejected([[maybe_unused]] ClassReference ref) override {}

  verifier::VerifierDeps* GetVerifierDeps() const override { return nullptr; }

 private:
  // When we supply compiler callbacks, we need an appropriate `ClassLinker` that can
  // handle `SdkChecker`-related calls that are unimplemented in the base `ClassLinker`.
  class PermissiveClassLinker : public ClassLinker {
   public:
    explicit PermissiveClassLinker(InternTable* intern_table)
        : ClassLinker(intern_table, /*fast_class_not_found_exceptions=*/ false) {}

    bool DenyAccessBasedOnPublicSdk([[maybe_unused]] ArtMethod* art_method) const override
        REQUIRES_SHARED(Locks::mutator_lock_) {
      return false;
    }
    bool DenyAccessBasedOnPublicSdk([[maybe_unused]] ArtField* art_field) const override
        REQUIRES_SHARED(Locks::mutator_lock_) {
      return false;
    }
    bool DenyAccessBasedOnPublicSdk(
        [[maybe_unused]] std::string_view type_descriptor) const override {
      return false;
    }
    void SetEnablePublicSdkChecks([[maybe_unused]] bool enabled) override {}
  };

  DISALLOW_COPY_AND_ASSIGN(NoopCompilerCallbacks);
};

}  // namespace art

#endif  // ART_RUNTIME_NOOP_COMPILER_CALLBACKS_H_
