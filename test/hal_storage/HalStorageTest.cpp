#include <HalStorage.h>
#include <Memory.h>
#include <SDCardManager.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

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

TEST_F(StorageAllocation, SpaceScanCountsFreeClustersAndRejectsInvalidCursor) {
  HalStorage::SpaceChunk result;
  ASSERT_TRUE(Storage.spaceChunk(0, result, nullptr));
  EXPECT_TRUE(result.supported);
  EXPECT_EQ(result.totalBytes, 8u * 4096u);
  EXPECT_EQ(result.freeBytes, result.totalBytes);
  EXPECT_EQ(result.nextCluster, 0u);
  EXPECT_FALSE(Storage.spaceChunk(1, result, nullptr));
  EXPECT_FALSE(Storage.spaceChunk(11, result, nullptr));
  FakeMemory::fail = true;
  EXPECT_FALSE(Storage.spaceChunk(0, result, nullptr));
}
TEST_F(StorageAllocation, ExFatReturnsCapacityWithoutInventingFreeSpace) {
  FakeSDK::filesystemType = 64;
  FakeMemory::fail = true;
  HalStorage::SpaceChunk result;
  ASSERT_TRUE(Storage.spaceChunk(0, result, nullptr));
  EXPECT_FALSE(result.supported);
  EXPECT_EQ(result.totalBytes, 32768u);
  EXPECT_EQ(result.freeBytes, 0u);
}
#include "pocket_daily/web/ReaderFilesPolicy.h"
TEST(ReaderFilesPolicy, ProtectsSystemPathsAndOnlyDeletesReadingFiles) {
  using namespace PocketDaily::Web;
  for (const auto path : {"/", "/Books", "/Articles/한글.epub"}) EXPECT_TRUE(readerFilePath(path));
  for (const auto path : {"", "relative", "/../book", "/.cache/book", "/Books//x", "/Books/", "/Books./x",
                          "/POCKET-DAILY/profile", "/crash_report.txt", "/%2e%2e/x"})
    EXPECT_FALSE(readerFilePath(path));
  EXPECT_TRUE(deletableReaderFile("/Books/BOOK.EPUB"));
  EXPECT_TRUE(deletableReaderFile("/Articles/note.txt"));
  EXPECT_FALSE(deletableReaderFile("/firmware.bin"));
  EXPECT_FALSE(deletableReaderFile("/crash_report.txt"));
}

TEST(ReaderFilesPolicy, DownloadsOnlyFormatsTheAppReads) {
  using namespace PocketDaily::Web;
  EXPECT_TRUE(downloadableReaderFile("/Books/BOOK.EPUB"));
  EXPECT_TRUE(downloadableReaderFile("/Articles/note.md"));
  EXPECT_TRUE(downloadableReaderFile("/notes.TXT"));
  for (const auto path : {"/Books/comic.xtc", "/firmware.bin", "/crash_report.txt", "/.crosspoint/progress.bin",
                          "/pocket-daily/learning/jp.pdl", "/Books", "Books/a.epub", "/a.epub/"})
    EXPECT_FALSE(downloadableReaderFile(path)) << path;
}

TEST(ReaderFilesPolicy, ByteCountsAreUnsignedDecimalsWithoutOverflow) {
  using namespace PocketDaily::Web;
  uint64_t value = 7;
  EXPECT_TRUE(parseByteCount("0", value));
  EXPECT_EQ(value, 0U);
  EXPECT_TRUE(parseByteCount("18446744073709551615", value));
  EXPECT_EQ(value, UINT64_MAX);
  value = 7;
  for (const auto text : {"", "-1", "+1", " 1", "1 ", "1x", "0x10", "18446744073709551616", "999999999999999999999"}) {
    EXPECT_FALSE(parseByteCount(text, value)) << text;
    EXPECT_EQ(value, 7U) << text;
  }
}

TEST(ReaderFilesPolicy, PiecesStayBoundedAndRejectChangedFilesAndPastTheEnd) {
  using namespace PocketDaily::Web;
  // 10,000-byte file, Same Wi-Fi pieces: 4096, 4096, 1808, then 416.
  auto piece = planDownloadPiece(10000, 10000, 0, kDownloadPieceSameWifi);
  EXPECT_EQ(piece.result, PieceResult::Ok);
  EXPECT_EQ(piece.length, 4096U);
  piece = planDownloadPiece(10000, 10000, 8192, kDownloadPieceSameWifi);
  EXPECT_EQ(piece.result, PieceResult::Ok);
  EXPECT_EQ(piece.length, 1808U);
  EXPECT_EQ(planDownloadPiece(10000, 10000, 10000, kDownloadPieceSameWifi).result, PieceResult::OutOfRange);
  EXPECT_EQ(planDownloadPiece(0, 0, 0, kDownloadPieceSameWifi).result, PieceResult::OutOfRange);
  // Direct pieces are smaller; a size the app did not list is a changed file.
  EXPECT_EQ(planDownloadPiece(10000, 10000, 0, kDownloadPieceDirect).length, 1024U);
  EXPECT_EQ(planDownloadPiece(10001, 10000, 0, kDownloadPieceSameWifi).result, PieceResult::SizeChanged);
  // Files past 4 GiB still plan correctly (offsets are 64-bit).
  piece = planDownloadPiece(5000000000ULL, 5000000000ULL, 4999999000ULL, kDownloadPieceSameWifi);
  EXPECT_EQ(piece.result, PieceResult::Ok);
  EXPECT_EQ(piece.length, 1000U);
}

