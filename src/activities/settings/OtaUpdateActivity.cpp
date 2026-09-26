#include "OtaUpdateActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>
#include <esp_err.h>

#include <cstdlib>
#include <cstring>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "network/OtaFailure.h"
#include "network/OtaUpdater.h"

void OtaUpdateActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_ERR("OTA", "WiFi connection failed, exiting");
    leave();
    return;
  }

  LOG_DBG("OTA", "WiFi connected, checking for update");

  {
    RenderLock lock(*this);
    state = CHECKING_FOR_UPDATE;
  }
  requestUpdateAndWait();

  startFreeHeap = ESP.getFreeHeap();
  startLargestBlock = ESP.getMaxAllocHeap();
  const auto res = updater.checkForUpdate();
  if (res != OtaUpdater::OK) {
    LOG_DBG("OTA", "Update check failed: %d", res);
    {
      RenderLock lock(*this);
      noteFailure(res, false);
      state = FAILED;
    }
    return;
  }

  if (!updater.isUpdateNewer()) {
    LOG_DBG("OTA", "No new update available");
    {
      RenderLock lock(*this);
      state = NO_UPDATE;
    }
    return;
  }

  {
    RenderLock lock(*this);
    state = WAITING_CONFIRMATION;
  }
}

static_assert(OtaFailure::kUpdaterHttpError == OtaUpdater::HTTP_ERROR, "OtaFailure mirrors OtaUpdater");
static_assert(OtaFailure::kUpdaterJsonParseError == OtaUpdater::JSON_PARSE_ERROR, "OtaFailure mirrors OtaUpdater");
static_assert(OtaFailure::kUpdaterOomError == OtaUpdater::OOM_ERROR, "OtaFailure mirrors OtaUpdater");
static_assert(OtaFailure::kEspErrNoMem == ESP_ERR_NO_MEM, "OtaFailure mirrors ESP-IDF");

void OtaUpdateActivity::noteFailure(const int updaterError, const bool installing) {
  const auto kb = [](uint32_t bytes) { return static_cast<unsigned>(bytes / 1024); };
  const HttpDownloader::Failure http = HttpDownloader::lastFailure();
  failureKind =
      static_cast<uint8_t>(OtaFailure::classify(updaterError, http.stage, http.code, http.tlsCode, installing));
  if (updaterError == OtaUpdater::HTTP_ERROR && http.stage) {
    int used = 0;
    if (http.tlsCode != 0)
      used = snprintf(failureDetail, sizeof(failureDetail), "%s %s tls -0x%X", http.stage, esp_err_to_name(http.code),
                      static_cast<unsigned>(std::abs(http.tlsCode)));
    else if (strcmp(http.stage, "open") == 0)
      used = snprintf(failureDetail, sizeof(failureDetail), "%s %s", http.stage, esp_err_to_name(http.code));
    else
      used = snprintf(failureDetail, sizeof(failureDetail), "%s %d", http.stage, http.code);
    if (used > 0 && static_cast<size_t>(used) < sizeof(failureDetail))
      snprintf(failureDetail + used, sizeof(failureDetail) - used, " | %u/%uK of %u/%uK", kb(http.freeHeap),
               kb(http.largestBlock), kb(startFreeHeap), kb(startLargestBlock));
    return;
  }
  snprintf(failureDetail, sizeof(failureDetail), "error %d | heap %u/%uK", updaterError, kb(ESP.getFreeHeap()),
           kb(ESP.getMaxAllocHeap()));
}

void OtaUpdateActivity::leave() {
  if (origin == Origin::PocketDaily) {
    // Mirror the Pocket Sync exit: never construct Pocket Daily while the STA
    // stack still fragments the heap; restart straight into it instead.
    if (WiFi.getMode() != WIFI_MODE_NULL) {
      WiFi.disconnect(false);
      delay(30);
    }
    silentRestartToPocketDaily();
    return;  // Returns only when a committed deep sleep supersedes the restart.
  }
  finish();
}

