#pragma once

#include "components/themes/BaseTheme.h"

class GfxRenderer;

// The centered message popup ("Indexing", "Failed to index ...", bookmark and
// mode toasts) without the display refresh, so the host suite rasterizes the
// exact device drawing.
namespace PopupFrame {

// Colour of each popup layer; true is black.
struct Ink {
  bool frame;
  bool box;
  bool text;
};

// `popupTextInverted` picks the text colour (true = black text, as Classic
// draws it) and the box always takes the opposite colour, with a frame in the
// text colour. The corner radius only shapes the box. It used to pick the fill
// too (rounded = black box) while the text colour stayed separate, so a UI
// pack that only rounded Classic's corners drew black text on a black box.
constexpr Ink inkFor(const ThemeMetrics& metrics) {
  const bool text = metrics.popupTextInverted;
  return Ink{text, !text, text};
}

// Draws the frame, box and message; the caller refreshes the display.
Rect draw(const GfxRenderer& renderer, const ThemeMetrics& metrics, int fontId, const char* message);

}  // namespace PopupFrame
