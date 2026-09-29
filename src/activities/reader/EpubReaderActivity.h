#pragma once

#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/PageLink.h>
#include <Epub/Section.h>

#include <atomic>
#include <memory>
#include <optional>
#include <vector>

#include "BookmarkEntry.h"
#include "ChapterPosition.h"
#include "EpubReaderMenuActivity.h"
#include "ProgressMapper.h"
#include "ReaderActivity.h"
#include "ReaderLayoutAhead.h"
#include "ReaderToolbarUi.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"

class EpubReaderActivity final : public ReaderActivity {
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
  std::string pendingAnchor;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  std::optional<uint32_t> cachedVisibleTextOffset;
  std::optional<uint32_t> currentPageVisibleOffset;
  std::optional<uint32_t> pendingOffsetJump;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  int8_t pendingManualTurn = 0;
  bool pendingPercentJump = false;
  float pendingSpineProgress = 0.0f;
  bool pendingScreenshot = false;
  bool articleEndHandled = false;
  bool articleReadSaveFailed = false;
  bool pendingSyncSaveError = false;
  uint8_t pageLoadRetryCount = 0;
  static constexpr uint8_t MAX_PAGE_LOAD_RETRIES = 3;
  bool skipNextButtonCheck = false;
  bool automaticPageTurnActive = false;
  bool showBookmarkMessage = false;
  bool showDictionaryMessage = false;
  unsigned long dictionaryMessageTime = 0UL;
  bool currentPageBookmarked = false;
  bool bookmarkRemoved = false;  // true when last toggle removed (controls popup text)
  // Bilingual view mode popup feedback. Set when the user cycles the mode with the
  // front Left button (or Long-press Confirm → Bilingual Toggle). Auto-clears after
  // BOOKMARK_MESSAGE_DURATION_MS so the same transient-popup pattern as bookmark
  // feedback is reused.
  bool showBilingualMessage = false;
  unsigned long bilingualMessageTime = 0UL;
  int idlePrewarmSpine = -1;
  int idlePrewarmPage = -1;
  unsigned long lastRenderCompleteMs = 0;
  std::vector<BookmarkEntry> cachedBookmarks;
  bool recentsEntryRemoved = false;
  unsigned long bookmarkMessageTime = 0UL;
  bool pendingReadFolderMove = false;
  // Pocket Daily reading-progress v1: another device left a position to ask about.
  bool readingOfferPending = false;
  void askReadingOffer();
  // An XPointer jump's chapter text offset (ProgressMapper CrossPointPosition::textOffset),
  // resolved to the page holding that character once its section is laid out far enough.
  struct PendingTextOffset {
    int spineIndex;
    uint32_t textOffset;
  };
  std::optional<PendingTextOffset> pendingTextOffset;
  void setPendingTextOffset(const CrossPointPosition& target);
  // False only when a build step failed (the caller shows the build error).
  bool resolvePendingTextOffset(const SectionLayout& layout);

