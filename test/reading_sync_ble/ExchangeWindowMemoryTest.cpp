#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include "pocket_daily/nearby_sync/ExchangeWindowMemory.h"

using namespace Pocket::NearbySync::Window;
namespace {
class WindowMemory : public ::testing::Test {
 protected:
  FontCacheManager caches;
  GfxRenderer renderer{&caches};
  GateInput input{true, true, 80, 80100, 61428};
  void SetUp() override {
    using namespace WindowMemoryTest;
    locked = false;
    releasedUnderLock = false;
    releases = locks = 0;
    freeHeap = input.freeHeap;
    block = input.largestBlock;
    releasedFree = BLE_WINDOW_MIN_FREE;
    releasedBlock = BLE_WINDOW_MIN_BLOCK;
  }
};
}  // namespace

TEST_F(WindowMemory, ReclaimsBeforeResamplingAndKeepsOriginalFloors) {
  EXPECT_EQ(prepareWindowMemory(input, renderer), Gate::OPEN);
  EXPECT_EQ(input.freeHeap, BLE_WINDOW_MIN_FREE);
  EXPECT_EQ(input.largestBlock, BLE_WINDOW_MIN_BLOCK);
  EXPECT_EQ(WindowMemoryTest::releases, 1u);
  EXPECT_TRUE(WindowMemoryTest::releasedUnderLock);
  EXPECT_FALSE(WindowMemoryTest::locked);
}

TEST_F(WindowMemory, TerminalFrameReleaseExcludesRendering) {
  EXPECT_TRUE(releaseSleepFrame(renderer));
  EXPECT_TRUE(renderer.frameReleased);
  EXPECT_EQ(WindowMemoryTest::locks, 1u);
  EXPECT_FALSE(WindowMemoryTest::locked);
}

TEST_F(WindowMemory, InsufficientTotalStillRefusesEvenWithLargeBlock) {
  WindowMemoryTest::releasedFree = BLE_WINDOW_MIN_FREE - 1;
  WindowMemoryTest::releasedBlock = 60000;
  EXPECT_EQ(prepareWindowMemory(input, renderer), Gate::LOW_MEMORY);
}

TEST_F(WindowMemory, FragmentationStillRefusesEvenWithEnoughTotal) {
  WindowMemoryTest::releasedFree = 120000;
  WindowMemoryTest::releasedBlock = BLE_WINDOW_MIN_BLOCK - 1;
  EXPECT_EQ(prepareWindowMemory(input, renderer), Gate::LOW_MEMORY);
}

TEST_F(WindowMemory, MissingCacheManagerCannotBypassTheGate) {
  renderer.caches = nullptr;
  EXPECT_EQ(prepareWindowMemory(input, renderer), Gate::LOW_MEMORY);
  EXPECT_EQ(WindowMemoryTest::releases, 0u);
}

TEST_F(WindowMemory, IneligibleWindowsDoNotTouchRenderingCaches) {
  input.enabled = false;
  EXPECT_EQ(prepareWindowMemory(input, renderer), Gate::SETTING_OFF);
  input.enabled = true;
  input.bonded = false;
  EXPECT_EQ(prepareWindowMemory(input, renderer), Gate::NO_BOND);
  input.bonded = true;
  input.batteryPercent = MIN_BATTERY_PERCENT;
  EXPECT_EQ(prepareWindowMemory(input, renderer), Gate::LOW_BATTERY);
  EXPECT_EQ(WindowMemoryTest::releases, 0u);
  EXPECT_EQ(WindowMemoryTest::locks, 0u);
}

TEST_F(WindowMemory, SufficientMemoryDoesNotEvictWarmCaches) {
  input.freeHeap = BLE_WINDOW_MIN_FREE;
  input.largestBlock = BLE_WINDOW_MIN_BLOCK;
  EXPECT_EQ(prepareWindowMemory(input, renderer), Gate::OPEN);
  EXPECT_EQ(WindowMemoryTest::releases, 0u);
  EXPECT_EQ(WindowMemoryTest::locks, 0u);
}
