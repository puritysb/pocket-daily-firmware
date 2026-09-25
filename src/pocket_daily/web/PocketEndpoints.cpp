#include "pocket_daily/web/PocketEndpoints.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <Logging.h>
#include <Memory.h>
#include <WebServer.h>
#include <esp_ota_ops.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "CrossPointSettings.h"
#include "activities/RenderLock.h"
#include "components/UITheme.h"
#include "network/FirmwareFlasher.h"
#include "pocket_daily/ContentActiveStore.h"
#include "pocket_daily/ContentPathPolicy.h"
#include "pocket_daily/ContentRevisionStore.h"
#include "pocket_daily/ContentSealStore.h"
#include "pocket_daily/PocketProfileStore.h"
#include "pocket_daily/PocketScreenPreview.h"
#include "pocket_daily/boot/DevBootReturn.h"
#include "pocket_daily/live_studio/DevTrace.h"
#include "pocket_daily/live_studio/HeapMap.h"
#include "pocket_daily/live_studio/LiveFrameCapture.h"
#include "pocket_daily/live_studio/NetHealth.h"
#include "pocket_daily/live_studio/StackReport.h"
#include "pocket_daily/live_studio/UiPackStore.h"
#include "pocket_daily/web/DisplayState.h"
#include "pocket_daily/web/ExactRouteDispatch.h"
#include "pocket_daily/web/PocketStatus.h"
#include "pocket_daily/web/PreferencesUpdate.h"
#include "pocket_daily/web/TcpCensus.h"
#include "util/BookCacheUtils.h"
#ifdef ENABLE_DEV_REMOTE_FLASH
#include "pocket_daily/product_identity.h"
#endif

