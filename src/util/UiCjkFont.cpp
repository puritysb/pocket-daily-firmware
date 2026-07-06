#include "util/UiCjkFont.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include <cstdint>
#include <cstring>

#include "CrossPointSettings.h"
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

const char* const* prioritiesFor(Script script) {
  static const char* const hangul[] = {"NotoSansKR", "Pretendard", "AgentDeckKR", "BIZUDGothic", "NotoSansJP", nullptr};
  // Japanese families must mirror lib/EpdFont/scripts/sd-fonts.yaml
  // (BIZUDGothic, IPAexMincho, NotoSansJP). IPAexMincho is a Mincho (serif) face
  // and is listed after the gothic families because gothic renders better at
  // small UI sizes — but it still has to be recognised, otherwise a user whose
  // only installed JP font is IPAexMincho gets no coverage from this list.
  static const char* const japanese[] = {"BIZUDGothic", "NotoSansJP", "IPAexMincho",
                                         "NotoSansKR",  "Pretendard", nullptr};
  static const char* const han[] = {"NotoSansJP", "BIZUDGothic", "IPAexMincho", "NotoSansKR", "Pretendard", nullptr};

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

uint8_t styleMaskFor(EpdFontFamily::Style style) {
  switch (style) {
    case EpdFontFamily::BOLD:
      return 0x02;
    case EpdFontFamily::ITALIC:
      return 0x04;
    case EpdFontFamily::BOLD_ITALIC:
      return 0x08;
    case EpdFontFamily::REGULAR:
    default:
      return 0x01;
  }
}

bool listContains(const char* const* list, const char* name) {
  if (!list || !name) return false;
  for (int i = 0; list[i]; i++) {
    if (strcmp(list[i], name) == 0) return true;
  }
  return false;
}

}  // namespace

namespace UiCjkFont {

int fontForText(const GfxRenderer& renderer, const char* text, int fallbackFontId, EpdFontFamily::Style style) {
  const char* const* priorities = prioritiesFor(classify(text));
  if (!priorities) return fallbackFontId;

  auto& mutableRenderer = const_cast<GfxRenderer&>(renderer);
  const uint8_t styleMask = styleMaskFor(style);
  const char* partialFamily = nullptr;
  int bestMissed = 0x7FFFFFFF;

  // 0) Prefer the currently-loaded SD font if it already covers the text.
  //    SdCardFontManager is single-slot — loading a different family unloads
  //    the resident one. When a screen renders several CJK strings of mixed
  //    script in quick succession (e.g. a kana book title + a kanji author),
  //    different script classifications map to different priority lists whose
  //    first choices differ (Kana→BIZUDGothic, Han→NotoSansJP). Without this
  //    reuse check, the second fontForText() call unloads the font the first
  //    call selected and returned, so by the time drawText() runs with that
  //    earlier font ID the family has been unloaded and every glyph is
  //    dropped — the "Japanese title looks garbled while the body is fine"
  //    symptom on devices that have more than one JP font installed.
  const int loadedFontId = sdFontSystem.currentLoadedFontId();
  if (loadedFontId != 0) {
    const int missed = renderer.ensureSdCardFontReady(loadedFontId, text, styleMask);
    if (missed == 0) {
      renderer.prewarmSdCardFont(loadedFontId, text, styleMask);
      LOG_DBG("UICJK", "Reusing loaded fontId=%d for \"%s\"", loadedFontId, text);
      return loadedFontId;
    }
    LOG_DBG("UICJK", "Loaded fontId=%d missed=%d for \"%s\", trying priorities", loadedFontId, missed, text);
  }

  // 1) Script-specific priority families (gothic before serif for small UI sizes).
  for (int i = 0; priorities[i]; i++) {
    const int fontId = sdFontSystem.ensureUiFamilyLoaded(mutableRenderer, priorities[i]);
    if (fontId == 0) {
      LOG_DBG("UICJK", "Family unavailable: %s", priorities[i]);
      continue;
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
  }

  // 2) The user's currently-selected reading font. Body text already renders
  //    through this family (SETTINGS.getReaderFontId()), so when it covers the
  //    text it is the authoritative match — and it may be a CJK family (e.g.
  //    IPAexMincho) that the hardcoded lists above do not name. Without this
  //    step, a Japanese reading font whose name isn't in the priority list
  //    leaves titles/TOC rendering with the Latin-only built-in UI font, which
  //    silently drops every kana/kanji glyph (renderCharImpl skips any codepoint
  //    the font lacks). Skipped when the reading font is already one of the
  //    priority candidates (dedup) or when no SD reading font is selected.
  const char* readingFamily = SETTINGS.sdFontFamilyName;
  if (readingFamily && readingFamily[0] != '\0' && !listContains(priorities, readingFamily)) {
    const int fontId = sdFontSystem.ensureUiFamilyLoaded(mutableRenderer, readingFamily);
    if (fontId != 0) {
      const int missed = renderer.ensureSdCardFontReady(fontId, text, styleMask);
      if (missed == 0) {
        renderer.prewarmSdCardFont(fontId, text, styleMask);
        LOG_DBG("UICJK", "Selected reading family %s fontId=%d for \"%s\"", readingFamily, fontId, text);
        return fontId;
      }
      LOG_DBG("UICJK", "Rejected reading family %s fontId=%d missed=%d for \"%s\"", readingFamily, fontId, missed,
              text);
      if (missed < bestMissed) {
        bestMissed = missed;
        partialFamily = readingFamily;
      }
    } else {
      LOG_DBG("UICJK", "Reading family unavailable: %s", readingFamily);
    }
  }

  // 3) No family fully covered the text — accept the partial match (fewest
  //    missing glyphs) over the Latin-only built-in fallback.
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
