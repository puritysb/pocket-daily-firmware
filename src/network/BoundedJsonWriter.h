#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace bounded_http {
// A single close-delimited response, coalesced without per-row allocations.
// Transport is nonblocking: zero means backpressure, negative means failure.
class JsonWriter {
 public:
  using Send = int (*)(void*, const uint8_t*, size_t);
  using Now = uint32_t (*)();
  using Progress = void (*)();
  JsonWriter(void* context, Send send, Now now, Progress progress, uint32_t budgetMs, size_t byteLimit)
      : context(context),
        send(send),
        now(now),
        progress(progress),
        started(now()),
        budgetMs(budgetMs),
        limit(byteLimit) {}
  bool healthy() {
    if (static_cast<uint32_t>(now() - started) >= budgetMs) failed = true;
    return !failed;
  }
  bool append(std::string_view text) {
    if (!healthy() || text.size() > limit - accepted) return fail();
    accepted += text.size();
    for (const auto ch : text) {
      if (used == sizeof(buffer) && !flush()) return false;
      buffer[used++] = static_cast<uint8_t>(ch);
    }
    return true;
  }
  bool quoted(std::string_view text) {
    if (!append("\"")) return false;
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    for (const unsigned char ch : text) {
      if (ch == '"' || ch == '\\') {
        const char escaped[] = {'\\', static_cast<char>(ch)};
        if (!append({escaped, sizeof(escaped)})) return false;
      } else if (ch < 32) {
        const char escaped[] = {'\\', 'u', '0', '0', HEX_DIGITS[ch >> 4], HEX_DIGITS[ch & 15]};
        if (!append({escaped, sizeof(escaped)})) return false;
      } else {
        const char byte = static_cast<char>(ch);
        if (!append({&byte, 1})) return false;
      }
    }
    return append("\"");
  }
  bool number(uint64_t value) {
    char text[24];
    const int count = snprintf(text, sizeof(text), "%llu", static_cast<unsigned long long>(value));
    return count > 0 && static_cast<size_t>(count) < sizeof(text) && append({text, static_cast<size_t>(count)});
  }
  bool flush() {
    size_t offset = 0;
    while (offset < used) {
      if (!healthy()) return false;
      const int count = send(context, buffer + offset, used - offset);
      if (count < 0 || static_cast<size_t>(count) > used - offset) return fail();
      offset += static_cast<size_t>(count);
      progress();
    }
    used = 0;
    return healthy();
  }

 private:
  bool fail() {
    failed = true;
    return false;
  }
  void* context;
  Send send;
  Now now;
  Progress progress;
  uint32_t started, budgetMs;
  size_t limit, accepted = 0, used = 0;
  uint8_t buffer[1460]{};
  bool failed = false;
};
}  // namespace bounded_http
