#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>

#include <atomic>
#include <optional>

#include "BookmarkEntry.h"
#include "EpubReaderMenuActivity.h"
#include "ProgressMapper.h"
#include "ReaderLayoutAhead.h"
#include "activities/Activity.h"

class EpubReaderActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  int activeReaderFontId = 0;
  // Consecutive page-load failures in render(). A failed load clears the section cache and
  // requestUpdate()s to re-parse and retry; this counter bounds that so a perpetually failing
  // page can't re-enter render() forever and trip the watchdog. Reset on a successful load.
  uint8_t pageLoadRetries = 0;
  std::optional<uint16_t> pendingPageJump;
  // Set when navigating to a footnote href with a fragment (e.g. #note1).
  // Cleared on the next render after the new section loads and resolves it to a page.
  std::string pendingAnchor;
  int pagesUntilFullRefresh = 0;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  // Signals that the next render should reposition within the newly loaded section
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  bool pendingScreenshot = false;
  bool articleEndHandled = false;
  bool articleReadSaveFailed = false;
  bool pendingSyncSaveError = false;
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  bool automaticPageTurnActive = false;
  bool showBookmarkMessage = false;
  bool ignoreNextConfirmRelease = false;
  bool currentPageBookmarked = false;
  bool bookmarkRemoved = false;  // true when last toggle removed (controls popup text)
  // Bilingual view mode popup feedback. Set when the user cycles the mode with the
  // front Left button (or Long-press Confirm → Bilingual Toggle). Auto-clears after
  // BOOKMARK_MESSAGE_DURATION_MS so the same transient-popup pattern as bookmark
  // feedback is reused.
  bool showBilingualMessage = false;
  unsigned long bilingualMessageTime = 0UL;
  std::vector<BookmarkEntry> cachedBookmarks;
  // Tracks whether this book is currently removed from Recent Books by the
  // removeReadBooksFromRecents feature (set at End-of-Book, cleared if paged back in).
  bool recentsEntryRemoved = false;
  unsigned long bookmarkMessageTime = 0UL;
  // Set when the reader is left at end-of-book and SETTINGS.moveFinishedToReadFolder is on.
  // Consumed in onExit() to relocate the finished book into /Read/.
  bool pendingReadFolderMove = false;
  // Pocket Daily reading-progress v1: another device left a position to ask about.
  bool readingOfferPending = false;
  void askReadingOffer();

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  // Last position persisted by render()'s saveProgress, used to skip redundant
  // writeAtomic calls on no-op re-renders (menu/bookmark/screenshot).
  int lastSavedSpineIndex = -1;
  int lastSavedPage = -1;
  int lastSavedPageCount = -1;

  SectionLayout layoutParams(uint16_t viewportWidth, uint16_t viewportHeight) const;
  // Params of the section on screen (valid once a section was opened).
  SectionLayout shownLayout;
  bool shownLayoutValid = false;
  unsigned long lastRenderDoneMs = 0UL;

  // Layout ahead of the reader (ReaderLayoutAhead): after each page (AA included) the render
  // task lays out the rest of the chapter, then the next one, yielding to any button.
  std::unique_ptr<ReaderLayoutAhead> layoutAhead;
  std::atomic<bool> inputQueued{false};
  // A backward turn into a chapter that is not laid out must lay it out to its last page
  // (seconds for a 250 KB spine on the X3), behind the "Indexing" popup. Presses meanwhile
  // do not queue behind it: forward cancels it (back to the page the reader came from, the
  // pages already laid out are kept as a partial), each further back press lands one page
  // earlier. Set by render() while that build runs.
  static constexpr uint16_t LANDING_LAST_PAGE = UINT16_MAX;  // pendingPageJump: the chapter's last page
  std::atomic<bool> landingInProgress{false};
  std::atomic<bool> landingCancelled{false};
  std::atomic<int> landingExtraBack{0};
  void layOutAhead();

  // Reading-position writes are deferred to idle time (loop()) instead of every render:
  // a turn no longer waits on the SD create/remove/rename, and rapid turns write once.
  static constexpr unsigned long PROGRESS_SAVE_IDLE_MS = 1500;
  bool progressSavePending = false;
  int pendingSaveSpine = 0;
  int pendingSavePage = 0;
  int pendingSavePageCount = 0;
  int pendingSaveBookPercent = -1;
  // `fromLoop`: runs on the main task while the render task may start a turn, so the
  // pending position is taken under a brief RenderLock and the SD write happens outside
  // it (a turn then only waits for individual SD operations, never the whole write). The
  // onExit path already holds the RenderLock.
  void flushPendingProgress(bool fromLoop);

  // Page-turn telemetry (PocketDaily::ReaderPerf): the next render() records a turn that
  // started when loop() handled the button (perfInputMs, 0 for the first page).
  bool perfTurnPending = false;
  // Set by pageTurn() (main task), read by the render task to cut a page's anti-aliasing
  // short when the reader has already moved on (ReaderPageRenderer::Options::nextTurnQueued).
  std::atomic<bool> turnQueued{false};
  uint8_t perfTurnFlags = 0;
  uint32_t perfInputMs = 0;
  void queuePerfTurn(uint8_t flags, uint32_t inputMs);

  // Per-spine values the status bar and progress save need on every turn, read once per
  // chapter instead of re-reading book.bin spine/TOC entries (4-6 SD seeks+reads a turn).
  struct SpineInfo {
    int spineIndex = -1;
    size_t bookSize = 0;
    size_t previousCumulative = 0;
    size_t cumulative = 0;
    std::string tocTitle;  // empty when the spine has no TOC entry
    bool hasTocEntry = false;
    // Same arithmetic as Epub::calculateProgress.
    float bookProgress(float spineRead) const;
  };
  SpineInfo spineInfo;
  const SpineInfo& currentSpineInfo();
  // Chapter-title text the status bar draws, and whether that happens with the loaded SD
  // font (CJK titles, see UiCjkFont): such text is prewarmed with the page's glyphs.
  std::string statusBarTitle();

  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void renderStatusBar();
  // Pages laid out per incremental-build pump: on the render path (catching up to the page
  // being shown) and per loop() tick (background build of a large chapter). Kept small so a
  // background build chunk never noticeably delays input or a pending render. The render
  // path pumps until the page it shows exists, so its chunk is one page: laying out 8 before
  // showing page 0 put up to ~2.9 s of layout on the X3 turn into an unbuilt chapter
  // (readerPerf 2026-09-28); loop() lays out the rest behind it.
  static constexpr int BUILD_PAGES_PER_CHUNK = 1;
  static constexpr int BACKGROUND_BUILD_PAGES_PER_TICK = 2;
  // How many pages to keep laid out ahead of the reader for a still-building section. A page
  // turn is ~1s on e-ink and a page builds in ~30ms, so the reader can't out-click the builder
  // -- a tiny buffer is enough. The background build stops once the watermark is this far
  // ahead and resumes as the reader advances; building unbounded instead locked up input by
  // monopolizing the RenderLock. A giant single-spine book therefore never finalizes its .bin
  // in one sitting -- instant reopen comes from Section::suspendBuild() persisting the pages
  // already laid out as a partial file on exit/sleep.
  static constexpr int BUILD_WINDOW_AHEAD = 5;
  // An in-progress build keeps its expat parser, CSS parser, and page table alive across
  // renders; that state used to be freed before any page was drawn. When the largest free
  // block drops below this after a background tick, the build is suspended (persisted as a
  // partial, parser freed) so the next grayscale render or progress save cannot hit a bare
  // `new` on an exhausted heap, which aborts the device (X3 crash report 2026-09-06: abort()
  // right after a 4 s tiled render that followed a full-chapter re-parse). Forward turns past
  // the watermark restart the build from the partial.
  static constexpr size_t BUILD_MIN_FREE_BLOCK = 12 * 1024;
  // Show the indexing popup when an initial build must lay out more than this many pages up front
  // (a deep resume/jump into a not-yet-built section), so it isn't a silent wait. Kept independent
  // of the small look-ahead window so ordinary landings stay popup-free.
  static constexpr int BUILD_POPUP_PAGE_THRESHOLD = 20;
  // Also show the popup when first building a spine larger than this (uncompressed bytes): its
  // whole HTML must be inflated before page 1 can lay out (the giant single-spine case), which is
  // a multi-second wait. Normal chapters are well under this and stay popup-free.
  static constexpr size_t BUILD_POPUP_BYTE_THRESHOLD = 96 * 1024;
  // Remap the cached relative reading position once the section's real page count is known
  // (used after a settings change re-paginates a chapter). Returns true if currentPage moved.
  // No-op while the section is still building or when the pagination is unchanged (plain resume).
  bool applyDeferredReposition();
  bool saveProgress(int spineIndex, int currentPage, int pageCount);
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  // Returns true if sync acted (launched, or surfaced a save error); false if it was a no-op
  // because no KOReader credentials are stored.
  bool launchKOReaderSync();
  void applyOrientation(uint8_t orientation);
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  void pageTurn(bool isForwardTurn);
  // Stores the current section's failed build step and heap state (BuildFailureLog).
  void recordBuildFailure() const;
  // Cycle bilingual view mode (Both → Original → Translation → Both), persist the new
  // value, drop the current section so it re-parses on next render, and surface a
  // transient popup naming the new mode. No-op on EPUBs without cp-original /
  // cp-translation markers (those just keep rendering everything regardless of mode).
  void cycleBilingualMode();
  void loadCachedBookmarks();
  void addBookmark();
  void updateBookmarkFlag();

  // Footnote navigation
  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub)
      : Activity("EpubReader", renderer, mappedInput), epub(std::move(epub)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool isReaderActivity() const override { return true; }
  ScreenshotInfo getScreenshotInfo() const override;
  CrossPointPosition getCurrentPosition() const;
};
