#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <Memory.h>
#include <ZipFile.h>
#include <esp_system.h>

#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>

#include "BookmarkEntry.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderBookmarksActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "ProgressMapper.h"
#include "QrDisplayActivity.h"
#include "ReaderPageRenderer.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/util/ConfirmationActivity.h"
#include "articles/ArticleStorage.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "pocket_daily/BuildFailureLog.h"
#include "pocket_daily/ReaderPerf.h"
#include "pocket_daily/ReadingProgressReader.h"
#include "util/BookmarkUtil.h"
#include "util/CjkScript.h"
#include "util/ScreenshotUtil.h"

namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
// pages per minute, first item is 1 to prevent division by zero if accessed
constexpr int PAGE_TURN_RATES[] = {1, 1, 3, 6, 12};
constexpr size_t initialBookmarkCacheCapacity = 16;
constexpr float bookmarkProgressEpsilon = 0.0001f;

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

// SD card folder finished books are moved into. Single source of truth for the path.
// constexpr ⇒ lives in flash .rodata, no DRAM cost.
constexpr char READ_FOLDER[] = "/read";

// True if path is inside READ_FOLDER (starts with "<READ_FOLDER>/"). Non-allocating so
// it is cheap to call from loop(), and avoids reintroducing a separate "/Read/" literal.
bool isInReadFolder(const std::string& path) {
  constexpr size_t n = sizeof(READ_FOLDER) - 1;  // length of "/Read" (excludes NUL)
  return path.size() > n && path.compare(0, n, READ_FOLDER) == 0 && path[n] == '/';
}

struct ProgressRange {
  float start;
  float end;
};

ProgressRange getPageProgressRange(const std::shared_ptr<Epub>& epub, const int spineIndex, const int page,
                                   const int pageCount) {
  if (pageCount <= 1) {
    return {epub->calculateProgress(spineIndex, 0.0f), epub->calculateProgress(spineIndex, 1.0f)};
  }

  const float step = 1.0f / static_cast<float>(pageCount - 1);
  const float anchor = std::clamp(static_cast<float>(page) * step, 0.0f, 1.0f);
  const float start = std::max(0.0f, anchor - (step * 0.5f));
  const float end = std::min(1.0f, anchor + (step * 0.5f));
  return {epub->calculateProgress(spineIndex, start), epub->calculateProgress(spineIndex, end)};
}

bool bookmarkMatchesProgress(const BookmarkEntry& bookmark, const int spineIndex, const int page, const int pageCount,
                             const ProgressRange& pageRange) {
  if (bookmark.computedSpineIndex == spineIndex && bookmark.computedChapterPageCount == pageCount &&
      bookmark.computedChapterProgress == page) {
    return true;
  }

  const float bookmarkProgress = std::clamp(bookmark.percentage, 0.0f, 1.0f);
  return bookmarkProgress + bookmarkProgressEpsilon >= pageRange.start &&
         bookmarkProgress - bookmarkProgressEpsilon <= pageRange.end;
}

// Pick a non-colliding destination path inside /Read/ for a finished book.
// Mirrors the suffixing scheme used elsewhere: "name.epub" -> "name (2).epub", etc.
std::string buildReadFolderDestination(const std::string& srcPath) {
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;

  Storage.mkdir(READ_FOLDER);
  std::string dstPath = std::string(READ_FOLDER) + "/" + filename;
  if (!Storage.exists(dstPath.c_str())) {
    return dstPath;
  }

  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = std::string(READ_FOLDER) + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  return dstPath;
}

// Relocate a finished book and its cache dir into /read/, keep it in recents by
// repointing its entry to the new path, and repoint the resume pointer too.
// On rename failure: LOG_ERR and leave everything in place (no UI alert subsystem here).
void moveFinishedBookToReadFolder(const std::string& srcPath, const std::string& dstPath,
                                  const std::string& oldCachePath) {
  LOG_INF("ERS", "Moving finished epub: %s -> %s", srcPath.c_str(), dstPath.c_str());
  if (!Storage.rename(srcPath.c_str(), dstPath.c_str())) {
    LOG_ERR("ERS", "Failed to move finished book to '/Read' folder");
    return;
  }

  // Cache dir is keyed by hash of the epub path (see Epub ctor), so it must be re-keyed.
  const std::string newCachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(dstPath));
  if (!oldCachePath.empty() && Storage.exists(oldCachePath.c_str())) {
    if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      LOG_ERR("ERS", "Failed to rename cache dir %s -> %s (non-fatal)", oldCachePath.c_str(), newCachePath.c_str());
    }
  }

  // Keep the book in recents (crossink behavior): repoint the entry to its new
  // location instead of dropping it. updatePath persists on success.
  RECENT_BOOKS.updatePath(srcPath, dstPath, oldCachePath, newCachePath);
  if (APP_STATE.openEpubPath == srcPath) {
    APP_STATE.openEpubPath = dstPath;
    APP_STATE.saveToFile();
  }
}

}  // namespace

void EpubReaderActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    return;
  }

  // Configure screen orientation based on settings
  // NOTE: This affects layout math and must be applied before any render calls.
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  epub->setupCacheDir();

  HalFile f;
  if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    int dataSize = f.read(data, 6);
    if (dataSize == 4 || dataSize == 6) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      if (nextPageNumber == UINT16_MAX) {
        // UINT16_MAX is an in-memory navigation sentinel for "open previous
        // chapter on its last page". It should never be treated as persisted
        // resume state after sleep or reopen.
        LOG_DBG("ERS", "Ignoring stale last-page sentinel from progress cache");
        nextPageNumber = 0;
      }
      cachedSpineIndex = currentSpineIndex;
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
    }
  }
  // We may want a better condition to detect if we are opening for the first time.
  // This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
  readingOfferPending = PocketDaily::ReadingProgress::hasPendingOffer(*epub);

  loadCachedBookmarks();

  layoutAhead = makeUniqueNoThrow<ReaderLayoutAhead>(epub, renderer);  // null: no layout ahead (OOM)
  PocketDaily::ReaderPerf::open();
  queuePerfTurn(PocketDaily::ReaderPerf::FLAG_OPEN, 0);

  // Trigger first update
  requestUpdate();
}

void EpubReaderActivity::queuePerfTurn(const uint8_t flags, const uint32_t inputMs) {
  perfTurnPending = true;
  perfTurnFlags = flags;
  perfInputMs = inputMs;
}

void EpubReaderActivity::onExit() {
  Activity::onExit();

  flushPendingProgress(/*fromLoop=*/false);
  layoutAhead.reset();
  // Page-turn timings survive the session in one small file (docs/reader-perf.md).
  PocketDaily::ReaderPerf::close();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();

  // Leaving mid-footnote loses the in-RAM return stack on deep sleep; persist the
  // pre-footnote position so the book reopens at the link origin, not the footnote.
  if (footnoteDepth > 0 && epub) {
    const SavedPosition& origin = savedPositions[0];
    saveProgress(origin.spineIndex, origin.pageNumber, 0);
  }

  // Pocket Daily reading-progress v1: capture the page's paragraph while the section is
  // loaded, release it, then record the position (and XPointer) for the companion app.
  const auto readingPosition =
      footnoteDepth > 0 ? PocketDaily::ReadingProgress::capturePosition(nullptr, savedPositions[0].spineIndex,
                                                                        savedPositions[0].pageNumber, 0)
                        : PocketDaily::ReadingProgress::capturePosition(
                              section.get(), currentSpineIndex, section ? section->currentPage : nextPageNumber,
                              section ? section->estimatedTotalPages() : cachedChapterTotalPageCount);
  section.reset();
  if (epub) PocketDaily::ReadingProgress::recordPosition(epub, readingPosition);
  if (pendingReadFolderMove && epub) {
    const std::string srcPath = epub->getPath();
    const std::string oldCachePath = epub->getCachePath();
    const std::string dstPath = buildReadFolderDestination(srcPath);
    epub.reset();  // release the Epub (and any open handles) before renaming on the SD card
    moveFinishedBookToReadFolder(srcPath, dstPath, oldCachePath);
  } else {
    epub.reset();
  }
}