  // Toolbar reader menu (SETTINGS.readerMenuStyle == READER_MENU_TOOLBAR): drawn
  // over the page instead of pushing the full-screen list menu. Select opens the
  // Toolbar; its tools open the Contents/Text/More bottom-sheet panels.
  enum class Overlay { None, Toolbar, Contents, Text, More };
  Overlay overlay = Overlay::None;
  int focusedTool = 0;  // toolbar tool focus: 0=Contents, 1=Text, 2=More
  int panelIndex = 0;   // selected row within the active panel
  // Panel list navigation: a tap steps one row, a hold jumps PANEL_HOLD_STEP rows in one go
  // (a contents list runs to hundreds of chapters). One jump per hold, not a repeat -- every
  // step repaints the panel, so repeating is bounded by the e-ink refresh anyway and reads as
  // sluggish. True once a hold has jumped, so the release that ends it is swallowed.
  static constexpr unsigned long PANEL_HOLD_MS = 1500;
  static constexpr int PANEL_HOLD_STEP = 10;
  bool panelHoldJumped = false;
  // Whether the panel draws its cursor row. Button boards always do; touch
  // boards only once a button has moved it, so a tapped row is not left inverted.
  bool panelCursorShown = false;
  // FreeInkUI chrome + tap targets for the overlay; created when it opens,
  // released when it closes.
  std::unique_ptr<ReaderToolbarUi> toolbarUi;
  // Modal option picker over the panel (same component the Settings screens
  // use), for enum rows: font size / line spacing / alignment / orientation /
  // auto page turn. Toggle rows stay one-tap toggles, as in Settings.
  OptionPopup overlayPopup;
  // True while a clean-page snapshot (renderer.storeBwBuffer) backs the open
  // overlay, letting panel->toolbar steps restore the page without a full
  // re-render. Discarded on close / whenever the page under the overlay changes.
  bool overlayPageStored = false;
  // True while a deferred overlay chrome refresh (pushOverlayRefresh) may still
  // be running on the panel. settleOverlayRefresh() must run before the
  // framebuffer is touched or another differential refresh is pushed.
  bool overlayRefreshPending = false;
  void pushOverlayRefresh();
  void settleOverlayRefresh();
  int autoTurnOption = 0;  // current auto page-turn rate index (More panel)
  std::vector<EpubReaderMenuActivity::MenuItem> moreItems;

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  std::vector<PageLink> currentPageLinks;
  int currentPageLinkMarginLeft = 0;
  int currentPageLinkMarginTop = 0;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  // Last position persisted by render()'s saveProgress, used to skip redundant
  // writeAtomic calls on no-op re-renders (menu/bookmark/screenshot).
  uint16_t buildViewportWidth = 0;
  uint16_t buildViewportHeight = 0;
  bool partialRebuildStartFailed = false;

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
  std::optional<uint32_t> pendingSaveVisibleOffset;
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
  static constexpr size_t BACKGROUND_BUILD_MIN_FREE_HEAP = 32 * 1024;
  static constexpr size_t BACKGROUND_BUILD_MIN_MAX_ALLOC = 16 * 1024;
  // Requires the render lock; heap admission is checked separately by the build tick.
  bool backgroundBuildWanted() const;
  bool buildTickHeapGate();
  bool buildHeapPaused = false;
  static constexpr size_t RENDER_MIN_FREE_HEAP = 24 * 1024;
  static constexpr int PARTIAL_REBUILD_START_MARGIN = 15;
  static constexpr unsigned long BUILD_POPUP_DEADLINE_MS = 1000;
  bool buildPopupPending = false;
  void showBuildPopup(GfxRenderer& renderer, int& pagesUntilFullRefresh);
  void clearDeferredReposition();
  void rememberCurrentContentOffset();
  bool saveProgress(int spineIndex, int currentPage, int pageCount);
  void jumpToPercent(int percent);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  // Live section position, or the values cached before a child screen
  // released the section.
  ChapterPosition chapterPosition() const;
  int bookPercentFor(const ChapterPosition& position) const;
  void openReaderMenu();
  // Toolbar reader menu (see Overlay above).
  bool usesToolbarMenu() const;
  void openOverlay(Overlay target);
  void closeOverlayToPage();
  void discardOverlayPage();
  void handleOverlayInput();
  void renderOverlay();
  std::string currentChapterTitle() const;
  // Text panel rows (font, size, line spacing, alignment, focus reading).
  std::string textRowName(int row) const;
  std::string textRowValue(int row) const;
  void showTextRowPopup(int row);
  // Persist + re-paginate + re-render under the open panel (live preview).
  void applyTextSettingLive();
  void paintOverlayPopup();
  // Persist the reader text settings, (re)load the selected SD font, and
  // re-paginate the current chapter so changes apply without re-opening the book.
  void applyReaderTextSettings();
  // More panel rows.
  void buildMoreActions();
  std::string moreRowName(int row) const;
  std::string moreRowValue(int row) const;
  void activateMoreRow(int row);
  void openFootnoteSelect(bool reopenMenuOnCancel);
  void openDictionaryWordSelect();
  bool launchKOReaderSync();
  unsigned long confirmLongPressThreshold() const;
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
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

  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

  void applyOrientation(uint8_t orientation);
  void applyInitialOrientation() override;
  // The orientation the current layout was built for. The control center's
  // orientation tile can move SETTINGS.orientation while this reader sits on
  // the activity stack, and Pop restores it without onEnter(), so the drift has
  // to be noticed here rather than assumed away.
  uint8_t appliedOrientation = 0;

  bool loadBook() override;
  std::string getBookTitle() const override { return epub ? epub->getTitle() : ""; }
  std::string getBookAuthor() const override { return epub ? epub->getAuthor() : ""; }
  std::string getBookThumbBmpPath() const override { return epub ? epub->getThumbBmpPath() : ""; }
  void renderBook() override;
  void onEndOfBookRendered() override;

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                              bool allowFastInitialRefresh)
      : ReaderActivity("EpubReader", renderer, mappedInput, std::move(bookPath), allowFastInitialRefresh) {}
  ~EpubReaderActivity() override;

  void loop() override;

  bool pageTurn(bool isForward) override;
  bool skipPages(int amount) override;
  bool isAtEndOfBook() const override;
  void onReturnFromEndOfBook() override;

  bool skipLoopDelay() override;

  ScreenshotInfo getScreenshotInfo() const override;
  CrossPointPosition getCurrentPosition() const;
};
