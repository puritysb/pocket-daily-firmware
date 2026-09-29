// Exchange-window gates and lifecycle (docs/reading-sync-ble-v1.md, "Reader:
// exchange windows").
#include <gtest/gtest.h>

#include "pocket_daily/nearby_sync/ExchangeWindowPolicy.h"

using namespace Pocket::NearbySync::Window;
using Action = Controller::Action;
using State = Controller::State;

namespace {
GateInput openGates() {
  GateInput input;
  input.bonded = true;
  input.enabled = true;
  input.batteryPercent = 80;
  input.freeHeap = 100 * 1024;
  input.largestBlock = 48 * 1024;
  return input;
}

// Arms and opens a window at `now`, returning the controller in OPEN.
Controller openWindow(const Trigger trigger, const uint32_t now) {
  Controller window;
  window.arm(trigger, now, 7);
  EXPECT_EQ(window.tick(now, 8, true), Action::START);
  window.started(true, now);
  EXPECT_EQ(window.state(), State::OPEN);
  return window;
}
}  // namespace

TEST(ExchangeWindowPolicy, DurationsMatchTheContract) {
  EXPECT_EQ(durationMs(Trigger::BOOK_CLOSED), 45000u);
  EXPECT_EQ(durationMs(Trigger::WAKE), 45000u);
  EXPECT_EQ(durationMs(Trigger::SLEEP), 20000u);
  EXPECT_EQ(BLE_WINDOW_MIN_BLOCK, 40u * 1024u);
}

TEST(ExchangeWindowPolicy, GatesSkipTheWindowSilently) {
  EXPECT_EQ(evaluate(openGates()), Gate::OPEN);
  GateInput input = openGates();
  input.enabled = false;
  EXPECT_EQ(evaluate(input), Gate::SETTING_OFF);
  input = openGates();
  input.bonded = false;
  EXPECT_EQ(evaluate(input), Gate::NO_BOND);
  input = openGates();
  input.batteryPercent = MIN_BATTERY_PERCENT;  // must be above 10 %
  EXPECT_EQ(evaluate(input), Gate::LOW_BATTERY);
  input.batteryPercent = MIN_BATTERY_PERCENT + 1;
  EXPECT_EQ(evaluate(input), Gate::OPEN);
  input.largestBlock = BLE_WINDOW_MIN_BLOCK - 1;
  EXPECT_EQ(evaluate(input), Gate::LOW_MEMORY);
  input.largestBlock = BLE_WINDOW_MIN_BLOCK;
  EXPECT_EQ(evaluate(input), Gate::OPEN);
  input.freeHeap = BLE_WINDOW_MIN_FREE - 1;
  EXPECT_EQ(evaluate(input), Gate::LOW_MEMORY);

  EXPECT_TRUE(readyAllowed(BLE_WINDOW_READY_MIN_FREE, BLE_WINDOW_READY_MIN_BLOCK));
  EXPECT_FALSE(readyAllowed(BLE_WINDOW_READY_MIN_FREE - 1, BLE_WINDOW_READY_MIN_BLOCK));
  EXPECT_FALSE(readyAllowed(BLE_WINDOW_READY_MIN_FREE, BLE_WINDOW_READY_MIN_BLOCK - 1));
}

TEST(ExchangeWindowPolicy, BookClosedWaitsForHomeThenRunsItsTime) {
  Controller window;
  EXPECT_EQ(window.tick(0, 0, true), Action::NONE);
  window.arm(Trigger::BOOK_CLOSED, 1000, 41);
  EXPECT_EQ(window.state(), State::ARMED);
  EXPECT_EQ(window.tick(1500, 41, true), Action::NONE);  // Home not drawn yet
  EXPECT_EQ(window.tick(1600, 42, true), Action::START);
  EXPECT_EQ(window.state(), State::STARTING);
  EXPECT_TRUE(window.radioUp());
  EXPECT_EQ(window.tick(1700, 42, true), Action::NONE);
  window.started(true, 1900);
  EXPECT_EQ(window.state(), State::OPEN);
  EXPECT_EQ(window.remainingMs(1900), 44700u);
  EXPECT_EQ(window.tick(46599, 42, true), Action::NONE);
  EXPECT_EQ(window.tick(46600, 42, true), Action::STOP);
  EXPECT_EQ(window.lastCloseReason(), CloseReason::TIME_UP);
  window.stopped();
  EXPECT_EQ(window.state(), State::IDLE);
  EXPECT_FALSE(window.radioUp());
}

TEST(ExchangeWindowPolicy, AMissingFrameStillOpensAfterTheRenderWait) {
  Controller window;
  window.arm(Trigger::WAKE, 0, 3);
  EXPECT_EQ(window.tick(RENDER_WAIT_MS - 1, 3, true), Action::NONE);
  EXPECT_EQ(window.tick(RENDER_WAIT_MS, 3, true), Action::START);
}

TEST(ExchangeWindowPolicy, SleepOpensAtOnceForTwentySeconds) {
  Controller window;
  window.arm(Trigger::SLEEP, 500, 9);
  EXPECT_EQ(window.tick(500, 9, true), Action::START);  // its frame is already on the panel
  window.started(true, 700);
  EXPECT_EQ(window.remainingMs(700), 19800u);
  EXPECT_EQ(window.tick(20500, 9, true), Action::STOP);
}

