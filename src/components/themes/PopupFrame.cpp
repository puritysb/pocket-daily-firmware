#include "PopupFrame.h"

#include <GfxRenderer.h>

namespace PopupFrame {

Rect draw(const GfxRenderer& renderer, const ThemeMetrics& metrics, const int fontId, const char* message) {
  const int marginX = metrics.popupMarginX;
  const int marginY = metrics.popupMarginY;
  const int frameThickness = metrics.popupFrameThickness;
  const EpdFontFamily::Style style = metrics.popupTextBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  // Scale y position proportionally to screen height
  const int y = static_cast<int>(renderer.getScreenHeight() * metrics.popupTopOffsetRatio);
  const int textWidth = renderer.getTextWidth(fontId, message, style);
  const int textHeight = renderer.getLineHeight(fontId);
  const int w = textWidth + marginX * 2;
  const int h = textHeight + marginY * 2;
  const int x = (renderer.getScreenWidth() - w) / 2;
  const Ink ink = inkFor(metrics);

  if (metrics.popupCornerRadius > 0) {
    renderer.fillRoundedRect(x - frameThickness, y - frameThickness, w + frameThickness * 2, h + frameThickness * 2,
                             metrics.popupCornerRadius + frameThickness, ink.frame ? Color::Black : Color::White);
    renderer.fillRoundedRect(x, y, w, h, metrics.popupCornerRadius, ink.box ? Color::Black : Color::White);
  } else {
    renderer.fillRect(x - frameThickness, y - frameThickness, w + frameThickness * 2, h + frameThickness * 2,
                      ink.frame);
    renderer.fillRect(x, y, w, h, ink.box);
  }

  const int textX = x + (w - textWidth) / 2;
  const int textY = y + marginY + metrics.popupTextBaselineOffsetY;
  renderer.drawText(fontId, textX, textY, message, ink.text, style);
  return Rect{x, y, w, h};
}

}  // namespace PopupFrame
