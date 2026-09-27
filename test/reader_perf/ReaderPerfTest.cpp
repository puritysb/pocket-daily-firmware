#include <Arduino.h>
#include <HalIoCounters.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "pocket_daily/ReaderPerf.h"

namespace Perf = PocketDaily::ReaderPerf;
namespace Clock = ReaderPerfTestClock;

namespace {
Perf::TurnRecord record(const uint16_t base) {
  Perf::TurnRecord r;
  for (size_t i = 0; i < Perf::STAGE_COUNT; ++i) r.ms[i] = static_cast<uint16_t>(base + i);
  for (size_t i = 0; i < Perf::COUNTER_COUNT; ++i) r.counters[i] = static_cast<uint16_t>(base * 2 + i);
  r.strips = 14;
  r.flags = Perf::FLAG_AA;
  r.freeHeap = 50000U + base;
  r.largestBlock = 20000U + base;
  return r;
}

std::unique_ptr<Perf::Snapshot> snapshotWith(const int turns) {
  auto s = std::make_unique<Perf::Snapshot>();
  std::strcpy(s->version, "1.7.0-test");
  for (int i = 0; i < turns; ++i) Perf::append(*s, record(static_cast<uint16_t>(i)));
  return s;
}

std::vector<uint8_t> encoded(const Perf::Snapshot& s) {
  std::vector<uint8_t> bytes(Perf::FILE_BYTES);
  EXPECT_EQ(Perf::encode(s, bytes.data(), bytes.size()), Perf::FILE_BYTES);
  return bytes;
}

// One complete turn on the live recorder: 5 ms queue wait, 40 ms prewarm, 300 ms BW refresh.
void recordTurn(const uint8_t flags) {
  const uint32_t input = Clock::nowMs;
  Clock::nowMs += 5;
  Perf::beginTurn(input, flags);
  halIoCounters.opens += 3;
  halIoCounters.readCalls += 100;
  halIoCounters.readBytes += 90 * 1024;
  Clock::nowMs += 40;
  Perf::mark(Perf::STAGE_PREWARM);
  Perf::noteGlyphs(120);
  Perf::sampleHeap();
  Clock::nowMs += 300;
  Perf::mark(Perf::STAGE_BW_REFRESH);
  Perf::noteStrip();
  Perf::endTurn();
}

class ReaderPerf : public testing::Test {
 protected:
  void SetUp() override {
    FakeSD::reset();
    Perf::close();  // no recorder left over from another test
    FakeSD::reset();
    Clock::nowMs = 1000;
    Clock::freeHeap = 100000;
    Clock::largestBlock = 60000;
    halIoCounters = HalIoCounters{};
  }
  void TearDown() override { Perf::close(); }
};
}  // namespace

TEST_F(ReaderPerf, FileRoundTripsRingAndTotals) {
  const auto s = snapshotWith(20);  // wraps the 16-entry ring
  const auto bytes = encoded(*s);
  auto decoded = std::make_unique<Perf::Snapshot>();
  ASSERT_TRUE(Perf::decode(bytes.data(), bytes.size(), *decoded));
  EXPECT_STREQ(decoded->version, "1.7.0-test");
  EXPECT_EQ(decoded->ringCount, Perf::RING_SIZE);
  EXPECT_EQ(decoded->totals.turns, 20U);
  // Newest first: turns 19, 18, ... 4.
  ASSERT_NE(decoded->recent(0), nullptr);
  EXPECT_EQ(decoded->recent(0)->ms[Perf::STAGE_INPUT], 19);
  EXPECT_EQ(decoded->recent(15)->ms[Perf::STAGE_INPUT], 4);
  EXPECT_EQ(decoded->recent(16), nullptr);
  EXPECT_EQ(decoded->recent(0)->counters[Perf::COUNTER_GLYPHS], 19 * 2 + Perf::COUNTER_GLYPHS);
  EXPECT_EQ(decoded->recent(0)->strips, 14);
  EXPECT_EQ(decoded->recent(0)->flags, Perf::FLAG_AA);
  // Sums, maxima and heap minima cover all 20 turns, not only the ring.
  EXPECT_EQ(decoded->totals.sum[Perf::STAGE_INPUT], 190U);
  EXPECT_EQ(decoded->totals.max[Perf::STAGE_TOTAL], 19 + Perf::STAGE_TOTAL);
  EXPECT_EQ(decoded->totals.minFreeHeap, 50000U);
  EXPECT_EQ(decoded->totals.minLargestBlock, 20000U);
}

