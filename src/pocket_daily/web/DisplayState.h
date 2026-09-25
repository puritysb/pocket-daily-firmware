#pragma once

#include <cstddef>
#include <cstdint>

namespace PocketDaily::Web {
// Resolved render inputs for the companion preview: exactly what the reader
// passes to the content-page renderer in this session (theme metrics after any
// UI pack, UI language, button remapping, orientation, installed font size).
struct DisplayInputs {
  const char* deviceId = "";
  const char* theme = "";
  uint8_t orientation = 0;  // GfxRenderer::Orientation, same order as the app
  const char* fontFamily = "";
  uint8_t fontPointSize = 0;  // 0 when the family is not installed
  int16_t sidePadding = 0;
  int16_t topPadding = 0;
  int16_t spacing = 0;
  const char* title = "";
  const char* empty = "";
  const char* labels[4] = {"", "", "", ""};
};

// CrossPointSettings::UI_THEME value to its stable wire name ("unknown" if new).
const char* themeName(uint8_t uiTheme);

// Pure JSON writer (schema 1). Returns bytes written excluding the terminator,
// or 0 when the document does not fit or an input is out of range.
size_t writeDisplayJson(const DisplayInputs& in, char* out, size_t cap);
}  // namespace PocketDaily::Web
