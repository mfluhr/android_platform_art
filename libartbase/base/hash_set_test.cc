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

#include "hash_set.h"

#include <forward_list>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

#include "hash_map.h"

namespace art {

struct IsEmptyFnString {
  void MakeEmpty(std::string& item) const {
    item.clear();
  }
  bool IsEmpty(const std::string& item) const {
    return item.empty();
  }
};

class HashSetTest : public ::testing::Test {
 public:
  HashSetTest() : seed_(97421), unique_number_(0) {
  }
  std::string RandomString(size_t len) {
    std::ostringstream oss;
    for (size_t i = 0; i < len; ++i) {
      oss << static_cast<char>('A' + PRand() % 64);
    }
    static_assert(' ' < 'A', "space must be less than a");
    oss << " " << unique_number_++;  // Relies on ' ' < 'A'
    return oss.str();
  }
  void SetSeed(size_t seed) {
    seed_ = seed;
  }
  size_t PRand() {  // Pseudo random.
    seed_ = seed_ * 1103515245 + 12345;
    return seed_;
  }

 private:
  size_t seed_;
  size_t unique_number_;
};

TEST_F(HashSetTest, TestSmoke) {
  HashSet<std::string, IsEmptyFnString> hash_set;
  const std::string test_string = "hello world 1234";
  ASSERT_TRUE(hash_set.empty());
  ASSERT_EQ(hash_set.size(), 0U);
  hash_set.insert(test_string);
  auto it = hash_set.find(test_string);
  ASSERT_EQ(*it, test_string);
  auto after_it = hash_set.erase(it);
  ASSERT_TRUE(after_it == hash_set.end());
  ASSERT_TRUE(hash_set.empty());
  ASSERT_EQ(hash_set.size(), 0U);
  it = hash_set.find(test_string);
  ASSERT_TRUE(it == hash_set.end());
}

TEST_F(HashSetTest, TestInsertAndErase) {
  HashSet<std::string, IsEmptyFnString> hash_set;
  static constexpr size_t count = 1000;
  std::vector<std::string> strings;
  for (size_t i = 0; i < count; ++i) {
    // Insert a bunch of elements and make sure we can find them.
    strings.push_back(RandomString(10));
    hash_set.insert(strings[i]);
    auto it = hash_set.find(strings[i]);
    ASSERT_TRUE(it != hash_set.end());
    ASSERT_EQ(*it, strings[i]);
  }
  ASSERT_EQ(strings.size(), hash_set.size());
  // Try to erase the odd strings.
  for (size_t i = 1; i < count; i += 2) {
    auto it = hash_set.find(strings[i]);
    ASSERT_TRUE(it != hash_set.end());
    ASSERT_EQ(*it, strings[i]);
    hash_set.erase(it);
  }
  // Test removed.
  for (size_t i = 1; i < count; i += 2) {
    auto it = hash_set.find(strings[i]);
    ASSERT_TRUE(it == hash_set.end());
  }
  for (size_t i = 0; i < count; i += 2) {
    auto it = hash_set.find(strings[i]);
    ASSERT_TRUE(it != hash_set.end());
    ASSERT_EQ(*it, strings[i]);
  }
}

TEST_F(HashSetTest, TestIterator) {
  HashSet<std::string, IsEmptyFnString> hash_set;
  ASSERT_TRUE(hash_set.begin() == hash_set.end());
  static constexpr size_t count = 1000;
  std::vector<std::string> strings;
  for (size_t i = 0; i < count; ++i) {
    // Insert a bunch of elements and make sure we can find them.
    strings.push_back(RandomString(10));
    hash_set.insert(strings[i]);
  }
  // Make sure we visit each string exactly once.
  std::map<std::string, size_t> found_count;
  for (const std::string& s : hash_set) {
    ++found_count[s];
  }
  for (size_t i = 0; i < count; ++i) {
    ASSERT_EQ(found_count[strings[i]], 1U);
  }
  found_count.clear();
  // Remove all the elements with iterator erase.
  for (auto it = hash_set.begin(); it != hash_set.end();) {
    ++found_count[*it];
    it = hash_set.erase(it);
    ASSERT_EQ(hash_set.Verify(), 0U);
  }
  for (size_t i = 0; i < count; ++i) {
    ASSERT_EQ(found_count[strings[i]], 1U);
  }
}

TEST_F(HashSetTest, TestSwap) {
  HashSet<std::string, IsEmptyFnString> hash_seta, hash_setb;
  std::vector<std::string> strings;
  static constexpr size_t count = 1000;
  for (size_t i = 0; i < count; ++i) {
    strings.push_back(RandomString(10));
    hash_seta.insert(strings[i]);
  }
  std::swap(hash_seta, hash_setb);
  hash_seta.insert("TEST");
  hash_setb.insert("TEST2");
  for (size_t i = 0; i < count; ++i) {
    strings.push_back(RandomString(10));
    hash_seta.insert(strings[i]);
  }
}

TEST_F(HashSetTest, TestShrink) {
  HashSet<std::string, IsEmptyFnString> hash_set;
  std::vector<std::string> strings = {"a", "b", "c", "d", "e", "f", "g"};
  for (size_t i = 0; i < strings.size(); ++i) {
    // Insert some strings into the beginning of our hash set to establish an initial size
    hash_set.insert(strings[i]);
  }

  hash_set.ShrinkToMaximumLoad();
  const double initial_load = hash_set.CalculateLoadFactor();

  // Insert a bunch of random strings to guarantee that we grow the capacity.
  std::vector<std::string> random_strings;
  static constexpr size_t count = 1000;
  for (size_t i = 0; i < count; ++i) {
    random_strings.push_back(RandomString(10));
    hash_set.insert(random_strings[i]);
  }

  // Erase all the extra strings which guarantees that our load factor will be really bad.
  for (size_t i = 0; i < count; ++i) {
    hash_set.erase(hash_set.find(random_strings[i]));
  }

  const double bad_load = hash_set.CalculateLoadFactor();
  EXPECT_GT(initial_load, bad_load);

  // Shrink again, the load factor should be good again.
  hash_set.ShrinkToMaximumLoad();
  EXPECT_DOUBLE_EQ(initial_load, hash_set.CalculateLoadFactor());

  // Make sure all the initial elements we had are still there
  for (const std::string& initial_string : strings) {
    EXPECT_NE(hash_set.end(), hash_set.find(initial_string))
        << "expected to find " << initial_string;
  }
}

TEST_F(HashSetTest, TestLoadFactor) {
  HashSet<std::string, IsEmptyFnString> hash_set;
  static constexpr size_t kStringCount = 1000;
  static constexpr double kEpsilon = 0.01;
  for (size_t i = 0; i < kStringCount; ++i) {
    hash_set.insert(RandomString(i % 10 + 1));
  }
  // Check that changing the load factor resizes the table to be within the target range.
  EXPECT_GE(hash_set.CalculateLoadFactor() + kEpsilon, hash_set.GetMinLoadFactor());
  EXPECT_LE(hash_set.CalculateLoadFactor() - kEpsilon, hash_set.GetMaxLoadFactor());
  hash_set.SetLoadFactor(0.1, 0.3);
  EXPECT_DOUBLE_EQ(0.1, hash_set.GetMinLoadFactor());
  EXPECT_DOUBLE_EQ(0.3, hash_set.GetMaxLoadFactor());
  EXPECT_LE(hash_set.CalculateLoadFactor() - kEpsilon, hash_set.GetMaxLoadFactor());
  hash_set.SetLoadFactor(0.6, 0.8);
  EXPECT_LE(hash_set.CalculateLoadFactor() - kEpsilon, hash_set.GetMaxLoadFactor());
}

TEST_F(HashSetTest, TestStress) {
  HashSet<std::string, IsEmptyFnString> hash_set;
  std::unordered_set<std::string> std_set;
  std::vector<std::string> strings;
  static constexpr size_t string_count = 2000;
  static constexpr size_t operations = 100000;
  static constexpr size_t target_size = 5000;
  for (size_t i = 0; i < string_count; ++i) {
    strings.push_back(RandomString(i % 10 + 1));
  }
  const size_t seed = time(nullptr);
  SetSeed(seed);
  LOG(INFO) << "Starting stress test with seed " << seed;
  for (size_t i = 0; i < operations; ++i) {
    ASSERT_EQ(hash_set.size(), std_set.size());
    size_t delta = std::abs(static_cast<ssize_t>(target_size) -
                            static_cast<ssize_t>(hash_set.size()));
    size_t n = PRand();
    if (n % target_size == 0) {
      hash_set.clear();
      std_set.clear();
      ASSERT_TRUE(hash_set.empty());
      ASSERT_TRUE(std_set.empty());
    } else  if (n % target_size < delta) {
      // Skew towards adding elements until we are at the desired size.
      const std::string& s = strings[PRand() % string_count];
      hash_set.insert(s);
      std_set.insert(s);
      ASSERT_EQ(*hash_set.find(s), *std_set.find(s));
    } else {
      const std::string& s = strings[PRand() % string_count];
      auto it1 = hash_set.find(s);
      auto it2 = std_set.find(s);
      ASSERT_EQ(it1 == hash_set.end(), it2 == std_set.end());
      if (it1 != hash_set.end()) {
        ASSERT_EQ(*it1, *it2);
        hash_set.erase(it1);
        std_set.erase(it2);
      }
    }
  }
}

struct IsEmptyStringPair {
  void MakeEmpty(std::pair<std::string, int>& pair) const {
    pair.first.clear();
  }
  bool IsEmpty(const std::pair<std::string, int>& pair) const {
    return pair.first.empty();
  }
};

TEST_F(HashSetTest, TestHashMap) {
  HashMap<std::string, int, IsEmptyStringPair> hash_map;
  hash_map.insert(std::make_pair(std::string("abcd"), 123));
  hash_map.insert(std::make_pair(std::string("abcd"), 124));
  hash_map.insert(std::make_pair(std::string("bags"), 444));
  auto it = hash_map.find(std::string("abcd"));
  ASSERT_EQ(it->second, 123);
  hash_map.erase(it);
  it = hash_map.find(std::string("abcd"));
  ASSERT_EQ(it, hash_map.end());
}

struct IsEmptyFnVectorInt {
  void MakeEmpty(std::vector<int>& item) const {
    item.clear();
  }
  bool IsEmpty(const std::vector<int>& item) const {
    return item.empty();
  }
};

template <typename T>
size_t HashIntSequence(T begin, T end) {
  size_t hash = 0;
  for (auto iter = begin; iter != end; ++iter) {
    hash = hash * 2 + *iter;
  }
  return hash;
}

struct VectorIntHashEquals {
  std::size_t operator()(const std::vector<int>& item) const {
    return HashIntSequence(item.begin(), item.end());
  }

