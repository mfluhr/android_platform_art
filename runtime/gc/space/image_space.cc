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

#include "image_space.h"

#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "android-base/logging.h"
#include "android-base/stringprintf.h"
#include "android-base/strings.h"
#include "android-base/unique_fd.h"
#include "arch/instruction_set.h"
#include "art_field-inl.h"
#include "art_method-inl.h"
#include "base/array_ref.h"
#include "base/bit_memory_region.h"
#include "base/callee_save_type.h"
#include "base/file_utils.h"
#include "base/globals.h"
#include "base/macros.h"
#include "base/memfd.h"
#include "base/os.h"
#include "base/pointer_size.h"
#include "base/stl_util.h"
#include "base/systrace.h"
#include "base/time_utils.h"
#include "base/utils.h"
#include "class_root-inl.h"
#include "dex/art_dex_file_loader.h"
#include "dex/dex_file_loader.h"
#include "exec_utils.h"
#include "gc/accounting/space_bitmap-inl.h"
#include "gc/task_processor.h"
#include "intern_table-inl.h"
#include "mirror/class-inl.h"
#include "mirror/executable-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object-refvisitor-inl.h"
#include "mirror/var_handle.h"
#include "oat/image-inl.h"
#include "oat/image.h"
#include "oat/oat.h"
#include "oat/oat_file.h"
#include "profile/profile_compilation_info.h"
#include "runtime.h"
#include "space-inl.h"

namespace art HIDDEN {
namespace gc {
namespace space {

namespace {

using ::android::base::Join;
using ::android::base::StringAppendF;
using ::android::base::StringPrintf;

// We do not allow the boot image and extensions to take more than 1GiB. They are
// supposed to be much smaller and allocating more that this would likely fail anyway.
static constexpr size_t kMaxTotalImageReservationSize = 1 * GB;

}  // namespace

Atomic<uint32_t> ImageSpace::bitmap_index_(0);

ImageSpace::ImageSpace(const std::string& image_filename,
                       const char* image_location,
                       const std::vector<std::string>& profile_files,
                       MemMap&& mem_map,
                       accounting::ContinuousSpaceBitmap&& live_bitmap,
                       uint8_t* end)
    : MemMapSpace(image_filename,
                  std::move(mem_map),
                  mem_map.Begin(),
                  end,
                  end,
                  kGcRetentionPolicyNeverCollect),
      live_bitmap_(std::move(live_bitmap)),
      oat_file_non_owned_(nullptr),
      image_location_(image_location),
      profile_files_(profile_files) {
  DCHECK(live_bitmap_.IsValid());
}

static int32_t ChooseRelocationOffsetDelta(int32_t min_delta, int32_t max_delta) {
  CHECK_ALIGNED(min_delta, kElfSegmentAlignment);
  CHECK_ALIGNED(max_delta, kElfSegmentAlignment);
  CHECK_LT(min_delta, max_delta);

  int32_t r = GetRandomNumber<int32_t>(min_delta, max_delta);
  if (r % 2 == 0) {
    r = RoundUp(r, kElfSegmentAlignment);
  } else {
    r = RoundDown(r, kElfSegmentAlignment);
  }
  CHECK_LE(min_delta, r);
  CHECK_GE(max_delta, r);
  CHECK_ALIGNED(r, kElfSegmentAlignment);
  return r;
}

static int32_t ChooseRelocationOffsetDelta() {
  return ChooseRelocationOffsetDelta(ART_BASE_ADDRESS_MIN_DELTA, ART_BASE_ADDRESS_MAX_DELTA);
}

static bool FindImageFilenameImpl(const char* image_location,
                                  const InstructionSet image_isa,
                                  bool* has_system,
                                  std::string* system_filename) {
  *has_system = false;

  // image_location = /system/framework/boot.art
  // system_image_location = /system/framework/<image_isa>/boot.art
  std::string system_image_filename(GetSystemImageFilename(image_location, image_isa));
  if (OS::FileExists(system_image_filename.c_str())) {
    *system_filename = system_image_filename;
    *has_system = true;
  }

  return *has_system;
}

bool ImageSpace::FindImageFilename(const char* image_location,
                                   const InstructionSet image_isa,
                                   std::string* system_filename,
                                   bool* has_system) {
  return FindImageFilenameImpl(image_location,
                               image_isa,
                               has_system,
                               system_filename);
}

static bool ReadSpecificImageHeader(File* image_file,
                                    const char* file_description,
                                    /*out*/ImageHeader* image_header,
                                    /*out*/std::string* error_msg) {
  if (!image_file->PreadFully(image_header, sizeof(ImageHeader), /*offset=*/ 0)) {
    *error_msg = StringPrintf("Unable to read image header from \"%s\"", file_description);
    return false;
  }
  if (!image_header->IsValid()) {
    *error_msg = StringPrintf("Image header from \"%s\" is invalid", file_description);
    return false;
  }
  return true;
}

static bool ReadSpecificImageHeader(const char* filename,
                                    /*out*/ImageHeader* image_header,
                                    /*out*/std::string* error_msg) {
  std::unique_ptr<File> image_file(OS::OpenFileForReading(filename));
  if (image_file.get() == nullptr) {
    *error_msg = StringPrintf("Unable to open file \"%s\" for reading image header", filename);
    return false;
  }
  return ReadSpecificImageHeader(image_file.get(), filename, image_header, error_msg);
}

static std::unique_ptr<ImageHeader> ReadSpecificImageHeader(const char* filename,
                                                            std::string* error_msg) {
  std::unique_ptr<ImageHeader> hdr(new ImageHeader);
  if (!ReadSpecificImageHeader(filename, hdr.get(), error_msg)) {
    return nullptr;
  }
  return hdr;
}

void ImageSpace::VerifyImageAllocations() {
  uint8_t* current = Begin() + RoundUp(sizeof(ImageHeader), kObjectAlignment);
  while (current < End()) {
    CHECK_ALIGNED(current, kObjectAlignment);
    auto* obj = reinterpret_cast<mirror::Object*>(current);
    CHECK(obj->GetClass() != nullptr) << "Image object at address " << obj << " has null class";
    CHECK(live_bitmap_.Test(obj)) << obj->PrettyTypeOf();
    if (kUseBakerReadBarrier) {
      obj->AssertReadBarrierState();
    }
    current += RoundUp(obj->SizeOf(), kObjectAlignment);
  }
}

// Helper class for relocating from one range of memory to another.
class RelocationRange {
 public:
  RelocationRange(const RelocationRange&) = default;
  RelocationRange(uintptr_t source, uintptr_t dest, uintptr_t length)
      : source_(source),
        dest_(dest),
        length_(length) {}

  bool InSource(uintptr_t address) const {
    return address - source_ < length_;
  }

  bool InDest(const void* dest) const {
    return InDest(reinterpret_cast<uintptr_t>(dest));
  }

  bool InDest(uintptr_t address) const {
    return address - dest_ < length_;
  }

  // Translate a source address to the destination space.
  uintptr_t ToDest(uintptr_t address) const {
    DCHECK(InSource(address));
    return address + Delta();
  }

  template <typename T>
  T* ToDest(T* src) const {
    return reinterpret_cast<T*>(ToDest(reinterpret_cast<uintptr_t>(src)));
  }

  // Returns the delta between the dest from the source.
  uintptr_t Delta() const {
    return dest_ - source_;
  }

  uintptr_t Source() const {
    return source_;
  }

  uintptr_t Dest() const {
    return dest_;
  }

  uintptr_t Length() const {
    return length_;
  }

 private:
  const uintptr_t source_;
  const uintptr_t dest_;
  const uintptr_t length_;
};

std::ostream& operator<<(std::ostream& os, const RelocationRange& reloc) {
  return os << "(" << reinterpret_cast<const void*>(reloc.Source()) << "-"
            << reinterpret_cast<const void*>(reloc.Source() + reloc.Length()) << ")->("
            << reinterpret_cast<const void*>(reloc.Dest()) << "-"
            << reinterpret_cast<const void*>(reloc.Dest() + reloc.Length()) << ")";
}

template <PointerSize kPointerSize, typename HeapVisitor, typename NativeVisitor>
class ImageSpace::PatchObjectVisitor final {
 public:
  explicit PatchObjectVisitor(HeapVisitor heap_visitor, NativeVisitor native_visitor)
      : heap_visitor_(heap_visitor), native_visitor_(native_visitor) {}

  void VisitClass(ObjPtr<mirror::Class> klass, ObjPtr<mirror::Class> class_class)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    // A mirror::Class object consists of
    //  - instance fields inherited from j.l.Object,
    //  - instance fields inherited from j.l.Class,
    //  - embedded tables (vtable, interface method table),
    //  - static fields of the class itself.
    // The reference fields are at the start of each field section (this is how the
    // ClassLinker orders fields; except when that would create a gap between superclass
    // fields and the first reference of the subclass due to alignment, it can be filled
    // with smaller fields - but that's not the case for j.l.Object and j.l.Class).

    DCHECK_ALIGNED(klass.Ptr(), kObjectAlignment);
    static_assert(IsAligned<kHeapReferenceSize>(kObjectAlignment), "Object alignment check.");
    // First, patch the `klass->klass_`, known to be a reference to the j.l.Class.class.
    // This should be the only reference field in j.l.Object and we assert that below.
    DCHECK_EQ(class_class,
              heap_visitor_(klass->GetClass<kVerifyNone, kWithoutReadBarrier>()));
    klass->SetFieldObjectWithoutWriteBarrier<
        /*kTransactionActive=*/ false,
        /*kCheckTransaction=*/ true,
        kVerifyNone>(mirror::Object::ClassOffset(), class_class);
    // Then patch the reference instance fields described by j.l.Class.class.
    // Use the sizeof(Object) to determine where these reference fields start;
    // this is the same as `class_class->GetFirstReferenceInstanceFieldOffset()`
    // after patching but the j.l.Class may not have been patched yet.
    size_t num_reference_instance_fields = class_class->NumReferenceInstanceFields<kVerifyNone>();
    DCHECK_NE(num_reference_instance_fields, 0u);
    static_assert(IsAligned<kHeapReferenceSize>(sizeof(mirror::Object)), "Size alignment check.");
    MemberOffset instance_field_offset(sizeof(mirror::Object));
    for (size_t i = 0; i != num_reference_instance_fields; ++i) {
      PatchReferenceField(klass, instance_field_offset);
      static_assert(sizeof(mirror::HeapReference<mirror::Object>) == kHeapReferenceSize,
                    "Heap reference sizes equality check.");
      instance_field_offset =
          MemberOffset(instance_field_offset.Uint32Value() + kHeapReferenceSize);
    }
    // Now that we have patched the `super_class_`, if this is the j.l.Class.class,
    // we can get a reference to j.l.Object.class and assert that it has only one
    // reference instance field (the `klass_` patched above).
    if (kIsDebugBuild && klass == class_class) {
      ObjPtr<mirror::Class> object_class =
          klass->GetSuperClass<kVerifyNone, kWithoutReadBarrier>();
      CHECK_EQ(object_class->NumReferenceInstanceFields<kVerifyNone>(), 1u);
    }
    // Then patch static fields.
    size_t num_reference_static_fields = klass->NumReferenceStaticFields<kVerifyNone>();
    if (num_reference_static_fields != 0u) {
      MemberOffset static_field_offset =
          klass->GetFirstReferenceStaticFieldOffset<kVerifyNone>(kPointerSize);
      for (size_t i = 0; i != num_reference_static_fields; ++i) {
        PatchReferenceField(klass, static_field_offset);
        static_assert(sizeof(mirror::HeapReference<mirror::Object>) == kHeapReferenceSize,
                      "Heap reference sizes equality check.");
        static_field_offset =
            MemberOffset(static_field_offset.Uint32Value() + kHeapReferenceSize);
      }
    }
    // Then patch native pointers.
    klass->FixupNativePointers<kVerifyNone>(klass.Ptr(), kPointerSize, *this);
  }

  template <typename T>
  T* operator()(T* ptr, [[maybe_unused]] void** dest_addr) const {
    return (ptr != nullptr) ? native_visitor_(ptr) : nullptr;
  }

  void VisitPointerArray(ObjPtr<mirror::PointerArray> pointer_array)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    // Fully patch the pointer array, including the `klass_` field.
    PatchReferenceField</*kMayBeNull=*/ false>(pointer_array, mirror::Object::ClassOffset());

    int32_t length = pointer_array->GetLength<kVerifyNone>();
    for (int32_t i = 0; i != length; ++i) {
      ArtMethod** method_entry = reinterpret_cast<ArtMethod**>(
          pointer_array->ElementAddress<kVerifyNone>(i, kPointerSize));
      PatchNativePointer</*kMayBeNull=*/ false>(method_entry);
    }
  }

  void VisitObject(mirror::Object* object) REQUIRES_SHARED(Locks::mutator_lock_) {
    // Visit all reference fields.
    object->VisitReferences</*kVisitNativeRoots=*/ false,
                            kVerifyNone,
                            kWithoutReadBarrier>(*this, *this);
    // This function should not be called for classes.
    DCHECK(!object->IsClass<kVerifyNone>());
  }

  // Visitor for VisitReferences().
  ALWAYS_INLINE void operator()(ObjPtr<mirror::Object> object,
                                MemberOffset field_offset,
                                bool is_static)
      const REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK(!is_static);
    PatchReferenceField(object, field_offset);
  }
  // Visitor for VisitReferences(), java.lang.ref.Reference case.
  ALWAYS_INLINE void operator()(ObjPtr<mirror::Class> klass, ObjPtr<mirror::Reference> ref) const
      REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK(klass->IsTypeOfReferenceClass());
    this->operator()(ref, mirror::Reference::ReferentOffset(), /*is_static=*/ false);
  }
  // Ignore class native roots; not called from VisitReferences() for kVisitNativeRoots == false.
  void VisitRootIfNonNull(
      [[maybe_unused]] mirror::CompressedReference<mirror::Object>* root) const {}
  void VisitRoot([[maybe_unused]] mirror::CompressedReference<mirror::Object>* root) const {}

  template <typename T> void VisitNativeDexCacheArray(mirror::NativeArray<T>* array)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (array == nullptr) {
      return;
    }
    DCHECK_ALIGNED(array, static_cast<size_t>(kPointerSize));
    uint32_t size = (kPointerSize == PointerSize::k32)
        ? reinterpret_cast<uint32_t*>(array)[-1]
        : dchecked_integral_cast<uint32_t>(reinterpret_cast<uint64_t*>(array)[-1]);
    for (uint32_t i = 0; i < size; ++i) {
      PatchNativePointer(array->GetPtrEntryPtrSize(i, kPointerSize));
    }
  }

  template <typename T> void VisitGcRootDexCacheArray(mirror::GcRootArray<T>* array)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (array == nullptr) {
      return;
    }
    DCHECK_ALIGNED(array, sizeof(GcRoot<T>));
    static_assert(sizeof(GcRoot<T>) == sizeof(uint32_t));
    uint32_t size = reinterpret_cast<uint32_t*>(array)[-1];
    for (uint32_t i = 0; i < size; ++i) {
      PatchGcRoot(array->GetGcRootAddress(i));
    }
  }

  void VisitDexCacheArrays(ObjPtr<mirror::DexCache> dex_cache)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    mirror::NativeArray<ArtMethod>* old_resolved_methods = dex_cache->GetResolvedMethodsArray();
    if (old_resolved_methods != nullptr) {
      mirror::NativeArray<ArtMethod>* resolved_methods = native_visitor_(old_resolved_methods);
      dex_cache->SetResolvedMethodsArray(resolved_methods);
      VisitNativeDexCacheArray(resolved_methods);
    }

    mirror::NativeArray<ArtField>* old_resolved_fields = dex_cache->GetResolvedFieldsArray();
    if (old_resolved_fields != nullptr) {
      mirror::NativeArray<ArtField>* resolved_fields = native_visitor_(old_resolved_fields);
      dex_cache->SetResolvedFieldsArray(resolved_fields);
      VisitNativeDexCacheArray(resolved_fields);
    }

    mirror::GcRootArray<mirror::String>* old_strings = dex_cache->GetStringsArray();
    if (old_strings != nullptr) {
      mirror::GcRootArray<mirror::String>* strings = native_visitor_(old_strings);
      dex_cache->SetStringsArray(strings);
      VisitGcRootDexCacheArray(strings);
    }

    mirror::GcRootArray<mirror::Class>* old_types = dex_cache->GetResolvedTypesArray();
    if (old_types != nullptr) {
      mirror::GcRootArray<mirror::Class>* types = native_visitor_(old_types);
      dex_cache->SetResolvedTypesArray(types);
      VisitGcRootDexCacheArray(types);
    }
  }

  template <bool kMayBeNull = true, typename T>
  ALWAYS_INLINE void PatchGcRoot(/*inout*/GcRoot<T>* root) const
      REQUIRES_SHARED(Locks::mutator_lock_) {
    static_assert(sizeof(GcRoot<mirror::Class*>) == sizeof(uint32_t), "GcRoot size check");
    T* old_value = root->template Read<kWithoutReadBarrier>();
    DCHECK(kMayBeNull || old_value != nullptr);
    if (!kMayBeNull || old_value != nullptr) {
      *root = GcRoot<T>(heap_visitor_(old_value));
    }
  }

  template <bool kMayBeNull = true, typename T>
  ALWAYS_INLINE void PatchNativePointer(/*inout*/T** entry) const {
    if (kPointerSize == PointerSize::k64) {
      uint64_t* raw_entry = reinterpret_cast<uint64_t*>(entry);
      T* old_value = reinterpret_cast64<T*>(*raw_entry);
      DCHECK(kMayBeNull || old_value != nullptr);
      if (!kMayBeNull || old_value != nullptr) {
        T* new_value = native_visitor_(old_value);
        *raw_entry = reinterpret_cast64<uint64_t>(new_value);
      }
    } else {
      uint32_t* raw_entry = reinterpret_cast<uint32_t*>(entry);
      T* old_value = reinterpret_cast32<T*>(*raw_entry);
      DCHECK(kMayBeNull || old_value != nullptr);
      if (!kMayBeNull || old_value != nullptr) {
        T* new_value = native_visitor_(old_value);
        *raw_entry = reinterpret_cast32<uint32_t>(new_value);
      }
    }
  }

  template <bool kMayBeNull = true>
  ALWAYS_INLINE void PatchReferenceField(ObjPtr<mirror::Object> object, MemberOffset offset) const
      REQUIRES_SHARED(Locks::mutator_lock_) {
    ObjPtr<mirror::Object> old_value =
        object->GetFieldObject<mirror::Object, kVerifyNone, kWithoutReadBarrier>(offset);
    DCHECK(kMayBeNull || old_value != nullptr);
    if (!kMayBeNull || old_value != nullptr) {
      ObjPtr<mirror::Object> new_value = heap_visitor_(old_value.Ptr());
      object->SetFieldObjectWithoutWriteBarrier</*kTransactionActive=*/ false,
                                                /*kCheckTransaction=*/ true,
                                                kVerifyNone>(offset, new_value);
    }
  }

 private:
  // Heap objects visitor.
  HeapVisitor heap_visitor_;

  // Native objects visitor.
  NativeVisitor native_visitor_;
};

template <typename ReferenceVisitor>
class ImageSpace::ClassTableVisitor final {
 public:
  explicit ClassTableVisitor(const ReferenceVisitor& reference_visitor)
      : reference_visitor_(reference_visitor) {}

  void VisitRoot(mirror::CompressedReference<mirror::Object>* root) const
      REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK(root->AsMirrorPtr() != nullptr);
    root->Assign(reference_visitor_(root->AsMirrorPtr()));
  }

 private:
  ReferenceVisitor reference_visitor_;
};

class ImageSpace::RemapInternedStringsVisitor {
 public:
  explicit RemapInternedStringsVisitor(
      const SafeMap<mirror::String*, mirror::String*>& intern_remap)
      REQUIRES_SHARED(Locks::mutator_lock_)
      : intern_remap_(intern_remap),
        string_class_(GetStringClass()) {}

