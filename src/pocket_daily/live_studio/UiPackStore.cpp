#include "UiPackStore.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_task_wdt.h>
#include <mbedtls/sha256.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "CrossPointSettings.h"
#include "UiPackState.h"
#include "components/UITheme.h"

namespace PocketDaily::LiveStudio {

namespace {
// Replaces LiveStudioService's 50-byte copy; shared by boot and network sessions.
char activeNameValue[25]{};
char activeVersionValue[17]{};
void packPath(const char* name, char* out, size_t cap) { snprintf(out, cap, "%s/%s.uipack", UIPACK_DIR, name); }
constexpr const char* STATE_SLOTS[] = {"/.crosspoint/ui-pack.state.0", "/.crosspoint/ui-pack.state.1"};

bool readSlot(int slot, PackState::Record& record) {
  HalFile file = Storage.open(STATE_SLOTS[slot]);
  uint8_t bytes[PackState::BYTES];
  return file && file.size() == sizeof(bytes) && file.read(bytes, sizeof(bytes)) == sizeof(bytes) &&
         PackState::decode(bytes, sizeof(bytes), record);
}
struct PackFileSource {
  HalFile& file;
  mbedtls_sha256_context sha;
  explicit PackFileSource(HalFile& input) : file(input) { mbedtls_sha256_init(&sha); }
  ~PackFileSource() { mbedtls_sha256_free(&sha); }
  static bool read(void* context, size_t offset, uint8_t* output, size_t bytes) {
    auto& input = static_cast<PackFileSource*>(context)->file;
    return input.seek(offset) && input.read(output, bytes) == static_cast<int>(bytes);
  }
  static bool digest(void* context, const uint8_t* data, size_t bytes) {
    return mbedtls_sha256_update(&static_cast<PackFileSource*>(context)->sha, data, bytes) == 0;
  }
};

StoreResult validateFile(HalFile& file, size_t size, UiPackInfo* info, ThemeOverride* overrides, size_t cap,
                         UiPackResult* validateError) {
  static_assert(sizeof(PackFileSource) + sizeof(UiPackSource) + 64 + 16 < 256,
                "Keep the pack validation locals within the embedded stack budget");
  PackFileSource input(file);
  if (mbedtls_sha256_starts(&input.sha, 0) != 0) return StoreResult::ShaMismatch;
  uint8_t expected[32];
  const UiPackSource source{&input, PackFileSource::read, PackFileSource::digest};
  const auto result = validatePackSource(source, size, info, overrides, cap, expected);
  if (result != UiPackResult::Ok) {
    if (validateError) *validateError = result;
    return result == UiPackResult::ReadFail ? StoreResult::ReadFail : StoreResult::ValidateFailed;
  }
  bool shaPresent = false;
  for (const auto byte : expected) shaPresent |= byte != 0;
  uint8_t actual[32];
  if (mbedtls_sha256_finish(&input.sha, actual) != 0 || (shaPresent && memcmp(actual, expected, 32) != 0))
    return StoreResult::ShaMismatch;
  return StoreResult::Ok;
}
}  // namespace

StoreResult loadPackFromSd(const char* name, UiPackInfo* info, ThemeOverride* overrides, size_t cap,
                           UiPackResult* validateError) {
  if (!PackState::validName(name)) return StoreResult::ValidateFailed;
  char path[80];
  packPath(name, path, sizeof(path));
  HalFile file = Storage.open(path);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    return StoreResult::OpenFail;
  }
  const size_t size = file.size();
  if (size < UIPACK_HEADER_SIZE) {
    if (validateError) *validateError = UiPackResult::TooSmall;
    return StoreResult::ValidateFailed;
  }
  if (size > UIPACK_DEVICE_MAX_BYTES) {
    file.close();
    return StoreResult::TooBig;
  }
  // Boot and network activation serialize access; the source must not change
  // between the integrity pass and record parsing pass.
  const auto result = validateFile(file, size, info, overrides, cap, validateError);
  if (result != StoreResult::Ok) return result;
  if (info && (strcmp(info->name, name) != 0 || !PackState::validVersion(info->packVersion)))
    return StoreResult::ValidateFailed;
  if (info != nullptr && info->assetCount > 0) {
    // Font assets ship in the container format but applying them on device
    // waits for the host-renderer milestone; refuse rather than half-apply.
    return StoreResult::ValidateFailed;
  }
  return StoreResult::Ok;
}

