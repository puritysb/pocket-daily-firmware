#pragma once

#include <Arduino.h>

#include <cstdint>

namespace PocketDaily::Web {

// The one sanctioned way pocket web modules reach their host (the inherited
// CrossPointWebServer). Plain function pointers: no heap, no vtable, safe
// under -fno-exceptions. The host wires a static instance once per server
// lifetime; every pointer is null-checked at the call site. `self` is the
// host object (or null for pure free-function thunks).
struct Host {
  void* self = nullptr;
  void (*noteClientActivity)(void* self) = nullptr;       // idle-lease heartbeat
  bool (*httpUploadBusy)(void* self) = nullptr;           // legacy HTTP or WS writer holds storage
  void (*releaseHttpUploadBuffer)(void* self) = nullptr;  // stream-begin heap hygiene
  void (*beginTransferFocus)(void* self) = nullptr;       // uploads own the DMA pool
  void (*endTransferFocus)(void* self) = nullptr;
  void (*requestRepaint)(void* self) = nullptr;
  String (*normalizeWebPath)(void* self, const String& path) = nullptr;
  bool (*isProtectedItemName)(void* self, const String& name) = nullptr;
};

}  // namespace PocketDaily::Web
