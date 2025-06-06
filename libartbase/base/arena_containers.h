/*
 * Copyright (C) 2014 The Android Open Source Project
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

#ifndef ART_LIBARTBASE_BASE_ARENA_CONTAINERS_H_
#define ART_LIBARTBASE_BASE_ARENA_CONTAINERS_H_

#include <deque>
#include <forward_list>
#include <list>
#include <queue>
#include <set>
#include <stack>
#include <unordered_map>
#include <utility>

#include "arena_allocator.h"
#include "dchecked_vector.h"
#include "hash_map.h"
#include "hash_set.h"
#include "safe_map.h"

namespace art {

// Adapter for use of ArenaAllocator in STL containers.
// Use ArenaAllocator::Adapter() to create an adapter to pass to container constructors.
// For example,
//   struct Foo {
//     explicit Foo(ArenaAllocator* allocator)
//         : foo_vector(allocator->Adapter(kArenaAllocMisc)),
//           foo_map(std::less<int>(), allocator->Adapter()) {
//     }
//     ArenaVector<int> foo_vector;
//     ArenaSafeMap<int, int> foo_map;
//   };
template <typename T>
class ArenaAllocatorAdapter;

template <typename T>
using ArenaDeque = std::deque<T, ArenaAllocatorAdapter<T>>;

template <typename T>
using ArenaForwardList = std::forward_list<T, ArenaAllocatorAdapter<T>>;

template <typename T>
using ArenaList = std::list<T, ArenaAllocatorAdapter<T>>;

template <typename T>
using ArenaQueue = std::queue<T, ArenaDeque<T>>;

template <typename T>
using ArenaVector = dchecked_vector<T, ArenaAllocatorAdapter<T>>;

template <typename T, typename Comparator = std::less<T>>
using ArenaPriorityQueue = std::priority_queue<T, ArenaVector<T>, Comparator>;

template <typename T>
using ArenaStdStack = std::stack<T, ArenaDeque<T>>;

template <typename T, typename Comparator = std::less<T>>
using ArenaSet = std::set<T, Comparator, ArenaAllocatorAdapter<T>>;

template <typename K, typename V, typename Comparator = std::less<K>>
using ArenaSafeMap =
    SafeMap<K, V, Comparator, ArenaAllocatorAdapter<std::pair<const K, V>>>;

template <typename T,
          typename EmptyFn = DefaultEmptyFn<T>,
          typename HashFn = DefaultHashFn<T>,
          typename Pred = DefaultPred<T>>
using ArenaHashSet = HashSet<T, EmptyFn, HashFn, Pred, ArenaAllocatorAdapter<T>>;

template <typename Key,
          typename Value,
          typename EmptyFn = DefaultEmptyFn<std::pair<Key, Value>>,
          typename HashFn = DefaultHashFn<Key>,
          typename Pred = DefaultPred<Key>>
using ArenaHashMap = HashMap<Key,
                             Value,
                             EmptyFn,
                             HashFn,
                             Pred,
                             ArenaAllocatorAdapter<std::pair<Key, Value>>>;

template <typename Key,
          typename Value,
          typename Hash = std::hash<Key>,
          typename Pred = std::equal_to<Key>>
using ArenaUnorderedMap = std::unordered_map<Key,
                                             Value,
                                             Hash,
                                             Pred,
                                             ArenaAllocatorAdapter<std::pair<const Key, Value>>>;

// Implementation details below.

template <bool kCount>
class ArenaAllocatorAdapterKindImpl;

template <>
class ArenaAllocatorAdapterKindImpl<false> {
 public:
  // Not tracking allocations, ignore the supplied kind and arbitrarily provide kArenaAllocSTL.
  explicit ArenaAllocatorAdapterKindImpl([[maybe_unused]] ArenaAllocKind kind) {}
  ArenaAllocatorAdapterKindImpl(const ArenaAllocatorAdapterKindImpl&) = default;
  ArenaAllocatorAdapterKindImpl& operator=(const ArenaAllocatorAdapterKindImpl&) = default;
  ArenaAllocKind Kind() { return kArenaAllocSTL; }
};

template <bool kCount>
class ArenaAllocatorAdapterKindImpl {
 public:
  explicit ArenaAllocatorAdapterKindImpl(ArenaAllocKind kind) : kind_(kind) { }
  ArenaAllocatorAdapterKindImpl(const ArenaAllocatorAdapterKindImpl&) = default;
  ArenaAllocatorAdapterKindImpl& operator=(const ArenaAllocatorAdapterKindImpl&) = default;
  ArenaAllocKind Kind() { return kind_; }

 private:
  ArenaAllocKind kind_;
};

using ArenaAllocatorAdapterKind = ArenaAllocatorAdapterKindImpl<kArenaAllocatorCountAllocations>;

template <>
class ArenaAllocatorAdapter<void> : private ArenaAllocatorAdapterKind {
 public:
  using value_type    = void;
  using pointer       = void*;
  using const_pointer = const void*;

  template <typename U>
  struct rebind {
    using other = ArenaAllocatorAdapter<U>;
  };

  explicit ArenaAllocatorAdapter(ArenaAllocator* allocator,
                                 ArenaAllocKind kind = kArenaAllocSTL)
      : ArenaAllocatorAdapterKind(kind),
        allocator_(allocator) {
  }
  template <typename U>
  ArenaAllocatorAdapter(const ArenaAllocatorAdapter<U>& other)
      : ArenaAllocatorAdapterKind(other),
        allocator_(other.allocator_) {
  }
  ArenaAllocatorAdapter(const ArenaAllocatorAdapter&) = default;
  ArenaAllocatorAdapter& operator=(const ArenaAllocatorAdapter&) = default;
  ~ArenaAllocatorAdapter() = default;

 private:
  ArenaAllocator* allocator_;

  template <typename U>
  friend class ArenaAllocatorAdapter;
};

template <typename T>
class ArenaAllocatorAdapter : private ArenaAllocatorAdapterKind {
 public:
  using value_type      = T;
  using pointer         = T*;
  using reference       = T&;
  using const_pointer   = const T*;
  using const_reference = const T&;
  using size_type       = size_t;
  using difference_type = ptrdiff_t;

  template <typename U>
  struct rebind {
    using other = ArenaAllocatorAdapter<U>;
  };

  ArenaAllocatorAdapter(ArenaAllocator* allocator, ArenaAllocKind kind)
      : ArenaAllocatorAdapterKind(kind),
        allocator_(allocator) {
  }
  template <typename U>
  ArenaAllocatorAdapter(const ArenaAllocatorAdapter<U>& other)
      : ArenaAllocatorAdapterKind(other),
        allocator_(other.allocator_) {
  }
  ArenaAllocatorAdapter(const ArenaAllocatorAdapter&) = default;
  ArenaAllocatorAdapter& operator=(const ArenaAllocatorAdapter&) = default;
  ~ArenaAllocatorAdapter() = default;

  size_type max_size() const {
    return static_cast<size_type>(-1) / sizeof(T);
  }

  pointer address(reference x) const { return &x; }
  const_pointer address(const_reference x) const { return &x; }

  pointer allocate(size_type n,
                   [[maybe_unused]] ArenaAllocatorAdapter<void>::pointer hint = nullptr) {
    DCHECK_LE(n, max_size());
    return allocator_->AllocArray<T>(n, ArenaAllocatorAdapterKind::Kind());
  }
  void deallocate(pointer p, size_type n) {
    allocator_->MakeInaccessible(p, sizeof(T) * n);
  }

  template <typename U, typename... Args>
  void construct(U* p, Args&&... args) {
    ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
  }
  template <typename U>
  void destroy(U* p) {
    p->~U();
  }

 private:
  ArenaAllocator* allocator_;

  template <typename U>
  friend class ArenaAllocatorAdapter;

  template <typename U>
  friend bool operator==(const ArenaAllocatorAdapter<U>& lhs,
                         const ArenaAllocatorAdapter<U>& rhs);
};

template <typename T>
inline bool operator==(const ArenaAllocatorAdapter<T>& lhs,
                       const ArenaAllocatorAdapter<T>& rhs) {
  return lhs.allocator_ == rhs.allocator_;
}

template <typename T>
inline bool operator!=(const ArenaAllocatorAdapter<T>& lhs,
                       const ArenaAllocatorAdapter<T>& rhs) {
  return !(lhs == rhs);
}

inline ArenaAllocatorAdapter<void> ArenaAllocator::Adapter(ArenaAllocKind kind) {
  return ArenaAllocatorAdapter<void>(this, kind);
}

// Special deleter that only calls the destructor. Also checks for double free errors.
template <typename T>
class ArenaDelete {
  static constexpr uint8_t kMagicFill = 0xCE;

 protected:
  // Used for variable sized objects such as RegisterLine.
  ALWAYS_INLINE void ProtectMemory(T* ptr, size_t size) const {
    if (kRunningOnMemoryTool) {
      memset(ptr, kMagicFill, size);
      MEMORY_TOOL_MAKE_NOACCESS(ptr, size);
    } else if (kIsDebugBuild) {
      // Write a magic value to try and catch use after free errors.
      memset(ptr, kMagicFill, size);
    }
  }

 public:
  void operator()(T* ptr) const {
    if (ptr != nullptr) {
      ptr->~T();
      ProtectMemory(ptr, sizeof(T));
    }
  }
};

// In general we lack support for arrays. We would need to call the destructor on each element,
// which requires access to the array size. Support for that is future work.
//
// However, we can support trivially destructible component types, as then a destructor doesn't
// need to be called.
template <typename T>
class ArenaDelete<T[]> {
 public:
  void operator()([[maybe_unused]] T* ptr) const {
    static_assert(std::is_trivially_destructible_v<T>,
                  "ArenaUniquePtr does not support non-trivially-destructible arrays.");
    // TODO: Implement debug checks, and MEMORY_TOOL support.
  }
};

// Arena unique ptr that only calls the destructor of the element.
template <typename T>
using ArenaUniquePtr = std::unique_ptr<T, ArenaDelete<T>>;

}  // namespace art

#endif  // ART_LIBARTBASE_BASE_ARENA_CONTAINERS_H_
