// Actual SdCardFontSystem/Manager/Font/renderer lifecycle; only discovery,
// storage, heap capabilities and settings persistence are host boundaries.
#include <CrossPointSettings.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <SdCardFontSystem.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "fontIds.h"
#include "lib/EpdFont/builtinFonts/ubuntu_12_regular.h"

namespace {
using Bytes = std::vector<uint8_t>;
bool worldDiscovered = true;

void put16(Bytes& bytes, size_t at, uint16_t value) {
  bytes[at] = static_cast<uint8_t>(value);
  bytes[at + 1] = static_cast<uint8_t>(value >> 8);
}
void put32(Bytes& bytes, size_t at, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) bytes[at + i] = static_cast<uint8_t>(value >> (8 * i));
}
Bytes fontBytes(uint32_t cp, uint8_t ink) {
  Bytes bytes(94, 0);
  std::memcpy(bytes.data(), "CPFONT", 6);
  bytes[8] = 4;
  bytes[12] = 1;
  put32(bytes, 36, 1);
  put32(bytes, 40, 1);
  bytes[44] = 29;
  bytes[45] = 24;
  put32(bytes, 56, 64);
  put32(bytes, 64, cp);
  put32(bytes, 68, cp);
  bytes[76] = 4;
  bytes[77] = 4;
  put16(bytes, 78, 96);
  put16(bytes, 82, 8);
  put16(bytes, 84, 2);
  bytes[92] = ink;
  bytes[93] = static_cast<uint8_t>(~ink);
  return bytes;
}

class LibraryFontSystem : public testing::Test {
 protected:
  void SetUp() override {
    worldDiscovered = true;
    SETTINGS.fontPointSize = 18;
    SETTINGS.sdFontFamilyName[0] = '\0';
    SETTINGS.saveCalls = 0;
    renderer.begin();
    renderer.insertFont(UI_12_FONT_ID, EpdFontFamily(&ui));
    system.begin(renderer);
    savedSettings.assign(reinterpret_cast<const uint8_t*>(&SETTINGS),
                         reinterpret_cast<const uint8_t*>(&SETTINGS) + sizeof(SETTINGS));
  }
  void TearDown() override {
    system.releaseLoaded(renderer);
    EXPECT_EQ(SETTINGS.saveCalls, 0u);
    EXPECT_EQ(savedSettings, Bytes(reinterpret_cast<const uint8_t*>(&SETTINGS),
                                   reinterpret_cast<const uint8_t*>(&SETTINGS) + sizeof(SETTINGS)));
  }
  Bytes draw(const char* text) {
    renderer.clearScreen();
    renderer.drawText(UI_12_FONT_ID, 10, 10, text);
    return Bytes(renderer.getFrameBuffer(), renderer.getFrameBuffer() + renderer.getBufferSize());
  }
  SdCardFont* loaded() {
    const auto it = renderer.getSdCardFonts().find(system.currentLoadedFontId());
    return it == renderer.getSdCardFonts().end() ? nullptr : it->second;
  }
  const Bytes world = fontBytes(0xD55C, 0xA5);  // 한, distinct from missing mark
  const Bytes symbols = fontBytes(0x2705, 0xCB);
  const Bytes reader = fontBytes('Z', 0xE9);
  const PocketUIHost::Asset assets[3]{
      {"/world.cpfont", world}, {"/symbols.cpfont", symbols}, {"/reader.cpfont", reader}};
  const PocketUIHost::AssetScope scope{assets};
  const EpdFont ui{&ubuntu_12_regular};
  HalDisplay panel;
  GfxRenderer renderer{panel};
  SdCardFontSystem system;
  Bytes savedSettings;
};

TEST_F(LibraryFontSystem, ColdLibrarySuppliesMissingGlyphWithoutChangingLatinOrReaderSize) {
  const Bytes missing = draw("한");
  const Bytes latin = draw("Pocket RC ");
  ASSERT_TRUE(system.ensureLibraryUiFont(renderer));
  ASSERT_NE(loaded(), nullptr);
  EXPECT_EQ(loaded()->loadMode(), SdCardFont::LoadMode::BoundedUI);
  EXPECT_FALSE(loaded()->hasAdvanceTable());
  EXPECT_NE(draw("한"), missing);
  EXPECT_EQ(draw("Pocket RC "), latin);
  EXPECT_EQ(SETTINGS.fontPointSize, 18);
  EXPECT_EQ(renderer.getSdCardFonts().size(), 1u);
}

