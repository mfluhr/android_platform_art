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

#ifndef ART_RUNTIME_INTERN_TABLE_INL_H_
#define ART_RUNTIME_INTERN_TABLE_INL_H_

#include "intern_table.h"

#include "dex/utf.h"
#include "gc/space/image_space.h"
#include "gc_root-inl.h"
#include "mirror/string-inl.h"
#include "oat/image.h"
#include "thread-current-inl.h"

namespace art HIDDEN {

ALWAYS_INLINE
inline uint32_t InternTable::Utf8String::Hash(uint32_t utf16_length, const char* utf8_data) {
  DCHECK_EQ(utf16_length, CountModifiedUtf8Chars(utf8_data));
  if (LIKELY(utf8_data[utf16_length] == 0)) {
    int32_t hash = ComputeUtf16Hash(utf8_data, utf16_length);
    DCHECK_EQ(hash, ComputeUtf16HashFromModifiedUtf8(utf8_data, utf16_length));
    return hash;
  } else {
    return ComputeUtf16HashFromModifiedUtf8(utf8_data, utf16_length);
  }
}

ALWAYS_INLINE
inline size_t InternTable::StringHash::operator()(const GcRoot<mirror::String>& root) const {
  if (kIsDebugBuild) {
    Locks::mutator_lock_->AssertSharedHeld(Thread::Current());
  }
  ObjPtr<mirror::String> s = root.Read<kWithoutReadBarrier>();
  int32_t hash = s->GetStoredHashCode();
  DCHECK_EQ(hash, s->ComputeHashCode());
  // An additional cast to prevent undesired sign extension.
  return static_cast<uint32_t>(hash);
}

ALWAYS_INLINE
inline bool InternTable::StringEquals::operator()(const GcRoot<mirror::String>& a,
                                                  const GcRoot<mirror::String>& b) const {
  if (kIsDebugBuild) {
    Locks::mutator_lock_->AssertSharedHeld(Thread::Current());
  }
  return a.Read<kWithoutReadBarrier>()->Equals(b.Read<kWithoutReadBarrier>());
}

ALWAYS_INLINE
inline bool InternTable::StringEquals::operator()(const GcRoot<mirror::String>& a,
                                                  const Utf8String& b) const {
  if (kIsDebugBuild) {
    Locks::mutator_lock_->AssertSharedHeld(Thread::Current());
  }
  ObjPtr<mirror::String> a_string = a.Read<kWithoutReadBarrier>();
  uint32_t a_length = static_cast<uint32_t>(a_string->GetLength());
  if (a_length != b.GetUtf16Length()) {
    return false;
  }
  DCHECK_GE(strlen(b.GetUtf8Data()), a_length);
  if (a_string->IsCompressed()) {
    // Modified UTF-8 single byte character range is 0x01 .. 0x7f.
    // The string compression occurs on regular ASCII with same exact range,
    // not on extended ASCII which is up to 0xff.
    return b.GetUtf8Data()[a_length] == 0 &&
           memcmp(b.GetUtf8Data(), a_string->GetValueCompressed(), a_length * sizeof(uint8_t)) == 0;
  } else if (mirror::kUseStringCompression && b.GetUtf8Data()[a_length] == 0) {
    // ASCII string `b` cannot equal non-ASCII `a_string`.
    return false;
  } else {
    const uint16_t* a_value = a_string->GetValue();
    return CompareModifiedUtf8ToUtf16AsCodePointValues(b.GetUtf8Data(), a_value, a_length) == 0;
  }
}

template <typename Visitor>
inline void InternTable::AddImageStringsToTable(gc::space::ImageSpace* image_space,
                                                const Visitor& visitor) {
  DCHECK(image_space != nullptr);
  // Only add if we have the interned strings section.
  const ImageHeader& header = image_space->GetImageHeader();
  const ImageSection& section = header.GetInternedStringsSection();
  if (section.Size() > 0) {
    AddTableFromMemory(image_space->Begin() + section.Offset(), visitor, !header.IsAppImage());
  }
}

template <typename Visitor>
inline size_t InternTable::AddTableFromMemory(const uint8_t* ptr,
                                              const Visitor& visitor,
                                              bool is_boot_image) {
  size_t read_count = 0;
  UnorderedSet set(ptr, /*make copy*/false, &read_count);
  {
    // Hold the lock while calling the visitor to prevent possible race
    // conditions with another thread adding intern strings.
    MutexLock mu(Thread::Current(), *Locks::intern_table_lock_);
    // Visit the unordered set, may remove elements.
    visitor(set);
    if (!set.empty()) {
      strong_interns_.AddInternStrings(std::move(set), is_boot_image);
    }
  }
  return read_count;
}

inline void InternTable::Table::AddInternStrings(UnorderedSet&& intern_strings,
                                                 bool is_boot_image) {
  if (kIsDebugBuild) {
    // Avoid doing read barriers since the space might not yet be added to the heap.
    // See b/117803941
    for (GcRoot<mirror::String>& string : intern_strings) {
      ObjPtr<mirror::String> s = string.Read<kWithoutReadBarrier>();
      uint32_t hash = static_cast<uint32_t>(s->GetStoredHashCode());
      CHECK_EQ(hash, static_cast<uint32_t>(s->ComputeHashCode()));
      CHECK(Find(s, hash) == nullptr)
          << "Already found " << string.Read<kWithoutReadBarrier>()->ToModifiedUtf8()
          << " in the intern table";
    }
  }

  // Insert before the last (unfrozen) table since we add new interns into the back.
  // Keep the order of previous frozen tables unchanged, so that we can can remember
  // the number of searched frozen tables and not search them again.
  DCHECK(!tables_.empty());
  tables_.insert(tables_.end() - 1, InternalTable(std::move(intern_strings), is_boot_image));
}

template <typename Visitor>
inline void InternTable::VisitInterns(const Visitor& visitor,
                                      bool visit_boot_images,
                                      bool visit_non_boot_images) {
  auto visit_tables = [&](dchecked_vector<Table::InternalTable>& tables)
      NO_THREAD_SAFETY_ANALYSIS {
    for (Table::InternalTable& table : tables) {
      // Determine if we want to visit the table based on the flags.
      const bool visit = table.IsBootImage() ? visit_boot_images : visit_non_boot_images;
      if (visit) {
        for (auto& intern : table.set_) {
          visitor(intern);
        }
      }
    }
  };
  visit_tables(strong_interns_.tables_);
  visit_tables(weak_interns_.tables_);
}

inline size_t InternTable::CountInterns(bool visit_boot_images, bool visit_non_boot_images) const {
  size_t ret = 0u;
  auto visit_tables = [&](const dchecked_vector<Table::InternalTable>& tables)
      NO_THREAD_SAFETY_ANALYSIS {
    for (const Table::InternalTable& table : tables) {
      // Determine if we want to visit the table based on the flags.
      const bool visit = table.IsBootImage() ? visit_boot_images : visit_non_boot_images;
      if (visit) {
        ret += table.set_.size();
      }
    }
  };
  visit_tables(strong_interns_.tables_);
  visit_tables(weak_interns_.tables_);
  return ret;
}

}  // namespace art

#endif  // ART_RUNTIME_INTERN_TABLE_INL_H_