  // Visitor for VisitReferences().
  ALWAYS_INLINE void operator()(ObjPtr<mirror::Object> object,
                                MemberOffset field_offset,
                                [[maybe_unused]] bool is_static) const
      REQUIRES_SHARED(Locks::mutator_lock_) {
    ObjPtr<mirror::Object> old_value =
        object->GetFieldObject<mirror::Object, kVerifyNone, kWithoutReadBarrier>(field_offset);
    if (old_value != nullptr &&
        old_value->GetClass<kVerifyNone, kWithoutReadBarrier>() == string_class_) {
      auto it = intern_remap_.find(old_value->AsString().Ptr());
      if (it != intern_remap_.end()) {
        mirror::String* new_value = it->second;
        object->SetFieldObjectWithoutWriteBarrier</*kTransactionActive=*/ false,
                                                  /*kCheckTransaction=*/ true,
                                                  kVerifyNone>(field_offset, new_value);
      }
    }
  }
  // Visitor for VisitReferences(), java.lang.ref.Reference case.
  ALWAYS_INLINE void operator()(ObjPtr<mirror::Class> klass, ObjPtr<mirror::Reference> ref) const
      REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK(klass->IsTypeOfReferenceClass());
    this->operator()(ref, mirror::Reference::ReferentOffset(), /*is_static=*/ false);
  }
  // Ignore class native roots; not called from VisitReferences() for kVisitNativeRoots == false.
  void VisitRootIfNonNull(
      [[maybe_unused]] mirror::CompressedReference<mirror::Object>* root) const {}
  void VisitRoot([[maybe_unused]] mirror::CompressedReference<mirror::Object>* root) const {}

 private:
  mirror::Class* GetStringClass() REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK(!intern_remap_.empty());
    return intern_remap_.begin()->first->GetClass<kVerifyNone, kWithoutReadBarrier>();
  }

  const SafeMap<mirror::String*, mirror::String*>& intern_remap_;
  mirror::Class* const string_class_;
};

// Helper class encapsulating loading, so we can access private ImageSpace members (this is a
// nested class), but not declare functions in the header.
class ImageSpace::Loader {
 public:
  static std::unique_ptr<ImageSpace> InitAppImage(const char* image_filename,
                                                  const char* image_location,
                                                  const OatFile* oat_file,
                                                  ArrayRef<ImageSpace* const> boot_image_spaces,
                                                  /*out*/std::string* error_msg)
        REQUIRES(!Locks::mutator_lock_) {
    TimingLogger logger(__PRETTY_FUNCTION__, /*precise=*/ true, VLOG_IS_ON(image));

    std::unique_ptr<ImageSpace> space = Init(image_filename,
                                             image_location,
                                             &logger,
                                             /*image_reservation=*/ nullptr,
                                             error_msg);
    if (space != nullptr) {
      space->oat_file_non_owned_ = oat_file;
      const ImageHeader& image_header = space->GetImageHeader();

      // Check the oat file checksum.
      const uint32_t oat_checksum = oat_file->GetOatHeader().GetChecksum();
      const uint32_t image_oat_checksum = image_header.GetOatChecksum();
      // Note image_oat_checksum is 0 for images generated by the runtime.
      if (image_oat_checksum != 0u && oat_checksum != image_oat_checksum) {
        *error_msg = StringPrintf("Oat checksum 0x%x does not match the image one 0x%x in image %s",
                                  oat_checksum,
                                  image_oat_checksum,
                                  image_filename);
        return nullptr;
      }
      size_t boot_image_space_dependencies;
      if (!ValidateBootImageChecksum(image_filename,
                                     image_header,
                                     oat_file,
                                     boot_image_spaces,
                                     &boot_image_space_dependencies,
                                     error_msg)) {
        DCHECK(!error_msg->empty());
        return nullptr;
      }

      uint32_t expected_reservation_size = RoundUp(image_header.GetImageSize(),
          kElfSegmentAlignment);
      if (!CheckImageReservationSize(*space, expected_reservation_size, error_msg) ||
          !CheckImageComponentCount(*space, /*expected_component_count=*/ 1u, error_msg)) {
        return nullptr;
      }

      {
        TimingLogger::ScopedTiming timing("RelocateImage", &logger);
        const PointerSize pointer_size = image_header.GetPointerSize();
        uint32_t boot_image_begin =
            reinterpret_cast32<uint32_t>(boot_image_spaces.front()->Begin());
        bool result;
        if (pointer_size == PointerSize::k64) {
          result = RelocateInPlace<PointerSize::k64>(boot_image_begin,
                                                     space->GetMemMap()->Begin(),
                                                     space->GetLiveBitmap(),
                                                     oat_file,
                                                     error_msg);
        } else {
          result = RelocateInPlace<PointerSize::k32>(boot_image_begin,
                                                     space->GetMemMap()->Begin(),
                                                     space->GetLiveBitmap(),
                                                     oat_file,
                                                     error_msg);
        }
        if (!result) {
          return nullptr;
        }
      }

      DCHECK_LE(boot_image_space_dependencies, boot_image_spaces.size());
      if (boot_image_space_dependencies != boot_image_spaces.size()) {
        TimingLogger::ScopedTiming timing("DeduplicateInternedStrings", &logger);
        // There shall be no duplicates with boot image spaces this app image depends on.
        ArrayRef<ImageSpace* const> old_spaces =
            boot_image_spaces.SubArray(/*pos=*/ boot_image_space_dependencies);
        SafeMap<mirror::String*, mirror::String*> intern_remap;
        ScopedObjectAccess soa(Thread::Current());
        RemoveInternTableDuplicates(old_spaces, space.get(), &intern_remap);
        if (!intern_remap.empty()) {
          RemapInternedStringDuplicates(intern_remap, space.get());
        }
      }

      const ImageHeader& primary_header = boot_image_spaces.front()->GetImageHeader();
      static_assert(static_cast<size_t>(ImageHeader::kResolutionMethod) == 0u);
      for (size_t i = 0u; i != static_cast<size_t>(ImageHeader::kImageMethodsCount); ++i) {
        ImageHeader::ImageMethod method = static_cast<ImageHeader::ImageMethod>(i);
        CHECK_EQ(primary_header.GetImageMethod(method), image_header.GetImageMethod(method))
            << method;
      }

      VLOG(image) << "ImageSpace::Loader::InitAppImage exiting " << *space.get();
    }
    if (VLOG_IS_ON(image)) {
      logger.Dump(LOG_STREAM(INFO));
    }
    return space;
  }

  static std::unique_ptr<ImageSpace> Init(const char* image_filename,
                                          const char* image_location,
                                          TimingLogger* logger,
                                          /*inout*/MemMap* image_reservation,
                                          /*out*/std::string* error_msg) {
    CHECK(image_filename != nullptr);
    CHECK(image_location != nullptr);

    FileWithRange file_with_range;
    {
      TimingLogger::ScopedTiming timing("OpenImageFile", logger);
      // Most likely, the image is compressed and doesn't really need alignment. We enforce page
      // size alignment just in case the image is uncompressed.
      file_with_range = OS::OpenFileDirectlyOrFromZip(
          image_filename, OatFile::kZipSeparator, /*alignment=*/MemMap::GetPageSize(), error_msg);
      if (file_with_range.file == nullptr) {
        return nullptr;
      }
    }
    return Init(file_with_range.file.get(),
                file_with_range.start,
                file_with_range.length,
                image_filename,
                image_location,
                /*profile_files=*/{},
                /*allow_direct_mapping=*/true,
                logger,
                image_reservation,
                error_msg);
  }

  static std::unique_ptr<ImageSpace> Init(File* file,
                                          off_t start,
                                          size_t image_file_size,
                                          const char* image_filename,
                                          const char* image_location,
                                          const std::vector<std::string>& profile_files,
                                          bool allow_direct_mapping,
                                          TimingLogger* logger,
                                          /*inout*/ MemMap* image_reservation,
                                          /*out*/ std::string* error_msg) {
    CHECK(image_filename != nullptr);
    CHECK(image_location != nullptr);

    VLOG(image) << "ImageSpace::Init entering image_filename=" << image_filename;

    ImageHeader image_header;
    {
      TimingLogger::ScopedTiming timing("ReadImageHeader", logger);
      bool success = file->PreadFully(&image_header, sizeof(image_header), start);
      if (!success || !image_header.IsValid()) {
        *error_msg = StringPrintf("Invalid image header in '%s'", image_filename);
        return nullptr;
      }
    }
    // Check that the file is larger or equal to the header size + data size.
    if (image_file_size < sizeof(ImageHeader) + image_header.GetDataSize()) {
      *error_msg = StringPrintf("Image file truncated: %zu vs. %" PRIu64 ".",
                                image_file_size,
                                sizeof(ImageHeader) + image_header.GetDataSize());
      return nullptr;
    }

    if (VLOG_IS_ON(startup)) {
      LOG(INFO) << "Dumping image sections";
      for (size_t i = 0; i < ImageHeader::kSectionCount; ++i) {
        const auto section_idx = static_cast<ImageHeader::ImageSections>(i);
        auto& section = image_header.GetImageSection(section_idx);
        LOG(INFO) << section_idx << " start="
            << reinterpret_cast<void*>(image_header.GetImageBegin() + section.Offset()) << " "
            << section;
      }
    }

    const auto& bitmap_section = image_header.GetImageBitmapSection();
    // The location we want to map from is the first aligned page after the end of the stored
    // (possibly compressed) data.
    const size_t image_bitmap_offset =
        RoundUp(sizeof(ImageHeader) + image_header.GetDataSize(), kElfSegmentAlignment);
    const size_t end_of_bitmap = image_bitmap_offset + bitmap_section.Size();
    if (end_of_bitmap != image_file_size) {
      *error_msg = StringPrintf("Image file size does not equal end of bitmap: size=%zu vs. %zu.",
                                image_file_size,
                                end_of_bitmap);
      return nullptr;
    }

    // GetImageBegin is the preferred address to map the image. If we manage to map the
    // image at the image begin, the amount of fixup work required is minimized.
    // If it is pic we will retry with error_msg for the2 failure case. Pass a null error_msg to
    // avoid reading proc maps for a mapping failure and slowing everything down.
    // For the boot image, we have already reserved the memory and we load the image
    // into the `image_reservation`.
    MemMap map = LoadImageFile(image_filename,
                               image_location,
                               image_header,
                               file->Fd(),
                               start,
                               allow_direct_mapping,
                               logger,
                               image_reservation,
                               error_msg);
    if (!map.IsValid()) {
      DCHECK(!error_msg->empty());
      return nullptr;
    }
    DCHECK_EQ(0, memcmp(&image_header, map.Begin(), sizeof(ImageHeader)));

    MemMap image_bitmap_map = MemMap::MapFile(bitmap_section.Size(),
                                              PROT_READ,
                                              MAP_PRIVATE,
                                              file->Fd(),
                                              start + image_bitmap_offset,
                                              /*low_4gb=*/false,
                                              image_filename,
                                              error_msg);
    if (!image_bitmap_map.IsValid()) {
      *error_msg = StringPrintf("Failed to map image bitmap: %s", error_msg->c_str());
      return nullptr;
    }
    const uint32_t bitmap_index = ImageSpace::bitmap_index_.fetch_add(1);
    std::string bitmap_name(StringPrintf("imagespace %s live-bitmap %u",
                                         image_filename,
                                         bitmap_index));
    // Bitmap only needs to cover until the end of the mirror objects section.
    const ImageSection& image_objects = image_header.GetObjectsSection();
    // We only want the mirror object, not the ArtFields and ArtMethods.
    uint8_t* const image_end = map.Begin() + image_objects.End();
    accounting::ContinuousSpaceBitmap bitmap;
    {
      TimingLogger::ScopedTiming timing("CreateImageBitmap", logger);
      bitmap = accounting::ContinuousSpaceBitmap::CreateFromMemMap(
          bitmap_name,
          std::move(image_bitmap_map),
          reinterpret_cast<uint8_t*>(map.Begin()),
          // Make sure the bitmap is aligned to card size instead of just bitmap word size.
          RoundUp(image_objects.End(), gc::accounting::CardTable::kCardSize));
      if (!bitmap.IsValid()) {
        *error_msg = StringPrintf("Could not create bitmap '%s'", bitmap_name.c_str());
        return nullptr;
      }
    }
    // We only want the mirror object, not the ArtFields and ArtMethods.
    std::unique_ptr<ImageSpace> space(new ImageSpace(image_filename,
                                                     image_location,
                                                     profile_files,
                                                     std::move(map),
                                                     std::move(bitmap),
                                                     image_end));
    return space;
  }

  static bool CheckImageComponentCount(const ImageSpace& space,
                                       uint32_t expected_component_count,
                                       /*out*/std::string* error_msg) {
    const ImageHeader& header = space.GetImageHeader();
    if (header.GetComponentCount() != expected_component_count) {
      *error_msg = StringPrintf("Unexpected component count in %s, received %u, expected %u",
                                space.GetImageFilename().c_str(),
                                header.GetComponentCount(),
                                expected_component_count);
      return false;
    }
    return true;
  }

  static bool CheckImageReservationSize(const ImageSpace& space,
                                        uint32_t expected_reservation_size,
                                        /*out*/std::string* error_msg) {
    const ImageHeader& header = space.GetImageHeader();
    if (header.GetImageReservationSize() != expected_reservation_size) {
      *error_msg = StringPrintf("Unexpected reservation size in %s, received %u, expected %u",
                                space.GetImageFilename().c_str(),
                                header.GetImageReservationSize(),
                                expected_reservation_size);
      return false;
    }
    return true;
  }

