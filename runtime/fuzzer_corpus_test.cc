/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include <cstdint>
#include <filesystem>
#include <unordered_set>

#include "android-base/file.h"
#include "common_runtime_test.h"
#include "dex/class_accessor-inl.h"
#include "dex/dex_file_verifier.h"
#include "dex/standard_dex_file.h"
#include "gtest/gtest.h"
#include "handle_scope-inl.h"
#include "jni/java_vm_ext.h"
#include "verifier/class_verifier.h"
#include "ziparchive/zip_archive.h"

namespace art {
// Manages the ZipArchiveHandle liveness.
class ZipArchiveHandleScope {
 public:
  explicit ZipArchiveHandleScope(ZipArchiveHandle* handle) : handle_(handle) {}
  ~ZipArchiveHandleScope() { CloseArchive(*(handle_.release())); }

 private:
  std::unique_ptr<ZipArchiveHandle> handle_;
};

class FuzzerCorpusTest : public CommonRuntimeTest {
 public:
  static void DexFileVerification(const uint8_t* data,
                                  size_t size,
                                  const std::string& name,
                                  bool expected_success) {
    // Do not verify the checksum as we only care about the DEX file contents,
    // and know that the checksum would probably be erroneous (i.e. random).
    constexpr bool kVerify = false;

    auto container = std::make_shared<art::MemoryDexFileContainer>(data, size);
    art::StandardDexFile dex_file(data,
                                  /*location=*/name,
                                  /*location_checksum=*/0,
                                  /*oat_dex_file=*/nullptr,
                                  container);

    std::string error_msg;
    bool is_valid_dex_file =
        art::dex::Verify(&dex_file, dex_file.GetLocation().c_str(), kVerify, &error_msg);
    ASSERT_EQ(is_valid_dex_file, expected_success) << " Failed for " << name;
  }

  static void ClassVerification(const uint8_t* data,
                                size_t size,
                                const std::string& name,
                                bool expected_success) {
    // Do not verify the checksum as we only care about the DEX file contents,
    // and know that the checksum would probably be erroneous (i.e. random)
    constexpr bool kVerify = false;
    bool passed_class_verification = true;

    auto container = std::make_shared<art::MemoryDexFileContainer>(data, size);
    art::StandardDexFile dex_file(data,
                                  /*location=*/name,
                                  /*location_checksum=*/0,
                                  /*oat_dex_file=*/nullptr,
                                  container);

    std::string error_msg;
    const bool success_dex =
        art::dex::Verify(&dex_file, dex_file.GetLocation().c_str(), kVerify, &error_msg);
    ASSERT_EQ(success_dex, true) << " Failed for " << name;

    art::Runtime* runtime = art::Runtime::Current();
    CHECK(runtime != nullptr);

    art::ScopedObjectAccess soa(art::Thread::Current());
    art::ClassLinker* class_linker = runtime->GetClassLinker();
    jobject class_loader = RegisterDexFileAndGetClassLoader(runtime, &dex_file);

    // Scope for the handles
    {
      art::StackHandleScope<3> scope(soa.Self());
      art::Handle<art::mirror::ClassLoader> h_loader =
          scope.NewHandle(soa.Decode<art::mirror::ClassLoader>(class_loader));

      for (ClassAccessor accessor : dex_file.GetClasses()) {
        const char* descriptor = accessor.GetDescriptor();
        const art::Handle<art::mirror::Class> h_klass(scope.NewHandle<art::mirror::Class>(
            class_linker->FindClass(soa.Self(), descriptor, h_loader)));
        const art::Handle<art::mirror::DexCache> h_dex_cache(
            scope.NewHandle<art::mirror::DexCache>(h_klass->GetDexCache()));

        // Ignore classes that couldn't be loaded since we are looking for crashes during
        // class/method verification.
        if (h_klass == nullptr || h_klass->IsErroneous()) {
          soa.Self()->ClearException();
          continue;
        }

        verifier::FailureKind failure =
            art::verifier::ClassVerifier::VerifyClass(soa.Self(),
                                                      /* verifier_deps= */ nullptr,
                                                      h_dex_cache->GetDexFile(),
                                                      h_klass,
                                                      h_dex_cache,
                                                      h_loader,
                                                      *h_klass->GetClassDef(),
                                                      runtime->GetCompilerCallbacks(),
                                                      art::verifier::HardFailLogMode::kLogWarning,
                                                      /* api_level= */ 0,
                                                      &error_msg);
        if (failure != verifier::FailureKind::kNoFailure) {
          passed_class_verification = false;
        }
      }
    }

    // Delete global ref and unload class loader to free RAM.
    soa.Env()->GetVm()->DeleteGlobalRef(soa.Self(), class_loader);
    // Do a GC to unregister the dex files.
    runtime->GetHeap()->CollectGarbage(/* clear_soft_references= */ true);

    ASSERT_EQ(passed_class_verification, expected_success) << " Failed for " << name;
  }