bool listPacks(const std::function<void(const char* name, size_t size)>& fn) {
  HalFile root = Storage.open(UIPACK_DIR);
  if (!root) {
    LOG_DBG("WEB", "Failed to open directory: %s", UIPACK_DIR);
    return false;
  }
  if (!root.isDirectory()) {
    LOG_DBG("WEB", "Not a directory: %s", UIPACK_DIR);
    root.close();
    return false;
  }
  char name[500];
  HalFile file = root.openNextFile();
  while (file) {
    file.getName(name, sizeof(name));
    const bool isDirectory = file.isDirectory();
    const size_t size = isDirectory ? 0 : file.size();
    const bool hidden = name[0] == '.' && !SETTINGS.showHiddenFiles;
    file.close();
    if (!hidden && !isDirectory) {
      const size_t rawLen = strlen(name);
      // A file literally named ".uipack" stays hidden (dot rule) unless the
      // user opted into hidden files; then it lists as an empty pack name,
      // exactly as the previous host-scan path did.
      if (rawLen >= 7 && strcmp(name + rawLen - 7, ".uipack") == 0) {
        name[rawLen - 7] = '\0';  // strip the extension for the caller
        fn(name, size);
      }
    }
    yield();               // Yield to allow WiFi and other tasks to process during long scans
    esp_task_wdt_reset();  // Reset watchdog to prevent timeout on large directories
    file = root.openNextFile();
  }
  root.close();
  return true;
}

bool writeState(const char* name, const char* version) {
  if (!name || !version || strlen(name) > 24 || strlen(version) > 16) return false;
  PackState::Record records[2];
  const bool valid0 = readSlot(0, records[0]);
  const bool valid1 = readSlot(1, records[1]);
  const int latest = PackState::latest(valid0, records[0].generation, valid1, records[1].generation);
  const uint32_t generation = latest < 0 ? 0 : records[latest].generation;
  if (generation == UINT32_MAX) return false;  // No ambiguous wraparound ordering.
  int protectedSlot = latest;
  for (int slot = 0; slot < 2; ++slot) {
    if ((slot == 0 ? valid0 : valid1) && activeNameValue[0] && strcmp(records[slot].name, activeNameValue) == 0 &&
        strcmp(records[slot].version, activeVersionValue) == 0) {
      protectedSlot = slot;
    }
  }
  const int target = protectedSlot == 0 ? 1 : 0;
  auto& next = records[target];
  next = {};
  next.generation = generation + 1;
  strcpy(next.name, name);
  strcpy(next.version, version);
  uint8_t bytes[PackState::BYTES];
  if (!PackState::encode(next, bytes)) return false;
  HalFile file;
  if (!Storage.openFileForWrite("PCK", STATE_SLOTS[target], file) || !file) return false;
  const bool written = file.write(bytes, sizeof(bytes)) == sizeof(bytes);
  file.flush();
  file.close();
  if (!written || !readSlot(target, records[target])) return false;
  uint8_t verified[PackState::BYTES];
  if (!PackState::encode(records[target], verified) || memcmp(bytes, verified, sizeof(bytes))) return false;
  // Legacy state remains a migration fallback until the first verified slot exists.
  if (Storage.exists(UIPACK_STATE_PATH)) Storage.remove(UIPACK_STATE_PATH);
  return true;
}

