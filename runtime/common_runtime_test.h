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

#ifndef ART_RUNTIME_COMMON_RUNTIME_TEST_H_
#define ART_RUNTIME_COMMON_RUNTIME_TEST_H_

#include <gtest/gtest.h>
#include <jni.h>

#include <functional>
#include <string>

#include <android-base/logging.h>

#include "arch/instruction_set.h"
#include "base/common_art_test.h"
#include "base/locks.h"
#include "base/macros.h"
#include "base/os.h"
#include "base/unix_file/fd_file.h"
#include "dex/art_dex_file_loader.h"
// TODO: Add inl file and avoid including inl.
#include "obj_ptr-inl.h"
#include "runtime_globals.h"
#include "scoped_thread_state_change-inl.h"

namespace art HIDDEN {

class MethodReference;
class TypeReference;

using LogSeverity = android::base::LogSeverity;
using ScopedLogSeverity = android::base::ScopedLogSeverity;

template<class MirrorType>
static inline ObjPtr<MirrorType> MakeObjPtr(MirrorType* ptr) REQUIRES_SHARED(Locks::mutator_lock_) {
  return ptr;
}

template<class MirrorType>
static inline ObjPtr<MirrorType> MakeObjPtr(ObjPtr<MirrorType> ptr)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  return ptr;
}

// OBJ pointer helpers to avoid needing .Decode everywhere.
#define EXPECT_OBJ_PTR_EQ(a, b) EXPECT_EQ(MakeObjPtr(a).Ptr(), MakeObjPtr(b).Ptr())
#define ASSERT_OBJ_PTR_EQ(a, b) ASSERT_EQ(MakeObjPtr(a).Ptr(), MakeObjPtr(b).Ptr())
#define EXPECT_OBJ_PTR_NE(a, b) EXPECT_NE(MakeObjPtr(a).Ptr(), MakeObjPtr(b).Ptr())
#define ASSERT_OBJ_PTR_NE(a, b) ASSERT_NE(MakeObjPtr(a).Ptr(), MakeObjPtr(b).Ptr())

class ClassLinker;
class CompilerCallbacks;
class DexFile;
class JavaVMExt;
class Runtime;
using RuntimeOptions = std::vector<std::pair<std::string, const void*>>;
class Thread;
class VariableSizedHandleScope;

class CommonRuntimeTestImpl : public CommonArtTestImpl {
 public:
  CommonRuntimeTestImpl();
  virtual ~CommonRuntimeTestImpl();

  // A helper function to fill the heap.
  static void FillHeap(Thread* self,
                       ClassLinker* class_linker,
                       VariableSizedHandleScope* handle_scope)
      REQUIRES_SHARED(Locks::mutator_lock_);
  // A helper to set up a small heap (4M) to make FillHeap faster.
  static void SetUpRuntimeOptionsForFillHeap(RuntimeOptions *options);

  template <typename Mutator>
  bool MutateDexFile(File* output_dex, const std::string& input_jar, const Mutator& mutator) {
    std::vector<std::unique_ptr<const DexFile>> dex_files;
    std::string error_msg;
    ArtDexFileLoader dex_file_loader(input_jar);
    CHECK(dex_file_loader.Open(/*verify=*/true,
                               /*verify_checksum=*/true,
                               &error_msg,
                               &dex_files))
        << error_msg;
    EXPECT_EQ(dex_files.size(), 1u) << "Only one input dex is supported";
    const std::unique_ptr<const DexFile>& dex = dex_files[0];
    CHECK(dex->EnableWrite()) << "Failed to enable write";
    DexFile* dex_file = const_cast<DexFile*>(dex.get());
    mutator(dex_file);
    const_cast<DexFile::Header&>(dex_file->GetHeader()).checksum_ = dex_file->CalculateChecksum();
    if (!output_dex->WriteFully(dex->Begin(), dex->Size())) {
      return false;
    }
    if (output_dex->Flush() != 0) {
      PLOG(FATAL) << "Could not flush the output file.";
    }
    return true;
  }

  void MakeInterpreted(ObjPtr<mirror::Class> klass)
      REQUIRES_SHARED(Locks::mutator_lock_);

  bool StartDex2OatCommandLine(/*out*/std::vector<std::string>* argv,
                               /*out*/std::string* error_msg,
                               bool use_runtime_bcp_and_image = true);

