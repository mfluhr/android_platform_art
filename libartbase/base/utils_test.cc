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

#include "utils.h"
#include "stl_util.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace art {

class UtilsTest : public ::testing::Test {};

TEST_F(UtilsTest, PrettySize) {
  EXPECT_EQ("1024MB", PrettySize(1 * GB));
  EXPECT_EQ("2048MB", PrettySize(2 * GB));
  if (sizeof(size_t) > sizeof(uint32_t)) {
    EXPECT_EQ("100GB", PrettySize(100 * GB));
  }
  EXPECT_EQ("1024KB", PrettySize(1 * MB));
  EXPECT_EQ("10MB", PrettySize(10 * MB));
  EXPECT_EQ("100MB", PrettySize(100 * MB));
  EXPECT_EQ("1024B", PrettySize(1 * KB));
  EXPECT_EQ("10KB", PrettySize(10 * KB));
  EXPECT_EQ("100KB", PrettySize(100 * KB));
  EXPECT_EQ("0B", PrettySize(0));
  EXPECT_EQ("1B", PrettySize(1));
  EXPECT_EQ("10B", PrettySize(10));
  EXPECT_EQ("100B", PrettySize(100));
  EXPECT_EQ("512B", PrettySize(512));
}

void Split(const char* arr, char s, std::vector<std::string_view>* sv) {
  Split<std::string_view>(std::string_view(arr), s, sv);
}

TEST_F(UtilsTest, Split) {
  std::vector<std::string_view> actual;
  std::vector<std::string_view> expected;

  expected.clear();

  actual.clear();
  Split("", ':', &actual);
  EXPECT_EQ(expected, actual);

  actual.clear();
  Split(":", ':', &actual);
  EXPECT_EQ(expected, actual);

  expected.clear();
  expected.push_back("foo");

  actual.clear();
  Split(":foo", ':', &actual);
  EXPECT_EQ(expected, actual);

  actual.clear();
  Split("foo:", ':', &actual);
  EXPECT_EQ(expected, actual);

  actual.clear();
  Split(":foo:", ':', &actual);
  EXPECT_EQ(expected, actual);

  expected.push_back("bar");

  actual.clear();
  Split("foo:bar", ':', &actual);
  EXPECT_EQ(expected, actual);

  actual.clear();
  Split(":foo:bar", ':', &actual);
  EXPECT_EQ(expected, actual);

  actual.clear();
  Split("foo:bar:", ':', &actual);
  EXPECT_EQ(expected, actual);

  actual.clear();
  Split(":foo:bar:", ':', &actual);
  EXPECT_EQ(expected, actual);

  expected.push_back("baz");

  actual.clear();
  Split("foo:bar:baz", ':', &actual);
  EXPECT_EQ(expected, actual);

  actual.clear();
  Split(":foo:bar:baz", ':', &actual);
  EXPECT_EQ(expected, actual);

  actual.clear();
  Split("foo:bar:baz:", ':', &actual);
  EXPECT_EQ(expected, actual);

  actual.clear();
  Split(":foo:bar:baz:", ':', &actual);
  EXPECT_EQ(expected, actual);
}

TEST_F(UtilsTest, GetProcessStatus) {
  EXPECT_THAT(
      GetProcessStatus("Name"),
      ::testing::AnyOf("art_libartbase_",    // Test binary name: `art_libartbase_test`.
                       "art_standalone_"));  // Test binary name: `art_standalone_libartbase_test`.
  EXPECT_EQ("R (running)", GetProcessStatus("State"));
  EXPECT_EQ("<unknown>", GetProcessStatus("tate"));
  EXPECT_EQ("<unknown>", GetProcessStatus("e"));
  EXPECT_EQ("<unknown>", GetProcessStatus("InvalidFieldName"));
}

TEST_F(UtilsTest, GetOsThreadStatQuick) {
  std::string my_stat = GetOsThreadStatQuick(GetTid());
  EXPECT_GT(my_stat.length(), 20);
  EXPECT_LT(my_stat.length(), 1000);
  EXPECT_EQ('R', GetStateFromStatString(my_stat));
}

TEST_F(UtilsTest, StringSplit) {
  auto range = SplitString("[ab[c[[d[e[", '[');
  auto it = range.begin();
  EXPECT_FALSE(it == range.end());
  EXPECT_EQ(*it++, "");
  EXPECT_FALSE(it == range.end());
  EXPECT_EQ(*it++, "ab");
  EXPECT_FALSE(it == range.end());
  EXPECT_EQ(*it++, "c");
  EXPECT_FALSE(it == range.end());
  EXPECT_EQ(*it++, "");
  EXPECT_FALSE(it == range.end());
  EXPECT_EQ(*it++, "d");
  EXPECT_FALSE(it == range.end());
  EXPECT_EQ(*it++, "e");
  EXPECT_FALSE(it == range.end());
  EXPECT_EQ(*it++, "");
  EXPECT_TRUE(it == range.end());
}

TEST_F(UtilsTest, StringSplit2) {
  auto range = SplitString("ab[c[[d[e", '[');
  auto it = range.begin();
  EXPECT_FALSE(it == range.end());
  EXPECT_EQ(*it++, "ab");
  EXPECT_FALSE(it == range.end());
  EXPECT_EQ(*it++, "c");
  EXPECT_FALSE(it == range.end());
  EXPECT_EQ(*it++, "");
  EXPECT_FALSE(it == range.end());
  EXPECT_EQ(*it++, "d");
  EXPECT_FALSE(it == range.end());
  EXPECT_EQ(*it++, "e");
  EXPECT_TRUE(it == range.end());
}

TEST_F(UtilsTest, StringSplit3) {
  auto range = SplitString("", '[');
  auto it = range.begin();
  EXPECT_FALSE(it == range.end());
  EXPECT_EQ(*it++, "");
  EXPECT_TRUE(it == range.end());
}

}  // namespace art
