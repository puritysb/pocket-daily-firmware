#pragma once

#include <Arduino.h>

#include <cstdint>

#include "pocket_daily/web/Profile.h"

namespace PocketDaily::Web {

class UploadStreamServer;

// Live Studio advertisement inputs. Host-owned until LiveStudioService owns
// them; a value snapshot keeps this module free of host pointers.
struct LiveInputs {
  bool push = false;       // listener can serve event push (STA, heap allows)
  bool suspended = false;  // torn down for transfer focus
  uint16_t wsPort = 0;
  char activePackName[33] = {};
  char activePackVersion[17] = {};
};

// Everything buildStatusJson reads from its host. Value snapshot on purpose:
// /api/status runs on the HTTP poll path, never per-frame, so copying the
// pack strings is cheaper than a lifetime-bearing pointer.
struct StatusInputs {
  bool apMode = false;
  Profile profile = Profile::FULL;
  const UploadStreamServer* stream = nullptr;  // null: listener not begun
  bool contentPresentation = false;
  LiveInputs live;
};

// Full /api/status body. The upstream base fields (deviceID, version, ip,
// mode, rssi, freeHeap, uptime, device) are a mirror obligation tracked in
// docs/SEAM.md: upstream's home page parses them, so they track upstream's
// handleStatus even though this module owns the builder.
String buildStatusJson(const StatusInputs& in);

// Below this free-heap floor the reader answers 503 for crash-report /
// screen-preview diagnostics instead of queueing chunked fetches at a
// starved private-AP reader; transfers remain available.
bool diagnosticsAffordable();

}  // namespace PocketDaily::Web