TEST(ExchangeWindowPolicy, SleepDuringAWindowCarriesItIntoSleep) {
  Controller window = openWindow(Trigger::BOOK_CLOSED, 0);
  window.arm(Trigger::SLEEP, 5000, 8);  // 40 s were left: sleep keeps exactly 20 s
  EXPECT_EQ(window.state(), State::OPEN);
  EXPECT_EQ(window.remainingMs(5000), SLEEP_MS);
  EXPECT_EQ(window.trigger(), Trigger::SLEEP);

  Controller late = openWindow(Trigger::SLEEP, 0);
  late.arm(Trigger::WAKE, 10000, 8);  // a longer trigger extends
  EXPECT_EQ(late.remainingMs(10000), WAKE_MS);
  late.arm(Trigger::BOOK_CLOSED, 11000, 8);
  EXPECT_EQ(late.remainingMs(11000), BOOK_CLOSED_MS);
}

TEST(ExchangeWindowPolicy, EarlyCloses) {
  // A book opening (or any radio owner) stops a running window at once.
  Controller window = openWindow(Trigger::WAKE, 0);
  EXPECT_EQ(window.close(CloseReason::BOOK_OPENED), Action::STOP);
  EXPECT_EQ(window.lastCloseReason(), CloseReason::BOOK_OPENED);
  window.stopped();
  EXPECT_EQ(window.close(CloseReason::RADIO_OWNER), Action::NONE);  // nothing running

  // Leaving the shell while starting stops the worker's radio too.
  Controller starting;
  starting.arm(Trigger::BOOK_CLOSED, 0, 1);
  ASSERT_EQ(starting.tick(10, 2, true), Action::START);
  EXPECT_EQ(starting.tick(20, 2, false), Action::STOP);
  EXPECT_EQ(starting.lastCloseReason(), CloseReason::LEFT_SHELL);

  Controller open = openWindow(Trigger::BOOK_CLOSED, 0);
  EXPECT_EQ(open.tick(100, 8, false), Action::STOP);

  // An armed window never starts once a book (or another screen) is up.
  Controller armed;
  armed.arm(Trigger::BOOK_CLOSED, 0, 1);
  EXPECT_EQ(armed.tick(10, 2, false), Action::NONE);
  EXPECT_EQ(armed.state(), State::IDLE);
  armed.arm(Trigger::WAKE, 0, 1);
  EXPECT_EQ(armed.close(CloseReason::BOOK_OPENED), Action::NONE);
  EXPECT_EQ(armed.state(), State::IDLE);
  EXPECT_EQ(armed.tick(100000, 9, true), Action::NONE);
}

TEST(ExchangeWindowPolicy, FailedStartsReturnToIdle) {
  Controller window;
  window.arm(Trigger::WAKE, 0, 0);
  ASSERT_EQ(window.tick(0, 1, true), Action::START);
  window.started(false, 10);
  EXPECT_EQ(window.state(), State::IDLE);
  EXPECT_EQ(window.lastCloseReason(), CloseReason::START_FAILED);
  window.started(true, 20);  // late reports are ignored
  EXPECT_EQ(window.state(), State::IDLE);

  // A start slower than the window itself closes on the next pass.
  window.arm(Trigger::SLEEP, 0, 0);
  ASSERT_EQ(window.tick(0, 0, true), Action::START);
  window.started(true, SLEEP_MS + 5);
  EXPECT_EQ(window.tick(SLEEP_MS + 6, 0, true), Action::STOP);
}

TEST(ExchangeWindowPolicy, MillisWrapIsHandled) {
  const uint32_t nearWrap = 0xFFFFFFFFu - 1000u;
  Controller window = openWindow(Trigger::SLEEP, nearWrap);
  EXPECT_EQ(window.tick(nearWrap + 19999u, 8, true), Action::NONE);
  EXPECT_EQ(window.tick(nearWrap + 20000u, 8, true), Action::STOP);
}

TEST(ExchangeWindowPolicy, HistoricalStarvedHomeIsSkippedAndFragmentationCloses) {
  auto input = openGates();
  input.freeHeap = 82900;
  input.largestBlock = 77800;
  EXPECT_EQ(evaluate(input), Gate::LOW_MEMORY);
  EXPECT_FALSE(readyAllowed(13700, 11300));
  EXPECT_FALSE(runningAllowed(30000, 2048));
  EXPECT_FALSE(runningAllowed(BLE_WINDOW_RUNNING_MIN_FREE - 1, 10000));
  EXPECT_TRUE(runningAllowed(BLE_WINDOW_RUNNING_MIN_FREE, BLE_WINDOW_RUNNING_MIN_BLOCK));
}

TEST(ExchangeWindowPolicy, ConnectionDuringStartupStillExpires) {
  UnbondedGrace grace;
  // Advertising already accepted a peer while the owner awaited init's result.
  EXPECT_FALSE(grace.expired(1, 100, 5099));
  EXPECT_TRUE(grace.expired(1, 100, 5100));
  EXPECT_FALSE(grace.expired(1, 100, 6000));
}

TEST(ExchangeWindowPolicy, ReconnectedPeerGetsItsOwnGracePeriod) {
  UnbondedGrace grace;
  EXPECT_TRUE(grace.expired(1, 100, 5100));
  EXPECT_FALSE(grace.expired(3, 6000, 10999));
  EXPECT_TRUE(grace.expired(3, 6000, 11000));
}

TEST(ExchangeWindowPolicy, UnbondedGraceHandlesMillisWrap) {
  UnbondedGrace grace;
  const uint32_t connected = UINT32_MAX - 1000;
  EXPECT_FALSE(grace.expired(1, connected, connected + 4999));
  EXPECT_TRUE(grace.expired(1, connected, connected + 5000));
}
