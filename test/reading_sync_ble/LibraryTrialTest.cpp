#include <gtest/gtest.h>

#include <climits>
#include <cstring>

#include "pocket_daily/dev/CaptureTicket.h"
#include "pocket_daily/dev/LibraryTrial.h"

using namespace PocketDaily::DevCapture;
namespace {
LibraryEvidence admitted() {
  LibraryEvidence value;
  value.run = 743;
  value.phase = LibraryPhase::Admitted;
  value.bytes = 43492;
  value.pages = 1;
  strcpy(value.reference, "/pocket-rc-original-ko.epub");
  for (unsigned i = 0; i < 32; ++i) {
    value.sourceSha[i] = i * 7 + 3;
    value.backupSha[i] = i * 11 + 1;
  }
  value.check = libraryChecksum(value);
  return value;
}
}  // namespace

TEST(LibraryTrial, AdmissionRecordDetectsEverySingleByteFaultAndForeignRun) {
  const auto original = admitted();
  ASSERT_TRUE(validLibraryEvidence(original, 743));
  EXPECT_FALSE(validLibraryEvidence(original, 744));
  for (size_t i = 0; i < sizeof(original); ++i) {
    auto damaged = original;
    reinterpret_cast<uint8_t*>(&damaged)[i] ^= 0x20;
    EXPECT_FALSE(validLibraryEvidence(damaged, 743)) << i;
  }
}
TEST(LibraryTrial, ReferenceCannotEscapeRootOrSelectOwnedBook) {
  for (const auto* path : {"/a.epub", "/pocket-rc-1_ko.epub"}) EXPECT_TRUE(validLibraryReference(path));
  for (const auto* path : {"a.epub", "/.epub", "/../a.epub", "/folder/a.epub", "/a.EPUB", "/Pocket visual 743.epub",
                           "/a%2fb.epub", "/a\\b.epub"})
    EXPECT_FALSE(validLibraryReference(path)) << path;
  EXPECT_FALSE(validLibraryReference(std::string("/") + std::string(125, 'a') + ".epub"));
}
TEST(LibraryTrial, DigestIsExactLowercaseFullWidthAndNonuniform) {
  uint8_t out[32];
  ASSERT_TRUE(decodeLibraryDigest("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", out));
  EXPECT_EQ(out[0], 1);
  EXPECT_EQ(out[1], 0x23);
  EXPECT_EQ(out[31], 0xef);
  EXPECT_FALSE(decodeLibraryDigest(std::string(64, 'G'), out));
  EXPECT_FALSE(decodeLibraryDigest(std::string(63, 'a'), out));
  EXPECT_FALSE(decodeLibraryDigest(std::string(65, 'a'), out));
}
TEST(LibraryTrial, OnlyCompleteVisibleTargetRowCanBecomeFrameEvidence) {
  EXPECT_TRUE(libraryRowFullyDrawn(18, 16, 3, 2, 18));
  EXPECT_FALSE(libraryRowFullyDrawn(19, 16, 3, 3, 19));  // Loaded trailing partial row.
  EXPECT_FALSE(libraryRowFullyDrawn(18, 16, 3, 2, 17));  // Wrong actual UI item.
  EXPECT_FALSE(libraryRowFullyDrawn(18, 17, 3, 2, 18));  // Viewport rebuilt.
  EXPECT_FALSE(libraryRowFullyDrawn(15, 16, 3, 0, 15));
  EXPECT_FALSE(libraryRowFullyDrawn(INT_MAX, INT_MAX, 2, 1, INT_MAX));
}
TEST(LibraryTrial, OpenRequiresBothFramesExactOwnedPathAndOneShot) {
  auto value = admitted();
  value.phase = LibraryPhase::Owned;
  value.flags = LIBRARY_FRAMES;
  EXPECT_TRUE(libraryCanOpen(value, true));
  EXPECT_FALSE(libraryCanOpen(value, false));
  for (uint32_t flag = 1; flag <= OWNED_FRAME; flag <<= 1) {
    value.flags = LIBRARY_FRAMES ^ flag;
    EXPECT_FALSE(libraryCanOpen(value, true));
  }
  value.flags = LIBRARY_FRAMES;
  value.opened = 1;
  EXPECT_FALSE(libraryCanOpen(value, true));
  value.opened = 0;
  value.phase = LibraryPhase::Original;
  EXPECT_FALSE(libraryCanOpen(value, true));
  value.phase = LibraryPhase::Owned;
  value.error = LibraryError::Persistence;
  EXPECT_FALSE(libraryCanOpen(value, true));
}
TEST(LibraryTrial, CheckedButImpossibleResultNeverPasses) {
  auto value = admitted();
  for (uint32_t flags : {ORIGINAL_FRAME, OWNED_ROW, OWNED_FRAME, ORIGINAL_ROW | OWNED_FRAME}) {
    value.flags = flags;
    value.check = libraryChecksum(value);
    EXPECT_FALSE(validLibraryEvidence(value, value.run));
  }
  value = admitted();
  value.phase = LibraryPhase::Completed;
  value.check = libraryChecksum(value);
  EXPECT_FALSE(validLibraryEvidence(value, value.run));
  value.flags = LIBRARY_FRAMES;
  value.opened = 1;
  value.check = libraryChecksum(value);
  EXPECT_TRUE(validLibraryEvidence(value, value.run));
}
TEST(LibraryTrial, NewActionCannotDowngradeOrLoseRequiredPaintAndLegacyStaysCompatible) {
  EXPECT_TRUE(isWalkActionTicket(LIBRARY_ACTION_TICKET));
  EXPECT_TRUE(paintEvidenceRequired(LIBRARY_ACTION_TICKET, false, false));
  EXPECT_FALSE(paintAllowsCleanup(LIBRARY_ACTION_TICKET, false, false, false));
  EXPECT_FALSE(walkRecoveryMatches(LIBRARY_ACTION_TICKET, 743, true, 744));
  EXPECT_TRUE(paintAllowsCleanup(4, false, false, false));
  EXPECT_FALSE(paintAllowsCleanup(5, false, false, false));
  for (uint32_t action = 0; action < 4; ++action) {
    EXPECT_FALSE(isWalkActionTicket(action));
    EXPECT_TRUE(walkRecoveryMatches(action, 743, false, 0));
    EXPECT_FALSE(paintEvidenceRequired(action, false, false));
  }
}

TEST(LibraryTrial, CleanupCannotEraseForeignOrCorruptRequest) {
  EXPECT_TRUE(libraryRequestMatches(false, 0, 12));
  EXPECT_TRUE(libraryRequestMatches(true, 12, 12));
  EXPECT_FALSE(libraryRequestMatches(true, 13, 12));
  EXPECT_FALSE(libraryRequestMatches(true, 0, 12));
  EXPECT_FALSE(libraryRequestMatches(false, 0, 0));
}