  template <typename Container>
  static void RemoveInternTableDuplicates(
      const Container& old_spaces,
      /*inout*/ImageSpace* new_space,
      /*inout*/SafeMap<mirror::String*, mirror::String*>* intern_remap)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    const ImageSection& new_interns = new_space->GetImageHeader().GetInternedStringsSection();
    if (new_interns.Size() != 0u) {
      const uint8_t* new_data = new_space->Begin() + new_interns.Offset();
      size_t new_read_count;
      InternTable::UnorderedSet new_set(new_data, /*make_copy_of_data=*/ false, &new_read_count);
      for (const auto& old_space : old_spaces) {
        const ImageSection& old_interns = old_space->GetImageHeader().GetInternedStringsSection();
        if (old_interns.Size() != 0u) {
          const uint8_t* old_data = old_space->Begin() + old_interns.Offset();
          size_t old_read_count;
          InternTable::UnorderedSet old_set(
              old_data, /*make_copy_of_data=*/ false, &old_read_count);
          RemoveDuplicates(old_set, &new_set, intern_remap);
        }
      }
    }
  }

  static void RemapInternedStringDuplicates(
      const SafeMap<mirror::String*, mirror::String*>& intern_remap,
      ImageSpace* new_space) REQUIRES_SHARED(Locks::mutator_lock_) {
    RemapInternedStringsVisitor visitor(intern_remap);
    static_assert(IsAligned<kObjectAlignment>(sizeof(ImageHeader)), "Header alignment check");
    uint32_t objects_end = new_space->GetImageHeader().GetObjectsSection().Size();
    DCHECK_ALIGNED(objects_end, kObjectAlignment);
    for (uint32_t pos = sizeof(ImageHeader); pos != objects_end; ) {
      mirror::Object* object = reinterpret_cast<mirror::Object*>(new_space->Begin() + pos);
      object->VisitReferences</*kVisitNativeRoots=*/ false,
                              kVerifyNone,
                              kWithoutReadBarrier>(visitor, visitor);
      pos += RoundUp(object->SizeOf<kVerifyNone>(), kObjectAlignment);
    }
  }

 private:
  // Remove duplicates found in the `old_set` from the `new_set`.
  // Record the removed Strings for remapping. No read barriers are needed as the
  // tables are either just being loaded and not yet a part of the heap, or boot
  // image intern tables with non-moveable Strings used when loading an app image.
  static void RemoveDuplicates(const InternTable::UnorderedSet& old_set,
                               /*inout*/InternTable::UnorderedSet* new_set,
                               /*inout*/SafeMap<mirror::String*, mirror::String*>* intern_remap)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (old_set.size() < new_set->size()) {
      for (const GcRoot<mirror::String>& old_s : old_set) {
        auto new_it = new_set->find(old_s);
        if (UNLIKELY(new_it != new_set->end())) {
          intern_remap->Put(new_it->Read<kWithoutReadBarrier>(), old_s.Read<kWithoutReadBarrier>());
          new_set->erase(new_it);
        }
      }
    } else {
      for (auto new_it = new_set->begin(), end = new_set->end(); new_it != end; ) {
        auto old_it = old_set.find(*new_it);
        if (UNLIKELY(old_it != old_set.end())) {
          intern_remap->Put(new_it->Read<kWithoutReadBarrier>(),
                            old_it->Read<kWithoutReadBarrier>());
          new_it = new_set->erase(new_it);
        } else {
          ++new_it;
        }
      }
    }
  }

  static bool ValidateBootImageChecksum(const char* image_filename,
                                        const ImageHeader& image_header,
                                        const OatFile* oat_file,
                                        ArrayRef<ImageSpace* const> boot_image_spaces,
                                        /*out*/size_t* boot_image_space_dependencies,
                                        /*out*/std::string* error_msg) {
    // Use the boot image component count to calculate the checksum from
    // the appropriate number of boot image chunks.
    uint32_t boot_image_component_count = image_header.GetBootImageComponentCount();
    size_t expected_image_component_count = ImageSpace::GetNumberOfComponents(boot_image_spaces);
    if (boot_image_component_count > expected_image_component_count) {
      *error_msg = StringPrintf("Too many boot image dependencies (%u > %zu) in image %s",
                                boot_image_component_count,
                                expected_image_component_count,
                                image_filename);
      return false;
    }
    uint32_t checksum = 0u;
    size_t chunk_count = 0u;
    size_t space_pos = 0u;
    uint64_t boot_image_size = 0u;
    for (size_t component_count = 0u; component_count != boot_image_component_count; ) {
      const ImageHeader& current_header = boot_image_spaces[space_pos]->GetImageHeader();
      if (current_header.GetComponentCount() > boot_image_component_count - component_count) {
        *error_msg = StringPrintf("Boot image component count in %s ends in the middle of a chunk, "
                                      "%u is between %zu and %zu",
                                  image_filename,
                                  boot_image_component_count,
                                  component_count,
                                  component_count + current_header.GetComponentCount());
        return false;
      }
      component_count += current_header.GetComponentCount();
      checksum ^= current_header.GetImageChecksum();
      chunk_count += 1u;
      space_pos += current_header.GetImageSpaceCount();
      boot_image_size += current_header.GetImageReservationSize();
    }
    if (image_header.GetBootImageChecksum() != checksum) {
      *error_msg = StringPrintf("Boot image checksum mismatch (0x%08x != 0x%08x) in image %s",
                                image_header.GetBootImageChecksum(),
                                checksum,
                                image_filename);
      return false;
    }
    if (image_header.GetBootImageSize() != boot_image_size) {
      *error_msg = StringPrintf("Boot image size mismatch (0x%08x != 0x%08" PRIx64 ") in image %s",
                                image_header.GetBootImageSize(),
                                boot_image_size,
                                image_filename);
      return false;
    }
    // Oat checksums, if present, have already been validated, so we know that
    // they match the loaded image spaces. Therefore, we just verify that they
    // are consistent in the number of boot image chunks they list by looking
    // for the kImageChecksumPrefix at the start of each component.
    const char* oat_boot_class_path_checksums =
        oat_file->GetOatHeader().GetStoreValueByKey(OatHeader::kBootClassPathChecksumsKey);
    if (oat_boot_class_path_checksums != nullptr) {
      size_t oat_bcp_chunk_count = 0u;
      while (*oat_boot_class_path_checksums == kImageChecksumPrefix) {
        oat_bcp_chunk_count += 1u;
        // Find the start of the next component if any.
        const char* separator = strchr(oat_boot_class_path_checksums, ':');
        oat_boot_class_path_checksums = (separator != nullptr) ? separator + 1u : "";
      }
      if (oat_bcp_chunk_count != chunk_count) {
        *error_msg = StringPrintf("Boot image chunk count mismatch (%zu != %zu) in image %s",
                                  oat_bcp_chunk_count,
                                  chunk_count,
                                  image_filename);
        return false;
      }
    }
    *boot_image_space_dependencies = space_pos;
    return true;
  }

  static MemMap LoadImageFile(const char* image_filename,
                              const char* image_location,
                              const ImageHeader& image_header,
                              int fd,
                              off_t start,
                              bool allow_direct_mapping,
                              TimingLogger* logger,
                              /*inout*/ MemMap* image_reservation,
                              /*out*/ std::string* error_msg) {
    TimingLogger::ScopedTiming timing("MapImageFile", logger);

    // The runtime might not be available at this point if we're running dex2oat or oatdump, in
    // which case we just truncate the madvise optimization limit completely.
    Runtime* runtime = Runtime::Current();
    const size_t madvise_size_limit = runtime ? runtime->GetMadviseWillNeedSizeArt() : 0;

    const bool is_compressed = image_header.HasCompressedBlock();
    if (!is_compressed && allow_direct_mapping) {
      uint8_t* address = (image_reservation != nullptr) ? image_reservation->Begin() : nullptr;
      // The reserved memory size is aligned up to kElfSegmentAlignment to ensure
      // that the next reserved area will be aligned to the value.
      MemMap map = MemMap::MapFileAtAddress(
          address,
          CondRoundUp<kPageSizeAgnostic>(image_header.GetImageSize(), kElfSegmentAlignment),
          PROT_READ | PROT_WRITE,
          MAP_PRIVATE,
          fd,
          start,
          /*low_4gb=*/true,
          image_filename,
          /*reuse=*/false,
          image_reservation,
          error_msg);
      if (map.IsValid()) {
        Runtime::MadviseFileForRange(
            madvise_size_limit, map.Size(), map.Begin(), map.End(), image_filename);
      }
      return map;
    }

    // Reserve output and copy/decompress into it.
    // The reserved memory size is aligned up to kElfSegmentAlignment to ensure
    // that the next reserved area will be aligned to the value.
    MemMap map = MemMap::MapAnonymous(image_location,
                                      CondRoundUp<kPageSizeAgnostic>(image_header.GetImageSize(),
                                                                     kElfSegmentAlignment),
                                      PROT_READ | PROT_WRITE,
                                      /*low_4gb=*/ true,
                                      image_reservation,
                                      error_msg);
    if (map.IsValid()) {
      const size_t stored_size = image_header.GetDataSize();
      MemMap temp_map = MemMap::MapFile(sizeof(ImageHeader) + stored_size,
                                        PROT_READ,
                                        MAP_PRIVATE,
                                        fd,
                                        start,
                                        /*low_4gb=*/false,
                                        image_filename,
                                        error_msg);
      if (!temp_map.IsValid()) {
        DCHECK(error_msg == nullptr || !error_msg->empty());
        return MemMap::Invalid();
      }

      Runtime::MadviseFileForRange(
          madvise_size_limit, temp_map.Size(), temp_map.Begin(), temp_map.End(), image_filename);

      if (is_compressed) {
        memcpy(map.Begin(), &image_header, sizeof(ImageHeader));

        Runtime::ScopedThreadPoolUsage stpu;
        ThreadPool* const pool = stpu.GetThreadPool();
        const uint64_t start_time = NanoTime();
        Thread* const self = Thread::Current();
        static constexpr size_t kMinBlocks = 2u;
        const bool use_parallel = pool != nullptr && image_header.GetBlockCount() >= kMinBlocks;
        bool failed_decompression = false;
        for (const ImageHeader::Block& block : image_header.GetBlocks(temp_map.Begin())) {
          auto function = [&](Thread*) {
            const uint64_t start2 = NanoTime();
            ScopedTrace trace("LZ4 decompress block");
            bool result = block.Decompress(/*out_ptr=*/map.Begin(),
                                           /*in_ptr=*/temp_map.Begin(),
                                           error_msg);
            if (!result) {
              failed_decompression = true;
              if (error_msg != nullptr) {
                *error_msg = "Failed to decompress image block " + *error_msg;
              }
            }
            VLOG(image) << "Decompress block " << block.GetDataSize() << " -> "
                        << block.GetImageSize() << " in " << PrettyDuration(NanoTime() - start2);
          };
          if (use_parallel) {
            pool->AddTask(self, new FunctionTask(std::move(function)));
          } else {
            function(self);
          }
        }
        if (use_parallel) {
          ScopedTrace trace("Waiting for workers");
          pool->Wait(self, true, false);
        }
        const uint64_t time = NanoTime() - start_time;
        // Add one 1 ns to prevent possible divide by 0.
        VLOG(image) << "Decompressing image took " << PrettyDuration(time) << " ("
                    << PrettySize(static_cast<uint64_t>(map.Size()) * MsToNs(1000) / (time + 1))
                    << "/s)";
        if (failed_decompression) {
          DCHECK(error_msg == nullptr || !error_msg->empty());
          return MemMap::Invalid();
        }
      } else {
        DCHECK(!allow_direct_mapping);
        // We do not allow direct mapping for boot image extensions compiled to a memfd.
        // This prevents wasting memory by kernel keeping the contents of the file alive
        // despite these contents being unreachable once the file descriptor is closed
        // and mmapped memory is copied for all existing mappings.
        //
        // Most pages would be copied during relocation while there is only one mapping.
        // We could use MAP_SHARED for relocation and then msync() and remap MAP_PRIVATE
        // as required for forking from zygote, but there would still be some pages
        // wasted anyway and we want to avoid that. (For example, static synchronized
        // methods use the class object for locking and thus modify its lockword.)

        // No other process should race to overwrite the extension in memfd.
        DCHECK_EQ(memcmp(temp_map.Begin(), &image_header, sizeof(ImageHeader)), 0);
        memcpy(map.Begin(), temp_map.Begin(), temp_map.Size());
      }
    }

    return map;
  }

  class EmptyRange {
   public:
    ALWAYS_INLINE bool InSource(uintptr_t) const { return false; }
    ALWAYS_INLINE bool InDest(uintptr_t) const { return false; }
    ALWAYS_INLINE uintptr_t ToDest(uintptr_t) const {
      LOG(FATAL) << "Unreachable";
      UNREACHABLE();
    }
  };

  template <typename Range0, typename Range1 = EmptyRange, typename Range2 = EmptyRange>
  class ForwardAddress {
   public:
    explicit ForwardAddress(const Range0& range0 = Range0(),
                            const Range1& range1 = Range1(),
                            const Range2& range2 = Range2())
        : range0_(range0), range1_(range1), range2_(range2) {}

    // Return the relocated address of a heap object.
    // Null checks must be performed in the caller (for performance reasons).
    template <typename T>
    ALWAYS_INLINE T* operator()(T* src) const {
      DCHECK(src != nullptr);
      const uintptr_t uint_src = reinterpret_cast<uintptr_t>(src);
      if (range2_.InSource(uint_src)) {
        return reinterpret_cast<T*>(range2_.ToDest(uint_src));
      }
      if (range1_.InSource(uint_src)) {
        return reinterpret_cast<T*>(range1_.ToDest(uint_src));
      }
      CHECK(range0_.InSource(uint_src))
          << reinterpret_cast<const void*>(src) << " not in "
          << reinterpret_cast<const void*>(range0_.Source()) << "-"
          << reinterpret_cast<const void*>(range0_.Source() + range0_.Length());
      return reinterpret_cast<T*>(range0_.ToDest(uint_src));
    }

   private:
    const Range0 range0_;
    const Range1 range1_;
    const Range2 range2_;
  };

  template <typename Forward>
  class FixupRootVisitor {
   public:
    template<typename... Args>
    explicit FixupRootVisitor(Args... args) : forward_(args...) {}

    ALWAYS_INLINE void VisitRootIfNonNull(mirror::CompressedReference<mirror::Object>* root) const
        REQUIRES_SHARED(Locks::mutator_lock_) {
      if (!root->IsNull()) {
        VisitRoot(root);
      }
    }

    ALWAYS_INLINE void VisitRoot(mirror::CompressedReference<mirror::Object>* root) const
        REQUIRES_SHARED(Locks::mutator_lock_) {
      mirror::Object* ref = root->AsMirrorPtr();
      mirror::Object* new_ref = forward_(ref);
      if (ref != new_ref) {
        root->Assign(new_ref);
      }
    }

   private:
    Forward forward_;
  };

  template <typename Forward>
  class FixupObjectVisitor {
   public:
    explicit FixupObjectVisitor(gc::accounting::ContinuousSpaceBitmap* visited,
                                const Forward& forward)
        : visited_(visited), forward_(forward) {}

    // Fix up separately since we also need to fix up method entrypoints.
    ALWAYS_INLINE void VisitRootIfNonNull(
        [[maybe_unused]] mirror::CompressedReference<mirror::Object>* root) const {}

    ALWAYS_INLINE void VisitRoot(
        [[maybe_unused]] mirror::CompressedReference<mirror::Object>* root) const {}

    ALWAYS_INLINE void operator()(ObjPtr<mirror::Object> obj,
                                  MemberOffset offset,
                                  [[maybe_unused]] bool is_static) const NO_THREAD_SAFETY_ANALYSIS {
      // Space is not yet added to the heap, don't do a read barrier.
      mirror::Object* ref = obj->GetFieldObject<mirror::Object, kVerifyNone, kWithoutReadBarrier>(
          offset);
      if (ref != nullptr) {
        // Use SetFieldObjectWithoutWriteBarrier to avoid card marking since we are writing to the
        // image.
        obj->SetFieldObjectWithoutWriteBarrier<false, true, kVerifyNone>(offset, forward_(ref));
      }
    }

    // java.lang.ref.Reference visitor.
    ALWAYS_INLINE void operator()(ObjPtr<mirror::Class> klass, ObjPtr<mirror::Reference> ref) const
        REQUIRES_SHARED(Locks::mutator_lock_) REQUIRES(Locks::heap_bitmap_lock_) {
      DCHECK(klass->IsTypeOfReferenceClass());
      this->operator()(ref, mirror::Reference::ReferentOffset(), /*is_static=*/ false);
    }

    void operator()(mirror::Object* obj) const
        NO_THREAD_SAFETY_ANALYSIS {
      if (!visited_->Set(obj)) {
        // Not already visited.
        obj->VisitReferences</*visit native roots*/false, kVerifyNone, kWithoutReadBarrier>(
            *this,
            *this);
        CHECK(!obj->IsClass());
      }
    }

   private:
    gc::accounting::ContinuousSpaceBitmap* const visited_;
    Forward forward_;
  };

  // Relocate an image space mapped at target_base which possibly used to be at a different base
  // address. In place means modifying a single ImageSpace in place rather than relocating from
  // one ImageSpace to another.
  template <PointerSize kPointerSize>
  static bool RelocateInPlace(uint32_t boot_image_begin,
                              uint8_t* target_base,
                              accounting::ContinuousSpaceBitmap* bitmap,
                              const OatFile* app_oat_file,
                              std::string* error_msg) {
    DCHECK(error_msg != nullptr);
    // Set up sections.
    ImageHeader* image_header = reinterpret_cast<ImageHeader*>(target_base);
    const uint32_t boot_image_size = image_header->GetBootImageSize();
    const ImageSection& objects_section = image_header->GetObjectsSection();
    // Where the app image objects are mapped to.
    uint8_t* objects_location = target_base + objects_section.Offset();
    TimingLogger logger(__FUNCTION__, true, false);
    RelocationRange boot_image(image_header->GetBootImageBegin(),
                               boot_image_begin,
                               boot_image_size);
    // Metadata is everything after the objects section, use exclusion to be safe.
    RelocationRange app_image_metadata(
        reinterpret_cast<uintptr_t>(image_header->GetImageBegin()) + objects_section.End(),
        reinterpret_cast<uintptr_t>(target_base) + objects_section.End(),
        image_header->GetImageSize() - objects_section.End());
    // App image heap objects, may be mapped in the heap.
    RelocationRange app_image_objects(
        reinterpret_cast<uintptr_t>(image_header->GetImageBegin()) + objects_section.Offset(),
        reinterpret_cast<uintptr_t>(objects_location),
        objects_section.Size());
    // Use the oat data section since this is where the OatFile::Begin is.
    RelocationRange app_oat(reinterpret_cast<uintptr_t>(image_header->GetOatDataBegin()),
                            // Not necessarily in low 4GB.
                            reinterpret_cast<uintptr_t>(app_oat_file->Begin()),
                            image_header->GetOatDataEnd() - image_header->GetOatDataBegin());
    VLOG(image) << "App image metadata " << app_image_metadata;
    VLOG(image) << "App image objects " << app_image_objects;
    VLOG(image) << "App oat " << app_oat;
    VLOG(image) << "Boot image " << boot_image;
    // True if we need to fixup any heap pointers.
    const bool fixup_image = boot_image.Delta() != 0 || app_image_metadata.Delta() != 0 ||
        app_image_objects.Delta() != 0;
    if (!fixup_image) {
      // Nothing to fix up.
      return true;
    }

    // TODO: Assert that the app image does not contain any Method, Constructor,
    // FieldVarHandle or StaticFieldVarHandle. These require extra relocation
    // for the `ArtMethod*` and `ArtField*` pointers they contain.

    using ForwardObject = ForwardAddress<RelocationRange, RelocationRange>;
    ForwardObject forward_object(boot_image, app_image_objects);
    ForwardObject forward_metadata(boot_image, app_image_metadata);
    using ForwardCode = ForwardAddress<RelocationRange, RelocationRange>;
    ForwardCode forward_code(boot_image, app_oat);
    PatchObjectVisitor<kPointerSize, ForwardObject, ForwardCode> patch_object_visitor(
        forward_object,
        forward_metadata);
    if (fixup_image) {
      // Two pass approach, fix up all classes first, then fix up non class-objects.
      // The visited bitmap is used to ensure that pointer arrays are not forwarded twice.
      gc::accounting::ContinuousSpaceBitmap visited_bitmap(
          gc::accounting::ContinuousSpaceBitmap::Create("Relocate bitmap",
                                                        target_base,
                                                        image_header->GetImageSize()));
      {
        TimingLogger::ScopedTiming timing("Fixup classes", &logger);
        const auto& class_table_section = image_header->GetClassTableSection();
        if (class_table_section.Size() > 0u) {
          ScopedObjectAccess soa(Thread::Current());
          ScopedDebugDisallowReadBarriers sddrb(Thread::Current());
          ObjPtr<mirror::ObjectArray<mirror::Object>> image_roots = app_image_objects.ToDest(
              image_header->GetImageRoots<kWithoutReadBarrier>().Ptr());
          int32_t class_roots_index = enum_cast<int32_t>(ImageHeader::kClassRoots);
          DCHECK_LT(class_roots_index, image_roots->GetLength<kVerifyNone>());
          ObjPtr<mirror::ObjectArray<mirror::Class>> class_roots =
              ObjPtr<mirror::ObjectArray<mirror::Class>>::DownCast(boot_image.ToDest(
                  image_roots->GetWithoutChecks<kVerifyNone,
                                                kWithoutReadBarrier>(class_roots_index).Ptr()));
          ObjPtr<mirror::Class> class_class =
              GetClassRoot<mirror::Class, kWithoutReadBarrier>(class_roots);
          ClassTableVisitor class_table_visitor(forward_object);
          size_t read_count = 0u;
          const uint8_t* data = target_base + class_table_section.Offset();
          // We avoid making a copy of the data since we want modifications to be propagated to the
          // memory map.
          ClassTable::ClassSet temp_set(data, /*make_copy_of_data=*/ false, &read_count);
          for (ClassTable::TableSlot& slot : temp_set) {
            slot.VisitRoot(class_table_visitor);
            ObjPtr<mirror::Class> klass = slot.Read<kWithoutReadBarrier>();
            if (!app_image_objects.InDest(klass.Ptr())) {
              continue;
            }
            const bool already_marked = visited_bitmap.Set(klass.Ptr());
            CHECK(!already_marked) << "App image class already visited";
            patch_object_visitor.VisitClass(klass, class_class);
            // Then patch the non-embedded vtable and iftable.
            ObjPtr<mirror::PointerArray> vtable =
                klass->GetVTable<kVerifyNone, kWithoutReadBarrier>();
            if (vtable != nullptr &&
                app_image_objects.InDest(vtable.Ptr()) &&
                !visited_bitmap.Set(vtable.Ptr())) {
              patch_object_visitor.VisitPointerArray(vtable);
            }
            ObjPtr<mirror::IfTable> iftable = klass->GetIfTable<kVerifyNone, kWithoutReadBarrier>();
            if (iftable != nullptr && app_image_objects.InDest(iftable.Ptr())) {
              // Avoid processing the fields of iftable since we will process them later anyways
              // below.
              int32_t ifcount = klass->GetIfTableCount<kVerifyNone>();
              for (int32_t i = 0; i != ifcount; ++i) {
                ObjPtr<mirror::PointerArray> unpatched_ifarray =
                    iftable->GetMethodArrayOrNull<kVerifyNone, kWithoutReadBarrier>(i);
                if (unpatched_ifarray != nullptr) {
                  // The iftable has not been patched, so we need to explicitly adjust the pointer.
                  ObjPtr<mirror::PointerArray> ifarray = forward_object(unpatched_ifarray.Ptr());
                  if (app_image_objects.InDest(ifarray.Ptr()) &&
                      !visited_bitmap.Set(ifarray.Ptr())) {
                    patch_object_visitor.VisitPointerArray(ifarray);
                  }
                }
              }
            }
          }
        }
      }

      // Fixup objects may read fields in the boot image so we hold the mutator lock (although it is
      // probably not required).
      TimingLogger::ScopedTiming timing("Fixup objects", &logger);
      ScopedObjectAccess soa(Thread::Current());
      ScopedDebugDisallowReadBarriers sddrb(Thread::Current());
      // Need to update the image to be at the target base.
      uintptr_t objects_begin = reinterpret_cast<uintptr_t>(target_base + objects_section.Offset());
      uintptr_t objects_end = reinterpret_cast<uintptr_t>(target_base + objects_section.End());
      FixupObjectVisitor<ForwardObject> fixup_object_visitor(&visited_bitmap, forward_object);
      bitmap->VisitMarkedRange(objects_begin, objects_end, fixup_object_visitor);
      // Fixup image roots.
      CHECK(app_image_objects.InSource(reinterpret_cast<uintptr_t>(
          image_header->GetImageRoots<kWithoutReadBarrier>().Ptr())));
      image_header->RelocateImageReferences(app_image_objects.Delta());
      image_header->RelocateBootImageReferences(boot_image.Delta());
      CHECK_EQ(image_header->GetImageBegin(), target_base);

      // Fix up dex cache arrays.
      ObjPtr<mirror::ObjectArray<mirror::DexCache>> dex_caches =
          image_header->GetImageRoot<kWithoutReadBarrier>(ImageHeader::kDexCaches)
              ->AsObjectArray<mirror::DexCache, kVerifyNone>();
      for (int32_t i = 0, count = dex_caches->GetLength(); i < count; ++i) {
        ObjPtr<mirror::DexCache> dex_cache =
            dex_caches->GetWithoutChecks<kVerifyNone, kWithoutReadBarrier>(i);
        patch_object_visitor.VisitDexCacheArrays(dex_cache);
      }
    }
    {
      // Only touches objects in the app image, no need for mutator lock.
      TimingLogger::ScopedTiming timing("Fixup methods", &logger);
      ScopedDebugDisallowReadBarriers sddrb(Thread::Current());
      image_header->VisitPackedArtMethods([&](ArtMethod& method) NO_THREAD_SAFETY_ANALYSIS {
        // TODO: Consider a separate visitor for runtime vs normal methods.
        if (UNLIKELY(method.IsRuntimeMethod())) {
          ImtConflictTable* table = method.GetImtConflictTable(kPointerSize);
          if (table != nullptr) {
            ImtConflictTable* new_table = forward_metadata(table);
            if (table != new_table) {
              method.SetImtConflictTable(new_table, kPointerSize);
            }
          }
        } else {
          patch_object_visitor.PatchGcRoot(&method.DeclaringClassRoot());
          if (method.IsNative()) {
            const void* old_native_code = method.GetEntryPointFromJniPtrSize(kPointerSize);
            const void* new_native_code = forward_code(old_native_code);
            if (old_native_code != new_native_code) {
              method.SetEntryPointFromJniPtrSize(new_native_code, kPointerSize);
            }
          }
        }
        const void* old_code = method.GetEntryPointFromQuickCompiledCodePtrSize(kPointerSize);
        const void* new_code = forward_code(old_code);
        if (old_code != new_code) {
          method.SetEntryPointFromQuickCompiledCode(new_code);
        }
      }, target_base, kPointerSize);
    }
    if (fixup_image) {
      {
        // Only touches objects in the app image, no need for mutator lock.
        TimingLogger::ScopedTiming timing("Fixup fields", &logger);
        ScopedDebugDisallowReadBarriers sddrb(Thread::Current());
        image_header->VisitPackedArtFields([&](ArtField& field) NO_THREAD_SAFETY_ANALYSIS {
          patch_object_visitor.template PatchGcRoot</*kMayBeNull=*/ false>(
              &field.DeclaringClassRoot());
        }, target_base);
      }
      {
        TimingLogger::ScopedTiming timing("Fixup imt", &logger);
        ScopedDebugDisallowReadBarriers sddrb(Thread::Current());
        image_header->VisitPackedImTables(forward_metadata, target_base, kPointerSize);
      }
      {
        TimingLogger::ScopedTiming timing("Fixup conflict tables", &logger);
        ScopedDebugDisallowReadBarriers sddrb(Thread::Current());
        image_header->VisitPackedImtConflictTables(forward_metadata, target_base, kPointerSize);
      }
      // Fix up the intern table.
      const auto& intern_table_section = image_header->GetInternedStringsSection();
      if (intern_table_section.Size() > 0u) {
        TimingLogger::ScopedTiming timing("Fixup intern table", &logger);
        ScopedObjectAccess soa(Thread::Current());
        ScopedDebugDisallowReadBarriers sddrb(Thread::Current());
        // Fixup the pointers in the newly written intern table to contain image addresses.
        InternTable temp_intern_table;
        // Note that we require that ReadFromMemory does not make an internal copy of the elements
        // so that the VisitRoots() will update the memory directly rather than the copies.
        temp_intern_table.AddTableFromMemory(target_base + intern_table_section.Offset(),
                                             [&](InternTable::UnorderedSet& strings)
            REQUIRES_SHARED(Locks::mutator_lock_) {
          for (GcRoot<mirror::String>& root : strings) {
            root = GcRoot<mirror::String>(forward_object(root.Read<kWithoutReadBarrier>()));
          }
        }, /*is_boot_image=*/ false);
      }
    }
    if (VLOG_IS_ON(image)) {
      logger.Dump(LOG_STREAM(INFO));
    }
    return true;
  }
};

