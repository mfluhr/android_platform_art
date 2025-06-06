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

#ifndef ART_RUNTIME_CLASS_LINKER_H_
#define ART_RUNTIME_CLASS_LINKER_H_

#include <list>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "base/array_ref.h"
#include "base/hash_map.h"
#include "base/intrusive_forward_list.h"
#include "base/locks.h"
#include "base/macros.h"
#include "base/mutex.h"
#include "base/pointer_size.h"
#include "dex/class_accessor.h"
#include "dex/dex_file_types.h"
#include "gc_root.h"
#include "handle.h"
#include "interpreter/mterp/nterp.h"
#include "jni.h"
#include "mirror/class.h"
#include "mirror/object.h"
#include "oat/jni_stub_hash_map.h"
#include "oat/oat_file.h"
#include "verifier/verifier_enums.h"

namespace art HIDDEN {

class ArtField;
class ArtMethod;
class ClassHierarchyAnalysis;
class ClassLoaderContext;
enum class ClassRoot : uint32_t;
class ClassTable;
class DexFile;
template<class T> class Handle;
class ImtConflictTable;
template<typename T> class LengthPrefixedArray;
template<class T> class MutableHandle;
class InternTable;
class LinearAlloc;
class OatFile;
template<class T> class ObjectLock;
class Runtime;
class ScopedObjectAccessAlreadyRunnable;
template<size_t kNumReferences> class PACKED(4) StackHandleScope;
class Thread;
class VariableSizedHandleScope;

enum VisitRootFlags : uint8_t;

namespace dex {
struct ClassDef;
struct MethodHandleItem;
}  // namespace dex

namespace gc {
namespace space {
class ImageSpace;
}  // namespace space
}  // namespace gc

namespace linker {
struct CompilationHelper;
class ImageWriter;
class OatWriter;
}  // namespace linker

namespace mirror {
class ClassLoader;
class DexCache;
class DexCachePointerArray;
class DexCacheMethodHandlesTest_Open_Test;
class DexCacheTest_Open_Test;
class IfTable;
class MethodHandle;
class MethodHandlesLookup;
class MethodType;
template<class T> class ObjectArray;
class RawMethodType;
class StackTraceElement;
}  // namespace mirror

namespace verifier {
class VerifierDeps;
}

class ClassVisitor {
 public:
  virtual ~ClassVisitor() {}
  // Return true to continue visiting.
  virtual bool operator()(ObjPtr<mirror::Class> klass) = 0;
};

template <typename Func>
class ClassFuncVisitor final : public ClassVisitor {
 public:
  explicit ClassFuncVisitor(Func func) : func_(func) {}
  bool operator()(ObjPtr<mirror::Class> klass) override REQUIRES_SHARED(Locks::mutator_lock_) {
    return func_(klass);
  }

 private:
  Func func_;
};

class ClassLoaderVisitor {
 public:
  virtual ~ClassLoaderVisitor() {}
  virtual void Visit(ObjPtr<mirror::ClassLoader> class_loader)
      REQUIRES_SHARED(Locks::classlinker_classes_lock_, Locks::mutator_lock_) = 0;
};

class DexCacheVisitor {
 public:
  virtual ~DexCacheVisitor() {}
  virtual void Visit(ObjPtr<mirror::DexCache> dex_cache)
      REQUIRES_SHARED(Locks::dex_lock_, Locks::mutator_lock_) = 0;
};

template <typename Func>
class ClassLoaderFuncVisitor final : public ClassLoaderVisitor {
 public:
  explicit ClassLoaderFuncVisitor(Func func) : func_(func) {}
  void Visit(ObjPtr<mirror::ClassLoader> cl) override REQUIRES_SHARED(Locks::mutator_lock_) {
    func_(cl);
  }

 private:
  Func func_;
};

class AllocatorVisitor {
 public:
  virtual ~AllocatorVisitor() {}
  // Return true to continue visiting.
  virtual bool Visit(LinearAlloc* alloc)
      REQUIRES_SHARED(Locks::classlinker_classes_lock_, Locks::mutator_lock_) = 0;
};

class ClassLinker {
 public:
  static constexpr bool kAppImageMayContainStrings = true;

  EXPORT explicit ClassLinker(InternTable* intern_table,
                              bool fast_class_not_found_exceptions = true);
  EXPORT virtual ~ClassLinker();

  // Initialize class linker by bootstraping from dex files.
  bool InitWithoutImage(std::vector<std::unique_ptr<const DexFile>> boot_class_path,
                        std::string* error_msg)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_);

