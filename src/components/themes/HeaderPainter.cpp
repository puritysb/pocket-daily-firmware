#include "HeaderPainter.h"

#include <FreeInkUIGfxRenderer.h>
#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>

#include "components/icons/headerIcons.h"
#include "fontIds.h"

namespace HeaderPainter {
namespace {
constexpr int BRAND_LOGO_WIDTH = 21;

// Monochrome Pocket Daily reader mark.
void drawBrandLogo(const GfxRenderer& renderer, const int x, const int y) {
  renderer.fillRoundedRect(x, y, BRAND_LOGO_WIDTH, BRAND_LOGO_HEIGHT, 5, Color::Black);
  renderer.fillRoundedRect(x + 2, y + 3, 17, 19, 2, Color::White);
  renderer.fillRect(x + 5, y + 7, 11, 1, true);
  renderer.fillRect(x + 5, y + 10, 8, 1, true);
  renderer.fillRect(x + 5, y + 13, 10, 1, true);
  renderer.fillRect(x + 5, y + 18, 6, 1, true);
  renderer.fillRect(x + 7, y + 24, 7, 1, false);
}
}  // namespace

void paintHeader(const GfxRenderer& renderer, const Inputs& in, const int x, const int y, const int width,
                 const int height, const char* title, const char* subtitle, const bool branded) {
  namespace fui = freeink::ui;
  // Existing BaseTheme passive frame/props stack, now shared with the host.
  // No new resident theme/framebuffer allocation or settings access.
  fui::GfxRendererFrame<1> ui(renderer, in.fonts[0], in.fonts[1], in.fonts[2]);
  ui.target.setFont(fui::GfxRendererTarget::FONT_LABEL, in.fonts[3]);
  fui::HeaderProps props;
  props.title = title;
  props.rightLabel = subtitle;
  props.titleText = in.titleText;
  props.subtitleText = in.subtitleText;
  props.styles.explicitlySet = true;
  props.styles.normal = in.normalStyle;
  props.sidePadding = in.sidePadding;
  props.borderEdges = fui::EdgeBottom;
  if (title && props.styles.normal.border.kind == fui::PaintKind::None && in.underline > 0) {
    props.styles.normal.border = fui::Paint::solid(fui::Color::Black);
    props.styles.normal.borderWidth = static_cast<uint8_t>(in.underline);
  }
  props.leadingSize = 48;
  props.trailingSize = 48;
  props.actionOffsetY = in.actionOffsetY;
  props.titleOffsetY = height < in.height ? 0 : in.stripHeight / 2;
  auto& status = props.status;
  status.showBattery = true;
  status.battery.text.font = fui::GfxRendererTarget::FONT_LABEL;
  status.battery.percent = in.batteryPercent;
  status.battery.charging = in.charging;
  status.battery.label = in.batteryLabel[0] ? in.batteryLabel : nullptr;
  status.battery.glyphWidth = in.batteryWidth;
  status.battery.glyphHeight = in.batteryHeight;
  status.battery.gap = 4;
  status.batteryLeft = in.batteryLeft;
  status.edgeInset = in.edgeInset;
  status.stripHeight = in.stripHeight;
  status.clockCentered = in.clockCentered;
  status.clockText = in.clock[0] ? in.clock : nullptr;
  if (branded) {
    props.titleText.align = fui::TextAlign::Left;
    props.titleOffsetY = 0;
    props.leftReserve = BRAND_LOGO_WIDTH + 8;
    status.batteryLeft = false;
    status.stripHeight = static_cast<int16_t>(height);
    // The dashboard already has a large clock immediately below this row.
    status.clockText = nullptr;
  } else if (in.showBack && title) {
    props.leadingIcon = fui::bitmapFromIcon(icon_header_back_32);
    props.leadingAction = 1;
  }
  fui::header(
      ui.frame,
      {static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(width), static_cast<int16_t>(height)},
      props);
  if (branded) drawBrandLogo(renderer, x + in.sidePadding, y + (height - BRAND_LOGO_HEIGHT) / 2);
}
}  // namespace HeaderPainter