void ImageSpace::AppendImageChecksum(uint32_t component_count,
                                     uint32_t checksum,
                                     /*inout*/ std::string* checksums) {
  static_assert(ImageSpace::kImageChecksumPrefix == 'i', "Format prefix check.");
  StringAppendF(checksums, "i;%u/%08x", component_count, checksum);
}

static bool CheckAndRemoveImageChecksum(uint32_t component_count,
                                        uint32_t checksum,
                                        /*inout*/std::string_view* oat_checksums,
                                        /*out*/std::string* error_msg) {
  std::string image_checksum;
  ImageSpace::AppendImageChecksum(component_count, checksum, &image_checksum);
  if (!oat_checksums->starts_with(image_checksum)) {
    *error_msg = StringPrintf("Image checksum mismatch, expected %s to start with %s",
                              std::string(*oat_checksums).c_str(),
                              image_checksum.c_str());
    return false;
  }
  oat_checksums->remove_prefix(image_checksum.size());
  return true;
}

std::string ImageSpace::BootImageLayout::GetPrimaryImageLocation() {
  DCHECK(!image_locations_.empty());
  std::string location = image_locations_[0];
  size_t profile_separator_pos = location.find(kProfileSeparator);
  if (profile_separator_pos != std::string::npos) {
    location.resize(profile_separator_pos);
  }
  if (location.find('/') == std::string::npos) {
    // No path, so use the path from the first boot class path component.
    size_t slash_pos = boot_class_path_.empty()
        ? std::string::npos
        : boot_class_path_[0].rfind('/');
    if (slash_pos == std::string::npos) {
      return std::string();
    }
    location.insert(0u, boot_class_path_[0].substr(0u, slash_pos + 1u));
  }
  return location;
}

bool ImageSpace::BootImageLayout::VerifyImageLocation(
    ArrayRef<const std::string> components,
    /*out*/size_t* named_components_count,
    /*out*/std::string* error_msg) {
  DCHECK(named_components_count != nullptr);

  // Validate boot class path. Require a path and non-empty name in each component.
  for (const std::string& bcp_component : boot_class_path_) {
    size_t bcp_slash_pos = bcp_component.rfind('/');
    if (bcp_slash_pos == std::string::npos || bcp_slash_pos == bcp_component.size() - 1u) {
      *error_msg = StringPrintf("Invalid boot class path component: %s", bcp_component.c_str());
      return false;
    }
  }

  // Validate the format of image location components.
  size_t components_size = components.size();
  if (components_size == 0u) {
    *error_msg = "Empty image location.";
    return false;
  }
  size_t wildcards_start = components_size;  // No wildcards.
  for (size_t i = 0; i != components_size; ++i) {
    const std::string& component = components[i];
    DCHECK(!component.empty());  // Guaranteed by Split().
    std::vector<std::string> parts = android::base::Split(component, {kProfileSeparator});
    size_t wildcard_pos = component.find('*');
    if (wildcard_pos == std::string::npos) {
      if (wildcards_start != components.size()) {
        *error_msg =
            StringPrintf("Image component without wildcard after component with wildcard: %s",
                         component.c_str());
        return false;
      }
      for (size_t j = 0; j < parts.size(); j++) {
        if (parts[j].empty()) {
          *error_msg = StringPrintf("Missing component and/or profile name in %s",
                                    component.c_str());
          return false;
        }
        if (parts[j].back() == '/') {
          *error_msg = StringPrintf("%s name ends with path separator: %s",
                                    j == 0 ? "Image component" : "Profile",
                                    component.c_str());
          return false;
        }
      }
    } else {
      if (parts.size() > 1) {
        *error_msg = StringPrintf("Unsupproted wildcard (*) and profile delimiter (!) in %s",
                                  component.c_str());
        return false;
      }
      if (wildcards_start == components_size) {
        wildcards_start = i;
      }
      // Wildcard must be the last character.
      if (wildcard_pos != component.size() - 1u) {
        *error_msg = StringPrintf("Unsupported wildcard (*) position in %s", component.c_str());
        return false;
      }
      // And it must be either plain wildcard or preceded by a path separator.
      if (component.size() != 1u && component[wildcard_pos - 1u] != '/') {
        *error_msg = StringPrintf("Non-plain wildcard (*) not preceded by path separator '/': %s",
                                  component.c_str());
        return false;
      }
      if (i == 0) {
        *error_msg = StringPrintf("Primary component contains wildcard (*): %s", component.c_str());
        return false;
      }
    }
  }

  *named_components_count = wildcards_start;
  return true;
}

bool ImageSpace::BootImageLayout::MatchNamedComponents(
    ArrayRef<const std::string> named_components,
    /*out*/std::vector<NamedComponentLocation>* named_component_locations,
    /*out*/std::string* error_msg) {
  DCHECK(!named_components.empty());
  DCHECK(named_component_locations->empty());
  named_component_locations->reserve(named_components.size());
  size_t bcp_component_count = boot_class_path_.size();
  size_t bcp_pos = 0;
  std::string base_name;
  for (size_t i = 0, size = named_components.size(); i != size; ++i) {
    std::string component = named_components[i];
    std::vector<std::string> profile_filenames;  // Empty.
    std::vector<std::string> parts = android::base::Split(component, {kProfileSeparator});
    for (size_t j = 0; j < parts.size(); j++) {
      if (j == 0) {
        component = std::move(parts[j]);
        DCHECK(!component.empty());  // Checked by VerifyImageLocation()
      } else {
        profile_filenames.push_back(std::move(parts[j]));
        DCHECK(!profile_filenames.back().empty());  // Checked by VerifyImageLocation()
      }
    }
    size_t slash_pos = component.rfind('/');
    std::string base_location;
    if (i == 0u) {
      // The primary boot image name is taken as provided. It forms the base
      // for expanding the extension filenames.
      if (slash_pos != std::string::npos) {
        base_name = component.substr(slash_pos + 1u);
        base_location = component;
      } else {
        base_name = component;
        base_location = GetBcpComponentPath(0u) + component;
      }
    } else {
      std::string to_match;
      if (slash_pos != std::string::npos) {
        // If we have the full path, we just need to match the filename to the BCP component.
        base_location = component.substr(0u, slash_pos + 1u) + base_name;
        to_match = component;
      }
      while (true) {
        if (slash_pos == std::string::npos) {
          // If we do not have a full path, we need to update the path based on the BCP location.
          std::string path = GetBcpComponentPath(bcp_pos);
          to_match = path + component;
          base_location = path + base_name;
        }
        if (ExpandLocation(base_location, bcp_pos) == to_match) {
          break;
        }
        ++bcp_pos;
        if (bcp_pos == bcp_component_count) {
          *error_msg = StringPrintf("Image component %s does not match a boot class path component",
                                    component.c_str());
          return false;
        }
      }
    }
    for (std::string& profile_filename : profile_filenames) {
      if (profile_filename.find('/') == std::string::npos) {
        profile_filename.insert(/*pos*/ 0u, GetBcpComponentPath(bcp_pos));
      }
    }
    NamedComponentLocation location;
    location.base_location = base_location;
    location.bcp_index = bcp_pos;
    location.profile_filenames = profile_filenames;
    named_component_locations->push_back(location);
    ++bcp_pos;
  }
  return true;
}

bool ImageSpace::BootImageLayout::ValidateBootImageChecksum(const char* file_description,
                                                            const ImageHeader& header,
                                                            /*out*/std::string* error_msg) {
  uint32_t boot_image_component_count = header.GetBootImageComponentCount();
  if (chunks_.empty() != (boot_image_component_count == 0u)) {
    *error_msg = StringPrintf("Unexpected boot image component count in %s: %u, %s",
                              file_description,
                              boot_image_component_count,
                              chunks_.empty() ? "should be 0" : "should not be 0");
    return false;
  }
  uint32_t component_count = 0u;
  uint32_t composite_checksum = 0u;
  uint64_t boot_image_size = 0u;
  for (const ImageChunk& chunk : chunks_) {
    if (component_count == boot_image_component_count) {
      break;  // Hit the component count.
    }
    if (chunk.start_index != component_count) {
      break;  // End of contiguous chunks, fail below; same as reaching end of `chunks_`.
    }
    if (chunk.component_count > boot_image_component_count - component_count) {
      *error_msg = StringPrintf("Boot image component count in %s ends in the middle of a chunk, "
                                    "%u is between %u and %u",
                                file_description,
                                boot_image_component_count,
                                component_count,
                                component_count + chunk.component_count);
      return false;
    }
    component_count += chunk.component_count;
    composite_checksum ^= chunk.checksum;
    boot_image_size += chunk.reservation_size;
  }
  DCHECK_LE(component_count, boot_image_component_count);
  if (component_count != boot_image_component_count) {
    *error_msg = StringPrintf("Missing boot image components for checksum in %s: %u > %u",
                              file_description,
                              boot_image_component_count,
                              component_count);
    return false;
  }
  if (composite_checksum != header.GetBootImageChecksum()) {
    *error_msg = StringPrintf("Boot image checksum mismatch in %s: 0x%08x != 0x%08x",
                              file_description,
                              header.GetBootImageChecksum(),
                              composite_checksum);
    return false;
  }
  if (boot_image_size != header.GetBootImageSize()) {
    *error_msg = StringPrintf("Boot image size mismatch in %s: 0x%08x != 0x%08" PRIx64,
                              file_description,
                              header.GetBootImageSize(),
                              boot_image_size);
    return false;
  }
  return true;
}

bool ImageSpace::BootImageLayout::ValidateHeader(const ImageHeader& header,
                                                 size_t bcp_index,
                                                 const char* file_description,
                                                 /*out*/std::string* error_msg) {
  size_t bcp_component_count = boot_class_path_.size();
  DCHECK_LT(bcp_index, bcp_component_count);
  size_t allowed_component_count = bcp_component_count - bcp_index;
  DCHECK_LE(total_reservation_size_, kMaxTotalImageReservationSize);
  size_t allowed_reservation_size = kMaxTotalImageReservationSize - total_reservation_size_;

  if (header.GetComponentCount() == 0u ||
      header.GetComponentCount() > allowed_component_count) {
    *error_msg = StringPrintf("Unexpected component count in %s, received %u, "
                                  "expected non-zero and <= %zu",
                              file_description,
                              header.GetComponentCount(),
                              allowed_component_count);
    return false;
  }
  if (header.GetImageReservationSize() > allowed_reservation_size) {
    *error_msg = StringPrintf("Reservation size too big in %s: %u > %zu",
                              file_description,
                              header.GetImageReservationSize(),
                              allowed_reservation_size);
    return false;
  }
  if (!ValidateBootImageChecksum(file_description, header, error_msg)) {
    return false;
  }

  return true;
}

bool ImageSpace::BootImageLayout::ValidateOatFile(
    const std::string& base_location,
    const std::string& base_filename,
    size_t bcp_index,
    size_t component_count,
    /*out*/std::string* error_msg) {
  std::string art_filename = ExpandLocation(base_filename, bcp_index);
  std::string art_location = ExpandLocation(base_location, bcp_index);
  std::string oat_filename = ImageHeader::GetOatLocationFromImageLocation(art_filename);
  std::string oat_location = ImageHeader::GetOatLocationFromImageLocation(art_location);
  int oat_fd = bcp_index < boot_class_path_oat_files_.size()
      ? boot_class_path_oat_files_[bcp_index].Fd()
      : -1;
  int vdex_fd = bcp_index < boot_class_path_vdex_files_.size()
      ? boot_class_path_vdex_files_[bcp_index].Fd()
      : -1;
  auto dex_filenames =
      ArrayRef<const std::string>(boot_class_path_).SubArray(bcp_index, component_count);
  ArrayRef<File> dex_files =
      bcp_index + component_count < boot_class_path_files_.size() ?
          ArrayRef<File>(boot_class_path_files_).SubArray(bcp_index, component_count) :
          ArrayRef<File>();
  // We open the oat file here only for validating that it's up-to-date. We don't open it as
  // executable or mmap it to a reserved space. This `OatFile` object will be dropped after
  // validation, and will not go into the `ImageSpace`.
  std::unique_ptr<OatFile> oat_file;
  DCHECK_EQ(oat_fd >= 0, vdex_fd >= 0);
  if (oat_fd >= 0) {
    oat_file.reset(OatFile::Open(
        /*zip_fd=*/ -1,
        vdex_fd,
        oat_fd,
        oat_location,
        /*executable=*/ false,
        /*low_4gb=*/ false,
        dex_filenames,
        dex_files,
        /*reservation=*/ nullptr,
        error_msg));
  } else {
    oat_file.reset(OatFile::Open(
        /*zip_fd=*/ -1,
        oat_filename,
        oat_location,
        /*executable=*/ false,
        /*low_4gb=*/ false,
        dex_filenames,
        dex_files,
        /*reservation=*/ nullptr,
        error_msg));
  }
  if (oat_file == nullptr) {
    *error_msg = StringPrintf("Failed to open oat file '%s' when validating it for image '%s': %s",
                              oat_filename.c_str(),
                              art_location.c_str(),
                              error_msg->c_str());
    return false;
  }
  if (!ImageSpace::ValidateOatFile(
          *oat_file, error_msg, dex_filenames, dex_files, apex_versions_)) {
    return false;
  }
  return true;
}

bool ImageSpace::BootImageLayout::ReadHeader(const std::string& base_location,
                                             const std::string& base_filename,
                                             size_t bcp_index,
                                             /*out*/std::string* error_msg) {
  DCHECK_LE(next_bcp_index_, bcp_index);
  DCHECK_LT(bcp_index, boot_class_path_.size());

  std::string actual_filename = ExpandLocation(base_filename, bcp_index);
  int bcp_image_fd = bcp_index < boot_class_path_image_files_.size() ?
                         boot_class_path_image_files_[bcp_index].Fd() :
                         -1;
  ImageHeader header;
  // When BCP image is provided as FD, it needs to be dup'ed (since it's stored in unique_fd) so
  // that it can later be used in LoadComponents.
  auto image_file = bcp_image_fd >= 0
      ? std::make_unique<File>(DupCloexec(bcp_image_fd), actual_filename, /*check_usage=*/ false)
      : std::unique_ptr<File>(OS::OpenFileForReading(actual_filename.c_str()));
  if (!image_file || !image_file->IsOpened()) {
    *error_msg = StringPrintf("Unable to open file \"%s\" for reading image header",
                              actual_filename.c_str());
    return false;
  }
  if (!ReadSpecificImageHeader(image_file.get(), actual_filename.c_str(), &header, error_msg)) {
    return false;
  }
  const char* file_description = actual_filename.c_str();
  if (!ValidateHeader(header, bcp_index, file_description, error_msg)) {
    return false;
  }

  // Validate oat files. We do it here so that the boot image will be re-compiled in memory if it's
  // outdated.
  size_t component_count = (header.GetImageSpaceCount() == 1u) ? header.GetComponentCount() : 1u;
  for (size_t i = 0; i < header.GetImageSpaceCount(); i++) {
    if (!ValidateOatFile(base_location, base_filename, bcp_index + i, component_count, error_msg)) {
      return false;
    }
  }

  if (chunks_.empty()) {
    base_address_ = reinterpret_cast32<uint32_t>(header.GetImageBegin());
  }
  ImageChunk chunk;
  chunk.base_location = base_location;
  chunk.base_filename = base_filename;
  chunk.start_index = bcp_index;
  chunk.component_count = header.GetComponentCount();
  chunk.image_space_count = header.GetImageSpaceCount();
  chunk.reservation_size = header.GetImageReservationSize();
  chunk.checksum = header.GetImageChecksum();
  chunk.boot_image_component_count = header.GetBootImageComponentCount();
  chunk.boot_image_checksum = header.GetBootImageChecksum();
  chunk.boot_image_size = header.GetBootImageSize();
  chunks_.push_back(std::move(chunk));
  next_bcp_index_ = bcp_index + header.GetComponentCount();
  total_component_count_ += header.GetComponentCount();
  total_reservation_size_ += header.GetImageReservationSize();
  return true;
}

