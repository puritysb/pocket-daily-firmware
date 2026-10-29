#include "ScreenCapture.h"

#include "CaptureTicket.h"
#ifdef ENABLE_DEV_REMOTE_FLASH
#include <Arduino.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <Logging.h>
#include <Memory.h>
#include <mbedtls/sha256.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "ReaderBenchmark.h"
#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"
#include "pocket_daily/preview/LayoutInputsDevice.h"
#include "pocket_daily/product_identity.h"
#include "util/ScreenshotUtil.h"

extern HalDisplay display;
extern GfxRenderer renderer;
extern ActivityManager activityManager;
namespace PocketDaily::DevCapture {
namespace {
constexpr const char* META = "/.pocket/dev-screen.run";
constexpr const char* REQUEST = "/.pocket/dev-reader.run";
WakeEvidence wake;
uint32_t activeRun = 0;
uint32_t started = 0;
constexpr const char* FIXTURE = "/.pocket/dev-reading.run";
constexpr const char* ACTION = "/.pocket/dev-reading.action";
constexpr const char* SUCCESS = "/.pocket/dev-reading.success";
ReaderAction readerAction = ReaderAction::Capture;
ReaderPhase readerPhase = ReaderPhase::Reading;
constexpr const char* WALK_RESULT = "/.pocket/dev-walk.result";
constexpr const char* WALK_PERFORMANCE = "/.pocket/dev-walk-performance.bin";
constexpr const char* WALK_PAGES = "/.pocket/dev-walk.pages";
constexpr const char* WALK_ERROR = "/.pocket/dev-walk-error.bin";
constexpr const char* WALK_ERROR_RUN = "/.pocket/dev-walk-error.run";
constexpr const char* WALK_IMAGES = "/.pocket/dev-walk-images.bin";
constexpr const char* WALK_IMAGES_RUN = "/.pocket/dev-walk-images.run";
constexpr const char* WALK_PAINT = "/.pocket/dev-walk-paint.bin";
constexpr const char* WALK_PAINT_RUN = "/.pocket/dev-walk-paint.run";
bool paintRequired = false;
constexpr const char* LIBRARY_RECORD = "/.pocket/dev-library.bin";
constexpr const char* LIBRARY_RUN = "/.pocket/dev-library.run";
constexpr const char* LIBRARY_ORIGINAL_RUN = "/.pocket/dev-library-original.run";
constexpr const char* LIBRARY_OWNED_RUN = "/.pocket/dev-library-owned.run";
LibraryEvidence libraryEvidence;
bool activeLibrary = false;
char ownedLibraryPath[64]{};
// Dev-only scratch, serialized by RenderLock / the radio-free boot path. Reuse
// prevents repeated heap buffers while hashing the fixed backup set.
uint8_t libraryHashBuffer[128];
mbedtls_sha256_context libraryHashContext;
bool storeLibrary();
bool loadLibrary(uint32_t run);
bool hasLibraryArtifacts();
bool libraryPreserved();
bool clearLibrary(uint32_t run);
// Dev-only fixed evidence; no per-page allocation or retained frame.
ReaderWalk walk;
// Keep the serialization frame separate from readerLoop's existing diagnostics.
__attribute__((noinline)) bool storeWalkPerformance() {
  namespace Benchmark = ReaderBenchmark;
  const uint32_t pages = walk.evidence.requested;
  // Longer correctness walks cannot fit a complete trace in the existing ring.
  if (walk.evidence.state != WalkState::Completed || pages > Benchmark::MAX_PAGES) return true;
  if (ReaderPerf::unsavedTurns() != pages) return false;
  uint8_t header[Benchmark::HEADER_BYTES];
  if (!Benchmark::encodeHeader(activeRun, pages, header)) return false;
  HalFile file;
  if (!Storage.openFileForWrite("BENCH", WALK_PERFORMANCE, file) ||
      file.write(header, sizeof(header)) != sizeof(header))
    return false;
  for (uint16_t i = 0; i < pages; ++i) {
    ReaderPerf::TurnRecord record;
    if (!ReaderPerf::copyRecent(pages - 1 - i, record) ||
        static_cast<bool>(record.flags & ReaderPerf::FLAG_OPEN) != (i == 0))
      return false;
    uint8_t bytes[Benchmark::RECORD_BYTES];
    Benchmark::encodeRecord(record, i, bytes);
    if (file.write(bytes, sizeof(bytes)) != sizeof(bytes)) return false;
  }
  return true;
}
bool loadWalk(WalkEvidence& evidence) {
  uint8_t bytes[WALK_RECORD_BYTES];
  HalFile file = Storage.open(WALK_RESULT);
  return file && file.size() == sizeof(bytes) && file.read(bytes, sizeof(bytes)) == sizeof(bytes) &&
         decodeWalk(bytes, sizeof(bytes), evidence);
}
bool storeWalk() {
  uint8_t bytes[WALK_RECORD_BYTES];
  encodeWalk(walk.evidence, bytes);
  {
    HalFile file;
    if (!Storage.openFileForWrite("VISUAL", WALK_RESULT, file) || file.write(bytes, sizeof(bytes)) != sizeof(bytes))
      return false;
  }
  uint8_t actual[WALK_RECORD_BYTES];
  HalFile verify = Storage.open(WALK_RESULT);
  return verify && verify.size() == sizeof(actual) && verify.read(actual, sizeof(actual)) == sizeof(actual) &&
         memcmp(bytes, actual, sizeof(bytes)) == 0;
}
bool storeImageDiagnostics() {
  uint8_t bytes[epub_image_diagnostics::RECORD_BYTES];
  epub_image_diagnostics::encode(epub_image_diagnostics::active.value, bytes);
  {
    HalFile file;
    if (!Storage.openFileForWrite("VISUAL", WALK_IMAGES, file) || file.write(bytes, sizeof(bytes)) != sizeof(bytes))
      return false;
  }
  uint8_t actual[epub_image_diagnostics::RECORD_BYTES];
  HalFile verify = Storage.open(WALK_IMAGES);
  return verify && verify.size() == sizeof(actual) && verify.read(actual, sizeof(actual)) == sizeof(actual) &&
         memcmp(bytes, actual, sizeof(bytes)) == 0;
}
bool storePaintDiagnostics() {
  uint8_t bytes[epub_paint_diagnostics::RECORD_BYTES];
  epub_paint_diagnostics::encode(epub_paint_diagnostics::active.value, bytes);
  {
    HalFile file;
    if (!Storage.openFileForWrite("VISUAL", WALK_PAINT, file) || file.write(bytes, sizeof(bytes)) != sizeof(bytes))
      return false;
  }
  uint8_t actual[epub_paint_diagnostics::RECORD_BYTES];
  HalFile verify = Storage.open(WALK_PAINT);
  return verify && verify.size() == sizeof(actual) && verify.read(actual, sizeof(actual)) == sizeof(actual) &&
         memcmp(bytes, actual, sizeof(bytes)) == 0;
}
bool writeTicket(const char* path, uint32_t run) {
  HalFile file;
  return writeTicketTo(Storage, path, file, run);
}
uint32_t readTicket(const char* path) {
  Ticket ticket{};
  HalFile file = Storage.open(path);
  if (!file || file.size() != sizeof(ticket) || file.read(&ticket, sizeof(ticket)) != sizeof(ticket)) return 0;
  return decodeTicket(&ticket, sizeof(ticket));
}
bool hasPaintArtifacts() {
  return Storage.exists(WALK_PAINT_RUN) || Storage.exists(WALK_PAINT) ||
         paintEvidenceRequired(readTicket(ACTION), false, false);
}
bool paintCleanupReady(uint32_t run) {
  epub_paint_diagnostics::Snapshot paint;
  const bool validComplete = loadPaintDiagnostics(run, paint) && paint.complete;
  return paintAllowsCleanup(readTicket(ACTION), Storage.exists(WALK_PAINT_RUN), Storage.exists(WALK_PAINT),
                            validComplete);
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
bool saveHome(uint32_t run, bool returned) {
  const char* meta = returned ? "/.pocket/dev-returned-home.run" : "/.pocket/dev-home.run";
  if (Storage.exists(meta) && !Storage.remove(meta)) return false;
  return run && !display.isInverted() &&
         ScreenshotUtil::saveFramebufferAsBmp(returned ? RETURNED_HOME_PATH : HOME_PATH, display.getFrameBuffer(),
                                              display.getDisplayWidth(), display.getDisplayHeight()) &&
         writeTicket(meta, run);
}
uint32_t homeRun(bool returned) {
  return readTicket(returned ? "/.pocket/dev-returned-home.run" : "/.pocket/dev-home.run");
}
bool removeHome(uint32_t run, bool returned) {
  return run && homeRun(returned) == run && Storage.remove(returned ? RETURNED_HOME_PATH : HOME_PATH) &&
         Storage.remove(returned ? "/.pocket/dev-returned-home.run" : "/.pocket/dev-home.run");
}
bool saveLayoutInputs(uint32_t run, bool sleep, bool returned) {
  // Developer-only cold SD evidence: <=8KiB encoded bytes and ~6KiB inputs,
  // request scoped on a radio-free Home, never a second framebuffer.
  auto inputs = makeUniqueNoThrow<Preview::LayoutInputs>();
  auto bytes = makeUniqueNoThrow<uint8_t[]>(Preview::MAX_BYTES);
  if (!run || !inputs || !bytes || !Preview::captureLayoutInputs(renderer, *inputs)) {
    LOG_ERR("VISUAL", "Could not capture actual layout inputs");
    return false;
  }
  const size_t count = Preview::encodeLayoutInputs(*inputs, bytes.get(), Preview::MAX_BYTES);
  HalFile file;
  return count &&
         Storage.openFileForWrite("VISUAL",
                                  returned ? RETURNED_HOME_INPUTS
                                  : sleep  ? SLEEP_INPUTS
                                           : HOME_INPUTS,
                                  file) &&
         file.write(bytes.get(), count) == count;
}
const WakeEvidence& wakeEvidence() { return wake; }
bool saveWake(uint32_t run, uint32_t refreshMs, bool skipped) {
  wake = {};
  if (!run || display.isInverted() ||
      !ScreenshotUtil::saveFramebufferAsBmp(WAKE_PATH, display.getFrameBuffer(), display.getDisplayWidth(),
                                            display.getDisplayHeight()))
    return false;
  wake = {run, refreshMs, skipped};
  return true;
}
bool removeWake(uint32_t run) {
  if (!run || wake.run != run || !Storage.remove(WAKE_PATH)) return false;
  wake = {};
  return true;
}
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
std::string exchangeFixture() {
  const uint32_t run = readTicket(FIXTURE);
  if (!run) return {};
  char path[64];
  bookPath(path, sizeof(path), run);
  return Storage.exists(path) ? std::string(path) : std::string();
}
bool clearFixture(uint32_t run) {
  if (!run || activeRun) return false;
  if (hasLibraryArtifacts()) return clearLibrary(run);
  WalkEvidence evidence;
  if (loadWalk(evidence) && evidence.run == run) {
    if (readTicket(REQUEST) && readTicket(REQUEST) != run) return false;
    // Never erase a failed diagnostic write to make the next trial look clean.
    // Legacy completed walks have neither an image record nor a request ticket.
    if (Storage.exists(WALK_IMAGES_RUN) || Storage.exists(WALK_IMAGES) || readTicket(REQUEST)) {
      epub_image_diagnostics::Snapshot images;
      if (!loadImageDiagnostics(run, images) || !images.complete) return false;
    }
    if (!paintCleanupReady(run)) return false;
    for (const auto* path : {WALK_PERFORMANCE, WALK_RESULT, WALK_PAGES, WALK_ERROR, WALK_ERROR_RUN, WALK_IMAGES,
                             WALK_IMAGES_RUN, WALK_PAINT, WALK_PAINT_RUN, ACTION, REQUEST})
      if (Storage.exists(path) && !Storage.remove(path)) return false;
    return true;
  }
  // A corrupt terminal write is still owned by its surviving checked request.
  if (readTicket(REQUEST) == run && isWalkActionTicket(readTicket(ACTION))) {
    // A torn admission may have no image marker yet. Absence is unknown,
    // never permission to discard the surviving checked request.
    epub_image_diagnostics::Snapshot images;
    if (!loadImageDiagnostics(run, images) || !images.complete) return false;
    if (!paintCleanupReady(run)) return false;
    for (const auto* path : {WALK_PERFORMANCE, WALK_RESULT, WALK_PAGES, WALK_ERROR, WALK_ERROR_RUN, WALK_IMAGES,
                             WALK_IMAGES_RUN, WALK_PAINT, WALK_PAINT_RUN, ACTION, REQUEST})
      if (Storage.exists(path) && !Storage.remove(path)) return false;
    return true;
  }
  if (hasPaintArtifacts() || readTicket(FIXTURE) != run) return false;
  if (Storage.exists(ACTION) && !Storage.remove(ACTION)) return false;
  if (Storage.exists(SUCCESS) && !Storage.remove(SUCCESS)) return false;
  return Storage.remove(FIXTURE);
}
bool continuitySucceeded(uint32_t run) { return run && readTicket(SUCCESS) == run; }
bool acceptOffer() { return activeRun && readerAction == ReaderAction::Accept && readerPhase == ReaderPhase::Reading; }
void offerAccepted() {
  readerPhase = ReaderPhase::Accepted;
  started = millis();
}
bool turnPage() {
  if (readerWalkActive()) {
    RenderLock lock(RenderLock::Mode::Try);
    return lock.ownsLock() && walk.takeTurn(millis());
  }
  if (!activeRun || readerAction != ReaderAction::Advance || readerPhase != ReaderPhase::Reading ||
      millis() - started < 10000)
    return false;
  readerPhase = ReaderPhase::Turned;
  started = millis();
  return true;
}
ReaderReadiness readerReadiness(uint32_t run) {
  char path[64];
  bookPath(path, sizeof(path), run);
  HalFile book = Storage.open(path);
  WalkEvidence stored;
  const bool valid = loadWalk(stored);
  return {activeRun,
          readTicket(REQUEST),
          readTicket(FIXTURE),
          run && book && !book.isDirectory() && validBookSize(book.size()),
          Storage.exists(WALK_RESULT) || Storage.exists(WALK_IMAGES_RUN) || Storage.exists(WALK_IMAGES) ||
              hasPaintArtifacts() || hasLibraryArtifacts() || Storage.exists(WALK_PERFORMANCE),
          valid ? stored.run : 0};
}
bool requestReader(uint32_t run, ReaderAction action, uint32_t pages) {
  char path[64];
  bookPath(path, sizeof(path), run);
  if (!run || activeRun || Storage.exists(REQUEST) || hasPaintArtifacts() || hasLibraryArtifacts() ||
      Storage.exists(WALK_PERFORMANCE))
    return false;
  {
    HalFile book = Storage.open(path);
    if (!book || book.isDirectory() || !validBookSize(book.size())) return false;
  }
  if (action == ReaderAction::Walk) {
    if (!validWalkPages(pages) || Storage.exists(FIXTURE) || Storage.exists(WALK_RESULT) ||
        Storage.exists(WALK_IMAGES_RUN) || Storage.exists(WALK_IMAGES) || !writeTicket(WALK_PAINT_RUN, run) ||
        readTicket(WALK_PAINT_RUN) != run || !writeTicket(WALK_PAGES, pages) ||
        !writeTicket(ACTION, PAINT_WALK_ACTION) || !writeTicket(REQUEST, run) || readTicket(REQUEST) != run)
      return false;
    if (!walk.begin(run, pages, millis()) || !storeWalk() || !writeTicket(WALK_IMAGES_RUN, run)) return false;
    for (const auto* stale : {WALK_ERROR, WALK_ERROR_RUN})
      if (Storage.exists(stale) && !Storage.remove(stale)) return false;
  } else if (pages) {
    return false;
  }
  if (Storage.exists(META) && !Storage.remove(META)) return false;
  if (Storage.exists(SUCCESS) && !Storage.remove(SUCCESS)) return false;
  if (action == ReaderAction::Walk) {
    if (!writeTicket(ACTION, PAINT_WALK_ACTION)) return false;
  } else if (action != ReaderAction::Capture) {
    const uint32_t previous = readTicket(FIXTURE);
    if ((previous && previous != run) || !writeTicket(FIXTURE, run) ||
        !writeTicket(ACTION, static_cast<uint32_t>(action)))
      return false;
  } else if (Storage.exists(ACTION) && !Storage.remove(ACTION)) {
    return false;
  }
  if (writeTicket(REQUEST, run) && readTicket(REQUEST) == run && armBoot('R')) return true;
  if (action == ReaderAction::Walk) {
    walk.evidence.state = WalkState::Interrupted;
    if (!storeWalk()) LOG_ERR("VISUAL", "Could not persist rejected walk request");
  } else {
    Storage.remove(REQUEST);
  }
  Storage.remove(DEV_BOOT_FILE_TRANSFER_MARKER);
  return false;
}
bool beginReader() {
  const uint32_t run = readTicket(REQUEST);
  if (!run || !armBoot('S')) return false;  // recovery route before book parsing
  const uint32_t action = readTicket(ACTION);
  if (action > LIBRARY_ACTION_TICKET) return false;
  if (hasLibraryArtifacts() || action == LIBRARY_ACTION_TICKET) {
    if (action != LIBRARY_ACTION_TICKET || !loadLibrary(run) || libraryEvidence.phase != LibraryPhase::Admitted ||
        libraryEvidence.pages != readTicket(WALK_PAGES))
      return false;
    activeLibrary = true;
  }
  if (isWalkActionTicket(action) || Storage.exists(WALK_RESULT) || Storage.exists(WALK_IMAGES_RUN) ||
      Storage.exists(WALK_IMAGES) || hasPaintArtifacts()) {
    WalkEvidence pending;
    const uint32_t pages = readTicket(WALK_PAGES);
    // A corrupt/missing ACTION must never downgrade a history-isolated walk
    // into ordinary Capture. Cross-check every persisted admission field.
    if (!isWalkActionTicket(action) || readTicket(WALK_IMAGES_RUN) != run ||
        (paintEvidenceRequired(action, false, false) ? readTicket(WALK_PAINT_RUN) != run : hasPaintArtifacts()) ||
        !loadWalk(pending) || !canBeginWalk(run, pages, pending) || !walk.begin(run, pages, millis()) || !storeWalk())
      return false;
    epub_image_diagnostics::active.begin(run, SETTINGS.imageRendering);
    if (!storeImageDiagnostics()) return false;
    paintRequired = paintEvidenceRequired(action, false, false);
    if (paintRequired) {
      epub_paint_diagnostics::active.begin(run);
      if (!storePaintDiagnostics()) return false;
    }
  }
  activeRun = run;
  readerAction = isWalkActionTicket(action) ? ReaderAction::Walk : static_cast<ReaderAction>(action);
  readerPhase = ReaderPhase::Reading;
  started = millis();
  char path[64];
  bookPath(path, sizeof(path), run);
  if (activeLibrary) {
    bookPath(ownedLibraryPath, sizeof(ownedLibraryPath), run);
    activityManager.goToLibrary();
  } else {
    activityManager.goToReader(path);
  }
  return true;
}
bool recoverReader() {
  if (!readTicket(REQUEST)) return false;
  if (hasLibraryArtifacts()) {
    const uint32_t run = readTicket(REQUEST);
    if (readTicket(ACTION) != LIBRARY_ACTION_TICKET || !loadLibrary(run)) return true;
    if (!libraryTerminal(libraryEvidence.phase)) {
      libraryEvidence.phase = LibraryPhase::Failed;
      libraryEvidence.error = LibraryError::Interrupted;
      storeLibrary();
    }
    // An interrupted Library request is never replayed or silently erased.
    return true;
  }
  // A lost action cannot turn unknown walk ownership into ordinary Capture.
  if ((Storage.exists(WALK_RESULT) || hasPaintArtifacts()) && !isWalkActionTicket(readTicket(ACTION))) return true;
  // A pre-armed LAN return after a crash/timeout must not repeat the open.
  WalkEvidence previous;
  const bool validWalk = loadWalk(previous);
  if (!walkRecoveryMatches(readTicket(ACTION), readTicket(REQUEST), validWalk, previous.run)) return true;
  if (validWalk && previous.run == readTicket(REQUEST) && previous.state == WalkState::Running) {
    walk.evidence = previous;
    walk.evidence.state = WalkState::Interrupted;
    if (!storeWalk()) {
      LOG_ERR("VISUAL", "Could not persist interrupted reader walk");
      return true;  // Keep the checked request for explicit owned cleanup.
    }
  }
  // Newly admitted walks require a terminal image record even when its file
  // or marker write was interrupted. Keep the checked request on unknown data.
  if (isWalkActionTicket(readTicket(ACTION)) || Storage.exists(WALK_IMAGES_RUN) || Storage.exists(WALK_IMAGES)) {
    epub_image_diagnostics::Snapshot images;
    if (!loadImageDiagnostics(readTicket(REQUEST), images) || !images.complete) return true;
  }
  if (paintEvidenceRequired(readTicket(ACTION), Storage.exists(WALK_PAINT_RUN), Storage.exists(WALK_PAINT)) &&
      !paintCleanupReady(readTicket(REQUEST)))
    return true;
  Storage.remove(REQUEST);
  return true;
}
bool loadPaintDiagnostics(uint32_t run, epub_paint_diagnostics::Snapshot& out) {
  if (!run || readTicket(WALK_PAINT_RUN) != run) return false;
  uint8_t bytes[epub_paint_diagnostics::RECORD_BYTES];
  HalFile file = Storage.open(WALK_PAINT);
  return file && file.size() == sizeof(bytes) && file.read(bytes, sizeof(bytes)) == sizeof(bytes) &&
         epub_paint_diagnostics::decode(bytes, sizeof(bytes), run, out);
}
int formatPaintDiagnostics(uint32_t run, char* out, size_t capacity) {
  epub_paint_diagnostics::Snapshot paint;
  return epub_paint_diagnostics::formatJson(out, capacity, loadPaintDiagnostics(run, paint) ? &paint : nullptr);
}
bool loadImageDiagnostics(uint32_t run, epub_image_diagnostics::Snapshot& out) {
  if (!run || readTicket(WALK_IMAGES_RUN) != run) return false;
  uint8_t bytes[epub_image_diagnostics::RECORD_BYTES];
  HalFile file = Storage.open(WALK_IMAGES);
  return file && file.size() == sizeof(bytes) && file.read(bytes, sizeof(bytes)) == sizeof(bytes) &&
         epub_image_diagnostics::decode(bytes, sizeof(bytes), run, out);
}
bool readerActive() { return activeRun != 0; }
bool readerWalkActive() { return activeRun && readerAction == ReaderAction::Walk; }
size_t readWalkPerformance(uint32_t run, uint8_t* out, size_t capacity) {
  WalkEvidence evidence;
  if (!out || activeRun || !loadWalk(evidence) || evidence.run != run || evidence.state != WalkState::Completed ||
      evidence.requested > ReaderBenchmark::MAX_PAGES)
    return 0;
  HalFile file = Storage.open(WALK_PERFORMANCE);
  if (!file || file.size() > capacity || file.size() > ReaderBenchmark::MAX_BYTES) return 0;
  const size_t size = file.size();
  if (size < ReaderBenchmark::HEADER_BYTES || file.read(out, size) != static_cast<int>(size) ||
      !ReaderBenchmark::validHeader(out, size, run, evidence.requested))
    return 0;
  return size;
}
WalkEvidence readerWalkEvidence(uint32_t run) {
  WalkEvidence evidence;
  if (readerWalkActive() && activeRun == run) return walk.evidence;
  if (!loadWalk(evidence) || evidence.run != run) return {};
  return evidence;
}
void readerPageRendered(uint32_t spine, uint32_t page, uint32_t progress) {
  if (readerWalkActive()) walk.observe(spine, page, progress, millis());
}
void readerTurnResult(bool moved) {
  if (!readerWalkActive()) return;
  RenderLock lock;  // Called by loop after pageTurn has released its own lock.
  walk.turnResult(moved);
}
bool readerBuildFailed(const BuildFailureLog::Entry& entry) {
  if (!readerWalkActive()) return false;
  if (walk.evidence.state == WalkState::BuildFailed) return true;
  walk.fail(entry.step, entry.detail, entry.freeHeap, entry.largestBlock, millis());
  {
    uint8_t bytes[BuildFailureLog::RECORD_BYTES];
    HalFile file;
    if (BuildFailureLog::encode(entry, bytes, sizeof(bytes)) != sizeof(bytes) ||
        !Storage.openFileForWrite("VISUAL", WALK_ERROR, file) || file.write(bytes, sizeof(bytes)) != sizeof(bytes)) {
      LOG_ERR("VISUAL", "Could not persist walk build failure");
      return true;  // Never replace the user's original failure record.
    }
  }
  if (!writeTicket(WALK_ERROR_RUN, activeRun)) LOG_ERR("VISUAL", "Could not bind walk build failure");
  return true;
}
bool loadWalkFailure(uint32_t run, BuildFailureLog::Entry& entry) {
  if (!run || readTicket(WALK_ERROR_RUN) != run) return false;
  uint8_t bytes[BuildFailureLog::RECORD_BYTES];
  HalFile file = Storage.open(WALK_ERROR);
  return file && file.size() == sizeof(bytes) && file.read(bytes, sizeof(bytes)) == sizeof(bytes) &&
         BuildFailureLog::decode(bytes, sizeof(bytes), entry);
}
void readerLoop() {
  if (!activeRun) return;
  if (activeLibrary && libraryEvidence.phase != LibraryPhase::Reading) {
    RenderLock lock(RenderLock::Mode::Try);
    if (!lock.ownsLock()) return;
    if (!libraryTerminal(libraryEvidence.phase) && millis() - started < 90000) return;
    if (!libraryTerminal(libraryEvidence.phase)) libraryFailed(LibraryError::Budget);
    walk.evidence.state = WalkState::Interrupted;
    walk.evidence.captureReady = 0;
    epub_image_diagnostics::active.value.complete = 1;
    epub_paint_diagnostics::active.value.complete = 1;
    storeWalk();
    storeImageDiagnostics();
    storePaintDiagnostics();
    storeLibrary();
    Storage.prepareForDeepSleep();
    ESP.restart();
    return;
  }
  if (readerWalkActive()) {
    RenderLock lock(RenderLock::Mode::Try);
    if (!lock.ownsLock()) return;
    const bool onReader = activityManager.isReaderActivity();
    if (!onReader) walk.evidence.state = WalkState::Interrupted;
    walk.tick(millis());
    if (walk.evidence.state == WalkState::Running) return;
    walk.evidence.captureReady = onReader && save(activeRun) ? 1 : 0;
    if (!walk.evidence.captureReady && walk.evidence.state == WalkState::Completed)
      walk.evidence.state = WalkState::CaptureFailed;
    epub_image_diagnostics::active.value.complete = 1;
    const bool imagesStored = storeImageDiagnostics();
    bool paintStored = true;
    if (paintRequired) {
      epub_paint_diagnostics::active.value.complete = 1;
      paintStored = storePaintDiagnostics();
    }
    bool libraryStored = true;
    if (activeLibrary) {
      libraryEvidence.phase =
          walk.evidence.state == WalkState::Completed ? LibraryPhase::Completed : LibraryPhase::Failed;
      if (!libraryPreserved()) {
        libraryEvidence.phase = LibraryPhase::Failed;
        libraryEvidence.error = LibraryError::Preservation;
      }
      libraryStored = storeLibrary();
    }
    const bool performanceStored = storeWalkPerformance();
    if (storeWalk() && imagesStored && paintStored && libraryStored && performanceStored) {
      Storage.remove(REQUEST);
    } else {
      LOG_ERR("VISUAL", "Could not persist reader walk diagnostics; keeping recovery request");
    }
    Storage.prepareForDeepSleep();
    // Like Capture, do not destroy the reader or publish fixture progress.
    ESP.restart();
    return;
  }
  if (readerPhase == ReaderPhase::Closing) {
    // Replacement has destroyed the EPUB activity and recorded its real page.
    if (activityManager.isReaderActivity()) return;
    writeTicket(SUCCESS, activeRun);
    Storage.remove(REQUEST);
    Storage.prepareForDeepSleep();
    ESP.restart();
    return;
  }
  bool done = false;
  bool finished = false;
  {
    RenderLock lock(RenderLock::Mode::Try);
    if (lock.ownsLock()) {
      const auto info = activityManager.getScreenshotInfo();
      // rememberBookOnceRendered normally persists APP_STATE; developer fixture
      // rendering bypasses that mutation but still uses the real EPUB activity.
      if (readyToCapture(readerAction, readerPhase) && info.readerType == ScreenshotInfo::ReaderType::Epub &&
          info.pageRendered && info.currentPage > 0 && info.totalPages > 0 && millis() - started > 10000) {
        finished = true;
        done = save(activeRun);
      }
    }
  }
  if (!finished && millis() - started < 90000) return;
  if (!done) LOG_ERR("VISUAL", "Reader capture timed out; returning to Same Wi-Fi");
  if (done && readerAction != ReaderAction::Capture) {
    readerPhase = ReaderPhase::Closing;
    activityManager.goHome();
    return;
  }
  // A synthetic book only. Do not run an exit that publishes synthetic progress.
  // The original book, recents and APP_STATE were never modified.
  Storage.remove(REQUEST);
  Storage.prepareForDeepSleep();
  ESP.restart();
}
namespace {
constexpr const char* BACKUP_SELECTORS[] = {"state", "recent", "settings", "index", "dirty"};
bool hashFile(const char* path, uint32_t cap, LibraryBackupInfo& out) {
  out = {};
  if (!Storage.exists(path)) return true;
  HalFile file = Storage.open(path);
  if (!file || file.isDirectory() || file.size() > cap) return false;
  out.present = true;
  out.bytes = file.size();
  mbedtls_sha256_init(&libraryHashContext);
  bool ok = mbedtls_sha256_starts(&libraryHashContext, 0) == 0;
  const uint32_t began = millis();
  for (uint32_t offset = 0; ok && offset < out.bytes;) {
    const size_t want = std::min<size_t>(sizeof(libraryHashBuffer), out.bytes - offset);
    ok = file.read(libraryHashBuffer, want) == static_cast<int>(want) &&
         mbedtls_sha256_update(&libraryHashContext, libraryHashBuffer, want) == 0 && millis() - began < 45000;
    offset += want;
    if ((offset & 4095u) == 0) delay(1);
  }
  if (ok) ok = mbedtls_sha256_finish(&libraryHashContext, out.sha) == 0;
  mbedtls_sha256_free(&libraryHashContext);
  return ok;
}
void digestHex(const uint8_t* digest, char* output) {
  static constexpr char HEX_DIGITS[] = "0123456789abcdef";
  for (size_t i = 0; i < 32; ++i) {
    output[i * 2] = HEX_DIGITS[digest[i] >> 4];
    output[i * 2 + 1] = HEX_DIGITS[digest[i] & 15];
  }
  output[64] = 0;
}
bool backupDigest(uint8_t* output, bool preservedOnly) {
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  bool ok = mbedtls_sha256_starts(&context, 0) == 0;
  for (unsigned i = 0; ok && i < (preservedOnly ? 3u : 4u); ++i) {
    LibraryBackupInfo info;
    ok = libraryBackupInfo(BACKUP_SELECTORS[i], info);
    const uint8_t header[] = {static_cast<uint8_t>(i),
                              static_cast<uint8_t>(info.present),
                              static_cast<uint8_t>(info.bytes),
                              static_cast<uint8_t>(info.bytes >> 8),
                              static_cast<uint8_t>(info.bytes >> 16),
                              static_cast<uint8_t>(info.bytes >> 24)};
    ok = ok && mbedtls_sha256_update(&context, header, sizeof(header)) == 0 &&
         mbedtls_sha256_update(&context, info.sha, sizeof(info.sha)) == 0;
  }
  if (ok) ok = mbedtls_sha256_finish(&context, output) == 0;
  mbedtls_sha256_free(&context);
  return ok;
}
bool storeLibrary() {
  libraryEvidence.check = libraryChecksum(libraryEvidence);
  {
    HalFile file;
    if (!Storage.openFileForWrite("LIBTRIAL", LIBRARY_RECORD, file) ||
        file.write(reinterpret_cast<const uint8_t*>(&libraryEvidence), sizeof(libraryEvidence)) !=
            sizeof(libraryEvidence))
      return false;
  }
  HalFile verify = Storage.open(LIBRARY_RECORD);
  if (!verify || verify.size() != sizeof(libraryEvidence)) return false;
  uint8_t bytes[64];
  const auto* expected = reinterpret_cast<const uint8_t*>(&libraryEvidence);
  for (size_t offset = 0; offset < sizeof(libraryEvidence); offset += sizeof(bytes)) {
    const size_t count = std::min(sizeof(bytes), sizeof(libraryEvidence) - offset);
    if (verify.read(bytes, count) != static_cast<int>(count) || memcmp(bytes, expected + offset, count)) return false;
  }
  return true;
}
bool loadLibrary(uint32_t run) {
  if (readTicket(LIBRARY_RUN) != run) return false;
  HalFile file = Storage.open(LIBRARY_RECORD);
  return file && file.size() == sizeof(libraryEvidence) &&
         file.read(&libraryEvidence, sizeof(libraryEvidence)) == sizeof(libraryEvidence) &&
         validLibraryEvidence(libraryEvidence, run);
}
bool hasLibraryArtifacts() {
  return readTicket(ACTION) == LIBRARY_ACTION_TICKET || Storage.exists(LIBRARY_RUN) || Storage.exists(LIBRARY_RECORD) ||
         Storage.exists(LIBRARY_ORIGINAL_RUN) || Storage.exists(LIBRARY_OWNED_RUN) ||
         Storage.exists(LIBRARY_ORIGINAL_PATH) || Storage.exists(LIBRARY_OWNED_PATH);
}
bool libraryPreserved() {
  uint8_t actual[32];
  return backupDigest(actual, true) && memcmp(actual, libraryEvidence.preservedSha, sizeof(actual)) == 0;
}
bool indexHasPath(library::LibraryIndexFile& index, const char* wanted) {
  std::string path;
  path.reserve(256);  // Cold bounded scan; reuse one path, never retain a row list.
  library::ClixRecord record{};
  for (uint16_t i = 0; i < index.bookCount(); ++i) {
    if (!index.readRecord(i, record) || !index.readPath(record, path)) return false;
    if (path == wanted) return true;
    if ((i & 31) == 0) delay(1);
  }
  return false;
}
// Keep independent bounded cold stages out of callers' already busy stack
// frames. Each helper owns its file/EPUB handle and returns before the next.
__attribute__((noinline)) bool libraryWalkReadyForCleanup(uint32_t run) {
  if (!libraryRequestMatches(Storage.exists(REQUEST), readTicket(REQUEST), run) ||
      readTicket(ACTION) != LIBRARY_ACTION_TICKET || readTicket(WALK_PAGES) != libraryEvidence.pages ||
      Storage.exists(WALK_ERROR) || Storage.exists(WALK_ERROR_RUN) || !paintCleanupReady(run))
    return false;
  WalkEvidence evidence;
  epub_image_diagnostics::Snapshot images;
  return loadWalk(evidence) && evidence.run == run && evidence.state == WalkState::Completed &&
         evidence.requested == libraryEvidence.pages && loadImageDiagnostics(run, images) && images.complete;
}
__attribute__((noinline)) bool removeOwnedLibraryBook() {
  LibraryBackupInfo owned;
  if (!hashFile(ownedLibraryPath, 4 * 1024 * 1024, owned) || !owned.present || owned.bytes != libraryEvidence.bytes ||
      memcmp(owned.sha, libraryEvidence.sourceSha, 32))
    return false;
  const Epub book(ownedLibraryPath, "/.crosspoint");
  return book.clearCache() && Storage.remove(ownedLibraryPath) && library::markLibraryIndexDirty();
}
__attribute__((noinline)) bool rebuildLibraryAfterCleanup() {
  if (!library::beginDevBuildBudget(4096, 131072, 45000)) return false;
  library::BuildStats stats;
  const bool rebuilt = library::buildLibraryIndex("/", stats, SETTINGS.libraryUseMetadata != 0);
  const auto budget = library::endDevBuildBudget();
  library::LibraryIndexFile index;
  return rebuilt && !budget.exhausted && index.open(library::libraryIndexPath()) &&
         !indexHasPath(index, ownedLibraryPath) && !index.ioFailed() &&
         indexHasPath(index, libraryEvidence.reference) && libraryPreserved();
}
__attribute__((noinline)) bool libraryCacheAbsent(const char* path) {
  const Epub book(path, "/.crosspoint");
  return !Storage.exists(book.getCachePath().c_str());
}
bool clearLibrary(uint32_t run) {
  if (readTicket(ACTION) != LIBRARY_ACTION_TICKET || !loadLibrary(run) ||
      libraryEvidence.phase != LibraryPhase::Completed || libraryEvidence.error != LibraryError::None ||
      libraryEvidence.flags != LIBRARY_FRAMES || libraryEvidence.opened != 1 || !libraryPreserved() ||
      !libraryWalkReadyForCleanup(run))
    return false;
  libraryEvidence.phase = LibraryPhase::Cleaning;
  if (!storeLibrary()) return false;  // Ambiguous DELETE is observed, never replayed.
  bookPath(ownedLibraryPath, sizeof(ownedLibraryPath), run);
  if (!removeOwnedLibraryBook() || !rebuildLibraryAfterCleanup()) return false;
  libraryEvidence.phase = LibraryPhase::Cleaned;
  if (!storeLibrary()) return false;
  // Keep the checked Library result and its frames until a separate explicit
  // acknowledgement; ordinary reader DELETE can never erase failure evidence.
  return true;
}
}  // namespace

const char* libraryBackupPath(std::string_view selector) {
  if (selector == "state") return CrossPointState::getFilePath();
  if (selector == "recent") return RecentBooksStore::getFilePath();
  if (selector == "settings") return CrossPointSettings::getFilePath();
  if (selector == "index") return library::libraryIndexPath();
  if (selector == "dirty") return "/.crosspoint/library.dirty";
  return nullptr;
}
bool libraryBackupInfo(std::string_view selector, LibraryBackupInfo& info) {
  const char* path = libraryBackupPath(selector);
  return path && hashFile(path, selector == "index" ? 8 * 1024 * 1024 : selector == "dirty" ? 1024 : 65536, info);
}
int formatLibraryBackup(uint32_t run, char* out, size_t capacity) {
  uint8_t digest[32];
  char hex[65];
  if (!backupDigest(digest, false)) return -1;
  digestHex(digest, hex);
  int count = snprintf(out, capacity, "{\"version\":1,\"run\":%lu,\"backupDigest\":\"%s\",\"files\":[",
                       static_cast<unsigned long>(run), hex);
  if (count < 0 || static_cast<size_t>(count) >= capacity) return -1;
  for (unsigned i = 0; i < 5; ++i) {
    LibraryBackupInfo info;
    if (!libraryBackupInfo(BACKUP_SELECTORS[i], info)) return -1;
    digestHex(info.sha, hex);
    const int n =
        snprintf(out + count, capacity - count,
                 "%s{\"selector\":\"%s\",\"present\":%s,\"bytes\":%lu,\"sha256\":\"%s\"}", i ? "," : "",
                 BACKUP_SELECTORS[i], info.present ? "true" : "false", static_cast<unsigned long>(info.bytes), hex);
    if (n < 0 || static_cast<size_t>(count + n + 3) > capacity) return -1;
    count += n;
  }
  memcpy(out + count, "]}", 3);
  return count + 2;
}
int formatLibraryEvidence(uint32_t run, char* out, size_t capacity) {
  const bool present = hasLibraryArtifacts();
  const bool valid = activeLibrary ? libraryEvidence.run == run : loadLibrary(run);
  if (!valid)
    return snprintf(out, capacity, "{\"version\":1,\"present\":%s,\"valid\":false}", present ? "true" : "false");
  return snprintf(
      out, capacity,
      "{\"version\":1,\"present\":true,\"valid\":true,\"run\":%lu,\"phase\":%lu,\"error\":%lu,\"flags\":%lu,"
      "\"openCount\":%lu,\"originalRow\":%lu,\"ownedRow\":%lu,\"elapsedMs\":%lu,\"originalCapture\":%s,"
      "\"ownedCapture\":%s,\"derivedIndexMayChange\":true}",
      static_cast<unsigned long>(run), static_cast<unsigned long>(libraryEvidence.phase),
      static_cast<unsigned long>(libraryEvidence.error), static_cast<unsigned long>(libraryEvidence.flags),
      static_cast<unsigned long>(libraryEvidence.opened), static_cast<unsigned long>(libraryEvidence.originalRow),
      static_cast<unsigned long>(libraryEvidence.ownedRow), static_cast<unsigned long>(libraryEvidence.elapsed),
      libraryFrameRun(true) == run ? "true" : "false", libraryFrameRun(false) == run ? "true" : "false");
}
bool requestLibrary(uint32_t run, uint32_t pages, uint32_t bytes, std::string_view reference,
                    std::string_view sourceSha, std::string_view titleSha, std::string_view backupSha) {
  if (!run || !validWalkPages(pages) || !validBookSize(bytes) || !validLibraryReference(reference) || activeRun ||
      Storage.exists(REQUEST) || Storage.exists(FIXTURE) || Storage.exists(ACTION) || Storage.exists(WALK_RESULT) ||
      hasPaintArtifacts() || hasLibraryArtifacts() || Storage.exists(WALK_PERFORMANCE) ||
      Storage.exists(WALK_IMAGES_RUN) || Storage.exists(WALK_IMAGES) || Storage.exists(WALK_ERROR_RUN) ||
      Storage.exists(WALK_ERROR) || Storage.exists(WALK_PAGES) || Storage.exists(META) || Storage.exists(PATH) ||
      Storage.exists(SUCCESS) || SETTINGS.libraryUseMetadata != 1)
    return false;
  for (const char* path : {"/.crosspoint/library.new", "/.crosspoint/library.bak", "/.crosspoint/library.stage",
                           "/.crosspoint/library.stage.f", "/.crosspoint/recent.bin"})
    if (Storage.exists(path)) return false;
  for (const auto& book : RECENT_BOOKS.getBooks()) {
    if (book.path.empty() || book.title.empty() || RecentBooksStore::isMissing(book) ||
        book.title.find("\xef\xbf\xbd") != std::string::npos || book.author.find("\xef\xbf\xbd") != std::string::npos)
      return false;
  }
  libraryEvidence = {};
  libraryEvidence.run = run;
  libraryEvidence.phase = LibraryPhase::Admitted;
  libraryEvidence.bytes = bytes;
  libraryEvidence.pages = pages;
  memcpy(libraryEvidence.reference, reference.data(), reference.size());
  if (!decodeLibraryDigest(sourceSha, libraryEvidence.sourceSha) ||
      !decodeLibraryDigest(titleSha, libraryEvidence.titleSha) ||
      !decodeLibraryDigest(backupSha, libraryEvidence.backupSha))
    return false;
  uint8_t actual[32];
  if (!backupDigest(actual, false) || memcmp(actual, libraryEvidence.backupSha, 32) ||
      !backupDigest(libraryEvidence.preservedSha, true))
    return false;
  LibraryBackupInfo info;
  bookPath(ownedLibraryPath, sizeof(ownedLibraryPath), run);
  for (const char* path : {libraryEvidence.reference, ownedLibraryPath})
    if (!hashFile(path, 4 * 1024 * 1024, info) || !info.present || info.bytes != bytes ||
        memcmp(info.sha, libraryEvidence.sourceSha, 32))
      return false;
  if (!libraryCacheAbsent(ownedLibraryPath)) return false;
  // Marker first: every torn admission remains visible and blocks new trials.
  if (!writeTicket(LIBRARY_RUN, run) || readTicket(LIBRARY_RUN) != run || !storeLibrary() ||
      !writeTicket(WALK_PAINT_RUN, run) || !writeTicket(WALK_IMAGES_RUN, run) || !writeTicket(WALK_PAGES, pages) ||
      !writeTicket(ACTION, LIBRARY_ACTION_TICKET) || !writeTicket(REQUEST, run) || readTicket(REQUEST) != run ||
      !walk.begin(run, pages, millis()) || !storeWalk())
    return false;
  return armBoot('R');
}
bool libraryTrialActive() { return activeLibrary && activeRun; }
bool libraryPreparing() { return libraryTrialActive() && libraryEvidence.phase < LibraryPhase::Reading; }
LibraryPhase libraryPhase() { return libraryTrialActive() ? libraryEvidence.phase : LibraryPhase::Absent; }
const char* libraryTargetPath() {
  return libraryEvidence.phase == LibraryPhase::Original ? libraryEvidence.reference : ownedLibraryPath;
}
uint32_t libraryTargetBytes() { return libraryEvidence.bytes; }
void libraryFailed(LibraryError error) {
  if (!libraryTrialActive()) return;
  libraryEvidence.phase = LibraryPhase::Failed;
  libraryEvidence.error = error;
  libraryEvidence.elapsed = millis() - started;
  if (!storeLibrary()) LOG_ERR("LIBTRIAL", "Could not persist failure; retaining request");
}
void libraryEntered(bool validIndex) {
  if (!libraryTrialActive()) return;
  if (libraryEvidence.phase == LibraryPhase::Failed) return;
  if (!validIndex || libraryEvidence.phase != LibraryPhase::Admitted || !libraryPreserved()) {
    libraryFailed(LibraryError::Index);
    return;
  }
  libraryEvidence.phase = LibraryPhase::Original;
  if (!storeLibrary()) libraryFailed(LibraryError::Persistence);
}
bool libraryRowLocated(uint32_t row, std::string_view title) {
  if (!libraryPreparing() ||
      (libraryEvidence.phase != LibraryPhase::Original && libraryEvidence.phase != LibraryPhase::Owned))
    return false;
  uint8_t digest[32];
  if (mbedtls_sha256(reinterpret_cast<const uint8_t*>(title.data()), title.size(), digest, 0) != 0 ||
      memcmp(digest, libraryEvidence.titleSha, sizeof(digest)))
    return false;
  if (libraryEvidence.phase == LibraryPhase::Original) {
    libraryEvidence.originalRow = row;
    libraryEvidence.flags |= ORIGINAL_ROW;
  } else {
    libraryEvidence.ownedRow = row;
    libraryEvidence.flags |= OWNED_ROW;
  }
  return storeLibrary();
}
bool libraryFrameDrawn(int selected, int first, int drawn, int offset, int itemValue) {
  if (!libraryPreparing() || libraryFrameReady() || !libraryRowFullyDrawn(selected, first, drawn, offset, itemValue))
    return false;
  const bool original = libraryEvidence.phase == LibraryPhase::Original;
  if ((!original && libraryEvidence.phase != LibraryPhase::Owned) ||
      selected != static_cast<int>(original ? libraryEvidence.originalRow : libraryEvidence.ownedRow))
    return false;
  const char* path = original ? LIBRARY_ORIGINAL_PATH : LIBRARY_OWNED_PATH;
  const char* meta = original ? LIBRARY_ORIGINAL_RUN : LIBRARY_OWNED_RUN;
  if (display.isInverted() ||
      !ScreenshotUtil::saveFramebufferAsBmp(path, display.getFrameBuffer(), display.getDisplayWidth(),
                                            display.getDisplayHeight()) ||
      !writeTicket(meta, activeRun) || readTicket(meta) != activeRun) {
    libraryFailed(LibraryError::Frame);
    return false;
  }
  libraryEvidence.flags |= original ? ORIGINAL_FRAME : OWNED_FRAME;
  if (!storeLibrary()) {
    libraryFailed(LibraryError::Persistence);
    return false;
  }
  return true;
}
bool libraryFrameReady() {
  const uint32_t flag = libraryEvidence.phase == LibraryPhase::Original ? ORIGINAL_FRAME : OWNED_FRAME;
  return libraryPreparing() && (libraryEvidence.flags & flag);
}
bool libraryNextTarget() {
  if (!libraryPreparing() || libraryEvidence.phase != LibraryPhase::Original || !libraryFrameReady()) return false;
  libraryEvidence.phase = LibraryPhase::Owned;
  return storeLibrary();
}
bool libraryOpenOwned(std::string_view path) {
  if (!libraryCanOpen(libraryEvidence, path == ownedLibraryPath) || !libraryPreserved()) return false;
  LibraryBackupInfo info;
  if (!hashFile(ownedLibraryPath, 4 * 1024 * 1024, info) || !info.present || info.bytes != libraryEvidence.bytes ||
      memcmp(info.sha, libraryEvidence.sourceSha, 32))
    return false;
  libraryEvidence.opened = 1;
  libraryEvidence.elapsed = millis() - started;
  libraryEvidence.phase = LibraryPhase::Reading;
  if (!storeLibrary() || !walk.begin(activeRun, libraryEvidence.pages, millis()) || !storeWalk()) return false;
  return true;
}
uint32_t libraryFrameRun(bool original) { return readTicket(original ? LIBRARY_ORIGINAL_RUN : LIBRARY_OWNED_RUN); }
bool acknowledgeLibrary(uint32_t run) {
  if (activeRun || readTicket(ACTION) != LIBRARY_ACTION_TICKET || !loadLibrary(run) ||
      libraryEvidence.phase != LibraryPhase::Cleaned || !libraryPreserved() || savedRun() != run ||
      libraryFrameRun(true) != run || libraryFrameRun(false) != run || !libraryWalkReadyForCleanup(run))
    return false;
  bookPath(ownedLibraryPath, sizeof(ownedLibraryPath), run);
  if (Storage.exists(ownedLibraryPath)) return false;
  // Result/ownership marker last. A torn acknowledgement still blocks admission.
  for (const char* path :
       {PATH, META, LIBRARY_ORIGINAL_PATH, LIBRARY_ORIGINAL_RUN, LIBRARY_OWNED_PATH, LIBRARY_OWNED_RUN,
        WALK_PERFORMANCE, WALK_RESULT, WALK_PAGES, WALK_ERROR, WALK_ERROR_RUN, WALK_IMAGES, WALK_IMAGES_RUN, WALK_PAINT,
        WALK_PAINT_RUN, REQUEST, ACTION, LIBRARY_RECORD, LIBRARY_RUN})
    if (Storage.exists(path) && !Storage.remove(path)) return false;
  return true;
}
}  // namespace PocketDaily::DevCapture
#endif
