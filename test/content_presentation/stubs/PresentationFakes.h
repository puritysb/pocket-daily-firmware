#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "pocket_daily/ContentViewState.h"

namespace PresentationFake {
inline auto loadResult = PocketDaily::Content::ContentViewState::LoadResult::Ready;
inline uint8_t cardCount = 3;
inline bool fontAvailable = true;
inline bool fontReady = true;
inline bool drawSucceeds = true;
inline int missingGlyphs = 0;
inline int loads = 0;
inline int releases = 0;
inline int draws = 0;
inline int displays = 0;
inline std::vector<char> titles;
inline std::vector<std::string> checkedText;
inline std::vector<std::string> labels;
inline std::function<void()> duringDisplay;
}  // namespace PresentationFake

class GfxRenderer {
 public:
  int ensureSdCardFontReady(int, const char* text, uint8_t) {
    PresentationFake::checkedText.emplace_back(text);
    return PresentationFake::missingGlyphs;
  }
  void displayBuffer() {
    ++PresentationFake::displays;
    if (PresentationFake::duringDisplay) PresentationFake::duringDisplay();
  }
};
class MappedInputManager {
 public:
  struct Labels {
    const char* btn1;
    const char* btn2;
    const char* btn3;
    const char* btn4;
  };
  Labels mapLabels(const char* a, const char* b, const char* c, const char* d) const { return {a, b, c, d}; }
};
class SdCardFont {
 public:
  enum class LoadMode { Cached, BoundedUI };
};
struct FakeFontSystem {
  int ensureUiFamilyLoaded(GfxRenderer&, const char*, SdCardFont::LoadMode mode, void (*)()) {
    ++PresentationFake::loads;
    return mode == SdCardFont::LoadMode::BoundedUI && PresentationFake::fontAvailable ? 1 : 0;
  }
  bool boundedUiFontReady(int) { return PresentationFake::fontReady; }
  void releaseLoaded(GfxRenderer&) { ++PresentationFake::releases; }
};
inline FakeFontSystem sdFontSystem;
struct FakeTheme {
  bool drawContentPage(GfxRenderer&, const PocketDaily::Content::ContentCard* card, const char*, int,
                       const char* const* labels) {
    PresentationFake::labels.assign(labels, labels + 4);
    ++PresentationFake::draws;
    PresentationFake::titles.push_back(card ? card->card.title[0] : '-');
    return PresentationFake::drawSucceeds;
  }
};
inline FakeTheme fakeTheme;
#define GUI fakeTheme