void EpubReaderActivity::loop() {
  if (!epub) {
    // Should never happen
    finish();
    return;
  }

  // Any button makes layout running on the render task after a page (layOutAhead) yield.
  if (mappedInput.wasAnyPressed() || mappedInput.wasAnyReleased()) inputQueued.store(true);

  // Drive any in-progress incremental section build forward, off the page-turn critical path,
  // but only within a small window ahead of the reader: an unbounded build monopolized the
  // RenderLock and locked out page turns. The build follows the reader instead, and instant
  // reopen comes from suspendBuild() persisting the laid-out pages as a partial on exit.
  // Skip while the render mutex is busy so we never delay a pending render; re-check
  // isBuilding() under the lock since render() may have just finished it.
  // A rebuild over a loaded partial re-parses from the top while pageCount stays pinned at the
  // partial's watermark, so the window test alone would not start it until the reader was
  // within BUILD_WINDOW_AHEAD of the watermark and would then have to lay out the whole prefix
  // synchronously on the next turn (several seconds of no response on a 60-page prefix). Keep
  // ticking until the rebuild has passed the watermark, then fall back to the window.
  if (section && section->isBuilding() && !RenderLock::peek() && !turnQueued.load() &&
      (section->isCatchingUp() || static_cast<int>(section->pageCount) < section->currentPage + BUILD_WINDOW_AHEAD)) {
    RenderLock lock;
    // Re-check under the lock: render() (which also holds the RenderLock) may have finalized the
    // build between the outer isBuilding() check and acquiring the lock here, in which case
    // buildSomeMore() would fail and wrongly reset the section. cppcheck can't see the cross-task
    // mutation, so it flags this as always true.
    // cppcheck-suppress knownConditionTrueFalse
    if (section->isBuilding()) {
      if (!section->buildSomeMore(BACKGROUND_BUILD_PAGES_PER_TICK)) {
        LOG_ERR("ERS", "Background section build failed");
        recordBuildFailure();
        // Keep the reader where it is: render() reloads at nextPageNumber, which still holds
        // the page the section was opened on, not the page being read.
        nextPageNumber = section->currentPage;
        section.reset();
        requestUpdate();
      } else if (section->isBuildComplete() && applyDeferredReposition()) {
        // The chapter re-paginated since the saved progress (settings changed): we now know the
        // real page count, so re-render at the remapped page. No-op for an unchanged resume.
        requestUpdate();
      } else if (section->isBuilding() && section->pageCount > 0 && ESP.getMaxAllocHeap() < BUILD_MIN_FREE_BLOCK) {
        // Heap floor: persist what is built and free the parser before a render can abort.
        // Guarded on pageCount > 0 so a suspend can never drop the section below the page
        // being read (suspendBuild keeps the larger of this build and any loaded partial).
        LOG_INF("ERS", "Suspending section build: largest free block %u B", (unsigned)ESP.getMaxAllocHeap());
        section->suspendBuild();
      }
    }
  }

  // Idle work, in order of importance: the reading position, then the next chapter. Never
  // in a frame that carries button input, so it cannot delay the turn that input triggers.
  if (!mappedInput.wasAnyPressed() && !mappedInput.wasAnyReleased()) {
    if (progressSavePending && millis() - lastRenderDoneMs >= PROGRESS_SAVE_IDLE_MS && !RenderLock::peek()) {
      flushPendingProgress(/*fromLoop=*/true);
    }
    // Page-turn timings now and then, not on every turn (onExit saves the rest).
    if (PocketDaily::ReaderPerf::unsavedTurns() >= PocketDaily::ReaderPerf::SAVE_EVERY_TURNS &&
        millis() - lastRenderDoneMs >= PROGRESS_SAVE_IDLE_MS && !RenderLock::peek()) {
      RenderLock lock;
      PocketDaily::ReaderPerf::save();
    }
  }

  // End-of-Book screen reached (currentSpineIndex == spine count) means the book is
  // finished. Two independent finished-book features key off this same condition.
  const bool atEndOfBook = currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount();

  // Drop this book from the Recent Books list; if the reader then pages back into the book,
  // re-add it. So removal only sticks if the reader leaves while still on the End-of-Book
  // screen. Acts only on the transition (guarded by recentsEntryRemoved) — no per-frame writes.
  if (!atEndOfBook) articleEndHandled = false;
  if (atEndOfBook && !articleEndHandled && Articles::isPath(epub->getPath())) {
    articleEndHandled = true;
    articleReadSaveFailed = !Articles::markRead(epub->getPath());
    if (articleReadSaveFailed) {
      LOG_ERR("ARTICLE", "Read status was not saved");
      requestUpdate();
    }
  }

  if (SETTINGS.removeReadBooksFromRecents) {
    if (atEndOfBook && !recentsEntryRemoved) {
      // Only treat the book as "removed by us" if it was actually in the list, so the
      // re-add branch below doesn't insert a book the feature never removed.
      recentsEntryRemoved = RECENT_BOOKS.removeByPath(epub->getPath());
    } else if (!atEndOfBook && recentsEntryRemoved) {
      // Re-add (goes to front of the list via addBook — accepted ordering side effect).
      RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
      recentsEntryRemoved = false;
    }
  }

  // Arm the move here so ANY exit path (Back, Home, file browser) relocates the book into
  // /Read/ in onExit(); paging back off the end screen disarms it (book not actually
  // finished). If removeReadBooksFromRecents also fired, RecentBooksStore::updatePath in the
  // move path becomes a safe no-op since the entry was already removed.
  if (atEndOfBook) {
    pendingReadFolderMove =
        SETTINGS.moveFinishedToReadFolder && !Articles::isPath(epub->getPath()) && !isInReadFolder(epub->getPath());
  } else {
    pendingReadFolderMove = false;
  }

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      automaticPageTurnActive = false;
      // updates chapter title space to indicate page turn disabled
      requestUpdate();
      return;
    }

    if (!section) {
      requestUpdate();
      return;
    }

    // Skips page turn if renderingMutex is busy
    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      pageTurn(true);
      return;
    }
  }

  if (showBookmarkMessage && (millis() - bookmarkMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showBookmarkMessage = false;
    requestUpdate();
  }

  if (showBilingualMessage && (millis() - bilingualMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showBilingualMessage = false;
    requestUpdate();
  }

  // Pocket Daily reading-progress v1: once the page is laid out, ask about a further
  // position another device offered for this book.
  if (readingOfferPending && section && !RenderLock::peek()) {
    readingOfferPending = false;
    askReadingOffer();
    return;
  }

  // Enter reader menu activity on short-press Confirm. A long-press that fired a bound
  // function (bookmark or KOReader sync) sets ignoreNextConfirmRelease so the release
  // following the hold does not also open the menu.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (ignoreNextConfirmRelease) {
      ignoreNextConfirmRelease = false;
    } else {
      const int currentPage = section ? section->currentPage + 1 : 0;
      const int totalPages = section ? section->estimatedTotalPages() : 0;
      float bookProgress = 0.0f;
      if (epub->getBookSize() > 0 && section && section->estimatedTotalPages() > 0) {
        const float chapterProgress =
            static_cast<float>(section->currentPage) / static_cast<float>(section->estimatedTotalPages());
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      }
      const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      startActivityForResult(std::make_unique<EpubReaderMenuActivity>(
                                 renderer, mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent,
                                 SETTINGS.orientation, !currentPageFootnotes.empty(), !cachedBookmarks.empty()),
                             [this](const ActivityResult& result) {
                               // Always apply orientation change even if the menu was cancelled
                               const auto& menu = std::get<MenuResult>(result.data);
                               applyOrientation(menu.orientation);
                               toggleAutoPageTurn(menu.pageTurnOption);
                               if (!result.isCancelled) {
                                 onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
                               }
                             });
    }
  }

  // Long-press Confirm runs the user-selected function (SETTINGS.longPressMenuFunction).
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    switch (SETTINGS.longPressMenuFunction) {
      case CrossPointSettings::LP_MENU_BOOKMARK:
        // Hold ~0.4s drops a bookmark at the current page.
        if (mappedInput.getHeldTime() >= ReaderUtils::BOOKMARK_HOLD_MS && !showBookmarkMessage) {
          addBookmark();
          showBookmarkMessage = true;
          ignoreNextConfirmRelease = true;  // Prevent accidental menu open after adding bookmark
          bookmarkMessageTime = millis();
          requestUpdate();
        }
        break;
      case CrossPointSettings::LP_MENU_KOSYNC:
        // Hold ~1s launches KOReader sync. If sync can't run (no credentials stored), fall
        // through so the normal Confirm-release still opens the reader menu.
        if (mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
          if (launchKOReaderSync()) {
            ignoreNextConfirmRelease = true;  // sync launched or error shown; suppress menu open
            return;
          }
        }
        break;
      case CrossPointSettings::LP_MENU_BILINGUAL_TOGGLE:
        // Hold ~0.4s cycles bilingual view mode (Both → Original → Translation → Both) and
        // re-renders the current section. The v130 header check invalidates the cached section so
        // the new mode re-parses on next render. No-op on non-bilingual EPUBs (no markers → Both).
        if (mappedInput.getHeldTime() >= ReaderUtils::BOOKMARK_HOLD_MS && !ignoreNextConfirmRelease) {
          cycleBilingualMode();             // also fires the transient mode-name popup
          ignoreNextConfirmRelease = true;  // Suppress the subsequent Confirm-release menu open
        }
        break;
      case CrossPointSettings::LP_MENU_DISABLED:
      default:
        // Migration: pre-bilingual-toggle firmwares shipped with DISABLED as the
        // default. Rather than force existing users to find the new setting,
        // treat a long-press Confirm as Bilingual Toggle when no other function
        // is bound. Users who explicitly want KOReader Sync or Bookmark can still
        // pick those in Settings → Controls → Long-press Menu. On non-bilingual
        // EPUBs (no cp-original/cp-translation markers) the cycle is a no-op for
        // rendering but the popup still fires for button-press feedback.
        if (mappedInput.getHeldTime() >= ReaderUtils::BOOKMARK_HOLD_MS && !ignoreNextConfirmRelease) {
          cycleBilingualMode();
          ignoreNextConfirmRelease = true;
        }
        break;
    }
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    if (epub && Articles::isPath(epub->getPath()))
      activityManager.goToArticles();
    else
      activityManager.goToFileBrowser(epub ? epub->getPath() : "");
    return;
  }

  // Short press BACK goes directly to home (or restores position if viewing footnote)
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
      return;
    }
    if (epub && Articles::isPath(epub->getPath()))
      activityManager.goToArticles();
    else
      onGoHome();
    return;
  }

  // auto [prevTriggered, nextTriggered] = ReaderUtils::detectPageTurn(mappedInput);

  // Handle short power button press for footnotes
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FOOTNOTES &&
      mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      !mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
    } else {
      if (currentPageFootnotes.size() == 1) {
        navigateToHref(currentPageFootnotes[0].href, true);
      } else if (currentPageFootnotes.size() > 1) {
        startActivityForResult(
            std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) {
                const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                navigateToHref(footnoteResult.href, true);
              }
              requestUpdate();
            });
      }
    }
    return;
  }

  const auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // At end of the book, forward button goes home and back button returns to last page
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    if (nextTriggered) {
      onGoHome();
    } else {
      currentSpineIndex = epub->getSpineItemsCount() - 1;
      nextPageNumber = 0;
      pendingPageJump = LANDING_LAST_PAGE;
      requestUpdate();
    }
    return;
  }

  const bool longPress = !fromTilt && mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;

  // Don't skip chapter after screenshot
  if (gpio.wasReleased(HalGPIO::BTN_POWER) && gpio.wasReleased(HalGPIO::BTN_DOWN)) {
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP) {
    if (!nextTriggered && section && section->currentPage > 0) {
      section->currentPage = 0;
      requestUpdate();
      return;
    }

    // We don't want to delete the section mid-render, so grab the semaphore
    {
      RenderLock lock(*this);
      nextPageNumber = 0;
      if (nextTriggered) {
        currentSpineIndex++;
      } else if (currentSpineIndex > 0) {
        currentSpineIndex--;
      }
      section.reset();
    }
    requestUpdate();
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.ORIENTATION_CHANGE) {
    const uint8_t newOrientation =
        nextTriggered ? (SETTINGS.orientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
                      : (SETTINGS.orientation + 1) % SETTINGS.ORIENTATION_COUNT;
    applyOrientation(newOrientation);
    requestUpdate();
    return;
  }

  // No current section, attempt to rerender the book
  if (!section) {
    requestUpdate();
    return;
  }

  if (prevTriggered) {
    pageTurn(false);
  } else {
    pageTurn(true);
  }
}

