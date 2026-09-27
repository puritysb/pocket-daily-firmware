#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "PresentationFakes.h"
#include "pocket_daily/ContentSessionInput.h"
#include "pocket_daily/PocketProfileStore.h"
#include "pocket_daily/PresentationSlot.h"
#include "pocket_daily/ScreenFrame.h"
#include "pocket_daily/web/GenerationArgument.h"

TEST(ScreenGenerationArgument, AcceptsOnlyBoundedDecimalWithoutChangingOutputOnFailure) {
  uint32_t value = 42;
  for (const auto text : {"", "-1", "+1", " 1", "1 ", "1x", "1.0", "4294967296", "9999999999", "00000000000"}) {
    EXPECT_FALSE(PocketDaily::Web::parseGeneration(text, value)) << text;
    EXPECT_EQ(value, 42U);
  }
  EXPECT_FALSE(PocketDaily::Web::parseGeneration(std::string_view("1\0x", 3), value));
  ASSERT_TRUE(PocketDaily::Web::parseGeneration("0", value));
  EXPECT_EQ(value, 0U);
  ASSERT_TRUE(PocketDaily::Web::parseGeneration("0000000007", value));
  EXPECT_EQ(value, 7U);
  ASSERT_TRUE(PocketDaily::Web::parseGeneration("4294967295", value));
  EXPECT_EQ(value, UINT32_MAX);
}

// The production ScreenPresentation, PresentationSlot and ContentPresentation
// run against fake storage, font and display boundaries. The frame module
// (SD inputs and the shared painters) is replaced below; it is device code.
namespace ScreenFake {
using PocketDaily::Screen::LoadResult;
inline LoadResult loadResult = LoadResult::Ready;
inline int loads = 0;
inline PocketDaily::Screen::Surface loadedSurface = PocketDaily::Screen::Surface::None;
inline std::vector<std::string> texts;
inline bool drawSucceeds = true;
inline int draws = 0;
inline int drawnFont = -1;
inline uint8_t drawOrientation = 0xFF;
inline std::function<void()> duringDraw;
inline PocketDaily::DailyProfile::Profile profile{};
inline uint32_t generation = 7;
}  // namespace ScreenFake

namespace PocketDaily::DailyProfile {
const Profile& current() { return ScreenFake::profile; }
uint32_t generation() { return ScreenFake::generation; }
}  // namespace PocketDaily::DailyProfile

namespace PocketDaily::Screen {
LoadResult loadFrame(Surface surface, const DailyProfile::Profile&, std::unique_ptr<Frame>& out) {
  ++ScreenFake::loads;
  ScreenFake::loadedSurface = surface;
  out.reset();
  if (ScreenFake::loadResult != LoadResult::Ready) return ScreenFake::loadResult;
  out = std::make_unique<Frame>();
  out->surface = surface;
  return LoadResult::Ready;
}
bool visitFrameText(const Frame&, void* context, bool (*visit)(void*, const char*)) {
  for (const auto& text : ScreenFake::texts)
    if (!visit(context, text.c_str())) return false;
  return true;
}
bool drawFrame(GfxRenderer&, const Frame&, int cjkFont) {
  ++ScreenFake::draws;
  ScreenFake::drawnFont = cjkFont;
  ScreenFake::drawOrientation = PresentationFake::orientation;
  if (ScreenFake::duringDraw) ScreenFake::duringDraw();
  return ScreenFake::drawSucceeds;
}
}  // namespace PocketDaily::Screen

// Same storage fake as the content presentation test.
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
ContentPageStyle currentContentPageStyle() { return {20, 5, 16, "Pocket", "Empty"}; }
}  // namespace PocketDaily::Content

using PocketDaily::Content::PresentationFailure;
using PocketDaily::Content::PresentationPhase;
using PocketDaily::Screen::LoadResult;
using PocketDaily::Screen::ScreenPresentation;
using PocketDaily::Screen::Surface;

