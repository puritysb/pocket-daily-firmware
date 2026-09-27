#include "StatusBarTitle.h"

#include <GfxRenderer.h>

namespace StatusBarTitle {
namespace {
struct LineBox {
  int ascender = 0;
  int descender = 0;  // positive: pixels below the baseline
};

LineBox lineBox(const GfxRenderer& renderer, const int fontId) {
  const auto it = renderer.getFontMap().find(fontId);
  if (it == renderer.getFontMap().end()) return {};
  const EpdFontData* data = it->second.getData(EpdFontFamily::REGULAR);
  if (!data) return {};
  return {data->ascender, data->descender < 0 ? -data->descender : data->descender};
}
}  // namespace

Placement place(const GfxRenderer& renderer, const int titleFontId, const int smallFontId, const int textY) {
  Placement placement;
  placement.y = textY;
  if (titleFontId == smallFontId) return placement;
  const LineBox title = lineBox(renderer, titleFontId);
  const LineBox small = lineBox(renderer, smallFontId);
  if (title.ascender + title.descender <= small.ascender + small.descender) return placement;
  // drawText puts the baseline at y + the font's full ascender; half-scaled glyphs hang
  // from that baseline, so align it with the small text's baseline.
  placement.style = EpdFontFamily::SUP;
  placement.scale = 2;
  placement.y = textY + small.ascender - title.ascender;
  return placement;
}

int width(const GfxRenderer& renderer, const int titleFontId, const char* text, const Placement& placement) {
  const int full = renderer.getTextWidth(titleFontId, text);
  return placement.scale > 1 ? (full + 1) / 2 : full;
}

}  // namespace StatusBarTitle
