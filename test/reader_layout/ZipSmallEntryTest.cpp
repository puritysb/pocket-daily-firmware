// Real ZIP/miniz streaming, with allocations faulted only by ZipInflateFaults.
#include <HalStorage.h>
#include <ZipFile.h>
#include <gtest/gtest.h>
#include <zlib.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

#include "ZipInflateFaults.h"

namespace {
using Bytes = std::vector<uint8_t>;
constexpr size_t CHUNK = 1024;
Bytes pattern(size_t count, bool random = false) {
  Bytes result(count);
  uint32_t state = 0x13579BDF;
  for (size_t i = 0; i < count; ++i) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    result[i] = random ? static_cast<uint8_t>(state) : static_cast<uint8_t>((i % 251) * 37 + (i / 251) % 3);
  }
  return result;
}
class Sink : public Print {
 public:
  Bytes bytes;
  size_t limit = std::numeric_limits<size_t>::max();
  unsigned calls = 0;
  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* data, size_t count) override {
    ++calls;
    const size_t accepted = std::min(count, limit - bytes.size());
    bytes.insert(bytes.end(), data, data + accepted);
    return accepted;
  }
};
class ZipSmallEntry : public testing::Test {
 protected:
  void SetUp() override {
    ZipInflateFaults::disable();
    char dir[] = "/tmp/zip-small-entry-XXXXXX";
    ASSERT_NE(mkdtemp(dir), nullptr);
    TestFs::root = dir;
  }
  void TearDown() override {
    EXPECT_EQ(ZipInflateFaults::stats().live, 0U);
    EXPECT_EQ(ZipInflateFaults::stats().canaryFailures, 0U);
    ZipInflateFaults::disable();
    std::filesystem::remove_all(TestFs::root);
  }
  Bytes compressed(const Bytes& input) {
    crc = static_cast<uint32_t>(crc32(0, input.data(), static_cast<uInt>(input.size())));
    z_stream stream{};
    EXPECT_EQ(deflateInit2(&stream, 9, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY), Z_OK);
    Bytes output(deflateBound(&stream, static_cast<uLong>(input.size())));
    stream.next_in = const_cast<Bytef*>(input.data());
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = output.data();
    stream.avail_out = static_cast<uInt>(output.size());
    EXPECT_EQ(deflate(&stream, Z_FINISH), Z_STREAM_END);
    output.resize(stream.total_out);
    EXPECT_EQ(deflateEnd(&stream), Z_OK);
    return output;
  }
  void archive(const Bytes& packed, uint32_t declared) {
    Bytes zip;
    auto word = [&](uint32_t value, size_t count) {
      for (size_t i = 0; i < count; ++i) zip.push_back(static_cast<uint8_t>(value >> (i * 8)));
    };
    constexpr char NAME[] = "item.bin";
    constexpr size_t NAME_SIZE = sizeof(NAME) - 1;
    auto name = [&] { zip.insert(zip.end(), NAME, NAME + NAME_SIZE); };
    word(0x04034B50, 4);
    word(20, 2);
    word(0, 2);
    word(8, 2);
    word(0, 4);
    word(crc, 4);
    word(static_cast<uint32_t>(packed.size()), 4);
    word(declared, 4);
    word(NAME_SIZE, 2);
    word(0, 2);
    name();
    zip.insert(zip.end(), packed.begin(), packed.end());
    const auto directory = static_cast<uint32_t>(zip.size());
    word(0x02014B50, 4);
    word(20, 2);
    word(20, 2);
    word(0, 2);
    word(8, 2);
    word(0, 4);
    word(crc, 4);
    word(static_cast<uint32_t>(packed.size()), 4);
    word(declared, 4);
    word(NAME_SIZE, 2);
    word(0, 2);
    word(0, 2);
    word(0, 2);
    word(0, 2);
    word(0, 4);
    word(0, 4);
    name();
    const auto directorySize = static_cast<uint32_t>(zip.size()) - directory;
    word(0x06054B50, 4);
    word(0, 2);
    word(0, 2);
    word(1, 2);
    word(1, 2);
    word(directorySize, 4);
    word(directory, 4);
    word(0, 2);
    std::ofstream file(TestFs::root + path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(zip.data()), static_cast<std::streamsize>(zip.size()));
    ASSERT_TRUE(file.good());
  }
  bool read(Sink& sink, size_t chunk = CHUNK, bool early = false) {
    return ZipFile(path).readFileToStream("item.bin", sink, chunk, early);
  }
  uint32_t crc = 0;
  const std::string path = "/fixture.zip";
};

TEST_F(ZipSmallEntry, SmallEntriesUseExistingChunkWhenEveryRingAllocationFails) {
  for (size_t size : {0U, 1U, 409U, 1023U}) {
    const auto data = pattern(size);
    archive(compressed(data), static_cast<uint32_t>(data.size()));
    for (size_t chunk : {1024U, 4096U, 8192U}) {
      ZipInflateFaults::reset(32 * 1024, true);
      Sink sink;
      ASSERT_TRUE(read(sink, chunk));
      EXPECT_EQ(sink.bytes, data);
      EXPECT_EQ(ZipFile::lastStreamError(), ZipFile::StreamError::None);
      EXPECT_EQ(ZipInflateFaults::stats().windowAttempts, 0U);
      EXPECT_EQ(ZipInflateFaults::stats().live, 0U);
    }
  }
}

