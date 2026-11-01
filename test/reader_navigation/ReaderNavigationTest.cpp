#include <ArduinoJson.h>
#include <gtest/gtest.h>

#include "CrossPointState.h"
#include "activities/ActivityManager.h"
#include "activities/reader/ReaderActivity.h"

TEST(ReaderNavigation, ImmediateAndQueuedLaunchPublishOriginBeforeOnEnter) {
  for (auto shell : {ReaderReturn::Home, ReaderReturn::PocketDaily}) {
    for (auto origin : {ReaderReturn::Home, ReaderReturn::PocketDaily, ReaderReturn::Articles}) {
      for (bool queued : {false, true}) {
        ActivityManager manager;
        manager.shell = shell;
        ReaderActivity::owner = &manager;
        ReaderActivity::fail = false;
        ReaderActivity::observed = {ReaderReturn::Home, ReaderReturn::Home};
        if (queued) manager.currentActivity = std::make_unique<Activity>();
        manager.goToReaderFrom(origin, "/book.epub");
        manager.settle();
        EXPECT_EQ(ReaderActivity::observed.reader, origin);
        EXPECT_EQ(ReaderActivity::observed.shell, shell);
      }
    }
  }
}

TEST(ReaderNavigation, OrdinaryAndInternalReopenKeepParentShellAndOrigin) {
  ActivityManager manager;
  ReaderActivity::owner = &manager;
  manager.shell = ReaderReturn::PocketDaily;
  manager.goToReader("/book.epub");
  EXPECT_EQ(ReaderActivity::observed.reader, ReaderReturn::PocketDaily);
  manager.readerReturn = ReaderReturn::Articles;
  manager.resumeReader("/next.epub");
  manager.settle();
  EXPECT_EQ(ReaderActivity::observed.reader, ReaderReturn::Articles);
  EXPECT_EQ(ReaderActivity::observed.shell, ReaderReturn::PocketDaily);
}

TEST(ReaderNavigation, FailedAndNonBookOpensDoNotInstallARequestedBookOrigin) {
  ActivityManager manager;
  ReaderActivity::owner = &manager;
  ReaderActivity::fail = true;
  manager.goToReaderFrom(ReaderReturn::Articles, "/missing.epub");
  EXPECT_FALSE(manager.currentActivity);
  EXPECT_EQ(manager.readerReturn.load(), ReaderReturn::Home);
  ReaderActivity::fail = false;
  manager.goToReaderFrom(ReaderReturn::Articles, "");
  EXPECT_TRUE(manager.browserOpened);
  manager.goToReaderFrom(ReaderReturn::Articles, "/cover.bmp");
  EXPECT_EQ(manager.readerReturn.load(), ReaderReturn::Home);
}

TEST(ReaderNavigation, JsonRoundTripPreservesAllOriginsAndShellsAcrossRepeatedSleep) {
  auto& state = CrossPointState::getInstance();
  for (auto shell : {ReaderReturn::Home, ReaderReturn::PocketDaily}) {
    for (auto origin : {ReaderReturn::Home, ReaderReturn::PocketDaily, ReaderReturn::Articles}) {
      state.savedReaderNavigation = ReaderNavigation{origin, shell};
      state.sleptCleanly = true;
      for (int cycle = 0; cycle < 3; ++cycle) {
        JsonDocument doc;
        state.toJson(doc);
        state.savedReaderNavigation.reset();
        ASSERT_TRUE(state.fromJson(doc.as<JsonVariantConst>()));
        ASSERT_TRUE(state.savedReaderNavigation);
        EXPECT_EQ(state.savedReaderNavigation->reader, origin);
        EXPECT_EQ(state.savedReaderNavigation->shell, shell);
        EXPECT_TRUE(state.sleptCleanly);
      }
    }
  }
}