TEST_F(LibraryFontSystem, ReentryReusesOneFontAndExitUnregistersBeforeNextRender) {
  const Bytes missing = draw("한");
  ASSERT_TRUE(system.ensureLibraryUiFont(renderer));
  SdCardFont* first = loaded();
  const Bytes rendered = draw("한");
  ASSERT_NE(rendered, missing);
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(system.ensureLibraryUiFont(renderer));
    EXPECT_EQ(loaded(), first);
    EXPECT_EQ(draw("한"), rendered);
  }
  system.releaseLoaded(renderer);
  EXPECT_EQ(system.currentLoadedFontId(), 0);
  EXPECT_TRUE(renderer.getSdCardFonts().empty());
  EXPECT_EQ(draw("한"), missing);
  ASSERT_TRUE(system.ensureLibraryUiFont(renderer));
  EXPECT_EQ(draw("한"), rendered);
}

TEST_F(LibraryFontSystem, NetworkBoundedUiDoesNotOptIntoLibraryProvider) {
  const Bytes missing = draw("한");
  ASSERT_TRUE(system.ensureLibraryUiFont(renderer));
  SdCardFont* first = loaded();
  ASSERT_NE(draw("한"), missing);
  ASSERT_NE(system.ensureUiFamilyLoaded(renderer, "PocketSansWorld", SdCardFont::LoadMode::BoundedUI), 0);
  EXPECT_EQ(loaded(), first);
  EXPECT_EQ(draw("한"), missing);
  ASSERT_TRUE(system.ensureLibraryUiFont(renderer));
  EXPECT_EQ(loaded(), first);
  EXPECT_NE(draw("한"), missing);
}

TEST_F(LibraryFontSystem, ReaderTransitionUsesCachedFamilyAndOriginalSymbolsProvider) {
  const Bytes missingHangul = draw("한");
  const Bytes missingSymbol = draw("✅");
  ASSERT_TRUE(system.ensureLibraryUiFont(renderer));
  EXPECT_NE(draw("한"), missingHangul);
  EXPECT_EQ(draw("✅"), missingSymbol);
  ASSERT_NE(system.ensureAutomaticReaderFontLoaded(renderer), 0);
  ASSERT_NE(loaded(), nullptr);
  EXPECT_EQ(loaded()->loadMode(), SdCardFont::LoadMode::Cached);
  EXPECT_NE(draw("✅"), missingSymbol);
  EXPECT_EQ(draw("한"), missingHangul);  // Library opt-in did not leak to UI
  ASSERT_NE(system.loadedFallbackFont(), nullptr);
  ASSERT_TRUE(system.ensureLibraryUiFont(renderer));
  EXPECT_EQ(system.loadedFallbackFont(), nullptr);  // no second Symbols font
  EXPECT_EQ(loaded()->loadMode(), SdCardFont::LoadMode::BoundedUI);
}

TEST_F(LibraryFontSystem, MissingFamilyFailsClosedAndCanRecoverOnNextDiscovery) {
  const Bytes missing = draw("한");
  ASSERT_TRUE(system.ensureLibraryUiFont(renderer));
  worldDiscovered = false;
  system.markRegistryDirty();
  EXPECT_FALSE(system.ensureLibraryUiFont(renderer));
  EXPECT_EQ(system.currentLoadedFontId(), 0);
  EXPECT_TRUE(renderer.getSdCardFonts().empty());
  EXPECT_EQ(draw("한"), missing);
  worldDiscovered = true;
  system.markRegistryDirty();
  ASSERT_TRUE(system.ensureLibraryUiFont(renderer));
  EXPECT_NE(draw("한"), missing);
}

TEST_F(LibraryFontSystem, UnreadableFontFailsClosedWithoutReplacingSettings) {
  const Bytes missing = draw("한");
  {  // Empty storage while discovery still reports the font.
    const PocketUIHost::AssetScope unavailable{std::span<const PocketUIHost::Asset>{}};
    EXPECT_FALSE(system.ensureLibraryUiFont(renderer));
    EXPECT_EQ(system.currentLoadedFontId(), 0);
    EXPECT_TRUE(renderer.getSdCardFonts().empty());
    EXPECT_EQ(draw("한"), missing);
  }
  ASSERT_TRUE(system.ensureLibraryUiFont(renderer));
  EXPECT_NE(draw("한"), missing);
}

