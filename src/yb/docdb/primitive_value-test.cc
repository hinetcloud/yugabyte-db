// Copyright (c) YugaByte, Inc.

#include "yb/docdb/primitive_value.h"

#include <limits>
#include <map>

#include "yb/util/random.h"
#include "yb/util/test_macros.h"
#include "yb/util/test_util.h"
#include "yb/util/bytes_formatter.h"
#include "yb/gutil/strings/substitute.h"

using std::map;
using std::string;
using std::numeric_limits;
using strings::Substitute;

namespace yb {
namespace docdb {

namespace {

void EncodeAndDecodeValue(const PrimitiveValue& primitive_value) {
  string bytes = primitive_value.ToValue();
  rocksdb::Slice slice(bytes);
  PrimitiveValue decoded;
  ASSERT_OK_PREPEND(
      decoded.DecodeFromValue(slice),
      Substitute(
          "Could not decode value bytes obtained by encoding primitive value $0: $1",
          primitive_value.ToString(), bytes));
  ASSERT_EQ(primitive_value.ToString(), decoded.ToString())
      << "String representation of decoded value is different from that of the original value.";
}

void EncodeAndDecode(const PrimitiveValue& primitive_value) {
  KeyBytes key_bytes = primitive_value.ToKeyBytes();
  PrimitiveValue decoded;
  rocksdb::Slice slice = key_bytes.AsSlice();
  ASSERT_OK_PREPEND(
      decoded.DecodeFromKey(&slice),
      Substitute(
          "Could not decode key bytes obtained by encoding primitive value $0: $1",
          primitive_value.ToString(), key_bytes.ToString()));
  ASSERT_TRUE(slice.empty())
      << "Not all bytes consumed when encoding/decoding primitive value "
      << primitive_value.ToString() << ": "
      << slice.size() << " bytes left."
      << "Key bytes: " << key_bytes.ToString() << ".";
  ASSERT_EQ(primitive_value.ToString(), decoded.ToString())
      << "String representation of decoded value is different from that of the original value.";
}

void TestEncoding(const char* expected_str, const PrimitiveValue& primitive_value) {
  ASSERT_STR_EQ_VERBOSE_TRIMMED(expected_str, primitive_value.ToKeyBytes().ToString());
}

template <typename T>
void CompareSlices(KeyBytes key_bytes1, KeyBytes key_bytes2, T val1, T val2) {
  rocksdb::Slice slice1 = key_bytes1.AsSlice();
  rocksdb::Slice slice2 = key_bytes2.AsSlice();
  if (val1 > val2) {
    ASSERT_LT(0, slice1.compare(slice2)) << strings::Substitute("Failed for values $0, $1",
                                                                val1, val2);
  } else if (val1 < val2) {
    ASSERT_GT(0, slice1.compare(slice2)) << strings::Substitute("Failed for values $0, $1",
                                                                val1, val2);
  } else {
    ASSERT_EQ(0, slice1.compare(slice2)) << strings::Substitute("Failed for values $0, $1",
                                                                val1, val2);
  }
}

}  // unnamed namespace

TEST(PrimitiveValueTest, TestToString) {
  ASSERT_EQ("\"foo\"", PrimitiveValue("foo").ToString());
  ASSERT_EQ("\"foo\\\"\\x00\\x01\\x02\\\"bar\"",
      PrimitiveValue(string("foo\"\x00\x01\x02\"bar", 11)).ToString());

  ASSERT_EQ("123456789000", PrimitiveValue(123456789000l).ToString());
  ASSERT_EQ("-123456789000", PrimitiveValue(-123456789000l).ToString());
  ASSERT_EQ("9223372036854775807",
      PrimitiveValue(numeric_limits<int64_t>::max()).ToString());
  ASSERT_EQ("-9223372036854775808",
      PrimitiveValue(numeric_limits<int64_t>::min()).ToString());

  ASSERT_EQ("123456789", PrimitiveValue::Int32(123456789).ToString());
  ASSERT_EQ("-123456789", PrimitiveValue::Int32(-123456789).ToString());
  ASSERT_EQ("2147483647",
            PrimitiveValue::Int32(numeric_limits<int32_t>::max()).ToString());
  ASSERT_EQ("-2147483648",
            PrimitiveValue::Int32(numeric_limits<int32_t>::min()).ToString());

  ASSERT_EQ("3.1415", PrimitiveValue::Double(3.1415).ToString());
  ASSERT_EQ("100.0", PrimitiveValue::Double(100.0).ToString());
  ASSERT_EQ("1.000000E-100", PrimitiveValue::Double(1e-100).ToString());

  ASSERT_EQ("3.1415", PrimitiveValue::Float(3.1415).ToString());
  ASSERT_EQ("100.0", PrimitiveValue::Float(100.0).ToString());
  ASSERT_EQ("1.000000E-37", PrimitiveValue::Float(1e-37).ToString());

  ASSERT_EQ("ArrayIndex(123)", PrimitiveValue::ArrayIndex(123).ToString());
  ASSERT_EQ("ArrayIndex(-123)", PrimitiveValue::ArrayIndex(-123).ToString());

  ASSERT_EQ("HT(p=100200300400500, l=1234)",
      PrimitiveValue(HybridTime(100200300400500l * 4096 + 1234)).ToString());

  // HybridTimes use an unsigned 64-bit integer as an internal representation.
  ASSERT_EQ("HT(Min)", PrimitiveValue(HybridTime(0)).ToString());
  ASSERT_EQ("HT(Initial)", PrimitiveValue(HybridTime(1)).ToString());
  ASSERT_EQ("HT(Max)", PrimitiveValue(HybridTime(numeric_limits<uint64_t>::max())).ToString());
  ASSERT_EQ("HT(Max)", PrimitiveValue(HybridTime(-1)).ToString());

  ASSERT_EQ("UInt16Hash(65535)",
            PrimitiveValue::UInt16Hash(numeric_limits<uint16_t>::max()).ToString());
  ASSERT_EQ("UInt16Hash(65535)", PrimitiveValue::UInt16Hash(-1).ToString());
  ASSERT_EQ("UInt16Hash(0)", PrimitiveValue::UInt16Hash(0).ToString());

  ASSERT_EQ("ColumnId(2147483647)",
            PrimitiveValue(ColumnId(numeric_limits<int32_t>::max())).ToString());
  ASSERT_EQ("ColumnId(0)",
            PrimitiveValue(ColumnId(0)).ToString());

  ASSERT_EQ("SystemColumnId(2147483647)",
            PrimitiveValue::SystemColumnId(ColumnId(numeric_limits<int32_t>::max())).ToString());
  ASSERT_EQ("SystemColumnId(0)",
            PrimitiveValue::SystemColumnId(ColumnId(0)).ToString());

#ifndef NDEBUG
  // These have DCHECK() and hence triggered only in DEBUG MODE.
  // Negative column ids are not allowed.
  EXPECT_EXIT(ColumnId(-1), ::testing::KilledBySignal(SIGABRT), "Check failed.*");

  ColumnId col;
  EXPECT_EXIT({col = static_cast<ColumnIdRep>(-1);}, ::testing::KilledBySignal(SIGABRT),
              "Check failed.*");
#endif
}

TEST(PrimitiveValueTest, TestRoundTrip) {
  for (auto primitive_value : {
      PrimitiveValue("foo"),
      PrimitiveValue(string("foo\0bar\x01", 8)),
      PrimitiveValue(123L),
      PrimitiveValue::Int32(123),
      PrimitiveValue::Int32(std::numeric_limits<int32_t>::max()),
      PrimitiveValue::Int32(std::numeric_limits<int32_t>::min()),
      PrimitiveValue(HybridTime(1000L)),
      PrimitiveValue(ColumnId(numeric_limits<ColumnIdRep>::max())),
      PrimitiveValue(ColumnId(0)),
      PrimitiveValue::SystemColumnId(ColumnId(numeric_limits<ColumnIdRep>::max())),
      PrimitiveValue::SystemColumnId(ColumnId(0)),
  }) {
    EncodeAndDecode(primitive_value);
  }

  for (auto primitive_value : {
      PrimitiveValue("foo"),
      PrimitiveValue(string("foo\0bar\x01", 8)),
      PrimitiveValue(123L),
      PrimitiveValue::Int32(123),
      PrimitiveValue::Int32(std::numeric_limits<int32_t>::max()),
      PrimitiveValue::Int32(std::numeric_limits<int32_t>::min()),
      PrimitiveValue::Double(3.14),
      PrimitiveValue::Float(3.14),
  }) {
    EncodeAndDecodeValue(primitive_value);
  }
}

TEST(PrimitiveValueTest, TestEncoding) {
  TestEncoding(R"#("$foo\x00\x00")#", PrimitiveValue("foo"));
  TestEncoding(R"#("$foo\x00\x01bar\x01\x00\x00")#", PrimitiveValue(string("foo\0bar\x01", 8)));
  TestEncoding(R"#("I\x80\x00\x00\x00\x00\x00\x00{")#", PrimitiveValue(123L));
  TestEncoding(R"#("I\x00\x00\x00\x00\x00\x00\x00\x00")#",
      PrimitiveValue(std::numeric_limits<int64_t>::min()));
  TestEncoding(R"#("I\xff\xff\xff\xff\xff\xff\xff\xff")#",
      PrimitiveValue(std::numeric_limits<int64_t>::max()));

  // int32_t.
  TestEncoding(R"#("H\x80\x00\x00{")#", PrimitiveValue::Int32(123));
  TestEncoding(R"#("H\x00\x00\x00\x00")#",
               PrimitiveValue::Int32(std::numeric_limits<int32_t>::min()));
  TestEncoding(R"#("H\xff\xff\xff\xff")#",
               PrimitiveValue::Int32(std::numeric_limits<int32_t>::max()));

  // HybridTime encoding --------------------------------------------------------------------------

  TestEncoding(R"#("#\xff\x05S\x1e\x85.\xbb52\x7fK")#",
               PrimitiveValue(HybridTime(1234567890123L, 3456)));

  TestEncoding(R"#("#\x80\x80C")#",
               PrimitiveValue(HybridTime::FromMicros(kYugaByteMicrosecondEpoch)));

  // A little lower timestamp results in a little higher value that gets sorted later.
  TestEncoding(R"#("#\x81\x80C")#",
               PrimitiveValue(HybridTime::FromMicros(kYugaByteMicrosecondEpoch - 1)));

  // On the other hand, with a higher timestamp, "~" is 0x7e, which is sorted earlier than 0x80.
  TestEncoding(R"#("#~\x80C")#",
               PrimitiveValue(HybridTime::FromMicros(kYugaByteMicrosecondEpoch + 1)));

  TestEncoding(R"#("#\xff\x05T=\xf7)\xbc\x18\x80J")#",
               PrimitiveValue(HybridTime::FromMicros(1000)));

  // Float and Double size, 1 byte for value_type.
  ASSERT_EQ(1 + sizeof(double), PrimitiveValue::Double(3.14).ToValue().size());
  ASSERT_EQ(1 + sizeof(float), PrimitiveValue::Float(3.14).ToValue().size());
}

TEST(PrimitiveValueTest, TestCompareStringsWithEmbeddedZeros) {
  const auto zero_char = PrimitiveValue(string("\x00", 1));
  const auto two_zero_chars = PrimitiveValue(string("\x00\x00", 2));

  ASSERT_EQ(zero_char, zero_char);
  ASSERT_EQ(two_zero_chars, two_zero_chars);

  ASSERT_LT(zero_char, two_zero_chars);
  ASSERT_GT(two_zero_chars, zero_char);
  ASSERT_NE(zero_char, two_zero_chars);
  ASSERT_NE(two_zero_chars, zero_char);

  ASSERT_FALSE(zero_char < zero_char);
  ASSERT_FALSE(zero_char > zero_char);
  ASSERT_FALSE(two_zero_chars < two_zero_chars);
  ASSERT_FALSE(two_zero_chars > two_zero_chars);
  ASSERT_FALSE(two_zero_chars < zero_char);
  ASSERT_FALSE(zero_char > two_zero_chars);
}


TEST(PrimitiveValueTest, TestPrimitiveValuesAsMapKeys) {
  map<PrimitiveValue, string> m;
  const PrimitiveValue key2("key2");
  const PrimitiveValue key1("key1");
  ASSERT_TRUE(m.emplace(key2, "value2").second);
  ASSERT_EQ(1, m.count(key2));
  ASSERT_NE(m.find(key2), m.end());
  ASSERT_TRUE(m.emplace(key1, "value1").second);
  ASSERT_EQ(1, m.count(key1));
  ASSERT_NE(m.find(key1), m.end());
}

TEST(PrimitiveValueTest, TestCorruption) {
  // No column id specified.
  KeyBytes key_bytes;
  key_bytes.AppendValueType(ValueType::kColumnId);
  rocksdb::Slice slice = key_bytes.AsSlice();
  PrimitiveValue decoded;
  ASSERT_TRUE(decoded.DecodeFromKey(&slice).IsCorruption());

  // Invalid varint.
  key_bytes.AppendInt64(std::numeric_limits<int64_t>::max());
  ASSERT_TRUE(decoded.DecodeFromKey(&slice).IsCorruption());
}

TEST(PrimitiveValueTest, TestVarintStorage) {
  // Verify varint occupies the appropriate amount of bytes.
  KeyBytes key_bytes;
  key_bytes.AppendColumnId(ColumnId(63));
  ASSERT_EQ(1, key_bytes.AsSlice().size());

  // 2 bytes for > 63 (total 3 = 1 + 2)
  key_bytes.AppendColumnId(ColumnId(64));
  ASSERT_EQ(3, key_bytes.AsSlice().size());

  key_bytes.Clear();
  key_bytes.AppendColumnId(ColumnId(std::numeric_limits<ColumnIdRep>::max()));
  ASSERT_EQ(5, key_bytes.AsSlice().size());
}

TEST(PrimitiveValueTest, TestRandomComparableColumnId) {
  Random r(0);
  for (int i = 0; i < 1000; i++) {
    ColumnId column_id1(r.Next() % (std::numeric_limits<ColumnIdRep>::max()));
    ColumnId column_id2(r.Next() % (std::numeric_limits<ColumnIdRep>::max()));
    CompareSlices(PrimitiveValue(column_id1).ToKeyBytes(), PrimitiveValue(column_id2).ToKeyBytes(),
                  column_id1, column_id2);
  }
}

TEST(PrimitiveValueTest, TestRandomComparableInt32) {
  Random r(0);
  for (int i = 0; i < 1000; i++) {
    int32_t val1 = r.Next32();
    int32_t val2 = r.Next32();
    CompareSlices(PrimitiveValue::Int32(val1).ToKeyBytes(),
                  PrimitiveValue::Int32(val2).ToKeyBytes(), val1, val2);
  }
}

}  // namespace docdb
}  // namespace yb
