#pragma once

#include <EpdFontFamily.h>
#include <GfxRenderer.h>

#include <algorithm>

#include "fontIds.h"

namespace PowerWakeCue {

// Pocket Daily draws its sleep sections in portrait. Keep content below the
// tab so cover-free reading progress and custom section orders stay visible.
inline int portraitContentTop(const bool isX3) { return isX3 ? 58 : 124; }

// A retained e-ink frame has no animation to suggest that the device is still
// responsive. Anchor one compact, high-contrast tab to the real power switch
// instead. X3's switch is on the top edge; X4's is on the upper-right edge.
inline void draw(GfxRenderer& renderer, const bool isX3, const char* label, const bool opening = false) {
  const auto original = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  const int w = renderer.getScreenWidth();

  int x;
  int y;
  int tabW;
  int tabH;
  int iconX;
  int iconY;
  int labelX;
  int labelY;

  if (isX3) {
    constexpr int powerX = 473;
    tabW = 104;
    tabH = 52;
    x = std::max(0, std::min(w - tabW, powerX - tabW / 2));
    y = 0;
    iconX = x + 25;
    iconY = 27;
    labelX = x + 46;
    labelY = 17;
  } else {
    constexpr int powerY = 74;
    tabW = 64;
    tabH = 88;
    x = w - tabW;
    y = std::max(0, powerY - tabH / 2);
    iconX = x + tabW / 2;
    iconY = y + 25;
    labelX = x + (tabW - renderer.getTextWidth(SMALL_FONT_ID, label, EpdFontFamily::BOLD)) / 2;
    labelY = y + 50;
  }

  // A white keyline keeps the tab visible even over a dark full-screen cover.
  renderer.fillRect(x, y, tabW, tabH, false);
  renderer.fillRect(x + 2, y + (isX3 ? 0 : 2), tabW - 2, tabH - 4, true);

  if (isX3) {
    // The retained closed book is the first keyframe. Wake replaces it with
    // the open book, then the actual page. No bitmap, timer or extra buffer.
    if (opening) {
      renderer.drawLine(iconX - 11, iconY - 8, iconX, iconY - 4, 2, false);
      renderer.drawLine(iconX, iconY - 4, iconX + 11, iconY - 8, 2, false);
      renderer.drawLine(iconX - 11, iconY - 8, iconX - 11, iconY + 7, 2, false);
      renderer.drawLine(iconX + 11, iconY - 8, iconX + 11, iconY + 7, 2, false);
      renderer.drawLine(iconX - 11, iconY + 7, iconX, iconY + 11, 2, false);
      renderer.drawLine(iconX, iconY + 11, iconX + 11, iconY + 7, 2, false);
      renderer.drawLine(iconX, iconY - 4, iconX, iconY + 11, 2, false);
    } else {
      renderer.drawRect(iconX - 8, iconY - 10, 16, 22, false);
      renderer.drawLine(iconX - 4, iconY - 10, iconX - 4, iconY + 11, 2, false);
      renderer.drawLine(iconX - 7, iconY + 7, iconX + 7, iconY + 7, 1, false);
    }
  } else {
    // Power glyph in knockout white. Its stem points directly into the chassis
    // edge, so the cue still works without a sentence or an imprecise arrow.
    renderer.drawLine(iconX, iconY - 9, iconX, iconY + 1, 2, false);
    renderer.drawLine(iconX - 5, iconY - 6, iconX - 9, iconY - 2, 2, false);
    renderer.drawLine(iconX - 9, iconY - 2, iconX - 9, iconY + 4, 2, false);
    renderer.drawLine(iconX - 9, iconY + 4, iconX - 5, iconY + 8, 2, false);
    renderer.drawLine(iconX - 5, iconY + 8, iconX + 5, iconY + 8, 2, false);
    renderer.drawLine(iconX + 5, iconY + 8, iconX + 9, iconY + 4, 2, false);
    renderer.drawLine(iconX + 9, iconY + 4, iconX + 9, iconY - 2, 2, false);
    renderer.drawLine(iconX + 9, iconY - 2, iconX + 5, iconY - 6, 2, false);
  }
  renderer.drawText(SMALL_FONT_ID, labelX, labelY, label, false, EpdFontFamily::BOLD);

  renderer.setOrientation(original);
}

}  // namespace PowerWakeCue