namespace PocketDaily::Web {
namespace {

// Keep diagnostic responses below the Pocket private AP's scarce contiguous
// heap and return to the global loop between every piece. The previous 512 B
// write loop could block inside lwIP long enough to trip the task watchdog.
// Each diagnostic request reads one chunk into the shared static staging
// buffer and answers with one bounded socket write. A 53 KB X3 frame needs
// ~13 requests instead of ~52.
constexpr size_t CRASH_REPORT_CHUNK_BYTES = 1024;
constexpr size_t SCREEN_PREVIEW_CHUNK_BYTES = firmware_flash::STAGING_BUFFER_BYTES;
// A diagnostic chunk send blocks the loop task. Unlike an SD write it can
// hang forever if the peer vanishes, so the loop watchdog must stay armed;
// bounding the socket below the 5 s task-WDT window guarantees the send
// returns (completed or aborted) before the watchdog could fire.
constexpr unsigned long DIAGNOSTIC_SEND_TIMEOUT_MS = 3000;

// LS-2 live frame fetch: same chunked octet-stream contract as the one-shot
// screen preview, but reading the latest captured frame. Single slot — the
// companion associates the fetched bytes with the newest `frame` event.
// The fetch gate sits below the diagnostics floor on purpose: a 4 KiB shared
// -buffer read plus one bounded send is far lighter than the crash report,
// and an X3 in STA File Transfer idles within a few hundred bytes of the
// 10 KiB diagnostics floor, which starved multi-chunk fetches in testing.
constexpr uint32_t LIVE_FETCH_MIN_FREE_HEAP = 6U * 1024U;

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
  if ((d.host.httpUploadBusy && d.host.httpUploadBusy(d.host.self)) || (d.stream && d.stream->transferActive())) {
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

bool admitContentOperation(WebServer& server, const RouteDeps& d, char (&deviceId)[9]) {
  if ((d.host.httpUploadBusy && d.host.httpUploadBusy(d.host.self)) || (d.stream && d.stream->receiving())) {
    server.send(409, "text/plain", "Another file transfer is active");
    return false;
  }
  snprintf(deviceId, sizeof(deviceId), "%08lX", static_cast<unsigned long>(ESP.getEfuseMac() & 0xFFFFFFFFUL));
  if (server.arg("deviceID") != deviceId) {
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
  char* end = nullptr;
  const unsigned long generation = strtoul(expected.c_str(), &end, 10);
  if (expected.isEmpty() || expected.length() > 10 || !end || *end || generation > UINT32_MAX) {
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

void sendPresentation(WebServer& server, const char* deviceId, const Content::PresentationReceipt& receipt) {
  const char* phase = "idle";
  switch (receipt.phase) {
    case Content::PresentationPhase::Idle:
      break;
    case Content::PresentationPhase::Queued:
      phase = "queued";
      break;
    case Content::PresentationPhase::Rendered:
      phase = "rendered";
      break;
    case Content::PresentationPhase::Failed:
      phase = "failed";
      break;
  }
  char body[224];
  const char* failure = "none";
  switch (receipt.failure) {
    case Content::PresentationFailure::None:
      break;
    case Content::PresentationFailure::Memory:
      failure = "memory";
      break;
    case Content::PresentationFailure::Preparation:
      failure = "preparation";
      break;
    case Content::PresentationFailure::Display:
      failure = "display";
      break;
  }
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

// Pocket clients upload to a unique hidden .part file, then ask the reader
// to verify size + CRC and atomically publish it. A dropped phone or Wi-Fi
// link therefore never turns a valid book or update.bin into a partial file.
void handleCommitUpload(WebServer& server, const RouteDeps& d) {
  if ((d.host.httpUploadBusy && d.host.httpUploadBusy(d.host.self)) || (d.stream && d.stream->receiving())) {
    server.send(409, "text/plain", "Another file transfer is active");
    return;
  }
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "Missing JSON body");
    return;
  }

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, server.arg("plain"));
  if (error || !doc["staging"].is<const char*>() || !doc["target"].is<const char*>() || !doc["size"].is<size_t>() ||
      !doc["crc32"].is<const char*>()) {
    server.send(400, "text/plain", "Invalid commit request");
    return;
  }

  const String staging = norm(d, doc["staging"].as<const char*>());
  const String target = norm(d, doc["target"].as<const char*>());
  if (!Content::genericContentWriteAllowed({staging.c_str(), staging.length()}) ||
      !Content::genericContentWriteAllowed({target.c_str(), target.length()})) {
    server.send(403, "text/plain", "Protected or unsafe content path");
    return;
  }
  const size_t expectedSize = doc["size"].as<size_t>();
  if (!Content::contentWriteWithinBudget({target.c_str(), target.length()}, 0, expectedSize)) {
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
    server.send(400, "text/plain", "Unsafe commit path or checksum");
    return;
  }

  const StagedUpload& staged = d.stream->staged();
  String uploadedPath = norm(d, staged.path + "/" + staged.fileName);
  const uint32_t uploadedCrc = staged.crc32 ^ 0xFFFFFFFFU;
  if (!staged.success || uploadedPath != staging || staged.size != expectedSize || uploadedCrc != expectedCrc ||
      !Storage.exists(staging.c_str())) {
    server.send(409, "text/plain", "Staged upload verification failed");
    return;
  }

  HalFile stagedFile = Storage.open(staging.c_str());
  const bool stagedSizeMatches = stagedFile && !stagedFile.isDirectory() && stagedFile.size() == expectedSize;
  if (stagedFile) stagedFile.close();
  if (!stagedSizeMatches) {
    server.send(409, "text/plain", "Staged file size mismatch");
    return;
  }

  const String backup = stagingParent + ".pocket-backup.part";
  Storage.remove(backup.c_str());
  const bool hadTarget = Storage.exists(target.c_str());
  if (hadTarget && !renameStorageFile(target, backup)) {
    server.send(500, "text/plain", "Could not preserve existing target");
    return;
  }
  if (!renameStorageFile(staging, target)) {
    if (hadTarget) renameStorageFile(backup, target);
    server.send(500, "text/plain", "Could not publish staged upload");
    return;
  }
  if (hadTarget) Storage.remove(backup.c_str());

  clearBookCache(target.c_str());
  char response[80];
  snprintf(response, sizeof(response), "{\"size\":%u,\"crc32\":\"%08lX\"}", static_cast<unsigned>(expectedSize),
           static_cast<unsigned long>(uploadedCrc));
  server.send(200, "application/json", response);
  LOG_INF("WEB", "Committed Pocket upload %s (%u bytes, crc32=%08lX)", target.c_str(),
          static_cast<unsigned>(expectedSize), static_cast<unsigned long>(uploadedCrc));
}

void handleScreenPreview(WebServer& server) {
  HalSystem::setCrashBreadcrumb("nearby:screen-preview");
  if (!diagnosticsAffordable()) {
    server.send(503, "text/plain", "Reader memory is too low for diagnostics right now");
    return;
  }
  HalFile preview = Storage.open(PocketDaily::SCREEN_PREVIEW_PATH);
  if (!preview || preview.isDirectory()) {
    if (preview) preview.close();
    server.send(404, "text/plain", "No Pocket Daily screen preview available");
    return;
  }

  if (!server.hasArg("offset")) {
    preview.close();
    server.send(400, "text/plain", "Missing screen preview offset");
    return;
  }

  const String offsetText = server.arg("offset");
  if (offsetText.isEmpty()) {
    preview.close();
    server.send(400, "text/plain", "Invalid screen preview offset");
    return;
  }
  for (size_t i = 0; i < offsetText.length(); i++) {
    if (offsetText[i] < '0' || offsetText[i] > '9') {
      preview.close();
      server.send(400, "text/plain", "Invalid screen preview offset");
      return;
    }
  }

  const size_t previewSize = preview.size();
  const size_t offset = static_cast<size_t>(offsetText.toInt());
  if (offset >= previewSize || !preview.seek(offset)) {
    preview.close();
    server.send(416, "text/plain", "Screen preview offset out of range");
    return;
  }

  uint8_t* body = firmware_flash::sharedStagingBuffer();
  const size_t requested = std::min(previewSize - offset, SCREEN_PREVIEW_CHUNK_BYTES);
  const int count = preview.read(body, requested);
  preview.close();
  if (count <= 0) {
    server.send(500, "text/plain", "Could not read screen preview chunk");
    return;
  }

  // The blocking TCP send can exceed the private-AP loop watchdog window on a
  // weak link (measured: a task-watchdog reset at nearby:screen-preview while
  // streaming 4 KiB chunks). Suspend the loop task's watchdog around the send,
  // exactly as the SD upload path does, and bound the peer wait so a vanished
  // companion cannot hang the reader while the watchdog is off.
  // Keep the watchdog armed; the bounded socket timeout, not a WDT suspension,
  // is what prevents a stuck send from either hanging the reader or tripping
  // the 5 s task watchdog.
  server.client().setTimeout(DIAGNOSTIC_SEND_TIMEOUT_MS);
  feedLoopWDT();
  server.setContentLength(static_cast<size_t>(count));
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/octet-stream", "");
  server.sendContent(reinterpret_cast<const char*>(body), static_cast<size_t>(count));
  feedLoopWDT();
  LOG_DBG("WEB", "Pocket screen preview offset=%u bytes=%u total=%u", static_cast<unsigned>(offset),
          static_cast<unsigned>(count), static_cast<unsigned>(previewSize));
}

// Pocket only needs four preferences. Avoid constructing and streaming the
// full localized settings registry on the no-PSRAM private-AP path: that
// response can consume the last contiguous heap immediately after Wi-Fi
// starts and strand the X3 on its retained Hotspot Mode frame.
void handleGetPreferences(WebServer& server) {
  char json[192];
  const int written = snprintf(
      json, sizeof(json), "{\"startupApp\":%u,\"pocketDailySleepCover\":%u,\"sleepTimeoutMinutes\":%u,\"fontSize\":%u}",
      static_cast<unsigned>(SETTINGS.startupApp), static_cast<unsigned>(SETTINGS.pocketDailySleepCover),
      static_cast<unsigned>(SETTINGS.sleepTimeoutMinutes), static_cast<unsigned>(SETTINGS.fontSize));
  if (written <= 0 || static_cast<size_t>(written) >= sizeof(json)) {
    server.send(500, "text/plain", "Could not encode Pocket preferences");
    return;
  }
  server.sendHeader("Connection", "close");
  server.send(200, "application/json", json);
}

void handlePostPreferences(WebServer& server, const RouteDeps& d) {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "Missing JSON body");
    return;
  }
  // Validate the whole body first; settings change only when every field is
  // valid, and a failed save restores the previous values (PreferencesUpdate.h).
  const String& body = server.arg("plain");
  const PreferenceLimits limits{CrossPointSettings::STARTUP_APP_COUNT, CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES,
                                CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, CrossPointSettings::FONT_SIZE_COUNT};
  PreferencesUpdate update;
  const char* error = nullptr;
  if (!parsePreferences(body.c_str(), body.length(), limits, update, error)) {
    server.send(400, "text/plain", error ? error : "Invalid preferences");
    return;
  }
  const uint8_t previous[4] = {SETTINGS.startupApp, SETTINGS.pocketDailySleepCover, SETTINGS.sleepTimeoutMinutes,
                               SETTINGS.fontSize};
  if (update.hasStartupApp) SETTINGS.startupApp = update.startupApp;
  if (update.hasSleepCover) SETTINGS.pocketDailySleepCover = update.sleepCover;
  if (update.hasSleepTimeout) SETTINGS.sleepTimeoutMinutes = update.sleepTimeoutMinutes;
  if (update.hasFontSize) SETTINGS.fontSize = update.fontSize;

  if (!SETTINGS.saveToFile()) {
    SETTINGS.startupApp = previous[0];
    SETTINGS.pocketDailySleepCover = previous[1];
    SETTINGS.sleepTimeoutMinutes = previous[2];
    SETTINGS.fontSize = previous[3];
    server.send(500, "text/plain", "Could not save Pocket preferences");
    return;
  }
  server.sendHeader("Connection", "close");
  server.send(200, "application/json", "{\"saved\":true}");
  d.liveStudio->notifyPrefsChanged();
}

void handleScreenLive(WebServer& server) {
  HalSystem::setCrashBreadcrumb("nearby:screen-live");
  if (ESP.getFreeHeap() < LIVE_FETCH_MIN_FREE_HEAP) {
    server.send(503, "text/plain", "Reader memory is too low for the live frame right now");
    return;
  }
  HalFile frame = Storage.open(PocketDaily::LiveFrameCapture::LIVE_FRAME_PATH);
  if (!frame || frame.isDirectory()) {
    if (frame) frame.close();
    server.send(404, "text/plain", "No live frame captured yet");
    return;
  }
  if (!server.hasArg("offset")) {
    frame.close();
    server.send(400, "text/plain", "Missing live frame offset");
    return;
  }
  const String offsetText = server.arg("offset");
  bool offsetValid = !offsetText.isEmpty();
  for (size_t i = 0; offsetValid && i < offsetText.length(); i++) {
    if (offsetText[i] < '0' || offsetText[i] > '9') offsetValid = false;
  }
  if (!offsetValid) {
    frame.close();
    server.send(400, "text/plain", "Invalid live frame offset");
    return;
  }
  const size_t frameSize = frame.size();
  const size_t offset = static_cast<size_t>(offsetText.toInt());
  if (offset >= frameSize || !frame.seek(offset)) {
    frame.close();
    server.send(416, "text/plain", "Live frame offset out of range");
    return;
  }
  uint8_t* body = firmware_flash::sharedStagingBuffer();
  const size_t requested = std::min(frameSize - offset, SCREEN_PREVIEW_CHUNK_BYTES);
  const int count = frame.read(body, requested);
  frame.close();
  if (count <= 0) {
    server.send(500, "text/plain", "Could not read live frame chunk");
    return;
  }
  server.client().setTimeout(DIAGNOSTIC_SEND_TIMEOUT_MS);
  feedLoopWDT();
  server.setContentLength(static_cast<size_t>(count));
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/octet-stream", "");
  server.sendContent(reinterpret_cast<const char*>(body), static_cast<size_t>(count));
  feedLoopWDT();
}

void handleUiPackList(WebServer& server, const RouteDeps& d) {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent("[");
  char line[160];
  bool first = true;
  PocketDaily::LiveStudio::listPacks([&](const char* name, size_t size) {
    const bool active = strcmp(name, d.liveStudio->activePackName()) == 0;
    const int n = snprintf(line, sizeof(line), R"({"name":"%.32s","size":%u,"active":%s})", name,
                           static_cast<unsigned>(size), active ? "true" : "false");
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(line)) return;
    if (!first) server.sendContent(",");
    first = false;
    server.sendContent(line);
  });
  server.sendContent("]");
  server.sendContent("");
}

// LS-3 apply: load + validate + layer over the current theme, persist the
// choice, and ask for a repaint so the live frame shows the result. An empty
// name reverts to the theme's own metrics.
void handleUiPackApply(WebServer& server, const RouteDeps& d) {
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }
  if (!doc["name"].is<const char*>()) {
    server.send(400, "text/plain", "Missing pack name");
    return;
  }
  const char* name = doc["name"].as<const char*>();
  if (name[0] == '\0') {
    bool saved;
    {
      RenderLock lock;
      saved = d.liveStudio->onPackCleared();
      if (saved) UITheme::getInstance().adoptPackMetrics(nullptr, 0);
    }
    if (!saved) {
      server.send(500, "text/plain", "Could not save pack state; active pack unchanged");
      return;
    }
    repaint(d);
    server.send(200, "application/json", "{\"applied\":false}");
    return;
  }
  PocketDaily::LiveStudio::UiPackInfo info;
  PocketDaily::LiveStudio::ThemeOverride* overrides = PocketDaily::LiveStudio::acquireOverrideBuffer();
  if (!overrides) {
    server.send(503, "text/plain", "Not enough memory to load the pack");
    return;
  }
  PocketDaily::LiveStudio::UiPackResult validateError = PocketDaily::LiveStudio::UiPackResult::Ok;
  const auto result = PocketDaily::LiveStudio::loadPackFromSd(
      name, &info, overrides, PocketDaily::LiveStudio::UIPACK_MAX_THEME_OVERRIDES, &validateError);
  if (result != PocketDaily::LiveStudio::StoreResult::Ok) {
    PocketDaily::LiveStudio::releaseOverrideBuffer(overrides);
    const int code = result == PocketDaily::LiveStudio::StoreResult::OpenFail ? 404 : 422;
    char body[128];
    snprintf(body, sizeof(body), "{\"applied\":false,\"error\":\"%s\"}",
             PocketDaily::LiveStudio::storeResultName(result));
    server.send(code, "application/json", body);
    return;
  }
  overrides = PocketDaily::LiveStudio::compactOverrideBuffer(overrides, info.themeOverrideCount);
  bool saved;
  {
    RenderLock lock;
    saved = d.liveStudio->onPackApplied(info.name, info.packVersion);
    if (saved) UITheme::getInstance().adoptPackMetrics(overrides, info.themeOverrideCount);
  }
  if (!saved) {
    PocketDaily::LiveStudio::releaseOverrideBuffer(overrides);
    server.send(500, "text/plain", "Could not save pack state; active pack unchanged");
    return;
  }
  repaint(d);
  char body[128];
  snprintf(body, sizeof(body), "{\"applied\":true,\"name\":\"%.32s\",\"version\":\"%.16s\",\"overrides\":%u}",
           info.name, info.packVersion, static_cast<unsigned>(info.themeOverrideCount));
  server.send(200, "application/json", body);
}

