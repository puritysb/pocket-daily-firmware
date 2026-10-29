#include <ArduinoJson.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include "network/BoundedJsonWriter.h"
namespace {
uint32_t clockMs;
uint32_t now() { return clockMs; }
void progress() { ++clockMs; }
struct Sink {
  std::string bytes;
  unsigned calls = 0;
  int maxChunk = 128;
  int failAfter = -1;
  static int send(void* p, const uint8_t* data, size_t count) {
    auto& s = *static_cast<Sink*>(p);
    ++s.calls;
    if (s.failAfter >= 0 && s.calls > static_cast<unsigned>(s.failAfter)) return -1;
    const auto n = std::min(count, static_cast<size_t>(s.maxChunk));
    s.bytes.append(reinterpret_cast<const char*>(data), n);
    return static_cast<int>(n);
  }
};
}  // namespace
TEST(BoundedHttp, EscapesControlQuoteBackslashAndPreservesUtf8) {
  clockMs = 0;
  Sink sink;
  bounded_http::JsonWriter writer(&sink, Sink::send, now, progress, 500, 4096);
  std::string name = "한국어\\\"\n";
  name.push_back('\0');
  ASSERT_TRUE(writer.quoted(name));
  ASSERT_TRUE(writer.flush());
  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, sink.bytes));
  const auto text = doc.as<JsonString>();
  EXPECT_EQ(std::string(text.c_str(), text.size()), name);
}
TEST(BoundedHttp, PartialWritesAreNotDroppedOrDuplicated) {
  clockMs = 0;
  Sink sink;
  sink.maxChunk = 7;
  bounded_http::JsonWriter writer(&sink, Sink::send, now, progress, 5000, 4096);
  std::string text(777, 'x');
  for (size_t i = 0; i < text.size(); ++i) text[i] = 'a' + i % 26;
  ASSERT_TRUE(writer.append(text));
  ASSERT_TRUE(writer.flush());
  EXPECT_EQ(sink.bytes, text);
}
TEST(BoundedHttp, BackpressureExpiresInsteadOfResettingBudget) {
  clockMs = 0;
  Sink sink;
  sink.maxChunk = 0;
  bounded_http::JsonWriter writer(&sink, Sink::send, now, progress, 10, 4096);
  ASSERT_TRUE(writer.append("body"));
  EXPECT_FALSE(writer.flush());
  EXPECT_LE(sink.calls, 10u);
  EXPECT_FALSE(writer.append("more"));
}
TEST(BoundedHttp, TransportFailureLatchesAndStopsFurtherWrites) {
  clockMs = 0;
  Sink sink;
  sink.failAfter = 1;
  bounded_http::JsonWriter writer(&sink, Sink::send, now, progress, 500, 4096);
  EXPECT_FALSE(writer.append(std::string(3000, 'x')));
  auto calls = sink.calls;
  EXPECT_FALSE(writer.flush());
  EXPECT_EQ(sink.calls, calls);
}
TEST(BoundedHttp, BudgetIsWholeRequestAndHandlesClockWrap) {
  clockMs = UINT32_MAX - 4;
  Sink sink;
  bounded_http::JsonWriter writer(&sink, Sink::send, now, progress, 10, 4096);
  ASSERT_TRUE(writer.append("a"));
  clockMs += 11;
  EXPECT_FALSE(writer.flush());
  EXPECT_EQ(sink.calls, 0u);
}
TEST(BoundedHttp, ByteLimitNeverPublishesBeyondLimit) {
  clockMs = 0;
  Sink sink;
  bounded_http::JsonWriter writer(&sink, Sink::send, now, progress, 500, 3);
  ASSERT_TRUE(writer.append("abc"));
  EXPECT_FALSE(writer.append("d"));
  EXPECT_FALSE(writer.flush());
  EXPECT_TRUE(sink.bytes.empty());
}
TEST(BoundedHttp, FormatsFullUint64WithoutTruncation) {
  clockMs = 0;
  Sink sink;
  bounded_http::JsonWriter writer(&sink, Sink::send, now, progress, 500, 4096);
  ASSERT_TRUE(writer.number(UINT64_MAX));
  ASSERT_TRUE(writer.flush());
  EXPECT_EQ(sink.bytes, "18446744073709551615");
}
