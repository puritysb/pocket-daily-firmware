#pragma once

#include <EpdFontFamily.h>

#include <cstdint>

class GfxRenderer;

namespace UiCjkFont {

// General library metadata can prefer a partial CJK match over dropping the
// whole string. Product-owned Pocket copy is different: its bundled font is
// coverage-tested at build time, so accepting a partial family would hide a
// packaging regression and render tofu on the retained E-ink frame.
enum class CoveragePolicy : uint8_t { AllowPartial, RequireFull };

int fontForText(const GfxRenderer& renderer, const char* text, int fallbackFontId,
                EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                CoveragePolicy policy = CoveragePolicy::AllowPartial);

}  // namespace UiCjkFont