// Translate an absolute percent into a spine index plus a normalized position
// within that spine so we can jump after the section is loaded.
void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) {
    return;
  }

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return;
  }

  // Normalize input to 0-100 to avoid invalid jumps.
  percent = clampPercent(percent);

  // Convert percent into a byte-like absolute position across the spine sizes.
  // Use an overflow-safe computation: (bookSize / 100) * percent + (bookSize % 100) * percent / 100
  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) {
    // Ensure the final percent lands inside the last spine item.
    targetSize = bookSize - 1;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return;
  }

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      // Found the spine item containing the absolute position.
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  // Store a normalized position within the spine so it can be applied once loaded.
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  if (pendingSpineProgress < 0.0f) {
    pendingSpineProgress = 0.0f;
  } else if (pendingSpineProgress > 1.0f) {
    pendingSpineProgress = 1.0f;
  }

  // Reset state so render() reloads and repositions on the target spine.
  {
    RenderLock lock(*this);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    section.reset();
  }
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  auto progressChangeResultHandler = [this](const ActivityResult& result) {
    loadCachedBookmarks();
    if (!result.isCancelled) {
      const auto& sync = std::get<ProgressChangeResult>(result.data);
      int targetSpineIndex = sync.spineIndex;
      int targetPage = sync.page;
      const int activeTotalPages = section ? section->estimatedTotalPages() : 0;
      const bool cachedPageMatchesActiveSection = section && sync.totalPages > 0 &&
                                                  currentSpineIndex == sync.spineIndex && sync.page >= 0 &&
                                                  sync.page < sync.totalPages && activeTotalPages == sync.totalPages;

      if (!cachedPageMatchesActiveSection && sync.hasSavedProgress) {
        const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
        CrossPointPosition fallback =
            ProgressMapper::toCrossPoint(epub, {sync.xpath, sync.percentage}, renderer, currentSpineIndex, totalPages);
        targetSpineIndex = fallback.spineIndex;
        targetPage = fallback.pageNumber;
        RenderLock lock(*this);
        setPendingTextOffset(fallback);
      }

      if (currentSpineIndex != targetSpineIndex) {
        RenderLock lock(*this);
        currentSpineIndex = targetSpineIndex;
        nextPageNumber = targetPage;
        section.reset();
      } else if (section && section->currentPage != targetPage) {
        RenderLock lock(*this);
        const int clampedTargetPage = std::max(0, targetPage);
        section->currentPage = clampedTargetPage;
      } else if (!section) {
        nextPageNumber = targetPage;
      }
    }
  };

  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spineIdx = currentSpineIndex;
      const std::string path = epub->getPath();
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, path, spineIdx),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              const auto& chapterResult = std::get<ChapterResult>(result.data);
              RenderLock lock(*this);

              currentSpineIndex = chapterResult.spineIndex;

              // If anchor is not empty, it will be used later to calculate the page number.
              pendingAnchor = chapterResult.anchor;

              // Otherwise page 0 will be used.
              nextPageNumber = 0;

              section.reset();
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                                 navigateToHref(footnoteResult.href, true);
                               }
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      float bookProgress = 0.0f;
      if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
        const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      }
      const int initialPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      startActivityForResult(
          std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        std::string fullText = section->getTextFromSectionFile();
        if (!fullText.empty()) {
          startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                                 [this](const ActivityResult& result) {});
          break;
        }
      }
      // If no text or page loading failed, just close menu
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      {
        RenderLock lock(*this);
        if (epub && section) {
          uint16_t backupSpine = currentSpineIndex;
          uint16_t backupPage = section->currentPage;
          uint16_t backupPageCount = section->pageCount;
          section.reset();
          epub->clearCache();
          epub->setupCacheDir();
          if (!saveProgress(backupSpine, backupPage, backupPageCount)) {
            LOG_ERR("ERS", "Failed to save progress before cache clear");
          }
        }
      }
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      {
        RenderLock lock(*this);
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC: {
      launchKOReaderSync();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOKMARKS: {
      startActivityForResult(
          std::make_unique<EpubReaderBookmarksActivity>(renderer, mappedInput, epub, epub->getPath()),
          progressChangeResultHandler);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TOGGLE_BOOKMARK: {
      addBookmark();
      break;
    }
  }
}

void EpubReaderActivity::askReadingOffer() {
  const int totalPages = section->estimatedTotalPages();
  const float pageStart =
      totalPages > 0 ? static_cast<float>(section->currentPage) / static_cast<float>(totalPages) : 0;
  std::string question;
  std::string xpointer;
  float percentage = 0.0f;
  if (!PocketDaily::ReadingProgress::takeFurtherOffer(*epub, epub->calculateProgress(currentSpineIndex, pageStart),
                                                      question, xpointer, percentage)) {
    return;
  }
  auto confirm = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, question, "");
  if (!confirm) return;  // The offer stays pending; the next open asks again.
  startActivityForResult(
      std::move(confirm), [this, xpointer = std::move(xpointer), percentage](const ActivityResult& r) {
        PocketDaily::ReadingProgress::discardOffer(*epub);
        if (r.isCancelled) return;
        const int pages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
        const CrossPointPosition target =
            ProgressMapper::toCrossPoint(epub, {xpointer, percentage}, renderer, currentSpineIndex, pages);
        RenderLock lock(*this);
        setPendingTextOffset(target);
        if (!section || currentSpineIndex != target.spineIndex) {
          currentSpineIndex = target.spineIndex;
          nextPageNumber = target.pageNumber;
          section.reset();
        } else {
          section->currentPage = std::max(0, target.pageNumber);
        }
      });
}

