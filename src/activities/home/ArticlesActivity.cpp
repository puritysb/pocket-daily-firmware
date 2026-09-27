#include "ArticlesActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>

#include "RecentBooksStore.h"
#include "activities/util/ConfirmationActivity.h"
#include "articles/ArticleStorage.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"
#include "util/UiCjkFont.h"

std::string ArticlesActivity::path(size_t index) const {
  return std::string(Articles::DIRECTORY) + "/" + state->entries[index].filename;
}
void ArticlesActivity::onEnter() {
  Activity::onEnter();
  state = makeUniqueNoThrow<State>();
  if (!state) {
    LOG_ERR("ARTICLE", "Cannot allocate article list");
    failed = true;
    requestUpdate();
    return;
  }
  rowsPerPage = std::max(
      1,
      std::min(int(PAGE_ROWS), UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, true) - 1));
  inputLocked = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  load();
  requestUpdate();
}
void ArticlesActivity::onExit() {
  Activity::onExit();
  state.reset();
}
void ArticlesActivity::load() {
  count = 0;
  limited = false;
  auto dir = Storage.open(Articles::DIRECTORY);
  if (!dir) {
    loadPage();
    return;
  }
  char filename[64];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(filename, sizeof(filename));
    if (file.isDirectory() || !Articles::isFilename(filename)) continue;
    file.close();
    const std::string fullPath = std::string(Articles::DIRECTORY) + "/" + filename;
    if (!Articles::readMetadata(fullPath.c_str(), state->scratch)) continue;
    size_t index = count;
    if (count == LIMIT) {
      limited = true;
      index = 0;
      for (size_t i = 1; i < count; ++i)
        if (state->entries[i].savedAt < state->entries[index].savedAt) index = i;
      if (state->scratch.savedAt() <= state->entries[index].savedAt) continue;
    } else
      ++count;
    snprintf(state->entries[index].filename, sizeof(state->entries[index].filename), "%s", filename);
    state->entries[index].savedAt = state->scratch.savedAt();
  }
  std::sort(state->entries, state->entries + count, [](const Entry& a, const Entry& b) {
    return a.savedAt != b.savedAt ? a.savedAt > b.savedAt : strcmp(a.filename, b.filename) < 0;
  });
  selected = std::min(selected, count);  // Last row is the explicit read-article cleanup action.
  loadPage();
}
void ArticlesActivity::loadPage() {
  pageStart = (selected / rowsPerPage) * rowsPerPage;
  for (size_t row = 0; row < rowsPerPage && pageStart + row < count; ++row) {
    const auto fullPath = path(pageStart + row);
    state->status[row] = ReadingState::New;
    if (!Articles::readMetadata(fullPath.c_str(), state->rows[row])) {
      state->status[row] = ReadingState::Unavailable;
      continue;
    }
    if (Articles::isRead(fullPath)) {
      state->status[row] = ReadingState::Read;
      continue;
    }
    char progress[64];
    snprintf(progress, sizeof(progress), "/.crosspoint/epub_%u/progress.bin",
             unsigned(std::hash<std::string>{}(fullPath)));
    if (Storage.exists(progress)) state->status[row] = ReadingState::Reading;
  }
}
void ArticlesActivity::move(int direction) {
  const size_t total = count + (count ? 1 : 0);
  if (!total) return;
  selected = direction > 0 ? (selected + 1) % total : (selected + total - 1) % total;
  if ((selected / rowsPerPage) * rowsPerPage != pageStart) loadPage();
  requestUpdate();
}
bool ArticlesActivity::deleteEntry(size_t index) {
  const auto fullPath = path(index);
  if (!Storage.remove(fullPath.c_str())) {
    LOG_ERR("ARTICLE", "Article removal failed");
    return false;
  }
  clearBookCache(fullPath);
  RECENT_BOOKS.removeByPath(fullPath);
  const auto marker = Articles::donePath(fullPath);
  if (Storage.exists(marker.c_str()) && !Storage.remove(marker.c_str())) {
    LOG_ERR("ARTICLE", "Read marker removal failed");
    return false;
  }
  return true;
}
void ArticlesActivity::promptDelete(bool readOnly) {
  size_t eligible = 0;
  if (readOnly) {
    for (size_t i = 0; i < count; ++i)
      if (Articles::isRead(path(i))) ++eligible;
    if (!eligible) return;
  }
  const std::string body =
      readOnly ? std::to_string(eligible) + " " + tr(STR_ARTICLES_READ) : state->rows[selected - pageStart].title();
  auto confirmation = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE), body);
  if (!confirmation) {
    LOG_ERR("ARTICLE", "Cannot allocate confirmation");
    failed = true;
    requestUpdate();
    return;
  }
  // Confirmation is one short-lived activity, required to make deletion explicit and cancellable.
  startActivityForResult(std::move(confirmation), [this, readOnly](const ActivityResult& result) {
    if (result.isCancelled) return;
    failed = false;
    if (readOnly) {
      for (size_t i = 0; i < count; ++i) {
        if (Articles::isRead(path(i)) && !deleteEntry(i)) {
          failed = true;
          break;
        }
      }
    } else
      failed = !deleteEntry(selected);
    load();
    inputLocked = true;
    requestUpdate();
  });
}
void ArticlesActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome(HomeMenuItem::ARTICLES);
    return;
  }
  if (!state) return;
  if (inputLocked) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) inputLocked = false;
    return;
  }
  navigator.onNextRelease([this] { move(1); });
  navigator.onPreviousRelease([this] { move(-1); });
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && count) {
    if (selected == count) {
      promptDelete(true);
      return;
    }
    if (state->status[selected - pageStart] == ReadingState::Unavailable) return;
    if (mappedInput.getHeldTime() >= 1000) {
      promptDelete(false);
      return;
    }
    onSelectBook(path(selected));
  }
}
void ArticlesActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_ARTICLES));
  if (!state || failed) {
    renderer.drawText(UiCjkFont::fontForText(renderer, tr(STR_ARTICLES_ERROR), UI_10_FONT_ID),
                      metrics.contentSidePadding, top, tr(STR_ARTICLES_ERROR));
  } else if (!count) {
    renderer.drawText(UiCjkFont::fontForText(renderer, tr(STR_ARTICLES_EMPTY), UI_10_FONT_ID),
                      metrics.contentSidePadding, top, tr(STR_ARTICLES_EMPTY));
  } else {
    const size_t visible = std::min(rowsPerPage, count + 1 - pageStart);
    GUI.drawList(
        renderer,
        Rect{0, top, width,
             renderer.getScreenHeight() - top - metrics.buttonHintsHeight - renderer.getLineHeight(UI_10_FONT_ID) -
                 metrics.verticalSpacing},
        visible, selected - pageStart,
        [this](int row) -> std::string {
          if (pageStart + row == count) return tr(STR_ARTICLES_CLEANUP);
          return state->status[row] == ReadingState::Unavailable ? tr(STR_ARTICLES_ERROR) : state->rows[row].title();
        },
        [this](int row) -> std::string {
          if (pageStart + row == count || state->status[row] == ReadingState::Unavailable) return "";
          const char* status = "";
          switch (state->status[row]) {
            case ReadingState::New:
              status = tr(STR_ARTICLES_NEW);
              break;
            case ReadingState::Reading:
              status = tr(STR_ARTICLES_READING);
              break;
            case ReadingState::Read:
              status = tr(STR_ARTICLES_READ);
              break;
            case ReadingState::Unavailable:
              break;
          }
          return std::string(status) + " · " + state->rows[row].source();
        },
        [](int) { return UITheme::getFileIcon("article.epub"); });
    const char* hint = limited ? tr(STR_ARTICLES_LIMIT) : tr(STR_HOLD_OPEN_TO_DELETE);
    renderer.drawText(UiCjkFont::fontForText(renderer, hint, UI_10_FONT_ID), metrics.contentSidePadding,
                      renderer.getScreenHeight() - metrics.buttonHintsHeight - renderer.getLineHeight(UI_10_FONT_ID),
                      hint);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_HOME), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
