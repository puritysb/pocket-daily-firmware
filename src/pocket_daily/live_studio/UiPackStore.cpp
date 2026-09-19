#include "UiPackStore.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <mbedtls/sha256.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "components/UITheme.h"

namespace PocketDaily::LiveStudio {

namespace {
void packPath(const char* name, char* out, size_t cap) { snprintf(out, cap, "%s/%.24s.uipack", UIPACK_DIR, name); }
}  // namespace

StoreResult loadPackFromSd(const char* name, UiPackInfo* info, ThemeOverride* overrides, size_t cap,
                           UiPackResult* validateError) {
  char path[80];
  packPath(name, path, sizeof(path));
  HalFile file = Storage.open(path);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    return StoreResult::OpenFail;
  }
  const size_t size = file.size();
  if (size > UIPACK_DEVICE_MAX_BYTES) {
    file.close();
    return StoreResult::TooBig;
  }
  uint8_t* buffer = static_cast<uint8_t*>(malloc(size ? size : 1));
  if (!buffer) {
    file.close();
    return StoreResult::Oom;
  }
  if (file.read(buffer, size) != static_cast<int>(size)) {
    free(buffer);
    file.close();
    return StoreResult::ReadFail;
  }
  file.close();

  // SHA-256 when the header carries one (nonzero).
  const uint8_t* sha = buffer + 80;
  bool shaPresent = false;
  for (size_t i = 0; i < 32 && !shaPresent; i++) {
    if (sha[i] != 0) shaPresent = true;
  }
  if (shaPresent) {
    uint8_t digest[32];
    const uint32_t payloadLen = buffer[72] | (buffer[73] << 8) | (buffer[74] << 16) | (buffer[75] << 24);
    if (payloadLen + UIPACK_HEADER_SIZE != size ||
        mbedtls_sha256(buffer + UIPACK_HEADER_SIZE, payloadLen, digest, 0) != 0 || memcmp(digest, sha, 32) != 0) {
      free(buffer);
      return StoreResult::ShaMismatch;
    }
  }

  const UiPackResult result = validatePack(buffer, size, info, overrides, cap);
  free(buffer);
  if (result != UiPackResult::Ok) {
    if (validateError) *validateError = result;
    return StoreResult::ValidateFailed;
  }
  if (info != nullptr && info->assetCount > 0) {
    // Font assets ship in the container format but applying them on device
    // waits for the host-renderer milestone; refuse rather than half-apply.
    return StoreResult::ValidateFailed;
  }
  return StoreResult::Ok;
}

bool writeState(const char* name, const char* version) {
  HalFile file;
  if (!Storage.openFileForWrite("PCK", UIPACK_STATE_PATH, file) || !file) return false;
  if (name == nullptr || name[0] == '\0') {
    const char empty[] = "name=\n";
    file.write(reinterpret_cast<const uint8_t*>(empty), sizeof(empty) - 1);
  } else {
    char line[80];
    int n = snprintf(line, sizeof(line), "name=%.32s\nversion=%.16s\n", name, version ? version : "");
    if (n <= 0) {
      file.close();
      return false;
    }
    file.write(reinterpret_cast<const uint8_t*>(line), static_cast<size_t>(n));
  }
  file.close();
  return true;
}

bool readState(char* name, size_t nameCap, char* versionCap, size_t versionCap2) {
  name[0] = '\0';
  if (versionCap) versionCap[0] = '\0';
  HalFile file = Storage.open(UIPACK_STATE_PATH);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    return false;
  }
  char line[96];
  while (file.available()) {
    int n = file.read(reinterpret_cast<uint8_t*>(line), sizeof(line) - 1);
    if (n <= 0) break;
    line[n] = '\0';
    char* nl = strchr(line, '\n');
    if (!nl) break;
    *nl = '\0';
    if (strncmp(line, "name=", 5) == 0) {
      snprintf(name, nameCap, "%s", line + 5);
    } else if (strncmp(line, "version=", 8) == 0 && versionCap) {
      snprintf(versionCap, versionCap2, "%s", line + 8);
    }
    // Only the first pair matters.
    if (name[0] != '\0' && versionCap && versionCap[0] != '\0') break;
  }
  file.close();
  return name[0] != '\0';
}

void clearState() { writeState("", ""); }

bool applyStartupPack() {
  char name[33];
  char version[17];
  if (!readState(name, sizeof(name), version, sizeof(version)) || name[0] == '\0') return false;
  UiPackInfo info;
  static ThemeOverride overrides[UIPACK_MAX_THEME_OVERRIDES];
  UiPackResult validateError = UiPackResult::Ok;
  if (loadPackFromSd(name, &info, overrides, UIPACK_MAX_THEME_OVERRIDES, &validateError) != StoreResult::Ok) {
    LOG_ERR("PCK", "Startup pack '%s' failed to load; reverting to theme metrics", name);
    return false;
  }
  UITheme::getInstance().applyPackMetrics(overrides, info.themeOverrideCount);
  LOG_INF("PCK", "Applied UI pack '%s' v%s (%u overrides)", name, version,
          static_cast<unsigned>(info.themeOverrideCount));
  return true;
}

const char* storeResultName(StoreResult r) {
  switch (r) {
    case StoreResult::Ok:
      return "ok";
    case StoreResult::OpenFail:
      return "pack not found";
    case StoreResult::TooBig:
      return "pack too large for on-device apply";
    case StoreResult::Oom:
      return "out of memory";
    case StoreResult::ReadFail:
      return "read failed";
    case StoreResult::ShaMismatch:
      return "sha256 mismatch";
    case StoreResult::ValidateFailed:
      return "validation failed";
  }
  return "?";
}

}  // namespace PocketDaily::LiveStudio
