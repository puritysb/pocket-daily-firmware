// Offline real-font check. The local fixture is explicit; no device or network I/O.
// This tests the renderer's borrowed per-glyph fallback contract. The separate
// LibraryFontSystemTest owns the actual policy/entry/exit lifecycle regression.
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <LibraryText.h>
#include <SdCardFont.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "EpdFont/builtinFonts/ubuntu_12_bold.h"
#include "EpdFont/builtinFonts/ubuntu_12_regular.h"
#include "fontIds.h"

namespace {
using Bytes = std::vector<uint8_t>;
constexpr int WORLD_ID = 7;
constexpr int X = 24, Y = 40;
constexpr const char* TITLE = "Pocket RC 한국어 읽기";
constexpr const char* NFD_TITLE =
    "Pocket RC \u1112\u1161\u11ab\u1100\u116e\u11a8\u110b\u1165 \u110b\u1175\u11b0\u1100\u1175";
constexpr const char* PREFIX = "Pocket RC ";
constexpr const char* FIRST = "한국어";
constexpr const char* LAST = "읽기";
constexpr const char* ELLIPSIS = "\xe2\x80\xa6";
constexpr uint32_t SYLLABLES[] = {0xD55C, 0xAD6D, 0xC5B4, 0xC77D, 0xAE30};

void require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);  // Host-only test failure, not firmware.
}
struct Fallback {
  SdCardFont* font = nullptr;
  bool enabled = false;
  unsigned requests = 0;
};
SdCardFont* fallback(void* context, bool load) {
  auto& state = *static_cast<Fallback*>(context);
  if (load) ++state.requests;
  return state.enabled ? state.font : nullptr;
}
std::string utf8(uint32_t cp) {
  std::string value;
  value.reserve(4);
  utf8AppendCodepoint(cp, value);
  return value;
}
bool equal(const HalDisplay& a, const HalDisplay& b) {
  return a.getBufferSize() == b.getBufferSize() &&
         std::memcmp(a.getFrameBuffer(), b.getFrameBuffer(), a.getBufferSize()) == 0;
}
bool nonblank(const HalDisplay& panel) {
  return std::any_of(panel.getFrameBuffer(), panel.getFrameBuffer() + panel.getBufferSize(),
                     [](uint8_t byte) { return byte != 0xff; });
}

// The independent reference routes whole known segments explicitly: Latin stays
// Ubuntu; Korean is drawn with the actual Cached World face at the UI baseline.
// It does not call the per-glyph fallback provider or replace the whole title font.
void referenceTitle(GfxRenderer& renderer, EpdFontFamily::Style style) {
  const int worldY = Y + renderer.getFontAscenderSize(UI_12_FONT_ID) - renderer.getFontAscenderSize(WORLD_ID);
  int x = X;
  renderer.drawText(UI_12_FONT_ID, x, Y, PREFIX, true, style);
  x += renderer.getTextAdvanceX(UI_12_FONT_ID, PREFIX, style);
  renderer.drawText(WORLD_ID, x, worldY, FIRST, true, style);
  x += renderer.getTextAdvanceX(WORLD_ID, FIRST, style);
  renderer.drawText(UI_12_FONT_ID, x, Y, " ", true, style);
  x += renderer.getTextAdvanceX(UI_12_FONT_ID, " ", style);
  renderer.drawText(WORLD_ID, x, worldY, LAST, true, style);
}