#ifdef ENABLE_DEV_REMOTE_FLASH
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
  if (d.profile == Profile::POCKET_SYNC && d.apMode) {
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

  routes.on("/api/pocket/v1/commit", HTTP_POST, [server = &server, deps = &d] {
    note(*deps);
    handleCommitUpload(*server, *deps);
  });

  if (d.profile == Profile::POCKET_SYNC) {
    routes.on("/api/pocket/v1/screen-preview", HTTP_GET, [server = &server, deps = &d] {
      note(*deps);
      handleScreenPreview(*server);
    });
  }
  if (isSyncProfile(d.profile)) {
    routes.on("/api/pocket/v1/profile", HTTP_GET, [server = &server, deps = &d] {
      note(*deps);
      handleGetProfile(*server, *deps);
    });
    routes.on("/api/pocket/v1/profile", HTTP_POST, [server = &server, deps = &d] {
      note(*deps);
      handlePostProfile(*server, *deps);
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
  if (!isSyncProfile(d.profile)) {
    // Frame streaming is optional browser functionality, not a dependency
    // of content/theme editing on either dedicated Sync bearer.
    routes.on("/api/pocket/v1/screen-live", HTTP_GET, [server = &server, deps = &d] {
      note(*deps);
      handleScreenLive(*server);
    });
  }
  // Both Sync profiles advertise uiPacks. Applying/reverting a data pack must
  // remain available without a router or WebSocket listener.
  routes.on("/api/pocket/v1/ui-packs", HTTP_GET, [server = &server, deps = &d] {
    note(*deps);
    handleUiPackList(*server, *deps);
  });
  routes.on("/api/pocket/v1/ui-pack/apply", HTTP_POST, [server = &server, deps = &d] {
    note(*deps);
    handleUiPackApply(*server, *deps);
  });
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
  // Host/device parity evidence (docs/pocket-profile-v1.md P1-1): capture the
  // completed content frame, then read it in chunks like screen-live.
  if (d.presentation.captureFrame) {
    routes.on("/api/pocket/v1/dev/capture", HTTP_POST, [server = &server, deps = &d] {
      note(*deps);
      const uint32_t bytes = deps->presentation.captureFrame(deps->presentation.self);
      if (!bytes) {
        server->send(409, "text/plain", "No completed content frame to capture");
        return;
      }
      char json[48];
      snprintf(json, sizeof(json), "{\"bytes\":%lu}", static_cast<unsigned long>(bytes));
      server->send(200, "application/json", json);
    });
    routes.on("/api/pocket/v1/dev/frame", HTTP_GET, [server = &server, deps = &d] {
      note(*deps);
      handleScreenLive(*server);
    });
  }
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
