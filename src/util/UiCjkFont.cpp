#include "util/UiCjkFont.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include <cstdint>

#include "SdCardFontSystem.h"

namespace {

enum class Script : uint8_t { None, Hangul, Kana, Han };

bool decodeNext(const unsigned char*& p, uint32_t& cp) {
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

Script classify(const char* text) {
  if (!text) return Script::None;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
  bool hasHan = false;
  uint32_t cp = 0;
  while (*p) {
    if (!decodeNext(p, cp)) continue;
    if ((cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0x1100 && cp <= 0x11FF) ||
        (cp >= 0x3130 && cp <= 0x318F)) {
      return Script::Hangul;
    }
    if ((cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x31F0 && cp <= 0x31FF) ||
        (cp >= 0xFF66 && cp <= 0xFF9F)) {
      return Script::Kana;
    }
    if ((cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
        (cp >= 0xF900 && cp <= 0xFAFF)) {
      hasHan = true;
    }
  }
  return hasHan ? Script::Han : Script::None;
}

const char* const* prioritiesFor(Script script) {
  static const char* const hangul[] = {"NotoSansKR", "Pretendard", "AgentDeckKR", "BIZUDGothic", "NotoSansJP",
                                      nullptr};
  static const char* const japanese[] = {"BIZUDGothic", "NotoSansJP", "NotoSansKR", "Pretendard", nullptr};
  static const char* const han[] = {"NotoSansJP", "BIZUDGothic", "NotoSansKR", "Pretendard", nullptr};

  switch (script) {
    case Script::Hangul:
      return hangul;
    case Script::Kana:
      return japanese;
    case Script::Han:
      return han;
    case Script::None:
    default:
      return nullptr;
  }
}

}  // namespace

namespace UiCjkFont {

int fontForText(const GfxRenderer& renderer, const char* text, int fallbackFontId, EpdFontFamily::Style style) {
  const char* const* priorities = prioritiesFor(classify(text));
  if (!priorities) return fallbackFontId;

  auto& mutableRenderer = const_cast<GfxRenderer&>(renderer);
  const char* partialFamily = nullptr;
  int bestMissed = 0x7FFFFFFF;
  for (int i = 0; priorities[i]; i++) {
    const int fontId = sdFontSystem.ensureUiFamilyLoaded(mutableRenderer, priorities[i]);
    if (fontId != 0) {
      uint8_t styleMask = 0x01;
      switch (style) {
        case EpdFontFamily::BOLD:
          styleMask = 0x02;
          break;
        case EpdFontFamily::ITALIC:
          styleMask = 0x04;
          break;
        case EpdFontFamily::BOLD_ITALIC:
          styleMask = 0x08;
          break;
        case EpdFontFamily::REGULAR:
        default:
          styleMask = 0x01;
          break;
      }
      const int missed = renderer.ensureSdCardFontReady(fontId, text, styleMask);
      if (missed == 0) {
        renderer.prewarmSdCardFont(fontId, text, styleMask);
        LOG_DBG("UICJK", "Selected %s fontId=%d for \"%s\"", priorities[i], fontId, text);
        return fontId;
      }
      LOG_DBG("UICJK", "Rejected %s fontId=%d missed=%d for \"%s\"", priorities[i], fontId, missed, text);
      if (missed < bestMissed) {
        bestMissed = missed;
        partialFamily = priorities[i];
      }
    } else {
      LOG_DBG("UICJK", "Family unavailable: %s", priorities[i]);
    }
  }

  if (partialFamily) {
    const int fontId = sdFontSystem.ensureUiFamilyLoaded(mutableRenderer, partialFamily);
    if (fontId != 0) {
      renderer.ensureSdCardFontReady(fontId, text, 0x0F);
      renderer.prewarmSdCardFont(fontId, text, 0x0F);
      LOG_DBG("UICJK", "Using partial %s fontId=%d for \"%s\"", partialFamily, fontId, text);
      return fontId;
    }
  }
  LOG_DBG("UICJK", "Fallback built-in fontId=%d for \"%s\"", fallbackFontId, text ? text : "");
  return fallbackFontId;
}

}  // namespace UiCjkFont
