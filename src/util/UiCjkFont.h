#pragma once

#include <EpdFontFamily.h>

class GfxRenderer;

namespace UiCjkFont {

int fontForText(const GfxRenderer& renderer, const char* text, int fallbackFontId,
                EpdFontFamily::Style style = EpdFontFamily::REGULAR);

}  // namespace UiCjkFont
