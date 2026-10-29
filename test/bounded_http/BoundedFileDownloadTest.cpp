#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "network/BoundedFileDownload.h"
namespace {
uint32_t timeMs;
uint32_t now() { return timeMs; }
void progress() { ++timeMs; }
struct IO {
  std::vector<uint8_t> input, output;
  size_t offset = 0, readLimit = 9999, sendLimit = 9999;
  int zeros = 0, failSend = -1;
  unsigned calls = 0;
  explicit IO(size_t bytes = 13079) : input(bytes) {
    output.reserve(bytes + 1000);
    for (size_t i = 0; i < bytes; ++i) input[i] = static_cast<uint8_t>((i * 37 + i / 31) % 251);
    timeMs = 0;
  }
  static int read(void* raw, uint8_t* buf, size_t n) {
    auto& self = *static_cast<IO*>(raw);
    const size_t count = std::min({n, self.readLimit, self.input.size() - self.offset});
    std::copy_n(self.input.data() + self.offset, count, buf);
    self.offset += count;
    return static_cast<int>(count);
  }
  static int send(void* raw, const uint8_t* buf, size_t n) {
    auto& self = *static_cast<IO*>(raw);
    ++self.calls;
    if (self.failSend >= 0 && self.calls > static_cast<unsigned>(self.failSend)) return -1;
    if (self.zeros-- > 0) return 0;
    const size_t count = std::min(n, self.sendLimit);
    self.output.insert(self.output.end(), buf, buf + count);
    return static_cast<int>(count);
  }
  bounded_http::DownloadIO callbacks() { return {this, send, read, now, progress}; }
};
}  // namespace
TEST(FileDownload, NonUniformBytesSurviveShortReadsPartialWritesAndBackpressure) {
  IO io;
  io.readLimit = 101;
  io.sendLimit = 17;
  io.zeros = 23;
  std::array<uint8_t, 193> buffer{};
  bounded_http::FileDownload writer(io.callbacks(), 10000, 1000);
  ASSERT_TRUE(writer.copy(io.input.size(), buffer.data(), buffer.size()));
  EXPECT_EQ(io.output, io.input);
  EXPECT_EQ(writer.bodyBytes(), io.input.size());
}
TEST(FileDownload, DeclaredLengthDoesNotReadOrSendPastSnapshot) {
  IO io(4137);
  std::array<uint8_t, 127> buffer{};
  bounded_http::FileDownload writer(io.callbacks(), 10000, 1000);
  ASSERT_TRUE(writer.copy(2053, buffer.data(), buffer.size()));
  EXPECT_EQ(io.output, std::vector<uint8_t>(io.input.begin(), io.input.begin() + 2053));
  EXPECT_EQ(io.offset, 2053u);
}
TEST(FileDownload, EarlySdEofCannotSucceed) {
  IO io(100);
  std::array<uint8_t, 127> buffer{};
  bounded_http::FileDownload writer(io.callbacks(), 10000, 1000);
  EXPECT_FALSE(writer.copy(101, buffer.data(), buffer.size()));
  EXPECT_EQ(writer.status(), bounded_http::DownloadResult::ReadFailure);
}
TEST(FileDownload, NoReaderAndDisconnectedPeerAreBounded) {
  IO io;
  io.sendLimit = 0;
  std::array<uint8_t, 127> buffer{};
  bounded_http::FileDownload writer(io.callbacks(), 10000, 20);
  EXPECT_FALSE(writer.copy(io.input.size(), buffer.data(), buffer.size()));
  EXPECT_EQ(writer.status(), bounded_http::DownloadResult::Timeout);
  EXPECT_LE(io.calls, 20u);
  IO closed;
  closed.failSend = 0;
  bounded_http::FileDownload failed(closed.callbacks(), 10000, 20);
  EXPECT_FALSE(failed.copy(closed.input.size(), buffer.data(), buffer.size()));
  EXPECT_EQ(failed.status(), bounded_http::DownloadResult::TransportFailure);
}
TEST(FileDownload, ProgressDoesNotRenewTotalDeadlineAndClockWrapIsSafe) {
  IO io;
  io.sendLimit = 1;
  std::array<uint8_t, 127> buffer{};
  timeMs = UINT32_MAX - 9;
  bounded_http::FileDownload writer(io.callbacks(), 30, 20);
  EXPECT_FALSE(writer.copy(io.input.size(), buffer.data(), buffer.size()));
  EXPECT_EQ(writer.status(), bounded_http::DownloadResult::Timeout);
  EXPECT_LT(io.output.size(), 30u);
}
TEST(FileDownload, ZeroAndBoundaryLengthsAreExact) {
  for (const size_t n : {0u, 1u, 127u, 128u, 1460u, 4096u, 4097u}) {
    IO io(n);
    std::array<uint8_t, 128> buffer{};
    bounded_http::FileDownload writer(io.callbacks(), 10000, 1000);
    ASSERT_TRUE(writer.copy(n, buffer.data(), buffer.size()));
    EXPECT_EQ(io.output, io.input);
  }
}
TEST(FileDownload, HeaderFramingLengthMimeAndNameCannotInject) {
  std::array<uint8_t, 512> buffer{};
  const auto n =
      bounded_http::downloadHeader(buffer.data(), buffer.size(), "한글\"\r\nX-Fake: bad.epub", true, 4294967301ULL);
  ASSERT_GT(n, 0u);
  std::string header(reinterpret_cast<char*>(buffer.data()), n);
  EXPECT_NE(header.find("Content-Length: 4294967301\r\n"), std::string::npos);
  EXPECT_NE(header.find("application/epub+zip"), std::string::npos);
  EXPECT_EQ(header.find("\r\nX-Fake:"), std::string::npos);
  EXPECT_TRUE(header.ends_with("\r\n\r\n"));
  EXPECT_EQ(bounded_http::downloadHeader(buffer.data(), 16, "a", false, 0), 0u);
  const auto zero = bounded_http::downloadHeader(buffer.data(), buffer.size(), "", false, 0);
  EXPECT_NE(std::string(reinterpret_cast<char*>(buffer.data()), zero).find("Content-Length: 0\r\n"), std::string::npos);
}

