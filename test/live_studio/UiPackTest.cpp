#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "pocket_daily/live_studio/ThemeFieldIds.h"
#include "pocket_daily/live_studio/UiPack.h"

namespace {

using namespace PocketDaily::LiveStudio;

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
  const std::vector<ThemeOverride> bogus = {
      {static_cast<uint16_t>(ThemeField::kFieldCount + 3), 1, 10}};
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

}  // namespace
