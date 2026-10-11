#include "pocket_daily/web/PocketEndpoints.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <Logging.h>
#include <Memory.h>
#include <WebServer.h>
#include <esp_ota_ops.h>
#include <sys/time.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "CrossPointSettings.h"
#include "RecentBooksStore.h"
#include "articles/ArticleStorage.h"
#include "components/UITheme.h"
#include "network/FirmwareFlasher.h"
#include "pocket_daily/ContentActiveStore.h"
#include "pocket_daily/ContentPathPolicy.h"
#include "pocket_daily/ContentRevisionStore.h"
#include "pocket_daily/ContentSealStore.h"
#include "pocket_daily/PocketGlanceStore.h"
#include "pocket_daily/PocketProfileStore.h"
#include "pocket_daily/PublicationReceipt.h"
#include "pocket_daily/ReadingExchange.h"
#include "pocket_daily/ReadingProgress.h"
#include "pocket_daily/StagedFirmwareStore.h"
#include "pocket_daily/boot/DevBootReturn.h"
#include "pocket_daily/live_studio/DevTrace.h"
#include "pocket_daily/live_studio/HeapMap.h"
#include "pocket_daily/live_studio/NetHealth.h"
#include "pocket_daily/live_studio/StackReport.h"
#include "pocket_daily/nearby_sync/WifiSetup.h"
#include "pocket_daily/staged_firmware.h"
#include "pocket_daily/web/DisplayState.h"
#include "pocket_daily/web/ExactRouteDispatch.h"
#include "pocket_daily/web/GenerationArgument.h"
#include "pocket_daily/web/LegacyFontSize.h"
#include "pocket_daily/web/PocketStatus.h"
#include "pocket_daily/web/PreferencesUpdate.h"
#include "pocket_daily/web/ReaderFilesPolicy.h"
#include "pocket_daily/web/TcpCensus.h"
#include "util/BookCacheUtils.h"
#ifdef ENABLE_DEV_REMOTE_FLASH
#include <HalDisplay.h>
#include <HalPowerManager.h>

#include "activities/RenderLock.h"
#include "pocket_daily/dev/ScreenCapture.h"
#include "pocket_daily/nearby_sync/DevSleepCycle.h"
#include "pocket_daily/nearby_sync/ExchangeWindowPolicy.h"
#include "pocket_daily/product_identity.h"
#include "util/ScreenshotUtil.h"
#endif