bool ImageSpace::BootImageLayout::CompileBootclasspathElements(
    const std::string& base_location,
    const std::string& base_filename,
    size_t bcp_index,
    const std::vector<std::string>& profile_filenames,
    ArrayRef<const std::string> dependencies,
    /*out*/std::string* error_msg) {
  DCHECK_LE(total_component_count_, next_bcp_index_);
  DCHECK_LE(next_bcp_index_, bcp_index);
  size_t bcp_component_count = boot_class_path_.size();
  DCHECK_LT(bcp_index, bcp_component_count);
  DCHECK(!profile_filenames.empty());
  if (total_component_count_ != bcp_index) {
    // We require all previous BCP components to have a boot image space (primary or extension).
    *error_msg = "Cannot compile extension because of missing dependencies.";
    return false;
  }
  Runtime* runtime = Runtime::Current();
  if (!runtime->IsImageDex2OatEnabled()) {
    *error_msg = "Cannot compile bootclasspath because dex2oat for image compilation is disabled.";
    return false;
  }

  // Check dependencies.
  DCHECK_EQ(dependencies.empty(), bcp_index == 0);
  size_t dependency_component_count = 0;
  for (size_t i = 0, size = dependencies.size(); i != size; ++i) {
    if (chunks_.size() == i || chunks_[i].start_index != dependency_component_count) {
      *error_msg = StringPrintf("Missing extension dependency \"%s\"", dependencies[i].c_str());
      return false;
    }
    dependency_component_count += chunks_[i].component_count;
  }

  // Collect locations from the profile.
  std::set<std::string> dex_locations;
  for (const std::string& profile_filename : profile_filenames) {
    std::unique_ptr<File> profile_file(OS::OpenFileForReading(profile_filename.c_str()));
    if (profile_file == nullptr) {
      *error_msg = StringPrintf("Failed to open profile file \"%s\" for reading, error: %s",
                                profile_filename.c_str(),
                                strerror(errno));
      return false;
    }

    // TODO: Rewrite ProfileCompilationInfo to provide a better interface and
    // to store the dex locations in uncompressed section of the file.
    auto collect_fn = [&dex_locations](const std::string& dex_location,
                                       [[maybe_unused]] uint32_t checksum) {
      dex_locations.insert(dex_location);  // Just collect locations.
      return false;                        // Do not read the profile data.
    };
    ProfileCompilationInfo info(/*for_boot_image=*/ true);
    if (!info.Load(profile_file->Fd(), /*merge_classes=*/ true, collect_fn)) {
      *error_msg = StringPrintf("Failed to scan profile from %s", profile_filename.c_str());
      return false;
    }
  }

  // Match boot class path components to locations from profile.
  // Note that the profile records only filenames without paths.
  size_t bcp_end = bcp_index;
  for (; bcp_end != bcp_component_count; ++bcp_end) {
    const std::string& bcp_component = boot_class_path_locations_[bcp_end];
    size_t slash_pos = bcp_component.rfind('/');
    DCHECK_NE(slash_pos, std::string::npos);
    std::string bcp_component_name = bcp_component.substr(slash_pos + 1u);
    if (dex_locations.count(bcp_component_name) == 0u) {
      break;  // Did not find the current location in dex file.
    }
  }

  if (bcp_end == bcp_index) {
    // No data for the first (requested) component.
    *error_msg = StringPrintf("The profile does not contain data for %s",
                              boot_class_path_locations_[bcp_index].c_str());
    return false;
  }

  // Create in-memory files.
  std::string art_filename = ExpandLocation(base_filename, bcp_index);
  std::string vdex_filename = ImageHeader::GetVdexLocationFromImageLocation(art_filename);
  std::string oat_filename = ImageHeader::GetOatLocationFromImageLocation(art_filename);
  android::base::unique_fd art_fd(memfd_create(art_filename.c_str(), /*flags=*/ 0));
  android::base::unique_fd vdex_fd(memfd_create(vdex_filename.c_str(), /*flags=*/ 0));
  android::base::unique_fd oat_fd(memfd_create(oat_filename.c_str(), /*flags=*/ 0));
  if (art_fd.get() == -1 || vdex_fd.get() == -1 || oat_fd.get() == -1) {
    *error_msg = StringPrintf("Failed to create memfd handles for compiling bootclasspath for %s",
                              boot_class_path_locations_[bcp_index].c_str());
    return false;
  }

  // Construct the dex2oat command line.
  std::string dex2oat = runtime->GetCompilerExecutable();
  ArrayRef<const std::string> head_bcp =
      boot_class_path_.SubArray(/*pos=*/ 0u, /*length=*/ dependency_component_count);
  ArrayRef<const std::string> head_bcp_locations =
      boot_class_path_locations_.SubArray(/*pos=*/ 0u, /*length=*/ dependency_component_count);
  ArrayRef<const std::string> bcp_to_compile =
      boot_class_path_.SubArray(/*pos=*/ bcp_index, /*length=*/ bcp_end - bcp_index);
  ArrayRef<const std::string> bcp_to_compile_locations =
      boot_class_path_locations_.SubArray(/*pos=*/ bcp_index, /*length=*/ bcp_end - bcp_index);
  std::string boot_class_path = head_bcp.empty() ?
                                    Join(bcp_to_compile, ':') :
                                    Join(head_bcp, ':') + ':' + Join(bcp_to_compile, ':');
  std::string boot_class_path_locations =
      head_bcp_locations.empty() ?
          Join(bcp_to_compile_locations, ':') :
          Join(head_bcp_locations, ':') + ':' + Join(bcp_to_compile_locations, ':');

  std::vector<std::string> args;
  args.push_back(dex2oat);
  args.push_back("--runtime-arg");
  args.push_back("-Xbootclasspath:" + boot_class_path);
  args.push_back("--runtime-arg");
  args.push_back("-Xbootclasspath-locations:" + boot_class_path_locations);
  if (dependencies.empty()) {
    args.push_back(android::base::StringPrintf("--base=0x%08x", ART_BASE_ADDRESS));
  } else {
    args.push_back("--boot-image=" + Join(dependencies, kComponentSeparator));
  }
  for (size_t i = bcp_index; i != bcp_end; ++i) {
    args.push_back("--dex-file=" + boot_class_path_[i]);
    args.push_back("--dex-location=" + boot_class_path_locations_[i]);
  }
  args.push_back("--image-fd=" + std::to_string(art_fd.get()));
  args.push_back("--output-vdex-fd=" + std::to_string(vdex_fd.get()));
  args.push_back("--oat-fd=" + std::to_string(oat_fd.get()));
  args.push_back("--oat-location=" + ImageHeader::GetOatLocationFromImageLocation(base_filename));
  args.push_back("--single-image");
  args.push_back("--image-format=uncompressed");

  // We currently cannot guarantee that the boot class path has no verification failures.
  // And we do not want to compile anything, compilation should be done by JIT in zygote.
  args.push_back("--compiler-filter=verify");

  // Pass the profiles.
  for (const std::string& profile_filename : profile_filenames) {
    args.push_back("--profile-file=" + profile_filename);
  }

  // Do not let the file descriptor numbers change the compilation output.
  args.push_back("--avoid-storing-invocation");

  runtime->AddCurrentRuntimeFeaturesAsDex2OatArguments(&args);

  if (!kIsTargetBuild) {
    args.push_back("--host");
  }

  // Image compiler options go last to allow overriding above args, such as --compiler-filter.
  for (const std::string& compiler_option : runtime->GetImageCompilerOptions()) {
    args.push_back(compiler_option);
  }

  // Compile.
  VLOG(image) << "Compiling boot bootclasspath for " << (bcp_end - bcp_index)
              << " components, starting from " << boot_class_path_locations_[bcp_index];
  if (!Exec(args, error_msg)) {
    return false;
  }

  // Read and validate the image header.
  ImageHeader header;
  {
    File image_file(art_fd.release(), /*check_usage=*/ false);
    if (!ReadSpecificImageHeader(&image_file, "compiled image file", &header, error_msg)) {
      return false;
    }
    art_fd.reset(image_file.Release());
  }
  const char* file_description = "compiled image file";
  if (!ValidateHeader(header, bcp_index, file_description, error_msg)) {
    return false;
  }

  DCHECK_EQ(chunks_.empty(), dependencies.empty());
  ImageChunk chunk;
  chunk.base_location = base_location;
  chunk.base_filename = base_filename;
  chunk.profile_files = profile_filenames;
  chunk.start_index = bcp_index;
  chunk.component_count = header.GetComponentCount();
  chunk.image_space_count = header.GetImageSpaceCount();
  chunk.reservation_size = header.GetImageReservationSize();
  chunk.checksum = header.GetImageChecksum();
  chunk.boot_image_component_count = header.GetBootImageComponentCount();
  chunk.boot_image_checksum = header.GetBootImageChecksum();
  chunk.boot_image_size = header.GetBootImageSize();
  chunk.art_fd.reset(art_fd.release());
  chunk.vdex_fd.reset(vdex_fd.release());
  chunk.oat_fd.reset(oat_fd.release());
  chunks_.push_back(std::move(chunk));
  next_bcp_index_ = bcp_index + header.GetComponentCount();
  total_component_count_ += header.GetComponentCount();
  total_reservation_size_ += header.GetImageReservationSize();
  return true;
}

template <typename FilenameFn>
bool ImageSpace::BootImageLayout::Load(FilenameFn&& filename_fn,
                                       bool allow_in_memory_compilation,
                                       /*out*/ std::string* error_msg) {
  DCHECK(GetChunks().empty());
  DCHECK_EQ(GetBaseAddress(), 0u);

  ArrayRef<const std::string> components = image_locations_;
  size_t named_components_count = 0u;
  if (!VerifyImageLocation(components, &named_components_count, error_msg)) {
    return false;
  }

  ArrayRef<const std::string> named_components =
      ArrayRef<const std::string>(components).SubArray(/*pos=*/ 0u, named_components_count);

  std::vector<NamedComponentLocation> named_component_locations;
  if (!MatchNamedComponents(named_components, &named_component_locations, error_msg)) {
    return false;
  }

  // Load the image headers of named components.
  DCHECK_EQ(named_component_locations.size(), named_components.size());
  const size_t bcp_component_count = boot_class_path_.size();
  size_t bcp_pos = 0u;
  for (size_t i = 0, size = named_components.size(); i != size; ++i) {
    const std::string& base_location = named_component_locations[i].base_location;
    size_t bcp_index = named_component_locations[i].bcp_index;
    const std::vector<std::string>& profile_filenames =
        named_component_locations[i].profile_filenames;
    DCHECK_EQ(i == 0, bcp_index == 0);
    if (bcp_index < bcp_pos) {
      DCHECK_NE(i, 0u);
      LOG(ERROR) << "Named image component already covered by previous image: " << base_location;
      continue;
    }
    std::string local_error_msg;
    std::string base_filename;
    if (!filename_fn(base_location, &base_filename, &local_error_msg) ||
        !ReadHeader(base_location, base_filename, bcp_index, &local_error_msg)) {
      LOG(ERROR) << "Error reading named image component header for " << base_location
                 << ", error: " << local_error_msg;
      // If the primary boot image is invalid, we generate a single full image. This is faster than
      // generating the primary boot image and the extension separately.
      if (bcp_index == 0) {
        if (!allow_in_memory_compilation) {
          // The boot image is unusable and we can't continue by generating a boot image in memory.
          // All we can do is to return.
          *error_msg = std::move(local_error_msg);
          return false;
        }
        // We must at least have profiles for the core libraries.
        if (profile_filenames.empty()) {
          *error_msg = "Full boot image cannot be compiled because no profile is provided.";
          return false;
        }
        std::vector<std::string> all_profiles;
        for (const NamedComponentLocation& named_component_location : named_component_locations) {
          const std::vector<std::string>& profiles = named_component_location.profile_filenames;
          all_profiles.insert(all_profiles.end(), profiles.begin(), profiles.end());
        }
        if (!CompileBootclasspathElements(base_location,
                                          base_filename,
                                          /*bcp_index=*/ 0,
                                          all_profiles,
                                          /*dependencies=*/ ArrayRef<const std::string>{},
                                          &local_error_msg)) {
          *error_msg =
              StringPrintf("Full boot image cannot be compiled: %s", local_error_msg.c_str());
          return false;
        }
        // No extensions are needed.
        return true;
      }
      bool should_compile_extension = allow_in_memory_compilation && !profile_filenames.empty();
      if (!should_compile_extension ||
          !CompileBootclasspathElements(base_location,
                                        base_filename,
                                        bcp_index,
                                        profile_filenames,
                                        components.SubArray(/*pos=*/ 0, /*length=*/ 1),
                                        &local_error_msg)) {
        if (should_compile_extension) {
          LOG(ERROR) << "Error compiling boot image extension for " << boot_class_path_[bcp_index]
                     << ", error: " << local_error_msg;
        }
        bcp_pos = bcp_index + 1u;  // Skip at least this component.
        DCHECK_GT(bcp_pos, GetNextBcpIndex());
        continue;
      }
    }
    bcp_pos = GetNextBcpIndex();
  }

  // Look for remaining components if there are any wildcard specifications.
  ArrayRef<const std::string> search_paths = components.SubArray(/*pos=*/ named_components_count);
  if (!search_paths.empty()) {
    const std::string& primary_base_location = named_component_locations[0].base_location;
    size_t base_slash_pos = primary_base_location.rfind('/');
    DCHECK_NE(base_slash_pos, std::string::npos);
    std::string base_name = primary_base_location.substr(base_slash_pos + 1u);
    DCHECK(!base_name.empty());
    while (bcp_pos != bcp_component_count) {
      const std::string& bcp_component =  boot_class_path_[bcp_pos];
      bool found = false;
      for (const std::string& path : search_paths) {
        std::string base_location;
        if (path.size() == 1u) {
          DCHECK_EQ(path, "*");
          size_t slash_pos = bcp_component.rfind('/');
          DCHECK_NE(slash_pos, std::string::npos);
          base_location = bcp_component.substr(0u, slash_pos + 1u) + base_name;
        } else {
          DCHECK(path.ends_with("/*"));
          base_location = path.substr(0u, path.size() - 1u) + base_name;
        }
        std::string err_msg;  // Ignored.
        std::string base_filename;
        if (filename_fn(base_location, &base_filename, &err_msg) &&
            ReadHeader(base_location, base_filename, bcp_pos, &err_msg)) {
          VLOG(image) << "Found image extension for " << ExpandLocation(base_location, bcp_pos);
          bcp_pos = GetNextBcpIndex();
          found = true;
          break;
        }
      }
      if (!found) {
        ++bcp_pos;
      }
    }
  }

  return true;
}

bool ImageSpace::BootImageLayout::LoadFromSystem(InstructionSet image_isa,
                                                 bool allow_in_memory_compilation,
                                                 /*out*/ std::string* error_msg) {
  auto filename_fn = [image_isa](const std::string& location,
                                 /*out*/ std::string* filename,
                                 [[maybe_unused]] /*out*/ std::string* err_msg) {
    *filename = GetSystemImageFilename(location.c_str(), image_isa);
    return true;
  };
  return Load(filename_fn, allow_in_memory_compilation, error_msg);
}

class ImageSpace::BootImageLoader {
 public:
  // Creates an instance.
  // `apex_versions` is created from `Runtime::GetApexVersions` and must outlive this instance.
  BootImageLoader(const std::vector<std::string>& boot_class_path,
                  const std::vector<std::string>& boot_class_path_locations,
                  ArrayRef<File> boot_class_path_files,
                  ArrayRef<File> boot_class_path_image_files,
                  ArrayRef<File> boot_class_path_vdex_files,
                  ArrayRef<File> boot_class_path_oat_files,
                  const std::vector<std::string>& image_locations,
                  InstructionSet image_isa,
                  bool relocate,
                  bool executable,
                  const std::string* apex_versions)
      : boot_class_path_(boot_class_path),
        boot_class_path_locations_(boot_class_path_locations),
        boot_class_path_files_(boot_class_path_files),
        boot_class_path_image_files_(boot_class_path_image_files),
        boot_class_path_vdex_files_(boot_class_path_vdex_files),
        boot_class_path_oat_files_(boot_class_path_oat_files),
        image_locations_(image_locations),
        image_isa_(image_isa),
        relocate_(relocate),
        executable_(executable),
        has_system_(false),
        apex_versions_(apex_versions) {}

  void FindImageFiles() {
    BootImageLayout layout(image_locations_,
                           boot_class_path_,
                           boot_class_path_locations_,
                           boot_class_path_files_,
                           boot_class_path_image_files_,
                           boot_class_path_vdex_files_,
                           boot_class_path_oat_files_,
                           apex_versions_);
    std::string image_location = layout.GetPrimaryImageLocation();
    std::string system_filename;
    bool found_image = FindImageFilenameImpl(image_location.c_str(),
                                             image_isa_,
                                             &has_system_,
                                             &system_filename);
    DCHECK_EQ(found_image, has_system_);
  }

  bool HasSystem() const { return has_system_; }

  bool LoadFromSystem(size_t extra_reservation_size,
                      bool allow_in_memory_compilation,
                      /*out*/std::vector<std::unique_ptr<ImageSpace>>* boot_image_spaces,
                      /*out*/MemMap* extra_reservation,
                      /*out*/std::string* error_msg) REQUIRES_SHARED(Locks::mutator_lock_);

 private:
  bool LoadImage(
      const BootImageLayout& layout,
      bool validate_oat_file,
      size_t extra_reservation_size,
      TimingLogger* logger,
      /*out*/std::vector<std::unique_ptr<ImageSpace>>* boot_image_spaces,
      /*out*/MemMap* extra_reservation,
      /*out*/std::string* error_msg) REQUIRES_SHARED(Locks::mutator_lock_) {
    ArrayRef<const BootImageLayout::ImageChunk> chunks = layout.GetChunks();
    DCHECK(!chunks.empty());
    const uint32_t base_address = layout.GetBaseAddress();
    const size_t image_component_count = layout.GetTotalComponentCount();
    const size_t image_reservation_size = layout.GetTotalReservationSize();

    DCHECK_LE(image_reservation_size, kMaxTotalImageReservationSize);
    static_assert(kMaxTotalImageReservationSize < std::numeric_limits<uint32_t>::max());
    if (extra_reservation_size > std::numeric_limits<uint32_t>::max() - image_reservation_size) {
      // Since the `image_reservation_size` is limited to kMaxTotalImageReservationSize,
      // the `extra_reservation_size` would have to be really excessive to fail this check.
      *error_msg = StringPrintf("Excessive extra reservation size: %zu", extra_reservation_size);
      return false;
    }

    // Reserve address space. If relocating, choose a random address for ALSR.
    uint8_t* addr = reinterpret_cast<uint8_t*>(
        relocate_ ? ART_BASE_ADDRESS + ChooseRelocationOffsetDelta() : base_address);
    MemMap image_reservation =
        ReserveBootImageMemory(addr, image_reservation_size + extra_reservation_size, error_msg);
    if (!image_reservation.IsValid()) {
      return false;
    }

    // Load components.
    std::vector<std::unique_ptr<ImageSpace>> spaces;
    spaces.reserve(image_component_count);
    size_t max_image_space_dependencies = 0u;
    for (size_t i = 0, num_chunks = chunks.size(); i != num_chunks; ++i) {
      const BootImageLayout::ImageChunk& chunk = chunks[i];
      std::string extension_error_msg;
      uint8_t* old_reservation_begin = image_reservation.Begin();
      size_t old_reservation_size = image_reservation.Size();
      DCHECK_LE(chunk.reservation_size, old_reservation_size);
      if (!LoadComponents(chunk,
                          validate_oat_file,
                          max_image_space_dependencies,
                          logger,
                          &spaces,
                          &image_reservation,
                          (i == 0) ? error_msg : &extension_error_msg)) {
        // Failed to load the chunk. If this is the primary boot image, report the error.
        if (i == 0) {
          return false;
        }
        // For extension, shrink the reservation (and remap if needed, see below).
        size_t new_reservation_size = old_reservation_size - chunk.reservation_size;
        if (new_reservation_size == 0u) {
          DCHECK_EQ(extra_reservation_size, 0u);
          DCHECK_EQ(i + 1u, num_chunks);
          image_reservation.Reset();
        } else if (old_reservation_begin != image_reservation.Begin()) {
          // Part of the image reservation has been used and then unmapped when
          // rollling back the partial boot image extension load. Try to remap
          // the image reservation. As this should be running single-threaded,
          // the address range should still be available to mmap().
          image_reservation.Reset();
          std::string remap_error_msg;
          image_reservation = ReserveBootImageMemory(old_reservation_begin,
                                                     new_reservation_size,
                                                     &remap_error_msg);
          if (!image_reservation.IsValid()) {
            *error_msg = StringPrintf("Failed to remap boot image reservation after failing "
                                          "to load boot image extension (%s: %s): %s",
                                      boot_class_path_locations_[chunk.start_index].c_str(),
                                      extension_error_msg.c_str(),
                                      remap_error_msg.c_str());
            return false;
          }
        } else {
          DCHECK_EQ(old_reservation_size, image_reservation.Size());
          image_reservation.SetSize(new_reservation_size);
        }
        LOG(ERROR) << "Failed to load boot image extension "
            << boot_class_path_locations_[chunk.start_index] << ": " << extension_error_msg;
      }
      // Update `max_image_space_dependencies` if all previous BCP components
      // were covered and loading the current chunk succeeded.
      size_t total_component_count = 0;
      for (const std::unique_ptr<ImageSpace>& space : spaces) {
        total_component_count += space->GetComponentCount();
      }
      if (max_image_space_dependencies == chunk.start_index &&
          total_component_count == chunk.start_index + chunk.component_count) {
        max_image_space_dependencies = chunk.start_index + chunk.component_count;
      }
    }

    MemMap local_extra_reservation;
    if (!RemapExtraReservation(extra_reservation_size,
                               &image_reservation,
                               &local_extra_reservation,
                               error_msg)) {
      return false;
    }

    MaybeRelocateSpaces(spaces, logger);
    DeduplicateInternedStrings(ArrayRef<const std::unique_ptr<ImageSpace>>(spaces), logger);
    boot_image_spaces->swap(spaces);
    *extra_reservation = std::move(local_extra_reservation);
    return true;
  }

 private:
  class SimpleRelocateVisitor {
   public:
    SimpleRelocateVisitor(uint32_t diff, uint32_t begin, uint32_t size)
        : diff_(diff), begin_(begin), size_(size) {}

