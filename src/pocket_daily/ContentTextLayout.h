#pragma once
#include <cstdint>

namespace PocketDaily::Content {
struct TextPainter {
  void* context = nullptr;
  int (*measure)(void*, const char*) = nullptr;
  void (*draw)(void*, const char*, unsigned line) = nullptr;
};
struct TextLayoutResult {
  unsigned lines = 0;
  bool truncated = false;
};
// Allocation-free UTF-8 word wrapping for the common device/host drawing path.
// Input is validated, NUL-terminated card text; callbacks cannot retain scratch.
TextLayoutResult drawWrappedText(const char* text, int width, unsigned maxLines, const TextPainter& painter);
// Coverage checks see normalized layout whitespace, not nonexistent newline/tab
// glyphs. Bounded UTF-8 chunks; callback cannot retain the temporary text.
bool checkLayoutText(const char* text, void* context, bool (*check)(void*, const char*));
}  // namespace PocketDaily::Content