namespace PocketDaily::Web {
namespace {

// Keep diagnostic responses below the Pocket private AP's scarce contiguous
// heap and return to the global loop between every piece. The previous 512 B
// write loop could block inside lwIP long enough to trip the task watchdog.
// Each diagnostic request reads one chunk into the shared static staging
// buffer and answers with one bounded socket write.
constexpr size_t CRASH_REPORT_CHUNK_BYTES = 1024;
// A diagnostic chunk send blocks the loop task. Unlike an SD write it can
// hang forever if the peer vanishes, so the loop watchdog must stay armed;
// bounding the socket below the 5 s task-WDT window guarantees the send
// returns (completed or aborted) before the watchdog could fire.
constexpr unsigned long DIAGNOSTIC_SEND_TIMEOUT_MS = 3000;

void note(const RouteDeps& d) {
  if (d.host.noteClientActivity) d.host.noteClientActivity(d.host.self);
}

void repaint(const RouteDeps& d) {
  if (d.host.requestRepaint) d.host.requestRepaint(d.host.self);
}

String norm(const RouteDeps& d, const String& path) {
  return d.host.normalizeWebPath ? d.host.normalizeWebPath(d.host.self, path) : path;
}

bool protectedName(const RouteDeps& d, const String& name) {
  return d.host.isProtectedItemName && d.host.isProtectedItemName(d.host.self, name);
}

bool renameStorageFile(const String& from, const String& to) {
  HalFile file = Storage.open(from.c_str());
  if (!file || file.isDirectory()) {
    if (file) file.close();
    return false;
  }
  const bool renamed = file.rename(to.c_str());
  file.close();
  return renamed;
}

void handleSessionEnd(WebServer& server, const RouteDeps& d) {
  if ((d.host.httpUploadBusy && d.host.httpUploadBusy(d.host.self)) || (d.stream && d.stream->receiving())) {
    server.send(409, "application/json", "{\"error\":\"transfer active\"}");
    return;
  }
  server.send(200, "application/json", "{\"ended\":true}");
  d.host.endDirectSession(d.host.self);
}

void sendPreparation(WebServer& server, const char* deviceId, const char* revision,
                     const Content::PreparedRevision& prepared) {
  char body[192];
  const int count = snprintf(
      body, sizeof(body), "{\"schema\":1,\"deviceID\":\"%s\",\"revision\":\"%s\",\"fileCount\":%u,\"verifiedMask\":%u}",
      deviceId, revision, prepared.manifest.fileCount, prepared.verifiedMask);
  if (count < 0 || static_cast<size_t>(count) >= sizeof(body)) {
    server.send(500, "text/plain", "Preparation response overflow");
    return;
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", body);
}

// `claimedId` is the reader identity the request names (query or body).
bool admitOperationFor(WebServer& server, const RouteDeps& d, char (&deviceId)[9], const char* claimedId) {
  if ((d.host.httpUploadBusy && d.host.httpUploadBusy(d.host.self)) || (d.stream && d.stream->receiving())) {
    server.send(409, "text/plain", "Another file transfer is active");
    return false;
  }
  snprintf(deviceId, sizeof(deviceId), "%08lX", static_cast<unsigned long>(ESP.getEfuseMac() & 0xFFFFFFFFUL));
  if (!claimedId || strcmp(claimedId, deviceId) != 0) {
    server.send(409, "text/plain", "Reader identity mismatch");
    return false;
  }
  // Cold-path validation only; leave headroom for two HAL handles and HTTP.
  // This is an admission guard, not a measured minimum for all hardware.
  if (ESP.getFreeHeap() < 8192 || ESP.getMaxAllocHeap() < 2048) {
    server.send(503, "text/plain", "Reader memory is too low for content verification");
    return false;
  }
  return true;
}

bool admitContentOperation(WebServer& server, const RouteDeps& d, char (&deviceId)[9]) {
  const String claimed = server.arg("deviceID");
  return admitOperationFor(server, d, deviceId, claimed.c_str());
}

bool contentRevisionArgument(WebServer& server, const String& revision) {
  if (revision.length() == 64 && Content::validRevision(revision.c_str())) return true;
  server.send(400, "text/plain", "Invalid content revision");
  return false;
}

void handlePrepareContent(WebServer& server, const RouteDeps& d) {
  char deviceId[9];
  if (!admitContentOperation(server, d, deviceId)) return;
  const String revision = server.arg("revision");
  if (!contentRevisionArgument(server, revision)) return;
  Content::PreparedRevision prepared;
  Content::ActiveRevision active;
  const auto recovered =
      Content::recoverActiveRevision(Content::SUPPORTED_CAPABILITIES, active, HalSystem::feedWatchdogIfRegistered);
  const auto result = Content::prepareStagedRevision(
      revision.c_str(), recovered == Content::ActiveResult::Ok ? active.revision : nullptr,
      Content::SUPPORTED_CAPABILITIES, prepared, HalSystem::feedWatchdogIfRegistered);
  if (result == Content::RevisionResult::CopyFailed) {
    server.send(503, "text/plain", "Reader could not copy or verify staged content. Check the SD card and free space.");
    return;
  }
  if (result != Content::RevisionResult::Ok) {
    server.send(422, "text/plain", "Staged content manifest could not be verified");
    return;
  }
  sendPreparation(server, deviceId, revision.c_str(), prepared);
}

void sendContentState(WebServer& server, const char* deviceId, const Content::ActiveRevision& active) {
  char body[224];
  const int count = active.generation
                        ? snprintf(body, sizeof(body),
                                   "{\"schema\":1,\"deviceID\":\"%s\",\"capabilities\":7,\"active\":{\"revision\":\"%"
                                   "s\",\"generation\":%lu}}",
                                   deviceId, active.revision, static_cast<unsigned long>(active.generation))
                        : snprintf(body, sizeof(body),
                                   "{\"schema\":1,\"deviceID\":\"%s\",\"capabilities\":7,\"active\":null}", deviceId);
  if (count < 0 || static_cast<size_t>(count) >= sizeof(body)) {
    server.send(500, "text/plain", "Content response overflow");
    return;
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", body);
}

void handleContentState(WebServer& server, const RouteDeps& d) {
  char deviceId[9];
  if (!admitContentOperation(server, d, deviceId)) return;
  Content::ActiveRevision active;
  const auto result =
      Content::recoverActiveRevision(Content::SUPPORTED_CAPABILITIES, active, HalSystem::feedWatchdogIfRegistered);
  if (result != Content::ActiveResult::Ok && result != Content::ActiveResult::NoActive) {
    server.send(503, "text/plain", "Active content could not be verified");
    return;
  }
  sendContentState(server, deviceId, active);
}

// Resolved render inputs for the companion preview (docs/pocket-profile-v1.md).
// Same identity/heap admission as content state; configuration reads only.
void handleDisplay(WebServer& server, const RouteDeps& d) {
  char deviceId[9];
  if (!admitContentOperation(server, d, deviceId)) return;
  Content::PageDescription page;
  if (!d.presentation.describe || !d.presentation.describe(d.presentation.self, page)) {
    server.send(503, "text/plain", "Display state is unavailable");
    return;
  }
  DisplayInputs in;
  in.deviceId = deviceId;
  in.theme = themeName(SETTINGS.uiTheme);
  in.orientation = page.orientation;
  in.fontFamily = page.fontFamily;
  in.fontPointSize = page.fontPointSize;
  in.sidePadding = page.style.sidePadding;
  in.topPadding = page.style.topPadding;
  in.spacing = page.style.spacing;
  in.title = page.style.title;
  in.empty = page.style.empty;
  for (int i = 0; i < 4; ++i) in.labels[i] = page.labels[i];
  static constexpr size_t kCap = 1024;  // request-scoped; admission reserved heap
  auto json = makeUniqueNoThrow<char[]>(kCap);
  if (!json) {
    server.send(503, "text/plain", "Reader memory is too low for display state");
    return;
  }
  if (!writeDisplayJson(in, json.get(), kCap)) {
    server.send(500, "text/plain", "Display state could not be encoded");
    return;
  }
  server.send(200, "application/json", json.get());
}

// Pocket Daily profile (docs/pocket-profile-v1.md). Identity/heap gated like
// content operations; POST validates the whole document before persisting.
bool sendProfile(WebServer& server, const char* deviceId) {
  static constexpr size_t kCap = 1024;
  auto json = makeUniqueNoThrow<char[]>(kCap);
  if (!json) {
    server.send(503, "text/plain", "Reader memory is too low for the profile");
    return false;
  }
  if (!DailyProfile::writeJson(DailyProfile::current(), DailyProfile::generation(), deviceId, json.get(), kCap)) {
    server.send(500, "text/plain", "Profile could not be encoded");
    return false;
  }
  server.send(200, "application/json", json.get());
  return true;
}

void handleGetProfile(WebServer& server, const RouteDeps& d) {
  char deviceId[9];
  if (!admitContentOperation(server, d, deviceId)) return;
  sendProfile(server, deviceId);
}

void handlePostProfile(WebServer& server, const RouteDeps& d) {
  char deviceId[9];
  if (!admitContentOperation(server, d, deviceId)) return;
  const String expected = server.arg("generation");
  uint32_t generation = 0;
  if (!parseGeneration({expected.c_str(), expected.length()}, generation)) {
    server.send(400, "text/plain", "Missing or invalid profile generation");
    return;
  }
  const String& body = server.arg("plain");
  DailyProfile::Profile profile;
  const char* error = nullptr;
  if (!DailyProfile::parseJson(body.c_str(), body.length(), profile, error)) {
    server.send(400, "text/plain", error ? error : "Invalid profile");
    return;
  }
  switch (DailyProfile::save(profile, static_cast<uint32_t>(generation))) {
    case DailyProfile::SaveResult::Ok:
      sendProfile(server, deviceId);
      return;
    case DailyProfile::SaveResult::Conflict:
      server.send(409, "text/plain", "The reader's profile changed; reload it before saving");
      return;
    case DailyProfile::SaveResult::Invalid:
      server.send(400, "text/plain", "Invalid profile");
      return;
    case DailyProfile::SaveResult::StorageError:
      break;
  }
  server.send(500, "text/plain", "Profile could not be stored; the previous profile is still in use");
}

// App-provided weather and today's events (docs/pocket-glance-v1.md). Same
// identity and heap admission as the profile; the whole document is validated
// before anything is stored, and the previous glance survives any failure.
void handlePostGlance(WebServer& server, const RouteDeps& d) {
  char deviceId[9];
  if (!admitContentOperation(server, d, deviceId)) return;
  const String& body = server.arg("plain");
  // ~800 B, request-scoped; parse and save allocate only after admission.
  auto snapshot = makeUniqueNoThrow<AppGlance::Snapshot>();
  if (!snapshot) {
    server.send(503, "text/plain", "Reader memory is too low for the glance");
    return;
  }
  const char* error = nullptr;
  bool outOfMemory = false;
  if (!AppGlance::parseJson(body.c_str(), body.length(), *snapshot, error, outOfMemory)) {
    server.send(outOfMemory ? 503 : 400, "text/plain", error ? error : "Invalid glance");
    return;
  }
  switch (AppGlance::save(*snapshot)) {
    case AppGlance::SaveResult::Ok:
      break;
    case AppGlance::SaveResult::Invalid:
      server.send(400, "text/plain", "Invalid glance");
      return;
    case AppGlance::SaveResult::OutOfMemory:
      server.send(503, "text/plain", "Reader memory is too low for the glance");
      return;
    case AppGlance::SaveResult::StorageError:
      server.send(500, "text/plain", "Glance could not be stored; the previous glance is still in use");
      return;
  }
  // The reader has no network clock of its own. An unset clock (cold boot)
  // takes the app's compose time, a lower bound that keeps the daily word and
  // saved-age labels honest; a set clock is never moved.
  if (time(nullptr) < static_cast<time_t>(AppGlance::MIN_EPOCH)) {
    const timeval now{static_cast<time_t>(snapshot->savedEpoch), 0};
    if (settimeofday(&now, nullptr) == 0) LOG_INF("GLANCE", "Clock set from the companion glance");
  }
  char response[96];
  snprintf(response, sizeof(response), "{\"schema\":1,\"deviceID\":\"%s\",\"savedEpoch\":%lu}", deviceId,
           static_cast<unsigned long>(snapshot->savedEpoch));
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", response);
}

void retireContent(const Content::RetiredRevision& retired, const RouteDeps& d) {
  if (!retired.revision[0] || !d.presentation.state) return;
  const auto displayed = d.presentation.state(d.presentation.self);
  const auto cleanup = Content::retireRevision(
      retired.revision, displayed.phase == Content::PresentationPhase::Idle ? "" : displayed.revision,
      Content::SUPPORTED_CAPABILITIES, HalSystem::feedWatchdogIfRegistered);
  if (cleanup != Content::RetirementResult::Ok && cleanup != Content::RetirementResult::Protected)
    LOG_ERR("CONTENT", "Retired content cleanup incomplete: %u", static_cast<unsigned>(cleanup));
}

void handleActivateContent(WebServer& server, const RouteDeps& d) {
  char deviceId[9];
  if (!admitContentOperation(server, d, deviceId)) return;
  const String revision = server.arg("revision");
  if (!contentRevisionArgument(server, revision)) return;
  Content::RevisionInfo sealed;
  if (Content::sealRevision(revision.c_str(), Content::SUPPORTED_CAPABILITIES, sealed,
                            HalSystem::feedWatchdogIfRegistered) != Content::SealResult::Ok) {
    server.send(422, "text/plain", "Content could not be sealed");
    return;
  }
  Content::ActiveRevision active;
  Content::RetiredRevision retired;
  if (Content::activateRevision(revision.c_str(), Content::SUPPORTED_CAPABILITIES, active,
                                HalSystem::feedWatchdogIfRegistered, &retired) != Content::ActiveResult::Ok) {
    server.send(503, "text/plain", "Activation outcome requires a state check");
    return;
  }
  retireContent(retired, d);
  // Stored selection only. No reboot, flash or claim that the screen rendered it.
  sendContentState(server, deviceId, active);
}

// Receipt vocabulary shared by content and screen presentation.
const char* phaseName(const Content::PresentationPhase phase) {
  switch (phase) {
    case Content::PresentationPhase::Idle:
      return "idle";
    case Content::PresentationPhase::Queued:
      return "queued";
    case Content::PresentationPhase::Rendered:
      return "rendered";
    case Content::PresentationPhase::Failed:
      return "failed";
  }
  return "idle";
}

const char* failureName(const Content::PresentationFailure failure) {
  switch (failure) {
    case Content::PresentationFailure::None:
      return "none";
    case Content::PresentationFailure::Memory:
      return "memory";
    case Content::PresentationFailure::Preparation:
      return "preparation";
    case Content::PresentationFailure::Display:
      return "display";
  }
  return "none";
}

void sendPresentation(WebServer& server, const char* deviceId, const Content::PresentationReceipt& receipt) {
  const char* phase = phaseName(receipt.phase);
  const char* failure = failureName(receipt.failure);
  char body[224];
  const int written =
      snprintf(body, sizeof(body),
               "{\"schema\":1,\"deviceID\":\"%s\",\"revision\":\"%s\",\"generation\":%lu,\"phase\":\"%s\",\"failure\":"
               "\"%s\",\"heap\":%lu,\"block\":%lu}",
               deviceId, receipt.revision, static_cast<unsigned long>(receipt.generation), phase, failure,
               static_cast<unsigned long>(receipt.heap), static_cast<unsigned long>(receipt.block));
  if (written <= 0 || static_cast<size_t>(written) >= sizeof(body)) {
    server.send(500, "text/plain", "Presentation response overflow");
    return;
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", body);
}

void handlePresentation(WebServer& server, const RouteDeps& d, bool requestPaint) {
  char deviceId[9];
  snprintf(deviceId, sizeof(deviceId), "%08lX", static_cast<unsigned long>(ESP.getEfuseMac() & 0xFFFFFFFFUL));
  if (server.arg("deviceID") != deviceId) {
    server.send(409, "text/plain", "Reader identity mismatch");
    return;
  }
  const String revision = server.arg("revision");
  if (!contentRevisionArgument(server, revision)) return;
  if (requestPaint) {
    if (!admitContentOperation(server, d, deviceId)) return;
    Content::ActiveRevision active;
    const auto result =
        Content::recoverActiveRevision(Content::SUPPORTED_CAPABILITIES, active, HalSystem::feedWatchdogIfRegistered);
    if (result != Content::ActiveResult::Ok || strcmp(active.revision, revision.c_str()) != 0) {
      server.send(409, "text/plain", "Requested content is not the verified active revision");
      return;
    }
    // Queue metadata only. The unchanged cold rendering admission runs after
    // WebServer releases this request's client/RX buffer and temporary strings.
    if (!d.presentation.prepare(d.presentation.self, revision.c_str(), active.generation)) {
      server.send(503, "text/plain", "Content view could not be queued; stored activation is unchanged");
      return;
    }
  }
  // This is metadata only: never rehash SD or wait for a rendering lock on GET.
  const auto receipt = d.presentation.state(d.presentation.self);
  if (!receipt.generation || strcmp(receipt.revision, revision.c_str()) != 0) {
    server.send(409, "text/plain", "This content revision is not being presented");
    return;
  }
  sendPresentation(server, deviceId, receipt);
}

// Home / Daily Brief presentation (docs/pocket-screen-present-v1.md).
const char* surfaceName(const Screen::Surface surface) {
  switch (surface) {
    case Screen::Surface::Home:
      return "home";
    case Screen::Surface::Brief:
      return "brief";
    case Screen::Surface::None:
      break;
  }
  return "";
}

void sendScreenPresentation(WebServer& server, const char* deviceId, const Screen::Receipt& receipt) {
  char body[192];
  const int written =
      snprintf(body, sizeof(body),
               "{\"schema\":1,\"deviceID\":\"%s\",\"surface\":\"%s\",\"generation\":%lu,\"phase\":\"%s\",\"failure\":"
               "\"%s\",\"heap\":%lu,\"block\":%lu}",
               deviceId, surfaceName(receipt.surface), static_cast<unsigned long>(receipt.generation),
               phaseName(receipt.phase), failureName(receipt.failure), static_cast<unsigned long>(receipt.heap),
               static_cast<unsigned long>(receipt.block));
  if (written <= 0 || static_cast<size_t>(written) >= sizeof(body)) {
    server.send(500, "text/plain", "Presentation response overflow");
    return;
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", body);
}

void handleScreenPresentation(WebServer& server, const RouteDeps& d, bool requestPaint) {
  char deviceId[9];
  snprintf(deviceId, sizeof(deviceId), "%08lX", static_cast<unsigned long>(ESP.getEfuseMac() & 0xFFFFFFFFUL));
  if (server.arg("deviceID") != deviceId) {
    server.send(409, "text/plain", "Reader identity mismatch");
    return;
  }
  const String surfaceArg = server.arg("surface");
  Screen::Surface surface = Screen::Surface::None;
  if (surfaceArg == "home") {
    surface = Screen::Surface::Home;
  } else if (surfaceArg == "brief") {
    surface = Screen::Surface::Brief;
  } else {
    server.send(400, "text/plain", "Invalid screen surface");
    return;
  }
  const String generationArg = server.arg("generation");
  uint32_t generation = 0;
  if (!parseGeneration({generationArg.c_str(), generationArg.length()}, generation)) {
    server.send(400, "text/plain", "Missing or invalid profile generation");
    return;
  }
  if (requestPaint) {
    // Admission as for content: 409 while an upload owns the reader, 503 when
    // memory is below the request floor.
    if (!admitContentOperation(server, d, deviceId)) return;
    if (DailyProfile::generation() != generation) {
      server.send(409, "text/plain", "The reader's profile changed; reload it before presenting");
      return;
    }
    if (surface == Screen::Surface::Brief && DailyProfile::current().sleepMode == DailyProfile::SleepMode::Reader) {
      server.send(409, "text/plain", "Sleep mode is the reader's own screen; there is no Daily Brief to draw");
      return;
    }
    // Queue surface and generation only. Inputs, fonts and the cold rendering
    // admission run after WebServer releases this request's client buffers.
    if (!d.screen.enqueue(d.screen.self, surface, static_cast<uint32_t>(generation))) {
      server.send(503, "text/plain", "Screen could not be queued; another presentation is in progress");
      return;
    }
  }
  // Metadata only: never read SD or wait for the rendering lock on GET.
  const auto receipt = d.screen.state(d.screen.self);
  if (receipt.surface != surface || receipt.generation != generation) {
    server.send(409, "text/plain", "This screen is not being presented");
    return;
  }
  sendScreenPresentation(server, deviceId, receipt);
}

void handleCrashReport(WebServer& server) {
  HalSystem::setCrashBreadcrumb("nearby:crash-chunk");
  if (!diagnosticsAffordable()) {
    server.send(503, "text/plain", "Reader memory is too low for diagnostics right now");
    return;
  }
  HalFile report = Storage.open("/crash_report.txt");
  if (!report || report.isDirectory()) {
    if (report) report.close();
    server.send(404, "text/plain", "No crash report recorded");
    return;
  }

  if (!server.hasArg("offset")) {
    report.close();
    server.send(400, "text/plain", "Missing crash report offset");
    return;
  }

  const String offsetText = server.arg("offset");
  if (offsetText.isEmpty()) {
    report.close();
    server.send(400, "text/plain", "Invalid crash report offset");
    return;
  }
  for (size_t i = 0; i < offsetText.length(); i++) {
    if (offsetText[i] < '0' || offsetText[i] > '9') {
      report.close();
      server.send(400, "text/plain", "Invalid crash report offset");
      return;
    }
  }

  const size_t reportSize = report.size();
  const size_t offset = static_cast<size_t>(offsetText.toInt());
  if (offset >= reportSize || !report.seek(offset)) {
    report.close();
    server.send(416, "text/plain", "Crash report offset out of range");
    return;
  }

  uint8_t* body = firmware_flash::sharedStagingBuffer();
  const size_t remaining = reportSize - offset;
  const size_t requested = std::min(remaining, CRASH_REPORT_CHUNK_BYTES);
  const int count = report.read(body, requested);
  report.close();
  if (count <= 0) {
    server.send(500, "text/plain", "Could not read crash report chunk");
    return;
  }

  server.client().setTimeout(DIAGNOSTIC_SEND_TIMEOUT_MS);
  feedLoopWDT();
  server.setContentLength(static_cast<size_t>(count));
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain; charset=utf-8", "");
  server.sendContent(reinterpret_cast<const char*>(body), static_cast<size_t>(count));
  feedLoopWDT();
  LOG_DBG("WEB", "Crash report chunk offset=%u bytes=%u total=%u", static_cast<unsigned>(offset),
          static_cast<unsigned>(count), static_cast<unsigned>(reportSize));
}

#ifdef ENABLE_DEV_REMOTE_FLASH
// One developer-owned SD image, never a second framebuffer. Only a completed
// Home/Brief presentation can be captured, under the same lock as the painter.
// Bounded download pieces reuse the existing firmware staging buffer.
constexpr const char* kScreenCapturePath = DevCapture::PATH;

// Keep the 88-byte card receipt out of the HTTP/chunk handler's stack frame.
__attribute__((noinline)) bool matchesCapture(WebServer& server, const RouteDeps& d, uint32_t generation) {
  if (server.arg("surface") == "card" && d.presentation.state) {
    const auto receipt = d.presentation.state(d.presentation.self);
    return receipt.phase == Content::PresentationPhase::Rendered && receipt.generation == generation &&
           server.arg("revision") == receipt.revision;
  }
  const auto receipt = d.screen.state(d.screen.self);
  return receipt.phase == Content::PresentationPhase::Rendered && receipt.generation == generation &&
         DailyProfile::generation() == generation && server.arg("surface") == surfaceName(receipt.surface);
}

void handleDevScreenCapture(WebServer& server, const RouteDeps& d) {
  char deviceId[9];
  if (!admitContentOperation(server, d, deviceId)) return;
  uint32_t run = 0;
  const String runArg = server.arg("run");
  if (!parseGeneration({runArg.c_str(), runArg.length()}, run) || !run) {
    server.send(400, "text/plain", "Invalid capture run");
    return;
  }
  if (server.method() == HTTP_POST) {
    uint32_t generation = 0;
    const String generationArg = server.arg("generation");
    if (!parseGeneration({generationArg.c_str(), generationArg.length()}, generation) || !d.screen.state) {
      server.send(400, "text/plain", "Invalid capture generation");
      return;
    }
    RenderLock lock(RenderLock::Mode::Try);
    if (!lock.ownsLock()) {
      server.send(409, "text/plain", "Reader is drawing");
      return;
    }
    if (!matchesCapture(server, d, generation) || display.isInverted()) {
      lock.unlock();
      server.send(409, "text/plain", "No matching completed normal-polarity screen");
      return;
    }
    const bool saved = DevCapture::save(run);
    lock.unlock();
    if (!saved) {
      server.send(500, "text/plain", "Screen capture could not be saved");
      return;
    }
    server.send(204, "text/plain", "");
    return;
  }
  if (DevCapture::savedRun() != run) {
    server.send(409, "text/plain", "Capture run mismatch");
    return;
  }
  if (server.method() == HTTP_DELETE) {
    if (!DevCapture::remove(run)) {
      server.send(500, "text/plain", "Capture cleanup failed");
      return;
    }
    server.send(204, "text/plain", "");
    return;
  }
  uint32_t offset = 0;
  const String offsetArg = server.arg("offset");
  if (!parseGeneration({offsetArg.c_str(), offsetArg.length()}, offset)) {
    server.send(400, "text/plain", "Invalid capture offset");
    return;
  }
  HalFile file = Storage.open(kScreenCapturePath);
  if (!file || offset >= file.size() || !file.seek(offset)) {
    server.send(416, "text/plain", "Capture offset unavailable");
    return;
  }
  const size_t total = file.size();
  const size_t count = std::min(total - static_cast<size_t>(offset), CRASH_REPORT_CHUNK_BYTES);
  uint8_t* body = firmware_flash::sharedStagingBuffer();
  const int read = file.read(body, count);
  file.close();
  if (read != static_cast<int>(count)) {
    server.send(500, "text/plain", "Capture read failed");
    return;
  }
  server.client().setTimeout(DIAGNOSTIC_SEND_TIMEOUT_MS);
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("X-Capture-Run", String(run));
  server.sendHeader("X-Capture-Size", String(static_cast<unsigned>(total)));
  server.setContentLength(count);
  feedLoopWDT();
  server.send(200, "application/octet-stream", "");
  server.sendContent(reinterpret_cast<const char*>(body), count);
  feedLoopWDT();
}
#endif

// docs/reader-files.md "Reader file download": one bounded piece per request.
static_assert(kDownloadPieceSameWifi <= firmware_flash::STAGING_BUFFER_BYTES, "piece must fit the staging buffer");
void handleReaderFileContent(WebServer& server, const RouteDeps& d) {
  HalSystem::setCrashBreadcrumb("nearby:file-piece");
  char deviceId[9];
  if (!admitContentOperation(server, d, deviceId)) return;
  if (d.presentation.busy && d.presentation.busy(d.presentation.self)) {
    server.send(409, "text/plain", "Reader is drawing; retry the download");
    return;
  }
  if (!diagnosticsAffordable()) {
    server.send(503, "text/plain", "Reader memory is too low for the download right now");
    return;
  }
  const String path = server.arg("path");
  const std::string_view pathView{path.c_str(), path.length()};
  if (!downloadableReaderFile(pathView)) {
    server.send(403, "text/plain", "Only EPUB, TXT and MD reading files can be downloaded");
    return;
  }
  uint64_t expectedSize = 0;
  uint64_t offset = 0;
  const String sizeArg = server.arg("size");
  const String offsetArg = server.arg("offset");
  if (!parseByteCount({sizeArg.c_str(), sizeArg.length()}, expectedSize) ||
      !parseByteCount({offsetArg.c_str(), offsetArg.length()}, offset)) {
    server.send(400, "text/plain", "Invalid size or offset");
    return;
  }
  HalFile file = Storage.open(path.c_str());
  if (!file || file.isDirectory()) {
    server.send(404, "text/plain", "File not found");
    return;
  }
  const size_t maxPiece = downloadPieceLimit(d.profile == Profile::POCKET_SYNC);
  const auto piece = planDownloadPiece(file.fileSize64(), expectedSize, offset, maxPiece);
  switch (piece.result) {
    case PieceResult::SizeChanged:
      server.send(409, "text/plain", "File changed; restart the download");
      return;
    case PieceResult::OutOfRange:
      server.send(416, "text/plain", "Offset out of range");
      return;
    case PieceResult::Ok:
      break;
  }
  // Shared static staging buffer (4 KiB): no allocation per piece.
  uint8_t* body = firmware_flash::sharedStagingBuffer();
  if (!readDownloadPiece(file, offset, body, piece.length)) {
    server.send(500, "text/plain", "Could not read the file; retry");
    return;
  }
  file.close();
  server.client().setTimeout(DIAGNOSTIC_SEND_TIMEOUT_MS);
  feedLoopWDT();
  server.setContentLength(piece.length);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/octet-stream", "");
  server.sendContent(reinterpret_cast<const char*>(body), piece.length);
  feedLoopWDT();
}

enum class ReaderFileAction { List, Remove, Space };
void handleReaderFiles(WebServer& server, const RouteDeps& d, ReaderFileAction action) {
  char deviceId[9];
  if (!admitContentOperation(server, d, deviceId)) return;
  if (ESP.getFreeHeap() < 16384 || ESP.getMaxAllocHeap() < 4096) {
    server.send(503, "text/plain", "Reader is busy; retry file management shortly");
    return;
  }
  const String path = server.arg("path");
  if (action != ReaderFileAction::Space && !readerFilePath({path.c_str(), path.length()})) {
    server.send(400, "text/plain", "Invalid or protected file path");
    return;
  }
  uint32_t cursor = 0;
  const String cursorArg = server.arg("cursor");
  if (!parseGeneration({cursorArg.c_str(), cursorArg.length()}, cursor)) {
    server.send(400, "text/plain", "Invalid cursor");
    return;
  }
  JsonDocument doc;
  doc["deviceID"] = deviceId;
  if (action == ReaderFileAction::Space) {
    HalStorage::SpaceChunk chunk;
    if (!Storage.spaceChunk(cursor, chunk, [] {
          HalSystem::feedWatchdogIfRegistered();
          delay(1);
        })) {
      server.send(503, "text/plain", "SD usage unavailable; check the card and retry");
      return;
    }
    doc["totalBytes"] = chunk.totalBytes;
    doc["freeBytes"] = chunk.freeBytes;
    doc["nextCursor"] = chunk.nextCluster;
    doc["supported"] = chunk.supported;
  } else {
    auto file = Storage.open(path.c_str());
    if (!file) {
      server.send(404, "text/plain", "File or folder not found");
      return;
    }
    if (action == ReaderFileAction::Remove) {
      if (file.isDirectory() || !deletableReaderFile({path.c_str(), path.length()})) {
        server.send(403, "text/plain", "Only reading files can be deleted here");
        return;
      }
      const String size = server.arg("size");
      char expected[24];
      snprintf(expected, sizeof(expected), "%llu", static_cast<unsigned long long>(file.fileSize64()));
      if (size != expected) {
        server.send(409, "text/plain", "File changed; refresh before deleting");
        return;
      }
      file.close();
      if (!Storage.remove(path.c_str())) {
        server.send(500, "text/plain", "Could not delete file; retry");
        return;
      }
      clearBookCache(path.c_str());
      RECENT_BOOKS.removeByPath(path.c_str());
      if (Articles::isPath(path.c_str())) Storage.remove(Articles::donePath(path.c_str()).c_str());
      doc["deleted"] = true;
    } else {
      if (!file.isDirectory() || cursor % 32 != 0 || cursor > 8U * 1024U * 1024U || !file.seekSet(cursor)) {
        server.send(400, "text/plain", "Invalid folder cursor");
        return;
      }
      doc["path"] = path;
      auto entries = doc["entries"].to<JsonArray>();
      bool more = true;
      // At most 12 rows/48 inspected entries; no recursive tree or retained catalog.
      HalFile entry;
      for (unsigned scanned = 0; scanned < 48 && entries.size() < 12; ++scanned) {
        HalSystem::feedWatchdogIfRegistered();
        delay(1);
        const auto read = file.openNextEntry(entry);
        if (read == HalFile::DirectoryRead::Error) {
          server.send(503, "text/plain", "Could not read folder; retry");
          return;
        }
        if (read == HalFile::DirectoryRead::End) {
          more = false;
          break;
        }
        char name[128];
        const auto length = entry.getName(name, sizeof(name));
        if (!length || length >= sizeof(name) - 1) continue;
        String full = path == "/" ? path + name : path + "/" + name;
        if (!readerFilePath({full.c_str(), full.length()})) continue;
        auto row = entries.add<JsonObject>();
        row["name"] = name;
        row["directory"] = entry.isDirectory();
        row["size"] = entry.isDirectory() ? 0 : entry.fileSize64();
        row["deletable"] = !entry.isDirectory() && deletableReaderFile({full.c_str(), full.length()});
        HalSystem::feedWatchdogIfRegistered();
        delay(1);
      }
      doc["nextCursor"] = more ? file.position() : 0;
    }
  }
  if (doc.overflowed()) {
    server.send(503, "text/plain", "Reader is busy; retry shortly");
    return;
  }
  String response;
  serializeJson(doc, response);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", response);
}

// Reading-progress v1 (docs/reading-progress-v1.md). Handlers only read and
// write the small per-book records; XPointers are computed by the reader when a
// book is left, never while serving a request.
namespace Reading = PocketDaily::ReadingProgress;

void handleReadingList(WebServer& server, const RouteDeps& d) {
  char deviceId[9];
  if (!admitContentOperation(server, d, deviceId)) return;
  auto work = makeUniqueNoThrow<Reading::ExchangeWork>();
  if (!work || ESP.getFreeHeap() < 12288 || ESP.getMaxAllocHeap() < 4096) {
    server.send(503, "text/plain", "Reader is busy; retry reading positions shortly");
    return;
  }
  // Streamed in chunks from one ~1.7 KB request buffer; the whole reply stays
  // within MAX_LIST_BYTES, dropping the oldest books first. The BLE READ_LIST
  // stream produces the same bytes without the paths (ReadingExchange).
  server.client().setTimeout(DIAGNOSTIC_SEND_TIMEOUT_MS);
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  Reading::ListStream stream(deviceId, true, *work);
  for (size_t length; (length = stream.next(work->entry, sizeof(work->entry))) != 0;) {
    server.sendContent(work->entry, length);
  }
  server.sendContent("");
  LOG_DBG("WEB", "Reading list: %u books, %u bytes", static_cast<unsigned>(stream.listed()),
          static_cast<unsigned>(stream.total()));
}

void handleReadingOffer(WebServer& server, const RouteDeps& d) {
  struct OfferWork {
    Reading::OfferRequest request;
    Reading::ExchangeWork reading;
  };
  auto work = makeUniqueNoThrow<OfferWork>();
  if (!work) {
    server.send(503, "text/plain", "Reader memory is too low for the reading position");
    return;
  }
  const String& body = server.arg("plain");
  const char* error = nullptr;
  bool outOfMemory = false;
  if (!Reading::parseOfferJson(body.c_str(), body.length(), work->request, error, outOfMemory)) {
    server.send(outOfMemory ? 503 : 400, "text/plain", error ? error : "Invalid reading position");
    return;
  }
  char deviceId[9];
  if (!admitOperationFor(server, d, deviceId, work->request.deviceID)) return;
  switch (Reading::storeOffer(work->request, work->reading)) {
    case Reading::OfferStoreResult::STORED:
      server.sendHeader("Cache-Control", "no-store");
      server.send(200, "application/json", "{\"ok\":true,\"state\":\"pending\"}");
      return;
    case Reading::OfferStoreResult::STALE_POSITION:
      server.send(409, "text/plain", "Reader position changed; exchange again");
      return;
    case Reading::OfferStoreResult::FAILED:
      server.send(500, "text/plain", "Reading position could not be stored; retry");
      return;
    case Reading::OfferStoreResult::UNKNOWN_DOCUMENT:
      break;
  }
  server.send(404, "text/plain", "That book is not among the reader's recent books");
}

// Idempotent cleanup owns only the supplied hidden UUID file. A completed
// publication is deliberately untouched, including a staged firmware update.
void handleTransferControl(WebServer& server, const RouteDeps& d) {
  if (!d.stream || (d.host.httpUploadBusy && d.host.httpUploadBusy(d.host.self)) || d.stream->receiving()) {
    server.send(409, "text/plain", "A transfer is still active; pause it before cleanup");
    return;
  }
  if (!server.hasArg("plain") || server.arg("plain").length() > 512) {
    server.send(400, "text/plain", "Invalid transfer request");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) || !doc["staging"].is<const char*>() ||
      !doc["action"].is<const char*>() || !doc["kind"].is<const char*>()) {
    server.send(400, "text/plain", "Invalid transfer request");
    return;
  }
  const String path = doc["staging"].as<const char*>();
  const int slash = path.lastIndexOf('/');
  const std::string_view view(path.c_str(), path.length());
  if (path.length() > 256 || path != norm(d, path) || slash < 0 || !isTransferStagingName(view.substr(slash + 1)) ||
      !Content::genericContentWriteAllowed(view)) {
    server.send(400, "text/plain", "Only UUID staging files can be controlled");
    return;
  }
  const std::string_view action = doc["action"].as<const char*>();
  const std::string_view kind = doc["kind"].as<const char*>();
  if (kind != "content" && kind != "firmware") {
    server.send(400, "text/plain", "Invalid transfer kind");
    return;
  }
  if (action == "prepare") {
    d.stream->prepareFeedback(path, kind == "firmware" ? TransferKind::Firmware : TransferKind::Content);
  } else if (action == "discard") {
    if (!d.stream->discardStaging(path, kind == "firmware" ? TransferKind::Firmware : TransferKind::Content)) {
      server.send(500, "text/plain", "Temporary file cleanup failed; retry after checking SD card");
      return;
    }
  } else {
    server.send(400, "text/plain", "Invalid transfer action");
    return;
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

// Pocket clients upload to a unique hidden .part file, then ask the reader
// to verify size + CRC and atomically publish it. A dropped phone or Wi-Fi
// link therefore never turns a valid book or update.bin into a partial file.
void handleCommitUpload(WebServer& server, const RouteDeps& d, const bool queryOnly = false) {
  if ((d.host.httpUploadBusy && d.host.httpUploadBusy(d.host.self)) || (d.stream && d.stream->receiving())) {
    if (d.stream && !queryOnly) d.stream->noteCommitFailed();
    server.send(409, "text/plain", "Another file transfer is active");
    return;
  }
  if (!server.hasArg("plain") || server.arg("plain").length() > 1536) {
    if (d.stream && !queryOnly) d.stream->noteCommitFailed();
    server.send(400, "text/plain", "Missing JSON body");
    return;
  }

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, server.arg("plain"));
  if (error || !doc["staging"].is<const char*>() || !doc["target"].is<const char*>() || !doc["size"].is<size_t>() ||
      !doc["crc32"].is<const char*>()) {
    if (d.stream && !queryOnly) d.stream->noteCommitFailed();
    server.send(400, "text/plain", "Invalid commit request");
    return;
  }

  const String staging = norm(d, doc["staging"].as<const char*>());
  const String target = norm(d, doc["target"].as<const char*>());
  if (!Content::genericContentWriteAllowed({staging.c_str(), staging.length()}) ||
      !Content::genericContentWriteAllowed({target.c_str(), target.length()})) {
    if (d.stream && !queryOnly) d.stream->noteCommitFailed();
    server.send(403, "text/plain", "Protected or unsafe content path");
    return;
  }
  const size_t expectedSize = doc["size"].as<size_t>();
  if (!Content::contentWriteWithinBudget({target.c_str(), target.length()}, 0, expectedSize)) {
    if (d.stream && !queryOnly) d.stream->noteCommitFailed();
    server.send(413, "text/plain", "Content staging file exceeds 256 KiB");
    return;
  }
  const String expectedCrcText = doc["crc32"].as<const char*>();
  char* crcEnd = nullptr;
  const uint32_t expectedCrc = strtoul(expectedCrcText.c_str(), &crcEnd, 16);

  const int stagingSlash = staging.lastIndexOf('/');
  const int targetSlash = target.lastIndexOf('/');
  const String stagingName = staging.substring(stagingSlash + 1);
  const String targetName = target.substring(targetSlash + 1);
  const String stagingParent = staging.substring(0, stagingSlash + 1);
  const String targetParent = target.substring(0, targetSlash + 1);
  if (!stagingName.startsWith(".pocket-") || !stagingName.endsWith(".part") || stagingParent != targetParent ||
      targetName.isEmpty() || protectedName(d, targetName) || !crcEnd || *crcEnd != '\0' ||
      expectedCrcText.length() != 8) {
    if (d.stream && !queryOnly) d.stream->noteCommitFailed();
    server.send(400, "text/plain", "Unsafe commit path or checksum");
    return;
  }

  const Publication::Request publication{staging.c_str(), target.c_str(), static_cast<uint32_t>(expectedSize),
                                         expectedCrc};
  if (!Publication::valid(publication)) {
    if (d.stream && !queryOnly) d.stream->noteCommitFailed();
    server.send(400, "text/plain", "Publication path exceeds receipt limit");
    return;
  }
  const bool published = Publication::matches(publication);
  if (queryOnly || published) {
    if (!published) {
      server.send(404, "application/json", "{\"state\":\"unknown\"}");
      return;
    }
    char response[80];
    snprintf(response, sizeof(response), "{\"size\":%u,\"crc32\":\"%08lX\"}", static_cast<unsigned>(expectedSize),
             static_cast<unsigned long>(expectedCrc));
    server.send(200, "application/json", response);
    return;
  }
  if (!d.stream) {
    server.send(409, "text/plain", "Upload stream unavailable");
    return;
  }
  const StagedUpload& staged = d.stream->staged();
  String uploadedPath = norm(d, staged.path + "/" + staged.fileName);
  const uint32_t uploadedCrc = staged.crc32 ^ 0xFFFFFFFFU;
  if (!staged.success || uploadedPath != staging || staged.size != expectedSize || uploadedCrc != expectedCrc ||
      !Storage.exists(staging.c_str())) {
    if (d.stream && !queryOnly) d.stream->noteCommitFailed();
    server.send(409, "text/plain", "Staged upload verification failed");
    return;
  }

  HalFile stagedFile = Storage.open(staging.c_str());
  const bool stagedSizeMatches = stagedFile && !stagedFile.isDirectory() && stagedFile.size() == expectedSize;
  if (stagedFile) stagedFile.close();
  if (!stagedSizeMatches) {
    if (d.stream && !queryOnly) d.stream->noteCommitFailed();
    server.send(409, "text/plain", "Staged file size mismatch");
    return;
  }

  const String backup = stagingParent + ".pocket-backup.part";
  Storage.remove(backup.c_str());
  const bool hadTarget = Storage.exists(target.c_str());
  if (hadTarget && !renameStorageFile(target, backup)) {
    if (d.stream && !queryOnly) d.stream->noteCommitFailed();
    server.send(500, "text/plain", "Could not preserve existing target");
    return;
  }
  if (!renameStorageFile(staging, target)) {
    if (hadTarget) renameStorageFile(backup, target);
    if (d.stream && !queryOnly) d.stream->noteCommitFailed();
    server.send(500, "text/plain", "Could not publish staged upload");
    return;
  }
  if (hadTarget) Storage.remove(backup.c_str());
  // Transport never flashes. Record the publication so the restart that ends
  // this session offers the on-device confirmation once (staged_firmware.h).
  if (StagedFirmware::isPublishTarget(target.c_str()))
    StagedFirmware::notePublished(static_cast<uint32_t>(expectedSize), uploadedCrc);

  d.stream->noteCommitted(StagedFirmware::isPublishTarget(target.c_str()));
  clearBookCache(target.c_str());
  if (Articles::isPath(target.c_str())) {
    const auto marker = Articles::donePath(target.c_str());
    if (Storage.exists(marker.c_str()) && !Storage.remove(marker.c_str())) {
      LOG_ERR("ARTICLE", "Could not reset read marker after replacement");
    }
  }
  // A lost receipt write leaves an explicitly unknown outcome; it never causes republication.
  if (!Publication::save(publication)) {
    server.send(500, "text/plain", "File published but receipt could not be saved");
    return;
  }
  char response[80];
  snprintf(response, sizeof(response), "{\"size\":%u,\"crc32\":\"%08lX\"}", static_cast<unsigned>(expectedSize),
           static_cast<unsigned long>(uploadedCrc));
  server.send(200, "application/json", response);
  LOG_INF("WEB", "Committed Pocket upload %s (%u bytes, crc32=%08lX)", target.c_str(),
          static_cast<unsigned>(expectedSize), static_cast<unsigned long>(uploadedCrc));
}

// One chunk of one file of a published revision, so the companion can load
// the reader's cards back into its editor. Published revisions are immutable
// and content-addressed; this reads only <content>/<revision>/<leaf> and never
// writes. The companion verifies every file against the manifest and the
// manifest against the revision, so nothing here is an integrity claim.
constexpr size_t CONTENT_FILE_CHUNK_BYTES = firmware_flash::STAGING_BUFFER_BYTES;

void handleContentFile(WebServer& server, const RouteDeps& d) {
  char deviceId[9];
  if (!admitContentOperation(server, d, deviceId)) return;
  // The shared staging buffer below also batches stream uploads.
  if (d.stream && d.stream->transferActive()) {
    server.send(409, "text/plain", "Another file transfer is active");
    return;
  }
  const String revision = server.arg("revision");
  if (!contentRevisionArgument(server, revision)) return;
  char path[Content::PUBLISHED_PATH_BYTES];
  if (!Content::publishedFilePath(revision.c_str(), server.arg("name").c_str(), path, sizeof(path))) {
    server.send(400, "text/plain", "Invalid content file name");
    return;
  }
  const String offsetText = server.arg("offset");
  if (offsetText.isEmpty() || offsetText.length() > 7) {
    server.send(400, "text/plain", "Invalid content file offset");
    return;
  }
  for (size_t i = 0; i < offsetText.length(); i++) {
    if (offsetText[i] < '0' || offsetText[i] > '9') {
      server.send(400, "text/plain", "Invalid content file offset");
      return;
    }
  }
  HalFile file = Storage.open(path);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    server.send(404, "text/plain", "Content file not found");
    return;
  }
  const size_t size = file.size();
  const size_t offset = static_cast<size_t>(offsetText.toInt());
  if (offset >= size || !file.seek(offset)) {
    file.close();
    server.send(416, "text/plain", "Content file offset out of range");
    return;
  }
  uint8_t* body = firmware_flash::sharedStagingBuffer();
  const int count = file.read(body, std::min(size - offset, CONTENT_FILE_CHUNK_BYTES));
  file.close();
  if (count <= 0) {
    server.send(500, "text/plain", "Could not read content file");
    return;
  }
  // Same bounded send as the screen preview: the socket timeout, not a
  // watchdog suspension, keeps a vanished peer from stalling the loop.
  server.client().setTimeout(DIAGNOSTIC_SEND_TIMEOUT_MS);
  feedLoopWDT();
  server.setContentLength(static_cast<size_t>(count));
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/octet-stream", "");
  server.sendContent(reinterpret_cast<const char*>(body), static_cast<size_t>(count));
  feedLoopWDT();
}

// Pocket only needs a handful of preferences. Avoid constructing and streaming
// the full localized settings registry on the no-PSRAM private-AP path: that
// response can consume the last contiguous heap immediately after Wi-Fi
// starts and strand the X3 on its retained Hotspot Mode frame. The button keys
// (added 2026-09-26) tell the companion this reader accepts them on POST.
// A bounded, temporary response; no full settings registry or retained buffer.
void handleGetPreferences(WebServer& server) {
  // 320 bytes exceeds the stack budget; temporary ownership avoids retained RAM.
  auto json = makeUniqueNoThrow<char[]>(320);
  if (!json) {
    server.send(503, "text/plain", "Not enough memory for preferences; retry");
    return;
  }
  const int written = snprintf(
      json.get(), 320,
      "{\"startupApp\":%u,\"pocketDailySleepCover\":%u,\"sleepTimeoutMinutes\":%u,\"fontSize\":%u,"
      "\"sideButtonLayout\":%u,\"frontButtonFollowOrientation\":%u,\"sleepWakeIndicator\":%u,\"orientation\":%u,"
      "\"lineSpacing\":%u,\"screenMargin\":%u}",
      static_cast<unsigned>(SETTINGS.startupApp), static_cast<unsigned>(SETTINGS.pocketDailySleepCover),
      static_cast<unsigned>(SETTINGS.sleepTimeoutMinutes),
      static_cast<unsigned>(LegacyFontSize::fromPoints(SETTINGS.fontPointSize)),
      static_cast<unsigned>(SETTINGS.sideButtonLayout), static_cast<unsigned>(SETTINGS.frontButtonFollowOrientation),
      static_cast<unsigned>(SETTINGS.sleepWakeIndicator), static_cast<unsigned>(SETTINGS.orientation),
      static_cast<unsigned>(SETTINGS.lineSpacing), static_cast<unsigned>(SETTINGS.screenMargin));
  if (written <= 0 || static_cast<size_t>(written) >= 320) {
    server.send(500, "text/plain", "Could not encode Pocket preferences");
    return;
  }
  server.sendHeader("Connection", "close");
  server.send(200, "application/json", json.get());
}

void handlePostPreferences(WebServer& server, const RouteDeps& d) {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "Missing JSON body");
    return;
  }
  // Validate the whole body first; settings change only when every field is
  // valid, and a failed save restores the previous values (PreferencesUpdate.h).
  const String& body = server.arg("plain");
  const PreferenceLimits limits{
      CrossPointSettings::STARTUP_APP_COUNT,         CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES,
      CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, LegacyFontSize::COUNT,
      CrossPointSettings::SIDE_BUTTON_LAYOUT_COUNT,  CrossPointSettings::ORIENTATION_COUNT,
      CrossPointSettings::LINE_COMPRESSION_COUNT,    CrossPointSettings::SCREEN_MARGIN_MIN,
      CrossPointSettings::SCREEN_MARGIN_MAX};
  PreferencesUpdate update;
  const char* error = nullptr;
  if (!parsePreferences(body.c_str(), body.length(), limits, update, error)) {
    server.send(400, "text/plain", error ? error : "Invalid preferences");
    return;
  }
  const uint8_t previous[10] = {SETTINGS.startupApp,          SETTINGS.pocketDailySleepCover,
                                SETTINGS.sleepTimeoutMinutes, SETTINGS.fontPointSize,
                                SETTINGS.sideButtonLayout,    SETTINGS.frontButtonFollowOrientation,
                                SETTINGS.sleepWakeIndicator,  SETTINGS.orientation,
                                SETTINGS.lineSpacing,         SETTINGS.screenMargin};
  if (update.hasStartupApp) SETTINGS.startupApp = update.startupApp;
  if (update.hasSleepWakeIndicator) SETTINGS.sleepWakeIndicator = update.sleepWakeIndicator;
  if (update.hasSleepCover) SETTINGS.pocketDailySleepCover = update.sleepCover;
  if (update.hasSleepTimeout) SETTINGS.sleepTimeoutMinutes = update.sleepTimeoutMinutes;
  // An unchanged legacy bucket must not quantize a precise size selected on-device.
  if (update.hasFontSize && update.fontSize != LegacyFontSize::fromPoints(SETTINGS.fontPointSize)) {
    SETTINGS.fontPointSize = LegacyFontSize::toPoints(update.fontSize);
  }
  if (update.hasOrientation) SETTINGS.orientation = update.orientation;
  if (update.hasLineSpacing) SETTINGS.lineSpacing = update.lineSpacing;
  if (update.hasScreenMargin) SETTINGS.screenMargin = update.screenMargin;
  if (update.hasSideButtonLayout) SETTINGS.sideButtonLayout = update.sideButtonLayout;
  if (update.hasFrontButtonFollowOrientation) {
    SETTINGS.frontButtonFollowOrientation = update.frontButtonFollowOrientation;
  }

  const bool changed = previous[0] != SETTINGS.startupApp || previous[1] != SETTINGS.pocketDailySleepCover ||
                       previous[2] != SETTINGS.sleepTimeoutMinutes || previous[3] != SETTINGS.fontPointSize ||
                       previous[4] != SETTINGS.sideButtonLayout ||
                       previous[5] != SETTINGS.frontButtonFollowOrientation ||
                       previous[6] != SETTINGS.sleepWakeIndicator || previous[7] != SETTINGS.orientation ||
                       previous[8] != SETTINGS.lineSpacing || previous[9] != SETTINGS.screenMargin;
  if (changed && !SETTINGS.saveToFile()) {
    SETTINGS.startupApp = previous[0];
    SETTINGS.pocketDailySleepCover = previous[1];
    SETTINGS.sleepTimeoutMinutes = previous[2];
    SETTINGS.fontPointSize = previous[3];
    SETTINGS.sideButtonLayout = previous[4];
    SETTINGS.frontButtonFollowOrientation = previous[5];
    SETTINGS.sleepWakeIndicator = previous[6];
    SETTINGS.orientation = previous[7];
    SETTINGS.lineSpacing = previous[8];
    SETTINGS.screenMargin = previous[9];
    server.send(500, "text/plain", "Could not save Pocket preferences");
    return;
  }
  server.sendHeader("Connection", "close");
  server.send(200, "application/json", "{\"saved\":true}");
  if (changed) d.liveStudio->notifyPrefsChanged();
}

#ifdef ENABLE_DEV_REMOTE_FLASH
void handleDevReaderCapture(WebServer& server, const RouteDeps& d) {
  char identity[9];
  if (!admitContentOperation(server, d, identity)) return;
  uint32_t run = 0;
  const String arg = server.arg("run");
  if (d.apMode || !parseGeneration({arg.c_str(), arg.length()}, run) || !run) {
    server.send(400, "text/plain", "Requires Same Wi-Fi and a nonzero run");
    return;
  }
  if (!DevCapture::requestReader(run)) {
    server.send(409, "text/plain", "Missing reader fixture or request could not be saved");
    return;
  }
  server.sendHeader("Connection", "close");
  server.send(202, "text/plain", "Reader capture scheduled; automatic Same Wi-Fi return");
  delay(750);
  ESP.restart();
}

void handleDevBleCycle(WebServer& server, const RouteDeps& d) {
  if (d.apMode || (d.host.httpUploadBusy && d.host.httpUploadBusy(d.host.self)) ||
      (d.stream && d.stream->receiving())) {
    server.send(409, "text/plain", "Requires idle Same Wi-Fi");
    return;
  }
  const String argument = server.arg("run");
  uint32_t run = 0;
  if (!parseGeneration(std::string_view(argument.c_str(), argument.length()), run) || !run) {
    server.send(400, "text/plain", "run must be a nonzero uint32");
    return;
  }
  using Pocket::NearbySync::DevSleepCycle::ReturnMode;
  const String sleep = server.arg("sleep");
  bool standbySupported = false;
#ifdef ENABLE_BLE_STANDBY
  standbySupported = gpio.deviceIsX3();
#endif
  if ((sleep != "" && sleep != "timer" && sleep != "standby") || (sleep == "timer" && !gpio.deviceIsX3()) ||
      (sleep == "standby" && !standbySupported)) {
    server.send(400, "text/plain", "timer sleep is supported only for X3 trials");
    return;
  }
  const auto mode = sleep == "standby" ? ReturnMode::AppStandby
                    : sleep == "timer" ? ReturnMode::TimedDeepSleep
                                       : ReturnMode::Restart;
  if (!Pocket::NearbySync::DevSleepCycle::request(run, mode)) {
    server.send(503, "text/plain", "Could not verify the one-shot experiment request");
    return;
  }
  server.sendHeader("Connection", "close");
  server.send(202, "text/plain", "BLE sleep-path test scheduled; automatic Same Wi-Fi return");
  delay(750);
  ESP.restart();
}

void handleDevBleCycleResult(WebServer& server) {
  using namespace Pocket::NearbySync;
  // At most 208 B, request-local: alongside JSON encoding it would exceed the
  // stack-frame budget. No allocation is retained during BLE measurement.
  auto record = makeUniqueNoThrow<DevSleepCycle::Record>();
  if (!record) {
    server.send(503, "text/plain", "No result memory");
    return;
  }
  if (!DevSleepCycle::read(*record)) {
    server.send(404, "text/plain", "No verified cycle record");
    return;
  }
  JsonDocument doc;
  doc["run"] = record->run;
  doc["state"] = static_cast<uint32_t>(record->state);
  doc["returnMode"] = static_cast<uint32_t>(record->returnMode);
  doc["timerArmed"] = record->timerArmed != 0;
  doc["checkpoint"] = static_cast<uint32_t>(record->checkpoint);
  doc["batteryPercent"] = record->batteryPercent;
  doc["timerWake"] = powerManager.devWokeFromTimer();
  doc["lightSleeps"] = record->lightSleeps;
  doc["lightSleepMs"] = record->lightSleepMs;
  doc["standbyMs"] = record->standbyMs;
  doc["fromWifi"] = record->fromWifi != 0;
  doc["frameReleased"] = record->released != 0;
  doc["frameBytes"] = record->frameBytes;
  doc["beforeFree"] = record->beforeFree;
  doc["beforeBlock"] = record->beforeBlock;
  doc["afterFree"] = record->afterFree;
  doc["afterBlock"] = record->afterBlock;
  doc["statsValid"] = Stats::valid(record->result);
  const auto& result = record->result;
  doc["opened"] = result.opened - record->baseline.opened;
  doc["connections"] = result.connections - record->baseline.connections;
  doc["lists"] = result.lists - record->baseline.lists;
  doc["offers"] = result.offers - record->baseline.offers;
  doc["gate"] = Window::gateName(static_cast<Window::Gate>(result.lastGate));
  doc["close"] = Window::closeReasonName(static_cast<Window::CloseReason>(result.lastClose));
  doc["startFree"] = result.startFree;
  doc["startBlock"] = result.startBlock;
  doc["openFree"] = result.openFree;
  doc["openBlock"] = result.openBlock;
  doc["minFree"] = result.minFree;
  doc["minBlock"] = result.minBlock;
  doc["closedFree"] = result.closedFree;
  doc["closedBlock"] = result.closedBlock;
  String response;
  serializeJson(doc, response);
  server.sendHeader("Connection", "close");
  server.send(200, "application/json", response);
}

// Developer builds only. Validates and flashes the staged /update.bin, then
// reboots; a one-shot marker makes the next boot rejoin the saved STA network.
// Missing credentials fall back to the normal Wi-Fi chooser. The response is sent before
// flashing because the loop blocks for the erase/write and ends in a chip
// restart.
void handleDevFlash(WebServer& server, const RouteDeps& d) {
  HalSystem::setCrashBreadcrumb("dev:remote-flash");
  // Allowed on the reader's own hotspot too: when the STA path is the thing
  // being repaired, the dedicated AP link is the delivery route (and it has
  // no router in the path). Still a dev-build-only endpoint.
  if (!Storage.exists("/update.bin")) {
    server.send(404, "text/plain", "No /update.bin staged on the reader");
    return;
  }
  const esp_partition_t* dest = esp_ota_get_next_update_partition(nullptr);
  if (!dest) {
    server.send(500, "text/plain", "No OTA partition available");
    return;
  }
  if (firmware_flash::validateImageFile("/update.bin", dest->size) != firmware_flash::Result::OK) {
    server.send(422, "text/plain", "Staged /update.bin failed image validation");
    return;
  }
  const char returnMode = PocketDaily::Boot::devBootMarker(d.profile, d.apMode);
  bool saved = false;
  {
    HalFile marker;
    if (Storage.openFileForWrite("DEV", PocketDaily::DEV_BOOT_FILE_TRANSFER_MARKER, marker) && marker) {
      saved = marker.write(reinterpret_cast<const uint8_t*>(&returnMode), 1) == 1;
    }
  }
  {
    HalFile marker = Storage.open(PocketDaily::DEV_BOOT_FILE_TRANSFER_MARKER);
    char actual = 0;
    saved = saved && marker && marker.size() == 1 && marker.read(&actual, 1) == 1 && actual == returnMode;
  }
  if (!saved) {
    Storage.remove(PocketDaily::DEV_BOOT_FILE_TRANSFER_MARKER);
    server.send(503, "text/plain", "Could not verify update return mode; firmware was not flashed");
    return;
  }
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", "Flashing /update.bin (dev build). Reader will return to its transfer mode.");
  delay(750);  // Put the response on the wire before the loop disappears.
  if (firmware_flash::flashFromSdPath("/update.bin", nullptr, nullptr, true) != firmware_flash::Result::OK) {
    LOG_ERR("WEB", "Dev remote flash failed; staying up");
    Storage.remove(PocketDaily::DEV_BOOT_FILE_TRANSFER_MARKER);
    return;
  }
  ESP.restart();
}
#endif

}  // namespace

// One route definition shared by the legacy WebServer registry and the
// allocation-free Sync dispatcher. Captures are request-local on Sync; no
// std::function conversion or SDK Uri clone is constructed on that path.
template <typename Routes>
void configurePocketRoutes(Routes& routes, WebServer& server, const RouteDeps& d) {
  // Preparation verifies staged bytes and may reuse verified published assets.
  routes.on("/api/pocket/v1/content/prepare", HTTP_POST, [server = &server, deps = &d] {
    note(*deps);
    handlePrepareContent(*server, *deps);
  });
  routes.on("/api/pocket/v1/content/state", HTTP_GET, [server = &server, deps = &d] {
    note(*deps);
    handleContentState(*server, *deps);
  });
  routes.on("/api/pocket/v1/content/file", HTTP_GET, [server = &server, deps = &d] {
    note(*deps);
    handleContentFile(*server, *deps);
  });
  routes.on("/api/pocket/v1/content/activate", HTTP_POST, [server = &server, deps = &d] {
    note(*deps);
    handleActivateContent(*server, *deps);
  });
  if (d.presentation.prepare && d.presentation.state) {
    routes.on("/api/pocket/v1/content/present", HTTP_POST, [server = &server, deps = &d] {
      note(*deps);
      handlePresentation(*server, *deps, true);
    });
    routes.on("/api/pocket/v1/content/presentation", HTTP_GET, [server = &server, deps = &d] {
      note(*deps);
      handlePresentation(*server, *deps, false);
    });
    routes.on("/api/pocket/v1/display", HTTP_GET, [server = &server, deps = &d] {
      note(*deps);
      handleDisplay(*server, *deps);
    });
  }
  // Home / Daily Brief share the content slot; Sync profiles only.
  if (isSyncProfile(d.profile) && d.screen.enqueue && d.screen.state && d.screen.busy) {
    routes.on("/api/pocket/v1/screen/present", HTTP_POST, [server = &server, deps = &d] {
      note(*deps);
      handleScreenPresentation(*server, *deps, true);
    });
    routes.on("/api/pocket/v1/screen/presentation", HTTP_GET, [server = &server, deps = &d] {
      note(*deps);
      handleScreenPresentation(*server, *deps, false);
    });
  }
  if (isSyncProfile(d.profile)) {
    routes.on("/api/pocket/v1/wifi/setup", HTTP_POST, [server = &server, deps = &d] {
      char deviceId[9];
      const String claimed = server->arg("deviceID");
      if (!admitOperationFor(*server, *deps, deviceId, claimed.c_str())) return;
      if (server->hasArg("plain") && server->arg("plain").length()) {
        server->send(400, "text/plain", "No credentials accepted over HTTP");
        return;
      }
      Pocket::NearbySync::WifiSetup::prepareBluetooth();
      server->sendHeader("Cache-Control", "no-store");
      server->send(200, "application/json", "{\"bluetooth\":true}");
    });
    routes.on("/api/pocket/v1/session/end", HTTP_POST,
              [server = &server, deps = &d] { handleSessionEnd(*server, *deps); });
  }
  // Diagnostics is available in every profile so a phone can retrieve the
  // previous panic after the reader has recovered, without exposing arbitrary
  // hidden files or requiring the SD card to be removed.
  routes.on("/api/pocket/v1/crash-report", HTTP_GET, [server = &server, deps = &d] {
    note(*deps);
    handleCrashReport(*server);
  });

  routes.on("/api/pocket/v1/transfer", HTTP_POST, [server = &server, deps = &d] {
    note(*deps);
    handleTransferControl(*server, *deps);
  });
  routes.on("/api/pocket/v1/publication", HTTP_POST, [server = &server, deps = &d] {
    note(*deps);
    handleCommitUpload(*server, *deps, true);
  });
  routes.on("/api/pocket/v1/commit", HTTP_POST, [server = &server, deps = &d] {
    note(*deps);
    handleCommitUpload(*server, *deps);
  });

  if (isSyncProfile(d.profile)) {
    routes.on("/api/pocket/v1/files", HTTP_GET, [server = &server, deps = &d] {
      note(*deps);
      handleReaderFiles(*server, *deps, ReaderFileAction::List);
    });
    routes.on("/api/pocket/v1/files", HTTP_DELETE, [server = &server, deps = &d] {
      note(*deps);
      handleReaderFiles(*server, *deps, ReaderFileAction::Remove);
    });
    routes.on("/api/pocket/v1/files/content", HTTP_GET, [server = &server, deps = &d] {
      note(*deps);
      handleReaderFileContent(*server, *deps);
    });
    routes.on("/api/pocket/v1/storage", HTTP_GET, [server = &server, deps = &d] {
      note(*deps);
      handleReaderFiles(*server, *deps, ReaderFileAction::Space);
    });
    routes.on("/api/pocket/v1/profile", HTTP_GET, [server = &server, deps = &d] {
      note(*deps);
      handleGetProfile(*server, *deps);
    });
    routes.on("/api/pocket/v1/profile", HTTP_POST, [server = &server, deps = &d] {
      note(*deps);
      handlePostProfile(*server, *deps);
    });
    routes.on("/api/pocket/v1/glance", HTTP_POST, [server = &server, deps = &d] {
      note(*deps);
      handlePostGlance(*server, *deps);
    });
    routes.on("/api/pocket/v1/reading", HTTP_GET, [server = &server, deps = &d] {
      note(*deps);
      handleReadingList(*server, *deps);
    });
    routes.on("/api/pocket/v1/reading", HTTP_POST, [server = &server, deps = &d] {
      note(*deps);
      handleReadingOffer(*server, *deps);
    });
  }
  if (d.profile == Profile::POCKET_SYNC || d.profile == Profile::COMPANION) {
    routes.on("/api/pocket/v1/preferences", HTTP_GET, [server = &server, deps = &d] {
      note(*deps);
      handleGetPreferences(*server);
    });
    routes.on("/api/pocket/v1/preferences", HTTP_POST, [server = &server, deps = &d] {
      note(*deps);
      handlePostPreferences(*server, *deps);
    });
  }
#if POCKET_HEAP_MAP_ENABLED
  // HN-2 evidence part 2: per-task stack high-water marks. Safe API per task
  // (no scheduler suspension, unlike uxTaskGetSystemState which hung).
  routes.on("/api/pocket/v1/dev/stack-report", HTTP_GET, [server = &server, deps = &d] {
    note(*deps);
    char report[640];
    const size_t n = PocketDaily::StackReport::render(report, sizeof(report));
    server->send(200, "text/plain; charset=utf-8", n ? String(report) : "unavailable");
  });
  // HN-2 evidence: caps-only, fixed small response. The fuller map hung the
  // request on the loaded X3 (uxTaskGetSystemState suspends every task; the
  // chunked variant never answered). Three numbers per capability survive
  // any fragmentation.
  routes.on("/api/pocket/v1/dev/heap-map", HTTP_GET, [server = &server, deps = &d] {
    note(*deps);
    char map[256];
    const size_t n = PocketDaily::HeapMap::renderCaps(map, sizeof(map));
    server->send(200, "text/plain; charset=utf-8", n ? String(map) : "unavailable");
  });
#endif
#ifdef ENABLE_DEV_REMOTE_FLASH
  // Developer builds only: flash the staged /update.bin over the LAN so
  // iteration does not walk the on-device Settings menus. Absent from
  // gh_release builds by build flag, not by request filtering.
  routes.on("/api/pocket/v1/dev/transfer-stats", HTTP_GET, [server = &server, deps = &d] {
    note(*deps);
    if (!deps->stream || deps->stream->receiving()) {
      server->send(409, "text/plain", "Transfer statistics available after receiving ends");
      return;
    }
    // Same activity loop owns the stream and HTTP, so the completed record
    // cannot change while these small chunks are serialized.
    char chunk[224];
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    server->send(200, "application/json", "");
    for (unsigned section = 0; section < 3; ++section) {
      const size_t count = deps->stream->metrics().format(chunk, sizeof(chunk), section);
      if (!count) break;
      server->sendContent(chunk, count);
    }
    server->sendContent("");
  });
  // Developer evidence: TCP PCB states and Sync TIME_WAIT releases. Heap is
  // sampled first so this response's own buffers are not counted.
  routes.on("/api/pocket/v1/dev/tcp", HTTP_GET, [server = &server, deps = &d] {
    note(*deps);
    const auto heap = ESP.getFreeHeap();
    const auto block = ESP.getMaxAllocHeap();
    char census[160];
    if (!renderTcpCensus(census, sizeof(census))) census[0] = '\0';
    const auto purged = deps->timeWaitPurged ? deps->timeWaitPurged(deps->host.self) : 0u;
    char line[240];
    snprintf(line, sizeof(line), "heap=%u block=%u timeWaitPurged=%u %s\n", static_cast<unsigned>(heap),
             static_cast<unsigned>(block), static_cast<unsigned>(purged), census);
    server->send(200, "text/plain", line);
  });
  routes.on("/api/pocket/v1/dev/flash", HTTP_POST, [server = &server, deps = &d] {
    note(*deps);
    handleDevFlash(*server, *deps);
  });
  routes.on("/api/pocket/v1/dev/reader-capture", HTTP_POST,
            [server = &server, deps = &d] { handleDevReaderCapture(*server, *deps); });
  routes.on("/api/pocket/v1/dev/ble-cycle", HTTP_POST, [server = &server, deps = &d] {
    note(*deps);
    handleDevBleCycle(*server, *deps);
  });
  routes.on("/api/pocket/v1/dev/ble-cycle", HTTP_GET, [server = &server, deps = &d] {
    note(*deps);
    handleDevBleCycleResult(*server);
  });
  for (const auto method : {HTTP_POST, HTTP_GET, HTTP_DELETE}) {
    routes.on("/api/pocket/v1/dev/screen-capture", method, [server = &server, deps = &d] {
      note(*deps);
      handleDevScreenCapture(*server, *deps);
    });
  }
  // Developer builds only: force one repaint (live-frame debugging).
  routes.on("/api/pocket/v1/dev/render", HTTP_POST, [server = &server, deps = &d] {
    note(*deps);
    DEV_TRACE(PocketDaily::DevTrace::RENDER_REQ);
    repaint(*deps);
    server->send(204, "text/plain", "");
  });

#endif
#ifdef ENABLE_DEV_NETWORK_DIAGNOSTICS
  // Explicit diagnostics builds only: network stability post-mortem log
  // (docs/network-stability-analysis.md). Readable after the radio died.
  routes.on("/api/pocket/v1/dev/net-health", HTTP_GET, [server = &server, deps = &d] {
    note(*deps);
    static char tail[4096];
    const size_t n = PocketDaily::NetHealth::tail(tail, sizeof(tail));
    server->send(200, "text/plain; charset=utf-8", n ? String(tail) : "no net-health log yet");
  });
  // Developer builds only: dump the live-studio trace ring (text lines
  // "ms tag aux"), oldest first. A gap between heartbeats is the stall.
  routes.on("/api/pocket/v1/dev/live-debug", HTTP_GET, [server = &server, deps = &d] {
    note(*deps);
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    server->send(200, "text/plain", "");
    static const char* names[] = {"HEARTBEAT",    "RENDER_START", "RENDER_DONE", "CAPTURE_ENTER", "CAPTURE_WRITE",
                                  "CAPTURE_DONE", "SEND_START",   "SEND_DONE",   "RENDER_REQ",    "TICK"};
    const size_t next = PocketDaily::DevTrace::gNext.load();
    char line[48];
    for (size_t k = 0; k < PocketDaily::DevTrace::CAPACITY; k++) {
      const auto& e = PocketDaily::DevTrace::gEntries[(next + k) % PocketDaily::DevTrace::CAPACITY];
      if (e.tag == 0 && e.ms == 0) continue;
      snprintf(line, sizeof(line), "%lu %s %u", static_cast<unsigned long>(e.ms),
               e.tag < sizeof(names) / sizeof(names[0]) ? names[e.tag] : "?", e.aux);
      server->sendContent(line);
      server->sendContent("\n");
    }
    server->sendContent("");
  });
#endif
}

void registerPocketRoutes(WebServer& server, const RouteDeps& d) {
  if (!isSyncProfile(d.profile)) configurePocketRoutes(server, server, d);
}

bool dispatchPocketRoute(WebServer& server, const RouteDeps& d) {
  if (!isSyncProfile(d.profile)) return false;
  // SDK uri() returns a String by value. Keep this single request-scoped copy
  // alive while the dispatcher borrows it; never retain it across requests.
  const String path = server.uri();
  ExactRouteDispatch routes({path.c_str(), path.length()}, server.method());
  configurePocketRoutes(routes, server, d);
  return routes.handled();
}

}  // namespace PocketDaily::Web
