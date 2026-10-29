#include <Epub/ImagePaintDiagnostics.h>
#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string>

#include "../../src/pocket_daily/dev/CaptureTicket.h"

namespace {
using namespace epub_paint_diagnostics;
TEST(ImagePaintDiagnostics, InactiveAndUnrelatedPngCallsCannotAcquireRunEvidence) {
  Observer o;
  o.rendered();
  o.cacheHit();
  o.lazyResult(false, 5, 20, 10);
  o.decoderStarted();
  o.pngStarted(100, 90, 80, 70);
  o.pngFailed(Stage::PngOpen, -7, 60, 50);
  o.decoderResult(false, 40, 30);
  o.placeholder(true);
  EXPECT_EQ(o.value.run, 0U);
  EXPECT_EQ(o.value.renderCalls, 0U);
  EXPECT_EQ(o.value.pngAttempts, 0U);
  EXPECT_EQ(o.value.firstFailureStage, 0U);
  o.begin(9);
  o.pngStarted(100, 90, 80, 70);
  o.pngFailed(Stage::PngOpen, -7, 60, 50);
  EXPECT_EQ(o.value.pngAttempts, 0U);
  EXPECT_EQ(o.value.firstFailureStage, 0U);
  o.rendered();
  o.decoderStarted();
  o.pngStarted(100, 90, 80, 70);
  o.pngFailed(Stage::PngOpen, -7, 60, 50);
  o.decoderResult(false, 40, 30);
  o.placeholder();
  EXPECT_EQ(o.value.pngAttempts, 1U);
  EXPECT_EQ(o.value.firstFailureStage, static_cast<uint32_t>(Stage::PngOpen));
  EXPECT_EQ(o.value.firstFailureCode, -7);
  o.value.complete = 1;
  o.begin(10);
  EXPECT_EQ(o.value.run, 10U);
  EXPECT_EQ(o.value.complete, 0U);
  EXPECT_EQ(o.value.firstFailureStage, 0U);
  EXPECT_EQ(o.value.renderCalls, 0U);
  EXPECT_EQ(o.value.pngObjectBytes, 0U);
}
TEST(ImagePaintDiagnostics, FirstFailureAndFirstPngMemorySurviveLaterPassesAndSuccess) {
  Observer o;
  o.begin(31);
  o.rendered();
  o.lazyResult(true, 0, 78000, 65000);
  o.decoderStarted();
  o.pngStarted(62000, 48000, 59000, 61440);
  o.pngFailed(Stage::PngAllocation, 0, 61000, 47000);
  o.decoderResult(false, 80000, 70000);
  o.placeholder();
  o.rendered();
  o.placeholder(true);
  o.rendered();
  o.decoderStarted();
  o.pngStarted(90000, 80000, 59000, 61440);
  o.decoderResult(true, 88000, 78000);
  o.rendered();
  o.cacheHit();
  EXPECT_EQ(o.value.renderCalls, 4U);
  EXPECT_EQ(o.value.cacheHits, 1U);
  EXPECT_EQ(o.value.pngAttempts, 2U);
  EXPECT_EQ(o.value.decoderAttempts, 2U);
  EXPECT_EQ(o.value.decoderSuccesses, 1U);
  EXPECT_EQ(o.value.placeholderCalls, 2U);
  EXPECT_EQ(o.value.suppressedCalls, 1U);
  EXPECT_EQ(o.value.pngEntryFreeHeap, 62000U);
  EXPECT_EQ(o.value.pngEntryLargestBlock, 48000U);
  EXPECT_EQ(o.value.firstFailureStage, static_cast<uint32_t>(Stage::PngAllocation));
  EXPECT_EQ(o.value.firstFailureFreeHeap, 61000U);
  EXPECT_EQ(o.value.firstFailureLargestBlock, 47000U);
  o.value.renderCalls = UINT32_MAX;
  o.rendered();
  EXPECT_EQ(o.value.renderCalls, UINT32_MAX);
}
TEST(ImagePaintDiagnostics, LazyErrorAndEveryFailureStageRemainDistinct) {
  Observer o;
  o.begin(1);
  o.rendered();
  o.lazyResult(false, 5, 55000, 40000);
  o.fail(Stage::FileOpen, 0, 70000, 65000);
  o.placeholder();
  EXPECT_EQ(o.value.lazyExtractAttempts, 1U);
  EXPECT_EQ(o.value.lazyExtractFailures, 1U);
  EXPECT_EQ(o.value.lastLazyExtractError, 5U);
  EXPECT_EQ(o.value.firstFailureStage, 1U);
  EXPECT_EQ(o.value.firstFailureCode, 5);
  EXPECT_EQ(o.value.firstFailureFreeHeap, 55000U);
  for (uint32_t stage = 1; stage <= static_cast<uint32_t>(Stage::Bounds); ++stage) {
    o.begin(stage);
    o.fail(Stage::None, 0, 1, 1);
    EXPECT_EQ(o.value.firstFailureStage, 0U);
    o.fail(static_cast<Stage>(stage), -static_cast<int32_t>(stage), 100 + stage, 70 + stage);
    EXPECT_EQ(o.value.firstFailureStage, stage);
    EXPECT_EQ(o.value.firstFailureCode, -static_cast<int32_t>(stage));
    EXPECT_EQ(o.value.firstFailureLargestBlock, 70 + stage);
  }
}
TEST(ImagePaintDiagnostics, CheckedRecordRejectsForeignIncompleteBytesAndSemanticallyInvalidPayloads) {
  Observer o;
  o.begin(123);
  o.rendered();
  o.decoderStarted();
  o.pngStarted(62000, 45000, 59000, 61440);
  o.pngFailed(Stage::PngOpen, -19, 61000, 44000);
  o.decoderResult(false, 70000, 50000);
  o.placeholder();
  uint8_t bytes[RECORD_BYTES];
  Snapshot result;
  encode(o.value, bytes);
  ASSERT_TRUE(decode(bytes, sizeof(bytes), 123, result));
  EXPECT_EQ(result.complete, 0U);
  o.value.complete = 1;
  encode(o.value, bytes);
  ASSERT_TRUE(decode(bytes, sizeof(bytes), 123, result));
  EXPECT_EQ(result.complete, 1U);
  EXPECT_EQ(result.firstFailureCode, -19);
  EXPECT_EQ(result.pngObjectBytes, 59000U);
  EXPECT_FALSE(decode(bytes, sizeof(bytes), 124, result));
  EXPECT_FALSE(decode(bytes, sizeof(bytes), 0, result));
  EXPECT_FALSE(decode(nullptr, sizeof(bytes), 123, result));
  EXPECT_FALSE(decode(bytes, sizeof(bytes) + 1, 123, result));
  for (size_t n = 0; n < sizeof(bytes); ++n) EXPECT_FALSE(decode(bytes, n, 123, result));
  for (auto& b : bytes) {
    b ^= 1;
    EXPECT_FALSE(decode(bytes, sizeof(bytes), 123, result));
    b ^= 1;
  }
  for (auto field :
       {&Snapshot::complete, &Snapshot::lastLazyExtractError, &Snapshot::firstFailureStage, &Snapshot::cacheHits,
        &Snapshot::lazyExtractAttempts, &Snapshot::lazyExtractFailures, &Snapshot::decoderAttempts,
        &Snapshot::decoderSuccesses, &Snapshot::placeholderCalls, &Snapshot::suppressedCalls, &Snapshot::pngAttempts}) {
    auto invalid = o.value;
    invalid.*field = 100;
    encode(invalid, bytes);
    EXPECT_FALSE(decode(bytes, sizeof(bytes), 123, result));
  }
  auto invalid = o.value;
  invalid.firstFailureStage = 0;
  encode(invalid, bytes);
  EXPECT_FALSE(decode(bytes, sizeof(bytes), 123, result));
  invalid = o.value;
  invalid.pngAttempts = 0;
  encode(invalid, bytes);
  EXPECT_FALSE(decode(bytes, sizeof(bytes), 123, result));
}
TEST(ImagePaintDiagnostics, MaximumJsonAndTruncationNeverOverrunTheAdvertisedBound) {
  Snapshot s{UINT32_MAX, 0,          UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, 9,
             UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
             UINT32_MAX, UINT32_MAX, 16,         INT32_MIN,  UINT32_MAX, UINT32_MAX};
  std::array<char, MAX_JSON_BYTES> json{};
  const int size = formatJson(json.data(), json.size(), &s);
  ASSERT_GT(size, 0);
  EXPECT_LT(static_cast<size_t>(size), json.size());
  EXPECT_EQ(size, 577);
  EXPECT_EQ(std::string(json.data()).size(), static_cast<size_t>(size));
  EXPECT_NE(std::string(json.data()).find("\"firstFailureCode\":-2147483648"), std::string::npos);
  EXPECT_NE(std::string(json.data()).find("\"complete\":false"), std::string::npos);
  std::array<char, 9> small;
  small.fill('!');
  EXPECT_EQ(formatJson(small.data(), 8, &s), size);
  EXPECT_EQ(small[7], '\0');
  EXPECT_EQ(small[8], '!');
  EXPECT_EQ(formatJson(json.data(), json.size(), nullptr), 4);
  EXPECT_STREQ(json.data(), "null");
}
TEST(ImagePaintDiagnostics, PersistedNewWalkCannotDowngradeMissingPaintToLegacyCleanup) {
  using namespace PocketDaily::DevCapture;
  const auto legacy = static_cast<uint32_t>(ReaderAction::Walk);
  EXPECT_TRUE(walkRecoveryMatches(legacy, 8, true, 8));
  EXPECT_TRUE(walkRecoveryMatches(PAINT_WALK_ACTION, 8, true, 8));
  EXPECT_FALSE(walkRecoveryMatches(PAINT_WALK_ACTION, 8, true, 9));
  EXPECT_FALSE(walkRecoveryMatches(legacy, 8, true, 9));
  EXPECT_FALSE(walkRecoveryMatches(PAINT_WALK_ACTION, 8, false, 8));
  EXPECT_TRUE(walkRecoveryMatches(static_cast<uint32_t>(ReaderAction::Seed), 8, false, 0));
  EXPECT_TRUE(isWalkActionTicket(legacy));
  EXPECT_TRUE(isWalkActionTicket(PAINT_WALK_ACTION));
  EXPECT_TRUE(isWalkActionTicket(LIBRARY_ACTION_TICKET));
  EXPECT_TRUE(walkRecoveryMatches(LIBRARY_ACTION_TICKET, 8, true, 8));
  EXPECT_FALSE(walkRecoveryMatches(LIBRARY_ACTION_TICKET, 8, true, 9));
  EXPECT_FALSE(walkRecoveryMatches(LIBRARY_ACTION_TICKET, 8, false, 8));
  EXPECT_TRUE(paintAllowsCleanup(legacy, false, false, false));
  EXPECT_FALSE(paintAllowsCleanup(PAINT_WALK_ACTION, false, false, false));
  for (bool marker : {false, true})
    for (bool record : {false, true}) {
      EXPECT_TRUE(paintEvidenceRequired(PAINT_WALK_ACTION, marker, record));
      EXPECT_FALSE(paintAllowsCleanup(PAINT_WALK_ACTION, marker, record, false));
      EXPECT_TRUE(paintAllowsCleanup(PAINT_WALK_ACTION, marker, record, true));
      EXPECT_TRUE(paintEvidenceRequired(LIBRARY_ACTION_TICKET, marker, record));
      EXPECT_FALSE(paintAllowsCleanup(LIBRARY_ACTION_TICKET, marker, record, false));
      EXPECT_TRUE(paintAllowsCleanup(LIBRARY_ACTION_TICKET, marker, record, true));
      if (marker || record) EXPECT_FALSE(paintAllowsCleanup(legacy, marker, record, false));
    }
  for (uint32_t action : {0U, 1U, 2U, 3U, 7U, UINT32_MAX}) {
    EXPECT_FALSE(isWalkActionTicket(action));
    EXPECT_FALSE(paintAllowsCleanup(action, false, false, true));
    if (action < 4) EXPECT_FALSE(paintEvidenceRequired(action, false, false));
  }
  EXPECT_TRUE(readyToCapture(ReaderAction::Capture, ReaderPhase::Reading));
  EXPECT_TRUE(readyToCapture(ReaderAction::Seed, ReaderPhase::Reading));
  EXPECT_TRUE(readyToCapture(ReaderAction::Accept, ReaderPhase::Accepted));
  EXPECT_TRUE(readyToCapture(ReaderAction::Advance, ReaderPhase::Turned));
}
}  // namespace