namespace {
// In-memory stand-in for a reader file. Every request opens the file again, so each
// piece starts from a fresh handle at position 0.
struct MemoryFile {
  const std::vector<uint8_t>& bytes;
  uint64_t position = 0;
  bool seek64(const uint64_t target) {
    if (target > bytes.size()) return false;
    position = target;
    return true;
  }
  int read(void* out, const size_t count) {
    const size_t available = std::min<size_t>(count, bytes.size() - position);
    std::copy_n(bytes.begin() + position, available, static_cast<uint8_t*>(out));
    position += available;
    return static_cast<int>(available);
  }
};
}  // namespace

TEST(ReaderFilesPolicy, PiecesReassembleToTheFileOnBothBearers) {
  using namespace PocketDaily::Web;
  EXPECT_EQ(downloadPieceLimit(/*directSession=*/true), 1024U);
  EXPECT_EQ(downloadPieceLimit(/*directSession=*/false), 4096U);
  // Not periodic in 1024 or 4096, so a piece read from the wrong offset cannot match.
  std::vector<uint8_t> original(10000);
  for (size_t i = 0; i < original.size(); ++i) original[i] = static_cast<uint8_t>(i * 31 + i / 251);
  for (const bool direct : {true, false}) {
    const size_t limit = downloadPieceLimit(direct);
    std::vector<uint8_t> received;
    size_t pieces = 0;
    for (uint64_t offset = 0;; ++pieces) {
      const auto piece = planDownloadPiece(original.size(), original.size(), offset, limit);
      if (piece.result != PieceResult::Ok) {
        EXPECT_EQ(piece.result, PieceResult::OutOfRange);
        break;
      }
      ASSERT_LE(piece.length, limit);
      uint8_t buffer[kDownloadPieceSameWifi];
      MemoryFile file{original};
      ASSERT_TRUE(readDownloadPiece(file, offset, buffer, piece.length));
      received.insert(received.end(), buffer, buffer + piece.length);
      offset += piece.length;
    }
    EXPECT_EQ(received, original);
    EXPECT_EQ(pieces, (original.size() + limit - 1) / limit);
  }
  // A truncated file (shorter than the size the request named) is a failed read, not short data.
  uint8_t buffer[16];
  const std::vector<uint8_t> shortFile(8, 1);
  MemoryFile file{shortFile};
  EXPECT_FALSE(readDownloadPiece(file, 0, buffer, sizeof(buffer)));
  MemoryFile past{shortFile};
  EXPECT_FALSE(readDownloadPiece(past, 9, buffer, 1));
}

TEST_F(StorageAllocation, SpaceScanIsChunkedAndStopsOnIOFailure) {
  FakeSDK::clusters = 8192;
  HalStorage::SpaceChunk result;
  ASSERT_TRUE(Storage.spaceChunk(0, result, nullptr));
  EXPECT_EQ(result.nextCluster, 4098u);
  EXPECT_EQ(result.freeBytes, 4096u * 4096u);
  FakeSDK::allocationByte = 255;
  ASSERT_TRUE(Storage.spaceChunk(4098, result, nullptr));
  EXPECT_EQ(result.nextCluster, 0u);
  EXPECT_EQ(result.freeBytes, 0u);
  FakeSDK::failSector = true;
  EXPECT_FALSE(Storage.spaceChunk(0, result, nullptr));
}

// Write-behind buffer (HalFile::setWriteBuffer): section builds serialize pages as 2-4 byte
// fields. Buffered, the SD layer sees one locked call per chunk with the same bytes in the
// same order, position() counts the pending bytes, and every other operation (seek, read,
// size, close, destruction) writes them first.
TEST_F(StorageAllocation, WriteBufferBatchesSmallWritesWithoutChangingBytes) {
  std::vector<uint8_t> expected;
  {
    HalFile file = Storage.open("/section.bin", O_WRITE);
    ASSERT_TRUE(file);
    uint8_t buffer[16];
    file.setWriteBuffer(buffer, sizeof(buffer));
    for (uint8_t i = 0; i < 40; ++i) {
      const uint8_t field[3] = {i, static_cast<uint8_t>(i + 1), static_cast<uint8_t>(i + 2)};
      EXPECT_EQ(file.write(field, sizeof(field)), sizeof(field));
      expected.insert(expected.end(), field, field + sizeof(field));
    }
    EXPECT_EQ(file.position(), expected.size());  // pending bytes included
    EXPECT_LE(FakeSDK::writes, expected.size() / 15 + 1);
    const unsigned before = FakeSDK::writes;
    const uint8_t big[20] = {};  // larger than the buffer: pending bytes, then straight through
    EXPECT_EQ(file.write(big, sizeof(big)), sizeof(big));
    expected.insert(expected.end(), big, big + sizeof(big));
    EXPECT_EQ(FakeSDK::written, expected);
    EXPECT_LE(FakeSDK::writes, before + 2);
    EXPECT_EQ(file.write(uint8_t{9}), 1u);
    expected.push_back(9);
    EXPECT_TRUE(file.seek(0));  // seek writes the pending byte first
    EXPECT_EQ(FakeSDK::written, expected);
    EXPECT_EQ(file.write(uint8_t{8}), 1u);
    expected.push_back(8);
    EXPECT_TRUE(file.close());  // so does close
    EXPECT_EQ(FakeSDK::written, expected);
  }
  {
    HalFile file = Storage.open("/section.bin", O_WRITE);
    uint8_t buffer[8];
    file.setWriteBuffer(buffer, sizeof(buffer));
    EXPECT_EQ(file.write(uint8_t{7}), 1u);
    expected.push_back(7);
    HalFile moved(std::move(file));  // the pending byte moves with the handle
    EXPECT_EQ(file.position(), 0u);
    EXPECT_EQ(moved.position(), 1u);
  }  // and destruction writes it
  EXPECT_EQ(FakeSDK::written, expected);
}