  std::size_t operator()(const std::forward_list<int>& item) const {
    return HashIntSequence(item.begin(), item.end());
  }

  bool operator()(const std::vector<int>& a, const std::vector<int>& b) const {
    return a == b;
  }

  bool operator()(const std::vector<int>& a, const std::forward_list<int>& b) const {
    auto aiter = a.begin();
    auto biter = b.begin();
    while (aiter != a.end() && biter != b.end()) {
      if (*aiter != *biter) {
        return false;
      }
      aiter++;
      biter++;
    }
    return (aiter == a.end() && biter == b.end());
  }
};

TEST_F(HashSetTest, TestLookupByAlternateKeyType) {
  HashSet<std::vector<int>, IsEmptyFnVectorInt, VectorIntHashEquals, VectorIntHashEquals> hash_set;
  hash_set.insert(std::vector<int>({1, 2, 3, 4}));
  hash_set.insert(std::vector<int>({4, 2}));
  ASSERT_EQ(hash_set.end(), hash_set.find(std::vector<int>({1, 1, 1, 1})));
  ASSERT_NE(hash_set.end(), hash_set.find(std::vector<int>({1, 2, 3, 4})));
  ASSERT_EQ(hash_set.end(), hash_set.find(std::forward_list<int>({1, 1, 1, 1})));
  ASSERT_NE(hash_set.end(), hash_set.find(std::forward_list<int>({1, 2, 3, 4})));
}

TEST_F(HashSetTest, TestReserve) {
  HashSet<std::string, IsEmptyFnString> hash_set;
  std::vector<size_t> sizes = {1, 10, 25, 55, 128, 1024, 4096};
  for (size_t size : sizes) {
    hash_set.reserve(size);
    const size_t buckets_before = hash_set.NumBuckets();
    // Check that we expanded enough.
    CHECK_GE(hash_set.ElementsUntilExpand(), size);
    // Try inserting elements until we are at our reserve size and ensure the hash set did not
    // expand.
    while (hash_set.size() < size) {
      hash_set.insert(std::to_string(hash_set.size()));
    }
    CHECK_EQ(hash_set.NumBuckets(), buckets_before);
  }
  // Check the behaviour for shrinking, it does not necessarily resize down.
  constexpr size_t size = 100;
  hash_set.reserve(size);
  CHECK_GE(hash_set.ElementsUntilExpand(), size);
}

TEST_F(HashSetTest, IteratorConversion) {
  const char* test_string = "test string";
  HashSet<std::string> hash_set;
  HashSet<std::string>::iterator it = hash_set.insert(test_string).first;
  HashSet<std::string>::const_iterator cit = it;
  ASSERT_TRUE(it == cit);
  ASSERT_EQ(*it, *cit);
}

TEST_F(HashSetTest, StringSearchStringView) {
  const char* test_string = "test string";
  HashSet<std::string> hash_set;
  HashSet<std::string>::iterator insert_pos = hash_set.insert(test_string).first;
  HashSet<std::string>::iterator it = hash_set.find(std::string_view(test_string));
  ASSERT_TRUE(it == insert_pos);
}

TEST_F(HashSetTest, DoubleInsert) {
  const char* test_string = "test string";
  HashSet<std::string> hash_set;
  hash_set.insert(test_string);
  hash_set.insert(test_string);
  ASSERT_EQ(1u, hash_set.size());
}

TEST_F(HashSetTest, Preallocated) {
  static const size_t kBufferSize = 64;
  uint32_t buffer[kBufferSize];
  HashSet<uint32_t> hash_set(buffer, kBufferSize);
  size_t max_without_resize = kBufferSize * hash_set.GetMaxLoadFactor();
  for (size_t i = 0; i != max_without_resize; ++i) {
    hash_set.insert(i);
  }
  ASSERT_FALSE(hash_set.owns_data_);
  hash_set.insert(max_without_resize);
  ASSERT_TRUE(hash_set.owns_data_);
}

class SmallIndexEmptyFn {
 public:
  void MakeEmpty(uint16_t& item) const {
    item = std::numeric_limits<uint16_t>::max();
  }
  bool IsEmpty(const uint16_t& item) const {
    return item == std::numeric_limits<uint16_t>::max();
  }
};

class StatefulHashFn {
 public:
  explicit StatefulHashFn(const std::vector<std::string>* strings)
      : strings_(strings) {}