bool readState(char* name, size_t nameCap, char* versionCap, size_t versionCap2) {
  if (!name || nameCap < 25 || !versionCap || versionCap2 < 17) return false;
  name[0] = versionCap[0] = '\0';
  PackState::Record records[2];
  const bool valid0 = readSlot(0, records[0]);
  const bool valid1 = readSlot(1, records[1]);
  const int latest = PackState::latest(valid0, records[0].generation, valid1, records[1].generation);
  if (latest >= 0) {
    strcpy(name, records[latest].name);
    strcpy(versionCap, records[latest].version);
    return name[0] != '\0';
  }
  HalFile file = Storage.open(UIPACK_STATE_PATH);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    return false;
  }
  char text[80]{};
  const size_t size = file.size();
  if (size >= sizeof(text) || file.read(reinterpret_cast<uint8_t*>(text), size) != size) return false;
  char* endName = strchr(text, '\n');
  if (strncmp(text, "name=", 5) || !endName) return false;
  *endName = 0;
  const char* versionLine = endName + 1;
  if (!PackState::validName(text + 5) || strncmp(versionLine, "version=", 8)) return false;
  char* endVersion = strchr(endName + 1, '\n');
  if (!endVersion) return false;
  *endVersion = 0;
  if (!PackState::validVersion(versionLine + 8)) return false;
  strcpy(name, text + 5);
  strcpy(versionCap, versionLine + 8);
  file.close();
  return name[0] != '\0';
}

bool clearState() { return writeState("", ""); }

const char* activeName() { return activeNameValue; }
const char* activeVersion() { return activeVersionValue; }
void noteActive(const char* name, const char* version) {
  snprintf(activeNameValue, sizeof(activeNameValue), "%s", name);
  snprintf(activeVersionValue, sizeof(activeVersionValue), "%s", version);
}

ThemeOverride* acquireOverrideBuffer() {
  return static_cast<ThemeOverride*>(malloc(sizeof(ThemeOverride) * UIPACK_MAX_THEME_OVERRIDES));
}

void releaseOverrideBuffer(ThemeOverride* buffer) { free(buffer); }

ThemeOverride* compactOverrideBuffer(ThemeOverride* buffer, size_t count) {
  if (count == 0) {
    free(buffer);
    return nullptr;
  }
  if (count >= UIPACK_MAX_THEME_OVERRIDES) return buffer;
  auto* compact = static_cast<ThemeOverride*>(realloc(buffer, sizeof(ThemeOverride) * count));
  if (compact) return compact;
  return buffer;
}

static bool tryStartupPack(const char* name, const char* version) {
  UiPackInfo info;
  ThemeOverride* overrides = acquireOverrideBuffer();
  if (!overrides) return false;
  UiPackResult validateError = UiPackResult::Ok;
  if (loadPackFromSd(name, &info, overrides, UIPACK_MAX_THEME_OVERRIDES, &validateError) != StoreResult::Ok) {
    LOG_ERR("PCK", "Startup pack '%s' failed to load; reverting to theme metrics", name);
    releaseOverrideBuffer(overrides);
    return false;
  }
  if (strcmp(version, info.packVersion)) {
    releaseOverrideBuffer(overrides);
    return false;
  }
  overrides = compactOverrideBuffer(overrides, info.themeOverrideCount);
  UITheme::getInstance().adoptPackMetrics(overrides, info.themeOverrideCount);
  noteActive(name, version);
  LOG_INF("PCK", "Applied UI pack '%s' v%s (%u overrides)", name, version,
          static_cast<unsigned>(info.themeOverrideCount));
  return true;
}

bool applyStartupPack() {
  PackState::Record records[2];
  const bool valid0 = readSlot(0, records[0]);
  const bool valid1 = readSlot(1, records[1]);
  const int latest = PackState::latest(valid0, records[0].generation, valid1, records[1].generation);
  noteActive("", "");
  if (latest >= 0) {
    if (!records[latest].name[0]) return false;  // Explicit revert must not revive an older pack.
    if (tryStartupPack(records[latest].name, records[latest].version)) return true;
    const int previous = 1 - latest;
    return (previous == 0 ? valid0 : valid1) && records[previous].name[0] &&
           tryStartupPack(records[previous].name, records[previous].version);
  }
  char name[25];
  char version[17];
  return readState(name, sizeof(name), version, sizeof(version)) && tryStartupPack(name, version);
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
