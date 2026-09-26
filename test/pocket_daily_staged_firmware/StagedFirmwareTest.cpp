#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "src/pocket_daily/staged_firmware.h"

using namespace PocketDaily::StagedFirmware;

namespace {
const uint8_t* bytesOf(const std::string& s) { return reinterpret_cast<const uint8_t*>(s.data()); }

std::string scan(const std::string& image, size_t chunk) {
  VersionScanner scanner;
  for (size_t i = 0; i < image.size(); i += chunk) scanner.feed(bytesOf(image) + i, std::min(chunk, image.size() - i));
  return scanner.version();
}

std::string withNul(const std::string& s) { return s + std::string(1, '\0'); }
}  // namespace

TEST(StagedFirmwarePath, OnlyTheExactPublishedPath) {
  EXPECT_TRUE(isPublishTarget("/update.bin"));
  EXPECT_FALSE(isPublishTarget(nullptr));
  EXPECT_FALSE(isPublishTarget(""));
  EXPECT_FALSE(isPublishTarget("update.bin"));
  EXPECT_FALSE(isPublishTarget("/UPDATE.BIN"));
  EXPECT_FALSE(isPublishTarget("/Books/update.bin"));
  EXPECT_FALSE(isPublishTarget("/update.bin.part"));
  EXPECT_FALSE(isPublishTarget("/.pocket-1234.part"));
}

TEST(StagedFirmwareMarker, ArmedMarkerIsTakenExactlyOnce) {
  Marker marker{};
  arm(marker, 6106496, 0x7B2B0410);
  uint32_t size = 0;
  uint32_t crc = 0;
  ASSERT_TRUE(take(marker, size, crc));
  EXPECT_EQ(size, 6106496U);
  EXPECT_EQ(crc, 0x7B2B0410U);
  // One-shot: a later boot never re-offers the same publication.
  EXPECT_FALSE(take(marker, size, crc));
}

TEST(StagedFirmwareMarker, RejectsColdBootGarbageAndTampering) {
  uint32_t size = 7;
  uint32_t crc = 9;
  Marker zero{};
  EXPECT_FALSE(take(zero, size, crc));
  Marker garbage{0xDEADBEEF, 0x12345678, 0x9ABCDEF0, 0x0F0F0F0F};
  EXPECT_FALSE(take(garbage, size, crc));
  EXPECT_EQ(garbage.magic, 0U) << "take() clears even an invalid marker";

  Marker marker{};
  arm(marker, 1000, 0xAABBCCDD);
  marker.size = 1001;  // a single flipped payload word fails the check word
  EXPECT_FALSE(take(marker, size, crc));
  EXPECT_EQ(size, 7U);
  EXPECT_EQ(crc, 9U);

  Marker empty{};
  arm(empty, 0, 0);
  EXPECT_FALSE(take(empty, size, crc)) << "an empty publication is never offered";
}

TEST(StagedFirmwareOffer, OnlyAfterSessionRestartOntoShellWithMatchingFile) {
  OfferInput base;
  base.markerValid = true;
  base.markerSize = 6000000;
  base.silentRestart = true;
  base.landsOnShell = true;
  base.fileExists = true;
  base.fileSize = 6000000;
  EXPECT_TRUE(shouldOffer(base));

  auto without = [&](auto mutate) {
    OfferInput input = base;
    mutate(input);
    return shouldOffer(input);
  };
  EXPECT_FALSE(without([](OfferInput& i) { i.markerValid = false; })) << "old file, no publication this session";
  EXPECT_FALSE(without([](OfferInput& i) { i.silentRestart = false; })) << "power-on / wake boot";
  EXPECT_FALSE(without([](OfferInput& i) { i.landsOnShell = false; })) << "reader, crash, recovery, dev routes";
  EXPECT_FALSE(without([](OfferInput& i) { i.fileExists = false; })) << "file removed since";
  EXPECT_FALSE(without([](OfferInput& i) { i.fileSize = 5999999; })) << "file replaced since";
  EXPECT_FALSE(without([](OfferInput& i) {
    i.markerSize = 0;
    i.fileSize = 0;
  }));
}

TEST(StagedFirmwareVersion, FindsMarkerAcrossChunkBoundaries) {
  const std::string image = std::string(5000, '\xFF') + withNul("CrossPoint version: 1.6.7-main+ab12cd3") + "tail";
  for (size_t chunk : {1U, 3U, 7U, 20U, 4096U, 100000U}) EXPECT_EQ(scan(image, chunk), "1.6.7-main+ab12cd3") << chunk;
}

TEST(StagedFirmwareVersion, SkipsMalformedCandidates) {
  // The scanner's own prefix literal (NUL right after the prefix), an
  // unterminated/non-printable candidate and a restart inside the prefix all
  // precede the real marker.
  const std::string image = withNul("CrossPoint version: ") + "CrossPoint version: bad\x01value" +
                            "CrossCrossPoint version: " + std::string(60, 'x') + withNul("") +
                            "CrossPoCrossPoint version: " + withNul("1.6.6");
  EXPECT_EQ(scan(image, 4096), "1.6.6");
}

TEST(StagedFirmwareVersion, AbsentMarkerYieldsEmpty) {
  EXPECT_EQ(scan(std::string(10000, 'A'), 4096), "");
  EXPECT_EQ(scan(withNul("CrossPoint version:1.0"), 4096), "") << "the space after the colon is required";
  VersionScanner scanner;
  scanner.feed(nullptr, 10);
  EXPECT_FALSE(scanner.found());
  EXPECT_STREQ(scanner.version(), "");
}

TEST(StagedFirmwareVersion, FirstWellFormedMarkerWinsAndResetClears) {
  VersionScanner scanner;
  const std::string image = withNul("CrossPoint version: 1.0.0") + withNul("CrossPoint version: 2.0.0");
  scanner.feed(bytesOf(image), image.size());
  EXPECT_STREQ(scanner.version(), "1.0.0");
  scanner.reset();
  EXPECT_FALSE(scanner.found());
  const std::string second = withNul("CrossPoint version: 2.0.0");
  scanner.feed(bytesOf(second), second.size());
  EXPECT_STREQ(scanner.version(), "2.0.0");
}

TEST(StagedFirmwareVersion, SameVersionNeedsIdenticalNonEmptyStrings) {
  EXPECT_TRUE(sameVersion("1.6.6-main+abc", "1.6.6-main+abc"));
  EXPECT_FALSE(sameVersion("1.6.6-main+abc", "1.6.6-main+abd"));
  EXPECT_FALSE(sameVersion("1.6.6", "1.6.6-slim"));
  EXPECT_FALSE(sameVersion("", ""));
  EXPECT_FALSE(sameVersion(nullptr, "1.6.6"));
  EXPECT_FALSE(sameVersion("1.6.6", nullptr));
}
