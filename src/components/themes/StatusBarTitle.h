#pragma once

#include <EpdFontFamily.h>

class GfxRenderer;

// Vertical fit of the status bar's centered title (BaseTheme::drawStatusBar).
//
// The status bar is laid out for the small UI font, but a CJK title is drawn
// with the reader's SD font (UiCjkFont reuses it: the SD font manager holds a
// single family at the reader's size). At reader sizes that font's line box is
// taller than the status lane, so the title's lower part ran off the bottom of
// the panel. A title font whose line box exceeds the small font's is drawn at
// half scale (the superscript glyph path) on the small text's baseline instead;
// a font that fits is drawn exactly as before.
namespace StatusBarTitle {

struct Placement {
  int y = 0;                                            // drawText y
  EpdFontFamily::Style style = EpdFontFamily::REGULAR;  // SUP = half-scale glyphs
  int scale = 1;                                        // 2 when half-scaled
};

// `textY` is where the status bar draws its small-font text (drawText y).
Placement place(const GfxRenderer& renderer, int titleFontId, int smallFontId, int textY);

// Drawn width of `text` for a placement (half-scaled advances halve the width).
int width(const GfxRenderer& renderer, int titleFontId, const char* text, const Placement& placement);

}  // namespace StatusBarTitle
