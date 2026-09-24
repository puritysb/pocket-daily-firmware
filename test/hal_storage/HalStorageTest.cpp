#include <HalStorage.h>
#include <Memory.h>
#include <SDCardManager.h>
#include <gtest/gtest.h>

class StorageAllocation : public testing::Test {
 protected:
  void SetUp() override {
    FakeSDK::reset();
    FakeMemory::fail = false;
  }
  void TearDown() override {
    EXPECT_EQ(FakeSDK::lockDepth, 0);
    EXPECT_EQ(FakeSDK::unlocked, 0u);
  }
};

TEST_F(StorageAllocation, OpenFailureDoesNotEnterFilesystem) {
  FakeMemory::fail = true;
  EXPECT_FALSE(Storage.open("/book", O_WRITE));
  HalFile file;
  EXPECT_FALSE(Storage.openFileForRead("TEST", "/book", file));
  EXPECT_FALSE(file);
  EXPECT_FALSE(Storage.openFileForWrite("TEST", "/book", file));
  EXPECT_FALSE(file);
  EXPECT_EQ(FakeSDK::opens, 0u);
}

TEST_F(StorageAllocation, FilesystemFormatUsesMountedMetadataWithoutScanOrAllocation) {
  FakeMemory::fail = true;
  for (uint8_t type : {12, 16, 32}) {
    FakeSDK::filesystemType = type;
    EXPECT_EQ(Storage.filesystemFormat(), HalStorage::FilesystemFormat::Fat);
  }
  FakeSDK::filesystemType = 64;
  EXPECT_EQ(Storage.filesystemFormat(), HalStorage::FilesystemFormat::ExFat);
  for (uint8_t type : {0, 1, 255}) {
    FakeSDK::filesystemType = type;
    EXPECT_EQ(Storage.filesystemFormat(), HalStorage::FilesystemFormat::Unavailable);
  }
  EXPECT_EQ(FakeSDK::opens, 0u);
  EXPECT_EQ(FakeSDK::reads, 0u);
}

TEST_F(StorageAllocation, FailedReplacementClosesPreviousHandleUnderLock) {
  HalFile file = Storage.open("/old");
  ASSERT_TRUE(file);
  FakeMemory::fail = true;
  EXPECT_FALSE(Storage.openFileForWrite("TEST", "/new", file));
  EXPECT_FALSE(file);
  EXPECT_EQ(FakeSDK::opens, 1u);
  EXPECT_EQ(FakeSDK::closes, 1u);
}

TEST_F(StorageAllocation, DirectoryAllocationFailureDoesNotAdvanceCursor) {
  auto directory = Storage.open("/");
  FakeMemory::fail = true;
  EXPECT_FALSE(directory.openNextFile());
  EXPECT_EQ(FakeSDK::nexts, 0u);
  FakeMemory::fail = false;
  EXPECT_TRUE(directory.openNextFile());
  EXPECT_EQ(FakeSDK::nexts, 1u);
}

TEST_F(StorageAllocation, RawDirectoryRecordsBoundWorkWithoutAllocating) {
  auto directory = Storage.open("/");
  uint8_t record[32] = {};
  FakeMemory::fail = true;     // An existing cursor requires no new file handle.
  FakeSDK::recordByte = 0xe5;  // Deleted slot is observable, not skipped.
  for (unsigned i = 0; i < 20; ++i) {
    EXPECT_EQ(directory.readDirectoryRecord(record), HalFile::DirectoryRead::Record);
    EXPECT_EQ(record[0], 0xe5);
    EXPECT_EQ(FakeSDK::reads, i + 1);
  }
  EXPECT_EQ(FakeSDK::nexts, 0u);
  EXPECT_EQ(directory.position(), 20u * 32);
}

