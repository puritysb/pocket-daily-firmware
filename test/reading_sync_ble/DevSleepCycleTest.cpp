#include <HalStorage.h>
#include <gtest/gtest.h>

#include "pocket_daily/boot/DevBootReturn.h"
#include "pocket_daily/dev/CaptureTicket.h"
#include "pocket_daily/nearby_sync/DevSleepCycle.h"
#include "pocket_daily/product_identity.h"

using namespace Pocket::NearbySync;
namespace {
Stats::Record baseline;
class DevCycle : public ::testing::Test {
 protected:
  void SetUp() override {
    DevSleepCycle::complete(nullptr);
    FakeSD::reset();
    baseline = {};
    Stats::begin(baseline);
    baseline.opened = 5;
    baseline.lists = 9;
    Stats::seal(baseline);
  }
};
}  // namespace
namespace Pocket::NearbySync::Window {
const Stats::Record* statistics() { return &baseline; }
}  // namespace Pocket::NearbySync::Window

TEST_F(DevCycle, ExecutesOnceAndPrearmsLanReturnBeforeReclaimingMemory) {
  ASSERT_TRUE(DevSleepCycle::request(1234567));
  auto& marker = FakeSD::files[PocketDaily::DEV_BOOT_FILE_TRANSFER_MARKER];
  ASSERT_EQ(marker.size(), 1U);
  EXPECT_EQ(PocketDaily::Boot::decodeDevBootMarker(reinterpret_cast<const char*>(marker.data()), marker.size()),
            PocketDaily::Boot::DevBootReturn::BleSleepCycle);
  ASSERT_TRUE(DevSleepCycle::begin());
  EXPECT_EQ(marker[0], 'S');
  EXPECT_TRUE(DevSleepCycle::active());
  DevSleepCycle::frameReleased(81001, 40101, 133289, 70001, 52272, true);
  auto result = baseline;
  result.opened = 6;
  result.lists = 11;
  Stats::seal(result);
  ASSERT_TRUE(DevSleepCycle::complete(&result));
  DevSleepCycle::Record record;
  ASSERT_TRUE(DevSleepCycle::read(record));
  EXPECT_EQ(record.run, 1234567U);
  EXPECT_EQ(record.state, DevSleepCycle::State::Completed);
  EXPECT_EQ(record.beforeFree, 81001U);
  EXPECT_EQ(record.afterFree, 133289U);
  EXPECT_EQ(record.beforeBlock, 40101U);
  EXPECT_EQ(record.afterBlock, 70001U);
  EXPECT_EQ(record.frameBytes, 52272U);
  EXPECT_EQ(record.released, 1U);
  EXPECT_EQ(record.result.opened - record.baseline.opened, 1);
  EXPECT_EQ(record.result.lists - record.baseline.lists, 2);
  EXPECT_FALSE(DevSleepCycle::active());
  EXPECT_FALSE(DevSleepCycle::begin());
}

TEST_F(DevCycle, PartialAndCorruptRequestsCannotArmBoot) {
  EXPECT_FALSE(DevSleepCycle::request(0));
  FakeSD::writeBudget = 3;
  EXPECT_FALSE(DevSleepCycle::request(42));
  EXPECT_EQ(FakeSD::files.count(PocketDaily::DEV_BOOT_FILE_TRANSFER_MARKER), 0U);
  FakeSD::writeBudget = 10000;
  FakeSD::corruptWrite = true;
  EXPECT_FALSE(DevSleepCycle::request(42));
  EXPECT_EQ(FakeSD::files.count(PocketDaily::DEV_BOOT_FILE_TRANSFER_MARKER), 0U);
  EXPECT_FALSE(DevSleepCycle::begin());
}

TEST_F(DevCycle, CannotBeginWithoutVerifiedReturnMarker) {
  ASSERT_TRUE(DevSleepCycle::request(77));
  FakeSD::failWriteOpen = true;
  EXPECT_FALSE(DevSleepCycle::begin());
  EXPECT_FALSE(DevSleepCycle::active());
}

TEST_F(DevCycle, DamagedResultIsNotReportedAsEvidence) {
  ASSERT_TRUE(DevSleepCycle::request(81));
  auto& bytes = FakeSD::files[DevSleepCycle::RESULT_PATH];
  bytes[8] ^= 1;
  DevSleepCycle::Record record;
  EXPECT_FALSE(DevSleepCycle::read(record));
  EXPECT_FALSE(DevSleepCycle::begin());
}

TEST_F(DevCycle, TimerModeRequiresExplicitRequestAndRecordsArming) {
  ASSERT_TRUE(DevSleepCycle::request(92, DevSleepCycle::ReturnMode::TimedDeepSleep));
  ASSERT_TRUE(DevSleepCycle::begin());
  EXPECT_TRUE(DevSleepCycle::timerRequested());
  DevSleepCycle::timerArmed(true);
  ASSERT_TRUE(DevSleepCycle::complete(&baseline));
  DevSleepCycle::Record record;
  ASSERT_TRUE(DevSleepCycle::read(record));
  EXPECT_EQ(record.returnMode, DevSleepCycle::ReturnMode::TimedDeepSleep);
  EXPECT_EQ(record.timerArmed, 1U);
  EXPECT_FALSE(DevSleepCycle::timerRequested());
}

