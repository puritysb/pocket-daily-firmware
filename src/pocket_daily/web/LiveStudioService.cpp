#include "pocket_daily/web/LiveStudioService.h"

#include <Arduino.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <Memory.h>

#include "pocket_daily/live_studio/DevTrace.h"
#include "pocket_daily/live_studio/LiveFrameCapture.h"
#include "pocket_daily/live_studio/LiveStudioEvents.h"
#include "pocket_daily/live_studio/UiPackStore.h"
#include "pocket_daily/web/UploadStreamServer.h"

namespace PocketDaily::Web {

void LiveStudioService::begin(const Config& config, LiveHost* host) {
  host_ = host;
  profile_ = config.profile;
  apMode_ = config.apMode;
  wsPort_ = config.wsPort;
  stream_ = config.stream;

  // The generic browser/File Transfer surface keeps its legacy WebSocket
  // upload surface. On STA the same listener additionally serves Live Studio
  // v1 event push when heap allows (`docs/live-studio-v1.md`). Both dedicated
  // Sync profiles stay poll-only to reserve memory for verified transfers.
  liveStudioPush =
      allowsLivePush(profile_, apMode_) && ESP.getFreeHeap() >= PocketDaily::LiveStudio::kMinListenerFreeHeap;
  if (!host_ || !host_->wsSlot || !host_->wireListener) {
    LOG_ERR("WEB", "Live listener host not wired; push stays off");
    liveStudioPush = false;
    return;
  }
  if (profile_ == Profile::FULL || liveStudioPush) startListener();
}

void LiveStudioService::startListener() {
  if (isSyncProfile(profile_)) return;
  if (*host_->wsSlot) return;
  LOG_DBG("WEB", "Starting WebSocket server on port %d (liveStudioPush=%d)...", wsPort_, liveStudioPush);
  *host_->wsSlot = makeUniqueNoThrow<WebSocketsServer>(wsPort_);
  if (*host_->wsSlot) {
    host_->wireListener(host_->self, **host_->wsSlot);
    LOG_DBG("WEB", "WebSocket server started");
  } else {
    if (profile_ == Profile::FULL) {
      LOG_ERR("WEB", "Could not allocate WebSocket server; HTTP File Transfer remains available");
    } else {
      liveStudioPush = false;
      LOG_INF("WEB", "Live listener stays off (allocation failed)");
    }
  }
}

void LiveStudioService::onServerStopping() {
  if (!host_ || !host_->wsSlot || !*host_->wsSlot) return;
  if (liveStudioPush) {
    // Best-effort notice so a studio client distinguishes an intentional
    // stop (mode change, shutdown) from a dropped link.
    char bye[32];
    if (PocketDaily::LiveStudio::encodeBye(bye, sizeof(bye))) (*host_->wsSlot)->broadcastTXT(bye);
  }
  (*host_->wsSlot)->close();
  host_->wsSlot->reset();
  if (host_->unwireListener) host_->unwireListener(host_->self);
  liveStudioPush = false;
  liveStudioClientAttached = false;
  liveStudioSubscribed = false;
  PocketDaily::LiveFrameCapture::clear();
  LOG_DBG("WEB", "WebSocket server stopped");
}

void LiveStudioService::suspendListener() {
  if (!host_ || !host_->wsSlot || liveListenerSuspended || !*host_->wsSlot) return;
  liveListenerSuspended = true;
  liveStudioSubscribed = false;
  liveStudioClientAttached = false;
  liveStudioClientNum = 255;
  PocketDaily::LiveFrameCapture::clear();
  LOG_INF("WEB", "Live listener suspended for transfer focus");
  if (host_->unwireListener) host_->unwireListener(host_->self);
  (*host_->wsSlot)->close();
  host_->wsSlot->reset();
}

void LiveStudioService::resumeListener() {
  if (isSyncProfile(profile_)) return;
  if (!liveListenerSuspended) return;
  if (!transferFocus_.allowsListener(millis()) || ESP.getFreeHeap() < PocketDaily::LiveStudio::kMinListenerFreeHeap)
    return;
  if (!liveStudioPush && profile_ != Profile::FULL) return;
  LOG_INF("WEB", "Live listener resuming after transfer");
  startListener();
  liveListenerSuspended = !*host_->wsSlot;
}

void LiveStudioService::sendLine(const char* line) {
  if (!host_ || !host_->wsSlot || !*host_->wsSlot || !liveStudioPush) return;
  // Guard before sending: links2004's send path blocks on a peer that died
  // without a close handshake, and the 15 s keepalive kept feeding those
  // dead sends - the activity loop crawled at ~one pass per TCP retransmit
  // cycle (~30 s) until power cycle. Verify the client, send to the single
  // subscribed client only, and tear the subscription down on a miss.
  if (liveStudioClientNum == 255 || !(*host_->wsSlot)->clientIsConnected(liveStudioClientNum)) {
    liveStudioSubscribed = false;
    liveStudioClientAttached = false;
    liveStudioClientNum = 255;
    PocketDaily::LiveFrameCapture::clear();
    return;
  }
  DEV_TRACE(PocketDaily::DevTrace::SEND_START);
  (*host_->wsSlot)->sendTXT(liveStudioClientNum, line);
  DEV_TRACE(PocketDaily::DevTrace::SEND_DONE);
}

void LiveStudioService::pushStatusIfChanged() {
  // Never allocate for the push channel while the port-82 upload stream or a
  // legacy WS upload is mid-transfer: that heap belongs to the transfer.
  if ((stream_ && stream_->transferActive()) ||
      (host_ && host_->legacyUploadBusy && host_->legacyUploadBusy(host_->self)))
    return;
  if (!host_ || !host_->statusInputs) return;
  if (!liveStudioClientAttached) return;
  const uint32_t now = millis();
  if (static_cast<uint32_t>(now - liveStudioLastCheckMs) < 1000) return;
  liveStudioLastCheckMs = now;
  DEV_TRACE(PocketDaily::DevTrace::TICK);

  // Fast-moving fields (uptime, rssi, freeHeap) do not trigger a send; the
  // signature covers the stable identity/capability fields the studio reacts
  // to. A periodic keepalive re-pushes the full status including live values.
  // Preview/session-end fields never vary in push mode (STA), so they stay
  // out of the signature.
  char signature[128];
  char deviceId[9];
  snprintf(deviceId, sizeof(deviceId), "%08lX", static_cast<unsigned long>(ESP.getEfuseMac() & 0xFFFFFFFFUL));
  snprintf(signature, sizeof(signature), "%s|%s|%s|%s|%d", CROSSPOINT_VERSION, apMode_ ? "AP" : "STA",
           gpio.deviceIsX3() ? "X3" : "X4", deviceId, stream_ && stream_->listening() ? 1 : 0);
  const bool changed = strcmp(signature, liveStudioSignature) != 0;
  // No periodic keepalive re-send: a dead WS peer turns every queued send
  // into a multi-second TCP retransmit stall on this no-PSRAM loop (see
  // sendLine). Live values (uptime, heap, rssi) are the app's job to poll
  // over HTTP; the WS push exists for change events.
  const bool spaced =
      static_cast<uint32_t>(now - liveStudioLastSendMs) >= PocketDaily::LiveStudio::kMinStatusIntervalMs;
  if (!changed || !spaced) return;

  const String json = buildStatusJson(host_->statusInputs(host_->self));
  char event[1024];
  if (!PocketDaily::LiveStudio::encodeStatusEvent(event, sizeof(event), json.c_str())) return;
  sendLine(event);
  strlcpy(liveStudioSignature, signature, sizeof(liveStudioSignature));
  liveStudioLastSendMs = now;
  liveStudioLastSignatureMs = now;
}

void LiveStudioService::tick() {
  resumeListener();
  if (!liveStudioPush || !liveStudioSubscribed) return;
  pushStatusIfChanged();
  // A frame landed since the last tick: notify, and the companion fetches
  // it over the chunked HTTP path.
  if (PocketDaily::LiveFrameCapture::consumeReady()) {
    char event[96];
    if (PocketDaily::LiveStudio::encodeFrameEvent(event, sizeof(event), PocketDaily::LiveFrameCapture::seq(),
                                                  PocketDaily::LiveFrameCapture::bytes())) {
      sendLine(event);
    }
  }
}

void LiveStudioService::onWsConnected(uint8_t num) {
  LOG_DBG("WS", "Client %u connected", num);
  if (liveStudioPush) {
    liveStudioClientAttached = true;
    liveStudioClientNum = num;
    char deviceId[9];
    snprintf(deviceId, sizeof(deviceId), "%08lX", static_cast<unsigned long>(ESP.getEfuseMac() & 0xFFFFFFFFUL));
    char hello[192];
    if (PocketDaily::LiveStudio::encodeHello(hello, sizeof(hello), deviceId, CROSSPOINT_VERSION)) {
      (*host_->wsSlot)->sendTXT(num, hello);
    }
  }
}

void LiveStudioService::onWsDisconnected(uint8_t num) {
  if (num == liveStudioClientNum) {
    liveStudioClientAttached = false;
    liveStudioSubscribed = false;
    liveStudioClientNum = 255;
    PocketDaily::LiveFrameCapture::clear();
  }
}

bool LiveStudioService::onWsText(uint8_t num, const uint8_t* payload, size_t length) {
  // Live Studio v1 control messages are JSON objects; the legacy upload
  // grammar is line-based and never starts with '{'. JSON wins on every
  // profile so a browser client cannot trigger upload state off-profile.
  if (length == 0 || payload[0] != '{') return false;
  PocketDaily::LiveStudio::Subscription subscription;
  switch (PocketDaily::LiveStudio::parseClientMessage(reinterpret_cast<const char*>(payload), length, &subscription)) {
    case PocketDaily::LiveStudio::ClientMessage::Subscribe:
      liveStudioSubscribed = true;
      liveStudioClientAttached = true;
      // Frame capture runs only where the push listener runs (STA);
      // elsewhere the subscription is a status-only subscription.
      PocketDaily::LiveFrameCapture::setRequested(liveStudioPush && subscription.frames, subscription.minIntervalMs);
      // Snapshot immediately: the studio should not wait for a change
      // to learn the current state.
      liveStudioLastCheckMs = 0;
      liveStudioLastSignatureMs = 0;
      liveStudioSignature[0] = '\0';
      LOG_DBG("WS", "Live studio subscribed (frames=%d minIntervalMs=%lu)", subscription.frames,
              static_cast<unsigned long>(subscription.minIntervalMs));
      pushStatusIfChanged();
      break;
    case PocketDaily::LiveStudio::ClientMessage::Unsubscribe:
      liveStudioSubscribed = false;
      PocketDaily::LiveFrameCapture::clear();
      break;
    case PocketDaily::LiveStudio::ClientMessage::Ping: {
      char pong[32];
      if (PocketDaily::LiveStudio::encodePong(pong, sizeof(pong))) (*host_->wsSlot)->sendTXT(num, pong);
      break;
    }
    case PocketDaily::LiveStudio::ClientMessage::None:
      break;
  }
  return true;
}

void LiveStudioService::notifyPrefsChanged() {
  // Tell a subscribed live-studio client that preferences changed so it can
  // refetch without waiting for its next poll.
  if (liveStudioPush && liveStudioSubscribed) {
    char prefs[48];
    if (PocketDaily::LiveStudio::encodePrefsChanged(prefs, sizeof(prefs))) sendLine(prefs);
  }
}

void LiveStudioService::beginTransferFocus() {
  transferFocus_.begin();
  suspendListener();
}

void LiveStudioService::endTransferFocus() { transferFocus_.end(millis()); }

bool LiveStudioService::onPackApplied(const char* name, const char* packVersion) {
  if (!PocketDaily::LiveStudio::writeState(name, packVersion)) return false;
  PocketDaily::LiveStudio::noteActive(name, packVersion);
  return true;
}

bool LiveStudioService::onPackCleared() {
  if (!PocketDaily::LiveStudio::clearState()) return false;
  PocketDaily::LiveStudio::noteActive("", "");
  return true;
}

const char* LiveStudioService::activePackName() const { return PocketDaily::LiveStudio::activeName(); }
const char* LiveStudioService::activePackVersion() const { return PocketDaily::LiveStudio::activeVersion(); }

}  // namespace PocketDaily::Web
