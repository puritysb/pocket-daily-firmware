#include <FsHelpers.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <utility>

#include "activities/ActivityManager.h"
#include "activities/reader/ReaderActivity.h"
#include "activities/util/BmpViewerActivity.h"

void ActivityManager::launchReader(const ReaderReturn origin, std::string path, const bool allowFastInitialRefresh) {
  if (path.empty()) {
    goToFileBrowser("/");
    return;
  }
  if (FsHelpers::hasBmpExtension(path) || FsHelpers::hasPngExtension(path)) {
    auto activity = makeUniqueNoThrow<BmpViewerActivity>(renderer, mappedInput, std::move(path));
    if (!activity) {
      LOG_ERR("ACT", "OOM: bitmap viewer activity");
      return;
    }
    replaceActivity(std::move(activity));
    return;
  }
  auto activity = ReaderActivity::create(renderer, mappedInput, std::move(path), allowFastInitialRefresh);
  if (!activity) return;
  // replaceActivity may call onEnter immediately on wake. Publish the origin
  // before either immediate installation or a queued transition can observe it.
  readerReturn = origin;
  replaceActivity(std::move(activity));
}

void ActivityManager::goToReader(std::string path, const bool fast) { launchReader(shell, std::move(path), fast); }
void ActivityManager::goToReaderFrom(ReaderReturn origin, std::string path, const bool fast) {
  launchReader(origin, std::move(path), fast);
}
void ActivityManager::resumeReader(std::string path) { launchReader(readerReturn, std::move(path), false); }

const char* ActivityManager::readerReturnLabel() const {
  switch (readerReturn.load(std::memory_order_relaxed)) {
    case ReaderReturn::Home:
      return tr(STR_EOB_HOME);
    case ReaderReturn::PocketDaily:
      return tr(STR_POCKET_DAILY_APP);
    case ReaderReturn::Articles:
      return tr(STR_ARTICLES);
  }
  return tr(STR_BACK);
}
const char* ActivityManager::shellReturnLabel() const {
  return shell.load(std::memory_order_relaxed) == ReaderReturn::PocketDaily ? tr(STR_BACK) : tr(STR_HOME);
}
