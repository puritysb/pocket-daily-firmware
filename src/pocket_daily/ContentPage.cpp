#include <GfxRenderer.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <I18n.h>

#include <cstdio>

#include "SdCardFontSystem.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "pocket_daily/ContentPageRenderer.h"
#include "pocket_daily/ContentPageStyle.h"
#include "pocket_daily/ContentRevisionStore.h"

namespace {
bool image(const GfxRenderer& renderer, const char* revision, const char* name, int x, int y, int w, int h) {
  if (!name[0] || w <= 0 || h <= 0) return true;
  char path[160];
  snprintf(path, sizeof(path), "%s/%s/%s", PocketDaily::Content::CONTENT_ROOT, revision, name);
  HalFile file = Storage.open(path, O_RDONLY);
  if (!file) return false;
  const PocketDaily::Content::ManifestSource source{
      &file, file.size(), [](void* context, size_t offset, uint8_t* bytes, size_t count) {
        HalSystem::feedWatchdogIfRegistered();
        auto& file = *static_cast<HalFile*>(context);
        return file.seek(offset) && file.read(bytes, count) == static_cast<int>(count);
      }};
  return GUI.drawContentImage(renderer, source, x, y, w, h);
}
}  // namespace

namespace PocketDaily::Content {
ContentPageStyle currentContentPageStyle() {
  const auto& m = UITheme::getInstance().getMetrics();
  return {static_cast<int16_t>(m.contentSidePadding), static_cast<int16_t>(m.topPadding),
          static_cast<int16_t>(m.verticalSpacing), tr(STR_POCKET_TITLE), tr(STR_POCKET_EMPTY)};
}
}  // namespace PocketDaily::Content

bool BaseTheme::drawContentPage(GfxRenderer& renderer, const PocketDaily::Content::ContentCard* card,
                                const char* revision, int fontId, const char* const labels[4]) const {
  if (!sdFontSystem.boundedUiFontReady(fontId)) return false;
  // Same values the display endpoint reports (ContentPageStyle.h).
  const auto style = PocketDaily::Content::currentContentPageStyle();
  const PocketDaily::Content::ContentPageOptions options{fontId,
                                                         style.sidePadding,
                                                         style.topPadding,
                                                         style.spacing,
                                                         style.title,
                                                         style.empty,
                                                         {labels[0], labels[1], labels[2], labels[3]}};
  struct ImageContext {
    const GfxRenderer& renderer;
    const char* revision;
  } context{renderer, revision};
  const PocketDaily::Content::ContentPageImagePainter images{
      &context, [](void* value, const char* name, int x, int y, int width, int height) {
        const auto& c = *static_cast<ImageContext*>(value);
        return image(c.renderer, c.revision, name, x, y, width, height);
      }};
  return PocketDaily::Content::renderContentPage(renderer, card, options, images) &&
         sdFontSystem.boundedUiFontReady(fontId);
}