    // Adapter taking the same arguments as SplitRangeRelocateVisitor
    // to simplify constructing the various visitors in DoRelocateSpaces().
    SimpleRelocateVisitor(uint32_t base_diff,
                          uint32_t current_diff,
                          uint32_t bound,
                          uint32_t begin,
                          uint32_t size)
        : SimpleRelocateVisitor(base_diff, begin, size) {
      // Check arguments unused by this class.
      DCHECK_EQ(base_diff, current_diff);
      DCHECK_EQ(bound, begin);
    }

    template <typename T>
    ALWAYS_INLINE T* operator()(T* src) const {
      DCHECK(InSource(src));
      uint32_t raw_src = reinterpret_cast32<uint32_t>(src);
      return reinterpret_cast32<T*>(raw_src + diff_);
    }

    template <typename T>
    ALWAYS_INLINE bool InSource(T* ptr) const {
      uint32_t raw_ptr = reinterpret_cast32<uint32_t>(ptr);
      return raw_ptr - begin_ < size_;
    }

    template <typename T>
    ALWAYS_INLINE bool InDest(T* ptr) const {
      uint32_t raw_ptr = reinterpret_cast32<uint32_t>(ptr);
      uint32_t src_ptr = raw_ptr - diff_;
      return src_ptr - begin_ < size_;
    }

   private:
    const uint32_t diff_;
    const uint32_t begin_;
    const uint32_t size_;
  };

  class SplitRangeRelocateVisitor {
   public:
    SplitRangeRelocateVisitor(uint32_t base_diff,
                              uint32_t current_diff,
                              uint32_t bound,
                              uint32_t begin,
                              uint32_t size)
        : base_diff_(base_diff),
          current_diff_(current_diff),
          bound_(bound),
          begin_(begin),
          size_(size) {
      DCHECK_NE(begin_, bound_);
      // The bound separates the boot image range and the extension range.
      DCHECK_LT(bound_ - begin_, size_);
    }

    template <typename T>
    ALWAYS_INLINE T* operator()(T* src) const {
      DCHECK(InSource(src));
      uint32_t raw_src = reinterpret_cast32<uint32_t>(src);
      uint32_t diff = (raw_src < bound_) ? base_diff_ : current_diff_;
      return reinterpret_cast32<T*>(raw_src + diff);
    }

    template <typename T>
    ALWAYS_INLINE bool InSource(T* ptr) const {
      uint32_t raw_ptr = reinterpret_cast32<uint32_t>(ptr);
      return raw_ptr - begin_ < size_;
    }

   private:
    const uint32_t base_diff_;
    const uint32_t current_diff_;
    const uint32_t bound_;
    const uint32_t begin_;
    const uint32_t size_;
  };

