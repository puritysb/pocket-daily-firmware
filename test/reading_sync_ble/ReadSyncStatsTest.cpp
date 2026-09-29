// Exchange-window figures kept in RTC_NOINIT memory (ReadSyncStats).
#include <gtest/gtest.h>

#include <cstring>

#include "pocket_daily/nearby_sync/ReadSyncStats.h"

using namespace Pocket::NearbySync;

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