  // Initialize class linker from one or more boot images.
  bool InitFromBootImage(std::string* error_msg)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_);

  // Add boot class path dex files that were not included in the boot image.
  // ClassLinker takes ownership of these dex files.
  // DO NOT use directly. Use `Runtime::AddExtraBootDexFiles`.
  void AddExtraBootDexFiles(Thread* self,
                            std::vector<std::unique_ptr<const DexFile>>&& additional_dex_files)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Add image spaces to the class linker, may fix up classloader fields and dex cache fields.
  // The dex files that were newly opened for the space are placed in the out argument `dex_files`.
  // Returns true if the operation succeeded.
  // The space must be already added to the heap before calling AddImageSpace since we need to
  // properly handle read barriers and object marking.
  bool AddImageSpaces(ArrayRef<gc::space::ImageSpace*> spaces,
                      Handle<mirror::ClassLoader> class_loader,
                      ClassLoaderContext* context,
                      /*out*/ std::vector<std::unique_ptr<const DexFile>>* dex_files,
                      /*out*/ std::string* error_msg) REQUIRES(!Locks::dex_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);

  EXPORT bool OpenImageDexFiles(gc::space::ImageSpace* space,
                                std::vector<std::unique_ptr<const DexFile>>* out_dex_files,
                                std::string* error_msg)
      REQUIRES(!Locks::dex_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Finds a class by its descriptor, loading it if necessary.
  // If class_loader is null, searches boot_class_path_.
  EXPORT ObjPtr<mirror::Class> FindClass(Thread* self,
                                         const char* descriptor,
                                         size_t descriptor_length,
                                         Handle<mirror::ClassLoader> class_loader)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_);

  // Helper overload that retrieves the descriptor and its length from the `dex_file`.
  EXPORT ObjPtr<mirror::Class> FindClass(Thread* self,
                                         const DexFile& dex_file,
                                         dex::TypeIndex type_index,
                                         Handle<mirror::ClassLoader> class_loader)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_);

  // Finds a class by its descriptor using the "system" class loader, ie by searching the
  // boot_class_path_.
  ObjPtr<mirror::Class> FindSystemClass(Thread* self, const char* descriptor)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_) {
    return FindClass(self, descriptor, strlen(descriptor), ScopedNullHandle<mirror::ClassLoader>());
  }

  // Finds the array class given for the element class.
  ObjPtr<mirror::Class> FindArrayClass(Thread* self, ObjPtr<mirror::Class> element_class)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_);

  // Returns true if the class linker is initialized.
  bool IsInitialized() const {
    return init_done_;
  }

  // Define a new a class based on a ClassDef from a DexFile
  ObjPtr<mirror::Class> DefineClass(Thread* self,
                                    const char* descriptor,
                                    size_t descriptor_length,
                                    size_t hash,
                                    Handle<mirror::ClassLoader> class_loader,
                                    const DexFile& dex_file,
                                    const dex::ClassDef& dex_class_def)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_);

  // Finds a class by its descriptor, returning null if it isn't wasn't loaded
  // by the given 'class_loader'.
  EXPORT ObjPtr<mirror::Class> LookupClass(Thread* self,
                                           std::string_view descriptor,
                                           ObjPtr<mirror::ClassLoader> class_loader)
      REQUIRES(!Locks::classlinker_classes_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);

  ObjPtr<mirror::Class> LookupPrimitiveClass(char type) REQUIRES_SHARED(Locks::mutator_lock_);
  ObjPtr<mirror::Class> FindPrimitiveClass(char type) REQUIRES_SHARED(Locks::mutator_lock_);

  void DumpForSigQuit(std::ostream& os) REQUIRES(!Locks::classlinker_classes_lock_);

  size_t NumLoadedClasses()
      REQUIRES(!Locks::classlinker_classes_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Resolve a String with the given index from the DexFile associated with the given `referrer`,
  // storing the result in the DexCache. The `referrer` is used to identify the target DexCache
  // to use for resolution.
  ObjPtr<mirror::String> ResolveString(dex::StringIndex string_idx,
                                       ArtField* referrer)
      REQUIRES_SHARED(Locks::mutator_lock_);
  ObjPtr<mirror::String> ResolveString(dex::StringIndex string_idx,
                                       ArtMethod* referrer)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Resolve a String with the given index from the DexFile associated with the given DexCache,
  // storing the result in the DexCache.
  ObjPtr<mirror::String> ResolveString(dex::StringIndex string_idx,
                                       Handle<mirror::DexCache> dex_cache)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Find a String with the given index from the DexFile associated with the given DexCache,
  // storing the result in the DexCache if found. Return null if not found.
  ObjPtr<mirror::String> LookupString(dex::StringIndex string_idx,
                                      ObjPtr<mirror::DexCache> dex_cache)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Resolve a Type with the given index from the DexFile associated with the given `referrer`,
  // storing the result in the DexCache. The `referrer` is used to identify the target DexCache
  // and ClassLoader to use for resolution.
  ObjPtr<mirror::Class> ResolveType(dex::TypeIndex type_idx, ObjPtr<mirror::Class> referrer)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_, !Roles::uninterruptible_);
  ObjPtr<mirror::Class> ResolveType(dex::TypeIndex type_idx, ArtField* referrer)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_, !Roles::uninterruptible_);
  ObjPtr<mirror::Class> ResolveType(dex::TypeIndex type_idx, ArtMethod* referrer)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_, !Roles::uninterruptible_);

  // Resolve a type with the given index from the DexFile associated with the given DexCache
  // and ClassLoader, storing the result in DexCache. The ClassLoader is used to search for
  // the type, since it may be referenced from but not contained within the DexFile.
  ObjPtr<mirror::Class> ResolveType(dex::TypeIndex type_idx,
                                    Handle<mirror::DexCache> dex_cache,
                                    Handle<mirror::ClassLoader> class_loader)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_, !Roles::uninterruptible_);

  // Look up a resolved type with the given index from the DexFile associated with the given
  // `referrer`, storing the result in the DexCache. The `referrer` is used to identify the
  // target DexCache and ClassLoader to use for lookup.
  ObjPtr<mirror::Class> LookupResolvedType(dex::TypeIndex type_idx,
                                           ObjPtr<mirror::Class> referrer)
      REQUIRES_SHARED(Locks::mutator_lock_);
  ObjPtr<mirror::Class> LookupResolvedType(dex::TypeIndex type_idx, ArtField* referrer)
      REQUIRES_SHARED(Locks::mutator_lock_);
  ObjPtr<mirror::Class> LookupResolvedType(dex::TypeIndex type_idx, ArtMethod* referrer)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Look up a resolved type with the given index from the DexFile associated with the given
  // DexCache and ClassLoader. The ClassLoader is used to search for the type, since it may
  // be referenced from but not contained within the DexFile.
  ObjPtr<mirror::Class> LookupResolvedType(dex::TypeIndex type_idx,
                                           ObjPtr<mirror::DexCache> dex_cache,
                                           ObjPtr<mirror::ClassLoader> class_loader)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Look up a resolved type with the given descriptor associated with the given ClassLoader.
  ObjPtr<mirror::Class> LookupResolvedType(std::string_view descriptor,
                                           ObjPtr<mirror::ClassLoader> class_loader)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Look up a previously resolved method with the given index.
  ArtMethod* LookupResolvedMethod(uint32_t method_idx,
                                  ObjPtr<mirror::DexCache> dex_cache,
                                  ObjPtr<mirror::ClassLoader> class_loader)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Find a method with the given index from class `klass`, and update the dex cache.
  EXPORT ArtMethod* FindResolvedMethod(ObjPtr<mirror::Class> klass,
                                       ObjPtr<mirror::DexCache> dex_cache,
                                       ObjPtr<mirror::ClassLoader> class_loader,
                                       uint32_t method_idx)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Find a method using the wrong lookup mechanism. If `klass` is an interface,
  // search for a class method. If it is a class, search for an interface method.
  // This is useful when throwing IncompatibleClassChangeError.
  ArtMethod* FindIncompatibleMethod(ObjPtr<mirror::Class> klass,
                                    ObjPtr<mirror::DexCache> dex_cache,
                                    ObjPtr<mirror::ClassLoader> class_loader,
                                    uint32_t method_idx)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Check invoke type against the referenced class. Throws IncompatibleClassChangeError
  // and returns true on mismatch (kInterface on a non-interface class,
  // kVirtual on interface, kDefault on interface for dex files not supporting default methods),
  // otherwise returns false.
  static bool ThrowIfInvokeClassMismatch(ObjPtr<mirror::Class> cls,
                                         const DexFile& dex_file,
                                         InvokeType type)
      REQUIRES_SHARED(Locks::mutator_lock_);

  ArtMethod* ResolveMethodWithChecks(uint32_t method_idx, ArtMethod* referrer, InvokeType type)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_, !Roles::uninterruptible_);

  EXPORT ArtMethod* ResolveMethodId(uint32_t method_idx,
                                    Handle<mirror::DexCache> dex_cache,
                                    Handle<mirror::ClassLoader> class_loader)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_, !Roles::uninterruptible_);

  ArtMethod* ResolveMethodId(uint32_t method_idx, ArtMethod* referrer)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_, !Roles::uninterruptible_);

  ArtField* LookupResolvedField(uint32_t field_idx, ArtMethod* referrer, bool is_static)
      REQUIRES_SHARED(Locks::mutator_lock_);
  // Find a field by its field index.
  ArtField* LookupResolvedField(uint32_t field_idx,
                                ObjPtr<mirror::DexCache> dex_cache,
                                ObjPtr<mirror::ClassLoader> class_loader,
                                bool is_static)
      REQUIRES_SHARED(Locks::mutator_lock_);
  ArtField* ResolveField(uint32_t field_idx, ArtMethod* referrer, bool is_static)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_, !Roles::uninterruptible_);

  // Resolve a field with a given ID from the DexFile associated with the given DexCache
  // and ClassLoader, storing the result in DexCache. The ClassLinker and ClassLoader
  // are used as in ResolveType. What is unique is the is_static argument which is used
  // to determine if we are resolving a static or non-static field.
  ArtField* ResolveField(uint32_t field_idx,
                         Handle<mirror::DexCache> dex_cache,
                         Handle<mirror::ClassLoader> class_loader,
                         bool is_static)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_, !Roles::uninterruptible_);

  // Resolve a field with a given ID from the DexFile associated with the given DexCache
  // and ClassLoader, storing the result in DexCache. The ClassLinker and ClassLoader
  // are used as in ResolveType. No is_static argument is provided so that Java
  // field resolution semantics are followed.
  EXPORT ArtField* ResolveFieldJLS(uint32_t field_idx,
                                   Handle<mirror::DexCache> dex_cache,
                                   Handle<mirror::ClassLoader> class_loader)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_, !Roles::uninterruptible_);

  // Find a field with a given ID from the DexFile associated with the given DexCache
  // and ClassLoader, storing the result in DexCache. The declaring class is assumed
  // to have been already resolved into `klass`. The `is_static` argument is used to
  // determine if we are resolving a static or non-static field.
  EXPORT ArtField* FindResolvedField(ObjPtr<mirror::Class> klass,
                                     ObjPtr<mirror::DexCache> dex_cache,
                                     ObjPtr<mirror::ClassLoader> class_loader,
                                     uint32_t field_idx,
                                     bool is_static)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Find a field with a given ID from the DexFile associated with the given DexCache
  // and ClassLoader, storing the result in DexCache. The declaring class is assumed
  // to have been already resolved into `klass`. No is_static argument is provided
  // so that Java field resolution semantics are followed.
  ArtField* FindResolvedFieldJLS(ObjPtr<mirror::Class> klass,
                                 ObjPtr<mirror::DexCache> dex_cache,
                                 ObjPtr<mirror::ClassLoader> class_loader,
                                 uint32_t field_idx)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Resolve a method type with a given ID from the DexFile associated with a given DexCache
  // and ClassLoader, storing the result in the DexCache.
  ObjPtr<mirror::MethodType> ResolveMethodType(Thread* self,
                                               dex::ProtoIndex proto_idx,
                                               Handle<mirror::DexCache> dex_cache,
                                               Handle<mirror::ClassLoader> class_loader)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_, !Roles::uninterruptible_);

  EXPORT ObjPtr<mirror::MethodType> ResolveMethodType(Thread* self,
                                                      dex::ProtoIndex proto_idx,
                                                      ArtMethod* referrer)
      REQUIRES_SHARED(Locks::mutator_lock_);

  bool ResolveMethodType(Thread* self,
                         dex::ProtoIndex proto_idx,
                         Handle<mirror::DexCache> dex_cache,
                         Handle<mirror::ClassLoader> class_loader,
                         /*out*/ mirror::RawMethodType method_type)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_, !Roles::uninterruptible_);

  // Resolve a method handle with a given ID from the DexFile. The
  // result is not cached in the DexCache as the instance will only be
  // used once in most circumstances.
  EXPORT ObjPtr<mirror::MethodHandle> ResolveMethodHandle(Thread* self,
                                                          uint32_t method_handle_idx,
                                                          ArtMethod* referrer)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Returns true on success, false if there's an exception pending.
  // can_run_clinit=false allows the compiler to attempt to init a class,
  // given the restriction that no <clinit> execution is possible.
  EXPORT bool EnsureInitialized(Thread* self,
                                Handle<mirror::Class> c,
                                bool can_init_fields,
                                bool can_init_parents)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_, !Roles::uninterruptible_);

  // Initializes a few essential classes, namely `java.lang.Class`,
  // `java.lang.Object` and `java.lang.reflect.Field`.
  EXPORT  void RunEarlyRootClinits(Thread* self)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_, !Roles::uninterruptible_);

  // Initializes classes that have instances in the image but that have
  // <clinit> methods so they could not be initialized by the compiler.
  void RunRootClinits(Thread* self)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_, !Roles::uninterruptible_);

  // Directly register an already existing dex cache. RegisterDexFile should be preferred since that
  // reduplicates DexCaches when possible. The DexCache given to this function must already be fully
  // initialized and not already registered.
  EXPORT void RegisterExistingDexCache(ObjPtr<mirror::DexCache> cache,
                                       ObjPtr<mirror::ClassLoader> class_loader)
      REQUIRES(!Locks::dex_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);
  EXPORT ObjPtr<mirror::DexCache> RegisterDexFile(const DexFile& dex_file,
                                                  ObjPtr<mirror::ClassLoader> class_loader)
      REQUIRES(!Locks::dex_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);

  const std::vector<const DexFile*>& GetBootClassPath() {
    return boot_class_path_;
  }

  EXPORT void VisitClasses(ClassVisitor* visitor)
      REQUIRES(!Locks::classlinker_classes_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Visits only the classes in the boot class path.
  template <typename Visitor>
  inline void VisitBootClasses(Visitor* visitor)
      REQUIRES_SHARED(Locks::classlinker_classes_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);
  // Less efficient variant of VisitClasses that copies the class_table_ into secondary storage
  // so that it can visit individual classes without holding the doesn't hold the
  // Locks::classlinker_classes_lock_. As the Locks::classlinker_classes_lock_ isn't held this code
  // can race with insertion and deletion of classes while the visitor is being called.
  EXPORT void VisitClassesWithoutClassesLock(ClassVisitor* visitor)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_);

  void VisitClassRoots(RootVisitor* visitor, VisitRootFlags flags)
      REQUIRES(!Locks::classlinker_classes_lock_, !Locks::trace_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);
  void VisitRoots(RootVisitor* visitor, VisitRootFlags flags, bool visit_class_roots = true)
      REQUIRES(!Locks::dex_lock_, !Locks::classlinker_classes_lock_, !Locks::trace_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);
  // Visits all dex-files accessible by any class-loader or the BCP.
  template<typename Visitor>
  void VisitKnownDexFiles(Thread* self, Visitor visitor) REQUIRES(Locks::mutator_lock_);

  EXPORT bool IsDexFileRegistered(Thread* self, const DexFile& dex_file)
      REQUIRES(!Locks::dex_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);
  EXPORT ObjPtr<mirror::DexCache> FindDexCache(Thread* self, const DexFile& dex_file)
      REQUIRES(!Locks::dex_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);
  ObjPtr<mirror::DexCache> FindDexCache(Thread* self, const OatDexFile& oat_dex_file)
      REQUIRES(!Locks::dex_lock_) REQUIRES_SHARED(Locks::mutator_lock_);
  ClassTable* FindClassTable(Thread* self, ObjPtr<mirror::DexCache> dex_cache)
      REQUIRES(!Locks::dex_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Allocating `ArtField` and `ArtMethod` arrays can lead to adding new arenas to
  // the `LinearAlloc` but the CMC GC's `CompactionPause()` does not expect new
  // arenas being concurrently added. Therefore we require these allocations to be
  // done with the mutator lock held shared as this prevents concurrent execution
  // with the `CompactionPause()` where we hold the mutator lock exclusively.
  LengthPrefixedArray<ArtField>* AllocArtFieldArray(Thread* self,
                                                    LinearAlloc* allocator,
                                                    size_t length)
      REQUIRES_SHARED(Locks::mutator_lock_);
  LengthPrefixedArray<ArtMethod>* AllocArtMethodArray(Thread* self,
                                                      LinearAlloc* allocator,
                                                      size_t length)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Convenience AllocClass() overload that uses mirror::Class::InitializeClassVisitor
  // for the class initialization and uses the `java_lang_Class` from class roots
  // instead of an explicit argument.
  EXPORT ObjPtr<mirror::Class> AllocClass(Thread* self, uint32_t class_size)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Roles::uninterruptible_);

  // Setup the classloader, class def index, type idx so that we can insert this class in the class
  // table.
  EXPORT void SetupClass(const DexFile& dex_file,
                         const dex::ClassDef& dex_class_def,
                         Handle<mirror::Class> klass,
                         ObjPtr<mirror::ClassLoader> class_loader)
      REQUIRES_SHARED(Locks::mutator_lock_);

  EXPORT void LoadClass(Thread* self,
                        const DexFile& dex_file,
                        const dex::ClassDef& dex_class_def,
                        Handle<mirror::Class> klass)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Link the class and place it into the class-table using the given descriptor. NB if the
  // descriptor is null the class will not be placed in any class-table. This is useful implementing
  // obsolete classes and should not be used otherwise.
  EXPORT bool LinkClass(Thread* self,
                        const char* descriptor,
                        Handle<mirror::Class> klass,
                        Handle<mirror::ObjectArray<mirror::Class>> interfaces,
                        MutableHandle<mirror::Class>* h_new_class_out)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::classlinker_classes_lock_);

  ObjPtr<mirror::PointerArray> AllocPointerArray(Thread* self, size_t length)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Roles::uninterruptible_);

  ObjPtr<mirror::ObjectArray<mirror::StackTraceElement>> AllocStackTraceElementArray(Thread* self,
                                                                                     size_t length)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Roles::uninterruptible_);

  EXPORT verifier::FailureKind VerifyClass(
      Thread* self,
      verifier::VerifierDeps* verifier_deps,
      Handle<mirror::Class> klass,
      verifier::HardFailLogMode log_level = verifier::HardFailLogMode::kLogNone)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_);
  EXPORT  // For `libarttest.so`.
  bool VerifyClassUsingOatFile(Thread* self,
                               const DexFile& dex_file,
                               Handle<mirror::Class> klass,
                               ClassStatus& oat_file_class_status)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_);
  void ResolveClassExceptionHandlerTypes(Handle<mirror::Class> klass)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_);
  void ResolveMethodExceptionHandlerTypes(ArtMethod* klass)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_);

  ObjPtr<mirror::Class> CreateProxyClass(ScopedObjectAccessAlreadyRunnable& soa,
                                         jstring name,
                                         jobjectArray interfaces,
                                         jobject loader,
                                         jobjectArray methods,
                                         jobjectArray throws)
      REQUIRES_SHARED(Locks::mutator_lock_);

  pid_t GetClassesLockOwner();  // For SignalCatcher.
  pid_t GetDexLockOwner();  // For SignalCatcher.

  // Is the given entry point quick code to run the resolution stub?
  EXPORT bool IsQuickResolutionStub(const void* entry_point) const;

  // Is the given entry point quick code to bridge into the interpreter?
  EXPORT bool IsQuickToInterpreterBridge(const void* entry_point) const;

  // Is the given entry point quick code to run the generic JNI stub?
  EXPORT bool IsQuickGenericJniStub(const void* entry_point) const;

  // Is the given entry point the JNI dlsym lookup stub?
  EXPORT bool IsJniDlsymLookupStub(const void* entry_point) const;

  // Is the given entry point the JNI dlsym lookup critical stub?
  EXPORT bool IsJniDlsymLookupCriticalStub(const void* entry_point) const;

  // Is the given entry point the nterp trampoline?
  bool IsNterpTrampoline(const void* entry_point) const {
    return nterp_trampoline_ == entry_point;
  }

  bool IsNterpEntryPoint(const void* entry_point) const {
    return entry_point == interpreter::GetNterpEntryPoint() ||
        entry_point == interpreter::GetNterpWithClinitEntryPoint();
  }

  const void* GetQuickToInterpreterBridgeTrampoline() const {
    return quick_to_interpreter_bridge_trampoline_;
  }

  InternTable* GetInternTable() const {
    return intern_table_;
  }

  // Set the entrypoints up for an obsolete method.
  EXPORT void SetEntryPointsForObsoleteMethod(ArtMethod* method) const
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Attempts to insert a class into a class table.  Returns null if
  // the class was inserted, otherwise returns an existing class with
  // the same descriptor and ClassLoader.
  ObjPtr<mirror::Class> InsertClass(std::string_view descriptor,
                                    ObjPtr<mirror::Class> klass,
                                    size_t hash)
      REQUIRES(!Locks::classlinker_classes_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Add an oat file with .bss GC roots to be visited again at the end of GC
  // for collector types that need it.
  void WriteBarrierForBootOatFileBssRoots(const OatFile* oat_file)
      REQUIRES(!Locks::classlinker_classes_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);

  template <ReadBarrierOption kReadBarrierOption = kWithReadBarrier>
  ObjPtr<mirror::ObjectArray<mirror::Class>> GetClassRoots() REQUIRES_SHARED(Locks::mutator_lock_);

  // Move the class table to the pre-zygote table to reduce memory usage. This works by ensuring
  // that no more classes are ever added to the pre zygote table which makes it that the pages
  // always remain shared dirty instead of private dirty.
  void MoveClassTableToPreZygote()
      REQUIRES(!Locks::classlinker_classes_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Calls `CreateWellKnownClassLoader()` with `WellKnownClasses::dalvik_system_PathClassLoader`,
  // and null parent and libraries. Wraps the result in a JNI global reference.
  EXPORT jobject CreatePathClassLoader(Thread* self, const std::vector<const DexFile*>& dex_files)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_);

  // Creates a `PathClassLoader`, `DelegateLastClassLoader` or `InMemoryDexClassLoader`
  // (specified by loader_class) that can be used to load classes from the given dex files.
  // The parent of the class loader will be set to `parent_loader`. If `parent_loader` is
  // null the parent will be the boot class loader.
  // If `loader_class` points to a different class than `PathClassLoader`,
  // `DelegateLastClassLoader` or `InMemoryDexClassLoader` this method will abort.
  // Note: the objects are not completely set up. Do not use this outside of tests and the compiler.
  ObjPtr<mirror::ClassLoader> CreateWellKnownClassLoader(
      Thread* self,
      const std::vector<const DexFile*>& dex_files,
      Handle<mirror::Class> loader_class,
      Handle<mirror::ClassLoader> parent_loader,
      Handle<mirror::ObjectArray<mirror::ClassLoader>> shared_libraries,
      Handle<mirror::ObjectArray<mirror::ClassLoader>> shared_libraries_after)
          REQUIRES_SHARED(Locks::mutator_lock_)
          REQUIRES(!Locks::dex_lock_);

  PointerSize GetImagePointerSize() const {
    return image_pointer_size_;
  }

  // Clear the ArrayClass cache. This is necessary when cleaning up for the image, as the cache
  // entries are roots, but potentially not image classes.
  EXPORT void DropFindArrayClassCache() REQUIRES_SHARED(Locks::mutator_lock_);

  // Clean up class loaders, this needs to happen after JNI weak globals are cleared.
  void CleanupClassLoaders()
      REQUIRES(!Locks::classlinker_classes_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Unlike GetOrCreateAllocatorForClassLoader, GetAllocatorForClassLoader asserts that the
  // allocator for this class loader is already created.
  EXPORT LinearAlloc* GetAllocatorForClassLoader(ObjPtr<mirror::ClassLoader> class_loader)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Return the linear alloc for a class loader if it is already allocated, otherwise allocate and
  // set it. TODO: Consider using a lock other than classlinker_classes_lock_.
  LinearAlloc* GetOrCreateAllocatorForClassLoader(ObjPtr<mirror::ClassLoader> class_loader)
      REQUIRES(!Locks::classlinker_classes_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // May be called with null class_loader due to legacy code. b/27954959
  void InsertDexFileInToClassLoader(ObjPtr<mirror::Object> dex_file,
                                    ObjPtr<mirror::ClassLoader> class_loader)
      REQUIRES(!Locks::classlinker_classes_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);

  EXPORT static bool IsBootClassLoader(ObjPtr<mirror::Object> class_loader)
      REQUIRES_SHARED(Locks::mutator_lock_);

  ArtMethod* AddMethodToConflictTable(ObjPtr<mirror::Class> klass,
                                      ArtMethod* conflict_method,
                                      ArtMethod* interface_method,
                                      ArtMethod* method)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Create a conflict table with a specified capacity.
  ImtConflictTable* CreateImtConflictTable(size_t count, LinearAlloc* linear_alloc);

  // Static version for when the class linker is not yet created.
  static ImtConflictTable* CreateImtConflictTable(size_t count,
                                                  LinearAlloc* linear_alloc,
                                                  PointerSize pointer_size);


  // Create the IMT and conflict tables for a class.
  EXPORT void FillIMTAndConflictTables(ObjPtr<mirror::Class> klass)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Visit all of the class tables. This is used by dex2oat to allow pruning dex caches.
  template <class Visitor>
  void VisitClassTables(const Visitor& visitor)
      REQUIRES(!Locks::classlinker_classes_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Visit all of the allocators that belong to classloaders except boot classloader.
  // This is used by 616-cha-unloading test to confirm memory reuse.
  EXPORT  // For `libarttest.so`.
  void VisitAllocators(AllocatorVisitor* visitor) const
      REQUIRES_SHARED(Locks::classlinker_classes_lock_, Locks::mutator_lock_);

  // Throw the class initialization failure recorded when first trying to initialize the given
  // class.
  void ThrowEarlierClassFailure(ObjPtr<mirror::Class> c,
                                bool wrap_in_no_class_def = false,
                                bool log = false)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_);

  // Get the actual holding class for a copied method. Pretty slow, don't call often.
  ObjPtr<mirror::Class> GetHoldingClassOfCopiedMethod(ArtMethod* method)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Get the class loader holding class for a copied method.
  ObjPtr<mirror::ClassLoader> GetHoldingClassLoaderOfCopiedMethod(Thread* self, ArtMethod* method)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::classlinker_classes_lock_);

  void GetClassLoaders(Thread* self, VariableSizedHandleScope* handles)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::classlinker_classes_lock_);

  // Returns null if not found.
  // This returns a pointer to the class-table, without requiring any locking - including the
  // boot class-table. It is the caller's responsibility to access this under lock, if required.
  EXPORT ClassTable* ClassTableForClassLoader(ObjPtr<mirror::ClassLoader> class_loader)
      REQUIRES_SHARED(Locks::mutator_lock_)
      NO_THREAD_SAFETY_ANALYSIS;

  // Dirty card in the card-table corresponding to the class_loader. Also log
  // the root if we are logging new roots and class_loader is null.
  void WriteBarrierOnClassLoaderLocked(ObjPtr<mirror::ClassLoader> class_loader,
                                       ObjPtr<mirror::Object> root)
      REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(Locks::classlinker_classes_lock_);
  void WriteBarrierOnClassLoader(Thread* self,
                                 ObjPtr<mirror::ClassLoader> class_loader,
                                 ObjPtr<mirror::Object> root) REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::classlinker_classes_lock_);

  // DO NOT use directly. Use `Runtime::AppendToBootClassPath`.
  void AppendToBootClassPath(Thread* self, const DexFile* dex_file)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_);

  // DO NOT use directly. Use `Runtime::AppendToBootClassPath`.
  void AppendToBootClassPath(const DexFile* dex_file, ObjPtr<mirror::DexCache> dex_cache)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_);

  // Visit all of the class loaders in the class linker.
  EXPORT void VisitClassLoaders(ClassLoaderVisitor* visitor) const
      REQUIRES_SHARED(Locks::classlinker_classes_lock_, Locks::mutator_lock_);

  // Visit all of the dex caches in the class linker.
  void VisitDexCaches(DexCacheVisitor* visitor) const
      REQUIRES_SHARED(Locks::dex_lock_, Locks::mutator_lock_);

  // Checks that a class and its superclass from another class loader have the same virtual methods.
  EXPORT bool ValidateSuperClassDescriptors(Handle<mirror::Class> klass)
      REQUIRES_SHARED(Locks::mutator_lock_);

  ClassHierarchyAnalysis* GetClassHierarchyAnalysis() {
    return cha_.get();
  }

  EXPORT
  void MakeInitializedClassesVisiblyInitialized(Thread* self, bool wait /* ==> no locks held */);

  // Registers the native method and returns the new entry point. NB The returned entry point
  // might be different from the native_method argument if some MethodCallback modifies it.
  const void* RegisterNative(Thread* self, ArtMethod* method, const void* native_method)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Unregister native code for a method.
  void UnregisterNative(Thread* self, ArtMethod* method) REQUIRES_SHARED(Locks::mutator_lock_);

  // Get the registered native method entrypoint, if any, otherwise null.
  const void* GetRegisteredNative(Thread* self, ArtMethod* method)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!critical_native_code_with_clinit_check_lock_);

  struct DexCacheData {
    // Construct an invalid data object.
    DexCacheData() : weak_root(nullptr), class_table(nullptr) {
      static std::atomic_uint64_t s_registration_count(0);
      registration_index = s_registration_count.fetch_add(1, std::memory_order_seq_cst);
    }
    DexCacheData(DexCacheData&&) = default;

    // Weak root to the DexCache. Note: Do not decode this unnecessarily or else class unloading may
    // not work properly.
    jweak weak_root;
    // Identify the associated class loader's class table. This is used to make sure that
    // the Java call to native DexCache.setResolvedType() inserts the resolved type in that
    // class table. It is also used to make sure we don't register the same dex cache with
    // multiple class loaders.
    ClassTable* class_table;
    // Monotonically increasing integer which records the order in which DexFiles were registered.
    // Used only to preserve determinism when creating compiled image.
    uint64_t registration_index;

   private:
    DISALLOW_COPY_AND_ASSIGN(DexCacheData);
  };

  // Forces a class to be marked as initialized without actually running initializers. Should only
  // be used by plugin code when creating new classes directly.
  EXPORT void ForceClassInitialized(Thread* self, Handle<mirror::Class> klass)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_, !Roles::uninterruptible_);

  // Verifies if the method is accessible according to the SdkChecker (if installed).
  virtual bool DenyAccessBasedOnPublicSdk(ArtMethod* art_method) const
      REQUIRES_SHARED(Locks::mutator_lock_);
  // Verifies if the field is accessible according to the SdkChecker (if installed).
  virtual bool DenyAccessBasedOnPublicSdk(ArtField* art_field) const
      REQUIRES_SHARED(Locks::mutator_lock_);
  // Verifies if the descriptor is accessible according to the SdkChecker (if installed).
  virtual bool DenyAccessBasedOnPublicSdk(std::string_view type_descriptor) const;
  // Enable or disable public sdk checks.
  virtual void SetEnablePublicSdkChecks(bool enabled);

  // Transaction constraint checks for AOT compilation.
  virtual bool TransactionWriteConstraint(Thread* self, ObjPtr<mirror::Object> obj)
      REQUIRES_SHARED(Locks::mutator_lock_);
  virtual bool TransactionWriteValueConstraint(Thread* self, ObjPtr<mirror::Object> value)
      REQUIRES_SHARED(Locks::mutator_lock_);
  virtual bool TransactionAllocationConstraint(Thread* self, ObjPtr<mirror::Class> klass)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Transaction bookkeeping for AOT compilation.
  virtual void RecordWriteFieldBoolean(mirror::Object* obj,
                                       MemberOffset field_offset,
                                       uint8_t value,
                                       bool is_volatile);
  virtual void RecordWriteFieldByte(mirror::Object* obj,
                                    MemberOffset field_offset,
                                    int8_t value,
                                    bool is_volatile);
  virtual void RecordWriteFieldChar(mirror::Object* obj,
                                    MemberOffset field_offset,
                                    uint16_t value,
                                    bool is_volatile);
  virtual void RecordWriteFieldShort(mirror::Object* obj,
                                     MemberOffset field_offset,
                                     int16_t value,
                                     bool is_volatile);
  virtual void RecordWriteField32(mirror::Object* obj,
                                  MemberOffset field_offset,
                                  uint32_t value,
                                  bool is_volatile);
  virtual void RecordWriteField64(mirror::Object* obj,
                                  MemberOffset field_offset,
                                  uint64_t value,
                                  bool is_volatile);
  virtual void RecordWriteFieldReference(mirror::Object* obj,
                                         MemberOffset field_offset,
                                         ObjPtr<mirror::Object> value,
                                         bool is_volatile)
      REQUIRES_SHARED(Locks::mutator_lock_);
  virtual void RecordWriteArray(mirror::Array* array, size_t index, uint64_t value)
      REQUIRES_SHARED(Locks::mutator_lock_);
  virtual void RecordStrongStringInsertion(ObjPtr<mirror::String> s)
      REQUIRES(Locks::intern_table_lock_);
  virtual void RecordWeakStringInsertion(ObjPtr<mirror::String> s)
      REQUIRES(Locks::intern_table_lock_);
  virtual void RecordStrongStringRemoval(ObjPtr<mirror::String> s)
      REQUIRES(Locks::intern_table_lock_);
  virtual void RecordWeakStringRemoval(ObjPtr<mirror::String> s)
      REQUIRES(Locks::intern_table_lock_);
  virtual void RecordResolveString(ObjPtr<mirror::DexCache> dex_cache, dex::StringIndex string_idx)
      REQUIRES_SHARED(Locks::mutator_lock_);
  virtual void RecordResolveMethodType(ObjPtr<mirror::DexCache> dex_cache,
                                       dex::ProtoIndex proto_idx)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Aborting transactions for AOT compilation.
  virtual void ThrowTransactionAbortError(Thread* self)
      REQUIRES_SHARED(Locks::mutator_lock_);
  virtual void AbortTransactionF(Thread* self, const char* fmt, ...)
      __attribute__((__format__(__printf__, 3, 4)))
      REQUIRES_SHARED(Locks::mutator_lock_);
  virtual void AbortTransactionV(Thread* self, const char* fmt, va_list args)
      REQUIRES_SHARED(Locks::mutator_lock_);
  virtual bool IsTransactionAborted() const;

  // Visit transaction roots for AOT compilation.
  virtual void VisitTransactionRoots(RootVisitor* visitor)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Get transactional switch interpreter entrypoint for AOT compilation.
  virtual const void* GetTransactionalInterpreter();

  void RemoveDexFromCaches(const DexFile& dex_file);
  ClassTable* GetBootClassTable() REQUIRES_SHARED(Locks::classlinker_classes_lock_) {
    return boot_class_table_.get();
  }
  // Find a matching JNI stub from boot images that we could reuse as entrypoint.
  EXPORT const void* FindBootJniStub(ArtMethod* method)
      REQUIRES_SHARED(Locks::mutator_lock_);

  EXPORT const void* FindBootJniStub(uint32_t flags, std::string_view shorty);

  const void* FindBootJniStub(JniStubKey key);

 protected:
  EXPORT virtual bool InitializeClass(Thread* self,
                                      Handle<mirror::Class> klass,
                                      bool can_run_clinit,
                                      bool can_init_parents)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_);

  EXPORT
  virtual verifier::FailureKind PerformClassVerification(Thread* self,
                                                         verifier::VerifierDeps* verifier_deps,
                                                         Handle<mirror::Class> klass,
                                                         verifier::HardFailLogMode log_level,
                                                         std::string* error_msg)
      REQUIRES_SHARED(Locks::mutator_lock_);

  virtual bool CanAllocClass() REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(!Locks::dex_lock_) {
    return true;
  }

 private:
  class LinkFieldsHelper;
  template <PointerSize kPointerSize>
  class LinkMethodsHelper;
  class LoadClassHelper;
  class MethodAnnotationsIterator;
  class OatClassCodeIterator;
  class VisiblyInitializedCallback;

  struct ClassLoaderData {
    jweak weak_root;  // Weak root to enable class unloading.
    ClassTable* class_table;
    LinearAlloc* allocator;
  };

  void VisiblyInitializedCallbackDone(Thread* self, VisiblyInitializedCallback* callback);
  VisiblyInitializedCallback* MarkClassInitialized(Thread* self, Handle<mirror::Class> klass)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Ensures that the supertype of 'klass' ('supertype') is verified. Returns false and throws
  // appropriate exceptions if verification failed hard. Returns true for successful verification or
  // soft-failures.
  bool AttemptSupertypeVerification(Thread* self,
                                    verifier::VerifierDeps* verifier_deps,
                                    Handle<mirror::Class> klass,
                                    Handle<mirror::Class> supertype)
      REQUIRES(!Locks::dex_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Prepare by removing dependencies on things allocated in data.allocator.
  // Please note that the allocator and class_table are not deleted in this
  // function. They are to be deleted after preparing all the class-loaders that
  // are to be deleted (see b/298575095).
  void PrepareToDeleteClassLoader(Thread* self, const ClassLoaderData& data, bool cleanup_cha)
      REQUIRES_SHARED(Locks::mutator_lock_);

  void VisitClassesInternal(ClassVisitor* visitor)
      REQUIRES_SHARED(Locks::classlinker_classes_lock_, Locks::mutator_lock_);

  // Returns the number of zygote and image classes.
  size_t NumZygoteClasses() const
      REQUIRES(Locks::classlinker_classes_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Returns the number of non zygote nor image classes.
  size_t NumNonZygoteClasses() const
      REQUIRES(Locks::classlinker_classes_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);

  void FinishInit(Thread* self)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_, !Roles::uninterruptible_);

  // If we do not allow moving classes (`art::kMovingClass` is false) or if
  // parameter `kMovable` is false (or both), the class object is allocated in
  // the non-moving space.
  template <bool kMovable = true, class PreFenceVisitor>
  ObjPtr<mirror::Class> AllocClass(Thread* self,
                                   ObjPtr<mirror::Class> java_lang_Class,
                                   uint32_t class_size,
                                   const PreFenceVisitor& pre_fence_visitor)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Roles::uninterruptible_);

  // Convenience AllocClass() overload that uses mirror::Class::InitializeClassVisitor
  // for the class initialization.
  template <bool kMovable = true>
  ObjPtr<mirror::Class> AllocClass(Thread* self,
                                   ObjPtr<mirror::Class> java_lang_Class,
                                   uint32_t class_size)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Roles::uninterruptible_);

  // Allocate a primitive array class and store it in appropriate class root.
  void AllocPrimitiveArrayClass(Thread* self,
                                ClassRoot primitive_root,
                                ClassRoot array_root)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Roles::uninterruptible_);

  // Finish setup of an array class.
  void FinishArrayClassSetup(ObjPtr<mirror::Class> array_class)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Roles::uninterruptible_);

  // Finish setup of a core array class (Object[], Class[], String[] and
  // primitive arrays) and insert it into the class table.
  void FinishCoreArrayClassSetup(ClassRoot array_root)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Roles::uninterruptible_);

  ObjPtr<mirror::DexCache> AllocDexCache(Thread* self, const DexFile& dex_file)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Roles::uninterruptible_);

  // Used for tests and AppendToBootClassPath.
  ObjPtr<mirror::DexCache> AllocAndInitializeDexCache(Thread* self,
                                                      const DexFile& dex_file,
                                                      ObjPtr<mirror::ClassLoader> class_loader)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_)
      REQUIRES(!Roles::uninterruptible_);

  // Create a primitive class and store it in the appropriate class root.
  void CreatePrimitiveClass(Thread* self, Primitive::Type type, ClassRoot primitive_root)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Roles::uninterruptible_);

  ObjPtr<mirror::Class> CreateArrayClass(Thread* self,
                                         const char* descriptor,
                                         size_t descriptor_length,
                                         size_t hash,
                                         Handle<mirror::ClassLoader> class_loader)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_, !Roles::uninterruptible_);

  // Precomputes size needed for Class, in the case of a non-temporary class this size must be
  // sufficient to hold all static fields.
  uint32_t SizeOfClassWithoutEmbeddedTables(const DexFile& dex_file,
                                            const dex::ClassDef& dex_class_def);

  void FixupStaticTrampolines(Thread* self, ObjPtr<mirror::Class> klass)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Finds a class in a Path- or DexClassLoader, loading it if necessary without using JNI. Hash
  // function is supposed to be ComputeModifiedUtf8Hash(descriptor). Returns true if the
  // class-loader chain could be handled, false otherwise, i.e., a non-supported class-loader
  // was encountered while walking the parent chain (currently only BootClassLoader and
  // PathClassLoader are supported).
  bool FindClassInBaseDexClassLoader(Thread* self,
                                     const char* descriptor,
                                     size_t descriptor_length,
                                     size_t hash,
                                     Handle<mirror::ClassLoader> class_loader,
                                     /*out*/ ObjPtr<mirror::Class>* result)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_);

  bool FindClassInSharedLibraries(Thread* self,
                                  const char* descriptor,
                                  size_t descriptor_length,
                                  size_t hash,
                                  Handle<mirror::ClassLoader> class_loader,
                                  /*out*/ ObjPtr<mirror::Class>* result)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_);

  bool FindClassInSharedLibrariesHelper(Thread* self,
                                        const char* descriptor,
                                        size_t descriptor_length,
                                        size_t hash,
                                        Handle<mirror::ClassLoader> class_loader,
                                        ArtField* field,
                                        /*out*/ ObjPtr<mirror::Class>* result)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_);

  bool FindClassInSharedLibrariesAfter(Thread* self,
                                       const char* descriptor,
                                       size_t descriptor_length,
                                       size_t hash,
                                       Handle<mirror::ClassLoader> class_loader,
                                       /*out*/ ObjPtr<mirror::Class>* result)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_);

  // Finds the class in the classpath of the given class loader. It only searches the class loader
  // dex files and does not recurse into its parent.
  // The method checks that the provided class loader is either a PathClassLoader or a
  // DexClassLoader.
  // If the class is found the method updates `result`.
  // The method always returns true, to notify to the caller a
  // BaseDexClassLoader has a known lookup.
  bool FindClassInBaseDexClassLoaderClassPath(
          Thread* self,
          const char* descriptor,
          size_t descriptor_length,
          size_t hash,
          Handle<mirror::ClassLoader> class_loader,
          /*out*/ ObjPtr<mirror::Class>* result)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_);

  // Finds the class in the boot class loader.
  // If the class is found the method updates `result`.
  // The method always returns true, to notify to the caller the
  // boot class loader has a known lookup.
  bool FindClassInBootClassLoaderClassPath(Thread* self,
                                           const char* descriptor,
                                           size_t descriptor_length,
                                           size_t hash,
                                           /*out*/ ObjPtr<mirror::Class>* result)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_);

  // Implementation of LookupResolvedType() called when the type was not found in the dex cache.
  EXPORT ObjPtr<mirror::Class> DoLookupResolvedType(dex::TypeIndex type_idx,
                                                    ObjPtr<mirror::Class> referrer)
      REQUIRES_SHARED(Locks::mutator_lock_);
  EXPORT ObjPtr<mirror::Class> DoLookupResolvedType(dex::TypeIndex type_idx,
                                                    ObjPtr<mirror::DexCache> dex_cache,
                                                    ObjPtr<mirror::ClassLoader> class_loader)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Implementation of ResolveString() called when the string was not found in the dex cache.
  EXPORT ObjPtr<mirror::String> DoResolveString(dex::StringIndex string_idx,
                                                ObjPtr<mirror::DexCache> dex_cache)
      REQUIRES_SHARED(Locks::mutator_lock_);
  EXPORT ObjPtr<mirror::String> DoResolveString(dex::StringIndex string_idx,
                                                Handle<mirror::DexCache> dex_cache)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Implementation of LookupString() called when the string was not found in the dex cache.
  EXPORT ObjPtr<mirror::String> DoLookupString(dex::StringIndex string_idx,
                                               ObjPtr<mirror::DexCache> dex_cache)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Implementation of ResolveType() called when the type was not found in the dex cache. May be
  // used with ArtField*, ArtMethod* or ObjPtr<Class>.
  template <typename RefType>
  EXPORT ObjPtr<mirror::Class> DoResolveType(dex::TypeIndex type_idx, RefType referrer)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_, !Roles::uninterruptible_);
  EXPORT ObjPtr<mirror::Class> DoResolveType(dex::TypeIndex type_idx,
                                             Handle<mirror::DexCache> dex_cache,
                                             Handle<mirror::ClassLoader> class_loader)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_, !Roles::uninterruptible_);

  // Finds a class by its descriptor, returning NULL if it isn't wasn't loaded
  // by the given 'class_loader'. Uses the provided hash for the descriptor.
  ObjPtr<mirror::Class> LookupClass(Thread* self,
                                    std::string_view descriptor,
                                    size_t hash,
                                    ObjPtr<mirror::ClassLoader> class_loader)
      REQUIRES(!Locks::classlinker_classes_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);

  void RegisterDexFileLocked(const DexFile& dex_file,
                             ObjPtr<mirror::DexCache> dex_cache,
                             ObjPtr<mirror::ClassLoader> class_loader)
      REQUIRES(Locks::dex_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);
  const DexCacheData* FindDexCacheDataLocked(const DexFile& dex_file)
      REQUIRES_SHARED(Locks::dex_lock_);
  const DexCacheData* FindDexCacheDataLocked(const OatDexFile& oat_dex_file)
      REQUIRES_SHARED(Locks::dex_lock_);
  static ObjPtr<mirror::DexCache> DecodeDexCacheLocked(Thread* self, const DexCacheData* data)
      REQUIRES_SHARED(Locks::dex_lock_, Locks::mutator_lock_);
  bool IsSameClassLoader(ObjPtr<mirror::DexCache> dex_cache,
                         const DexCacheData* data,
                         ObjPtr<mirror::ClassLoader> class_loader)
      REQUIRES_SHARED(Locks::dex_lock_, Locks::mutator_lock_);

  bool InitializeDefaultInterfaceRecursive(Thread* self,
                                           Handle<mirror::Class> klass,
                                           bool can_run_clinit,
                                           bool can_init_parents)
      REQUIRES(!Locks::dex_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);
  bool WaitForInitializeClass(Handle<mirror::Class> klass,
                              Thread* self,
                              ObjectLock<mirror::Class>& lock);

  bool IsSameDescriptorInDifferentClassContexts(Thread* self,
                                                const char* descriptor,
                                                Handle<mirror::ClassLoader> class_loader1,
                                                Handle<mirror::ClassLoader> class_loader2)
      REQUIRES_SHARED(Locks::mutator_lock_);

  bool IsSameMethodSignatureInDifferentClassContexts(Thread* self,
                                                     ArtMethod* method,
                                                     ObjPtr<mirror::Class> klass1,
                                                     ObjPtr<mirror::Class> klass2)
      REQUIRES_SHARED(Locks::mutator_lock_);

  bool LinkSuperClass(Handle<mirror::Class> klass)
      REQUIRES_SHARED(Locks::mutator_lock_);

  bool LoadSuperAndInterfaces(Handle<mirror::Class> klass, const DexFile& dex_file)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_);

  bool LinkMethods(Thread* self,
                   Handle<mirror::Class> klass,
                   Handle<mirror::ObjectArray<mirror::Class>> interfaces,
                   bool* out_new_conflict,
                   ArtMethod** out_imt)
      REQUIRES_SHARED(Locks::mutator_lock_);

  ObjPtr<mirror::MethodHandle> ResolveMethodHandleForField(
      Thread* self,
      const dex::MethodHandleItem& method_handle,
      ArtMethod* referrer) REQUIRES_SHARED(Locks::mutator_lock_);

  ObjPtr<mirror::MethodHandle> ResolveMethodHandleForMethod(
      Thread* self,
      const dex::MethodHandleItem& method_handle,
      ArtMethod* referrer) REQUIRES_SHARED(Locks::mutator_lock_);

  bool LinkStaticFields(Thread* self, Handle<mirror::Class> klass, size_t* class_size)
      REQUIRES_SHARED(Locks::mutator_lock_);
  bool LinkInstanceFields(Thread* self, Handle<mirror::Class> klass)
      REQUIRES_SHARED(Locks::mutator_lock_);
  bool VerifyRecordClass(Handle<mirror::Class> klass, ObjPtr<mirror::Class> super)
      REQUIRES_SHARED(Locks::mutator_lock_);

  void CheckProxyConstructor(ArtMethod* constructor) const
      REQUIRES_SHARED(Locks::mutator_lock_);
  void CheckProxyMethod(ArtMethod* method, ArtMethod* prototype) const
      REQUIRES_SHARED(Locks::mutator_lock_);

  size_t GetDexCacheCount() REQUIRES_SHARED(Locks::mutator_lock_, Locks::dex_lock_) {
    return dex_caches_.size();
  }
  const std::unordered_map<const DexFile*, DexCacheData>& GetDexCachesData()
      REQUIRES_SHARED(Locks::mutator_lock_, Locks::dex_lock_) {
    return dex_caches_;
  }

  void CreateProxyConstructor(Handle<mirror::Class> klass, ArtMethod* out)
      REQUIRES_SHARED(Locks::mutator_lock_);
  void CreateProxyMethod(Handle<mirror::Class> klass, ArtMethod* prototype, ArtMethod* out)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Register a class loader and create its class table and allocator. Should not be called if
  // these are already created.
  void RegisterClassLoader(ObjPtr<mirror::ClassLoader> class_loader)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(Locks::classlinker_classes_lock_);

  // Insert a new class table if not found.
  ClassTable* InsertClassTableForClassLoader(ObjPtr<mirror::ClassLoader> class_loader)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(Locks::classlinker_classes_lock_);

  // EnsureResolved is called to make sure that a class in the class_table_ has been resolved
  // before returning it to the caller. Its the responsibility of the thread that placed the class
  // in the table to make it resolved. The thread doing resolution must notify on the class' lock
  // when resolution has occurred. This happens in mirror::Class::SetStatus. As resolution may
  // retire a class, the version of the class in the table is returned and this may differ from
  // the class passed in.
  ObjPtr<mirror::Class> EnsureResolved(Thread* self,
                                       std::string_view descriptor,
                                       ObjPtr<mirror::Class> klass)
      WARN_UNUSED
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::dex_lock_);

  void FixupTemporaryDeclaringClass(ObjPtr<mirror::Class> temp_class,
                                    ObjPtr<mirror::Class> new_class)
      REQUIRES_SHARED(Locks::mutator_lock_);

  void SetClassRoot(ClassRoot class_root, ObjPtr<mirror::Class> klass)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Return the quick generic JNI stub for testing.
  const void* GetRuntimeQuickGenericJniStub() const;

  bool CanWeInitializeClass(ObjPtr<mirror::Class> klass,
                            bool can_init_statics,
                            bool can_init_parents)
      REQUIRES_SHARED(Locks::mutator_lock_);

  void UpdateClassMethods(ObjPtr<mirror::Class> klass,
                          LengthPrefixedArray<ArtMethod>* new_methods)
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!Locks::classlinker_classes_lock_);

  // Check that c1 == FindSystemClass(self, descriptor). Abort with class dumps otherwise.
  void CheckSystemClass(Thread* self, Handle<mirror::Class> c1, const char* descriptor)
      REQUIRES(!Locks::dex_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);

  // Sets imt_ref appropriately for LinkInterfaceMethods.
  // If there is no method in the imt location of imt_ref it will store the given method there.
  // Otherwise it will set the conflict method which will figure out which method to use during
  // runtime.
  void SetIMTRef(ArtMethod* unimplemented_method,
                 ArtMethod* imt_conflict_method,
                 ArtMethod* current_method,
                 /*out*/bool* new_conflict,
                 /*out*/ArtMethod** imt_ref) REQUIRES_SHARED(Locks::mutator_lock_);

  void FillIMTFromIfTable(ObjPtr<mirror::IfTable> if_table,
                          ArtMethod* unimplemented_method,
                          ArtMethod* imt_conflict_method,
                          ObjPtr<mirror::Class> klass,
                          bool create_conflict_tables,
                          bool ignore_copied_methods,
                          /*out*/bool* new_conflict,
                          /*out*/ArtMethod** imt) REQUIRES_SHARED(Locks::mutator_lock_);

  ObjPtr<mirror::IfTable> GetArrayIfTable() REQUIRES_SHARED(Locks::mutator_lock_);

  bool OpenAndInitImageDexFiles(const gc::space::ImageSpace* space,
                                Handle<mirror::ClassLoader> class_loader,
                                std::vector<std::unique_ptr<const DexFile>>* out_dex_files,
                                std::string* error_msg) REQUIRES(!Locks::dex_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);

  bool AddImageSpace(gc::space::ImageSpace* space,
                     Handle<mirror::ClassLoader> class_loader,
                     ClassLoaderContext* context,
                     const std::vector<std::unique_ptr<const DexFile>>& dex_files,
                     std::string* error_msg) REQUIRES(!Locks::dex_lock_)
      REQUIRES_SHARED(Locks::mutator_lock_);

  std::vector<const DexFile*> boot_class_path_;
  std::vector<std::unique_ptr<const DexFile>> boot_dex_files_;

  // JNI weak globals and side data to allow dex caches to get unloaded. We lazily delete weak
  // globals when we register new dex files.
  std::unordered_map<const DexFile*, DexCacheData> dex_caches_ GUARDED_BY(Locks::dex_lock_);

  // This contains the class loaders which have class tables. It is populated by
  // InsertClassTableForClassLoader.
  std::list<ClassLoaderData> class_loaders_
      GUARDED_BY(Locks::classlinker_classes_lock_);

  // Boot class path table. Since the class loader for this is null.
  std::unique_ptr<ClassTable> boot_class_table_ GUARDED_BY(Locks::classlinker_classes_lock_);

  // New gc-roots, only used by CMS/CMC since the GC needs to mark these in the pause.
  std::vector<GcRoot<mirror::Object>> new_roots_ GUARDED_BY(Locks::classlinker_classes_lock_);

  // Boot image oat files with new .bss GC roots to be visited in the pause by CMS.
  std::vector<const OatFile*> new_bss_roots_boot_oat_files_
      GUARDED_BY(Locks::classlinker_classes_lock_);

  // Number of times we've searched dex caches for a class. After a certain number of misses we move
  // the classes into the class_table_ to avoid dex cache based searches.
  Atomic<uint32_t> failed_dex_cache_class_lookups_;

  // Well known mirror::Class roots.
  GcRoot<mirror::ObjectArray<mirror::Class>> class_roots_;

  // Method hashes for virtual methods from java.lang.Object used
  // to avoid recalculating them for each class we link.
  uint32_t object_virtual_method_hashes_[mirror::Object::kVTableLength];

  // A cache of the last FindArrayClass results. The cache serves to avoid creating array class
  // descriptors for the sake of performing FindClass.
  static constexpr size_t kFindArrayCacheSize = 16;
  std::atomic<GcRoot<mirror::Class>> find_array_class_cache_[kFindArrayCacheSize];
  size_t find_array_class_cache_next_victim_;

  bool init_done_;
  bool log_new_roots_ GUARDED_BY(Locks::classlinker_classes_lock_);

  InternTable* intern_table_;

  const bool fast_class_not_found_exceptions_;

  // Trampolines within the image the bounce to runtime entrypoints. Done so that there is a single
  // patch point within the image. TODO: make these proper relocations.
  const void* jni_dlsym_lookup_trampoline_;
  const void* jni_dlsym_lookup_critical_trampoline_;
  const void* quick_resolution_trampoline_;
  const void* quick_imt_conflict_trampoline_;
  const void* quick_generic_jni_trampoline_;
  const void* quick_to_interpreter_bridge_trampoline_;
  const void* nterp_trampoline_;

  // Image pointer size.
  PointerSize image_pointer_size_;

  // Classes to transition from ClassStatus::kInitialized to ClassStatus::kVisiblyInitialized.
  Mutex visibly_initialized_callback_lock_;
  std::unique_ptr<VisiblyInitializedCallback> visibly_initialized_callback_
      GUARDED_BY(visibly_initialized_callback_lock_);
  IntrusiveForwardList<VisiblyInitializedCallback> running_visibly_initialized_callbacks_
      GUARDED_BY(visibly_initialized_callback_lock_);

  // Whether to use `membarrier()` to make classes visibly initialized.
  bool visibly_initialize_classes_with_membarier_;

  // Registered native code for @CriticalNative methods of classes that are not visibly
  // initialized. These code pointers cannot be stored in ArtMethod as that would risk
  // skipping the class initialization check for direct calls from compiled code.
  Mutex critical_native_code_with_clinit_check_lock_;
  std::map<ArtMethod*, void*> critical_native_code_with_clinit_check_
      GUARDED_BY(critical_native_code_with_clinit_check_lock_);

  // Load unique JNI stubs from boot images. If the subsequently loaded native methods could find a
  // matching stub, then reuse it without JIT/AOT compilation.
  JniStubHashMap<const void*> boot_image_jni_stubs_;

  std::unique_ptr<ClassHierarchyAnalysis> cha_;

  class FindVirtualMethodHolderVisitor;

  friend class AppImageLoadingHelper;
  friend class ImageDumper;  // for DexLock
  friend struct linker::CompilationHelper;  // For Compile in ImageTest.
  friend class linker::ImageWriter;  // for GetClassRoots
  friend class JniCompilerTest;  // for GetRuntimeQuickGenericJniStub
  friend class JniInternalTest;  // for GetRuntimeQuickGenericJniStub
  friend class VerifyClassesFuzzerHelper;  // for FindDexCacheDataLocked.
  friend class VerifyClassesFuzzerCorpusTestHelper;  // for FindDexCacheDataLocked.
  friend class VMClassLoader;  // for LookupClass and FindClassInBaseDexClassLoader.
  ART_FRIEND_TEST(ClassLinkerTest, RegisterDexFileName);  // for DexLock, and RegisterDexFileLocked
  ART_FRIEND_TEST(mirror::DexCacheMethodHandlesTest, Open);  // for AllocDexCache
  ART_FRIEND_TEST(mirror::DexCacheTest, Open);  // for AllocDexCache
  DISALLOW_COPY_AND_ASSIGN(ClassLinker);
};

