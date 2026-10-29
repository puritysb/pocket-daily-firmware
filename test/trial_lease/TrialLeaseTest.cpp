#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "FakeSDK.h"
#include "HalPowerManager.h"
#include "mbedtls/sha256.h"
#include "network/FirmwareFlasher.h"
#include "network/OtaBootSwitch.h"
#include "pocket_daily/boot/TrialLease.h"
uint32_t silentRebootMagic = 0, silentRebootTarget = 0, silentRebootPayload = 0;
namespace PocketDaily::TrialReader {
bool permitTimerResume = false;
unsigned resumeCalls = 0;
bool beginTimerResume(uint32_t) {
  ++resumeCalls;
  return permitTimerResume;
}
void commitSleep() {}
}  // namespace PocketDaily::TrialReader
namespace Lease = PocketDaily::TrialLease;
namespace Fake {
int resetReason = 3, wakeCause = 0;
std::vector<uint8_t> blob, staged, flash[2];
bool namespaceExists, failCommit, failTimer, failSelect, failRead;
unsigned writes, selections, resets, timerStarts;
uint32_t running, selected;
uint64_t timerMicros;
void (*callback)(void*);
void reset() {
  ::silentRebootMagic = ::silentRebootTarget = ::silentRebootPayload = 0;
  resetReason = 3;
  wakeCause = 0;
  PocketDaily::TrialReader::permitTimerResume = false;
  PocketDaily::TrialReader::resumeCalls = 0;
  blob.clear();
  staged.assign(65536, 0x47);
  std::memcpy(staged.data() + 4090, Lease::kCapability, sizeof(Lease::kCapability));
  flash[0].assign(Lease::kSlotSize, 0x19);
  flash[1].assign(Lease::kSlotSize, 0x82);
  namespaceExists = failCommit = failTimer = failSelect = failRead = false;
  writes = selections = resets = timerStarts = 0;
  running = selected = 0;
  timerMicros = 0;
  callback = nullptr;
}
}  // namespace Fake
namespace {
esp_partition_t parts[2] = {{0, 16, Lease::kSlot0, Lease::kSlotSize, "app0"},
                            {0, 17, Lease::kSlot1, Lease::kSlotSize, "app1"}};
void digest(const std::vector<uint8_t>& data, uint8_t* out) {
  mbedtls_sha256_context c;
  mbedtls_sha256_starts(&c, 0);
  mbedtls_sha256_update(&c, data.data(), data.size());
  mbedtls_sha256_finish(&c, out);
}
std::string returnSHA() {
  uint8_t bytes[32];
  digest(Fake::flash[0], bytes);
  std::string result;
  for (auto byte : bytes) {
    result.push_back("0123456789abcdef"[byte >> 4]);
    result.push_back("0123456789abcdef"[byte & 15]);
  }
  return result;
}
std::string stagedSHA() {
  uint8_t d[32];
  digest(Fake::staged, d);
  std::string s;
  for (auto b : d) {
    s.push_back("0123456789abcdef"[b >> 4]);
    s.push_back("0123456789abcdef"[b & 15]);
  }
  return s;
}
Lease::Record record(Lease::Phase phase = Lease::Phase::Prepared) {
  Lease::Record r;
  r.run = 917;
  r.returnAddress = Lease::kSlot0;
  r.trialAddress = Lease::kSlot1;
  r.imageBytes = 65536;
  r.seconds = 30;
  r.phase = phase;
  digest(Fake::flash[0], r.returnSHA256);
  std::vector<uint8_t> prefix(Fake::flash[1].begin(), Fake::flash[1].begin() + r.imageBytes);
  digest(prefix, r.trialSHA256);
  r.checksum = Lease::checksum(r);
  return r;
}
void persist(Lease::Record r) {
  r.checksum = Lease::checksum(r);
  Fake::namespaceExists = true;
  Fake::blob.resize(sizeof(r));
  std::memcpy(Fake::blob.data(), &r, sizeof(r));
}
Lease::Record stored() {
  Lease::Record r;
  EXPECT_TRUE(Lease::read(r));
  return r;
}
class TrialLease : public ::testing::Test {
  void SetUp() override {
    parts[0] = {0, 16, Lease::kSlot0, Lease::kSlotSize, "app0"};
    parts[1] = {0, 17, Lease::kSlot1, Lease::kSlotSize, "app1"};
    Fake::reset();
  }
};
}  // namespace
void delay(unsigned) {}
void esp_restart() {
  ++Fake::resets;
  throw Fake::Restart{};
}
int esp_reset_reason() { return Fake::resetReason; }
int esp_sleep_get_wakeup_cause() { return Fake::wakeCause; }
const esp_partition_t* esp_ota_get_running_partition() { return &parts[Fake::running]; }
const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t*) { return &parts[1 - Fake::running]; }
const esp_partition_t* esp_partition_find_first(int, int sub, const char*) {
  return sub == 16 ? &parts[0] : sub == 17 ? &parts[1] : nullptr;
}
int esp_partition_read(const esp_partition_t* p, size_t off, void* out, size_t n) {
  if (Fake::failRead || !p || off + n > p->size) return ESP_FAIL;
  std::memcpy(out, Fake::flash[p->subtype - 16].data() + off, n);
  return ESP_OK;
}
int esp_timer_create(const esp_timer_create_args_t* a, esp_timer_handle_t* h) {
  if (Fake::failTimer) return ESP_FAIL;
  Fake::callback = a->callback;
  *h = reinterpret_cast<void*>(1);
  return ESP_OK;
}
int esp_timer_start_once(esp_timer_handle_t, uint64_t us) {
  ++Fake::timerStarts;
  Fake::timerMicros = us;
  return ESP_OK;
}
int nvs_open(const char*, int mode, nvs_handle_t* h) {
  if (!Fake::namespaceExists && mode == NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;
  Fake::namespaceExists = true;
  *h = 1;
  return ESP_OK;
}
int nvs_get_blob(nvs_handle_t, const char*, void* out, size_t* n) {
  if (Fake::blob.empty()) return ESP_ERR_NVS_NOT_FOUND;
  if (!out) {
    *n = Fake::blob.size();
    return ESP_OK;
  }
  if (*n < Fake::blob.size()) return ESP_FAIL;
  *n = Fake::blob.size();
  std::memcpy(out, Fake::blob.data(), *n);
  return ESP_OK;
}
int nvs_set_blob(nvs_handle_t, const char*, const void* b, size_t n) {
  ++Fake::writes;
  if (Fake::failCommit) return ESP_FAIL;
  Fake::blob.assign(static_cast<const uint8_t*>(b), static_cast<const uint8_t*>(b) + n);
  return ESP_OK;
}
int nvs_commit(nvs_handle_t) { return Fake::failCommit ? ESP_FAIL : ESP_OK; }
int nvs_erase_key(nvs_handle_t, const char*) {
  Fake::blob.clear();
  return ESP_OK;
}
void nvs_close(nvs_handle_t) {}
namespace firmware_flash {
uint8_t* sharedStagingBuffer() {
  static uint8_t bytes[4096];
  return bytes;
}
}  // namespace firmware_flash
namespace ota_boot {
bool switchTo(const esp_partition_t* p) {
  ++Fake::selections;
  if (Fake::failSelect || !p) return false;
  Fake::selected = p->subtype - 16;
  return true;
}
}  // namespace ota_boot
TEST_F(TrialLease, NoLeaseCreatesNoTimerAndWritesNothing) {
  Lease::onBoot();
  EXPECT_EQ(Fake::writes, 0u);
  EXPECT_EQ(Fake::timerStarts, 0u);
  EXPECT_EQ(Fake::selections, 0u);
}
TEST_F(TrialLease, AllRecordBytesAndChecksumAreGuarded) {
  auto r = record();
  EXPECT_TRUE(Lease::valid(r));
  for (size_t i = 0; i < sizeof(r); ++i) {
    auto bad = r;
    reinterpret_cast<uint8_t*>(&bad)[i] ^= 1;
    EXPECT_FALSE(Lease::valid(bad)) << i;
  }
}
TEST_F(TrialLease, ResealedInvalidLimitsAndSlotsAreRefused) {
  auto r = record();
  r.seconds = 901;
  r.checksum = Lease::checksum(r);
  EXPECT_FALSE(Lease::valid(r));
  r = record();
  r.returnAddress = 0x1000;
  r.checksum = Lease::checksum(r);
  EXPECT_FALSE(Lease::valid(r));
  r = record();
  r.returnAttempts = 3;
  r.checksum = Lease::checksum(r);
  EXPECT_FALSE(Lease::valid(r));
}
TEST_F(TrialLease, PrepareHashesEntireReturnAndCapabilityAcrossBlockBoundary) {
  ASSERT_TRUE(Lease::prepare(917, 30, stagedSHA().c_str(), returnSHA().c_str()));
  auto r = stored();
  EXPECT_EQ(r.imageBytes, Fake::staged.size());
  uint8_t expected[32];
  digest(Fake::flash[0], expected);
  EXPECT_EQ(std::memcmp(r.returnSHA256, expected, 32), 0);
  EXPECT_EQ(Fake::selections, 0u);
}
TEST_F(TrialLease, MissingCapabilityDoesNotProvision) {
  std::fill(Fake::staged.begin(), Fake::staged.end(), 4);
  EXPECT_FALSE(Lease::prepare(917, 30, stagedSHA().c_str(), returnSHA().c_str()));
  EXPECT_EQ(Fake::writes, 0u);
}
TEST_F(TrialLease, WrongStagedHashDoesNotProvision) {
  EXPECT_FALSE(Lease::prepare(917, 30, std::string(64, 'a').c_str(), returnSHA().c_str()));
  EXPECT_EQ(Fake::writes, 0u);
}
TEST_F(TrialLease, MalformedExistingRecordCannotBeOverwritten) {
  Fake::namespaceExists = true;
  Fake::blob = {1, 2, 3};
  EXPECT_FALSE(Lease::prepare(917, 30, stagedSHA().c_str(), returnSHA().c_str()));
  EXPECT_EQ(Fake::blob, (std::vector<uint8_t>{1, 2, 3}));
  EXPECT_EQ(Fake::writes, 0u);
}
TEST_F(TrialLease, FirstTrialBootPersistsRunningBeforeTimer) {
  persist(record());
  Fake::running = 1;
  Lease::onBoot();
  EXPECT_EQ(stored().phase, Lease::Phase::Running);
  EXPECT_EQ(Fake::timerMicros, 30000000u);
  EXPECT_EQ(Fake::selections, 0u);
  EXPECT_TRUE(Lease::wantsSync());
}
TEST_F(TrialLease, TimerCallbackRestartsWithoutFlashOrNVSWork) {
  persist(record());
  Fake::running = 1;
  Lease::onBoot();
  auto writes = Fake::writes;
  ASSERT_NE(Fake::callback, nullptr);
  EXPECT_THROW(Fake::callback(nullptr), Fake::Restart);
  EXPECT_EQ(Fake::writes, writes);
  EXPECT_EQ(Fake::selections, 0u);
}
TEST_F(TrialLease, RepeatedBootReturnsBeforeAnyTimer) {
  persist(record(Lease::Phase::Running));
  Fake::running = 1;
  EXPECT_THROW(Lease::onBoot(), Fake::Restart);
  EXPECT_EQ(Fake::selected, 0u);
  EXPECT_EQ(Fake::timerStarts, 0u);
  EXPECT_EQ(stored().phase, Lease::Phase::Returning);
  EXPECT_EQ(stored().returnAttempts, 1u);
}
TEST_F(TrialLease, CorruptReturnTailPreventsSelection) {
  persist(record(Lease::Phase::Running));
  Fake::running = 1;
  Fake::flash[0].back() ^= 1;
  Lease::onBoot();
  EXPECT_EQ(Fake::selections, 0u);
  EXPECT_EQ(stored().phase, Lease::Phase::Failed);
}
TEST_F(TrialLease, CorruptTrialPreventsUnboundedTimer) {
  persist(record());
  Fake::running = 1;
  Fake::flash[1][4] ^= 1;
  EXPECT_THROW(Lease::onBoot(), Fake::Restart);
  EXPECT_EQ(Fake::timerStarts, 0u);
  EXPECT_EQ(stored().failure, Lease::Failure::TrialImage);
}
TEST_F(TrialLease, TimerAllocationFailureReturnsImmediately) {
  persist(record());
  Fake::running = 1;
  Fake::failTimer = true;
  EXPECT_THROW(Lease::onBoot(), Fake::Restart);
  EXPECT_EQ(Fake::selected, 0u);
  EXPECT_EQ(stored().phase, Lease::Phase::Returning);
}
TEST_F(TrialLease, ReturnBootChecksWholeOriginalAndReportsTerminal) {
  persist(record(Lease::Phase::Returning));
  Lease::onBoot();
  EXPECT_EQ(stored().phase, Lease::Phase::Returned);
  EXPECT_TRUE(Lease::wantsSync());
  EXPECT_EQ(Fake::timerStarts, 0u);
}
TEST_F(TrialLease, UnstartedTrialIsNotReportedAsReturned) {
  persist(record());
  Lease::onBoot();
  EXPECT_EQ(stored().phase, Lease::Phase::Failed);
  EXPECT_EQ(Fake::timerStarts, 0u);
}
TEST_F(TrialLease, ExhaustedReturnBudgetDoesNotWriteOTA) {
  auto r = record(Lease::Phase::Returning);
  r.returnAttempts = 2;
  persist(r);
  Fake::running = 1;
  Lease::onBoot();
  EXPECT_EQ(Fake::selections, 0u);
  EXPECT_EQ(stored().phase, Lease::Phase::Failed);
}
TEST_F(TrialLease, ForeignRunAndPendingCannotBeCleared) {
  persist(record());
  EXPECT_FALSE(Lease::clear(918));
  EXPECT_FALSE(Lease::clear(917));
  EXPECT_FALSE(Fake::blob.empty());
}
TEST_F(TrialLease, CompletedMatchingRunCanBeCleared) {
  persist(record(Lease::Phase::Returned));
  EXPECT_FALSE(Lease::clear(918));
  EXPECT_TRUE(Lease::clear(917));
  EXPECT_TRUE(Fake::blob.empty());
}
TEST_F(TrialLease, CancelRequiresMatchingPreparedRun) {
  persist(record());
  EXPECT_FALSE(Lease::cancelPrepared(918));
  EXPECT_TRUE(Lease::cancelPrepared(917));
  EXPECT_EQ(stored().phase, Lease::Phase::Failed);
}
TEST_F(TrialLease, FlashReadFailureDoesNotSelectOrArm) {
  persist(record());
  Fake::running = 1;
  Fake::failRead = true;
  Lease::onBoot();
  EXPECT_EQ(Fake::timerStarts, 0u);
  EXPECT_EQ(Fake::selections, 0u);
}

TEST_F(TrialLease, CommitFailureNeverLeavesUnboundedTrial) {
  persist(record());
  Fake::running = 1;
  Fake::failCommit = true;
  EXPECT_THROW(Lease::onBoot(), Fake::Restart);
  EXPECT_EQ(Fake::timerStarts, 0u);
  EXPECT_EQ(Fake::selected, 0u);
}
TEST_F(TrialLease, SelectorFailureDoesNotResetOrRetry) {
  persist(record(Lease::Phase::Running));
  Fake::running = 1;
  Fake::failSelect = true;
  Lease::onBoot();
  EXPECT_EQ(Fake::selections, 1u);
  EXPECT_EQ(Fake::resets, 0u);
  EXPECT_EQ(stored().failure, Lease::Failure::Selection);
}

TEST_F(TrialLease, LeasedSleepArmsBoundedHALTimer) {
  persist(record());
  Fake::running = 1;
  Lease::onBoot();
  Fake::sleepSupported = true;
  Lease::beforeSleep();
  EXPECT_EQ(Fake::sleepSeconds, 5u);
}
TEST_F(TrialLease, UnsupportedSleepPowerRestartsInstead) {
  persist(record());
  Fake::running = 1;
  Lease::onBoot();
  Fake::sleepSupported = false;
  EXPECT_THROW(Lease::beforeSleep(), Fake::Restart);
}

TEST_F(TrialLease, ExpectedReturnHashMustMatchWholeHostBackup) {
  EXPECT_FALSE(Lease::prepare(917, 30, stagedSHA().c_str(), std::string(64, 'a').c_str()));
  EXPECT_EQ(Fake::writes, 0u);
}

TEST_F(TrialLease, PendingMaintenanceOnlyRestartsPreservedSlotToSync) {
  auto r = record();
  EXPECT_TRUE(Lease::allowsMaintenanceMode(r, r.returnAddress, true));
  EXPECT_FALSE(Lease::allowsMaintenanceMode(r, r.returnAddress, false));
  EXPECT_FALSE(Lease::allowsMaintenanceMode(r, r.trialAddress, true));
  r.phase = Lease::Phase::Returned;
  EXPECT_TRUE(Lease::allowsMaintenanceMode(r, r.returnAddress, false));
}

TEST_F(TrialLease, DeadlineTicketBindsRunAndSoftwareReset) {
  uint32_t m = 0, t = 0, p = 0;
  Lease::armDeadlineTicket(m, t, p, 917);
  EXPECT_FALSE(Lease::consumeDeadlineTicket(m, t, p, 918, true));
  EXPECT_EQ(m, 0u);
  Lease::armDeadlineTicket(m, t, p, 917);
  EXPECT_FALSE(Lease::consumeDeadlineTicket(m, t, p, 917, false));
  Lease::armDeadlineTicket(m, t, p, 917);
  EXPECT_TRUE(Lease::consumeDeadlineTicket(m, t, p, 917, true));
  EXPECT_EQ(m | t | p, 0u);
}
TEST_F(TrialLease, ExpectedDeadlineResetRetainsSuccessfulReturnOutcome) {
  persist(record(Lease::Phase::Running));
  Fake::running = 1;
  Lease::armDeadlineTicket(silentRebootMagic, silentRebootTarget, silentRebootPayload, 917);
  EXPECT_THROW(Lease::onBoot(), Fake::Restart);
  EXPECT_EQ(stored().failure, Lease::Failure::None);
}
TEST_F(TrialLease, UnexpectedSoftwareResetReturnsButCannotPass) {
  persist(record(Lease::Phase::Running));
  Fake::running = 1;
  EXPECT_THROW(Lease::onBoot(), Fake::Restart);
  EXPECT_EQ(stored().failure, Lease::Failure::UnexpectedReset);
}
TEST_F(TrialLease, ActualTimerSleepResetCanReturnSuccessfully) {
  persist(record(Lease::Phase::Running));
  Fake::running = 1;
  Fake::resetReason = 8;
  Fake::wakeCause = 4;
  EXPECT_THROW(Lease::onBoot(), Fake::Restart);
  EXPECT_EQ(stored().failure, Lease::Failure::None);
}
TEST_F(TrialLease, RunningRecordOnReturnDoesNotInventCompletedRecovery) {
  persist(record(Lease::Phase::Running));
  Lease::onBoot();
  EXPECT_EQ(stored().phase, Lease::Phase::Failed);
  EXPECT_EQ(stored().failure, Lease::Failure::UnexpectedReset);
}

TEST_F(TrialLease, ActualX3LayoutHashesWholeLargerReturnAndReturns) {
  parts[0].size = parts[1].size = Lease::kX3SlotSize;
  parts[1].address = Lease::kX3Slot1;
  Fake::flash[0].resize(Lease::kX3SlotSize, 0x93);
  Fake::flash[1].resize(Lease::kX3SlotSize, 0x51);
  ASSERT_TRUE(Lease::prepare(917, 30, stagedSHA().c_str(), returnSHA().c_str()));
  auto r = stored();
  EXPECT_EQ(r.trialAddress, Lease::kX3Slot1);
  EXPECT_TRUE(Lease::valid(r));
  std::copy(Fake::staged.begin(), Fake::staged.end(), Fake::flash[1].begin());
  Fake::running = 1;
  Lease::onBoot();
  EXPECT_EQ(stored().phase, Lease::Phase::Running);
  ASSERT_NE(Fake::callback, nullptr);
  EXPECT_THROW(Fake::callback(nullptr), Fake::Restart);
  EXPECT_THROW(Lease::onBoot(), Fake::Restart);
  EXPECT_EQ(Fake::selected, 0u);
  Fake::running = 0;
  Lease::onBoot();
  EXPECT_EQ(stored().phase, Lease::Phase::Returned);
  EXPECT_EQ(stored().failure, Lease::Failure::None);
}
TEST_F(TrialLease, MixedOrUnrecognizedLayoutsNeverProvision) {
  parts[1].address = Lease::kX3Slot1;
  EXPECT_FALSE(Lease::prepare(917, 30, stagedSHA().c_str(), returnSHA().c_str()));
  parts[0].size = parts[1].size = Lease::kX3SlotSize;
  parts[1].address += 0x10000;
  EXPECT_FALSE(Lease::prepare(917, 30, stagedSHA().c_str(), returnSHA().c_str()));
  EXPECT_EQ(Fake::writes, 0u);
}
TEST_F(TrialLease, X3ImagePastOldSlotLimitAllowedOnlyInActualLargerLayout) {
  auto r = record();
  r.trialAddress = Lease::kX3Slot1;
  r.imageBytes = Lease::kSlotSize + 1;
  r.checksum = Lease::checksum(r);
  EXPECT_TRUE(Lease::valid(r));
  r.trialAddress = Lease::kSlot1;
  r.checksum = Lease::checksum(r);
  EXPECT_FALSE(Lease::valid(r));
}

TEST_F(TrialLease, ConsumedTimerResumeStillVerifiesBothImagesAndArmsDeadlineWithoutSync) {
  persist(record(Lease::Phase::Running));
  Fake::running = 1;
  Fake::resetReason = 8;
  Fake::wakeCause = 4;
  PocketDaily::TrialReader::permitTimerResume = true;
  Lease::onBoot();
  EXPECT_EQ(PocketDaily::TrialReader::resumeCalls, 1u);
  EXPECT_EQ(Fake::timerStarts, 1u);
  EXPECT_FALSE(Lease::wantsSync());
  EXPECT_EQ(Fake::selections, 0u);
  EXPECT_THROW(Fake::callback(nullptr), Fake::Restart);
  Fake::resetReason = 3;
  Fake::wakeCause = 0;
  EXPECT_THROW(Lease::onBoot(), Fake::Restart);
  EXPECT_EQ(PocketDaily::TrialReader::resumeCalls, 1u);
}
TEST_F(TrialLease, ResumedCandidateCorruptionCannotReachUI) {
  persist(record(Lease::Phase::Running));
  Fake::running = 1;
  Fake::flash[1][320] ^= 0x08;
  PocketDaily::TrialReader::permitTimerResume = true;
  EXPECT_THROW(Lease::onBoot(), Fake::Restart);
  EXPECT_EQ(stored().failure, Lease::Failure::TrialImage);
  EXPECT_EQ(Fake::timerStarts, 0u);
}

TEST_F(TrialLease, SurfaceDurationsAreExplicitAndCorruptModesAreRejected) {
  for (auto surface : {Lease::Surface::Sync, Lease::Surface::FileTransfer, Lease::Surface::Home}) {
    auto r = record();
    r.surface = surface;
    r.seconds = Lease::durationLimit(surface);
    r.checksum = Lease::checksum(r);
    EXPECT_TRUE(Lease::valid(r));
    ++r.seconds;
    r.checksum = Lease::checksum(r);
    EXPECT_FALSE(Lease::valid(r));
  }
  auto r = record();
  r.surface = Lease::Surface::None;
  r.checksum = Lease::checksum(r);
  EXPECT_FALSE(Lease::valid(r));
  r.surface = static_cast<Lease::Surface>(99);
  r.checksum = Lease::checksum(r);
  EXPECT_FALSE(Lease::valid(r));
}
TEST_F(TrialLease, PreviousV3CapabilityCannotProvisionV4) {
  std::memcpy(Fake::staged.data() + 4090, "POCKET_TRIAL_LEASE_V3", sizeof(Lease::kCapability));
  EXPECT_FALSE(Lease::prepare(917, 30, stagedSHA().c_str(), returnSHA().c_str()));
  EXPECT_TRUE(Fake::blob.empty());
  EXPECT_EQ(Fake::selections, 0u);
}
TEST_F(TrialLease, FileTransferIsOnlySelectedByAnImageBoundActiveLease) {
  ASSERT_TRUE(Lease::prepare(917, 600, stagedSHA().c_str(), returnSHA().c_str(), Lease::Surface::FileTransfer));
  Fake::flash[1] = Fake::staged;
  Fake::flash[1].resize(Lease::kSlotSize, 0xff);
  Fake::running = 1;
  Lease::onBoot();
  EXPECT_EQ(Lease::requestedSurface(), Lease::Surface::FileTransfer);
  EXPECT_FALSE(Lease::wantsSync());
  EXPECT_TRUE(Lease::active());
  Lease::recordHttpMemory(false, 48013, 27191);
  Lease::recordHttpMemory(true, 31017, 17233);
  auto r = stored();
  EXPECT_EQ(r.preHttpFree, 48013u);
  EXPECT_EQ(r.preHttpLargest, 27191u);
  EXPECT_EQ(r.readyFree, 31017u);
  EXPECT_EQ(r.readyLargest, 17233u);
  EXPECT_FALSE(Lease::recordSurfaceMemory(90000, 70000));
}
TEST_F(TrialLease, HomeCheckpointRequiresHomeAndCommitSuccess) {
  ASSERT_TRUE(Lease::prepare(917, 30, stagedSHA().c_str(), returnSHA().c_str(), Lease::Surface::Home));
  Fake::flash[1] = Fake::staged;
  Fake::flash[1].resize(Lease::kSlotSize, 0xff);
  Fake::running = 1;
  Lease::onBoot();
  ASSERT_EQ(Lease::requestedSurface(), Lease::Surface::Home);
  ASSERT_TRUE(Lease::recordSurfaceMemory(91213, 75217));
  EXPECT_EQ(stored().surfaceFree, 91213u);
  EXPECT_EQ(stored().surfaceLargest, 75217u);
  Fake::failCommit = true;
  EXPECT_FALSE(Lease::recordSurfaceMemory(89891, 72319));
}
TEST_F(TrialLease, NoLeaseCannotRetainAFormerSurfaceOrWriteMetrics) {
  ASSERT_TRUE(Lease::prepare(917, 30, stagedSHA().c_str(), returnSHA().c_str(), Lease::Surface::Home));
  Fake::flash[1] = Fake::staged;
  Fake::flash[1].resize(Lease::kSlotSize, 0xff);
  Fake::running = 1;
  Lease::onBoot();
  Fake::blob.clear();
  Fake::writes = 0;
  Lease::onBoot();
  EXPECT_EQ(Lease::requestedSurface(), Lease::Surface::None);
  EXPECT_FALSE(Lease::active());
  EXPECT_FALSE(Lease::recordSurfaceMemory(90000, 70000));
  Lease::recordHttpMemory(true, 32000, 14000);
  EXPECT_EQ(Fake::writes, 0u);
}

TEST_F(TrialLease, SurfaceNamesAndUiCaptureModesAreClosedAndCaseSensitive) {
  const std::pair<Lease::Surface, const char*> cases[] = {{Lease::Surface::Sync, "sync"},
                                                          {Lease::Surface::FileTransfer, "file-transfer"},
                                                          {Lease::Surface::Home, "home"},
                                                          {Lease::Surface::Library, "library"},
                                                          {Lease::Surface::Settings, "settings"}};
  for (const auto& [surface, name] : cases) {
    EXPECT_STREQ(Lease::surfaceName(surface), name);
    EXPECT_EQ(Lease::surfaceFromName(name), surface);
  }
  EXPECT_TRUE(Lease::capturesUi(Lease::Surface::Home));
  EXPECT_TRUE(Lease::capturesUi(Lease::Surface::Library));
  EXPECT_TRUE(Lease::capturesUi(Lease::Surface::Settings));
  EXPECT_FALSE(Lease::capturesUi(Lease::Surface::FileTransfer));
  EXPECT_FALSE(Lease::capturesUi(Lease::Surface::Sync));
  for (const char* name : {"", "Home", "file-transfer-extra", "future"})
    EXPECT_EQ(Lease::surfaceFromName(name), Lease::Surface::None);
  EXPECT_EQ(Lease::surfaceFromName(nullptr), Lease::Surface::None);
}

TEST_F(TrialLease, WireBudgetsAndSchemaCannotBeRelaxedByResealing) {
  EXPECT_EQ(Lease::durationLimit(Lease::Surface::Sync), 900u);
  EXPECT_EQ(Lease::durationLimit(Lease::Surface::FileTransfer), 900u);
  EXPECT_EQ(Lease::durationLimit(Lease::Surface::Home), 180u);
  auto r = record();
  r.schema = 3;
  r.checksum = Lease::checksum(r);
  EXPECT_FALSE(Lease::valid(r));
  r = record();
  r.magic = 0x50445433;
  r.checksum = Lease::checksum(r);
  EXPECT_FALSE(Lease::valid(r));
}