TEST(ReaderNavigation, InvalidOrLegacyStateFallsBackWithoutStaleContext) {
  auto& state = CrossPointState::getInstance();
  for (const auto json : {R"({})", R"({"readerNavigation":{"version":2,"reader":1,"shell":1}})",
                          R"({"readerNavigation":{"version":1,"reader":3,"shell":1}})",
                          R"({"readerNavigation":{"version":1,"reader":1,"shell":2}})",
                          R"({"readerNavigation":{"version":1,"reader":"1","shell":1}})",
                          R"({"readerNavigation":{"version":1,"reader":-1,"shell":1}})",
                          R"({"readerNavigation":{"version":1,"reader":1}})"}) {
    JsonDocument doc;
    ASSERT_EQ(deserializeJson(doc, json), DeserializationError::Ok);
    state.savedReaderNavigation = ReaderNavigation{ReaderReturn::Articles, ReaderReturn::PocketDaily};
    ASSERT_TRUE(state.fromJson(doc.as<JsonVariantConst>()));
    EXPECT_FALSE(state.savedReaderNavigation) << json;
  }
  EXPECT_EQ(legacyReaderNavigation(true).reader, ReaderReturn::PocketDaily);
  EXPECT_EQ(legacyReaderNavigation(false).shell, ReaderReturn::Home);
}

TEST(ReaderNavigation, CleanPowerCutAndRetainedWakeResumeButColdButtonDoesNot) {
  EXPECT_TRUE(hasSleepNavigation(true, false, false));  // X4 power-cut wake reported as Other
  EXPECT_TRUE(hasSleepNavigation(false, true, false));  // legacy retained frame
  EXPECT_FALSE(hasSleepNavigation(false, true, true));  // cold battery boot reports PowerButton
  EXPECT_FALSE(hasSleepNavigation(false, false, false));
}

TEST(ReaderNavigation, FailedInternalReopenPreservesActiveArticlesOrigin) {
  ActivityManager manager;
  ReaderActivity::owner = &manager;
  ReaderActivity::fail = false;
  manager.goToReaderFrom(ReaderReturn::Articles, "/article.epub");
  ReaderActivity::fail = true;
  manager.resumeReader("/next.epub");
  ReaderActivity::fail = false;
  EXPECT_TRUE(manager.currentActivity);
  EXPECT_FALSE(manager.pendingActivity);
  EXPECT_EQ(manager.readerReturn.load(), ReaderReturn::Articles);
}

TEST(ReaderNavigation, WakeShortcutFromShellUsesShellButSleepingBookKeepsItsOrigin) {
  for (auto shell : {ReaderReturn::Home, ReaderReturn::PocketDaily}) {
    for (auto oldOrigin : {ReaderReturn::Home, ReaderReturn::PocketDaily, ReaderReturn::Articles}) {
      const ReaderNavigation saved{oldOrigin, shell};
      EXPECT_EQ(wakeReaderOrigin(saved, false), shell);
      EXPECT_EQ(wakeReaderOrigin(saved, true), oldOrigin);
      ActivityManager manager;
      manager.shell = shell;
      ReaderActivity::owner = &manager;
      manager.goToReaderFrom(wakeReaderOrigin(saved, false), "/remembered.epub");
      EXPECT_EQ(ReaderActivity::observed.reader, shell);
      EXPECT_EQ(ReaderActivity::observed.shell, shell);
    }
  }
}

TEST(ReaderNavigation, ExitAndFooterLabelsFollowRestoredDestination) {
  ActivityManager manager;
  for (auto shell : {ReaderReturn::Home, ReaderReturn::PocketDaily}) {
    manager.shell = shell;
    EXPECT_STREQ(manager.shellReturnLabel(), shell == ReaderReturn::Home ? "Home" : "Back");
    manager.readerReturn = ReaderReturn::Home;
    EXPECT_STREQ(manager.readerReturnLabel(), "Home");
    manager.readerReturn = ReaderReturn::PocketDaily;
    EXPECT_STREQ(manager.readerReturnLabel(), "Pocket Daily");
    manager.readerReturn = ReaderReturn::Articles;
    EXPECT_STREQ(manager.readerReturnLabel(), "Articles");
  }
}
