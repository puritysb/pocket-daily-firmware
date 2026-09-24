#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "pocket_daily/live_studio/MetricGeometry.h"
#include "pocket_daily/live_studio/ThemeFieldIds.h"
#include "pocket_daily/live_studio/UiPack.h"
#include "pocket_daily/live_studio/UiPackMetrics.h"
#include "pocket_daily/live_studio/UiPackState.h"

namespace {

using namespace PocketDaily::LiveStudio;

TEST(UiPackGeometry, PaginationUsesAppliedAndRevertedRowHeight) {
  ThemeMetrics base{};
  base.listRowHeight = 30;
  ThemeMetrics output{};
  ThemeOverride override{ThemeField::kListRowHeight, 1, 60};
  composePackMetrics(base, &override, 1, output);
  EXPECT_EQ(MetricGeometry::pageItems(300, output.listRowHeight), 5);
  composePackMetrics(base, nullptr, 0, output);
  EXPECT_EQ(MetricGeometry::pageItems(300, output.listRowHeight), 10);
}

TEST(UiPackGeometry, TinyAndInvalidDimensionsKeepPaginationDivisorsPositive) {
  EXPECT_EQ(MetricGeometry::pageItems(10, 120), 1);
  EXPECT_EQ(MetricGeometry::pageItems(0, 30), 1);
  EXPECT_EQ(MetricGeometry::pageItems(-30, 30), 1);
  EXPECT_EQ(MetricGeometry::pageItems(300, 0), 300);
  EXPECT_EQ(MetricGeometry::pageItems(300, -1), 300);
  EXPECT_EQ(MetricGeometry::rowStep(30, -30), 1);
  EXPECT_EQ(MetricGeometry::rowStep(INT32_MAX, INT32_MAX), INT32_MAX);
  EXPECT_EQ(MetricGeometry::rowStep(INT32_MIN, INT32_MIN), 1);
  EXPECT_EQ(MetricGeometry::pageItems(INT64_MAX, 1), INT32_MAX);
}

TEST(UiPack, ReplacementAndRevertDoNotInheritPreviousOverrides) {
  ThemeMetrics base{};
  base.headerHeight = 30;
  base.menuSpacing = 4;
  ThemeMetrics output{};
  const ThemeOverride first[] = {{ThemeField::kHeaderHeight, 1, 60}};
  composePackMetrics(base, first, 1, output);
  EXPECT_EQ(output.headerHeight, 60);
  const ThemeOverride second[] = {{ThemeField::kMenuSpacing, 1, 10}};
  composePackMetrics(base, second, 1, output);
  EXPECT_EQ(output.headerHeight, 30);
  EXPECT_EQ(output.menuSpacing, 10);
  composePackMetrics(base, nullptr, 0, output);
  EXPECT_EQ(output.headerHeight, 30);
  EXPECT_EQ(output.menuSpacing, 4);
}

TEST(UiPackState, TornOrCorruptReplacementRetainsPreviousSlot) {
  PackState::Record previous{7, "studio-old", "old"};
  PackState::Record next{8, "studio-new", "new"};
  uint8_t bytes[PackState::BYTES];
  ASSERT_TRUE(PackState::encode(next, bytes));
  for (size_t size = 0; size < sizeof(bytes); ++size) {
    PackState::Record decoded{};
    EXPECT_FALSE(PackState::decode(bytes, size, decoded));
    EXPECT_EQ(PackState::latest(true, previous.generation, false, decoded.generation), 0);
  }
  for (size_t offset = 0; offset < sizeof(bytes); ++offset) {
    bytes[offset] ^= 1;
    PackState::Record decoded{};
    EXPECT_FALSE(PackState::decode(bytes, sizeof(bytes), decoded));
    bytes[offset] ^= 1;
  }
  PackState::Record decoded{};
  ASSERT_TRUE(PackState::decode(bytes, sizeof(bytes), decoded));
  EXPECT_EQ(PackState::latest(true, previous.generation, true, decoded.generation), 1);
  EXPECT_STREQ(decoded.name, "studio-new");
  EXPECT_STREQ(decoded.version, "new");
}

TEST(UiPackState, RevertIsANewValidTombstone) {
  PackState::Record tombstone{9, "", ""};
  uint8_t bytes[PackState::BYTES];
  ASSERT_TRUE(PackState::encode(tombstone, bytes));
  PackState::Record decoded{};
  ASSERT_TRUE(PackState::decode(bytes, sizeof(bytes), decoded));
  EXPECT_EQ(PackState::latest(true, 8, true, decoded.generation), 1);
  EXPECT_STREQ(decoded.name, "");
  EXPECT_STREQ(decoded.version, "");
  EXPECT_EQ(PackState::latest(false, 0, false, 0), -1);
}

TEST(UiPackState, RejectsUnsafeNamesAndMalformedRecords) {
  EXPECT_FALSE(PackState::validName("../bad"));
  EXPECT_FALSE(PackState::validName("a.b"));
  EXPECT_FALSE(PackState::validName("abcdefghijklmnopqrstuvwxyz"));
  EXPECT_FALSE(PackState::validVersion("quote\""));
  EXPECT_TRUE(PackState::validVersion("1.0-abc"));
  PackState::Record invalid{0, "studio", "1"};
  uint8_t bytes[PackState::BYTES];
  EXPECT_FALSE(PackState::encode(invalid, bytes));
  invalid.generation = 1;
  memset(invalid.name, 'x', sizeof(invalid.name));
  EXPECT_FALSE(PackState::encode(invalid, bytes));
}

TEST(UiPack, SwiftMixedTypesGolden) {
  // Exact encoder output pinned in the companion UiPackEncoderTests.swift.
  const char* hex =
      "504455490100000073747564696f0000000000000000000000000000000000000000000000000000312e3000000000000000000000000000"
      "00000000000000000000000000000000030000000000000015000000cb099a5fb2ce69981565f3640b239a91fcce4b10cc9a5a44396e3b79"
      "63afb513259b61210400012a000000330002010000002e00030000003f";
  std::vector<uint8_t> pack;
  const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (size_t i = 0; hex[i] != '\0'; i += 2) {
    pack.push_back(static_cast<uint8_t>(digit(hex[i]) * 16 + digit(hex[i + 1])));
  }
  UiPackInfo info{};
  ThemeOverride overrides[3]{};
  ASSERT_EQ(UiPackResult::Ok, validatePack(pack.data(), pack.size(), &info, overrides, 3));
  EXPECT_STREQ(info.name, "studio");
  EXPECT_STREQ(info.packVersion, "1.0");
  ThemeMetrics metrics{};
  for (const auto& field : overrides) {
    ThemeField::applyOverride(metrics, field.fieldId, field.type, field.value);
  }
  EXPECT_EQ(metrics.headerHeight, 42);
  EXPECT_TRUE(metrics.popupTextBold);
  EXPECT_FLOAT_EQ(metrics.popupTopOffsetRatio, 0.5f);
}

// Minimal valid pack: header + N theme overrides.
std::vector<uint8_t> buildPack(const std::vector<ThemeOverride>& overrides, uint32_t crcOverride = 0,
                               uint16_t countOverride = 0xFFFF) {
  std::vector<uint8_t> payload;
  for (const auto& o : overrides) {
    payload.push_back(o.fieldId & 0xFF);
    payload.push_back(o.fieldId >> 8);
    payload.push_back(o.type);
    payload.push_back(o.value & 0xFF);
    payload.push_back((o.value >> 8) & 0xFF);
    payload.push_back((o.value >> 16) & 0xFF);
    payload.push_back((o.value >> 24) & 0xFF);
  }
  std::vector<uint8_t> pack(UIPACK_HEADER_SIZE + payload.size(), 0);
  memcpy(pack.data(), "PDUI", 4);
  pack[4] = 1;
  memcpy(pack.data() + 8, "studio", 6);
  memcpy(pack.data() + 40, "1.0", 3);
  const uint16_t count = countOverride == 0xFFFF ? static_cast<uint16_t>(overrides.size()) : countOverride;
  pack[72] = count & 0xFF;
  pack[73] = count >> 8;
  const uint32_t payloadLen = static_cast<uint32_t>(payload.size());
  pack[80] = payloadLen & 0xFF;
  pack[81] = (payloadLen >> 8) & 0xFF;
  pack[82] = (payloadLen >> 16) & 0xFF;
  pack[83] = (payloadLen >> 24) & 0xFF;
  const uint32_t crc = crcOverride ? crcOverride : uiPackCrc32(payload.data(), payload.size());
  pack[84] = crc & 0xFF;
  pack[85] = (crc >> 8) & 0xFF;
  pack[86] = (crc >> 16) & 0xFF;
  pack[87] = (crc >> 24) & 0xFF;
  memcpy(pack.data() + UIPACK_HEADER_SIZE, payload.data(), payload.size());
  return pack;
}

TEST(UiPack, AcceptsValidPackAndCopiesOverrides) {
  const std::vector<ThemeOverride> overrides = {
      {ThemeField::kListRowHeight, 1, 64},
      {ThemeField::kMenuRowHeight, 1, 70},
      {ThemeField::kPopupTextBold, 2, 1},
  };
  const auto pack = buildPack(overrides);
  UiPackInfo info;
  ThemeOverride out[8];
  ASSERT_EQ(UiPackResult::Ok, validatePack(pack.data(), pack.size(), &info, out, 8));
  EXPECT_STREQ(info.name, "studio");
  EXPECT_STREQ(info.packVersion, "1.0");
  ASSERT_EQ(info.themeOverrideCount, 3u);
  EXPECT_EQ(out[1].fieldId, ThemeField::kMenuRowHeight);
  EXPECT_EQ(out[1].value, 70);
}

TEST(UiPack, AppliesOverridesToMetrics) {
  const std::vector<ThemeOverride> overrides = {
      {ThemeField::kHeaderHeight, 1, 42},
      {ThemeField::kPopupTextBold, 2, 1},
  };
  const auto pack = buildPack(overrides);
  ThemeOverride out[4];
  ASSERT_EQ(UiPackResult::Ok, validatePack(pack.data(), pack.size(), nullptr, out, 4));
  ThemeMetrics metrics{};
  metrics.headerHeight = 30;
  metrics.popupTextBold = false;
  for (size_t i = 0; i < 2; i++) {
    ThemeField::applyOverride(metrics, out[i].fieldId, out[i].type, out[i].value);
  }
  EXPECT_EQ(metrics.headerHeight, 42);
  EXPECT_TRUE(metrics.popupTextBold);
}

TEST(UiPack, RejectsCorruption) {
  const std::vector<ThemeOverride> overrides = {{ThemeField::kTabSpacing, 1, 8}};
  auto pack = buildPack(overrides, 0xdeadbeef);
  EXPECT_EQ(UiPackResult::BadCrc, validatePack(pack.data(), pack.size(), nullptr, nullptr, 0));

  pack = buildPack(overrides);
  pack[0] = 'X';
  EXPECT_EQ(UiPackResult::BadMagic, validatePack(pack.data(), pack.size(), nullptr, nullptr, 0));

  pack = buildPack(overrides);
  pack[4] = 9;
  EXPECT_EQ(UiPackResult::BadVersion, validatePack(pack.data(), pack.size(), nullptr, nullptr, 0));

  pack = buildPack(overrides, 0, 2);  // claims 2 overrides, payload has 1
  EXPECT_EQ(UiPackResult::BadRecord, validatePack(pack.data(), pack.size(), nullptr, nullptr, 0));

  pack = buildPack(overrides, 0, 99);  // above the header count ceiling
  EXPECT_EQ(UiPackResult::BadHeaderFields, validatePack(pack.data(), pack.size(), nullptr, nullptr, 0));

  std::vector<uint8_t> tiny(50, 0);
  EXPECT_EQ(UiPackResult::TooSmall, validatePack(tiny.data(), tiny.size(), nullptr, nullptr, 0));
}

TEST(UiPack, RejectsUnknownFieldsAndTypeMismatch) {
  // id beyond the registry
  const std::vector<ThemeOverride> bogus = {{static_cast<uint16_t>(ThemeField::kFieldCount + 3), 1, 10}};
  auto pack = buildPack(bogus);
  EXPECT_EQ(UiPackResult::UnknownField, validatePack(pack.data(), pack.size(), nullptr, nullptr, 0));

  // known id, wrong type code (bool field claimed as int)
  const std::vector<ThemeOverride> mismatch = {{ThemeField::kPopupTextBold, 1, 1}};
  pack = buildPack(mismatch);
  EXPECT_EQ(UiPackResult::UnknownField, validatePack(pack.data(), pack.size(), nullptr, nullptr, 0));
}

TEST(UiPack, RegistryMatchesGeneratedJsonFieldCount) {
  // The generator emits 63 fields for the current BaseTheme.h; the test pins
  // the count so a regeneration without a companion update is noticed.
  EXPECT_EQ(ThemeField::kFieldCount, 63u);
}

TEST(UiPack, RejectsNonCanonicalBoolAndNonFiniteFloatEvenWithValidCrc) {
  for (const uint32_t value : {2u, 255u, 0xFFFFFFFFu}) {
    const auto pack = buildPack({{ThemeField::kPopupTextBold, 2, static_cast<int32_t>(value)}});
    EXPECT_EQ(UiPackResult::BadRecord, validatePack(pack.data(), pack.size(), nullptr, nullptr, 0));
  }
  // Positive/negative infinity, quiet/signalling NaN, both signs.
  for (const uint32_t value : {0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x7F800001u, 0xFFC00000u}) {
    const auto pack = buildPack({{ThemeField::kPopupTopOffsetRatio, 3, static_cast<int32_t>(value)}});
    EXPECT_EQ(UiPackResult::BadRecord, validatePack(pack.data(), pack.size(), nullptr, nullptr, 0));
  }
}

TEST(UiPack, AcceptsCanonicalBoolsAndBoundedFloatBitPatterns) {
  for (const int32_t value : {0, 1}) {
    const auto pack = buildPack({{ThemeField::kPopupTextBold, 2, value}});
    EXPECT_EQ(UiPackResult::Ok, validatePack(pack.data(), pack.size(), nullptr, nullptr, 0));
  }
  // Preserve signed zero, smallest positive subnormal and both ratio bounds.
  for (const uint32_t value : {0u, 0x80000000u, 1u, 0x3F000000u, 0x3F800000u}) {
    const auto pack = buildPack({{ThemeField::kPopupTopOffsetRatio, 3, static_cast<int32_t>(value)}});
    ThemeOverride out{};
    ASSERT_EQ(UiPackResult::Ok, validatePack(pack.data(), pack.size(), nullptr, &out, 1));
    EXPECT_EQ(static_cast<uint32_t>(out.value), value);
  }
}

TEST(UiPack, RejectsOutOfRangePopupRatioEvenWithValidCrc) {
  // Immediately above one, smallest negative subnormal, +/- largest finite.
  for (const uint32_t value : {0x3F800001u, 0x80000001u, 0x7F7FFFFFu, 0xFF7FFFFFu}) {
    const auto pack = buildPack({{ThemeField::kPopupTopOffsetRatio, 3, static_cast<int32_t>(value)}});
    EXPECT_EQ(UiPackResult::BadRecord, validatePack(pack.data(), pack.size(), nullptr, nullptr, 0));
  }
}

TEST(UiPack, CoverHeightMustBePositiveAndWithinPackBudget) {
  for (const int32_t value : {INT32_MIN, -1, 0, 2049, INT32_MAX}) {
    const auto pack = buildPack({{ThemeField::kHomeCoverHeight, 1, value}});
    EXPECT_EQ(UiPackResult::BadRecord, validatePack(pack.data(), pack.size(), nullptr, nullptr, 0));
  }
  for (const int32_t value : {1, 226, 300, 400, 2048}) {
    const auto pack = buildPack({{ThemeField::kHomeCoverHeight, 1, value}});
    EXPECT_EQ(UiPackResult::Ok, validatePack(pack.data(), pack.size(), nullptr, nullptr, 0));
  }
}

struct TestSource {
  const std::vector<uint8_t>& bytes;
  size_t calls = 0;
  size_t failCall = 0;
  size_t maxRead = 0;
  std::vector<uint8_t> digested;
  bool digestFails = false;
  UiPackSource source() {
    return {this,
            [](void* context, size_t offset, uint8_t* output, size_t count) {
              auto& self = *static_cast<TestSource*>(context);
              ++self.calls;
              self.maxRead = std::max(self.maxRead, count);
              if (self.calls == self.failCall || offset > self.bytes.size() || count > self.bytes.size() - offset)
                return false;
              memcpy(output, self.bytes.data() + offset, count);
              return true;
            },
            [](void* context, const uint8_t* data, size_t count) {
              auto& self = *static_cast<TestSource*>(context);
              if (self.digestFails) return false;
              self.digested.insert(self.digested.end(), data, data + count);
              return true;
            }};
  }
};

TEST(UiPack, SourceBoundsReadsAndDigestsPayloadExactlyOnce) {
  const auto pack = buildPack(std::vector<ThemeOverride>(40, {ThemeField::kHeaderHeight, 1, 42}));
  TestSource input{pack};
  UiPackInfo info{};
  uint8_t sha[32];
  EXPECT_EQ(UiPackResult::Ok, validatePackSource(input.source(), pack.size(), &info, nullptr, 0, sha));
  EXPECT_EQ(info.themeOverrideCount, 40u);
  EXPECT_LE(input.maxRead, 128u);
  EXPECT_EQ(input.digested, std::vector<uint8_t>(pack.begin() + UIPACK_HEADER_SIZE, pack.end()));
  EXPECT_EQ(0, memcmp(sha, pack.data() + 88, sizeof(sha)));
}

TEST(UiPack, SourceReadAndDigestFailuresAreTerminal) {
  const auto pack = buildPack({{ThemeField::kHeaderHeight, 1, 42}});
  // Header, integrity pass, then record pass.
  for (size_t call = 1; call <= 3; ++call) {
    TestSource input{pack};
    input.failCall = call;
    EXPECT_EQ(UiPackResult::ReadFail, validatePackSource(input.source(), pack.size(), nullptr, nullptr, 0));
  }
  TestSource input{pack};
  input.digestFails = true;
  EXPECT_EQ(UiPackResult::ReadFail, validatePackSource(input.source(), pack.size(), nullptr, nullptr, 0));
  EXPECT_STREQ("read failed", uiPackResultName(UiPackResult::ReadFail));
}

TEST(UiPack, SourceTruncationNeverReadsPastAvailableBytes) {
  const auto pack = buildPack({{ThemeField::kHeaderHeight, 1, 42}});
  for (size_t length = 0; length < pack.size(); ++length) {
    const std::vector<uint8_t> truncated(pack.begin(), pack.begin() + length);
    TestSource input{truncated};
    EXPECT_NE(UiPackResult::Ok, validatePackSource(input.source(), length, nullptr, nullptr, 0));
    EXPECT_EQ(0u, input.digested.size());
  }
  const std::vector<uint8_t> truncated(pack.begin(), pack.end() - 1);
  TestSource input{truncated};
  EXPECT_EQ(UiPackResult::ReadFail, validatePackSource(input.source(), pack.size(), nullptr, nullptr, 0));
}

}  // namespace
