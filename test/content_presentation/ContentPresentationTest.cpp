#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>

#include "PresentationFakes.h"
#include "pocket_daily/ContentPresentation.h"

// Replace only storage loading; the production controller and model layout are
// compiled unchanged. Full real-store recovery is covered by content_store_test.
namespace PocketDaily::Content {
ContentViewState::LoadResult ContentViewState::load(const char* revision, void (*)()) {
  reset();
  if (PresentationFake::loadResult != LoadResult::Ready && PresentationFake::loadResult != LoadResult::Empty)
    return PresentationFake::loadResult;
  std::memcpy(revision_, revision, sizeof(revision_));
  generation_ = 7;
  if (PresentationFake::loadResult == LoadResult::Ready) {
    cards_ = std::make_unique<RevisionCards>();
    cards_->count = PresentationFake::cardCount;
    for (uint8_t i = 0; i < cards_->count; ++i) cards_->cards[i].card.title[0] = 'A' + i;
  }
  return PresentationFake::loadResult;
}
void ContentViewState::reset() {
  cards_.reset();
  revision_[0] = 0;
  generation_ = 0;
}
}  // namespace PocketDaily::Content

using namespace PocketDaily::Content;
class ContentPresentationTest : public testing::Test {
 protected:
  void SetUp() override {
    using namespace PresentationFake;
    loadResult = ContentViewState::LoadResult::Ready;
    cardCount = 3;
    fontAvailable = fontReady = drawSucceeds = true;
    missingGlyphs = loads = releases = draws = displays = 0;
    titles.clear();
    checkedText.clear();
    labels.clear();
    duringDisplay = {};
  }
  void TearDown() override { PresentationFake::duringDisplay = {}; }
  const char* revision = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  ContentPresentation presentation;
  GfxRenderer renderer;
  MappedInputManager input;
};

TEST_F(ContentPresentationTest, NavigationHintsDescribeDistinctActionsAndAreFontChecked) {
  ASSERT_TRUE(presentation.prepare(revision, renderer));
  ASSERT_TRUE(presentation.render(renderer, input));
  EXPECT_EQ(PresentationFake::labels, (std::vector<std::string>{"Back", "", "Prev", "Next"}));
  EXPECT_NE(std::find(PresentationFake::checkedText.begin(), PresentationFake::checkedText.end(), "Prev"),
            PresentationFake::checkedText.end());
  EXPECT_NE(std::find(PresentationFake::checkedText.begin(), PresentationFake::checkedText.end(), "Next"),
            PresentationFake::checkedText.end());
}

TEST_F(ContentPresentationTest, DeferredRequestDoesNotLoadOrPaintUntilServicedAfterHttp) {
  ASSERT_TRUE(presentation.enqueue(revision, 7));
  EXPECT_TRUE(presentation.busy());
  EXPECT_EQ(presentation.receipt().phase, PresentationPhase::Queued);
  EXPECT_EQ(presentation.receipt().generation, 7);
  EXPECT_STREQ(presentation.receipt().revision, revision);
  EXPECT_TRUE(presentation.render(renderer, input));
  EXPECT_EQ(PresentationFake::loads, 0);
  EXPECT_EQ(PresentationFake::displays, 0);
  EXPECT_FALSE(presentation.enqueue(revision, 7));
  ASSERT_TRUE(presentation.service(renderer, 16384, 4096));
  EXPECT_EQ(PresentationFake::loads, 1);
  EXPECT_TRUE(presentation.busy());
  EXPECT_FALSE(presentation.service(renderer, 16384, 4096));
  presentation.render(renderer, input);
  EXPECT_EQ(presentation.receipt().phase, PresentationPhase::Rendered);
  EXPECT_EQ(PresentationFake::displays, 1);
}

TEST_F(ContentPresentationTest, DeferredAdmissionKeepsBothFloorsAndFailureReceiptWithoutRetry) {
  for (const auto budget : {std::pair{16383U, 4096U}, std::pair{16384U, 4095U}}) {
    ASSERT_TRUE(presentation.enqueue(revision, 7));
    EXPECT_FALSE(presentation.service(renderer, budget.first, budget.second));
    EXPECT_FALSE(presentation.busy());
    EXPECT_EQ(presentation.receipt().phase, PresentationPhase::Failed);
    EXPECT_EQ(presentation.receipt().failure, PresentationFailure::Memory);
    EXPECT_EQ(presentation.receipt().heap, budget.first);
    EXPECT_EQ(presentation.receipt().block, budget.second);
    EXPECT_EQ(presentation.receipt().generation, 7);
    EXPECT_STREQ(presentation.receipt().revision, revision);
    EXPECT_FALSE(presentation.service(renderer, 32000, 16000));
    presentation.render(renderer, input);
  }
  EXPECT_EQ(PresentationFake::loads, 0);
  EXPECT_EQ(PresentationFake::displays, 0);
}