TEST_F(StorageAllocation, RawDirectoryEndIsDistinctFromReadFailure) {
  uint8_t record[32];
  for (int count : {0, 7, -1, 32}) {
    auto directory = Storage.open("/");
    FakeSDK::readCount = count;
    FakeSDK::recordByte = 0;  // Zero entry marker is also a physical end.
    const auto expected = count == 0 || count == 32 ? HalFile::DirectoryRead::End : HalFile::DirectoryRead::Error;
    EXPECT_EQ(directory.readDirectoryRecord(record), expected);
    for (auto byte : record) EXPECT_EQ(byte, 0);
  }
}

TEST_F(StorageAllocation, RawDirectoryRefusesInvalidOrErroredCursorsBeforeReading) {
  uint8_t record[32];
  HalFile empty;
  EXPECT_EQ(empty.readDirectoryRecord(record), HalFile::DirectoryRead::Error);
  auto directory = Storage.open("/");
  FakeSDK::directory = false;
  EXPECT_EQ(directory.readDirectoryRecord(record), HalFile::DirectoryRead::Error);
  FakeSDK::directory = true;
  ASSERT_TRUE(directory.seekSet(1));
  EXPECT_EQ(directory.readDirectoryRecord(record), HalFile::DirectoryRead::Error);
  ASSERT_TRUE(directory.seekSet(0));
  FakeSDK::readError = 1;
  EXPECT_EQ(directory.readDirectoryRecord(record), HalFile::DirectoryRead::Error);
  EXPECT_EQ(FakeSDK::reads, 0u);
  for (auto byte : record) EXPECT_EQ(byte, 0);
}

TEST_F(StorageAllocation, RawDirectoryReadErrorCannotMasqueradeAsEndOrRecord) {
  uint8_t record[32];
  for (int count : {0, 32}) {
    FakeSDK::readError = 0;
    FakeSDK::errorAfterRead = true;
    FakeSDK::readCount = count;
    auto directory = Storage.open("/");
    EXPECT_EQ(directory.readDirectoryRecord(record), HalFile::DirectoryRead::Error);
    for (auto byte : record) EXPECT_EQ(byte, 0);
  }
}

TEST_F(StorageAllocation, EmptyAndMovedFromHandlesHaveSafeFailureResults) {
  auto file = Storage.open("/book");
  auto owner = std::move(file);
  ASSERT_TRUE(owner);
  char name[8] = "old";
  EXPECT_EQ(file.getName(name, sizeof(name)), 0u);
  EXPECT_STREQ(name, "");
  EXPECT_EQ(file.size(), 0u);
  EXPECT_EQ(file.fileSize(), 0u);
  EXPECT_EQ(file.fileSize64(), 0u);
  EXPECT_EQ(file.available(), 0);
  EXPECT_EQ(file.position(), 0u);
  EXPECT_FALSE(file.seek(0));
  EXPECT_FALSE(file.seek64(0));
  EXPECT_FALSE(file.seekCur(0));
  EXPECT_FALSE(file.seekSet(0));
  EXPECT_FALSE(file.preAllocate(1));
  EXPECT_EQ(file.read(name, 1), -1);
  EXPECT_EQ(file.read(), -1);
  EXPECT_EQ(file.write(name, 1), 0u);
  EXPECT_EQ(file.write(uint8_t(1)), 0u);
  EXPECT_FALSE(file.rename("/other"));
  EXPECT_FALSE(file.isDirectory());
  EXPECT_FALSE(file.openNextFile());
  file.flush();
  file.rewindDirectory();
  EXPECT_TRUE(file.close());
  EXPECT_EQ(FakeSDK::writes, 0u);
}

TEST_F(StorageAllocation, NormalAndFilesystemFailedOpensPreserveBehavior) {
  HalFile file;
  EXPECT_TRUE(Storage.openFileForRead("TEST", "/book", file));
  EXPECT_EQ(file.read(), 7);
  EXPECT_TRUE(Storage.openFileForWrite("TEST", "/book", file));
  EXPECT_EQ(file.write(uint8_t(9)), 1u);
  FakeSDK::failOpen = true;
  EXPECT_FALSE(Storage.openFileForRead("TEST", "/missing", file));
  EXPECT_FALSE(file);
  EXPECT_TRUE(file.close());
}