TEST_F(LibraryFontSystem, SavedReaderFamilyAndPointSizeSurviveLibraryLifetime) {
  std::strcpy(SETTINGS.sdFontFamilyName, "SavedReader");
  savedSettings.assign(reinterpret_cast<const uint8_t*>(&SETTINGS),
                       reinterpret_cast<const uint8_t*>(&SETTINGS) + sizeof(SETTINGS));
  system.ensureLoaded(renderer);
  const int readerId = system.currentLoadedFontId();
  ASSERT_NE(readerId, 0);
  ASSERT_TRUE(system.ensureLibraryUiFont(renderer));
  EXPECT_NE(system.currentLoadedFontId(), readerId);
  system.releaseLoaded(renderer);
  system.ensureLoaded(renderer);
  EXPECT_EQ(system.currentLoadedFontId(), readerId);
  ASSERT_NE(loaded(), nullptr);
  EXPECT_EQ(loaded()->loadMode(), SdCardFont::LoadMode::Cached);
  EXPECT_STREQ(SETTINGS.sdFontFamilyName, "SavedReader");
  EXPECT_EQ(SETTINGS.fontPointSize, 18);
}

// Proposed overlay regressions: compile/run only after root approves the patch.
TEST_F(LibraryFontSystem, ReaderOverlayKeepsResidentReaderAndLayoutKey) {
  std::strcpy(SETTINGS.sdFontFamilyName, "SavedReader");
  savedSettings.assign(reinterpret_cast<const uint8_t*>(&SETTINGS),
                       reinterpret_cast<const uint8_t*>(&SETTINGS) + sizeof(SETTINGS));
  system.ensureLoaded(renderer);
  const int readerId = system.currentLoadedFontId();
  SdCardFont* readerFace = loaded();
  const auto layoutKey = renderer.glyphLayoutKey();
  const Bytes missing = draw("한");
  const Bytes latin = draw("Pocket RC ");
  renderer.clearScreen();
  renderer.drawText(readerId, 20, 20, "ZZZZ");
  const Bytes readerPixels(renderer.getFrameBuffer(), renderer.getFrameBuffer() + renderer.getBufferSize());
  uint32_t owner = system.beginReaderOverlayUiFont(renderer);
  ASSERT_NE(owner, 0u);
  ASSERT_NE(system.loadedFallbackFont(), nullptr);
  EXPECT_EQ(system.loadedFallbackFont()->loadMode(), SdCardFont::LoadMode::BoundedUI);
  EXPECT_FALSE(system.loadedFallbackFont()->hasAdvanceTable());
  EXPECT_EQ(system.currentLoadedFontId(), readerId);
  EXPECT_EQ(loaded(), readerFace);
  EXPECT_EQ(readerFace->loadMode(), SdCardFont::LoadMode::Cached);
  EXPECT_EQ(renderer.getSdCardFonts().size(), 1u);
  EXPECT_EQ(renderer.glyphLayoutKey(), layoutKey);
  EXPECT_NE(draw("한"), missing);
  EXPECT_EQ(draw("Pocket RC "), latin);
  system.endReaderOverlayUiFont(owner);
  EXPECT_EQ(system.loadedFallbackFont(), nullptr);
  EXPECT_EQ(loaded(), readerFace);
  EXPECT_EQ(renderer.glyphLayoutKey(), layoutKey);
  renderer.clearScreen();
  renderer.drawText(readerId, 20, 20, "ZZZZ");
  EXPECT_EQ(Bytes(renderer.getFrameBuffer(), renderer.getFrameBuffer() + renderer.getBufferSize()), readerPixels);
}

TEST_F(LibraryFontSystem, NestedReaderOverlayOwnersReleaseOnlyAtLastExit) {
  system.ensureAutomaticReaderFontLoaded(renderer);
  const Bytes missing = draw("한");
  uint32_t owner = system.beginReaderOverlayUiFont(renderer);
  ASSERT_NE(owner, 0u);
  const SdCardFont* first = system.loadedFallbackFont();
  const Bytes rendered = draw("한");
  ASSERT_NE(rendered, missing);
  ASSERT_EQ(system.beginReaderOverlayUiFont(renderer), owner);
  EXPECT_EQ(system.loadedFallbackFont(), first);
  system.endReaderOverlayUiFont(owner);
  EXPECT_EQ(system.loadedFallbackFont(), first);
  EXPECT_EQ(draw("한"), rendered);
  system.endReaderOverlayUiFont(owner);
  EXPECT_EQ(system.loadedFallbackFont(), nullptr);
  EXPECT_EQ(draw("한"), missing);
  system.endReaderOverlayUiFont(owner);  // a non-owner cannot underflow the depth
  const uint32_t oldOwner = owner;
  owner = system.beginReaderOverlayUiFont(renderer);
  ASSERT_NE(owner, 0u);
  ASSERT_NE(owner, oldOwner);
  system.endReaderOverlayUiFont(oldOwner);
  EXPECT_EQ(draw("한"), rendered);
  system.endReaderOverlayUiFont(owner);
}