  static void** PointerAddress(ArtMethod* method, MemberOffset offset) {
    return reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(method) + offset.Uint32Value());
  }

  template <PointerSize kPointerSize>
  static void DoRelocateSpaces(ArrayRef<const std::unique_ptr<ImageSpace>>& spaces,
                               int64_t base_diff64) REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK(!spaces.empty());
    gc::accounting::ContinuousSpaceBitmap patched_objects(
        gc::accounting::ContinuousSpaceBitmap::Create(
            "Marked objects",
            spaces.front()->Begin(),
            spaces.back()->End() - spaces.front()->Begin()));
    const ImageHeader& base_header = spaces[0]->GetImageHeader();
    size_t base_image_space_count = base_header.GetImageSpaceCount();
    DCHECK_LE(base_image_space_count, spaces.size());
    DoRelocateSpaces<kPointerSize, /*kExtension=*/ false>(
        spaces.SubArray(/*pos=*/ 0u, base_image_space_count),
        base_diff64,
        &patched_objects);

    for (size_t i = base_image_space_count, size = spaces.size(); i != size; ) {
      const ImageHeader& ext_header = spaces[i]->GetImageHeader();
      size_t ext_image_space_count = ext_header.GetImageSpaceCount();
      DCHECK_LE(ext_image_space_count, size - i);
      DoRelocateSpaces<kPointerSize, /*kExtension=*/ true>(
          spaces.SubArray(/*pos=*/ i, ext_image_space_count),
          base_diff64,
          &patched_objects);
      i += ext_image_space_count;
    }
  }

  template <PointerSize kPointerSize, bool kExtension>
  static void DoRelocateSpaces(ArrayRef<const std::unique_ptr<ImageSpace>> spaces,
                               int64_t base_diff64,
                               gc::accounting::ContinuousSpaceBitmap* patched_objects)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    DCHECK(!spaces.empty());
    const ImageHeader& first_header = spaces.front()->GetImageHeader();
    uint32_t image_begin = reinterpret_cast32<uint32_t>(first_header.GetImageBegin());
    uint32_t image_size = first_header.GetImageReservationSize();
    DCHECK_NE(image_size, 0u);
    uint32_t source_begin = kExtension ? first_header.GetBootImageBegin() : image_begin;
    uint32_t source_size = kExtension ? first_header.GetBootImageSize() + image_size : image_size;
    if (kExtension) {
      DCHECK_EQ(first_header.GetBootImageBegin() + first_header.GetBootImageSize(), image_begin);
    }
    int64_t current_diff64 = kExtension
        ? static_cast<int64_t>(reinterpret_cast32<uint32_t>(spaces.front()->Begin())) -
              static_cast<int64_t>(image_begin)
        : base_diff64;
    if (base_diff64 == 0 && current_diff64 == 0) {
      return;
    }
    uint32_t base_diff = static_cast<uint32_t>(base_diff64);
    uint32_t current_diff = static_cast<uint32_t>(current_diff64);

    // For boot image the main visitor is a SimpleRelocateVisitor. For the boot image extension we
    // mostly use a SplitRelocationVisitor but some work can still use the SimpleRelocationVisitor.
    using MainRelocateVisitor = typename std::conditional<
        kExtension, SplitRangeRelocateVisitor, SimpleRelocateVisitor>::type;
    SimpleRelocateVisitor simple_relocate_visitor(current_diff, image_begin, image_size);
    MainRelocateVisitor main_relocate_visitor(
        base_diff, current_diff, /*bound=*/ image_begin, source_begin, source_size);

    using MainPatchRelocateVisitor =
        PatchObjectVisitor<kPointerSize, MainRelocateVisitor, MainRelocateVisitor>;
    using SimplePatchRelocateVisitor =
        PatchObjectVisitor<kPointerSize, SimpleRelocateVisitor, SimpleRelocateVisitor>;
    MainPatchRelocateVisitor main_patch_object_visitor(main_relocate_visitor,
                                                       main_relocate_visitor);
    SimplePatchRelocateVisitor simple_patch_object_visitor(simple_relocate_visitor,
                                                           simple_relocate_visitor);

    // Retrieve the Class.class, Method.class and Constructor.class needed in the loops below.
    ObjPtr<mirror::ObjectArray<mirror::Class>> class_roots;
    ObjPtr<mirror::Class> class_class;
    ObjPtr<mirror::Class> method_class;
    ObjPtr<mirror::Class> constructor_class;
    ObjPtr<mirror::Class> field_var_handle_class;
    ObjPtr<mirror::Class> static_field_var_handle_class;
    {
      ObjPtr<mirror::ObjectArray<mirror::Object>> image_roots =
          simple_relocate_visitor(first_header.GetImageRoots<kWithoutReadBarrier>().Ptr());
      DCHECK(!patched_objects->Test(image_roots.Ptr()));

      SimpleRelocateVisitor base_relocate_visitor(
          base_diff,
          source_begin,
          kExtension ? source_size - image_size : image_size);
      int32_t class_roots_index = enum_cast<int32_t>(ImageHeader::kClassRoots);
      DCHECK_LT(class_roots_index, image_roots->GetLength<kVerifyNone>());
      class_roots = ObjPtr<mirror::ObjectArray<mirror::Class>>::DownCast(base_relocate_visitor(
          image_roots->GetWithoutChecks<kVerifyNone,
                                        kWithoutReadBarrier>(class_roots_index).Ptr()));
      if (kExtension) {
        // Class roots must have been visited if we relocated the primary boot image.
        DCHECK(base_diff == 0 || patched_objects->Test(class_roots.Ptr()));
        class_class = GetClassRoot<mirror::Class, kWithoutReadBarrier>(class_roots);
        method_class = GetClassRoot<mirror::Method, kWithoutReadBarrier>(class_roots);
        constructor_class = GetClassRoot<mirror::Constructor, kWithoutReadBarrier>(class_roots);
        field_var_handle_class =
            GetClassRoot<mirror::FieldVarHandle, kWithoutReadBarrier>(class_roots);
        static_field_var_handle_class =
            GetClassRoot<mirror::StaticFieldVarHandle, kWithoutReadBarrier>(class_roots);
      } else {
        DCHECK(!patched_objects->Test(class_roots.Ptr()));
        class_class = simple_relocate_visitor(
            GetClassRoot<mirror::Class, kWithoutReadBarrier>(class_roots).Ptr());
        method_class = simple_relocate_visitor(
            GetClassRoot<mirror::Method, kWithoutReadBarrier>(class_roots).Ptr());
        constructor_class = simple_relocate_visitor(
            GetClassRoot<mirror::Constructor, kWithoutReadBarrier>(class_roots).Ptr());
        field_var_handle_class = simple_relocate_visitor(
            GetClassRoot<mirror::FieldVarHandle, kWithoutReadBarrier>(class_roots).Ptr());
        static_field_var_handle_class = simple_relocate_visitor(
            GetClassRoot<mirror::StaticFieldVarHandle, kWithoutReadBarrier>(class_roots).Ptr());
      }
    }

    for (const std::unique_ptr<ImageSpace>& space : spaces) {
      // First patch the image header.
      reinterpret_cast<ImageHeader*>(space->Begin())->RelocateImageReferences(current_diff64);
      reinterpret_cast<ImageHeader*>(space->Begin())->RelocateBootImageReferences(base_diff64);

      // Patch fields and methods.
      const ImageHeader& image_header = space->GetImageHeader();
      image_header.VisitPackedArtFields([&](ArtField& field) REQUIRES_SHARED(Locks::mutator_lock_) {
        // Fields always reference class in the current image.
        simple_patch_object_visitor.template PatchGcRoot</*kMayBeNull=*/ false>(
            &field.DeclaringClassRoot());
      }, space->Begin());
      image_header.VisitPackedArtMethods([&](ArtMethod& method)
          REQUIRES_SHARED(Locks::mutator_lock_) {
        main_patch_object_visitor.PatchGcRoot(&method.DeclaringClassRoot());
        if (!method.HasCodeItem()) {
          void** data_address = PointerAddress(&method, ArtMethod::DataOffset(kPointerSize));
          main_patch_object_visitor.PatchNativePointer(data_address);
        }
        void** entrypoint_address =
            PointerAddress(&method, ArtMethod::EntryPointFromQuickCompiledCodeOffset(kPointerSize));
        main_patch_object_visitor.PatchNativePointer(entrypoint_address);
      }, space->Begin(), kPointerSize);
      auto method_table_visitor = [&](ArtMethod* method) {
        DCHECK(method != nullptr);
        return main_relocate_visitor(method);
      };
      image_header.VisitPackedImTables(method_table_visitor, space->Begin(), kPointerSize);
      image_header.VisitPackedImtConflictTables(method_table_visitor, space->Begin(), kPointerSize);
      image_header.VisitJniStubMethods</*kUpdate=*/ true>(method_table_visitor,
                                                          space->Begin(),
                                                          kPointerSize);

      // Patch the intern table.
      if (image_header.GetInternedStringsSection().Size() != 0u) {
        const uint8_t* data = space->Begin() + image_header.GetInternedStringsSection().Offset();
        size_t read_count;
        InternTable::UnorderedSet temp_set(data, /*make_copy_of_data=*/ false, &read_count);
        for (GcRoot<mirror::String>& slot : temp_set) {
          // The intern table contains only strings in the current image.
          simple_patch_object_visitor.template PatchGcRoot</*kMayBeNull=*/ false>(&slot);
        }
      }

      // Patch the class table and classes, so that we can traverse class hierarchy to
      // determine the types of other objects when we visit them later.
      if (image_header.GetClassTableSection().Size() != 0u) {
        uint8_t* data = space->Begin() + image_header.GetClassTableSection().Offset();
        size_t read_count;
        ClassTable::ClassSet temp_set(data, /*make_copy_of_data=*/ false, &read_count);
        DCHECK(!temp_set.empty());
        // The class table contains only classes in the current image.
        ClassTableVisitor class_table_visitor(simple_relocate_visitor);
        for (ClassTable::TableSlot& slot : temp_set) {
          slot.VisitRoot(class_table_visitor);
          ObjPtr<mirror::Class> klass = slot.Read<kWithoutReadBarrier>();
          DCHECK(klass != nullptr);
          DCHECK(!patched_objects->Test(klass.Ptr()));
          patched_objects->Set(klass.Ptr());
          main_patch_object_visitor.VisitClass(klass, class_class);
          // Then patch the non-embedded vtable and iftable.
          ObjPtr<mirror::PointerArray> vtable =
              klass->GetVTable<kVerifyNone, kWithoutReadBarrier>();
          if ((kExtension ? simple_relocate_visitor.InDest(vtable.Ptr()) : vtable != nullptr) &&
              !patched_objects->Set(vtable.Ptr())) {
            main_patch_object_visitor.VisitPointerArray(vtable);
          }
          ObjPtr<mirror::IfTable> iftable = klass->GetIfTable<kVerifyNone, kWithoutReadBarrier>();
          if (kExtension ? simple_relocate_visitor.InDest(iftable.Ptr()) : iftable != nullptr) {
            int32_t ifcount = iftable->Count<kVerifyNone>();
            for (int32_t i = 0; i != ifcount; ++i) {
              ObjPtr<mirror::PointerArray> unpatched_ifarray =
                  iftable->GetMethodArrayOrNull<kVerifyNone, kWithoutReadBarrier>(i);
              if (kExtension ? simple_relocate_visitor.InSource(unpatched_ifarray.Ptr())
                             : unpatched_ifarray != nullptr) {
                // The iftable has not been patched, so we need to explicitly adjust the pointer.
                ObjPtr<mirror::PointerArray> ifarray =
                    simple_relocate_visitor(unpatched_ifarray.Ptr());
                if (!patched_objects->Set(ifarray.Ptr())) {
                  main_patch_object_visitor.VisitPointerArray(ifarray);
                }
              }
            }
          }
        }
      }
    }

    for (const std::unique_ptr<ImageSpace>& space : spaces) {
      const ImageHeader& image_header = space->GetImageHeader();

      static_assert(IsAligned<kObjectAlignment>(sizeof(ImageHeader)), "Header alignment check");
      uint32_t objects_end = image_header.GetObjectsSection().Size();
      DCHECK_ALIGNED(objects_end, kObjectAlignment);
      for (uint32_t pos = sizeof(ImageHeader); pos != objects_end; ) {
        mirror::Object* object = reinterpret_cast<mirror::Object*>(space->Begin() + pos);
        // Note: use Test() rather than Set() as this is the last time we're checking this object.
        if (!patched_objects->Test(object)) {
          // This is the last pass over objects, so we do not need to Set().
          main_patch_object_visitor.VisitObject(object);
          ObjPtr<mirror::Class> klass = object->GetClass<kVerifyNone, kWithoutReadBarrier>();
          if (klass == method_class || klass == constructor_class) {
            // Patch the ArtMethod* in the mirror::Executable subobject.
            ObjPtr<mirror::Executable> as_executable =
                ObjPtr<mirror::Executable>::DownCast(object);
            ArtMethod* unpatched_method = as_executable->GetArtMethod<kVerifyNone>();
            ArtMethod* patched_method = main_relocate_visitor(unpatched_method);
            as_executable->SetArtMethod</*kTransactionActive=*/ false,
                                        /*kCheckTransaction=*/ true,
                                        kVerifyNone>(patched_method);
          } else if (klass == field_var_handle_class || klass == static_field_var_handle_class) {
            // Patch the ArtField* in the mirror::FieldVarHandle subobject.
            ObjPtr<mirror::FieldVarHandle> as_field_var_handle =
                ObjPtr<mirror::FieldVarHandle>::DownCast(object);
            ArtField* unpatched_field = as_field_var_handle->GetArtField<kVerifyNone>();
            ArtField* patched_field = main_relocate_visitor(unpatched_field);
            as_field_var_handle->SetArtField<kVerifyNone>(patched_field);
          }
        }
        pos += RoundUp(object->SizeOf<kVerifyNone>(), kObjectAlignment);
      }
    }
    if (kIsDebugBuild && !kExtension) {
      // We used just Test() instead of Set() above but we need to use Set()
      // for class roots to satisfy a DCHECK() for extensions.
      DCHECK(!patched_objects->Test(class_roots.Ptr()));
      patched_objects->Set(class_roots.Ptr());
    }
  }

  void MaybeRelocateSpaces(const std::vector<std::unique_ptr<ImageSpace>>& spaces,
                           TimingLogger* logger)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    TimingLogger::ScopedTiming timing("MaybeRelocateSpaces", logger);
    ImageSpace* first_space = spaces.front().get();
    const ImageHeader& first_space_header = first_space->GetImageHeader();
    int64_t base_diff64 =
        static_cast<int64_t>(reinterpret_cast32<uint32_t>(first_space->Begin())) -
        static_cast<int64_t>(reinterpret_cast32<uint32_t>(first_space_header.GetImageBegin()));
    if (!relocate_) {
      DCHECK_EQ(base_diff64, 0);
    }

    // While `Thread::Current()` is null, the `ScopedDebugDisallowReadBarriers`
    // cannot be used but the class `ReadBarrier` shall not allow read barriers anyway.
    // For some gtests we actually have an initialized `Thread:Current()`.
    std::optional<ScopedDebugDisallowReadBarriers> sddrb(std::nullopt);
    if (kCheckDebugDisallowReadBarrierCount && Thread::Current() != nullptr) {
      sddrb.emplace(Thread::Current());
    }

    ArrayRef<const std::unique_ptr<ImageSpace>> spaces_ref(spaces);
    PointerSize pointer_size = first_space_header.GetPointerSize();
    if (pointer_size == PointerSize::k64) {
      DoRelocateSpaces<PointerSize::k64>(spaces_ref, base_diff64);
    } else {
      DoRelocateSpaces<PointerSize::k32>(spaces_ref, base_diff64);
    }
  }

  void DeduplicateInternedStrings(ArrayRef<const std::unique_ptr<ImageSpace>> spaces,
                                  TimingLogger* logger) REQUIRES_SHARED(Locks::mutator_lock_) {
    TimingLogger::ScopedTiming timing("DeduplicateInternedStrings", logger);
    DCHECK(!spaces.empty());
    size_t num_spaces = spaces.size();
    const ImageHeader& primary_header = spaces.front()->GetImageHeader();
    size_t primary_image_count = primary_header.GetImageSpaceCount();
    size_t primary_image_component_count = primary_header.GetComponentCount();
    DCHECK_LE(primary_image_count, num_spaces);
    // The primary boot image can be generated with `--single-image` on device, when generated
    // in-memory or with odrefresh.
    DCHECK(primary_image_count == primary_image_component_count || primary_image_count == 1);
    size_t component_count = primary_image_component_count;
    size_t space_pos = primary_image_count;
    while (space_pos != num_spaces) {
      const ImageHeader& current_header = spaces[space_pos]->GetImageHeader();
      size_t image_space_count = current_header.GetImageSpaceCount();
      DCHECK_LE(image_space_count, num_spaces - space_pos);
      size_t dependency_component_count = current_header.GetBootImageComponentCount();
      DCHECK_LE(dependency_component_count, component_count);
      if (dependency_component_count < component_count) {
        // There shall be no duplicate strings with the components that this space depends on.
        // Find the end of the dependencies, i.e. start of non-dependency images.
        size_t start_component_count = primary_image_component_count;
        size_t start_pos = primary_image_count;
        while (start_component_count != dependency_component_count) {
          const ImageHeader& dependency_header = spaces[start_pos]->GetImageHeader();
          DCHECK_LE(dependency_header.GetComponentCount(),
                    dependency_component_count - start_component_count);
          start_component_count += dependency_header.GetComponentCount();
          start_pos += dependency_header.GetImageSpaceCount();
        }
        // Remove duplicates from all intern tables belonging to the chunk.
        ArrayRef<const std::unique_ptr<ImageSpace>> old_spaces =
            spaces.SubArray(/*pos=*/ start_pos, space_pos - start_pos);
        SafeMap<mirror::String*, mirror::String*> intern_remap;
        for (size_t i = 0; i != image_space_count; ++i) {
          ImageSpace* new_space = spaces[space_pos + i].get();
          Loader::RemoveInternTableDuplicates(old_spaces, new_space, &intern_remap);
        }
        // Remap string for all spaces belonging to the chunk.
        if (!intern_remap.empty()) {
          for (size_t i = 0; i != image_space_count; ++i) {
            ImageSpace* new_space = spaces[space_pos + i].get();
            Loader::RemapInternedStringDuplicates(intern_remap, new_space);
          }
        }
      }
      component_count += current_header.GetComponentCount();
      space_pos += image_space_count;
    }
  }

  std::unique_ptr<ImageSpace> Load(const std::string& image_location,
                                   const std::string& image_filename,
                                   const std::vector<std::string>& profile_files,
                                   android::base::unique_fd art_fd,
                                   TimingLogger* logger,
                                   /*inout*/MemMap* image_reservation,
                                   /*out*/std::string* error_msg)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    if (art_fd.get() != -1) {
      VLOG(startup) << "Using image file " << image_filename.c_str() << " for image location "
                    << image_location << " for compiled extension";

      File image_file(art_fd.release(), image_filename, /*check_usage=*/false);
      int64_t file_length = image_file.GetLength();
      if (file_length < 0) {
        *error_msg =
            ART_FORMAT("Failed to get file length of '{}': {}", image_filename, strerror(errno));
        return nullptr;
      }
      std::unique_ptr<ImageSpace> result = Loader::Init(&image_file,
                                                        /*start=*/0,
                                                        file_length,
                                                        image_filename.c_str(),
                                                        image_location.c_str(),
                                                        profile_files,
                                                        /*allow_direct_mapping=*/false,
                                                        logger,
                                                        image_reservation,
                                                        error_msg);
      // Note: We're closing the image file descriptor here when we destroy
      // the `image_file` as we no longer need it.
      return result;
    }

    VLOG(startup) << "Using image file " << image_filename.c_str() << " for image location "
                  << image_location;

    // If we are in /system we can assume the image is good. We can also
    // assume this if we are using a relocated image (i.e. image checksum
    // matches) since this is only different by the offset. We need this to
    // make sure that host tests continue to work.
    // Since we are the boot image, pass null since we load the oat file from the boot image oat
    // file name.
    return Loader::Init(image_filename.c_str(),
                        image_location.c_str(),
                        logger,
                        image_reservation,
                        error_msg);
  }

  bool OpenOatFile(ImageSpace* space,
                   android::base::unique_fd vdex_fd,
                   android::base::unique_fd oat_fd,
                   ArrayRef<const std::string> dex_filenames,
                   ArrayRef<File> dex_files,
                   bool validate_oat_file,
                   ArrayRef<const std::unique_ptr<ImageSpace>> dependencies,
                   TimingLogger* logger,
                   /*inout*/ MemMap* image_reservation,
                   /*out*/ std::string* error_msg) {
    // VerifyImageAllocations() will be called later in Runtime::Init()
    // as some class roots like ArtMethod::java_lang_reflect_ArtMethod_
    // and ArtField::java_lang_reflect_ArtField_, which are used from
    // Object::SizeOf() which VerifyImageAllocations() calls, are not
    // set yet at this point.
    DCHECK(image_reservation != nullptr);
    std::unique_ptr<OatFile> oat_file;
    {
      TimingLogger::ScopedTiming timing("OpenOatFile", logger);
      std::string oat_filename =
          ImageHeader::GetOatLocationFromImageLocation(space->GetImageFilename());

      DCHECK_EQ(vdex_fd.get() != -1, oat_fd.get() != -1);
      if (vdex_fd.get() == -1) {
        oat_file.reset(OatFile::Open(/*zip_fd=*/-1,
                                     oat_filename,
                                     oat_filename,
                                     executable_,
                                     /*low_4gb=*/false,
                                     dex_filenames,
                                     dex_files,
                                     image_reservation,
                                     error_msg));
      } else {
        oat_file.reset(OatFile::Open(/*zip_fd=*/-1,
                                     vdex_fd.get(),
                                     oat_fd.get(),
                                     oat_filename,
                                     executable_,
                                     /*low_4gb=*/false,
                                     dex_filenames,
                                     dex_files,
                                     image_reservation,
                                     error_msg));
        // We no longer need the file descriptors and they will be closed by
        // the unique_fd destructor when we leave this function.
      }

      if (oat_file == nullptr) {
        *error_msg = StringPrintf("Failed to open oat file '%s' referenced from image %s: %s",
                                  oat_filename.c_str(),
                                  space->GetName(),
                                  error_msg->c_str());
        return false;
      }
      const ImageHeader& image_header = space->GetImageHeader();
      uint32_t oat_checksum = oat_file->GetOatHeader().GetChecksum();
      uint32_t image_oat_checksum = image_header.GetOatChecksum();
      if (oat_checksum != image_oat_checksum) {
        *error_msg = StringPrintf("Failed to match oat file checksum 0x%x to expected oat checksum"
                                  " 0x%x in image %s",
                                  oat_checksum,
                                  image_oat_checksum,
                                  space->GetName());
        return false;
      }
      const char* oat_boot_class_path =
          oat_file->GetOatHeader().GetStoreValueByKey(OatHeader::kBootClassPathKey);
      oat_boot_class_path = (oat_boot_class_path != nullptr) ? oat_boot_class_path : "";
      const char* oat_boot_class_path_checksums =
          oat_file->GetOatHeader().GetStoreValueByKey(OatHeader::kBootClassPathChecksumsKey);
      oat_boot_class_path_checksums =
          (oat_boot_class_path_checksums != nullptr) ? oat_boot_class_path_checksums : "";
      size_t component_count = image_header.GetComponentCount();
      if (component_count == 0u) {
        if (oat_boot_class_path[0] != 0 || oat_boot_class_path_checksums[0] != 0) {
          *error_msg = StringPrintf("Unexpected non-empty boot class path %s and/or checksums %s"
                                    " in image %s",
                                    oat_boot_class_path,
                                    oat_boot_class_path_checksums,
                                    space->GetName());
          return false;
        }
      } else if (dependencies.empty()) {
        std::string expected_boot_class_path = Join(ArrayRef<const std::string>(
              boot_class_path_locations_).SubArray(0u, component_count), ':');
        if (expected_boot_class_path != oat_boot_class_path) {
          *error_msg = StringPrintf("Failed to match oat boot class path %s to expected "
                                    "boot class path %s in image %s",
                                    oat_boot_class_path,
                                    expected_boot_class_path.c_str(),
                                    space->GetName());
          return false;
        }
      } else {
        std::string local_error_msg;
        if (!VerifyBootClassPathChecksums(
                 oat_boot_class_path_checksums,
                 oat_boot_class_path,
                 dependencies,
                 ArrayRef<const std::string>(boot_class_path_locations_),
                 ArrayRef<const std::string>(boot_class_path_),
                 &local_error_msg)) {
          *error_msg = StringPrintf("Failed to verify BCP %s with checksums %s in image %s: %s",
                                    oat_boot_class_path,
                                    oat_boot_class_path_checksums,
                                    space->GetName(),
                                    local_error_msg.c_str());
          return false;
        }
      }
      ptrdiff_t relocation_diff = space->Begin() - image_header.GetImageBegin();
      CHECK(image_header.GetOatDataBegin() != nullptr);
      uint8_t* oat_data_begin = image_header.GetOatDataBegin() + relocation_diff;
      if (oat_file->Begin() != oat_data_begin) {
        *error_msg = StringPrintf("Oat file '%s' referenced from image %s has unexpected begin"
                                      " %p v. %p",
                                  oat_filename.c_str(),
                                  space->GetName(),
                                  oat_file->Begin(),
                                  oat_data_begin);
        return false;
      }
    }
    if (validate_oat_file) {
      TimingLogger::ScopedTiming timing("ValidateOatFile", logger);
      if (!ImageSpace::ValidateOatFile(*oat_file, error_msg)) {
        DCHECK(!error_msg->empty());
        return false;
      }
    }

    // As an optimization, madvise the oat file into memory if it's being used
    // for execution with an active runtime. This can significantly improve
    // ZygoteInit class preload performance.
    if (executable_) {
      Runtime* runtime = Runtime::Current();
      if (runtime != nullptr) {
        Runtime::MadviseFileForRange(runtime->GetMadviseWillNeedSizeOdex(),
                                     oat_file->Size(),
                                     oat_file->Begin(),
                                     oat_file->End(),
                                     oat_file->GetLocation());
      }
    }

    space->oat_file_ = std::move(oat_file);
    space->oat_file_non_owned_ = space->oat_file_.get();

    return true;
  }

  bool LoadComponents(const BootImageLayout::ImageChunk& chunk,
                      bool validate_oat_file,
                      size_t max_image_space_dependencies,
                      TimingLogger* logger,
                      /*inout*/std::vector<std::unique_ptr<ImageSpace>>* spaces,
                      /*inout*/MemMap* image_reservation,
                      /*out*/std::string* error_msg)
      REQUIRES_SHARED(Locks::mutator_lock_) {
    // Make sure we destroy the spaces we created if we're returning an error.
    // Note that this can unmap part of the original `image_reservation`.
    class Guard {
     public:
      explicit Guard(std::vector<std::unique_ptr<ImageSpace>>* spaces_in)
          : spaces_(spaces_in), committed_(spaces_->size()) {}
      void Commit() {
        DCHECK_LT(committed_, spaces_->size());
        committed_ = spaces_->size();
      }
      ~Guard() {
        DCHECK_LE(committed_, spaces_->size());
        spaces_->resize(committed_);
      }
     private:
      std::vector<std::unique_ptr<ImageSpace>>* const spaces_;
      size_t committed_;
    };
    Guard guard(spaces);

    bool is_extension = (chunk.start_index != 0u);
    DCHECK_NE(spaces->empty(), is_extension);
    if (max_image_space_dependencies < chunk.boot_image_component_count) {
      DCHECK(is_extension);
      *error_msg = StringPrintf("Missing dependencies for extension component %s, %zu < %u",
                                boot_class_path_locations_[chunk.start_index].c_str(),
                                max_image_space_dependencies,
                                chunk.boot_image_component_count);
      return false;
    }
    ArrayRef<const std::string> requested_bcp_locations =
        ArrayRef<const std::string>(boot_class_path_locations_).SubArray(
            chunk.start_index, chunk.image_space_count);
    std::vector<std::string> locations =
        ExpandMultiImageLocations(requested_bcp_locations, chunk.base_location, is_extension);
    std::vector<std::string> filenames =
        ExpandMultiImageLocations(requested_bcp_locations, chunk.base_filename, is_extension);
    DCHECK_EQ(locations.size(), filenames.size());
    size_t max_dependency_count = spaces->size();
    for (size_t i = 0u, size = locations.size(); i != size; ++i) {
      android::base::unique_fd image_fd;
      if (chunk.art_fd.get() >= 0) {
        DCHECK_EQ(locations.size(), 1u);
        image_fd = std::move(chunk.art_fd);
      } else {
        size_t pos = chunk.start_index + i;
        int arg_image_fd =
            pos < boot_class_path_image_files_.size() ? boot_class_path_image_files_[pos].Fd() : -1;
        if (arg_image_fd >= 0) {
          image_fd.reset(DupCloexec(arg_image_fd));
        }
      }
      spaces->push_back(Load(locations[i],
                             filenames[i],
                             chunk.profile_files,
                             std::move(image_fd),
                             logger,
                             image_reservation,
                             error_msg));
      const ImageSpace* space = spaces->back().get();
      if (space == nullptr) {
        return false;
      }
      uint32_t expected_component_count = (i == 0u) ? chunk.component_count : 0u;
      uint32_t expected_reservation_size = (i == 0u) ? chunk.reservation_size : 0u;
      if (!Loader::CheckImageReservationSize(*space, expected_reservation_size, error_msg) ||
          !Loader::CheckImageComponentCount(*space, expected_component_count, error_msg)) {
        return false;
      }
      const ImageHeader& header = space->GetImageHeader();
      if (i == 0 && (chunk.checksum != header.GetImageChecksum() ||
                     chunk.image_space_count != header.GetImageSpaceCount() ||
                     chunk.boot_image_component_count != header.GetBootImageComponentCount() ||
                     chunk.boot_image_checksum != header.GetBootImageChecksum() ||
                     chunk.boot_image_size != header.GetBootImageSize())) {
        *error_msg = StringPrintf("Image header modified since previously read from %s; "
                                      "checksum: 0x%08x -> 0x%08x,"
                                      "image_space_count: %u -> %u"
                                      "boot_image_component_count: %u -> %u, "
                                      "boot_image_checksum: 0x%08x -> 0x%08x"
                                      "boot_image_size: 0x%08x -> 0x%08x",
                                  space->GetImageFilename().c_str(),
                                  chunk.checksum,
                                  chunk.image_space_count,
                                  header.GetImageSpaceCount(),
                                  header.GetImageChecksum(),
                                  chunk.boot_image_component_count,
                                  header.GetBootImageComponentCount(),
                                  chunk.boot_image_checksum,
                                  header.GetBootImageChecksum(),
                                  chunk.boot_image_size,
                                  header.GetBootImageSize());
        return false;
      }
    }
    DCHECK_GE(max_image_space_dependencies, chunk.boot_image_component_count);
    size_t dependency_count = 0;
    size_t dependency_component_count = 0;
    while (dependency_component_count < chunk.boot_image_component_count &&
           dependency_count < max_dependency_count) {
      const ImageHeader& current_header = (*spaces)[dependency_count]->GetImageHeader();
      dependency_component_count += current_header.GetComponentCount();
      dependency_count += current_header.GetImageSpaceCount();
    }
    if (dependency_component_count != chunk.boot_image_component_count) {
      *error_msg = StringPrintf(
          "Unable to find dependencies from image spaces; boot_image_component_count: %u",
          chunk.boot_image_component_count);
      return false;
    }
    ArrayRef<const std::unique_ptr<ImageSpace>> dependencies =
        ArrayRef<const std::unique_ptr<ImageSpace>>(*spaces).SubArray(
            /*pos=*/ 0u, dependency_count);
    for (size_t i = 0u, size = locations.size(); i != size; ++i) {
      ImageSpace* space = (*spaces)[spaces->size() - chunk.image_space_count + i].get();
      size_t bcp_chunk_size = (chunk.image_space_count == 1u) ? chunk.component_count : 1u;

      size_t pos = chunk.start_index + i;
      ArrayRef<File> boot_class_path_files =
          boot_class_path_files_.empty() ?
              ArrayRef<File>() :
              boot_class_path_files_.SubArray(/*pos=*/pos, bcp_chunk_size);

      // Select vdex and oat FD if any exists.
      android::base::unique_fd vdex_fd;
      android::base::unique_fd oat_fd;
      if (chunk.vdex_fd.get() >= 0) {
        DCHECK_EQ(locations.size(), 1u);
        vdex_fd = std::move(chunk.vdex_fd);
      } else {
        int arg_vdex_fd =
            pos < boot_class_path_vdex_files_.size() ? boot_class_path_vdex_files_[pos].Fd() : -1;
        if (arg_vdex_fd >= 0) {
          vdex_fd.reset(DupCloexec(arg_vdex_fd));
        }
      }
      if (chunk.oat_fd.get() >= 0) {
        DCHECK_EQ(locations.size(), 1u);
        oat_fd = std::move(chunk.oat_fd);
      } else {
        int arg_oat_fd =
            pos < boot_class_path_oat_files_.size() ? boot_class_path_oat_files_[pos].Fd() : -1;
        if (arg_oat_fd >= 0) {
          oat_fd.reset(DupCloexec(arg_oat_fd));
        }
      }

      if (!OpenOatFile(space,
                       std::move(vdex_fd),
                       std::move(oat_fd),
                       boot_class_path_.SubArray(/*pos=*/pos, bcp_chunk_size),
                       boot_class_path_files,
                       validate_oat_file,
                       dependencies,
                       logger,
                       image_reservation,
                       error_msg)) {
        return false;
      }
    }

    guard.Commit();
    return true;
  }

  MemMap ReserveBootImageMemory(uint8_t* addr,
                                uint32_t reservation_size,
                                /*out*/std::string* error_msg) {
    DCHECK_ALIGNED(reservation_size, kElfSegmentAlignment);
    DCHECK_ALIGNED(addr, kElfSegmentAlignment);
    return MemMap::MapAnonymous("Boot image reservation",
                                addr,
                                reservation_size,
                                PROT_NONE,
                                /*low_4gb=*/ true,
                                /*reuse=*/ false,
                                /*reservation=*/ nullptr,
                                error_msg);
  }

  bool RemapExtraReservation(size_t extra_reservation_size,
                             /*inout*/MemMap* image_reservation,
                             /*out*/MemMap* extra_reservation,
                             /*out*/std::string* error_msg) {
    DCHECK_ALIGNED(extra_reservation_size, kElfSegmentAlignment);
    DCHECK(!extra_reservation->IsValid());
    size_t expected_size = image_reservation->IsValid() ? image_reservation->Size() : 0u;
    if (extra_reservation_size != expected_size) {
      *error_msg = StringPrintf("Image reservation mismatch after loading boot image: %zu != %zu",
                                extra_reservation_size,
                                expected_size);
      return false;
    }
    if (extra_reservation_size != 0u) {
      DCHECK(image_reservation->IsValid());
      DCHECK_EQ(extra_reservation_size, image_reservation->Size());
      *extra_reservation = image_reservation->RemapAtEnd(image_reservation->Begin(),
                                                         "Boot image extra reservation",
                                                         PROT_NONE,
                                                         error_msg);
      if (!extra_reservation->IsValid()) {
        return false;
      }
    }
    DCHECK(!image_reservation->IsValid());
    return true;
  }

  const ArrayRef<const std::string> boot_class_path_;
  const ArrayRef<const std::string> boot_class_path_locations_;
  ArrayRef<File> boot_class_path_files_;
  ArrayRef<File> boot_class_path_image_files_;
  ArrayRef<File> boot_class_path_vdex_files_;
  ArrayRef<File> boot_class_path_oat_files_;
  const ArrayRef<const std::string> image_locations_;
  const InstructionSet image_isa_;
  const bool relocate_;
  const bool executable_;
  bool has_system_;
  const std::string* apex_versions_;
};

bool ImageSpace::BootImageLoader::LoadFromSystem(
    size_t extra_reservation_size,
    bool allow_in_memory_compilation,
    /*out*/std::vector<std::unique_ptr<ImageSpace>>* boot_image_spaces,
    /*out*/MemMap* extra_reservation,
    /*out*/std::string* error_msg) {
  TimingLogger logger(__PRETTY_FUNCTION__, /*precise=*/ true, VLOG_IS_ON(image));

  BootImageLayout layout(image_locations_,
                         boot_class_path_,
                         boot_class_path_locations_,
                         boot_class_path_files_,
                         boot_class_path_image_files_,
                         boot_class_path_vdex_files_,
                         boot_class_path_oat_files_,
                         apex_versions_);
  if (!layout.LoadFromSystem(image_isa_, allow_in_memory_compilation, error_msg)) {
    return false;
  }

  // Load the image. We don't validate oat files in this stage because they have been validated
  // before.
  if (!LoadImage(layout,
                 /*validate_oat_file=*/ false,
                 extra_reservation_size,
                 &logger,
                 boot_image_spaces,
                 extra_reservation,
                 error_msg)) {
    return false;
  }

  if (VLOG_IS_ON(image)) {
    LOG(INFO) << "ImageSpace::BootImageLoader::LoadFromSystem exiting "
        << *boot_image_spaces->front();
    logger.Dump(LOG_STREAM(INFO));
  }
  return true;
}