void EpubReaderActivity::setPendingTextOffset(const CrossPointPosition& target) {
  if (target.hasTextOffset) {
    pendingTextOffset = PendingTextOffset{target.spineIndex, target.textOffset};
  } else {
    pendingTextOffset.reset();
  }
}

// Moves to the page holding the pending XPointer's character. A building section (or a
// partial) is laid out until a page starting past that character exists, so the landing is
// the exact page rather than the estimate toCrossPoint made without the layout.
bool EpubReaderActivity::resolvePendingTextOffset(const SectionLayout& layout) {
  const PendingTextOffset pending = *pendingTextOffset;
  pendingTextOffset.reset();
  if (!section || pending.spineIndex != currentSpineIndex) return true;  // navigated elsewhere meanwhile
  for (;;) {
    uint16_t page = 0;
    const auto lookup = section->findPageForTextOffset(pending.textOffset, page);
    if (lookup == Section::TextOffsetLookup::Found) {
      LOG_DBG("ERS", "Text offset %lu -> page %u (estimate %d)", static_cast<unsigned long>(pending.textOffset), page,
              section->currentPage);
      section->currentPage = page;
      return true;
    }
    if (lookup == Section::TextOffsetLookup::Unavailable) return true;  // keep the estimate
    if (!section->isBuilding() && !(section->isPartial() && layout.startBuild(*section))) {
      return true;
    }
    PocketDaily::ReaderPerf::addFlags(PocketDaily::ReaderPerf::FLAG_BUILT);
    if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
      LOG_ERR("ERS", "Failed during incremental section build");
      return false;
    }
  }
}

bool EpubReaderActivity::launchKOReaderSync() {
  if (!KOREADER_STORE.hasCredentials()) return false;  // no-op: nothing to launch

  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  // Pre-compute local KO position and chapter name while Epub is still in RAM.
  CrossPointPosition localPos = getCurrentPosition();
  SavedProgressPosition localKoPos = ProgressMapper::toSavedProgress(epub, localPos);
  const int tocIdx = epub->getTocIndexForSpineIndex(currentSpineIndex);
  std::string localChapterName = (tocIdx >= 0) ? epub->getTocItem(tocIdx).title : "";
  const std::string savedEpubPath = epub->getPath();

  // Persist current position so the reader resumes at the right page on return.
  // goToReader() depends on this file, so abort the sync if the write fails.
  if (!saveProgress(currentSpineIndex, currentPage, totalPages)) {
    LOG_ERR("KOSync", "Aborting sync because current progress could not be saved");
    pendingSyncSaveError = true;
    requestUpdate();
    return true;  // acted: surfaced a save error to the user
  }

  // Release Epub and Section to free ~65KB RAM for the TLS handshake.
  LOG_DBG("KOSync", "Releasing epub for sync (heap before: %u)", (unsigned)ESP.getFreeHeap());
  {
    RenderLock lock(*this);
    if (section) {
      nextPageNumber = section->currentPage;
    }
    section.reset();
    epub.reset();
  }
  LOG_DBG("KOSync", "Epub released (heap after: %u)", (unsigned)ESP.getFreeHeap());

  activityManager.replaceActivity(std::make_unique<KOReaderSyncActivity>(
      renderer, mappedInput, savedEpubPath, currentSpineIndex, currentPage, totalPages, std::move(localKoPos),
      std::move(localChapterName), paragraphIndex));
  return true;  // acted: launched the sync activity
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // No-op if the selected orientation matches current settings.
  if (SETTINGS.orientation == orientation) {
    return;
  }

  // Preserve current reading position so we can restore after reflow.
  {
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }

    // Persist the selection so the reader keeps the new orientation on next launch.
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();

    // Update renderer orientation to match the new logical coordinate system.
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

    // Reset section to force re-layout in the new orientation.
    section.reset();
  }
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= std::size(PAGE_TURN_RATES)) {
    automaticPageTurnActive = false;
    return;
  }

  lastPageTurnTime = millis();
  // calculates page turn duration by dividing by number of pages
  pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_RATES[selectedPageTurnOption];
  automaticPageTurnActive = true;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  // resets cached section so that space is reserved for auto page turn indicator when None or progress bar only
  if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
    // Preserve current reading position so we can restore after reflow.
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    section.reset();
  }
}

void EpubReaderActivity::cycleBilingualMode() {
  SETTINGS.bilingualViewMode = (SETTINGS.bilingualViewMode + 1) % CrossPointSettings::BILINGUAL_VIEW_MODE_COUNT;
  SETTINGS.saveToFile();
  showBilingualMessage = true;
  bilingualMessageTime = millis();
  {
    RenderLock lock(*this);
    if (section && section->currentlyModeAgnostic()) {
      // No bilingual marker in this chapter so far (finalized cache tagged
      // BILINGUAL_MODE_ANY, or an in-progress parse that has not met one): its layout is
      // identical in every mode, so keep the section (and the reading position) and only
      // show the mode popup. Dropping it here forced a full re-parse of a monolingual chapter
      // on every accidental long press, with the parser state then held across the next
      // render. Marker-bearing chapters still rebuild lazily via the header mode check.
    } else {
      // Drop the current section so the next render re-parses with the new mode; the
      // header mode check invalidates the on-disk cache. Preserve the reading position
      // the same way updateStatusBarSetting does: re-layout changes pagination, so the
      // render path restores proportionally via cachedChapterTotalPageCount.
      if (section) {
        cachedSpineIndex = currentSpineIndex;
        cachedChapterTotalPageCount = section->pageCount;
        nextPageNumber = section->currentPage;
      }
      section.reset();
    }
  }
  requestUpdate();
}

namespace {
namespace BuildLog = PocketDaily::BuildFailureLog;
static_assert(static_cast<uint8_t>(Section::BuildStep::HtmlStream) == BuildLog::STEP_HTML_STREAM &&
                  static_cast<uint8_t>(Section::BuildStep::SectionFile) == BuildLog::STEP_SECTION_FILE &&
                  static_cast<uint8_t>(Section::BuildStep::BuildContext) == BuildLog::STEP_BUILD_CONTEXT &&
                  static_cast<uint8_t>(Section::BuildStep::Parser) == BuildLog::STEP_PARSER &&
                  static_cast<uint8_t>(Section::BuildStep::BeginParse) == BuildLog::STEP_BEGIN_PARSE &&
                  static_cast<uint8_t>(Section::BuildStep::Layout) == BuildLog::STEP_LAYOUT &&
                  static_cast<uint8_t>(Section::BuildStep::Commit) == BuildLog::STEP_COMMIT,
              "BuildFailureLog step codes mirror Section::BuildStep");
static_assert(static_cast<uint8_t>(ZipFile::StreamError::Window) == 5 &&
                  static_cast<uint8_t>(ZipFile::StreamError::Method) == 9,
              "BuildFailureLog detail names mirror ZipFile::StreamError");
}  // namespace