  void TestFuzzerHelper(
      const std::string& archive_filename,
      const std::unordered_set<std::string>& valid_dex_files,
      std::function<void(const uint8_t*, size_t, const std::string&, bool)> verify_file) {
    // Consistency checks.
    const std::string folder = android::base::GetExecutableDirectory();
    ASSERT_TRUE(std::filesystem::is_directory(folder)) << folder << " is not a folder";
    ASSERT_FALSE(std::filesystem::is_empty(folder)) << " No files found for directory " << folder;
    const std::string filename = folder + "/" + archive_filename;

    // Iterate using ZipArchiveHandle. We have to be careful about managing the pointers with
    // CloseArchive, StartIteration, and EndIteration.
    std::string error_msg;
    ZipArchiveHandle handle;
    ZipArchiveHandleScope scope(&handle);
    int32_t error = OpenArchive(filename.c_str(), &handle);
    ASSERT_TRUE(error == 0) << "Error: " << error;

    void* cookie;
    error = StartIteration(handle, &cookie);
    ASSERT_TRUE(error == 0) << "couldn't iterate " << filename << " : " << ErrorCodeString(error);

    ZipEntry64 entry;
    std::string name;
    std::vector<char> data;
    while ((error = Next(cookie, &entry, &name)) >= 0) {
      if (!name.ends_with(".dex")) {
        // Skip non-DEX files.
        LOG(WARNING) << "Found a non-dex file: " << name;
        continue;
      }
      data.resize(entry.uncompressed_length);
      error = ExtractToMemory(handle, &entry, reinterpret_cast<uint8_t*>(data.data()), data.size());
      ASSERT_TRUE(error == 0) << "failed to extract entry: " << name << " from " << filename << ""
                              << ErrorCodeString(error);

      const uint8_t* file_data = reinterpret_cast<const uint8_t*>(data.data());
      // Special case for empty dex file. Set a fake data since the size is 0 anyway.
      if (file_data == nullptr) {
        ASSERT_EQ(data.size(), 0);
        file_data = reinterpret_cast<const uint8_t*>(&name);
      }

      const bool is_valid_dex_file = valid_dex_files.find(name) != valid_dex_files.end();
      verify_file(file_data, data.size(), name, is_valid_dex_file);
    }

    ASSERT_TRUE(error >= -1) << "failed iterating " << filename << " : " << ErrorCodeString(error);
    EndIteration(cookie);
  }

 private:
  static jobject RegisterDexFileAndGetClassLoader(art::Runtime* runtime,
                                                  art::StandardDexFile* dex_file)
      REQUIRES_SHARED(art::Locks::mutator_lock_) {
    art::Thread* self = art::Thread::Current();
    art::ClassLinker* class_linker = runtime->GetClassLinker();
    const std::vector<const art::DexFile*> dex_files = {dex_file};
    jobject class_loader = class_linker->CreatePathClassLoader(self, dex_files);
    art::ObjPtr<art::mirror::ClassLoader> cl = self->DecodeJObject(class_loader)->AsClassLoader();
    class_linker->RegisterDexFile(*dex_file, cl);
    return class_loader;
  }
};

// Tests that we can verify dex files without crashing.
TEST_F(FuzzerCorpusTest, VerifyCorpusDexFiles) {
  // These dex files are expected to pass verification. The others are regressions tests.
  const std::unordered_set<std::string> valid_dex_files = {"Main.dex", "hello_world.dex"};
  const std::string archive_filename = "dex_verification_fuzzer_corpus.zip";

  TestFuzzerHelper(archive_filename, valid_dex_files, DexFileVerification);
}

// Tests that we can verify classes from dex files without crashing.
TEST_F(FuzzerCorpusTest, VerifyCorpusClassDexFiles) {
  // These dex files are expected to pass verification. The others are regressions tests.
  const std::unordered_set<std::string> valid_dex_files = {"Main.dex", "hello_world.dex"};
  const std::string archive_filename = "class_verification_fuzzer_corpus.zip";

  TestFuzzerHelper(archive_filename, valid_dex_files, ClassVerification);
}

}  // namespace art