TEST_F(LibraryFontSystem, ReaderOverlayCannotOverrideNetworkOrLibraryOwnership) {
  ASSERT_TRUE(system.ensureLibraryUiFont(renderer));
  const SdCardFont* libraryFace = loaded();
  EXPECT_FALSE(system.beginReaderOverlayUiFont(renderer));
  system.endReaderOverlayUiFont(0);
  EXPECT_EQ(loaded(), libraryFace);
  ASSERT_NE(system.ensureUiFamilyLoaded(renderer, "PocketSansWorld", SdCardFont::LoadMode::BoundedUI), 0);
  EXPECT_FALSE(system.beginReaderOverlayUiFont(renderer));
  EXPECT_EQ(loaded(), libraryFace);
}

TEST_F(LibraryFontSystem, ReaderOverlayMissingFileRestoresSymbolsAndKeepsReader) {
  system.ensureAutomaticReaderFontLoaded(renderer);
  SdCardFont* readerFace = loaded();
  const Bytes symbol = draw("✅");
  ASSERT_NE(system.loadedFallbackFont(), nullptr);
  {
    const PocketUIHost::AssetScope unavailable{std::span<const PocketUIHost::Asset>{}};
    EXPECT_FALSE(system.beginReaderOverlayUiFont(renderer));
    EXPECT_EQ(loaded(), readerFace);
    EXPECT_EQ(system.loadedFallbackFont(), nullptr);
  }
  EXPECT_EQ(draw("✅"), symbol);  // failed begin must permit Symbols reload
  ASSERT_NE(system.loadedFallbackFont(), nullptr);
  EXPECT_EQ(system.loadedFallbackFont()->loadMode(), SdCardFont::LoadMode::Cached);
  uint32_t owner = system.beginReaderOverlayUiFont(renderer);
  ASSERT_NE(owner, 0u);
  system.endReaderOverlayUiFont(owner);
  EXPECT_EQ(loaded(), readerFace);
}

TEST_F(LibraryFontSystem, ReaderOverlayReadFailureIsStickyAndExitReleasesIt) {
  system.ensureAutomaticReaderFontLoaded(renderer);
  SdCardFont* readerFace = loaded();
  Bytes invalid = world;
  invalid[76] = 65;  // existing BoundedUI maximum glyph width is 64
  const PocketUIHost::Asset damagedAssets[]{{"/world.cpfont", invalid}, {"/symbols.cpfont", symbols}};
  const PocketUIHost::AssetScope damaged{damagedAssets};
  uint32_t owner = system.beginReaderOverlayUiFont(renderer);
  ASSERT_NE(owner, 0u);
  draw("한");
  ASSERT_NE(system.loadedFallbackFont(), nullptr);
  EXPECT_TRUE(system.loadedFallbackFont()->boundedReadFailed());
  EXPECT_FALSE(system.beginReaderOverlayUiFont(renderer));  // no nested retry of failed state
  EXPECT_EQ(loaded(), readerFace);
  system.endReaderOverlayUiFont(owner);
  EXPECT_EQ(system.loadedFallbackFont(), nullptr);
}

TEST_F(LibraryFontSystem, ReaderOverlayNormalFamilyTransitionDropsAllOwners) {
  system.ensureAutomaticReaderFontLoaded(renderer);
  uint32_t owner = system.beginReaderOverlayUiFont(renderer);
  ASSERT_NE(owner, 0u);
  ASSERT_EQ(system.beginReaderOverlayUiFont(renderer), owner);
  system.ensureAutomaticReaderFontLoaded(renderer);
  EXPECT_EQ(system.loadedFallbackFont(), nullptr);
  EXPECT_EQ(loaded()->loadMode(), SdCardFont::LoadMode::Cached);
  system.endReaderOverlayUiFont(owner);
  system.endReaderOverlayUiFont(owner);
  const uint32_t staleOwner = owner;
  owner = system.beginReaderOverlayUiFont(renderer);
  ASSERT_NE(owner, staleOwner);
  ASSERT_NE(owner, 0u);
  system.endReaderOverlayUiFont(staleOwner);
  ASSERT_NE(system.loadedFallbackFont(), nullptr);
  system.endReaderOverlayUiFont(owner);
  EXPECT_EQ(system.loadedFallbackFont(), nullptr);
}

