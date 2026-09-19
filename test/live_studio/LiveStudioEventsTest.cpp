#include "pocket_daily/live_studio/LiveStudioEvents.h"

#include <cstring>

#include <gtest/gtest.h>

namespace {

using PocketDaily::LiveStudio::ClientMessage;
using PocketDaily::LiveStudio::Subscription;

// Lengths are derived from the literals so a test can never read past a
// message the transport would have delivered.
ClientMessage parse(const char* message, Subscription* out = nullptr) {
  return PocketDaily::LiveStudio::parseClientMessage(message, std::strlen(message), out);
}

TEST(LiveStudioEvents, HelloCarriesProtocolIdentityAndCaps) {
  char out[192];
  ASSERT_TRUE(PocketDaily::LiveStudio::encodeHello(out, sizeof(out), "ABCD1234", "1.4.1-dev"));
  EXPECT_STREQ(out,
               "{\"hello\":{\"proto\":\"live-studio/1\",\"deviceID\":\"ABCD1234\",\"version\":\"1.4.1-dev\","
               "\"caps\":[\"status\",\"prefs\"]}}");
}

TEST(LiveStudioEvents, EncodersRejectSmallBuffers) {
  char tiny[8];
  EXPECT_FALSE(PocketDaily::LiveStudio::encodeHello(tiny, sizeof(tiny), "ABCD1234", "1.4.1"));
  EXPECT_FALSE(PocketDaily::LiveStudio::encodePong(tiny, sizeof(tiny)));
  EXPECT_FALSE(PocketDaily::LiveStudio::encodeBye(tiny, sizeof(tiny)));
  EXPECT_FALSE(PocketDaily::LiveStudio::encodePrefsChanged(tiny, sizeof(tiny)));
}

TEST(LiveStudioEvents, SimpleEventsHaveExactBodies) {
  char pong[32];
  ASSERT_TRUE(PocketDaily::LiveStudio::encodePong(pong, sizeof(pong)));
  EXPECT_STREQ(pong, "{\"pong\":{}}");
  char bye[32];
  ASSERT_TRUE(PocketDaily::LiveStudio::encodeBye(bye, sizeof(bye)));
  EXPECT_STREQ(bye, "{\"bye\":{}}");
  char prefs[48];
  ASSERT_TRUE(PocketDaily::LiveStudio::encodePrefsChanged(prefs, sizeof(prefs)));
  EXPECT_STREQ(prefs, "{\"prefs\":{\"changed\":true}}");
}

TEST(LiveStudioEvents, StatusEventEmbedsStatusVerbatim) {
  char out[128];
  ASSERT_TRUE(PocketDaily::LiveStudio::encodeStatusEvent(out, sizeof(out), "{\"version\":\"1.4.1\"}"));
  EXPECT_STREQ(out, "{\"status\":{\"version\":\"1.4.1\"}}");
  // Exact bound: body of 19 needs 10 + 19 + '}' + NUL = 31 bytes.
  char tight[30];
  EXPECT_FALSE(PocketDaily::LiveStudio::encodeStatusEvent(tight, sizeof(tight), "{\"version\":\"1.4.1\"}"));
  char exact[31];
  EXPECT_TRUE(PocketDaily::LiveStudio::encodeStatusEvent(exact, sizeof(exact), "{\"version\":\"1.4.1\"}"));
}

TEST(LiveStudioEvents, ParsesSubscribeWithClamping) {
  Subscription sub;
  EXPECT_EQ(ClientMessage::Subscribe, parse("{\"subscribe\":{\"frames\":true,\"minIntervalMs\":100}}", &sub));
  EXPECT_TRUE(sub.frames);
  // Below the documented floor: the reader clamps, the app never gets faster
  // pushes than the contract allows.
  EXPECT_EQ(sub.minIntervalMs, PocketDaily::LiveStudio::kMinCaptureIntervalMs);

  Subscription slow;
  EXPECT_EQ(ClientMessage::Subscribe, parse("{\"subscribe\":{\"frames\":false,\"minIntervalMs\":5000}}", &slow));
  EXPECT_FALSE(slow.frames);
  EXPECT_EQ(slow.minIntervalMs, 5000u);

  Subscription absurd;
  EXPECT_EQ(ClientMessage::Subscribe, parse("{\"subscribe\":{\"frames\":true,\"minIntervalMs\":999999999}}", &absurd));
  EXPECT_EQ(absurd.minIntervalMs, 60000u);
}

TEST(LiveStudioEvents, ParsesUnsubscribePingAndRejectsGarbage) {
  EXPECT_EQ(ClientMessage::Ping, parse("{\"ping\":{}}"));
  EXPECT_EQ(ClientMessage::Unsubscribe, parse("{\"unsubscribe\":{}}"));
  // Not JSON, unknown keys, empty, and binary junk are all None so the
  // server ignores them without touching upload state.
  EXPECT_EQ(ClientMessage::None, parse("START:book.epub:10:/"));
  EXPECT_EQ(ClientMessage::None, parse("{\"what\":1}"));
  EXPECT_EQ(ClientMessage::None, PocketDaily::LiveStudio::parseClientMessage("", 0, nullptr));
  EXPECT_EQ(ClientMessage::None, PocketDaily::LiveStudio::parseClientMessage("\x01\x02", 2, nullptr));
}

TEST(LiveStudioEvents, FrameCapturePolicy) {
  const uint32_t floor = PocketDaily::LiveStudio::kMinCaptureFreeHeap;
  EXPECT_FALSE(PocketDaily::LiveStudio::shouldCaptureFrame(floor - 1, 1000, 0));
  EXPECT_TRUE(PocketDaily::LiveStudio::shouldCaptureFrame(floor, 1000, 0));
  EXPECT_FALSE(PocketDaily::LiveStudio::shouldCaptureFrame(floor, 1000 + 249, 1000));
  EXPECT_TRUE(PocketDaily::LiveStudio::shouldCaptureFrame(floor, 1000 + 250, 1000));
  // Spacing since the previous capture uses wrap-safe math: 0xFFFFFFF0 is
  // -16 ms, so 100 - (-16) = 116 is below the floor; 0xFFFFFF00 is -256 ms,
  // so 400 - (-256) = 656 crosses it.
  EXPECT_FALSE(PocketDaily::LiveStudio::shouldCaptureFrame(floor, 100, 0xFFFFFFF0u));
  EXPECT_TRUE(PocketDaily::LiveStudio::shouldCaptureFrame(floor, 400, 0xFFFFFF00u));
}

}  // namespace
