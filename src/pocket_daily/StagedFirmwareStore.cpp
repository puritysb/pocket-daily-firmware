#include "pocket_daily/StagedFirmwareStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <esp_attr.h>

#include "pocket_daily/staged_firmware.h"

namespace {
// RTC_NOINIT: survives ESP.restart() (every transfer session ends in one) but
// holds garbage after power-on, which take() rejects.
RTC_NOINIT_ATTR PocketDaily::StagedFirmware::Marker publishedMarker;
}  // namespace

namespace PocketDaily::StagedFirmware {

void notePublished(const uint32_t size, const uint32_t crc32) {
  arm(publishedMarker, size, crc32);
  LOG_INF("FWSTAGE", "Firmware published this session (%lu bytes)", static_cast<unsigned long>(size));
}

bool consumeOffer(const bool silentRestart, const bool landsOnShell) {
  OfferInput input;
  uint32_t crc32 = 0;
  input.markerValid = take(publishedMarker, input.markerSize, crc32);
  if (!input.markerValid) return false;
  input.silentRestart = silentRestart;
  input.landsOnShell = landsOnShell;
  HalFile file;
  if (Storage.exists(kPublishPath) && Storage.openFileForRead("FWSTAGE", kPublishPath, file) && file &&
      !file.isDirectory()) {
    input.fileExists = true;
    input.fileSize = file.fileSize64();
  }
  const bool offer = shouldOffer(input);
  LOG_INF("FWSTAGE", "Staged firmware marker consumed: offer=%d", offer ? 1 : 0);
  return offer;
}

}  // namespace PocketDaily::StagedFirmware
