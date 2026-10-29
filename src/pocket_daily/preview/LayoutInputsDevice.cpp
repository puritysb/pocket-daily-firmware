#include "LayoutInputsDevice.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "SdCardFontSystem.h"
#include "components/themes/BaseTheme.h"
#include "pocket_daily/PocketProfileStore.h"
#include "pocket_daily/ScreenFrame.h"
#include "pocket_daily/home/HomeCover.h"

namespace PocketDaily::Preview {
bool captureLayoutInputs(const GfxRenderer& renderer, LayoutInputs& out) {
  std::unique_ptr<Screen::Frame> frame;
  if (Screen::loadPreviewFrame(frame) != Screen::LoadResult::Ready) return false;
  clearLayoutInputs(out);
  out.schema = 2;
  Home::coverMetadata(out.coverKey, out.coverSize, out.coverCrc);
  snprintf(out.deviceId, sizeof(out.deviceId), "%08lX", static_cast<unsigned long>(ESP.getEfuseMac() & 0xFFFFFFFFUL));
  snprintf(out.contentRevision, sizeof(out.contentRevision), "%s", frame->cards.revision());
  out.contentGeneration = frame->cards.generation();
  out.cardCount = frame->cards.cards() ? frame->cards.cards()->count : 0;
  out.profileGeneration = DailyProfile::generation();
  out.physicalWidth = renderer.getDisplayWidth();
  out.physicalHeight = renderer.getDisplayHeight();
  out.isX3 = gpio.deviceIsX3();
  out.sleepCover = SETTINGS.pocketDailySleepCover;
  out.metrics = Home::deviceMetrics();
  if (!BaseTheme::captureHeaderInputs(renderer, out.header)) return false;
  out.book = frame->book;
  out.glance = frame->glance;
  out.glanceStale = frame->glanceStale;
  out.word = frame->word;
  snprintf(out.fontFamily, sizeof(out.fontFamily), "PocketSansWorld");
  // Registry lookup only. Do not load a font, trigger a scan or change the
  // renderer's font state merely to inspect a proposed profile.
  const auto* family = sdFontSystem.registry().findFamily(out.fontFamily);
  const auto* font = family ? family->findNearestSize(12) : nullptr;
  out.fontPointSize = font ? font->pointSize : 0;
  if (!setLayoutStrings(out, Home::deviceStrings(), tr(STR_POCKET_SLEEP_PREVIEW)) || !validLayoutInputs(out)) {
    LOG_ERR("PREVIEW", "Layout inputs cannot be represented by schema 1");
    return false;
  }
  snprintf(out.labels[20], sizeof(out.labels[20]), "%s", tr(STR_POCKET_ASLEEP));
  snprintf(out.labels[21], sizeof(out.labels[21]), "%s", tr(STR_POCKET_WAKE_READ));
  return true;
}
}  // namespace PocketDaily::Preview