TEST_F(DevCycle, StandbyEvidenceDistinguishesConnectedWifiEntryFromShellEntry) {
  ASSERT_TRUE(DevSleepCycle::request(93, DevSleepCycle::ReturnMode::AppStandby));
  ASSERT_TRUE(DevSleepCycle::begin());
  ASSERT_TRUE(DevSleepCycle::standbyRequested());
  DevSleepCycle::startedOnWifi(true);
  DevSleepCycle::standbyEvidence(23, 481, 619);
  ASSERT_TRUE(DevSleepCycle::complete(&baseline));
  DevSleepCycle::Record record;
  ASSERT_TRUE(DevSleepCycle::read(record));
  EXPECT_EQ(record.fromWifi, 1U);
  EXPECT_EQ(record.lightSleeps, 23U);
  EXPECT_EQ(record.lightSleepMs, 481U);
  EXPECT_EQ(record.standbyMs, 619U);
  ASSERT_TRUE(DevSleepCycle::request(94, DevSleepCycle::ReturnMode::AppStandby));
  ASSERT_TRUE(DevSleepCycle::begin());
  DevSleepCycle::startedOnWifi(false);
  ASSERT_TRUE(DevSleepCycle::complete(&baseline));
  ASSERT_TRUE(DevSleepCycle::read(record));
  EXPECT_EQ(record.fromWifi, 0U);
}

TEST_F(DevCycle, UnverifiedBootMarkerIsRemoved) {
  FakeSD::failReadOpen = PocketDaily::DEV_BOOT_FILE_TRANSFER_MARKER;
  EXPECT_FALSE(DevSleepCycle::request(93));
  EXPECT_EQ(FakeSD::files.count(PocketDaily::DEV_BOOT_FILE_TRANSFER_MARKER), 0U);
}

TEST_F(DevCycle, InterruptedRunReturnsEvidenceWithoutRepeatingTheExperiment) {
  ASSERT_TRUE(DevSleepCycle::request(94));
  ASSERT_TRUE(DevSleepCycle::begin());
  const auto interruptedRecord = FakeSD::files[DevSleepCycle::RESULT_PATH];
  ASSERT_TRUE(DevSleepCycle::complete(&baseline));
  // Simulate the SD bytes left by a reset while the loop was running.
  FakeSD::files[DevSleepCycle::RESULT_PATH] = interruptedRecord;
  EXPECT_TRUE(DevSleepCycle::interruptIfRunning());
  DevSleepCycle::Record record;
  ASSERT_TRUE(DevSleepCycle::read(record));
  EXPECT_EQ(record.state, DevSleepCycle::State::Interrupted);
  EXPECT_FALSE(DevSleepCycle::begin());
  EXPECT_FALSE(DevSleepCycle::active());
  EXPECT_FALSE(DevSleepCycle::interruptIfRunning());
}

TEST_F(DevCycle, InterruptedTrialRetainsLastVerifiedCheckpointAndBattery) {
  ASSERT_TRUE(DevSleepCycle::request(95, DevSleepCycle::ReturnMode::AppStandby));
  ASSERT_TRUE(DevSleepCycle::begin());
  DevSleepCycle::startedOnWifi(true);
  DevSleepCycle::frameReleased(70001, 38001, 122273, 61001, 52272, true);
  ASSERT_TRUE(DevSleepCycle::checkpoint(DevSleepCycle::Checkpoint::BeforeLightSleep, 67, &baseline));
  const auto retained = FakeSD::files[DevSleepCycle::RESULT_PATH];
  ASSERT_TRUE(DevSleepCycle::complete(&baseline));
  FakeSD::files[DevSleepCycle::RESULT_PATH] = retained;
  ASSERT_TRUE(DevSleepCycle::interruptIfRunning());
  DevSleepCycle::Record record;
  ASSERT_TRUE(DevSleepCycle::read(record));
  EXPECT_EQ(record.state, DevSleepCycle::State::Interrupted);
  EXPECT_EQ(record.checkpoint, DevSleepCycle::Checkpoint::BeforeLightSleep);
  EXPECT_EQ(record.batteryPercent, 67U);
  EXPECT_EQ(record.fromWifi, 1U);
  EXPECT_EQ(record.afterFree, 122273U);
  EXPECT_TRUE(Stats::valid(record.result));
  EXPECT_FALSE(DevSleepCycle::checkpoint(DevSleepCycle::Checkpoint::StandbyAlive, 60));
}

TEST(DevCaptureTicket, RejectsPartialCorruptAndZeroWhileAcceptingUnalignedBytes) {
  using namespace PocketDaily::DevCapture;
  const auto ticket = ticketFor(0x12345678);
  uint8_t unaligned[sizeof(ticket) + 1]{};
  memcpy(unaligned + 1, &ticket, sizeof(ticket));
  EXPECT_EQ(decodeTicket(unaligned + 1, sizeof(ticket)), 0x12345678U);
  for (size_t size = 0; size < sizeof(ticket); ++size) EXPECT_EQ(decodeTicket(unaligned + 1, size), 0U);
  for (size_t byte = 1; byte <= sizeof(ticket); ++byte) {
    unaligned[byte] ^= 1;
    EXPECT_EQ(decodeTicket(unaligned + 1, sizeof(ticket)), 0U);
    unaligned[byte] ^= 1;
  }
  const auto zero = ticketFor(0);
  EXPECT_EQ(decodeTicket(&zero, sizeof(zero)), 0U);
  EXPECT_EQ(decodeTicket(nullptr, sizeof(ticket)), 0U);
  EXPECT_EQ(PocketDaily::Boot::decodeDevBootMarker("R", 1), PocketDaily::Boot::DevBootReturn::ReaderCapture);
}
