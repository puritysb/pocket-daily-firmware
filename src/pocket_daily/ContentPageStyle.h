#pragma once

#include <cstdint>

namespace PocketDaily::Content {
// The page inputs BaseTheme::drawContentPage hands to renderContentPage besides
// the card, font and labels. The display endpoint reports these same values so
// a companion preview cannot drift from what the reader draws.
struct ContentPageStyle {
  int16_t sidePadding = 0;
  int16_t topPadding = 0;
  int16_t spacing = 0;
  const char* title = "";
  const char* empty = "";
};

// Device only: effective UITheme metrics (base theme plus any UI pack) and the
// current UI language. Strings are static translation table entries.
ContentPageStyle currentContentPageStyle();
}  // namespace PocketDaily::Content