// Keeps the failed step and the heap it met (read before the section is released) in
// /.crosspoint/last-build-error.bin for /api/status. Runs only on a build failure.
void EpubReaderActivity::recordBuildFailure() const {
  if (!epub) return;
  BuildLog::Entry entry;
  entry.book = static_cast<uint32_t>(std::hash<std::string>{}(epub->getPath()));
  entry.spine = static_cast<uint16_t>(std::max(0, currentSpineIndex));
  if (section) {
    entry.step = static_cast<uint8_t>(section->lastBuildFailure());
    entry.detail = section->lastBuildFailureDetail();
  }
  entry.freeHeap = ESP.getFreeHeap();
  entry.largestBlock = ESP.getMaxAllocHeap();
  entry.uptimeSec = millis() / 1000;
  snprintf(entry.version, sizeof(entry.version), "%s", CROSSPOINT_VERSION);
  LOG_ERR("ERS", "Section build failed: spine %d step %s (%u), free %u, largest block %u", currentSpineIndex,
          BuildLog::stepName(entry.step), static_cast<unsigned>(entry.detail), static_cast<unsigned>(entry.freeHeap),
          static_cast<unsigned>(entry.largestBlock));
  if (!BuildLog::record(entry)) LOG_ERR("ERS", "Could not store the build failure record");
}

void EpubReaderActivity::pageTurn(bool isForwardTurn) {
  if (landingInProgress.load()) {
    // The render laying out the previous chapter acts on it (see landingInProgress).
    if (isForwardTurn) {
      landingCancelled.store(true);
    } else {
      landingExtraBack.fetch_add(1);
    }
    return;
  }
  queuePerfTurn(isForwardTurn ? 0 : PocketDaily::ReaderPerf::FLAG_BACK, millis());
  turnQueued.store(true);
  if (isForwardTurn) {
    // Advance within the section while there are (or may still be) more pages: either a built
    // page ahead, or the section is still building (windowed), in which case more pages exist
    // beyond the current watermark and render()'s ensure-built pump will lay them out. Only when
    // the section is fully built AND we're on its last page do we move to the next spine -- using
    // the live pageCount alone would mistake the build watermark for the end of a giant spine.
    // A partial whose build was suspended (heap floor) also has pages beyond pageCount:
    // render()'s partial-extension loop restarts the build to reach them. Its watermark
    // trailer says whether unparsed bytes remain, so a suspended build at the true end of a
    // chapter still advances to the next spine instead of stalling on the last page.
    if (section->currentPage < section->pageCount - 1 || section->mayHaveMorePages()) {
      section->currentPage++;
    } else {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        currentSpineIndex++;
        section.reset();
      }
    }
  } else {
    if (section->currentPage > 0) {
      section->currentPage--;
    } else if (currentSpineIndex > 0) {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        pendingPageJump = LANDING_LAST_PAGE;
        currentSpineIndex--;
        section.reset();
      }
    }
  }
  lastPageTurnTime = millis();
  requestUpdate();
}