// Width oracle for this non-ligating fixture uses each actual face's advances
// and Ubuntu's own Latin kerning. No missing-glyph provider or truncator is used.
int explicitWidth(const EpdFontFamily& ui, SdCardFont& world, const char* text, EpdFontFamily::Style style) {
  const auto* cursor = reinterpret_cast<const uint8_t*>(text);
  int width = 0;
  int32_t priorAdvance = 0;
  uint32_t priorPrimary = 0;
  bool first = true;
  while (const uint32_t cp = utf8NextCodepoint(&cursor)) {
    const EpdGlyph* glyph = ui.findGlyph(cp, style);
    const bool primary = glyph != nullptr;
    if (!glyph) glyph = world.getEpdFont(world.resolveStyle(static_cast<uint8_t>(style)))->findGlyph(cp);
    require(glyph != nullptr, "reference fixture glyph missing");
    if (!first)
      width += fp4::toPixel(priorAdvance + (primary && priorPrimary ? ui.getKerning(priorPrimary, cp, style) : 0));
    priorAdvance = glyph->advanceX;
    priorPrimary = primary ? cp : 0;
    first = false;
  }
  return width + fp4::toPixel(priorAdvance);
}
std::string truncatedOracle(const EpdFontFamily& ui, SdCardFont& world, int bound, EpdFontFamily::Style style) {
  std::string expected;
  expected.reserve(std::strlen(TITLE) + 3);
  const char* cursor = TITLE;
  while (*cursor) {
    const char* next = cursor + 1;
    while ((*next & 0xc0) == 0x80) ++next;
    std::string candidate(TITLE, next);
    candidate += ELLIPSIS;
    if (explicitWidth(ui, world, candidate.c_str(), style) >= bound) break;
    expected.assign(TITLE, next);
    cursor = next;
  }
  expected += ELLIPSIS;
  return expected;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "Usage: gfx_library_ui_glyph_check <local-PocketSansWorld_12.cpfont>\n");
    return 2;
  }
  try {
    std::ifstream input(argv[1], std::ios::binary | std::ios::ate);
    require(bool(input), "local font fixture unavailable");
    const auto count = input.tellg();
    require(count > 0 && count <= 64 * 1024 * 1024, "font fixture outside host-only 64MiB limit");
    // Host-only immutable fixture bytes, once per process. Too large for stack;
    // the HAL borrows this storage, and the test never copies font-sized buffers.
    Bytes bytes(static_cast<size_t>(count));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), count);
    require(bool(input), "font fixture read incomplete");
    const PocketUIHost::Asset assets[]{{"/World12.cpfont", bytes}};
    const PocketUIHost::AssetScope scope(assets);
    SdCardFont bounded, cached;
    require(bounded.load("/World12.cpfont", SdCardFont::LoadMode::BoundedUI), "bounded fixture load failed");
    require(cached.load("/World12.cpfont", SdCardFont::LoadMode::Cached), "cached reference load failed");
    EpdFont uiRegular(&ubuntu_12_regular), uiBold(&ubuntu_12_bold);
    const EpdFontFamily ui(&uiRegular, &uiBold);
    HalDisplay actualPanel(792, 528), referencePanel(792, 528);
    GfxRenderer actual(actualPanel), reference(referencePanel);
    actual.begin();
    reference.begin();
    actual.insertFont(UI_12_FONT_ID, ui);
    reference.insertFont(UI_12_FONT_ID, ui);
    reference.insertFont(WORLD_ID, EpdFontFamily(cached.getEpdFont(0), cached.getEpdFont(1)));
    reference.registerSdCardFont(WORLD_ID, &cached);
    Fallback provider{&bounded, true, 0};
    actual.setFallbackFontProvider(&fallback, &provider);
    unsigned glyphCases = 0, titleCases = 0, latinCases = 0, truncationCases = 0, missingMarkCases = 0;
    unsigned nfdFrames = 0, nfdTruncations = 0;
    for (auto orientation : {GfxRenderer::Portrait, GfxRenderer::LandscapeClockwise, GfxRenderer::PortraitInverted,
                             GfxRenderer::LandscapeCounterClockwise}) {
      actual.setOrientation(orientation);
      reference.setOrientation(orientation);
      for (auto style : {EpdFontFamily::REGULAR, EpdFontFamily::BOLD}) {
        for (uint32_t cp : SYLLABLES) {
          require(ui.findGlyph(cp, style) == nullptr, "fixture must exercise a real Ubuntu coverage gap");
          require(bounded.hasGlyph(cp, static_cast<uint8_t>(style)), "World fixture must cover every Korean syllable");
          const auto text = utf8(cp);
          actual.clearScreen();
          reference.clearScreen();
          provider.enabled = true;
          actual.drawText(UI_12_FONT_ID, X, Y, text.c_str(), true, style);
          reference.drawText(WORLD_ID, X,
                             Y + actual.getFontAscenderSize(UI_12_FONT_ID) - reference.getFontAscenderSize(WORLD_ID),
                             text.c_str(), true, style);
          require(nonblank(actualPanel) && equal(actualPanel, referencePanel),
                  "per-glyph World pixels/baseline differ");
          ++glyphCases;
          // The old disabled-provider behavior must draw the exact dotted mark,
          // not a blank or an accidental success with synthetic/covered glyphs.
          actual.clearScreen();
          reference.clearScreen();
          provider.enabled = false;
          actual.drawText(UI_12_FONT_ID, X, Y, text.c_str(), true, style);
          for (int y = 0; y < 12; ++y)
            for (int x = 0; x < 8; ++x)
              if ((x == 0 || x == 7 || y == 0 || y == 11) && ((x + y) & 1) == 0)
                reference.drawPixel(X + 2 + x, Y + 24 - 12 + y);
          require(equal(actualPanel, referencePanel), "disabled fallback did not match real 8x12 missing mark");
          ++missingMarkCases;
        }
        provider.enabled = true;
        actual.clearScreen();
        reference.clearScreen();
        actual.drawText(UI_12_FONT_ID, X, Y, TITLE, true, style);
        referenceTitle(reference, style);
        require(equal(actualPanel, referencePanel), "mixed title changed Latin face or Korean baseline/position");
        require(actual.getTextAdvanceX(UI_12_FONT_ID, TITLE, style) == explicitWidth(ui, cached, TITLE, style),
                "mixed title advance differs from explicit face metrics");
        ++titleCases;
        // Display composition runs on the copied label, before the real renderer
        // measures/truncates it. The raw filename/metadata must remain untouched.
        const std::string rawTitle = NFD_TITLE;
        std::string display = rawTitle;
        library::composeDisplayText(display);
        require(rawTitle == NFD_TITLE, "NFD raw identity changed");
        actual.clearScreen();
        actual.drawText(UI_12_FONT_ID, X, Y, display.c_str(), true, style);
        require(equal(actualPanel, referencePanel), "NFD display pixels differ from NFC reference");
        require(
            actual.getTextAdvanceX(UI_12_FONT_ID, display.c_str(), style) == explicitWidth(ui, cached, TITLE, style),
            "NFD display width differs from NFC reference");
        ++nfdFrames;
        // A non-composing path must visibly differ, otherwise the fixture cannot
        // detect the original separate-Jamo regression.
        actual.clearScreen();
        actual.drawText(UI_12_FONT_ID, X, Y, rawTitle.c_str(), true, style);
        require(!equal(actualPanel, referencePanel), "raw NFD negative failed to expose separate Jamo");

        const unsigned requests = provider.requests;
        actual.clearScreen();
        reference.clearScreen();
        actual.drawText(UI_12_FONT_ID, X, Y, PREFIX, true, style);
        reference.drawText(UI_12_FONT_ID, X, Y, PREFIX, true, style);
        require(equal(actualPanel, referencePanel) && provider.requests == requests,
                "Latin-only text invoked fallback");
        ++latinCases;
        for (int bound : {165, 195, 225}) {
          const auto expected = truncatedOracle(ui, cached, bound, style);
          const auto result = actual.truncatedText(UI_12_FONT_ID, TITLE, bound, style);
          require(result == expected, "truncation differs from explicit Korean/Latin metric oracle");
          require(explicitWidth(ui, cached, result.c_str(), style) < bound, "truncated title exceeds row width");
          ++truncationCases;
          const auto nfdResult = actual.truncatedText(UI_12_FONT_ID, display.c_str(), bound, style);
          require(nfdResult == expected, "NFD truncation differs from NFC metric oracle");
          actual.clearScreen();
          reference.clearScreen();
          actual.drawText(UI_12_FONT_ID, X, Y, nfdResult.c_str(), true, style);
          reference.setFallbackFontProvider(&fallback, &provider);
          reference.drawText(UI_12_FONT_ID, X, Y, expected.c_str(), true, style);
          reference.setFallbackFontProvider(nullptr, nullptr);
          require(equal(actualPanel, referencePanel), "NFD truncated pixels differ from NFC");
          ++nfdTruncations;
        }
        require(actualPanel.guardsIntact() && referencePanel.guardsIntact(), "framebuffer guard changed");
        require(!bounded.boundedReadFailed(), "BoundedUI font read failed");
      }
    }
    require(provider.requests > 0, "fallback path not exercised");
    std::printf(
        "PASS: %u actual Korean glyph frames, %u exact missing-mark negatives, %u mixed titles, %u Latin frames, "
        "%u truncations; regular/bold and four orientations; Cached reference == BoundedUI fallback. "
        "NFD: %u composed frames and %u truncations equal NFC; raw NFD differs. "
        "Host-only pixels, not physical panel/lifecycle proof.\n",
        glyphCases, missingMarkCases, titleCases, latinCases, truncationCases, nfdFrames, nfdTruncations);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Library UI glyph check failed: %s\n", error.what());
    return 1;
  }
}
