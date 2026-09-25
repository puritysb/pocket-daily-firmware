#pragma once

#include <cstddef>
#include <cstdint>

namespace PocketDaily::Text {
// Strict UTF-8 for text the reader draws: rejects overlong encodings,
// surrogates, non-Unicode values, truncation, NUL and control characters (C0
// and C1). LF is accepted only when `allowNewline` is set. `length` is in
// bytes; the text need not be NUL-terminated.
inline bool validUtf8(const char* text, const size_t length, const bool allowNewline) {
  if (!text && length) return false;
  for (size_t i = 0; i < length;) {
    uint32_t point = static_cast<uint8_t>(text[i++]);
    unsigned continuation = 0;
    uint32_t minimum = 0;
    if (point >= 0xC2 && point <= 0xDF) {
      point &= 0x1F;
      continuation = 1;
      minimum = 0x80;
    } else if (point >= 0xE0 && point <= 0xEF) {
      point &= 0x0F;
      continuation = 2;
      minimum = 0x800;
    } else if (point >= 0xF0 && point <= 0xF4) {
      point &= 7;
      continuation = 3;
      minimum = 0x10000;
    } else if (point >= 0x80) {
      return false;
    }
    if (continuation > length - i) return false;
    for (unsigned j = 0; j < continuation; ++j) {
      const uint8_t next = static_cast<uint8_t>(text[i++]);
      if ((next & 0xC0) != 0x80) return false;
      point = (point << 6) | (next & 0x3F);
    }
    if (point < minimum || point > 0x10FFFF || (point >= 0xD800 && point <= 0xDFFF)) return false;
    if ((point < 0x20 && !(allowNewline && point == '\n')) || (point >= 0x7F && point <= 0x9F)) return false;
  }
  return true;
}
}  // namespace PocketDaily::Text