// TODO: Failure handling
void EpubReaderActivity::render(RenderLock&& lock) {
  if (!epub) {
    return;
  }

  namespace Perf = PocketDaily::ReaderPerf;
  turnQueued.store(false);  // this render serves every turn queued so far
  inputQueued.store(false);
  if (perfTurnPending) {
    perfTurnPending = false;
    Perf::beginTurn(perfInputMs, perfTurnFlags);
  }
  // Renders that do not reach a page (end of book, errors) are not recorded; the page
  // path below calls endTurn() first, which makes this a no-op.
  struct PerfTurnGuard {
    PerfTurnGuard() = default;
    PerfTurnGuard(const PerfTurnGuard&) = delete;
    PerfTurnGuard& operator=(const PerfTurnGuard&) = delete;
    ~PerfTurnGuard() { PocketDaily::ReaderPerf::abortTurn(); }
  } perfTurnGuard;

  activeReaderFontId = sdFontSystem.ensureAutomaticReaderFontLoaded(renderer);
  // A page of the current chapter never renders next to a running pre-build (only the
  // forward turn into the pre-built chapter adopts it, below).
  if (section && layoutAhead) layoutAhead->dropBuilding();

  const auto showPendingSyncSaveError = [this]() {
    if (!pendingSyncSaveError) return;
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  };

  // Invalid EPUB markup, low memory, or an SD write failure used to leave the
  // persistent e-ink panel showing "Indexing" indefinitely. Replace it with a
  // terminal error state so exiting and reopening is never the only feedback.
  const auto showBuildError = [this]() {
    recordBuildFailure();
    section.reset();
    renderer.clearScreen();
    GUI.drawPopup(renderer, tr(STR_INDEX_FAILED));
    automaticPageTurnActive = false;
  };

  // edge case handling for sub-zero spine index
  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  // based bounds of book, show end of book screen
  if (currentSpineIndex > epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount();
  }

  // Show end of book screen
  if (currentSpineIndex == epub->getSpineItemsCount()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
    if (articleReadSaveFailed) GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  // Apply screen viewable areas and additional padding
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

  // reserves space for automatic page turn indicator when no status bar or progress bar only
  if (automaticPageTurnActive &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    orientedMarginBottom +=
        std::max(SETTINGS.screenMargin,
                 static_cast<uint8_t>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin));
  } else {
    orientedMarginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);
  }

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;

  // Landing on the last page of a previous chapter (backward turn): see landingInProgress.
  const bool landing = !section && pendingPageJump.has_value() && *pendingPageJump == LANDING_LAST_PAGE;
  struct LandingGuard {
    std::atomic<bool>& flag;
    ~LandingGuard() { flag.store(false); }
  } landingGuard{landingInProgress};
  if (landing) {
    landingCancelled.store(false);
    landingExtraBack.store(0);
    landingInProgress.store(true);
  }
  // A forward press during that build: return to the first page of the chapter the reader
  // came from, keeping what was laid out (the section's destructor persists it as a partial).
  const auto cancelLanding = [&]() {
    LOG_DBG("ERS", "Back-turn build of section %d cancelled", currentSpineIndex);
    section.reset();
    currentSpineIndex++;
    nextPageNumber = 0;
    pendingPageJump.reset();
    landingCancelled.store(false);
    requestUpdate();
  };

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
    Perf::addFlags(Perf::FLAG_CHAPTER);

    const SectionLayout layout = layoutParams(viewportWidth, viewportHeight);
    shownLayout = layout;
    shownLayoutValid = true;
    // Adopt the idle pre-build of this chapter (finished or still laying out) when it was
    // made with the current layout. Jumps that need the whole chapter or an anchor map
    // (percent, footnote/TOC anchors) take the normal path.
    std::unique_ptr<Section> adopted;
    if (layoutAhead && !pendingPercentJump && pendingAnchor.empty()) {
      adopted = layoutAhead->adopt(currentSpineIndex, layout);
    } else if (layoutAhead) {
      layoutAhead->clear();
    }
    if (adopted) {
      section = std::move(adopted);
      Perf::addFlags(Perf::FLAG_PREBUILT);
      if (!section->isBuilding() && !section->isPartial()) cachedChapterTotalPageCount = 0;
      LOG_DBG("ERS", "Adopted pre-built section %d (%u pages%s)", currentSpineIndex, section->pageCount,
              section->isBuilding() ? ", still building" : "");
    } else {
      section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));
      // A finalized cache serves every page as-is. A partial cache (suspended build from a
      // previous session) serves its pages instantly too, but a build must still run to lay
      // out the rest -- it re-parses from the top in the background (HTML already cached,
      // pages are deterministic) and finalizes, so the partial machinery retires itself.
      const bool cacheLoaded = layout.load(*section);
      if (cacheLoaded) {
        // Matching render params means identical pagination, so the saved page number is valid
        // as-is: consume any pending settings-change reposition. Without this, a chapter total
        // saved while the section was still building (i.e. a watermark, not the real count)
        // would remap the resume page against the finalized count and teleport the reader.
        cachedChapterTotalPageCount = 0;
      }
      const bool cacheComplete = cacheLoaded && !section->isPartial();
      if (!cacheComplete) {
        if (section->isPartial()) {
          LOG_DBG("ERS", "Partial cache found (%d pages), resuming build...", section->pageCount);
        } else {
          LOG_DBG("ERS", "Cache not found, building...");
        }

        // Jumps that need the final pagination or the anchor map -- explicit page jumps,
        // fragment anchors, percent jumps, and cross-setting progress repositioning -- can't
        // resolve their landing page until the whole chapter is laid out, so they take the full
        // (blocking) build with the indexing popup. Everything else -- plain forward reads, resume,
        // and explicit page jumps -- only needs a specific page, so it builds incrementally to that
        // page and finishes the rest in loop(). The settings-change reposition (cachedChapterTotal*)
        // is NOT a full-build trigger: it's deferred to applyDeferredReposition() once the real page
        // count is known, so it never blocks the first page.
        // Only a percent jump truly needs the whole chapter up front (percent -> page needs the final
        // page count). Anchor jumps (TOC / chapter select / footnotes) resolve incrementally below --
        // the anchor is recorded as its page is laid out, so a chapter-top anchor lands on page 0
        // without indexing the whole chapter.
        const bool needsFullBuild = pendingPercentJump;
        Perf::addFlags(Perf::FLAG_BUILT);
        if (needsFullBuild) {
          GUI.drawPopup(renderer, tr(STR_INDEXING));
          // The popup's own refresh is a plain FAST, so force the page that replaces it onto the HALF
          // ghost-cleanup path -- otherwise the "INDEXING" text ghosts under the rendered page.
          pagesUntilFullRefresh = 1;
          const auto popupFn = [this]() { GUI.drawPopup(renderer, tr(STR_INDEXING)); };
          if (!section->createSectionFile(activeReaderFontId, SETTINGS.getReaderLineCompression(),
                                          SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                          viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                          SETTINGS.imageRendering, SETTINGS.focusReadingEnabled,
                                          SETTINGS.bilingualViewMode, popupFn)) {
            LOG_ERR("ERS", "Failed to persist page data to SD");
            showBuildError();
            return;
          }
        } else {
          // Lay out just enough to show the landing page; loop() builds the rest behind it. Show the
          // indexing popup up front only when the build will actually be slow: a large spine (its
          // whole HTML must be inflated before page 1 can lay out -- the giant single-spine case), or
          // a deep resume/jump that must lay out many pages to reach the landing page. Tiny sections
          // build in a blink and stay popup-free.
          const int target = pendingPageJump.has_value() ? *pendingPageJump : (nextPageNumber < 0 ? 0 : nextPageNumber);
          const size_t spineBytes =
              epub->getCumulativeSpineItemSize(currentSpineIndex) -
              (currentSpineIndex > 0 ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0);
          // Popup only when the build will actually be slow: a big spine whose HTML still needs
          // inflating (the multi-second cost), or a deep page target. A reopen with cached HTML builds
          // fast, so no popup -- that's what made an already-indexed book look like it was reindexing.
          // A partial cache that already covers the target page shows it instantly: never popup.
          const bool willInflate = !section->hasHtmlCache();
          const bool anchorJump = !pendingAnchor.empty();
          bool showPopup;
          if (anchorJump) {
            // An anchor jump's cost is bounded by the anchor's page, not `target`. An anchor already
            // in the on-disk map (partial or finalized cache) lands instantly: no popup. Otherwise it
            // lies beyond the indexed watermark and the build may lay out the whole spine to find it,
            // so gate on spine size alone -- laying out a big spine takes seconds even with cached
            // HTML. Ordinary chapter-top TOC jumps resolve on page 0 and stay popup-free.
            showPopup = !section->findAnchor(pendingAnchor).has_value() && spineBytes > BUILD_POPUP_BYTE_THRESHOLD;
          } else {
            const bool targetAvailable = target < static_cast<int>(section->pageCount);
            showPopup = !targetAvailable && ((spineBytes > BUILD_POPUP_BYTE_THRESHOLD && willInflate) ||
                                             target > BUILD_POPUP_PAGE_THRESHOLD);
          }
          if (showPopup) {
            GUI.drawPopup(renderer, tr(STR_INDEXING));
            // HALF-clear the popup when the page replaces it, else "INDEXING" ghosts under the page.
            pagesUntilFullRefresh = 1;
          }
          if (!layout.startBuild(*section)) {
            LOG_ERR("ERS", "Failed to start section build");
            showBuildError();
            return;
          }
          while (!section->isBuildComplete() &&
                 (anchorJump ? !section->findAnchor(pendingAnchor) : static_cast<int>(section->pageCount) <= target)) {
            // Anchor jump: build until the anchor's page is laid out (usually page 0), checking a
            // partial's on-disk anchor map too so an already-indexed anchor resolves immediately.
            // Otherwise: build until the target page exists. loop() builds the rest behind it.
            if (landing && landingCancelled.load()) {
              cancelLanding();
              return;
            }
            if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
              LOG_ERR("ERS", "Failed during incremental section build");
              showBuildError();
              return;
            }
          }
        }
      } else {
        LOG_DBG("ERS", "Cache found, skipping build...");
      }
    }

    if (pendingPageJump.has_value()) {
      section->currentPage = *pendingPageJump;
      pendingPageJump.reset();
    } else {
      section->currentPage = nextPageNumber;
      if (section->currentPage < 0) {
        section->currentPage = 0;
      }
    }

    if (!pendingAnchor.empty()) {
      // Resolve from the pages laid out so far and/or the on-disk map (finalized or partial).
      const auto page = section->findAnchor(pendingAnchor);
      if (page) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      } else {
        LOG_DBG("ERS", "Anchor '%s' not found in section %d", pendingAnchor.c_str(), currentSpineIndex);
      }
      pendingAnchor.clear();
    }

    if (pendingPercentJump && section->pageCount > 0) {
      // Apply the pending percent jump now that we know the new section's page count.
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) {
        newPage = section->pageCount - 1;
      }
      section->currentPage = newPage;
      pendingPercentJump = false;
    }
  }

  // Extend the build to the requested page if needed (for partials and in-progress builds).
  // This runs every render, so it covers both the first page and any forward turn that gets
  // ahead of the background builder; pages already built do no work here.
  while (section->isPartial() && section->currentPage >= static_cast<int>(section->pageCount)) {
    Perf::addFlags(Perf::FLAG_BUILT);
    // Start a build to extend a partial toward the requested page.
    if (!section->isBuilding() && !layoutParams(viewportWidth, viewportHeight).startBuild(*section)) {
      LOG_ERR("ERS", "Failed to start partial extension build");
      showBuildError();
      return;
    }
    // Extend until either the target page exists or the build completes.
    while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
      if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
        LOG_ERR("ERS", "Failed during incremental section build");
        showBuildError();
        return;
      }
    }
  }
  // For an in-progress incremental build, make sure the page we're about to show has been laid out.
  if (section->isBuilding()) {
    while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
      Perf::addFlags(Perf::FLAG_BUILT);
      if (landing && landingCancelled.load()) {
        cancelLanding();
        return;
      }
      if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
        LOG_ERR("ERS", "Failed during incremental section build");
        showBuildError();
        return;
      }
    }
  }

  if (pendingTextOffset && !resolvePendingTextOffset(layoutParams(viewportWidth, viewportHeight))) {
    showBuildError();
    return;
  }

  // The requested page is now as built as it will get. If it still lands past the end,
  // clamp to the last real page: the UINT16_MAX "last page" sentinel from backward chapter
  // navigation, an explicit jump beyond a finished chapter, or a stale saved position.
  // Guarded on !isBuilding() because a still-building section's pageCount is only the current
  // watermark (not the final count) and has already been driven far enough by the loops above.
  if (!section->isBuilding() && section->pageCount > 0 &&
      section->currentPage >= static_cast<int>(section->pageCount)) {
    // A landing lands one page earlier per back press made while it was laid out.
    const int earlier = landing ? landingExtraBack.exchange(0) : 0;
    section->currentPage = std::max(0, static_cast<int>(section->pageCount) - 1 - earlier);
  }
  if (landing && landingCancelled.load()) {
    // Forward pressed after the layout finished but before the page was drawn.
    cancelLanding();
    return;
  }
  landingInProgress.store(false);

  // Apply a deferred settings-change reposition now that the real page count is known (a no-op for
  // a plain resume / unchanged pagination). If still building, this defers to loop() on completion.
  applyDeferredReposition();
  Perf::mark(Perf::STAGE_SECTION);

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  updateBookmarkFlag();

  {
    // Unified page read: the in-progress build's in-RAM table if it has reached the page,
    // otherwise the on-disk file (finalized section, or a partial from a previous session).
    auto p = section->loadPage(section->currentPage);
    if (!p) {
      // A load can fail on a corrupt/partial SD cache. Normally we clear the cache and
      // requestUpdate() to re-parse and retry, but bound the retries: if the page keeps
      // failing, requestUpdate() would re-enter render() forever and trip the watchdog.
      constexpr uint8_t MAX_PAGE_LOAD_RETRIES = 3;
      if (pageLoadRetries >= MAX_PAGE_LOAD_RETRIES) {
        LOG_ERR("ERS", "Page load failed %u times - giving up", pageLoadRetries);
        renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
        renderStatusBar();
        renderer.displayBuffer();
        automaticPageTurnActive = false;
        showPendingSyncSaveError();
        return;
      }
      pageLoadRetries++;
      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache (retry %u)", pageLoadRetries);
      // Abandon (not suspend) any active build BEFORE clearing: clearCache deletes the files,
      // and the destructor's suspend would otherwise commit tables into a deleted handle.
      section->abandonBuild();
      section->clearCache();
      section.reset();
      requestUpdate();  // Re-parse and try again after clearing cache
      automaticPageTurnActive = false;
      showPendingSyncSaveError();
      return;
    }

    // Page loaded successfully - reset the failure guard.
    pageLoadRetries = 0;
    Perf::mark(Perf::STAGE_PAGE);

    // Collect footnotes from the loaded page
    currentPageFootnotes = std::move(p->footnotes);

    const auto start = millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
  }
  // Only persist when the position actually changed. render() also runs on menu,
  // bookmark and screenshot re-renders, and writeAtomic is several FAT ops for 6 bytes.
  // The write itself waits for PROGRESS_SAVE_IDLE_MS of idle time in loop()
  // (flushPendingProgress) or for onExit, so a turn never waits on it and rapid turns write
  // once; a crash inside that window resumes at most those last turns back.
  const int estimatedPageCount = section->estimatedTotalPages();
  if (currentSpineIndex != lastSavedSpineIndex || section->currentPage != lastSavedPage ||
      estimatedPageCount != lastSavedPageCount) {
    progressSavePending = true;
    pendingSaveSpine = currentSpineIndex;
    pendingSavePage = section->currentPage;
    pendingSavePageCount = estimatedPageCount;
    // Whole-book percent (7th byte), same math as saveProgress(), computed here on the render
    // task so the idle write touches nothing the next render may be changing.
    const float frac = estimatedPageCount > 0 ? static_cast<float>(section->currentPage) / estimatedPageCount : 0.0f;
    pendingSaveBookPercent = static_cast<int>(currentSpineInfo().bookProgress(frac) * 100.0f + 0.5f);
  } else {
    progressSavePending = false;
  }
  Perf::mark(Perf::STAGE_SAVE);
  Perf::endTurn();
  lastRenderDoneMs = millis();

  showPendingSyncSaveError();

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }

  if (showBookmarkMessage) {
    GUI.drawPopup(renderer, bookmarkRemoved ? tr(STR_BOOKMARK_REMOVED) : tr(STR_BOOKMARK_ADDED));
  }

  if (showBilingualMessage) {
    // Index matches CrossPointSettings::BILINGUAL_VIEW_MODE enum order (Both/Original/Translation).
    static constexpr StrId bilingualModeLabels[CrossPointSettings::BILINGUAL_VIEW_MODE_COUNT] = {
        StrId::STR_BILINGUAL_BOTH, StrId::STR_BILINGUAL_ORIGINAL_ONLY, StrId::STR_BILINGUAL_TRANSLATION_ONLY};
    const uint8_t mode = SETTINGS.bilingualViewMode;
    GUI.drawPopup(renderer, I18N.get(bilingualModeLabels[mode]));
  }

  layOutAhead();
}

