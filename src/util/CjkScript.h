#pragma once

#include <cstdint>

// Which SD CJK script a UI string needs. Shared by UiCjkFont (family
// priorities) and the Sync screen presenter (whether to load the bounded CJK
// family at all), so both classify text identically. Pure; no allocation.
namespace CjkScript {

enum class Script : uint8_t { None, Hangul, Kana, Han };

inline bool decodeNext(const unsigned char*& p, uint32_t& cp) {
  const unsigned char c = *p;
  if (c == 0) return false;
  if (c < 0x80) {
    cp = c;
    p++;
    return true;
  }

  uint32_t value = 0;
  int extra = 0;
  if ((c & 0xE0) == 0xC0) {
    value = c & 0x1F;
    extra = 1;
  } else if ((c & 0xF0) == 0xE0) {
    value = c & 0x0F;
    extra = 2;
  } else if ((c & 0xF8) == 0xF0) {
    value = c & 0x07;
    extra = 3;
  } else {
    p++;
    return false;
  }

  p++;
  for (int i = 0; i < extra; i++) {
    if ((*p & 0xC0) != 0x80) return false;
    value = (value << 6) | (*p & 0x3F);
    p++;
  }
  cp = value;
  return true;
}

// Hangul or Kana anywhere decides immediately; Han alone is reported last.
inline Script classify(const char* text) {
  if (!text) return Script::None;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
  bool hasHan = false;
  uint32_t cp = 0;
  while (*p) {
    if (!decodeNext(p, cp)) continue;
    if ((cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x3130 && cp <= 0x318F)) {
      return Script::Hangul;
    }
    if ((cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x31F0 && cp <= 0x31FF) || (cp >= 0xFF66 && cp <= 0xFF9F)) {
      return Script::Kana;
    }
    if ((cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF)) {
      hasHan = true;
    }
  }
  return hasHan ? Script::Han : Script::None;
}

// True when the built-in Latin UI fonts cannot draw `text`.
inline bool needsCjkFont(const char* text) { return classify(text) != Script::None; }

}  // namespace CjkScript