TEST_F(ZipSmallEntry, ExactBoundaryAndLargerEntriesRetainRingAndExactBytes) {
  for (size_t size : {1024U, 1025U, 95000U}) {
    const auto data = pattern(size);
    archive(compressed(data), static_cast<uint32_t>(data.size()));
    ZipInflateFaults::reset();
    Sink sink;
    ASSERT_TRUE(read(sink));
    EXPECT_EQ(sink.bytes, data);
    EXPECT_EQ(ZipInflateFaults::stats().windowAttempts, 1U);
    ZipInflateFaults::reset(static_cast<size_t>(-1), true);
    Sink blocked;
    EXPECT_FALSE(read(blocked));
    EXPECT_EQ(ZipFile::lastStreamError(), ZipFile::StreamError::Window);
    EXPECT_TRUE(blocked.bytes.empty());
    EXPECT_EQ(ZipInflateFaults::stats().live, 0U);
  }
}

TEST_F(ZipSmallEntry, SmallIncompressibleEntryCanRefillInputWithoutReusingOutput) {
  const auto data = pattern(CHUNK - 1, true);
  const auto packed = compressed(data);
  ASSERT_GT(packed.size(), CHUNK);
  archive(packed, static_cast<uint32_t>(data.size()));
  ZipInflateFaults::reset(16 * 1024, true);
  Sink sink;
  ASSERT_TRUE(read(sink));
  EXPECT_EQ(sink.bytes, data);
  EXPECT_EQ(ZipInflateFaults::stats().windowAttempts, 0U);
}

TEST_F(ZipSmallEntry, ValidPrefixSinkStillRequiresExplicitEarlyStop) {
  const auto data = pattern(409);
  archive(compressed(data), static_cast<uint32_t>(data.size()));
  for (bool early : {false, true}) {
    ZipInflateFaults::reset(16 * 1024, true);
    Sink sink;
    sink.limit = 24;
    EXPECT_EQ(read(sink, CHUNK, early), early);
    EXPECT_EQ(sink.bytes, Bytes(data.begin(), data.begin() + 24));
    EXPECT_EQ(ZipFile::lastStreamError(), early ? ZipFile::StreamError::None : ZipFile::StreamError::Write);
    EXPECT_EQ(ZipInflateFaults::stats().live, 0U);
  }
}

TEST_F(ZipSmallEntry, FalseDeclaredSizesFailBeforeAnyFullOrPrefixWrite) {
  for (size_t actual : {409U, 4096U}) {
    const auto packed = compressed(pattern(actual));
    for (uint32_t declared : {0U, 408U, 410U}) {
      archive(packed, declared);
      for (bool early : {false, true}) {
        ZipInflateFaults::reset();
        Sink sink;
        if (early) sink.limit = 24;
        EXPECT_FALSE(read(sink, CHUNK, early));
        EXPECT_EQ(ZipFile::lastStreamError(), ZipFile::StreamError::Inflate);
        EXPECT_EQ(sink.calls, 0U);
        EXPECT_TRUE(sink.bytes.empty());
        EXPECT_EQ(ZipInflateFaults::stats().canaryFailures, 0U);
      }
    }
  }
}

TEST_F(ZipSmallEntry, CorruptOrTruncatedSmallStreamsCannotBecomeSuccessfulPrefixReads) {
  const auto data = pattern(409);
  const auto packed = compressed(data);
  auto truncated = packed;
  truncated.pop_back();
  auto corrupt = packed;
  corrupt[0] = static_cast<uint8_t>((corrupt[0] & ~6) | 6);  // Reserved deflate block type.
  for (const auto& invalid : {truncated, corrupt}) {
    archive(invalid, static_cast<uint32_t>(data.size()));
    for (bool early : {false, true}) {
      ZipInflateFaults::reset();
      Sink sink;
      if (early) sink.limit = 24;
      EXPECT_FALSE(read(sink, CHUNK, early));
      EXPECT_EQ(ZipFile::lastStreamError(), ZipFile::StreamError::Inflate);
      EXPECT_EQ(sink.calls, 0U);
      EXPECT_EQ(ZipInflateFaults::stats().canaryFailures, 0U);
    }
  }
}

TEST_F(ZipSmallEntry, AllocationFailuresReleaseBuffersAndDoNotPoisonNextRead) {
  const auto data = pattern(409);
  archive(compressed(data), static_cast<uint32_t>(data.size()));
  const ZipFile::StreamError errors[] = {ZipFile::StreamError::ReadBuffer, ZipFile::StreamError::OutputBuffer,
                                         ZipFile::StreamError::Window};
  for (size_t i = 0; i < 3; ++i) {
    // Resolve metadata before injecting the stream allocation failure, so
    // the EOCD scan's own temporary allocation does not mask this boundary.
    ZipInflateFaults::disable();
    ZipFile zip(path);
    ASSERT_TRUE(zip.loadAllFileStatSlims());
    ZipInflateFaults::reset(i * CHUNK);
    Sink failed;
    EXPECT_FALSE(zip.readFileToStream("item.bin", failed, CHUNK));
    EXPECT_EQ(ZipFile::lastStreamError(), errors[i]);
    EXPECT_EQ(failed.calls, 0U);
    EXPECT_EQ(ZipInflateFaults::stats().windowAttempts, 0U);
    EXPECT_EQ(ZipInflateFaults::stats().live, 0U);
    ZipInflateFaults::reset(16 * 1024, true);
    Sink retried;
    ASSERT_TRUE(read(retried));
    EXPECT_EQ(retried.bytes, data);
    EXPECT_EQ(ZipFile::lastStreamError(), ZipFile::StreamError::None);
  }
}
}  // namespace