  bool CompileBootImage(const std::vector<std::string>& extra_args,
                        const std::string& image_file_name_prefix,
                        ArrayRef<const std::string> dex_files,
                        ArrayRef<const std::string> dex_locations,
                        std::string* error_msg,
                        const std::string& use_fd_prefix = "");

  bool CompileBootImage(const std::vector<std::string>& extra_args,
                        const std::string& image_file_name_prefix,
                        ArrayRef<const std::string> dex_files,
                        std::string* error_msg,
                        const std::string& use_fd_prefix = "") {
    return CompileBootImage(
        extra_args, image_file_name_prefix, dex_files, dex_files, error_msg, use_fd_prefix);
  }

  bool RunDex2Oat(const std::vector<std::string>& args, std::string* error_msg);

 protected:
  // Allow subclases such as CommonCompilerTest to add extra options.
  virtual void SetUpRuntimeOptions([[maybe_unused]] RuntimeOptions* options) {}

  // Called before the runtime is created.
  virtual void PreRuntimeCreate() {}

  // Called after the runtime is created.
  virtual void PostRuntimeCreate() {}

  // Loads the test dex file identified by the given dex_name into a PathClassLoader.
  // Returns the created class loader.
  jobject LoadDex(const char* dex_name) REQUIRES_SHARED(Locks::mutator_lock_);
  // Loads the test dex file identified by the given first_dex_name and second_dex_name
  // into a PathClassLoader. Returns the created class loader.
  jobject LoadMultiDex(const char* first_dex_name, const char* second_dex_name)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // The following helper functions return global JNI references to the class loader.
  jobject LoadDexInPathClassLoader(const std::string& dex_name,
                                   jobject parent_loader,
                                   jobject shared_libraries = nullptr,
                                   jobject shared_libraries_after = nullptr);
  jobject LoadDexInPathClassLoader(const std::vector<std::string>& dex_names,
                                   jobject parent_loader,
                                   jobject shared_libraries = nullptr,
                                   jobject shared_libraries_after = nullptr);
  jobject LoadDexInDelegateLastClassLoader(const std::string& dex_name, jobject parent_loader);
  jobject LoadDexInInMemoryDexClassLoader(const std::string& dex_name, jobject parent_loader);
  jobject LoadDexInWellKnownClassLoader(ScopedObjectAccess& soa,
                                        const std::vector<std::string>& dex_names,
                                        ObjPtr<mirror::Class> loader_class,
                                        jobject parent_loader,
                                        jobject shared_libraries = nullptr,
                                        jobject shared_libraries_after = nullptr)
      REQUIRES_SHARED(Locks::mutator_lock_);

  void VisitDexes(ArrayRef<const std::string> dexes,
                  const std::function<void(MethodReference)>& method_visitor,
                  const std::function<void(TypeReference)>& class_visitor,
                  size_t method_frequency = 1u,
                  size_t class_frequency = 1u);

  void GenerateProfile(ArrayRef<const std::string> dexes,
                       File* out_file,
                       size_t method_frequency = 1u,
                       size_t type_frequency = 1u,
                       bool for_boot_image = false);
  void GenerateBootProfile(ArrayRef<const std::string> dexes,
                           File* out_file,
                           size_t method_frequency = 1u,
                           size_t type_frequency = 1u) {
    return GenerateProfile(
        dexes, out_file, method_frequency, type_frequency, /*for_boot_image=*/ true);
  }

  ObjPtr<mirror::Class> FindClass(const char* descriptor,
                                  Handle<mirror::ClassLoader> class_loader) const
      REQUIRES_SHARED(Locks::mutator_lock_);

  std::unique_ptr<Runtime> runtime_;

  // The class_linker_, java_lang_dex_file_, and boot_class_path_ are all
  // owned by the runtime.
  ClassLinker* class_linker_;
  const DexFile* java_lang_dex_file_;
  std::vector<const DexFile*> boot_class_path_;

  // Get the dex files from a PathClassLoader or DelegateLastClassLoader.
  // This only looks into the current class loader and does not recurse into the parents.
  std::vector<const DexFile*> GetDexFiles(jobject jclass_loader);
  std::vector<const DexFile*> GetDexFiles(Thread* self, Handle<mirror::ClassLoader> class_loader)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Get the first dex file from a PathClassLoader. Will abort if it is null.
  const DexFile* GetFirstDexFile(jobject jclass_loader);

