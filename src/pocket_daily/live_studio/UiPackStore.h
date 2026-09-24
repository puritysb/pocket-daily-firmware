#pragma once

#include <cstddef>
#include <functional>

#include "UiPack.h"

// Device side of .uipack storage: SD layout, state file, and the boot-time
// apply. v1 scope: theme-override packs (strings validated but not applied,
// font assets rejected on device until the host-renderer work lands).
namespace PocketDaily::LiveStudio {

inline constexpr char UIPACK_DIR[] = "/pocket-daily/ui-packs";
inline constexpr char UIPACK_STATE_PATH[] = "/.crosspoint/ui-pack.state";
// A pack the reader can apply must stay far below the File Transfer heap
// floor; 8 KiB covers the 96-override + 32-string maximum many times over.
inline constexpr size_t UIPACK_DEVICE_MAX_BYTES = 8 * 1024;

enum class StoreResult : uint8_t {
  Ok = 0,
  OpenFail,
  TooBig,
  Oom,
  ReadFail,
  ShaMismatch,
  ValidateFailed,
};

// Loads `<UIPACK_DIR>/<name>.uipack`, verifies SHA-256 when present,
// validates, and copies theme overrides out.
StoreResult loadPackFromSd(const char* name, UiPackInfo* info, ThemeOverride* overrides, size_t cap,
                           UiPackResult* validateError);

// Streams `(name, size)` for every stored pack, extension stripped, in
// directory order. Dot-prefixed entries follow the showHiddenFiles setting,
// as the web file browser does. Streaming callback: no intermediate
// allocation. False when the pack directory cannot be opened (nothing is
// reported; callers answer as for an empty store).
bool listPacks(const std::function<void(const char* name, size_t size)>& fn);

// Alternating CRC/generation slots; verified readback precedes activation.
// Legacy text is read only until the first successful migration. Empty name reverts.
bool writeState(const char* name, const char* version);
bool readState(char* name, size_t nameCap, char* versionCap, size_t versionCap2);
bool clearState();
// Actual runtime activation, not merely a persisted selection that might fail boot validation.
const char* activeName();
const char* activeVersion();
void noteActive(const char* name, const char* version);

// Override scratch, heap-allocated for the duration of a load and freed by
// releaseOverrideBuffer() - the idle baseline carries no pack buffers.
ThemeOverride* acquireOverrideBuffer();
// Best-effort shrink before activation; failure retains the valid bounded buffer.
ThemeOverride* compactOverrideBuffer(ThemeOverride* buffer, size_t count);
void releaseOverrideBuffer(ThemeOverride* buffer);

// Boot path: read state, load the pack, layer it over the current theme.
bool applyStartupPack();

const char* storeResultName(StoreResult r);

}  // namespace PocketDaily::LiveStudio
