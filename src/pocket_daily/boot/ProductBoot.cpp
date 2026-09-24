#include "pocket_daily/boot/ProductBoot.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <cstring>

#include "SilentRestart.h"
#include "components/UITheme.h"
#include "pocket_daily/PocketScreenPreview.h"
#include "pocket_daily/live_studio/NetHealth.h"
#include "pocket_daily/live_studio/UiPackStore.h"
#include "util/ScreenshotUtil.h"
#ifdef ENABLE_DEV_REMOTE_FLASH
#include "pocket_daily/product_identity.h"
#endif

// PlatformIO turns the compact Pocket Daily Japanese font into linker data.
// Keep this subset in firmware so a clean SD card can render the offline deck
// before the device has ever reached AgentDeck or File Transfer.
extern const uint8_t pocketJpFontStart[] asm("_binary_assets_fonts_PocketJP_PocketSansJP_12_cpfont_start");
extern const uint8_t pocketJpFontEnd[] asm("_binary_assets_fonts_PocketJP_PocketSansJP_12_cpfont_end");

// RTC_NOINIT words defined in main.cpp next to upstream's silent-reboot state.
extern uint32_t silentRebootMagic;
extern uint32_t silentRebootTarget;
// Mirror of main.cpp's SILENT_REBOOT_MAGIC (SEAM.md base-field obligation).
constexpr uint32_t kSilentRebootMagic = 0xC1EAB007;

namespace {
GfxRenderer* bootRenderer = nullptr;
const bool* deepSleepLatch = nullptr;
}  // namespace

namespace PocketDaily::Boot {

void begin(GfxRenderer& renderer, const bool& deepSleepInProgress) {
  bootRenderer = &renderer;
  deepSleepLatch = &deepSleepInProgress;
}

void beginNetHealth() { PocketDaily::NetHealth::begin(); }

void applyStartupUiPack() {
  // Live Studio LS-3: layer the persisted UI pack over the selected theme
  // before any surface renders.
  PocketDaily::LiveStudio::applyStartupPack();
}

DevBootReturn consumeDevBootReturn() {
#ifdef ENABLE_DEV_REMOTE_FLASH
  if (!Storage.exists(PocketDaily::DEV_BOOT_FILE_TRANSFER_MARKER)) return DevBootReturn::None;
  DevBootReturn target = DevBootReturn::None;
  {
    HalFile marker = Storage.open(PocketDaily::DEV_BOOT_FILE_TRANSFER_MARKER);
    char value = 0;
    if (marker && marker.size() == 1 && marker.read(&value, 1) == 1) target = decodeDevBootMarker(&value, 1);
  }
  if (!Storage.remove(PocketDaily::DEV_BOOT_FILE_TRANSFER_MARKER)) {
    LOG_ERR("MAIN", "Could not consume dev boot marker; skipping automatic return");
    return DevBootReturn::None;
  }
  return target;
#else
  return DevBootReturn::None;
#endif
}

void installJapaneseFont() {
  static constexpr char kHiddenRoot[] = "/.fonts";
  static constexpr char kVisibleRoot[] = "/fonts";
  static constexpr char kFamily[] = "PocketSansJP";
  static constexpr char kFilename[] = "PocketSansJP_12.cpfont";
  static constexpr char kRevision[] = "3\n";

  const char* root = Storage.exists("/fonts/PocketSansJP") ? kVisibleRoot : kHiddenRoot;
  char familyDir[64];
  char finalPath[96];
  char tempPath[96];
  char revisionPath[96];
  snprintf(familyDir, sizeof(familyDir), "%s/%s", root, kFamily);
  snprintf(finalPath, sizeof(finalPath), "%s/%s", familyDir, kFilename);
  snprintf(tempPath, sizeof(tempPath), "%s/%s.tmp", familyDir, kFilename);
  snprintf(revisionPath, sizeof(revisionPath), "%s/.pocket-revision", familyDir);

  // OTA replaces this embedded, content-derived subset whenever its revision
  // changes. Merely checking for file existence left the original 16-word font
  // installed forever even after the learning deck grew to 612 records.
  if (Storage.exists(finalPath)) {
    char installedRevision[sizeof(kRevision)] = {};
    HalFile revisionFile;
    if (Storage.openFileForRead("FONT", revisionPath, revisionFile) &&
        revisionFile.read(installedRevision, sizeof(kRevision) - 1) == sizeof(kRevision) - 1 &&
        memcmp(installedRevision, kRevision, sizeof(kRevision) - 1) == 0) {
      return;
    }
  }

  if (!Storage.exists(root) && !Storage.mkdir(root)) return;
  if (!Storage.exists(familyDir) && !Storage.mkdir(familyDir)) return;

  HalFile file;
  if (!Storage.openFileForWrite("FONT", tempPath, file)) return;
  // Linker-provided symbols are separate declarations even though they bound
  // one embedded blob. Integer addresses express that contract without the
  // undefined cross-object pointer subtraction cppcheck correctly rejects.
  const size_t total = reinterpret_cast<uintptr_t>(pocketJpFontEnd) - reinterpret_cast<uintptr_t>(pocketJpFontStart);
  size_t written = 0;
  while (written < total) {
    const size_t chunk = (total - written) > 4096 ? 4096 : (total - written);
    const size_t n = file.write(pocketJpFontStart + written, chunk);
    if (n != chunk) break;
    written += n;
  }
  file.close();
  if (written != total) {
    Storage.remove(tempPath);
    LOG_ERR("FONT", "Pocket JP install incomplete: %u/%u", (unsigned)written, (unsigned)total);
    return;
  }
  Storage.remove(finalPath);
  if (!Storage.rename(tempPath, finalPath)) {
    Storage.remove(tempPath);
    return;
  }

  HalFile revisionFile;
  if (Storage.openFileForWrite("FONT", revisionPath, revisionFile)) {
    revisionFile.write(kRevision, sizeof(kRevision) - 1);
  }
  LOG_INF("FONT", "Installed Pocket Japanese subset (%u bytes)", (unsigned)total);
}

}  // namespace PocketDaily::Boot

