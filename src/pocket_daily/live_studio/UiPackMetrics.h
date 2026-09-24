#pragma once

#include "ThemeFieldIds.h"
#include "UiPack.h"

namespace PocketDaily::LiveStudio {
// Always rebuild from the native theme, never from a previous pack's output.
inline void composePackMetrics(const ThemeMetrics& base, const ThemeOverride* overrides, size_t count,
                               ThemeMetrics& output) {
  output = base;
  for (size_t i = 0; i < count; ++i) {
    ThemeField::applyOverride(output, overrides[i].fieldId, overrides[i].type, overrides[i].value);
  }
}
}  // namespace PocketDaily::LiveStudio
