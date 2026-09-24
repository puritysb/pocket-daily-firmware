#include <DirectoryAccounting.h>
#include <SDCardManager.h>
#include <gtest/gtest.h>

#include <cstring>
#include <limits>

namespace {
using Scan = DirectoryAccounting;
using State = Scan::State;
using Format = HalStorage::FilesystemFormat;
constexpr Scan::Limits generous{128, 32, 32, UINT64_MAX};

void shortEntry(uint8_t (&r)[32], bool directory = false, uint32_t size = 9) {
  memset(r, 0, sizeof(r));
  memcpy(r, "FILE    BIN", 11);
  r[11] = directory ? 0x10 : 0x20;
  for (unsigned i = 0; i < 4; ++i) r[28 + i] = size >> (8 * i);
}
uint8_t shortChecksum(const uint8_t* r) {
  uint8_t sum = 0;
  for (unsigned i = 0; i < 11; ++i) sum = ((sum & 1) ? 128 : 0) + (sum / 2) + r[i];
  return sum;
}
void exfatSet(uint8_t (&r)[3][32]) {
  memset(r, 0, sizeof(r));
  r[0][0] = 0x85;
  r[0][1] = 2;
  r[0][2] = 0xa6;  // Fixed checksum for the complete three-record fixture.
  r[0][3] = 0x32;
  r[0][4] = 0x20;
  r[1][0] = 0xc0;
  r[1][1] = 1;
  r[1][3] = 1;
  r[1][8] = 9;
  r[1][20] = 2;
  r[1][24] = 9;
  r[2][0] = 0xc1;
  r[2][2] = 'a';
}
void checksumSet(uint8_t (&r)[3][32]) {
  unsigned sum = 0;
  for (unsigned i = 0; i < 96; ++i) {
    if (i == 2 || i == 3) continue;
    sum = (((sum & 1) ? 32768 : 0) + sum / 2 + r[i / 32][i % 32]) & 65535;
  }
  r[0][2] = sum;
  r[0][3] = sum >> 8;
}
}  // namespace

TEST(DirectoryAccounting, FatCountsLogicalFilesAndChildrenOnlyAfterEnd) {
  Scan scan(Format::Fat, generous);
  uint8_t r[32];
  shortEntry(r);
  EXPECT_EQ(scan.consume(r), State::Reading);
  Scan::Totals total{99, 99, 99};
  EXPECT_FALSE(scan.totals(total));
  EXPECT_EQ(total.bytes, 0u);
  shortEntry(r, true, 0);
  EXPECT_EQ(scan.consume(r), State::Reading);
  memcpy(r, ".          ", 11);
  EXPECT_EQ(scan.consume(r), State::Reading);
  memcpy(r, "..         ", 11);
  EXPECT_EQ(scan.consume(r), State::Reading);
  shortEntry(r, false, 0);
  r[11] = 8;  // Label is not a file.
  EXPECT_EQ(scan.consume(r), State::Reading);
  memset(r, 0, sizeof(r));
  EXPECT_EQ(scan.consume(r), State::Complete);
  ASSERT_TRUE(scan.totals(total));
  EXPECT_EQ(total.files, 1u);
  EXPECT_EQ(total.directories, 1u);
  EXPECT_EQ(total.bytes, 9u);
  EXPECT_EQ(scan.consume(r), State::Complete);  // Terminal state is sticky.
}

TEST(DirectoryAccounting, FatLongNameSequenceRequiresOrderAndShortNameChecksum) {
  uint8_t file[32], lfn[32] = {};
  shortEntry(file);
  lfn[0] = 0x42;
  lfn[11] = 0x0f;
  lfn[13] = shortChecksum(file);
  Scan good(Format::Fat, generous);
  ASSERT_EQ(good.consume(lfn), State::Reading);
  lfn[0] = 1;
  ASSERT_EQ(good.consume(lfn), State::Reading);
  EXPECT_EQ(good.consume(file), State::Reading);
  EXPECT_EQ(good.finish(), State::Complete);

  for (int fault = 0; fault < 4; ++fault) {
    Scan scan(Format::Fat, generous);
    lfn[0] = 0x41;
    ASSERT_EQ(scan.consume(lfn), State::Reading);
    if (fault == 0) EXPECT_EQ(scan.finish(), State::Invalid);
    if (fault == 1) EXPECT_EQ(scan.consume(lfn), State::Invalid);
    if (fault == 2) {
      file[0] = 'X';
      EXPECT_EQ(scan.consume(file), State::Invalid);
      file[0] = 'F';
    }
    if (fault == 3) {
      lfn[0] = 0xe5;
      EXPECT_EQ(scan.consume(lfn), State::Invalid);
    }
    Scan::Totals total;
    EXPECT_FALSE(scan.totals(total));
  }
}