class ScreenPresentationTest : public testing::Test {
 protected:
  void SetUp() override {
    using namespace PresentationFake;
    loadResult = PocketDaily::Content::ContentViewState::LoadResult::Ready;
    cardCount = 3;
    fontAvailable = fontReady = drawSucceeds = true;
    missingGlyphs = loads = releases = draws = displays = 0;
    titles.clear();
    checkedText.clear();
    labels.clear();
    duringDisplay = {};
    orientation = 1;  // the transfer view is not portrait in these tests
    ScreenFake::loadResult = LoadResult::Ready;
    ScreenFake::loads = ScreenFake::draws = 0;
    ScreenFake::loadedSurface = Surface::None;
    ScreenFake::texts = {"Pocket Daily", "Library", "Continue Reading"};
    ScreenFake::drawSucceeds = true;
    ScreenFake::drawnFont = -1;
    ScreenFake::drawOrientation = 0xFF;
    ScreenFake::duringDraw = {};
    ScreenFake::profile = PocketDaily::DailyProfile::Profile{};
    ScreenFake::generation = 7;
  }
  void TearDown() override {
    PresentationFake::duringDisplay = {};
    ScreenFake::duringDraw = {};
  }
  static bool checked(const std::string& text) {
    const auto& all = PresentationFake::checkedText;
    return std::find(all.begin(), all.end(), text) != all.end();
  }
  void expectFailedAndReleased(const ScreenPresentation& screen, PresentationFailure failure) {
    EXPECT_EQ(screen.receipt().phase, PresentationPhase::Failed);
    EXPECT_EQ(screen.receipt().failure, failure);
    EXPECT_FALSE(screen.busy());
    EXPECT_FALSE(screen.holdsInputs());
    EXPECT_FALSE(screen.canCaptureFrame());
  }
  ScreenPresentation screen;
  GfxRenderer renderer;
  MappedInputManager input;
};

TEST_F(ScreenPresentationTest, QueueRecordsOnlySurfaceAndGenerationUntilServicedAfterHttp) {
  EXPECT_FALSE(screen.enqueue(Surface::None, 7));
  EXPECT_FALSE(screen.visible());
  ASSERT_TRUE(screen.enqueue(Surface::Home, 7));
  EXPECT_TRUE(screen.busy());
  EXPECT_TRUE(screen.visible());
  auto receipt = screen.receipt();
  EXPECT_EQ(receipt.surface, Surface::Home);
  EXPECT_EQ(receipt.generation, 7U);
  EXPECT_EQ(receipt.phase, PresentationPhase::Queued);
  EXPECT_EQ(receipt.failure, PresentationFailure::None);
  EXPECT_EQ(receipt.heap, 0U);
  EXPECT_EQ(receipt.block, 0U);
  // An incidental paint before preparation neither loads nor draws.
  EXPECT_TRUE(screen.render(renderer));
  EXPECT_EQ(ScreenFake::loads, 0);
  EXPECT_EQ(PresentationFake::loads, 0);
  EXPECT_EQ(ScreenFake::draws, 0);
  EXPECT_EQ(PresentationFake::displays, 0);
  EXPECT_FALSE(screen.enqueue(Surface::Brief, 7));  // busy owns the slot
  ASSERT_TRUE(screen.service(renderer, 16384, 4096));
  EXPECT_EQ(ScreenFake::loads, 1);
  EXPECT_EQ(ScreenFake::loadedSurface, Surface::Home);
  EXPECT_TRUE(screen.busy());
  EXPECT_TRUE(screen.holdsInputs());
  EXPECT_FALSE(screen.service(renderer, 16384, 4096));  // one preparation per request
  receipt = screen.receipt();
  EXPECT_EQ(receipt.heap, 16384U);
  EXPECT_EQ(receipt.block, 4096U);
}