TEST_F(ReaderPerf, DecodeRejectsDamagedOrForeignFiles) {
  const auto s = snapshotWith(3);
  auto bytes = encoded(*s);
  auto out = std::make_unique<Perf::Snapshot>();
  EXPECT_FALSE(Perf::decode(bytes.data(), bytes.size() - 1, *out));
  auto flipped = bytes;
  flipped[40] ^= 0x10;  // inside the version string: CRC mismatch
  EXPECT_FALSE(Perf::decode(flipped.data(), flipped.size(), *out));
  auto magic = bytes;
  magic[0] = 'X';
  EXPECT_FALSE(Perf::decode(magic.data(), magic.size(), *out));
  EXPECT_FALSE(Perf::decode(nullptr, bytes.size(), *out));
  // The caller's snapshot is untouched by a failed decode.
  EXPECT_EQ(out->totals.turns, 0U);
  EXPECT_TRUE(Perf::decode(bytes.data(), bytes.size(), *out));
  EXPECT_EQ(out->totals.turns, 3U);
}

TEST_F(ReaderPerf, FormatsRecordsAndTotalsInFieldOrder) {
  const auto s = snapshotWith(2);
  char line[200];
  ASSERT_GT(Perf::formatRecord(*s->recent(0), line, sizeof(line)), 0U);
  EXPECT_STREQ(line, "1,2,3,4,5,6,7,8,9,10,11,12,2,3,4,5,14,16,50001,20001");
  ASSERT_GT(Perf::formatTotals(s->totals, false, line, sizeof(line)), 0U);
  // Averages of 0..11 and 1..12 round half up; counters of (0,1,2,3) and (2,3,4,5).
  EXPECT_STREQ(line, "1,2,3,4,5,6,7,8,9,10,11,12,1,2,3,4");
  ASSERT_GT(Perf::formatTotals(s->totals, true, line, sizeof(line)), 0U);
  EXPECT_STREQ(line, "1,2,3,4,5,6,7,8,9,10,11,12,2,3,4,5");
  const std::string names = Perf::fieldNames();
  EXPECT_EQ(std::count(names.begin(), names.end(), ','), 19);
  // A too-small buffer yields an empty string, never a truncated list.
  char tiny[8];
  EXPECT_EQ(Perf::formatRecord(*s->recent(0), tiny, sizeof(tiny)), 0U);
  EXPECT_STREQ(tiny, "");
}

TEST_F(ReaderPerf, RecorderSplitsStagesAndCountsSdTraffic) {
  Perf::open();
  recordTurn(Perf::FLAG_CHAPTER);
  ASSERT_TRUE(Perf::save());
  auto stored = std::make_unique<Perf::Snapshot>();
  ASSERT_TRUE(Perf::load(*stored));
  const Perf::TurnRecord* turn = stored->recent(0);
  ASSERT_NE(turn, nullptr);
  EXPECT_EQ(turn->ms[Perf::STAGE_INPUT], 5);
  EXPECT_EQ(turn->ms[Perf::STAGE_PREWARM], 40);
  EXPECT_EQ(turn->ms[Perf::STAGE_BW_REFRESH], 300);
  EXPECT_EQ(turn->ms[Perf::STAGE_TOTAL], 345);
  EXPECT_EQ(turn->counters[Perf::COUNTER_SD_OPENS], 3);
  EXPECT_EQ(turn->counters[Perf::COUNTER_SD_READS], 100);
  EXPECT_EQ(turn->counters[Perf::COUNTER_SD_KB], 90);
  EXPECT_EQ(turn->counters[Perf::COUNTER_GLYPHS], 120);
  EXPECT_EQ(turn->strips, 1);
  EXPECT_EQ(turn->flags, Perf::FLAG_CHAPTER);
  EXPECT_EQ(turn->freeHeap, 100000U);
  EXPECT_EQ(turn->largestBlock, 60000U);
  EXPECT_STREQ(stored->version, "1.7.0-test");
}

