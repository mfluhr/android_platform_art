/*
 * Copyright (C) 2016 The Android Open Source Project
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

#ifndef ART_RUNTIME_IMTABLE_H_
#define ART_RUNTIME_IMTABLE_H_

#include "base/bit_utils.h"
#include "base/casts.h"
#include "base/locks.h"
#include "base/macros.h"
#include "base/pointer_size.h"

namespace art HIDDEN {

class ArtMethod;
class DexFile;

class ImTable {
 public:
  // Interface method table size. Increasing this value reduces the chance of two interface methods
  // colliding in the interface method table but increases the size of classes that implement
  // (non-marker) interfaces.
  // When this value changes, old images become incompatible, so image file version must change too.
  static constexpr size_t kSize = 43;
  // Default methods cannot store the imt_index, so instead we make its IMT index depend on the
  // method_index and mask it with the closest power of 2 of kSize - 1. This
  // is to simplify fetching it in the interpreter.
  static constexpr size_t kSizeTruncToPowerOfTwo = TruncToPowerOfTwo(kSize);

  uint8_t* AddressOfElement(size_t index, PointerSize pointer_size) {
    return reinterpret_cast<uint8_t*>(this) + OffsetOfElement(index, pointer_size);
  }

  ArtMethod* Get(size_t index, PointerSize pointer_size) {
    DCHECK_LT(index, kSize);
    uint8_t* ptr = AddressOfElement(index, pointer_size);
    if (pointer_size == PointerSize::k32) {
      uint32_t value = *reinterpret_cast<uint32_t*>(ptr);
      return reinterpret_cast32<ArtMethod*>(value);
    } else {
      uint64_t value = *reinterpret_cast<uint64_t*>(ptr);
      return reinterpret_cast64<ArtMethod*>(value);
    }
  }

  void Set(size_t index, ArtMethod* method, PointerSize pointer_size) {
    DCHECK_LT(index, kSize);
    uint8_t* ptr = AddressOfElement(index, pointer_size);
    if (pointer_size == PointerSize::k32) {
      *reinterpret_cast<uint32_t*>(ptr) = reinterpret_cast32<uint32_t>(method);
    } else {
      *reinterpret_cast<uint64_t*>(ptr) = reinterpret_cast64<uint64_t>(method);
    }
  }

  static size_t OffsetOfElement(size_t index, PointerSize pointer_size) {
    return index * static_cast<size_t>(pointer_size);
  }

  void Populate(ArtMethod** data, PointerSize pointer_size) {
    for (size_t i = 0; i < kSize; ++i) {
      Set(i, data[i], pointer_size);
    }
  }

  constexpr static size_t SizeInBytes(PointerSize pointer_size) {
    return kSize * static_cast<size_t>(pointer_size);
  }

  // Converts a method to the base hash components used in GetImtIndex.
  ALWAYS_INLINE static inline void GetImtHashComponents(const DexFile& dex_file,
                                                        uint32_t dex_method_index,
                                                        uint32_t* class_hash,
                                                        uint32_t* name_hash,
                                                        uint32_t* signature_hash);

  ALWAYS_INLINE static inline uint32_t GetImtIndexForAbstractMethod(const DexFile& dex_file,
                                                                    uint32_t dex_method_index);

  // The (complete) hashing scheme to map an ArtMethod to a slot in the Interface Method Table
  // (IMT).
  ALWAYS_INLINE static inline uint32_t GetImtIndex(ArtMethod* method)
      REQUIRES_SHARED(Locks::mutator_lock_);
};

}  // namespace art

#endif  // ART_RUNTIME_IMTABLE_H_