TEST_F(ScreenPresentationTest, LatinFramePaintsInPortraitWithoutFontsAndReleasesInputs) {
  ASSERT_TRUE(screen.enqueue(Surface::Brief, 7));
  ASSERT_TRUE(screen.service(renderer, 20000, 8000));
  EXPECT_EQ(PresentationFake::loads, 0);  // built-in fonts only
  PresentationFake::duringDisplay = [&] {
    EXPECT_TRUE(screen.busy());
    EXPECT_EQ(screen.receipt().phase, PresentationPhase::Queued);
    EXPECT_FALSE(screen.canCaptureFrame());
  };
  EXPECT_TRUE(screen.render(renderer));
  EXPECT_EQ(ScreenFake::draws, 1);
  EXPECT_EQ(ScreenFake::drawnFont, 0);
  EXPECT_EQ(ScreenFake::drawOrientation, 0);    // Portrait while drawing
  EXPECT_EQ(PresentationFake::orientation, 1);  // restored for the transfer view
  EXPECT_EQ(PresentationFake::displays, 1);
  EXPECT_EQ(PresentationFake::releases, 0);
  EXPECT_FALSE(screen.busy());
  EXPECT_FALSE(screen.holdsInputs());
  EXPECT_TRUE(screen.canCaptureFrame());
  EXPECT_EQ(screen.receipt().phase, PresentationPhase::Rendered);
  EXPECT_EQ(screen.receipt().surface, Surface::Brief);
  // RSSI/heartbeat repaints never redraw the presented frame.
  EXPECT_TRUE(screen.render(renderer));
  EXPECT_EQ(ScreenFake::draws, 1);
  EXPECT_EQ(PresentationFake::displays, 1);
}

TEST_F(ScreenPresentationTest, CjkFrameLoadsBoundedFontOnceAndChecksEveryStringInBothStyles) {
  ScreenFake::texts = {"포켓 데일리", "Library", "今日の漢字", "習"};
  ASSERT_TRUE(screen.enqueue(Surface::Home, 7));
  ASSERT_TRUE(screen.service(renderer, 16384, 4096));
  EXPECT_EQ(PresentationFake::loads, 1);  // BoundedUI only (the fake refuses Cached)
  for (const auto* text : {"포켓 데일리", "Library", "今日の漢字", "習", "\xE2\x80\xA6", "..."})
    EXPECT_TRUE(checked(text)) << text;
  PresentationFake::duringDisplay = [&] { EXPECT_EQ(PresentationFake::releases, 0); };
  EXPECT_TRUE(screen.render(renderer));
  EXPECT_EQ(ScreenFake::drawnFont, 1);
  EXPECT_EQ(PresentationFake::displays, 1);
  EXPECT_EQ(PresentationFake::releases, 1);  // released before admitting writers
  EXPECT_FALSE(screen.busy());
  EXPECT_FALSE(screen.holdsInputs());
  EXPECT_EQ(screen.receipt().phase, PresentationPhase::Rendered);
}

TEST_F(ScreenPresentationTest, AdmissionGatesFailOnceAsMemoryWithoutLoadingOrRetrying) {
  for (const auto budget : {std::pair{16383U, 4096U}, std::pair{16384U, 4095U}}) {
    ASSERT_TRUE(screen.enqueue(Surface::Home, 7));
    EXPECT_FALSE(screen.service(renderer, budget.first, budget.second));
    expectFailedAndReleased(screen, PresentationFailure::Memory);
    EXPECT_EQ(screen.receipt().heap, budget.first);
    EXPECT_EQ(screen.receipt().block, budget.second);
    EXPECT_EQ(screen.receipt().surface, Surface::Home);
    EXPECT_EQ(screen.receipt().generation, 7U);
    EXPECT_FALSE(screen.service(renderer, 64000, 32000));
    EXPECT_TRUE(screen.render(renderer));
  }
  EXPECT_EQ(ScreenFake::loads, 0);
  EXPECT_EQ(ScreenFake::draws, 0);
  EXPECT_EQ(PresentationFake::displays, 0);
}

TEST_F(ScreenPresentationTest, ChangedGenerationOrReaderSleepModeFailPreparationBeforeAnyInput) {
  ScreenFake::generation = 8;
  ASSERT_TRUE(screen.enqueue(Surface::Home, 7));
  EXPECT_FALSE(screen.service(renderer, 20000, 8000));
  expectFailedAndReleased(screen, PresentationFailure::Preparation);
  ScreenFake::generation = 7;
  ScreenFake::profile.sleepMode = PocketDaily::DailyProfile::SleepMode::Reader;
  ASSERT_TRUE(screen.enqueue(Surface::Brief, 7));
  EXPECT_FALSE(screen.service(renderer, 20000, 8000));
  expectFailedAndReleased(screen, PresentationFailure::Preparation);
  EXPECT_EQ(ScreenFake::loads, 0);
  // Home still draws with the reader's own sleep screen selected.
  ASSERT_TRUE(screen.enqueue(Surface::Home, 7));
  EXPECT_TRUE(screen.service(renderer, 20000, 8000));
  // Generation zero (no profile saved yet) is a real generation.
  screen.render(renderer);
  ScreenFake::generation = 0;
  ASSERT_TRUE(screen.enqueue(Surface::Home, 0));
  EXPECT_TRUE(screen.service(renderer, 20000, 8000));
}

