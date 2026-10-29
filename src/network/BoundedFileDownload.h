#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace bounded_http {
// Allow slow but progressing SD/Wi-Fi transfers without an arbitrary fixed
// large-file cutoff; cap resource ownership at one hour, independently of idle.
constexpr uint32_t downloadBudgetMs(uint64_t bytes) {
  const uint64_t blocks = bytes / 16384 + (bytes % 16384 != 0);
  return static_cast<uint32_t>(30 + std::min<uint64_t>(blocks, 3570)) * 1000;
}
enum class DownloadResult { Complete, ReadFailure, TransportFailure, Timeout, InvalidBuffer };
struct DownloadIO {
  void* context;
  int (*send)(void*, const uint8_t*, size_t);  // nonblocking: zero=backpressure, negative=closed/error
  int (*read)(void*, uint8_t*, size_t);
  uint32_t (*now)();
  void (*progress)();  // feeds watchdog and yields even during backpressure
};

class FileDownload {
 public:
  FileDownload(DownloadIO io, uint32_t totalMs, uint32_t idleMs, size_t maxWrite = 1460)
      : io(io), maxWrite(maxWrite), started(io.now()), lastProgress(started), totalMs(totalMs), idleMs(idleMs) {}
  DownloadResult status() const { return result; }
  uint64_t bodyBytes() const { return bodySent; }
  bool write(const uint8_t* data, size_t size) {
    if (!maxWrite) return fail(DownloadResult::InvalidBuffer);
    size_t offset = 0;
    while (offset < size) {
      if (!healthy()) return false;
      const size_t wanted = std::min(size - offset, maxWrite);
      const int sent = io.send(io.context, data + offset, wanted);
      if (sent < 0 || static_cast<size_t>(sent) > wanted) return fail(DownloadResult::TransportFailure);
      offset += static_cast<size_t>(sent);
      wireSent += static_cast<uint32_t>(sent);
      if (sent > 0) lastProgress = io.now();
      io.progress();
    }
    return healthy();
  }
  bool copy(uint64_t bytes, uint8_t* buffer, size_t capacity) {
    if (!buffer || !capacity) return fail(DownloadResult::InvalidBuffer);
    while (bodySent < bytes) {
      if (!healthy()) return false;
      const auto wanted = static_cast<size_t>(std::min<uint64_t>(bytes - bodySent, capacity));
      const int got = io.read(io.context, buffer, wanted);
      if (got <= 0 || static_cast<size_t>(got) > wanted) return fail(DownloadResult::ReadFailure);
      io.progress();
      const uint64_t before = wireSent;
      const bool sent = write(buffer, static_cast<size_t>(got));
      bodySent += wireSent - before;
      if (!sent) return false;
    }
    return healthy();
  }

 private:
  bool fail(DownloadResult why) {
    result = why;
    return false;
  }
  bool healthy() {
    if (result != DownloadResult::Complete) return false;
    const auto now = io.now();
    if (static_cast<uint32_t>(now - started) >= totalMs || static_cast<uint32_t>(now - lastProgress) >= idleMs)
      return fail(DownloadResult::Timeout);
    return true;
  }
  DownloadIO io;
  size_t maxWrite;
  uint32_t started, lastProgress, totalMs, idleMs;
  uint64_t bodySent = 0, wireSent = 0;
  DownloadResult result = DownloadResult::Complete;
};

// Cold-path header only. A filename cannot inject response headers or quotes.
[[gnu::noinline]] inline size_t downloadHeader(uint8_t* buffer, size_t capacity, const char* filename, bool epub,
                                               uint64_t bytes, const char* version = "HTTP/1.1") {
  char safe[128];
  size_t i = 0;
  for (; filename[i] && i < sizeof(safe) - 1; ++i) {
    const auto ch = static_cast<unsigned char>(filename[i]);
    safe[i] = ch < 32 || ch == 127 || ch == '"' || ch == '\\' ? '_' : filename[i];
  }
  safe[i] = '\0';
  const int count = snprintf(
      reinterpret_cast<char*>(buffer), capacity,
      "%s 200 OK\r\nContent-Type: %s\r\nContent-Length: %llu\r\n"
      "Content-Disposition: attachment; filename=\"%s\"\r\nConnection: close\r\n"
      "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: *\r\nAccess-Control-Allow-Headers: *\r\n\r\n",
      version, epub ? "application/epub+zip" : "application/octet-stream", static_cast<unsigned long long>(bytes),
      i ? safe : "download");
  return count > 0 && static_cast<size_t>(count) < capacity ? static_cast<size_t>(count) : 0;
}
}  // namespace bounded_http