TEST_F(LibraryFontSystem, ReaderOverlayMissingWorldLeavesExistingSymbolsUntouched) {
  system.ensureAutomaticReaderFontLoaded(renderer);
  worldDiscovered = false;
  system.markRegistryDirty();
  // Refresh only discovery, not reader selection or manager ownership.
  system.refreshIfDirty();
  const Bytes symbol = draw("✅");
  const SdCardFont* symbolsFace = system.loadedFallbackFont();
  ASSERT_NE(symbolsFace, nullptr);
  SdCardFont* readerFace = loaded();
  EXPECT_FALSE(system.beginReaderOverlayUiFont(renderer));
  EXPECT_EQ(system.loadedFallbackFont(), symbolsFace);
  EXPECT_EQ(loaded(), readerFace);
  EXPECT_EQ(draw("✅"), symbol);
}

TEST_F(LibraryFontSystem, PagePrewarmDoesNotRetainFallbackAcrossOverlayLifetime) {
  system.ensureAutomaticReaderFontLoaded(renderer);
  const int readerId = system.currentLoadedFontId();
  SdCardFont* readerFace = loaded();
  FontCacheManager cache(renderer.getFontMap(), renderer.getSdCardFonts());
  renderer.setFontCacheManager(&cache);
  struct Detach {
    GfxRenderer& renderer;
    ~Detach() { renderer.setFontCacheManager(nullptr); }
  } detach{renderer};
  Bytes before;
  {
    auto scope = cache.createPrewarmScope();
    draw("✅");
    scope.endScanAndPrewarm();
    ASSERT_TRUE(cache.isPageGlyphSetPinned());
    before = draw("✅");
  }
  EXPECT_FALSE(cache.isPageGlyphSetPinned());
  const uint32_t owner = system.beginReaderOverlayUiFont(renderer);
  ASSERT_NE(owner, 0u);
  cache.clearCache();  // resolves current World face; no saved Symbols pointer
  draw("한");
  system.endReaderOverlayUiFont(owner);
  EXPECT_EQ(system.loadedFallbackFont(), nullptr);
  {
    auto scope = cache.createPrewarmScope();
    draw("✅");
    scope.endScanAndPrewarm();
    EXPECT_EQ(draw("✅"), before);
  }
  EXPECT_FALSE(cache.isPageGlyphSetPinned());
  EXPECT_EQ(system.currentLoadedFontId(), readerId);
  EXPECT_EQ(loaded(), readerFace);
}

}  // namespace

// Fixed discovery fixture, not a replacement font parser. Actual font bytes
// are loaded/validated/rasterized through the production implementations.
bool SdCardFontRegistry::discover() {
  families_.clear();
  if (worldDiscovered) families_.push_back({"PocketSansWorld", {{"/world.cpfont", 12, 0}}, false});
  families_.push_back({"SavedReader", {{"/reader.cpfont", 18, 0}}, false});
  fallbackFamily_ = {"PocketSymbols", {{"/symbols.cpfont", 12, 0}}, false};
  hasFallback_ = true;
  return true;
}
const SdCardFontFamilyInfo* SdCardFontRegistry::findFamily(const std::string& name) const {
  for (const auto& family : families_)
    if (family.name == name) return &family;
  return nullptr;
}
const SdCardFontFileInfo* SdCardFontFamilyInfo::findNearestSize(uint8_t, uint8_t) const {
  return files.empty() ? nullptr : &files.front();  // each fixture has one size
}
const SdCardFontFileInfo* SdCardFontFamilyInfo::findFile(uint8_t pointSize, uint8_t style) const {
  for (const auto& file : files)
    if (file.pointSize == pointSize && file.style == style) return &file;
  return nullptr;
}
std::vector<uint8_t> SdCardFontFamilyInfo::availableSizes() const {
  return files.empty() ? std::vector<uint8_t>{} : std::vector<uint8_t>{files.front().pointSize};
}
HalMemory::HeapStats HalMemory::getPsramHeap() { return {}; }
int CrossPointSettings::getReaderFontId() const { return NOTOSERIF_18_FONT_ID; }
void CrossPointSettings::clearSdFontFamily() {
  sdFontFamilyName[0] = '\0';
  saveToFile();
}