TEST_F(ScreenPresentationTest, InputLoadFailuresMapToMemoryOrPreparationAndNeverPaint) {
  ScreenFake::loadResult = LoadResult::OutOfMemory;
  ASSERT_TRUE(screen.enqueue(Surface::Home, 7));
  EXPECT_FALSE(screen.service(renderer, 20000, 8000));
  expectFailedAndReleased(screen, PresentationFailure::Memory);
  ScreenFake::loadResult = LoadResult::Unavailable;
  ASSERT_TRUE(screen.enqueue(Surface::Brief, 7));
  EXPECT_FALSE(screen.service(renderer, 20000, 8000));
  expectFailedAndReleased(screen, PresentationFailure::Preparation);
  EXPECT_TRUE(screen.render(renderer));
  EXPECT_EQ(PresentationFake::loads, 0);
  EXPECT_EQ(ScreenFake::draws, 0);
  EXPECT_EQ(PresentationFake::displays, 0);
}

TEST_F(ScreenPresentationTest, FontLoadCoverageAndReadFailuresReleaseEverythingWithoutPainting) {
  ScreenFake::texts = {"포켓 데일리"};
  // Family unavailable: nothing loaded, so nothing to release.
  PresentationFake::fontAvailable = false;
  ASSERT_TRUE(screen.enqueue(Surface::Home, 7));
  EXPECT_FALSE(screen.service(renderer, 20000, 8000));
  expectFailedAndReleased(screen, PresentationFailure::Preparation);
  EXPECT_EQ(PresentationFake::releases, 0);
  // Missing coverage.
  PresentationFake::fontAvailable = true;
  PresentationFake::missingGlyphs = 1;
  ASSERT_TRUE(screen.enqueue(Surface::Home, 7));
  EXPECT_FALSE(screen.service(renderer, 20000, 8000));
  expectFailedAndReleased(screen, PresentationFailure::Preparation);
  EXPECT_EQ(PresentationFake::releases, 1);
  // Latched bounded read failure during the check.
  PresentationFake::missingGlyphs = 0;
  PresentationFake::fontReady = false;
  ASSERT_TRUE(screen.enqueue(Surface::Brief, 7));
  EXPECT_FALSE(screen.service(renderer, 20000, 8000));
  expectFailedAndReleased(screen, PresentationFailure::Preparation);
  EXPECT_EQ(PresentationFake::releases, 2);
  // No implicit retry.
  EXPECT_FALSE(screen.service(renderer, 20000, 8000));
  EXPECT_TRUE(screen.render(renderer));
  EXPECT_EQ(ScreenFake::draws, 0);
  EXPECT_EQ(PresentationFake::displays, 0);
  EXPECT_EQ(PresentationFake::loads, 3);
}

TEST_F(ScreenPresentationTest, PaintFailuresAreDisplayFailuresWithNoDisplayCallOrRetry) {
  ScreenFake::texts = {"포켓 데일리"};
  ASSERT_TRUE(screen.enqueue(Surface::Home, 7));
  ASSERT_TRUE(screen.service(renderer, 20000, 8000));
  ScreenFake::drawSucceeds = false;      // e.g. the header refused an unchecked font
  EXPECT_TRUE(screen.render(renderer));  // keeps the panel, not the transfer view
  expectFailedAndReleased(screen, PresentationFailure::Display);
  EXPECT_EQ(PresentationFake::displays, 0);
  EXPECT_EQ(PresentationFake::releases, 1);
  EXPECT_EQ(PresentationFake::orientation, 1);
  EXPECT_TRUE(screen.render(renderer));
  EXPECT_EQ(ScreenFake::draws, 1);

  // A bounded read failure latched while drawing is not certified either.
  ScreenFake::drawSucceeds = true;
  ASSERT_TRUE(screen.enqueue(Surface::Home, 7));
  ASSERT_TRUE(screen.service(renderer, 20000, 8000));
  ScreenFake::duringDraw = [] { PresentationFake::fontReady = false; };
  EXPECT_TRUE(screen.render(renderer));
  expectFailedAndReleased(screen, PresentationFailure::Display);
  EXPECT_EQ(PresentationFake::displays, 0);
  EXPECT_EQ(PresentationFake::releases, 2);
}