bool ImageSpace::IsBootClassPathOnDisk(InstructionSet image_isa) {
  Runtime* runtime = Runtime::Current();
  BootImageLayout layout(ArrayRef<const std::string>(runtime->GetImageLocations()),
                         ArrayRef<const std::string>(runtime->GetBootClassPath()),
                         ArrayRef<const std::string>(runtime->GetBootClassPathLocations()),
                         runtime->GetBootClassPathFiles(),
                         runtime->GetBootClassPathImageFiles(),
                         runtime->GetBootClassPathVdexFiles(),
                         runtime->GetBootClassPathOatFiles(),
                         &runtime->GetApexVersions());
  const std::string image_location = layout.GetPrimaryImageLocation();
  std::unique_ptr<ImageHeader> image_header;
  std::string error_msg;

  std::string system_filename;
  bool has_system = false;

  if (FindImageFilename(image_location.c_str(),
                        image_isa,
                        &system_filename,
                        &has_system)) {
    DCHECK(has_system);
    image_header = ReadSpecificImageHeader(system_filename.c_str(), &error_msg);
  }

  return image_header != nullptr;
}

bool ImageSpace::LoadBootImage(const std::vector<std::string>& boot_class_path,
                               const std::vector<std::string>& boot_class_path_locations,
                               ArrayRef<File> boot_class_path_files,
                               ArrayRef<File> boot_class_path_image_files,
                               ArrayRef<File> boot_class_path_vdex_files,
                               ArrayRef<File> boot_class_path_odex_files,
                               const std::vector<std::string>& image_locations,
                               const InstructionSet image_isa,
                               bool relocate,
                               bool executable,
                               size_t extra_reservation_size,
                               bool allow_in_memory_compilation,
                               const std::string& apex_versions,
                               /*out*/ std::vector<std::unique_ptr<ImageSpace>>* boot_image_spaces,
                               /*out*/ MemMap* extra_reservation) {
  ScopedTrace trace(__FUNCTION__);

  DCHECK(boot_image_spaces != nullptr);
  DCHECK(boot_image_spaces->empty());
  DCHECK_ALIGNED(extra_reservation_size, kElfSegmentAlignment);
  DCHECK(extra_reservation != nullptr);
  DCHECK_NE(image_isa, InstructionSet::kNone);

  if (image_locations.empty()) {
    return false;
  }

  BootImageLoader loader(boot_class_path,
                         boot_class_path_locations,
                         boot_class_path_files,
                         boot_class_path_image_files,
                         boot_class_path_vdex_files,
                         boot_class_path_odex_files,
                         image_locations,
                         image_isa,
                         relocate,
                         executable,
                         &apex_versions);
  loader.FindImageFiles();

  std::string error_msg;
  if (loader.LoadFromSystem(extra_reservation_size,
                            allow_in_memory_compilation,
                            boot_image_spaces,
                            extra_reservation,
                            &error_msg)) {
    return true;
  }
  LOG(ERROR) << "Could not create image space with image file '"
             << Join(image_locations, kComponentSeparator)
             << "'. Attempting to fall back to imageless running. Error was: "
             << error_msg;

  return false;
}

ImageSpace::~ImageSpace() {
  // Everything done by member destructors. Classes forward-declared in header are now defined.
}

std::unique_ptr<ImageSpace> ImageSpace::CreateFromAppImage(const char* image,
                                                           const OatFile* oat_file,
                                                           std::string* error_msg) {
  // Note: The oat file has already been validated.
  const std::vector<ImageSpace*>& boot_image_spaces =
      Runtime::Current()->GetHeap()->GetBootImageSpaces();
  return CreateFromAppImage(image,
                            oat_file,
                            ArrayRef<ImageSpace* const>(boot_image_spaces),
                            error_msg);
}

std::unique_ptr<ImageSpace> ImageSpace::CreateFromAppImage(
    const char* image,
    const OatFile* oat_file,
    ArrayRef<ImageSpace* const> boot_image_spaces,
    std::string* error_msg) {
  return Loader::InitAppImage(image,
                              image,
                              oat_file,
                              boot_image_spaces,
                              error_msg);
}

const OatFile* ImageSpace::GetOatFile() const {
  return oat_file_non_owned_;
}

std::unique_ptr<const OatFile> ImageSpace::ReleaseOatFile() {
  CHECK(oat_file_ != nullptr);
  return std::move(oat_file_);
}

void ImageSpace::Dump(std::ostream& os) const {
  os << GetType()
      << " begin=" << reinterpret_cast<void*>(Begin())
      << ",end=" << reinterpret_cast<void*>(End())
      << ",size=" << PrettySize(Size())
      << ",name=\"" << GetName() << "\"]";
}

bool ImageSpace::ValidateApexVersions(const OatFile& oat_file,
                                      std::string_view runtime_apex_versions,
                                      std::string* error_msg) {
  // For a boot image, the key value store only exists in the first OAT file. Skip other OAT files.
  if (oat_file.GetOatHeader().GetKeyValueStoreSize() == 0) {
    return true;
  }

  std::optional<std::string_view> oat_apex_versions = oat_file.GetApexVersions();
  if (!oat_apex_versions.has_value()) {
    *error_msg = StringPrintf("ValidateApexVersions failed to get APEX versions from oat file '%s'",
                              oat_file.GetLocation().c_str());
    return false;
  }

  return ValidateApexVersions(
      *oat_apex_versions, runtime_apex_versions, oat_file.GetLocation(), error_msg);
}

bool ImageSpace::ValidateApexVersions(std::string_view oat_apex_versions,
                                      std::string_view runtime_apex_versions,
                                      const std::string& file_location,
                                      std::string* error_msg) {
  // For a boot image, it can be generated from a subset of the bootclasspath.
  // For an app image, some dex files get compiled with a subset of the bootclasspath.
  // For such cases, the OAT APEX versions will be a prefix of the runtime APEX versions.
  if (!runtime_apex_versions.starts_with(oat_apex_versions)) {
    *error_msg = ART_FORMAT(
        "ValidateApexVersions found APEX versions mismatch between oat file '{}' and the runtime "
        "(Oat file: '{}', Runtime: '{}')",
        file_location,
        oat_apex_versions,
        runtime_apex_versions);
    return false;
  }
  return true;
}

bool ImageSpace::ValidateOatFile(const OatFile& oat_file, std::string* error_msg) {
  DCHECK(Runtime::Current() != nullptr);
  return ValidateOatFile(oat_file, error_msg, {}, {}, Runtime::Current()->GetApexVersions());
}

bool ImageSpace::ValidateOatFile(const OatFile& oat_file,
                                 std::string* error_msg,
                                 ArrayRef<const std::string> dex_filenames,
                                 ArrayRef<File> dex_files,
                                 const std::string& apex_versions) {
  if (!ValidateApexVersions(oat_file, apex_versions, error_msg)) {
    return false;
  }

  // For a boot image, the key value store only exists in the first OAT file. Skip other OAT files.
  if (oat_file.GetOatHeader().GetKeyValueStoreSize() != 0 &&
      oat_file.GetOatHeader().IsConcurrentCopying() != gUseReadBarrier) {
    *error_msg =
        ART_FORMAT("ValidateOatFile found read barrier state mismatch (oat file: {}, runtime: {})",
                   oat_file.GetOatHeader().IsConcurrentCopying(),
                   gUseReadBarrier);
    return false;
  }

  size_t dex_file_index = 0;  // Counts only primary dex files.
  const std::vector<const OatDexFile*>& oat_dex_files = oat_file.GetOatDexFiles();
  for (size_t i = 0; i < oat_dex_files.size();) {
    DCHECK(dex_filenames.empty() || dex_file_index < dex_filenames.size());
    const std::string& dex_file_location = dex_filenames.empty() ?
                                               oat_dex_files[i]->GetDexFileLocation() :
                                               dex_filenames[dex_file_index];
    File no_file;  // Invalid object.
    File& dex_file = dex_file_index < dex_files.size() ? dex_files[dex_file_index] : no_file;
    dex_file_index++;

    if (DexFileLoader::IsMultiDexLocation(oat_dex_files[i]->GetDexFileLocation())) {
      return false;  // Expected primary dex file.
    }
    uint32_t oat_checksum = DexFileLoader::GetMultiDexChecksum(oat_dex_files, &i);

    // Original checksum.
    std::optional<uint32_t> dex_checksum;
    ArtDexFileLoader dex_loader(&dex_file, dex_file_location);
    bool ok = dex_loader.GetMultiDexChecksum(&dex_checksum, error_msg);
    if (!ok) {
      *error_msg = StringPrintf(
          "ValidateOatFile failed to get checksum of dex file '%s' "
          "referenced by oat file %s: %s",
          dex_file_location.c_str(),
          oat_file.GetLocation().c_str(),
          error_msg->c_str());
      return false;
    }
    CHECK(dex_checksum.has_value());

    if (oat_checksum != dex_checksum) {
      *error_msg = StringPrintf(
          "ValidateOatFile found checksum mismatch between oat file "
          "'%s' and dex file '%s' (0x%x != 0x%x)",
          oat_file.GetLocation().c_str(),
          dex_file_location.c_str(),
          oat_checksum,
          dex_checksum.value());
      return false;
    }
  }
  return true;
}

std::string ImageSpace::GetBootClassPathChecksums(
    ArrayRef<ImageSpace* const> image_spaces,
    ArrayRef<const DexFile* const> boot_class_path) {
  DCHECK(!boot_class_path.empty());
  size_t bcp_pos = 0u;
  std::string boot_image_checksum;

  for (size_t image_pos = 0u, size = image_spaces.size(); image_pos != size; ) {
    const ImageSpace* main_space = image_spaces[image_pos];
    // Caller must make sure that the image spaces correspond to the head of the BCP.
    DCHECK_NE(main_space->oat_file_non_owned_->GetOatDexFiles().size(), 0u);
    DCHECK_EQ(main_space->oat_file_non_owned_->GetOatDexFiles()[0]->GetDexFileLocation(),
              boot_class_path[bcp_pos]->GetLocation());
    const ImageHeader& current_header = main_space->GetImageHeader();
    uint32_t image_space_count = current_header.GetImageSpaceCount();
    DCHECK_NE(image_space_count, 0u);
    DCHECK_LE(image_space_count, image_spaces.size() - image_pos);
    if (image_pos != 0u) {
      boot_image_checksum += ':';
    }
    uint32_t component_count = current_header.GetComponentCount();
    AppendImageChecksum(component_count, current_header.GetImageChecksum(), &boot_image_checksum);
    for (size_t space_index = 0; space_index != image_space_count; ++space_index) {
      const ImageSpace* space = image_spaces[image_pos + space_index];
      const OatFile* oat_file = space->oat_file_non_owned_;
      size_t num_dex_files = oat_file->GetOatDexFiles().size();
      if (kIsDebugBuild) {
        CHECK_NE(num_dex_files, 0u);
        CHECK_LE(oat_file->GetOatDexFiles().size(), boot_class_path.size() - bcp_pos);
        for (size_t i = 0; i != num_dex_files; ++i) {
          CHECK_EQ(oat_file->GetOatDexFiles()[i]->GetDexFileLocation(),
                   boot_class_path[bcp_pos + i]->GetLocation());
        }
      }
      bcp_pos += num_dex_files;
    }
    image_pos += image_space_count;
  }

  ArrayRef<const DexFile* const> boot_class_path_tail =
      ArrayRef<const DexFile* const>(boot_class_path).SubArray(bcp_pos);
  DCHECK(boot_class_path_tail.empty() ||
         !DexFileLoader::IsMultiDexLocation(boot_class_path_tail.front()->GetLocation()));
  for (size_t i = 0; i < boot_class_path_tail.size();) {
    uint32_t checksum = DexFileLoader::GetMultiDexChecksum(boot_class_path_tail, &i);
    if (!boot_image_checksum.empty()) {
      boot_image_checksum += ':';
    }
    boot_image_checksum += kDexFileChecksumPrefix;
    StringAppendF(&boot_image_checksum, "/%08x", checksum);
  }
  return boot_image_checksum;
}

size_t ImageSpace::GetNumberOfComponents(ArrayRef<ImageSpace* const> image_spaces) {
  size_t n = 0;
  for (auto&& is : image_spaces) {
    n += is->GetComponentCount();
  }
  return n;
}

size_t ImageSpace::CheckAndCountBCPComponents(std::string_view oat_boot_class_path,
                                         ArrayRef<const std::string> boot_class_path,
                                         /*out*/std::string* error_msg) {
  // Check that the oat BCP is a prefix of current BCP locations and count components.
  size_t component_count = 0u;
  std::string_view remaining_bcp(oat_boot_class_path);
  bool bcp_ok = false;
  for (const std::string& location : boot_class_path) {
    if (!remaining_bcp.starts_with(location)) {
      break;
    }
    remaining_bcp.remove_prefix(location.size());
    ++component_count;
    if (remaining_bcp.empty()) {
      bcp_ok = true;
      break;
    }
    if (!remaining_bcp.starts_with(":")) {
      break;
    }
    remaining_bcp.remove_prefix(1u);
  }
  if (!bcp_ok) {
    *error_msg = StringPrintf("Oat boot class path (%s) is not a prefix of"
                              " runtime boot class path (%s)",
                              std::string(oat_boot_class_path).c_str(),
                              Join(boot_class_path, ':').c_str());
    return static_cast<size_t>(-1);
  }
  return component_count;
}

bool ImageSpace::VerifyBootClassPathChecksums(
    std::string_view oat_checksums,
    std::string_view oat_boot_class_path,
    ArrayRef<const std::unique_ptr<ImageSpace>> image_spaces,
    ArrayRef<const std::string> boot_class_path_locations,
    ArrayRef<const std::string> boot_class_path,
    /*out*/std::string* error_msg) {
  DCHECK_EQ(boot_class_path.size(), boot_class_path_locations.size());
  DCHECK_GE(boot_class_path_locations.size(), image_spaces.size());
  if (oat_checksums.empty() || oat_boot_class_path.empty()) {
    *error_msg = oat_checksums.empty() ? "Empty checksums." : "Empty boot class path.";
    return false;
  }

  size_t oat_bcp_size =
      CheckAndCountBCPComponents(oat_boot_class_path, boot_class_path_locations, error_msg);
  if (oat_bcp_size == static_cast<size_t>(-1)) {
    DCHECK(!error_msg->empty());
    return false;
  }
  const size_t num_image_spaces = image_spaces.size();
  size_t dependency_component_count = 0;
  for (const std::unique_ptr<ImageSpace>& space : image_spaces) {
    dependency_component_count += space->GetComponentCount();
  }
  if (dependency_component_count != oat_bcp_size) {
    *error_msg = StringPrintf("Image header records %s dependencies (%zu) than BCP (%zu)",
                              dependency_component_count < oat_bcp_size ? "less" : "more",
                              dependency_component_count,
                              oat_bcp_size);
    return false;
  }

  // Verify image checksums.
  size_t bcp_pos = 0u;
  size_t image_pos = 0u;
  while (image_pos != num_image_spaces && oat_checksums.starts_with("i")) {
    // Verify the current image checksum.
    const ImageHeader& current_header = image_spaces[image_pos]->GetImageHeader();
    uint32_t image_space_count = current_header.GetImageSpaceCount();
    DCHECK_NE(image_space_count, 0u);
    DCHECK_LE(image_space_count, image_spaces.size() - image_pos);
    uint32_t component_count = current_header.GetComponentCount();
    uint32_t checksum = current_header.GetImageChecksum();
    if (!CheckAndRemoveImageChecksum(component_count, checksum, &oat_checksums, error_msg)) {
      DCHECK(!error_msg->empty());
      return false;
    }

    if (kIsDebugBuild) {
      for (size_t space_index = 0; space_index != image_space_count; ++space_index) {
        const OatFile* oat_file = image_spaces[image_pos + space_index]->oat_file_non_owned_;
        size_t num_dex_files = oat_file->GetOatDexFiles().size();
        CHECK_NE(num_dex_files, 0u);
        const std::string main_location = oat_file->GetOatDexFiles()[0]->GetDexFileLocation();
        CHECK_EQ(main_location, boot_class_path_locations[bcp_pos + space_index]);
        CHECK(!DexFileLoader::IsMultiDexLocation(main_location));
        size_t num_base_locations = 1u;
        for (size_t i = 1u; i != num_dex_files; ++i) {
          if (!DexFileLoader::IsMultiDexLocation(
                  oat_file->GetOatDexFiles()[i]->GetDexFileLocation())) {
            CHECK_EQ(image_space_count, 1u);  // We can find base locations only for --single-image.
            ++num_base_locations;
          }
        }
        if (image_space_count == 1u) {
          CHECK_EQ(num_base_locations, component_count);
        }
      }
    }

    image_pos += image_space_count;
    bcp_pos += component_count;

    if (!oat_checksums.starts_with(":")) {
      // Check that we've reached the end of checksums and BCP.
      if (!oat_checksums.empty()) {
         *error_msg = StringPrintf("Expected ':' separator or end of checksums, remaining %s.",
                                   std::string(oat_checksums).c_str());
         return false;
      }
      if (bcp_pos != oat_bcp_size) {
        *error_msg = StringPrintf("Component count mismatch between checksums (%zu) and BCP (%zu)",
                                  bcp_pos,
                                  oat_bcp_size);
        return false;
      }
      return true;
    }
    oat_checksums.remove_prefix(1u);
  }

  // We do not allow dependencies of extensions on dex files. That would require
  // interleaving the loading of the images with opening the other BCP dex files.
  return false;
}

std::vector<std::string> ImageSpace::ExpandMultiImageLocations(
    ArrayRef<const std::string> dex_locations,
    const std::string& image_location,
    bool boot_image_extension) {
  DCHECK(!dex_locations.empty());

  // Find the path.
  size_t last_slash = image_location.rfind('/');
  CHECK_NE(last_slash, std::string::npos);

  // We also need to honor path components that were encoded through '@'. Otherwise the loading
  // code won't be able to find the images.
  if (image_location.find('@', last_slash) != std::string::npos) {
    last_slash = image_location.rfind('@');
  }

  // Find the dot separating the primary image name from the extension.
  size_t last_dot = image_location.rfind('.');
  // Extract the extension and base (the path and primary image name).
  std::string extension;
  std::string base = image_location;
  if (last_dot != std::string::npos && last_dot > last_slash) {
    extension = image_location.substr(last_dot);  // Including the dot.
    base.resize(last_dot);
  }
  // For non-empty primary image name, add '-' to the `base`.
  if (last_slash + 1u != base.size()) {
    base += '-';
  }

  std::vector<std::string> locations;
  locations.reserve(dex_locations.size());
  size_t start_index = 0u;
  if (!boot_image_extension) {
    start_index = 1u;
    locations.push_back(image_location);
  }

  // Now create the other names. Use a counted loop to skip the first one if needed.
  for (size_t i = start_index; i < dex_locations.size(); ++i) {
    // Replace path with `base` (i.e. image path and prefix) and replace the original
    // extension (if any) with `extension`.
    std::string name = dex_locations[i];
    size_t last_dex_slash = name.rfind('/');
    if (last_dex_slash != std::string::npos) {
      name = name.substr(last_dex_slash + 1);
    }
    size_t last_dex_dot = name.rfind('.');
    if (last_dex_dot != std::string::npos) {
      name.resize(last_dex_dot);
    }
    locations.push_back(ART_FORMAT("{}{}{}", base, name, extension));
  }
  return locations;
}

void ImageSpace::DumpSections(std::ostream& os) const {
  const uint8_t* base = Begin();
  const ImageHeader& header = GetImageHeader();
  for (size_t i = 0; i < ImageHeader::kSectionCount; ++i) {
    auto section_type = static_cast<ImageHeader::ImageSections>(i);
    const ImageSection& section = header.GetImageSection(section_type);
    os << section_type << " " << reinterpret_cast<const void*>(base + section.Offset())
       << "-" << reinterpret_cast<const void*>(base + section.End()) << "\n";
  }
}

void ImageSpace::ReleaseMetadata() {
  const ImageSection& metadata = GetImageHeader().GetMetadataSection();
  VLOG(image) << "Releasing " << metadata.Size() << " image metadata bytes";
  // Avoid using ZeroAndReleasePages since the zero fill might not be word atomic.
  uint8_t* const page_begin = AlignUp(Begin() + metadata.Offset(), gPageSize);
  uint8_t* const page_end = AlignDown(Begin() + metadata.End(), gPageSize);
  if (page_begin < page_end) {
    CHECK_NE(madvise(page_begin, page_end - page_begin, MADV_DONTNEED), -1) << "madvise failed";
  }
}

}  // namespace space
}  // namespace gc
}  // namespace art