  size_t operator() (const uint16_t& index) const {
    CHECK_LT(index, strings_->size());
    return (*this)((*strings_)[index]);
  }

  size_t operator() (std::string_view s) const {
    return DataHash()(s);
  }

 private:
  const std::vector<std::string>* strings_;
};

class StatefulPred {
 public:
  explicit StatefulPred(const std::vector<std::string>* strings)
      : strings_(strings) {}

  bool operator() (const uint16_t& lhs, const uint16_t& rhs) const {
    CHECK_LT(rhs, strings_->size());
    return (*this)(lhs, (*strings_)[rhs]);
  }

  bool operator() (const uint16_t& lhs, std::string_view rhs) const {
    CHECK_LT(lhs, strings_->size());
    return (*strings_)[lhs] == rhs;
  }

 private:
  const std::vector<std::string>* strings_;
};

TEST_F(HashSetTest, StatefulHashSet) {
  std::vector<std::string> strings{
      "duplicate",
      "a",
      "b",
      "xyz",
      "___",
      "123",
      "placeholder",
      "duplicate"
  };
  const size_t duplicateFirstIndex = 0;
  const size_t duplicateSecondIndex = strings.size() - 1u;
  const size_t otherIndex = 1u;

  StatefulHashFn hashfn(&strings);
  StatefulPred pred(&strings);
  HashSet<uint16_t, SmallIndexEmptyFn, StatefulHashFn, StatefulPred> hash_set(hashfn, pred);
  for (size_t index = 0, size = strings.size(); index != size; ++index) {
    bool inserted = hash_set.insert(index).second;
    ASSERT_EQ(index != duplicateSecondIndex, inserted) << index;
  }

  // Check search by string.
  for (size_t index = 0, size = strings.size(); index != size; ++index) {
    auto it = hash_set.find(strings[index]);
    ASSERT_FALSE(it == hash_set.end());
    ASSERT_EQ(index == duplicateSecondIndex ? duplicateFirstIndex : index, *it) << index;
  }
  ASSERT_TRUE(hash_set.find("missing") == hash_set.end());

  // Check search by index.
  for (size_t index = 0, size = strings.size(); index != size; ++index) {
    auto it = hash_set.find(index);
    ASSERT_FALSE(it == hash_set.end());
    ASSERT_EQ(index == duplicateSecondIndex ? duplicateFirstIndex : index, *it) << index;
  }
  // Note: Searching for index >= strings.size() is not supported by Stateful{HashFn,Pred}.

  // Test removal and search by missing index.
  auto remove_it = hash_set.find(otherIndex);
  ASSERT_FALSE(remove_it == hash_set.end());
  hash_set.erase(remove_it);
  auto search_it = hash_set.find(otherIndex);
  ASSERT_TRUE(search_it == hash_set.end());
}

}  // namespace art
