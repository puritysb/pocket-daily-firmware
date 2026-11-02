// Exchange-window figures kept in RTC_NOINIT memory (ReadSyncStats).
#include <gtest/gtest.h>

#include <cstring>

#include "pocket_daily/nearby_sync/ReadSyncStats.h"

using namespace Pocket::NearbySync;

TEST(HomePaintTiming, SeparatesCompositionFromRefreshAndDetectsCorruption) {
  Stats::HomePaintRecord r{};
  EXPECT_FALSE(Stats::valid(r));
  Stats::notePaint(r, 5400, 958, 4800, 60000, 200, 250, 150, 160);
  ASSERT_TRUE(Stats::valid(r));
  EXPECT_EQ(r.composeMs, 5400u);
  EXPECT_EQ(r.refreshMs, 958u);
  EXPECT_EQ(r.coverMs, 4800u);
  EXPECT_EQ(r.minFreeHeap, 60000u);
  EXPECT_EQ(r.resolveMs, 200u);
  EXPECT_EQ(r.headerMs, 250u);
  EXPECT_EQ(r.snapshotMs, 150u);
  EXPECT_EQ(r.cpuMhz, 160u);
  EXPECT_EQ(r.sequence, 1u);
  Stats::notePaint(r, 120, 700);
  EXPECT_EQ(r.sequence, 2u);
  EXPECT_EQ(r.composeMs, 120u);
  EXPECT_EQ(r.refreshMs, 700u);
  EXPECT_EQ(r.coverMs, 0u);
  r.refreshMs ^= 1;
  EXPECT_FALSE(Stats::valid(r));
}

TEST(HomeReturnTiming, MeasuresEachBoundaryAndRetainsReceiptAcrossCopy) {
  Stats::HomeReturnRecord r{};
  using Stage = Stats::ReturnStage;
  ASSERT_TRUE(Stats::noteReturn(r, Stage::InputDetected, 1000));
  ASSERT_TRUE(Stats::noteReturn(r, Stage::Requested, 1020));
  ASSERT_TRUE(Stats::noteReturn(r, Stage::ReaderExited, 1130));
  ASSERT_TRUE(Stats::noteReturn(r, Stage::HomeEntered, 1470));
  ASSERT_TRUE(Stats::noteReturn(r, Stage::HomePainted, 8020));
  const auto retained = r;
  ASSERT_TRUE(Stats::valid(retained));
  EXPECT_EQ(retained.requestedMs, 20u);
  EXPECT_EQ(retained.exitedMs, 130u);
  EXPECT_EQ(retained.enteredMs, 470u);
  EXPECT_EQ(retained.paintedMs, 7020u);
  EXPECT_FALSE(Stats::noteReturn(r, Stage::HomePainted, 9000));
  ASSERT_TRUE(Stats::noteReturn(r, Stage::InputDetected, 10000));
  EXPECT_EQ(r.sequence, retained.sequence + 1);
  EXPECT_EQ(r.paintedMs, 0u);
}

TEST(HomeReturnTiming, RejectsGarbageSkippedStagesAndCorruption) {
  Stats::HomeReturnRecord r;
  memset(&r, 0xA5, sizeof(r));
  using Stage = Stats::ReturnStage;
  EXPECT_FALSE(Stats::valid(r));
  EXPECT_FALSE(Stats::noteReturn(r, Stage::HomeEntered, 10));
  ASSERT_TRUE(Stats::noteReturn(r, Stage::InputDetected, 20));
  EXPECT_FALSE(Stats::noteReturn(r, Stage::HomePainted, 30));
  EXPECT_FALSE(Stats::noteReturn(r, static_cast<Stage>(6), 30));
  r.started ^= 1;
  EXPECT_FALSE(Stats::valid(r));
  EXPECT_FALSE(Stats::noteReturn(r, Stage::ReaderExited, 40));
}

TEST(HomeReturnTiming, HandlesMillisWrap) {
  Stats::HomeReturnRecord r{};
  using Stage = Stats::ReturnStage;
  ASSERT_TRUE(Stats::noteReturn(r, Stage::InputDetected, UINT32_MAX - 19));
  ASSERT_TRUE(Stats::noteReturn(r, Stage::Requested, 0));
  ASSERT_TRUE(Stats::noteReturn(r, Stage::ReaderExited, 10));
  EXPECT_EQ(r.exitedMs, 30u);
}

TEST(ReadSyncStats, GarbageAfterPowerOnStartsAFreshRecord) {
  Stats::Record record;
  memset(&record, 0xA5, sizeof(record));
  EXPECT_FALSE(Stats::valid(record));
  Stats::begin(record);
  EXPECT_TRUE(Stats::valid(record));
  EXPECT_EQ(record.opened, 0U);
  EXPECT_EQ(record.minFree, 0U);
}

TEST(ReadSyncStats, RecordsSkipsOpeningsLowWaterAndRelease) {
  Stats::Record record;
  memset(&record, 0, sizeof(record));
  Stats::startAttempt(record, 1, 4, 30000, 20000, false);
  EXPECT_EQ(record.skipped, 1U);
  EXPECT_EQ(record.lastGate, 4U);
  EXPECT_EQ(record.startFree, 30000U);

  Stats::startAttempt(record, 0, 0, 90000, 60000, true);
  Stats::ready(record, 52000, 30000, true);
  EXPECT_EQ(record.opened, 1U);
  EXPECT_EQ(record.minFree, 52000U);
  Stats::sample(record, 48000, 31000);
  Stats::sample(record, 50000, 26000);
  EXPECT_EQ(record.minFree, 48000U);
  EXPECT_EQ(record.minBlock, 26000U);
  Stats::closed(record, 0, 1, 1, 2, 89000, 59000);
  EXPECT_EQ(record.connections, 1U);
  EXPECT_EQ(record.offers, 2U);
  EXPECT_EQ(record.closedFree, 89000U);
  EXPECT_TRUE(Stats::valid(record));

  Stats::ready(record, 18000, 7000, false);
  EXPECT_EQ(record.refused, 1U);

  record.offers = 99;  // a stray write is detected by the check word
  EXPECT_FALSE(Stats::valid(record));
}
