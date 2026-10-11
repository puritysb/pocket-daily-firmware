#include "ScreenCapture.h"

#include "CaptureTicket.h"
#ifdef ENABLE_DEV_REMOTE_FLASH
#include <Arduino.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <Logging.h>

#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"
#include "pocket_daily/product_identity.h"
#include "util/ScreenshotUtil.h"

extern HalDisplay display;
extern ActivityManager activityManager;
namespace PocketDaily::DevCapture {
namespace {
constexpr const char* META = "/.pocket/dev-screen.run";
constexpr const char* REQUEST = "/.pocket/dev-reader.run";
uint32_t activeRun = 0;
uint32_t started = 0;
bool writeTicket(const char* path, uint32_t run) {
  const Ticket ticket = ticketFor(run);
  HalFile file;
  return Storage.openFileForWrite("VISUAL", path, file) &&
         file.write(reinterpret_cast<const uint8_t*>(&ticket), sizeof(ticket)) == sizeof(ticket);
}
uint32_t readTicket(const char* path) {
  Ticket ticket{};
  HalFile file = Storage.open(path);
  if (!file || file.size() != sizeof(ticket) || file.read(&ticket, sizeof(ticket)) != sizeof(ticket)) return 0;
  return decodeTicket(&ticket, sizeof(ticket));
}
bool armBoot(char marker) {
  {
    HalFile file;
    if (!Storage.openFileForWrite("VISUAL", DEV_BOOT_FILE_TRANSFER_MARKER, file) ||
        file.write(reinterpret_cast<const uint8_t*>(&marker), 1) != 1)
      return false;
  }
  HalFile verify = Storage.open(DEV_BOOT_FILE_TRANSFER_MARKER);
  char actual = 0;
  return verify && verify.size() == 1 && verify.read(&actual, 1) == 1 && actual == marker;
}
void bookPath(char* output, size_t size, uint32_t run) {
  snprintf(output, size, "/Pocket visual %lu.epub", static_cast<unsigned long>(run));
}
}  // namespace
bool save(uint32_t run) {
  if (!run || display.isInverted()) return false;
  // Invalidate first: failed writes cannot expose an earlier successful frame.
  if (Storage.exists(META) && !Storage.remove(META)) return false;
  return ScreenshotUtil::saveFramebufferAsBmp(PATH, display.getFrameBuffer(), display.getDisplayWidth(),
                                              display.getDisplayHeight()) &&
         writeTicket(META, run) && readTicket(META) == run;
}
uint32_t savedRun() { return readTicket(META); }
bool remove(uint32_t run) {
  if (!run || savedRun() != run) return false;
  return Storage.remove(PATH) && Storage.remove(META);
}
bool requestReader(uint32_t run) {
  char path[64];
  bookPath(path, sizeof(path), run);
  if (!run || activeRun || Storage.exists(REQUEST)) return false;
  {
    HalFile book = Storage.open(path);
    if (!book || book.isDirectory() || !book.size() || book.size() > 65536) return false;
  }
  if (Storage.exists(META) && !Storage.remove(META)) return false;
  if (writeTicket(REQUEST, run) && readTicket(REQUEST) == run && armBoot('R')) return true;
  Storage.remove(REQUEST);
  Storage.remove(DEV_BOOT_FILE_TRANSFER_MARKER);
  return false;
}
bool beginReader() {
  const uint32_t run = readTicket(REQUEST);
  if (!run || !armBoot('S')) return false;  // recovery route before book parsing
  activeRun = run;
  started = millis();
  char path[64];
  bookPath(path, sizeof(path), run);
  activityManager.goToReader(path);
  return true;
}
bool recoverReader() {
  if (!readTicket(REQUEST)) return false;
  // A pre-armed LAN return after a crash/timeout must not repeat the open.
  Storage.remove(REQUEST);
  return true;
}
bool readerActive() { return activeRun != 0; }
void readerLoop() {
  if (!activeRun) return;
  bool done = false;
  bool finished = false;
  {
    RenderLock lock(RenderLock::Mode::Try);
    if (lock.ownsLock()) {
      const auto info = activityManager.getScreenshotInfo();
      // rememberBookOnceRendered normally persists APP_STATE; developer fixture
      // rendering bypasses that mutation but still uses the real EPUB activity.
      if (info.readerType == ScreenshotInfo::ReaderType::Epub && info.pageRendered && info.currentPage > 0 &&
          info.totalPages > 0 && millis() - started > 10000) {
        finished = true;
        done = save(activeRun);
      }
    }
  }
  if (!finished && millis() - started < 90000) return;
  if (!done) LOG_ERR("VISUAL", "Reader capture timed out; returning to Same Wi-Fi");
  // A synthetic book only. Do not run an exit that publishes synthetic progress.
  // The original book, recents and APP_STATE were never modified.
  Storage.remove(REQUEST);
  Storage.prepareForDeepSleep();
  ESP.restart();
}
}  // namespace PocketDaily::DevCapture
#endif
