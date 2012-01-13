//////////////////////////////////////////////////////////////////////////////
// Licensed to Qualys, Inc. (QUALYS) under one or more
// contributor license agreements.  See the NOTICE file distributed with
// this work for additional information regarding copyright ownership.
// QUALYS licenses this file to You under the Apache License, Version 2.0
// (the "License"); you may not use this file except in compliance with
// the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////
/// @file
/// @brief IronBee - UUID Test Functions
/// 
/// @author Brian Rectanus <brectanus@qualys.com>
//////////////////////////////////////////////////////////////////////////////

#include "ironbee_config_auto.h"
#include "ironbee/util.h"

#include "gtest/gtest.h"
#include "gtest/gtest-spi.h"

TEST(TestIBUtilUnescapeString, singleCharacter) {
  char str[7] = "\\r\\n\\t";
  char str2[4];
  size_t len;
  ASSERT_EQ(IB_OK, ib_util_unescape_string(str2, &len, str, strlen(str)));
  
  EXPECT_EQ('\r', str2[0]);
  EXPECT_EQ('\n', str2[1]);
  EXPECT_EQ('\t', str2[2]);
  EXPECT_EQ('\0', str2[3]);
  ASSERT_EQ(3, len);
}

TEST(TestIBUtilUnescapeString, singleBytes) {
  char *str = "\\x01\\x02";
  char str2[3];
  char chk[2] = { 1, 2 };
  size_t len;
  ASSERT_EQ(IB_OK, ib_util_unescape_string(str2, &len, str, strlen(str)));
  
  EXPECT_EQ(chk[0], str2[0]);
  EXPECT_EQ(chk[1], str2[1]);
  EXPECT_EQ('\0', str2[2]);
  EXPECT_EQ(2, len);
}

TEST(TestIBUtilUnescapeString, longBytes) {
  char *str = "\\u0001\\u4321";
  char str2[5];
  char chk[4] = { 0, 1, 67, 33 };
  size_t len;
  ASSERT_EQ(IB_OK, ib_util_unescape_string(str2, &len, str, strlen(str)));
  
  EXPECT_EQ(chk[0], str2[0]);
  EXPECT_EQ(chk[1], str2[1]);
  EXPECT_EQ(chk[2], str2[2]);
  EXPECT_EQ(chk[3], str2[3]);
  EXPECT_EQ('\0', str2[4]);
  EXPECT_EQ(4, len);
}

TEST(TestIBUtilUnescapeString, shortSingleBytesEndOfLine) {
  char *str = "\\x01\\x0";
  char str2[3];
  size_t len;
  ASSERT_EQ(IB_EINVAL, ib_util_unescape_string(str2, &len, str, strlen(str)));
}

TEST(TestIBUtilUnescapeString, shortSingleBytes) {
  char *str = "\\x0\\x00";
  char str2[3];
  size_t len;
  ASSERT_EQ(IB_EINVAL, ib_util_unescape_string(str2, &len, str, strlen(str)));
}

TEST(TestIBUtilUnescapeString, shortLongBytes) {
  char *str = "\\u001\\u4321";
  char str2[5];
  size_t len;
  ASSERT_EQ(IB_EINVAL, ib_util_unescape_string(str2, &len, str, strlen(str)));
}

TEST(TestIBUtilUnescapeString, shortLongBytesEndOfLine) {
  char *str = "\\u0001\\u431";
  char str2[5];
  size_t len;
  ASSERT_EQ(IB_EINVAL, ib_util_unescape_string(str2, &len, str, strlen(str)));
}