TEST_F(ReaderPerf, AbortedTurnsAndCallsWithoutARecorderLeaveNoTrace) {
  recordTurn(0);  // not open: nothing recorded, nothing written
  EXPECT_FALSE(Perf::turnActive());
  EXPECT_EQ(Perf::unsavedTurns(), 0);
  EXPECT_TRUE(Perf::save());
  EXPECT_TRUE(FakeSD::files.empty());

  Perf::open();
  Perf::beginTurn(0, 0);
  Perf::abortTurn();
  Perf::mark(Perf::STAGE_PAGE);
  Perf::endTurn();
  EXPECT_EQ(Perf::unsavedTurns(), 0);
}

TEST_F(ReaderPerf, HistoryContinuesOnlyForTheSameFirmware) {
  Perf::open();
  recordTurn(0);
  recordTurn(Perf::FLAG_BACK);
  EXPECT_EQ(Perf::unsavedTurns(), 2);
  Perf::close();
  ASSERT_EQ(FakeSD::files.count(Perf::PATH), 1U);

  Perf::open();
  recordTurn(0);
  Perf::close();
  auto stored = std::make_unique<Perf::Snapshot>();
  ASSERT_TRUE(Perf::load(*stored));
  EXPECT_EQ(stored->totals.turns, 3U);

  // A file from another build starts a new history.
  std::strcpy(stored->version, "1.6.9");
  auto bytes = encoded(*stored);
  FakeSD::files[Perf::PATH] = bytes;
  Perf::open();
  recordTurn(0);
  Perf::close();
  ASSERT_TRUE(Perf::load(*stored));
  EXPECT_EQ(stored->totals.turns, 1U);
  EXPECT_STREQ(stored->version, "1.7.0-test");
}

TEST_F(ReaderPerf, DeferredWorkIsChargedToTheNewestTurnButNotItsTotal) {
  Perf::open();
  recordTurn(0);
  ASSERT_TRUE(Perf::save());
  EXPECT_EQ(Perf::unsavedTurns(), 0);
  Perf::addToLastTurn(Perf::STAGE_SAVE, 37);
  EXPECT_EQ(Perf::unsavedTurns(), 1);
  Perf::addToLastTurn(Perf::STAGE_TOTAL, 1000);  // ignored: totals stay turn-bound
  Perf::close();
  auto stored = std::make_unique<Perf::Snapshot>();
  ASSERT_TRUE(Perf::load(*stored));
  EXPECT_EQ(stored->recent(0)->ms[Perf::STAGE_SAVE], 37);
  EXPECT_EQ(stored->recent(0)->ms[Perf::STAGE_TOTAL], 345);
  EXPECT_EQ(stored->totals.sum[Perf::STAGE_SAVE], 37U);
  EXPECT_EQ(stored->totals.max[Perf::STAGE_SAVE], 37);
}

TEST_F(ReaderPerf, SaveFailureKeepsThePreviousFile) {
  Perf::open();
  recordTurn(0);
  ASSERT_TRUE(Perf::save());
  const auto before = FakeSD::files[Perf::PATH];
  recordTurn(0);
  FakeSD::writeBudget = 10;  // torn temp write
  EXPECT_FALSE(Perf::save());
  EXPECT_EQ(FakeSD::files[Perf::PATH], before);
  EXPECT_EQ(Perf::unsavedTurns(), 1);
}