TEST_F(ScreenPresentationTest, HideCancelsPendingOrQueuedWorkAndClearsTheReceipt) {
  ASSERT_TRUE(screen.enqueue(Surface::Home, 7));
  screen.hide(renderer);
  EXPECT_FALSE(screen.service(renderer, 20000, 8000));
  EXPECT_FALSE(screen.visible());
  EXPECT_FALSE(screen.busy());
  EXPECT_EQ(screen.receipt().surface, Surface::None);
  EXPECT_EQ(ScreenFake::loads, 0);
  ScreenFake::texts = {"포켓 데일리"};
  ASSERT_TRUE(screen.enqueue(Surface::Brief, 7));
  ASSERT_TRUE(screen.service(renderer, 20000, 8000));
  screen.hide(renderer);  // queued for the render task: fonts and inputs released
  EXPECT_FALSE(screen.holdsInputs());
  EXPECT_EQ(PresentationFake::releases, 1);
  EXPECT_FALSE(screen.render(renderer));
  EXPECT_EQ(ScreenFake::draws, 0);
  EXPECT_EQ(screen.receipt().phase, PresentationPhase::Idle);
}

class PresentationSlotTest : public ScreenPresentationTest {
 protected:
  const char* revision = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  PocketDaily::PresentationSlot slot;
};

TEST_F(PresentationSlotTest, ScreenRequestReplacesARenderedCardPageAndItsReceipt) {
  ASSERT_TRUE(slot.enqueueContent(revision, 7, renderer));
  ASSERT_TRUE(slot.service(renderer, 20000, 8000));
  ASSERT_TRUE(slot.render(renderer, input));
  ASSERT_EQ(slot.content.receipt().phase, PresentationPhase::Rendered);
  EXPECT_TRUE(slot.navigationAvailable());

  ASSERT_TRUE(slot.enqueueScreen(Surface::Home, 7, renderer));
  EXPECT_FALSE(slot.content.visible());
  EXPECT_EQ(slot.content.receipt().generation, 0U);  // content GET now 409
  EXPECT_FALSE(slot.navigationAvailable());
  EXPECT_TRUE(slot.busy());
  EXPECT_TRUE(slot.preparationPending());
  ASSERT_TRUE(slot.service(renderer, 20000, 8000));
  EXPECT_EQ(ScreenFake::loads, 1);
  EXPECT_EQ(PresentationFake::loads, 1);  // content's own font load only
  EXPECT_TRUE(slot.render(renderer, input));
  EXPECT_EQ(ScreenFake::draws, 1);
  EXPECT_EQ(PresentationFake::draws, 1);  // no card page redraw
  EXPECT_TRUE(slot.canCaptureFrame());
  EXPECT_EQ(slot.screen.receipt().phase, PresentationPhase::Rendered);
}

TEST_F(PresentationSlotTest, CardRequestReplacesARenderedScreen) {
  ASSERT_TRUE(slot.enqueueScreen(Surface::Brief, 7, renderer));
  ASSERT_TRUE(slot.service(renderer, 20000, 8000));
  ASSERT_TRUE(slot.render(renderer, input));
  ASSERT_TRUE(slot.enqueueContent(revision, 7, renderer));
  EXPECT_FALSE(slot.screen.visible());
  EXPECT_EQ(slot.screen.receipt().surface, Surface::None);  // screen GET now 409
  ASSERT_TRUE(slot.service(renderer, 20000, 8000));
  EXPECT_TRUE(slot.render(renderer, input));
  EXPECT_EQ(PresentationFake::draws, 1);
  EXPECT_EQ(ScreenFake::draws, 1);
  EXPECT_TRUE(slot.navigationAvailable());
}