TEST(DirectoryAccounting, LimitsRefusePartialInventoriesAndIncludeDeletedSlots) {
  uint8_t r[32];
  shortEntry(r);
  for (Scan::Limits limit : {Scan::Limits{0, 10, 10, 100}, Scan::Limits{10, 0, 10, 100}, Scan::Limits{10, 10, 10, 8}}) {
    Scan scan(Format::Fat, limit);
    EXPECT_EQ(scan.consume(r), State::LimitExceeded);
    Scan::Totals total;
    EXPECT_FALSE(scan.totals(total));
  }
  Scan deleted(Format::Fat, {2, 10, 10, 100});
  r[0] = 0xe5;
  EXPECT_EQ(deleted.consume(r), State::Reading);
  EXPECT_EQ(deleted.consume(r), State::Reading);
  EXPECT_EQ(deleted.consume(r), State::LimitExceeded);
  Scan folders(Format::Fat, {10, 10, 0, 100});
  shortEntry(r, true, 0);
  EXPECT_EQ(folders.consume(r), State::LimitExceeded);
}

TEST(DirectoryAccounting, ExfatRequiresCompleteChecksummedEntrySet) {
  uint8_t r[3][32];
  exfatSet(r);
  Scan good(Format::ExFat, generous);
  for (auto& record : r) ASSERT_EQ(good.consume(record), State::Reading);
  ASSERT_EQ(good.finish(), State::Complete);
  Scan::Totals total;
  ASSERT_TRUE(good.totals(total));
  EXPECT_EQ(total.files, 1u);
  EXPECT_EQ(total.bytes, 9u);
  for (unsigned stop = 1; stop < 3; ++stop) {
    Scan incomplete(Format::ExFat, generous);
    for (unsigned i = 0; i < stop; ++i) incomplete.consume(r[i]);
    EXPECT_EQ(incomplete.finish(), State::Invalid);
  }
  Scan corrupt(Format::ExFat, generous);
  r[2][2] = 'b';
  corrupt.consume(r[0]);
  corrupt.consume(r[1]);
  EXPECT_EQ(corrupt.consume(r[2]), State::Invalid);
  EXPECT_FALSE(corrupt.totals(total));
  EXPECT_EQ(total.files, 0u);
}

TEST(DirectoryAccounting, ExfatRejectsInvalidLengthsSequencesAndUnknownEntries) {
  uint8_t r[3][32];
  exfatSet(r);
  for (unsigned field : {0, 3, 8}) {
    Scan scan(Format::ExFat, generous);
    scan.consume(r[0]);
    const auto original = r[1][field];
    r[1][field] = field == 8 ? 10 : 0;
    EXPECT_EQ(scan.consume(r[1]), State::Invalid);
    r[1][field] = original;
  }
  Scan extension(Format::ExFat, generous);
  r[0][1] = 3;
  extension.consume(r[0]);
  EXPECT_EQ(extension.consume(r[1]), State::Unsupported);
  Scan unknown(Format::ExFat, generous);
  r[0][0] = 0xa0;
  EXPECT_EQ(unknown.consume(r[0]), State::Unsupported);
  Scan unavailable(Format::Unavailable, generous);
  EXPECT_EQ(unavailable.state(), State::Unsupported);
}

TEST(DirectoryAccounting, HalStepBoundsReadCallsAndPreservesFailureDistinction) {
  FakeSDK::reset();
  auto directory = Storage.open("/");
  Scan scan(Format::Fat, {2, 10, 10, UINT64_MAX});
  EXPECT_EQ(scan.step(directory), State::Reading);
  EXPECT_EQ(scan.step(directory), State::Reading);
  EXPECT_EQ(scan.step(directory), State::LimitExceeded);
  EXPECT_EQ(FakeSDK::reads, 2u);
  FakeSDK::readCount = 0;
  Scan end(Format::Fat, generous);
  EXPECT_EQ(end.step(directory), State::Complete);
  FakeSDK::readError = 1;
  Scan failure(Format::Fat, generous);
  EXPECT_EQ(failure.step(directory), State::IoError);
  EXPECT_EQ(FakeSDK::unlocked, 0u);
}

TEST(DirectoryAccounting, ExfatByteTotalsAre64BitAndOverflowCannotPassAdmission) {
  uint8_t r[3][32];
  exfatSet(r);
  checksumSet(r);
  EXPECT_EQ(r[0][2], 0xa6);
  EXPECT_EQ(r[0][3], 0x32);
  memset(r[1] + 24, 0xff, 8);
  checksumSet(r);
  Scan exact(Format::ExFat, generous);
  for (auto& record : r) ASSERT_EQ(exact.consume(record), State::Reading);
  ASSERT_EQ(exact.finish(), State::Complete);
  Scan::Totals total;
  ASSERT_TRUE(exact.totals(total));
  EXPECT_EQ(total.bytes, UINT64_MAX);
  Scan overflow(Format::ExFat, generous);
  for (auto& record : r) ASSERT_EQ(overflow.consume(record), State::Reading);
  exfatSet(r);
  overflow.consume(r[0]);
  overflow.consume(r[1]);
  EXPECT_EQ(overflow.consume(r[2]), State::LimitExceeded);
  EXPECT_FALSE(overflow.totals(total));
  EXPECT_EQ(total.bytes, 0u);
  Scan folder(Format::ExFat, {128, 10, 0, UINT64_MAX});
  r[0][4] = 0x10;
  checksumSet(r);
  folder.consume(r[0]);
  folder.consume(r[1]);
  EXPECT_EQ(folder.consume(r[2]), State::LimitExceeded);
}