bool EpubReaderActivity::applyDeferredReposition() {
  if (cachedChapterTotalPageCount == 0 || !section || section->isBuilding()) {
    return false;
  }
  bool changed = false;
  // Only remap when the chapter actually re-paginated (e.g. after a settings change). A plain
  // resume has identical pagination, so section->pageCount == cachedChapterTotalPageCount and
  // nothing moves.
  if (currentSpineIndex == cachedSpineIndex && section->pageCount != cachedChapterTotalPageCount) {
    const float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
    int newPage = static_cast<int>(progress * static_cast<float>(section->pageCount));
    if (newPage < 0) newPage = 0;
    if (section->pageCount > 0 && newPage >= static_cast<int>(section->pageCount)) {
      newPage = section->pageCount - 1;
    }
    if (newPage != section->currentPage) {
      section->currentPage = newPage;
      changed = true;
    }
  }
  cachedChapterTotalPageCount = 0;  // consumed; don't read cached progress again
  return changed;
}

bool EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  // Whole-book percent for the AgentDeck glance strip (7th byte). Same math as
  // the status bar, from the chapter's cached spine sizes when it is the current one.
  const float frac = (pageCount > 0) ? (float)currentPage / (float)pageCount : 0.0f;
  const float progress = spineIndex == currentSpineIndex ? currentSpineInfo().bookProgress(frac)
                                                         : epub->calculateProgress(spineIndex, frac);
  const int bookPercent = (int)(progress * 100.0f + 0.5f);
  return EpubReaderUtils::saveProgress(*epub, spineIndex, currentPage, pageCount, bookPercent);
}

void EpubReaderActivity::flushPendingProgress(const bool fromLoop) {
  int spine = 0, page = 0, pageCount = 0, bookPercent = -1;
  {
    std::optional<RenderLock> lock;
    if (fromLoop) lock.emplace();  // onExit's caller already holds it
    if (!progressSavePending || !epub) return;
    progressSavePending = false;
    spine = pendingSaveSpine;
    page = pendingSavePage;
    pageCount = pendingSavePageCount;
    bookPercent = pendingSaveBookPercent;
    lastSavedSpineIndex = spine;
    lastSavedPage = page;
    lastSavedPageCount = pageCount;
  }
  const unsigned long start = millis();
  // Only the SD write: the percent was computed by the render that queued the save.
  const bool saved = EpubReaderUtils::saveProgress(*epub, spine, page, pageCount, bookPercent);
  const unsigned long elapsed = millis() - start;
  std::optional<RenderLock> lock;
  if (fromLoop) lock.emplace();
  if (!saved) {
    lastSavedSpineIndex = -1;  // retry at the next idle moment unless a newer position is queued
    if (!progressSavePending) {
      progressSavePending = true;
      pendingSaveSpine = spine;
      pendingSavePage = page;
      pendingSavePageCount = pageCount;
      pendingSaveBookPercent = bookPercent;
    }
  }
  PocketDaily::ReaderPerf::addToLastTurn(PocketDaily::ReaderPerf::STAGE_SAVE, elapsed);
}

SectionLayout EpubReaderActivity::layoutParams(const uint16_t viewportWidth, const uint16_t viewportHeight) const {
  SectionLayout params;
  params.fontId = activeReaderFontId;
  params.lineCompression = SETTINGS.getReaderLineCompression();
  params.extraParagraphSpacing = SETTINGS.extraParagraphSpacing;
  params.paragraphAlignment = SETTINGS.paragraphAlignment;
  params.viewportWidth = viewportWidth;
  params.viewportHeight = viewportHeight;
  params.hyphenationEnabled = SETTINGS.hyphenationEnabled;
  params.embeddedStyle = SETTINGS.embeddedStyle;
  params.imageRendering = SETTINGS.imageRendering;
  params.focusReadingEnabled = SETTINGS.focusReadingEnabled;
  params.bilingualViewMode = SETTINGS.bilingualViewMode;
  return params;
}

void EpubReaderActivity::layOutAhead() {
  if (!layoutAhead || !section || !shownLayoutValid || automaticPageTurnActive) return;
  // Runs on the render task after a complete page, holding the render lock; any button seen
  // by loop() (inputQueued) or a queued turn makes it stop after the page in progress.
  const auto yield = [](void* context) {
    const auto* self = static_cast<EpubReaderActivity*>(context);
    return self->inputQueued.load() || self->turnQueued.load();
  };
  const auto largestBlock = [](void*) { return static_cast<uint32_t>(ESP.getMaxAllocHeap()); };
  switch (layoutAhead->afterPage(section.get(), currentSpineIndex, shownLayout, yield, largestBlock, this)) {
    case ReaderLayoutAhead::Result::ShownFailed:
      recordBuildFailure();
      nextPageNumber = section->currentPage;
      section.reset();
      requestUpdate();
      break;
    case ReaderLayoutAhead::Result::ShownComplete:
      if (applyDeferredReposition()) requestUpdate();  // re-paginated since the saved position
      break;
    case ReaderLayoutAhead::Result::Done:
      break;
  }
}