  std::unique_ptr<CompilerCallbacks> callbacks_;

  bool use_boot_image_;

  virtual void SetUp();

  virtual void TearDown();

  // Called to finish up runtime creation and filling test fields. By default runs root
  // initializers, initialize well-known classes, and creates the heap thread pool.
  virtual void FinalizeSetup();

  // Returns the directory where the pre-compiled boot.art can be found.
  static std::string GetImageLocation();
  static std::string GetSystemImageFile();
};

template <typename TestType>
class CommonRuntimeTestBase : public TestType, public CommonRuntimeTestImpl {
 public:
  CommonRuntimeTestBase() {}
  virtual ~CommonRuntimeTestBase() {}

 protected:
  void SetUp() override {
    CommonRuntimeTestImpl::SetUp();
  }

  void TearDown() override {
    CommonRuntimeTestImpl::TearDown();
  }
};

using CommonRuntimeTest = CommonRuntimeTestBase<::testing::Test>;

template <typename Param>
using CommonRuntimeTestWithParam = CommonRuntimeTestBase<::testing::TestWithParam<Param>>;

// Sets a CheckJni abort hook to catch failures. Note that this will cause CheckJNI to carry on
// rather than aborting, so be careful!
class CheckJniAbortCatcher {
 public:
  CheckJniAbortCatcher();

  ~CheckJniAbortCatcher();

  void Check(const std::string& expected_text);
  void Check(const char* expected_text);

 private:
  static void Hook(void* data, const std::string& reason);

  JavaVMExt* const vm_;
  std::string actual_;

  DISALLOW_COPY_AND_ASSIGN(CheckJniAbortCatcher);
};

#define TEST_DISABLED() GTEST_SKIP() << "WARNING: TEST DISABLED";

#define TEST_DISABLED_FOR_ARM()                                                        \
  if (kRuntimeISA == InstructionSet::kArm || kRuntimeISA == InstructionSet::kThumb2) { \
    GTEST_SKIP() << "WARNING: TEST DISABLED FOR ARM";                                  \
  }

#define TEST_DISABLED_FOR_ARM64()                       \
  if (kRuntimeISA == InstructionSet::kArm64) {          \
    GTEST_SKIP() << "WARNING: TEST DISABLED FOR ARM64"; \
  }

#define TEST_DISABLED_FOR_RISCV64()                       \
  if (kRuntimeISA == InstructionSet::kRiscv64) {          \
    GTEST_SKIP() << "WARNING: TEST DISABLED FOR RISCV64"; \
  }

#define TEST_DISABLED_FOR_X86()                       \
  if (kRuntimeISA == InstructionSet::kX86) {          \
    GTEST_SKIP() << "WARNING: TEST DISABLED FOR X86"; \
  }

#define TEST_DISABLED_FOR_X86_64()                       \
  if (kRuntimeISA == InstructionSet::kX86_64) {          \
    GTEST_SKIP() << "WARNING: TEST DISABLED FOR X86_64"; \
  }

#define TEST_DISABLED_WITHOUT_BAKER_READ_BARRIERS()                             \
  if (!gUseReadBarrier || !kUseBakerReadBarrier) {                              \
    GTEST_SKIP() << "WARNING: TEST DISABLED FOR GC WITHOUT BAKER READ BARRIER"; \
  }

#define TEST_DISABLED_FOR_MEMORY_TOOL_WITH_HEAP_POISONING_WITHOUT_READ_BARRIERS()              \
  if (kRunningOnMemoryTool && kPoisonHeapReferences && !gUseReadBarrier) {                     \
    GTEST_SKIP()                                                                               \
        << "WARNING: TEST DISABLED FOR MEMORY TOOL WITH HEAP POISONING WITHOUT READ BARRIERS"; \
  }

#define TEST_DISABLED_FOR_KERNELS_WITH_CACHE_SEGFAULT()                                   \
  if (CacheOperationsMaySegFault()) {                                                     \
    GTEST_SKIP() << "WARNING: TEST DISABLED ON KERNEL THAT SEGFAULT ON CACHE OPERATIONS"; \
  }

#define TEST_DISABLED_ON_VM() \
  if (RunningOnVM()) {        \
    GTEST_SKIP();             \
  }

}  // namespace art

#endif  // ART_RUNTIME_COMMON_RUNTIME_TEST_H_