TEST_F(ContentPresentationTest, DeferredStorageFailureAndChangedGenerationNeverPaint) {
  ASSERT_TRUE(presentation.enqueue(revision, 8));
  EXPECT_FALSE(presentation.service(renderer, 16384, 4096));
  EXPECT_EQ(presentation.receipt().failure, PresentationFailure::Preparation);
  EXPECT_EQ(presentation.receipt().generation, 8);
  EXPECT_EQ(PresentationFake::releases, 1);
  EXPECT_FALSE(presentation.busy());
  PresentationFake::loadResult = ContentViewState::LoadResult::Unavailable;
  ASSERT_TRUE(presentation.enqueue(revision, 7));
  EXPECT_FALSE(presentation.service(renderer, 16384, 4096));
  EXPECT_STREQ(presentation.receipt().revision, revision);
  EXPECT_EQ(presentation.receipt().generation, 7);
  EXPECT_EQ(presentation.receipt().failure, PresentationFailure::Preparation);
  presentation.render(renderer, input);
  EXPECT_EQ(PresentationFake::displays, 0);
}

TEST_F(ContentPresentationTest, HideCancelsPendingPreparationAndInvalidQueueDoesNotChangeState) {
  EXPECT_FALSE(presentation.enqueue(nullptr, 7));
  EXPECT_FALSE(presentation.enqueue("bad", 7));
  EXPECT_FALSE(presentation.enqueue(revision, 0));
  ASSERT_TRUE(presentation.enqueue(revision, 7));
  presentation.hide(renderer);
  EXPECT_FALSE(presentation.service(renderer, 16384, 4096));
  EXPECT_FALSE(presentation.busy());
  EXPECT_EQ(presentation.receipt().generation, 0);
  EXPECT_EQ(PresentationFake::loads, 0);
}

TEST_F(ContentPresentationTest, QueuesUntilDriverReturnsAndReleasesFontBeforeAdmittingWriters) {
  EXPECT_FALSE(presentation.render(renderer, input));
  ASSERT_TRUE(presentation.prepare(revision, renderer));
  EXPECT_TRUE(presentation.busy());
  EXPECT_EQ(presentation.receipt().phase, PresentationPhase::Queued);
  PresentationFake::duringDisplay = [&] {
    EXPECT_TRUE(presentation.busy());
    EXPECT_EQ(presentation.receipt().phase, PresentationPhase::Queued);
    EXPECT_EQ(PresentationFake::releases, 0);
  };
  EXPECT_TRUE(presentation.render(renderer, input));
  EXPECT_FALSE(presentation.busy());
  EXPECT_EQ(PresentationFake::releases, 1);
  EXPECT_EQ(presentation.receipt().phase, PresentationPhase::Rendered);
  EXPECT_STREQ(presentation.receipt().revision, revision);
  EXPECT_EQ(presentation.receipt().generation, 7);
  EXPECT_TRUE(presentation.render(renderer, input));
  EXPECT_EQ(PresentationFake::displays, 1);  // incidental updates never repaint
}

TEST_F(ContentPresentationTest, FailedPaintDoesNotDisplayOrLeaveWriterGateHeld) {
  ASSERT_TRUE(presentation.prepare(revision, renderer));
  PresentationFake::drawSucceeds = false;
  EXPECT_TRUE(presentation.render(renderer, input));
  EXPECT_EQ(presentation.receipt().phase, PresentationPhase::Failed);
  EXPECT_FALSE(presentation.busy());
  EXPECT_EQ(PresentationFake::displays, 0);
  EXPECT_EQ(PresentationFake::releases, 1);
  EXPECT_TRUE(presentation.render(renderer, input));
  EXPECT_EQ(PresentationFake::draws, 1);
  presentation.hide(renderer);
  EXPECT_FALSE(presentation.visible());
  EXPECT_EQ(presentation.receipt().generation, 0);
  EXPECT_EQ(PresentationFake::releases, 1);
}

TEST_F(ContentPresentationTest, StorageFailureDoesNotLoadFontsOrExposeSuccess) {
  for (const auto failure : {ContentViewState::LoadResult::NoActive, ContentViewState::LoadResult::InvalidTarget,
                             ContentViewState::LoadResult::TargetChanged, ContentViewState::LoadResult::Unavailable,
                             ContentViewState::LoadResult::OutOfMemory}) {
    PresentationFake::loadResult = failure;
    EXPECT_FALSE(presentation.prepare(revision, renderer));
    EXPECT_FALSE(presentation.busy());
    EXPECT_EQ(presentation.receipt().generation, 0);
    EXPECT_EQ(presentation.receipt().phase, PresentationPhase::Failed);
  }
  EXPECT_EQ(PresentationFake::loads, 0);
  EXPECT_EQ(PresentationFake::displays, 0);
}