// Definitions for upstream SilentRestart.h: identical call sites, bodies moved
// verbatim from main.cpp (deepSleepInProgress read through the wired latch).
void silentRestartToPocketDaily() {
  if (deepSleepLatch && *deepSleepLatch) return;  // sleeping supersedes the heap-defrag reboot
  if (Storage.exists(PocketDaily::SCREEN_PREVIEW_PATH)) Storage.remove(PocketDaily::SCREEN_PREVIEW_PATH);
  silentRebootTarget = PocketDaily::Boot::kRebootTargetPocketDaily;
  silentRebootMagic = kSilentRebootMagic;
  LOG_DBG("MAIN", "Silent restart (target=Pocket Daily)");
  GUI.drawPopup(*bootRenderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void silentRestartToPocketNearbySync() {
  if (deepSleepLatch && *deepSleepLatch) return;  // sleeping supersedes the heap-defrag reboot
  // Preserve the exact Pocket Daily surface before the loading popup replaces
  // it. Writing the BMP row-by-row costs no framebuffer-sized allocation and
  // gives the Apple companion a pixel-identical preview after the clean reboot.
  [[maybe_unused]] const bool previewSaved =
      ScreenshotUtil::saveFramebufferAsBmp(PocketDaily::SCREEN_PREVIEW_PATH, bootRenderer->getFrameBuffer(),
                                           bootRenderer->getDisplayWidth(), bootRenderer->getDisplayHeight());
  LOG_DBG("MAIN", "Pocket screen preview %s", previewSaved ? "saved" : "unavailable");
  silentRebootTarget = PocketDaily::Boot::kRebootTargetPocketNearbySync;
  silentRebootMagic = kSilentRebootMagic;
  LOG_DBG("MAIN", "Silent restart (target=Pocket Nearby Sync)");
  GUI.drawPopup(*bootRenderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}