class ClassLoadCallback {
 public:
  virtual ~ClassLoadCallback() {}

  // Called immediately before beginning class-definition and immediately before returning from it.
  virtual void BeginDefineClass() REQUIRES_SHARED(Locks::mutator_lock_) {}
  virtual void EndDefineClass() REQUIRES_SHARED(Locks::mutator_lock_) {}

  // If set we will replace initial_class_def & initial_dex_file with the final versions. The
  // callback author is responsible for ensuring these are allocated in such a way they can be
  // cleaned up if another transformation occurs. Note that both must be set or null/unchanged on
  // return.
  // Note: the class may be temporary, in which case a following ClassPrepare event will be a
  //       different object. It is the listener's responsibility to handle this.
  // Note: This callback is rarely useful so a default implementation has been given that does
  //       nothing.
  virtual void ClassPreDefine([[maybe_unused]] const char* descriptor,
                              [[maybe_unused]] Handle<mirror::Class> klass,
                              [[maybe_unused]] Handle<mirror::ClassLoader> class_loader,
                              [[maybe_unused]] const DexFile& initial_dex_file,
                              [[maybe_unused]] const dex::ClassDef& initial_class_def,
                              [[maybe_unused]] /*out*/ DexFile const** final_dex_file,
                              [[maybe_unused]] /*out*/ dex::ClassDef const** final_class_def)
      REQUIRES_SHARED(Locks::mutator_lock_) {}

  // A class has been loaded.
  // Note: the class may be temporary, in which case a following ClassPrepare event will be a
  //       different object. It is the listener's responsibility to handle this.
  virtual void ClassLoad(Handle<mirror::Class> klass) REQUIRES_SHARED(Locks::mutator_lock_) = 0;

  // A class has been prepared, i.e., resolved. As the ClassLoad event might have been for a
  // temporary class, provide both the former and the current class.
  virtual void ClassPrepare(Handle<mirror::Class> temp_klass,
                            Handle<mirror::Class> klass) REQUIRES_SHARED(Locks::mutator_lock_) = 0;
};

}  // namespace art

#endif  // ART_RUNTIME_CLASS_LINKER_H_