TEST_F(PresentationSlotTest, EitherKindQueuedOrDrawingRefusesTheOther) {
  ASSERT_TRUE(slot.enqueueContent(revision, 7, renderer));
  EXPECT_FALSE(slot.enqueueScreen(Surface::Home, 7, renderer));
  EXPECT_EQ(slot.content.receipt().phase, PresentationPhase::Queued);
  ASSERT_TRUE(slot.service(renderer, 20000, 8000));  // prepared, awaiting the render task
  EXPECT_FALSE(slot.enqueueScreen(Surface::Home, 7, renderer));
  slot.render(renderer, input);
  ASSERT_TRUE(slot.enqueueScreen(Surface::Home, 7, renderer));
  EXPECT_FALSE(slot.enqueueContent(revision, 7, renderer));
  ASSERT_TRUE(slot.service(renderer, 20000, 8000));
  EXPECT_FALSE(slot.enqueueContent(revision, 7, renderer));
  EXPECT_EQ(slot.screen.receipt().surface, Surface::Home);
}

TEST_F(PresentationSlotTest, BackDismissesEitherKindAndCardNavigationNeedsAVisibleCardPage) {
  PocketDaily::Content::ContentSessionInput session;
  using Action = PocketDaily::Content::ContentSessionInput::Action;
  ASSERT_TRUE(slot.enqueueScreen(Surface::Home, 7, renderer));
  ASSERT_TRUE(slot.service(renderer, 20000, 8000));
  slot.render(renderer, input);
  // Page buttons do nothing on a presented Home frame.
  EXPECT_EQ(session.update(slot.visible(), slot.busy(), slot.navigationAvailable(), false, true, false), Action::None);
  EXPECT_EQ(session.update(slot.visible(), slot.busy(), slot.navigationAvailable(), true, false, false),
            Action::DismissView);
  slot.hide(renderer);
  EXPECT_FALSE(slot.visible());
  EXPECT_EQ(session.update(slot.visible(), slot.busy(), slot.navigationAvailable(), true, false, false),
            Action::ExitSession);

  // Back during a paint is latched and dismisses once the paint finishes.
  ASSERT_TRUE(slot.enqueueScreen(Surface::Brief, 7, renderer));
  ASSERT_TRUE(slot.service(renderer, 20000, 8000));
  EXPECT_EQ(session.update(slot.visible(), slot.busy(), slot.navigationAvailable(), true, false, false), Action::None);
  slot.render(renderer, input);
  EXPECT_EQ(session.update(slot.visible(), slot.busy(), slot.navigationAvailable(), false, false, false),
            Action::DismissView);
  slot.hide(renderer);

  ASSERT_TRUE(slot.enqueueContent(revision, 7, renderer));
  ASSERT_TRUE(slot.service(renderer, 20000, 8000));
  slot.render(renderer, input);
  EXPECT_EQ(session.update(slot.visible(), slot.busy(), slot.navigationAvailable(), false, true, false), Action::Next);
}

TEST_F(PresentationSlotTest, HideReleasesBothKindsAndCaptureFollowsTheVisibleKind) {
  EXPECT_FALSE(slot.canCaptureFrame());
  ScreenFake::texts = {"포켓 데일리"};
  ASSERT_TRUE(slot.enqueueScreen(Surface::Home, 7, renderer));
  ASSERT_TRUE(slot.service(renderer, 20000, 8000));
  EXPECT_FALSE(slot.canCaptureFrame());
  slot.hide(renderer);  // activity exit while queued
  EXPECT_FALSE(slot.busy());
  EXPECT_FALSE(slot.visible());
  EXPECT_FALSE(slot.screen.holdsInputs());
  EXPECT_EQ(PresentationFake::releases, 1);
  EXPECT_FALSE(slot.render(renderer, input));
  EXPECT_FALSE(slot.canCaptureFrame());

  // A failed screen paint never exposes a capturable frame.
  ScreenFake::drawSucceeds = false;
  ASSERT_TRUE(slot.enqueueScreen(Surface::Home, 7, renderer));
  ASSERT_TRUE(slot.service(renderer, 20000, 8000));
  EXPECT_TRUE(slot.render(renderer, input));
  EXPECT_TRUE(slot.visible());
  EXPECT_FALSE(slot.canCaptureFrame());
  EXPECT_EQ(PresentationFake::displays, 0);
}
