#pragma once

#include <WebSocketsServer.h>

#include <cstdint>
#include <memory>

#include "pocket_daily/web/PocketStatus.h"
#include "pocket_daily/web/Profile.h"
#include "pocket_daily/web/TransferFocus.h"

namespace PocketDaily::Web {

// Host reach-in for the Live Studio push listener, same contract as Host.h:
// plain function pointers, no heap, no vtable, -fno-exceptions safe. The
// listener's WebSocketsServer allocation lives in a host-owned slot because
// the inherited file keeps the legacy WS upload grammar against that same
// server; wire/unwire bracket the allocation so the host's C-style event
// trampoline (file-static wsInstance) stays inherited.
struct LiveHost {
  void* self = nullptr;
  std::unique_ptr<WebSocketsServer>* wsSlot = nullptr;
  // Called right after a successful allocation, in the original order:
  // wsInstance install, begin(), onEvent(...).
  void (*wireListener)(void* self, WebSocketsServer& server) = nullptr;
  // Called after the server is closed and the slot reset: wsInstance = nullptr.
  void (*unwireListener)(void* self) = nullptr;
  StatusInputs (*statusInputs)(void* self) = nullptr;
  // Legacy WebSocket upload in progress (host file-static): the push channel
  // never allocates while a transfer owns the heap.
  bool (*legacyUploadBusy)(void* self) = nullptr;
};

// Live Studio v1 (`docs/live-studio-v1.md`): subscription, change-signature,
// and send pacing state for the WebSocket push channel, plus transfer focus
// (uploads own the DMA pool; the listener's buffers are torn down for the
// duration and rebuilt afterwards) and the persisted active-pack
// advertisement. The wire encoding lives in LiveStudioEvents.h; this class
// owns only per-connection state and lifecycle.
class LiveStudioService final {
 public:
  struct Config {
    bool apMode = false;
    Profile profile = Profile::FULL;
    uint16_t wsPort = 81;
    const UploadStreamServer* stream = nullptr;  // transfer gating + signature
  };

  void begin(const Config& config, LiveHost* host);
  // The server's stop() path: best-effort bye, close, reset slot, forget state.
  void onServerStopping();

  // One handleClient slice when the listener exists: status push + frame
  // events, gated on push capability and an active subscription.
  void tick();

  // WebSocket event routing (the legacy upload grammar stays with the host).
  void onWsConnected(uint8_t num);
  void onWsDisconnected(uint8_t num);
  // True if the payload was a Live Studio JSON control message (consumed);
  // false -> the caller continues with the legacy upload grammar.
  bool onWsText(uint8_t num, const uint8_t* payload, size_t length);

  void notifyPrefsChanged();
  void beginTransferFocus();
  void endTransferFocus();
  // Pack apply/clear: persist + record the advertisement the status builder
  // and the ui-packs listing read back.
  bool onPackApplied(const char* name, const char* packVersion);
  bool onPackCleared();

  // Status/listing inputs.
  bool pushActive() const { return liveStudioPush && host_ && host_->wsSlot && *host_->wsSlot; }
  bool listenerSuspended() const { return liveListenerSuspended; }
  const char* activePackName() const;
  const char* activePackVersion() const;

 private:
  void startListener();
  void suspendListener();
  void resumeListener();
  void sendLine(const char* line);
  void pushStatusIfChanged();

  LiveHost* host_ = nullptr;
  Profile profile_ = Profile::FULL;
  bool apMode_ = false;
  uint16_t wsPort_ = 81;
  const UploadStreamServer* stream_ = nullptr;

  bool liveStudioPush = false;
  bool liveStudioSubscribed = false;
  bool liveStudioClientAttached = false;
  uint8_t liveStudioClientNum = 255;
  uint32_t liveStudioLastSendMs = 0;
  uint32_t liveStudioLastCheckMs = 0;
  uint32_t liveStudioLastSignatureMs = 0;
  char liveStudioSignature[128] = {};
  bool liveListenerSuspended = false;
  TransferFocus transferFocus_;
};

}  // namespace PocketDaily::Web