void OtaUpdateActivity::onEnter() {
  Activity::onEnter();

  // Turn on WiFi immediately
  LOG_DBG("OTA", "Turning on WiFi...");
  WiFi.mode(WIFI_STA);

  // Launch WiFi selection subactivity
  LOG_DBG("OTA", "Launching WifiSelectionActivity...");
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void OtaUpdateActivity::onExit() {
  Activity::onExit();

  // Success path reboots via the SHUTTING_DOWN state's plain ESP.restart()
  // (loop() above) so the new firmware boots normally. Back-out paths land
  // here with wifi still active; silent-restart to free the LWIP/mbedTLS
  // fragmentation, same as the other wifi activities.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void OtaUpdateActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_UPDATE));
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;

  float updaterProgress = 0;
  if (state == UPDATE_IN_PROGRESS) {
    LOG_DBG("OTA", "Update progress: %d / %d", updater.getProcessedSize(), updater.getTotalSize());
    updaterProgress = static_cast<float>(updater.getProcessedSize()) / static_cast<float>(updater.getTotalSize());
    // Only update every 2% at the most
    if (static_cast<int>(updaterProgress * 50) == lastUpdaterPercentage / 2) {
      return;
    }
    lastUpdaterPercentage = static_cast<int>(updaterProgress * 100);
  }

  if (state == CHECKING_FOR_UPDATE) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_CHECKING_UPDATE));
  } else if (state == WAITING_CONFIRMATION) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_NEW_UPDATE), true, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, top + height + metrics.verticalSpacing,
                      (std::string(tr(STR_CURRENT_VERSION)) + CROSSPOINT_VERSION).c_str());
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, top + height * 2 + metrics.verticalSpacing * 2,
                      (std::string(tr(STR_NEW_VERSION)) + updater.getLatestVersion()).c_str());

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_UPDATE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == UPDATE_IN_PROGRESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATING));

    int y = top + height + metrics.verticalSpacing;
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, y, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        static_cast<int>(updaterProgress * 100), 100);

    y += metrics.progressBarHeight + metrics.verticalSpacing;
    // Percent label is drawn by BaseTheme::drawProgressBar; this slot is left intentionally empty
    // so the bytes line below stays at the same Y it was at when the activity drew its own percent.
    y += height + metrics.verticalSpacing;
    renderer.drawCenteredText(
        UI_10_FONT_ID, y,
        (std::to_string(updater.getProcessedSize()) + " / " + std::to_string(updater.getTotalSize())).c_str());
  } else if (state == NO_UPDATE) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_NO_UPDATE), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == FAILED) {
    // Plain reason first, then what to do; the technical line stays last and
    // small for support. A reader that cannot hold a TLS session is pointed
    // to the companion app, which downloads the release and sends it locally.
    const auto kind = static_cast<OtaFailure::Kind>(failureKind);
    StrId reason = StrId::STR_UPDATE_FAIL_NETWORK;
    switch (kind) {
      case OtaFailure::Kind::Memory:
        reason = StrId::STR_UPDATE_FAIL_MEMORY;
        break;
      case OtaFailure::Kind::Server:
        reason = StrId::STR_UPDATE_FAIL_SERVER;
        break;
      case OtaFailure::Kind::Release:
        reason = StrId::STR_UPDATE_FAIL_RELEASE;
        break;
      case OtaFailure::Kind::Install:
        reason = StrId::STR_UPDATE_FAIL_INSTALL;
        break;
      case OtaFailure::Kind::Network:
        break;
    }
    const int textW = pageWidth - metrics.contentSidePadding * 2;
    int y = top - height * 2;
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_UPDATE_FAILED), true, EpdFontFamily::BOLD);
    y += height + metrics.verticalSpacing;
    renderer.drawCenteredText(UI_10_FONT_ID, y, renderer.truncatedText(UI_10_FONT_ID, I18N.get(reason), textW).c_str());
    y += height + metrics.verticalSpacing * 2;
    if (kind == OtaFailure::Kind::Memory) {
      renderer.drawCenteredText(UI_10_FONT_ID, y,
                                renderer.truncatedText(UI_10_FONT_ID, tr(STR_UPDATE_USE_APP), textW).c_str());
      y += height + 2;
      renderer.drawCenteredText(UI_10_FONT_ID, y,
                                renderer.truncatedText(UI_10_FONT_ID, tr(STR_UPDATE_USE_APP_STEPS), textW).c_str(),
                                true, EpdFontFamily::BOLD);
      y += height + metrics.verticalSpacing * 2;
    } else if (kind != OtaFailure::Kind::Install) {
      renderer.drawCenteredText(UI_10_FONT_ID, y,
                                renderer.truncatedText(UI_10_FONT_ID, tr(STR_UPDATE_TRY_AGAIN), textW).c_str());
      y += height + metrics.verticalSpacing * 2;
    }
    if (failureDetail[0])
      renderer.drawCenteredText(SMALL_FONT_ID, y, renderer.truncatedText(SMALL_FONT_ID, failureDetail, textW).c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == FINISHED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_COMPLETE), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height + metrics.verticalSpacing, tr(STR_POWER_ON_HINT));
  }

  renderer.displayBuffer();
}

void OtaUpdateActivity::loop() {
  if (state == WAITING_CONFIRMATION) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      LOG_DBG("OTA", "New update available, starting download...");
      {
        RenderLock lock(*this);
        state = UPDATE_IN_PROGRESS;
      }
      requestUpdateAndWait();
      const auto res = updater.installUpdate(
          [](void* ctx) {
            // immediate=true notifies the render task directly. The default deferred path only
            // sets a flag consumed at the end of ActivityManager::loop(), which never runs while
            // installUpdate() blocks this task.
            static_cast<OtaUpdateActivity*>(ctx)->requestUpdate(true);
          },
          this);

      if (res != OtaUpdater::OK) {
        LOG_DBG("OTA", "Update failed: %d", res);
        {
          RenderLock lock(*this);
          noteFailure(res, true);
          state = FAILED;
        }
        requestUpdate();
        return;
      }

      {
        RenderLock lock(*this);
        state = FINISHED;
      }
      requestUpdateAndWait();
      // Hold the completion screen briefly so the user sees it, then restart.
      delay(3000);
      {
        RenderLock lock(*this);
        state = SHUTTING_DOWN;
      }
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      leave();
    }

    return;
  }

  if (state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      leave();
    }
    return;
  }

  if (state == NO_UPDATE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      leave();
    }
    return;
  }

  if (state == SHUTTING_DOWN) {
    ESP.restart();
  }
}
