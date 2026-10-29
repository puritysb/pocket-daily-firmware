#include <gtest/gtest.h>

#include "pocket_daily/boot/TrialReaderPolicy.h"
using namespace PocketDaily::TrialReader;
namespace {
Record prepared() {
  Record r;
  r.run = 781;
  r.requested = 20;
  r.bytes = 234567;
  r.checksum = checksum(r);
  return r;
}
void reseal(Record& r) { r.checksum = checksum(r); }
}  // namespace
TEST(TrialReader, RequiresExactBoundedRecordAndChecksum) {
  auto r = prepared();
  EXPECT_TRUE(valid(r));
  r.run++;
  EXPECT_FALSE(valid(r));
  reseal(r);
  EXPECT_TRUE(valid(r));
  r.magic++;
  reseal(r);
  EXPECT_FALSE(valid(r));
}
TEST(TrialReader, RejectsUnboundedOrMalformedCounts) {
  for (auto pages : {0u, 1u, 21u, UINT32_MAX}) {
    auto r = prepared();
    r.requested = pages;
    reseal(r);
    EXPECT_FALSE(valid(r));
  }
  for (auto bytes : {0u, 4u * 1024 * 1024 + 1, UINT32_MAX}) {
    auto r = prepared();
    r.bytes = bytes;
    reseal(r);
    EXPECT_FALSE(valid(r));
  }
  auto r = prepared();
  r.rendered = 21;
  reseal(r);
  EXPECT_FALSE(valid(r));
  r = prepared();
  r.stage = static_cast<Stage>(9);
  reseal(r);
  EXPECT_FALSE(valid(r));
}
TEST(TrialReader, CountsOnlyStrictlyAdvancedCompletedPositions) {
  EXPECT_TRUE(progressed(2, 4, 2, 3));
  EXPECT_TRUE(progressed(3, 1, 2, 99));
  EXPECT_FALSE(progressed(2, 3, 2, 3));
  EXPECT_FALSE(progressed(1, 99, 2, 3));
  EXPECT_FALSE(progressed(2, 2, 2, 3));
  EXPECT_FALSE(progressed(-1, 4, -2, 3));
  EXPECT_FALSE(progressed(3, 0, 2, 3));
}
TEST(TrialReader, ContinuityRequiresSpinePageAndVisibleOffset) {
  auto r = prepared();
  r.spine = 3;
  r.page = 7;
  r.hasVisibleOffset = 1;
  r.visibleOffset = 791;
  EXPECT_TRUE(matches(r, 3, 7, true, 791));
  EXPECT_FALSE(matches(r, 4, 7, true, 791));
  EXPECT_FALSE(matches(r, 3, 8, true, 791));
  EXPECT_FALSE(matches(r, 3, 7, false, 791));
  EXPECT_FALSE(matches(r, 3, 7, true, 792));
  r.hasVisibleOffset = 0;
  EXPECT_TRUE(matches(r, 3, 7, false, 123));
  EXPECT_FALSE(matches(r, 3, 7, true, 123));
}
TEST(TrialReader, SleepRequiresEveryIndependentPrerequisite) {
  auto r = prepared();
  r.rendered = r.memorySamples = r.renderMemorySamples = 20;
  r.reopened = r.missingObserved = r.recovered = 1;
  EXPECT_TRUE(readyForSleep(r));
  for (auto field : {&Record::rendered, &Record::reopened, &Record::missingObserved, &Record::recovered,
                     &Record::memorySamples, &Record::renderMemorySamples}) {
    auto missing = r;
    missing.*field = 0;
    EXPECT_FALSE(readyForSleep(missing));
  }
}

TEST(TrialReader, MissingFreshRenderCannotPermitSleep) {
  auto r = prepared();
  r.rendered = r.memorySamples = 20;
  r.reopened = r.missingObserved = r.recovered = 1;
  r.renderMemorySamples = 19;
  EXPECT_FALSE(readyForSleep(r));
  r.renderMemorySamples = 20;
  EXPECT_TRUE(readyForSleep(r));
  r.renderMemorySamples = 21;
  EXPECT_FALSE(readyForSleep(r));
}