float EpubReaderActivity::SpineInfo::bookProgress(const float spineRead) const {
  if (bookSize == 0) {
    return 0.0f;
  }
  const size_t curChapterSize = cumulative - previousCumulative;
  const float sectionProgSize = spineRead * static_cast<float>(curChapterSize);
  const float totalProgress = static_cast<float>(previousCumulative) + sectionProgSize;
  return totalProgress / static_cast<float>(bookSize);
}

const EpubReaderActivity::SpineInfo& EpubReaderActivity::currentSpineInfo() {
  if (spineInfo.spineIndex != currentSpineIndex) {
    spineInfo.spineIndex = currentSpineIndex;
    spineInfo.bookSize = epub->getBookSize();
    spineInfo.previousCumulative = currentSpineIndex >= 1 ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
    spineInfo.cumulative = epub->getCumulativeSpineItemSize(currentSpineIndex);
    const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    spineInfo.hasTocEntry = tocIndex != -1;
    spineInfo.tocTitle = spineInfo.hasTocEntry ? epub->getTocItem(tocIndex).title : std::string();
  }
  return spineInfo;
}

std::string EpubReaderActivity::statusBarTitle() {
  if (automaticPageTurnActive) {
    return tr(STR_AUTO_TURN_ENABLED) + std::to_string(60 * 1000 / pageTurnDuration);
  }
  if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    const SpineInfo& info = currentSpineInfo();
    return info.hasTocEntry ? info.tocTitle : std::string(tr(STR_UNNAMED));
  }
  if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    return epub->getTitle();
  }
  return {};
}

void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  (void)orientedMarginRight;
  (void)orientedMarginBottom;
  ReaderPageRenderer::Options options;
  options.fontId = activeReaderFontId;
  options.marginTop = orientedMarginTop;
  options.marginLeft = orientedMarginLeft;
  options.textAntiAliasing = SETTINGS.textAntiAliasing;
  options.refreshFrequency = SETTINGS.getRefreshFrequency();
  options.drawStatusBar = [](void* context) { static_cast<EpubReaderActivity*>(context)->renderStatusBar(); };
  options.context = this;
  options.nextTurnQueued = [](void* context) { return static_cast<EpubReaderActivity*>(context)->turnQueued.load(); };
  // A CJK status-bar title is drawn with the loaded SD font (UiCjkFont reuses it when it
  // covers the text); load its glyphs with the page's instead of one SD open per glyph.
  const std::string title = statusBarTitle();
  const int loadedFontId = sdFontSystem.currentLoadedFontId();
  if (loadedFontId != 0 && !title.empty() && CjkScript::classify(title.c_str()) != CjkScript::Script::None) {
    options.statusText = title.c_str();
    options.statusFontId = loadedFontId;
  }
  ReaderPageRenderer::render(renderer, *page, options, pagesUntilFullRefresh);
}

void EpubReaderActivity::renderStatusBar() {
  // Calculate progress in book. Use the estimated total while a giant spine is still building so
  // "page X of Y" and the progress bar don't read off the small build watermark.
  const int currentPage = section->currentPage + 1;
  const float pageCount = section->estimatedTotalPages();
  const float sectionChapterProg = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) : 0;
  const float bookProgress = currentSpineInfo().bookProgress(sectionChapterProg) * 100;

  std::string title = statusBarTitle();

  int textYOffset = 0;

  if (automaticPageTurnActive) {
    // calculates textYOffset when rendering title in status bar
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

    // offsets text if no status bar or progress bar only
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }
  }

  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset, true, currentPageBookmarked,
                    section->isBuilding());
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;

  // Push current position onto saved stack
  if (savePosition && section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, section->currentPage);
  }

  // Extract fragment anchor (e.g. "#note1" or "chapter2.xhtml#note1")
  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  // Check for same-file anchor reference (#anchor only)
  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';

  int targetSpineIndex;
  if (sameFile) {
    targetSpineIndex = currentSpineIndex;
  } else {
    targetSpineIndex = epub->resolveHrefToSpineIndex(hrefStr);
  }

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    if (savePosition && footnoteDepth > 0) footnoteDepth--;  // undo push
    return;
  }

  {
    RenderLock lock(*this);
    pendingAnchor = std::move(anchor);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    section.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::restoreSavedPosition() {
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock(*this);
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    section.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::loadCachedBookmarks() {
  cachedBookmarks.clear();
  if (cachedBookmarks.capacity() < initialBookmarkCacheCapacity) {
    cachedBookmarks.reserve(initialBookmarkCacheCapacity);
  }
  if (!epub) {
    currentPageBookmarked = false;
    return;
  }

  const std::string bmPath = BookmarkUtil::getBookmarkPath(epub->getPath());
  if (Storage.exists(bmPath.c_str())) {
    String json = Storage.readFile(bmPath.c_str());
    if (!json.isEmpty()) {
      JsonSettingsIO::loadBookmarks(cachedBookmarks, json.c_str());
    }
  }
  updateBookmarkFlag();
}

void EpubReaderActivity::addBookmark() {
  if (!section || !epub) {
    return;
  }
  LOG_DBG("ERS", "Toggle bookmark at spine %d, page %d", currentSpineIndex, section ? section->currentPage : -1);
  int currentPage;
  int pageCount;
  {
    RenderLock lock(*this);
    pageCount = section->estimatedTotalPages();
    currentPage = section->currentPage;
  }

  SavedProgressPosition progress = ProgressMapper::toSavedProgress(epub, getCurrentPosition());
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, currentPage, pageCount);

  const size_t bookmarkCountBeforeToggle = cachedBookmarks.size();
  cachedBookmarks.erase(std::remove_if(cachedBookmarks.begin(), cachedBookmarks.end(),
                                       [&](const BookmarkEntry& b) {
                                         return bookmarkMatchesProgress(b, currentSpineIndex, currentPage, pageCount,
                                                                        pageRange);
                                       }),
                        cachedBookmarks.end());
  if (cachedBookmarks.size() != bookmarkCountBeforeToggle) {
    bookmarkRemoved = true;
    currentPageBookmarked = false;
  } else {
    std::string pageText;
    if (currentPage >= 0 && currentPage < pageCount) {
      pageText = section->getTextFromSectionFile();
    }
    BookmarkEntry entry;
    entry.percentage = progress.percentage;
    entry.xpath = progress.xpath;
    entry.summary = BookmarkUtil::sanitizeBookmarkSummary(pageText);
    entry.computedSpineIndex = currentSpineIndex;
    entry.computedChapterPageCount = pageCount;
    entry.computedChapterProgress = currentPage;
    cachedBookmarks.insert(cachedBookmarks.begin(), entry);
    bookmarkRemoved = false;
    currentPageBookmarked = true;
  }

  const std::string path = BookmarkUtil::getBookmarkPath(epub->getPath());
  const std::string bookmarksDir = BookmarkUtil::getBookmarksDir();
  Storage.mkdir(bookmarksDir.c_str());
  const bool ok = JsonSettingsIO::saveBookmarks(cachedBookmarks, path.c_str());
  if (!ok) {
    LOG_ERR("ERS", "Failed to save bookmarks to: %s", path.c_str());
  }
  requestUpdate();
}

void EpubReaderActivity::updateBookmarkFlag() {
  if (!section || !epub || cachedBookmarks.empty()) {
    currentPageBookmarked = false;
    return;
  }
  const int pageCount = section->estimatedTotalPages();
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, section->currentPage, pageCount);
  currentPageBookmarked = std::any_of(cachedBookmarks.begin(), cachedBookmarks.end(), [&](const BookmarkEntry& b) {
    return bookmarkMatchesProgress(b, currentSpineIndex, section->currentPage, pageCount, pageRange);
  });
}

ScreenshotInfo EpubReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Epub;
  if (epub) {
    snprintf(info.title, sizeof(info.title), "%s", epub->getTitle().c_str());
    info.spineIndex = currentSpineIndex;
  }
  if (section) {
    info.currentPage = section->currentPage + 1;
    info.totalPages = section->estimatedTotalPages();
    if (epub && epub->getBookSize() > 0 && info.totalPages > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(info.totalPages);
      int pct = static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      info.progressPercent = pct;
    }
  }
  return info;
}

CrossPointPosition EpubReaderActivity::getCurrentPosition() const {
  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPages};
  if (paragraphIndex.has_value()) {
    localPos.paragraphIndex = *paragraphIndex;
    localPos.hasParagraphIndex = true;
  }
  return localPos;
}