TEST(FileDownload, HeaderAndBodyShareBufferWithoutLeakingOrDuplicatingBytes) {
  IO io(4907);
  io.zeros = 3;
  io.sendLimit = 29;
  std::array<uint8_t, 512> buffer{};
  const size_t n =
      bounded_http::downloadHeader(buffer.data(), buffer.size(), "sample.epub", true, io.input.size(), "HTTP/1.0");
  const std::string header(reinterpret_cast<char*>(buffer.data()), n);
  ASSERT_TRUE(header.starts_with("HTTP/1.0 200 OK\r\n"));
  bounded_http::FileDownload writer(io.callbacks(), 10000, 1000);
  ASSERT_TRUE(writer.write(buffer.data(), n));
  ASSERT_TRUE(writer.copy(io.input.size(), buffer.data(), buffer.size()));
  ASSERT_EQ(io.output.size(), n + io.input.size());
  EXPECT_EQ(std::string(io.output.begin(), io.output.begin() + n), header);
  EXPECT_EQ(std::vector<uint8_t>(io.output.begin() + n, io.output.end()), io.input);
  EXPECT_EQ(writer.bodyBytes(), io.input.size());
}
TEST(FileDownload, FailedPartialChunkAccountsForActualBodyBytesAndIsSticky) {
  IO io(999);
  io.sendLimit = 13;
  io.failSend = 3;
  std::array<uint8_t, 127> buffer{};
  bounded_http::FileDownload writer(io.callbacks(), 10000, 1000);
  EXPECT_FALSE(writer.copy(io.input.size(), buffer.data(), buffer.size()));
  EXPECT_EQ(writer.bodyBytes(), 39u);
  EXPECT_EQ(io.output.size(), 39u);
  EXPECT_FALSE(writer.write(buffer.data(), 10));
  EXPECT_EQ(io.output.size(), 39u);
}

TEST(FileDownload, SizeBudgetHasHeadroomAndCannotOverflow) {
  EXPECT_EQ(bounded_http::downloadBudgetMs(0), 30000u);
  EXPECT_EQ(bounded_http::downloadBudgetMs(1), 31000u);
  EXPECT_EQ(bounded_http::downloadBudgetMs(16384), 31000u);
  EXPECT_EQ(bounded_http::downloadBudgetMs(16385), 32000u);
  EXPECT_GT(bounded_http::downloadBudgetMs(6483104), 400000u);
  EXPECT_EQ(bounded_http::downloadBudgetMs(UINT64_MAX), 3600000u);
}
TEST(FileDownload, ConfiguredPacketCapAndInvalidZeroAreEnforced) {
  IO io(4101);
  std::array<uint8_t, 1436> buffer{};
  bounded_http::FileDownload writer(io.callbacks(), 10000, 1000, 1436);
  ASSERT_TRUE(writer.copy(io.input.size(), buffer.data(), buffer.size()));
  EXPECT_EQ(io.input, io.output);
  IO invalid;
  bounded_http::FileDownload zero(invalid.callbacks(), 10000, 1000, 0);
  EXPECT_FALSE(zero.write(buffer.data(), 1));
  EXPECT_EQ(zero.status(), bounded_http::DownloadResult::InvalidBuffer);
}