#include "pocket_daily/boot/RTCProbePolicy.h"
TEST(RTCProbe, OnlyMatchingActualHardwareGPIOCanResumeOnce) {
  uint32_t trace[32]{};
  trace[0] = PocketDaily::RTCProbe::MAGIC;
  trace[1] = 739;
  trace[2] = 2;
  trace[3] = 8;
  trace[5] = 4;
  trace[6] = 8;
  trace[31] = 1;
  using PocketDaily::RTCProbe::permitsGPIOResume;
  EXPECT_TRUE(permitsGPIOResume(trace, 739, true, true, 8));
  EXPECT_FALSE(permitsGPIOResume(trace, 738, true, true, 8));
  EXPECT_FALSE(permitsGPIOResume(trace, 739, false, true, 8));
  EXPECT_FALSE(permitsGPIOResume(trace, 739, true, false, 8));
  EXPECT_FALSE(permitsGPIOResume(trace, 739, true, true, 0));
  EXPECT_FALSE(permitsGPIOResume(trace, 739, true, true, 9));
  for (const auto field : {0u, 1u, 2u, 3u, 5u, 6u, 31u}) {
    const auto saved = trace[field];
    trace[field] = 0;
    EXPECT_FALSE(permitsGPIOResume(trace, 739, true, true, 8)) << field;
    trace[field] = saved;
  }
  trace[2] = 5;
  EXPECT_TRUE(PocketDaily::RTCProbe::owned(trace, 739));
  EXPECT_FALSE(permitsGPIOResume(trace, 739, true, true, 8));
  trace[2] = 3;
  EXPECT_FALSE(permitsGPIOResume(trace, 739, true, true, 8));
}

#include "pocket_daily/boot/TrialWakePolicy.h"
TEST(TrialWake, RequiresRealIdleAndOneMatchingTimerWake) {
  using W = PocketDaily::TrialWake::Stage;
  PocketDaily::TrialWake::Record r;
  r.run = 812;
  r.stage = W::SleepCommitted;
  r.timeoutMs = 60000;
  r.idleMs = 60007;
  auto seal = [&]() { r.checksum = PocketDaily::TrialWake::checksum(r); };
  seal();
  using PocketDaily::TrialWake::permitsResume;
  EXPECT_TRUE(permitsResume(r, 812, true, true));
  EXPECT_FALSE(permitsResume(r, 813, true, true));
  EXPECT_FALSE(permitsResume(r, 812, false, true));
  EXPECT_FALSE(permitsResume(r, 812, true, false));
  for (const auto stage : {W::Prepared, W::WaitingIdle, W::ObservingResume, W::TurnPending, W::Complete, W::Failed}) {
    r.stage = stage;
    seal();
    EXPECT_FALSE(permitsResume(r, 812, true, true));
  }
  r.stage = W::SleepCommitted;
  r.idleMs = 59999;
  seal();
  EXPECT_FALSE(permitsResume(r, 812, true, true));
  r.idleMs = 60001;
  r.timeoutMs = 1;
  seal();
  EXPECT_FALSE(permitsResume(r, 812, true, true));
  r.timeoutMs = 60000;
  seal();
  r.run++;
  EXPECT_FALSE(permitsResume(r, 813, true, true));
}

TEST(TrialWake, NaturalIdleCannotBeRecordedDuringIncompleteReaderPreparation) {
  using PocketDaily::TrialWake::permitsIdleObservation;
  EXPECT_TRUE(permitsIdleObservation(true, 60000, 60010));
  EXPECT_FALSE(permitsIdleObservation(false, 60000, 60010));
  EXPECT_FALSE(permitsIdleObservation(true, 60000, 59999));
  EXPECT_FALSE(permitsIdleObservation(true, 1, 60010));
}
