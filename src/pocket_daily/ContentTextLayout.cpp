#include "ContentTextLayout.h"

#include <cstring>

namespace PocketDaily::Content {
namespace {
bool space(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }
unsigned span(const char* p) {
  const auto c = static_cast<uint8_t>(*p);
  const unsigned size = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
  for (unsigned i = 1; i < size; ++i)
    if (!p[i] || (static_cast<uint8_t>(p[i]) & 0xC0) != 0x80) return 1;
  return size;
}
}  // namespace

bool checkLayoutText(const char* text, void* context, bool (*check)(void*, const char*)) {
  if (!text || !check) return false;
  while (*text) {
    char chunk[128]{};
    unsigned length = 0;
    while (*text) {
      const unsigned bytes = span(text);
      if (length + bytes >= sizeof(chunk)) break;
      if (space(*text))
        chunk[length] = ' ';
      else
        memcpy(chunk + length, text, bytes);
      length += bytes;
      text += bytes;
    }
    chunk[length] = '\0';
    if (!check(context, chunk)) return false;
  }
  return true;
}

TextLayoutResult drawWrappedText(const char* text, int width, unsigned maxLines, const TextPainter& painter) {
  TextLayoutResult result;
  if (!text || !painter.measure || !painter.draw || width <= 0 || !maxLines) return result;
  while (*text && result.lines < maxLines) {
    while (space(*text)) ++text;
    if (!*text) break;
    char line[128]{};
    unsigned fit = 0, lastSpace = 0;
    while (text[fit] && text[fit] != '\n' && text[fit] != '\r') {
      const unsigned bytes = span(text + fit);
      if (fit + bytes >= sizeof(line) - 3) break;
      memcpy(line + fit, text + fit, bytes);
      if (text[fit] == '\t') line[fit] = ' ';
      line[fit + bytes] = '\0';
      if (painter.measure(painter.context, line) > width) break;
      if (space(text[fit])) lastSpace = fit;
      fit += bytes;
    }
    unsigned consumed = fit;
    if (text[fit] && !space(text[fit]) && lastSpace) fit = consumed = lastSpace;
    if (!fit) {
      // A glyph cannot fit: do not paint outside the requested text region.
      result.truncated = true;
      return result;
    }
    line[fit] = '\0';
    text += consumed;
    while (space(*text)) ++text;
    if (result.lines + 1 == maxLines && *text) {
      result.truncated = true;
      while (true) {
        memcpy(line + fit, "...", 4);
        if (painter.measure(painter.context, line) <= width) break;
        if (!fit) {
          line[0] = '\0';
          break;
        }
        --fit;
        while (fit && (static_cast<uint8_t>(line[fit]) & 0xC0) == 0x80) --fit;
      }
    }
    painter.draw(painter.context, line, result.lines++);
  }
  return result;
}
}  // namespace PocketDaily::Content