TEST_F(ContentPresentationTest, MissingCoverageAndFontReadFailureReleaseLoadedFont) {
  PresentationFake::missingGlyphs = 1;
  EXPECT_FALSE(presentation.prepare(revision, renderer));
  EXPECT_FALSE(presentation.busy());
  EXPECT_EQ(PresentationFake::releases, 1);
  PresentationFake::missingGlyphs = 0;
  PresentationFake::fontReady = false;
  EXPECT_FALSE(presentation.prepare(revision, renderer));
  EXPECT_EQ(PresentationFake::releases, 2);
  EXPECT_EQ(PresentationFake::displays, 0);
  PresentationFake::fontReady = true;
  PresentationFake::fontAvailable = false;
  EXPECT_FALSE(presentation.prepare(revision, renderer));
  EXPECT_EQ(PresentationFake::releases, 2);
  EXPECT_FALSE(presentation.busy());
}

TEST_F(ContentPresentationTest, NavigationWrapsAndFailedReloadDoesNotPaintOrRetainBusy) {
  ASSERT_TRUE(presentation.prepare(revision, renderer));
  EXPECT_FALSE(presentation.navigate(true, renderer));  // queued frame owns the gate
  presentation.render(renderer, input);
  ASSERT_TRUE(presentation.navigate(false, renderer));
  presentation.render(renderer, input);
  ASSERT_TRUE(presentation.navigate(true, renderer));
  presentation.render(renderer, input);
  EXPECT_EQ(PresentationFake::titles, (std::vector<char>{'A', 'C', 'A'}));
  PresentationFake::fontAvailable = false;
  EXPECT_FALSE(presentation.navigate(true, renderer));
  EXPECT_EQ(presentation.receipt().phase, PresentationPhase::Failed);
  EXPECT_FALSE(presentation.busy());
  EXPECT_EQ(PresentationFake::displays, 3);
}

TEST_F(ContentPresentationTest, EmptyContentPaintsAndHideReleasesQueuedResources) {
  PresentationFake::loadResult = ContentViewState::LoadResult::Empty;
  ASSERT_TRUE(presentation.prepare(revision, renderer));
  presentation.render(renderer, input);
  EXPECT_EQ(PresentationFake::titles, (std::vector<char>{'-'}));
  EXPECT_FALSE(presentation.navigate(true, renderer));
  ASSERT_TRUE(presentation.prepare(revision, renderer));
  presentation.hide(renderer);
  EXPECT_FALSE(presentation.busy());
  EXPECT_FALSE(presentation.render(renderer, input));
  EXPECT_EQ(PresentationFake::releases, 2);
  EXPECT_EQ(presentation.receipt().phase, PresentationPhase::Idle);
}

TEST_F(ContentPresentationTest, FailedPaintRecoversOnlyThroughExplicitNewPreparation) {
  ASSERT_TRUE(presentation.prepare(revision, renderer));
  PresentationFake::drawSucceeds = false;
  presentation.render(renderer, input);
  PresentationFake::drawSucceeds = true;
  presentation.render(renderer, input);
  EXPECT_EQ(PresentationFake::displays, 0);
  const char* replacement = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  ASSERT_TRUE(presentation.prepare(replacement, renderer));
  EXPECT_EQ(presentation.receipt().phase, PresentationPhase::Queued);
  EXPECT_STREQ(presentation.receipt().revision, replacement);
  presentation.render(renderer, input);
  EXPECT_EQ(presentation.receipt().phase, PresentationPhase::Rendered);
  EXPECT_EQ(PresentationFake::displays, 1);
  EXPECT_EQ(PresentationFake::releases, 2);
}

TEST_F(ContentPresentationTest, CaptureRequiresCompletedDisplayAndRejectsPendingFailedOrHiddenFrames) {
  EXPECT_FALSE(presentation.canCaptureFrame());
  ASSERT_TRUE(presentation.prepare(revision, renderer));
  EXPECT_FALSE(presentation.canCaptureFrame());
  PresentationFake::duringDisplay = [&] { EXPECT_FALSE(presentation.canCaptureFrame()); };
  presentation.render(renderer, input);
  EXPECT_TRUE(presentation.canCaptureFrame());
  ASSERT_TRUE(presentation.navigate(true, renderer));
  EXPECT_FALSE(presentation.canCaptureFrame());
  PresentationFake::drawSucceeds = false;
  presentation.render(renderer, input);
  EXPECT_FALSE(presentation.canCaptureFrame());
  presentation.hide(renderer);
  EXPECT_FALSE(presentation.canCaptureFrame());
  PresentationFake::drawSucceeds = true;
  ASSERT_TRUE(presentation.prepare(revision, renderer));
  presentation.render(renderer, input);
  EXPECT_TRUE(presentation.canCaptureFrame());
  PresentationFake::loadResult = ContentViewState::LoadResult::Unavailable;
  EXPECT_FALSE(presentation.prepare(revision, renderer));
  EXPECT_FALSE(presentation.canCaptureFrame());
}
