// Real reader/page/image/font/cache pipeline. Only the codec is deterministic:
// it refuses a cold decode while actual SD-font mini bitmaps occupy the heap.
// Pixel oracles are independently seeded .pxc bytes, not the tested decoder.
#include <Epub.h>
#include <Epub/Page.h>
#include <Epub/Section.h>
#include <Epub/converters/DirectPixelWriter.h>
#include <Epub/converters/ImageDecoderFactory.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <PngToBmpConverter.h>
#include <SdCardFont.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>
#include <vector>

#include "activities/reader/ReaderLayoutAhead.h"
#include "activities/reader/ReaderPageRenderer.h"

// Track the real ImageBlock RAM slot allocation. The codec's bounded heap model
// rejects while this exact live payload or resident SD glyph data occupies it.
// Other allocations are unchanged; no production allocator or renderer hook.
constexpr size_t TEST_PXC_BYTES = 40 * 32 / 4;
std::atomic<unsigned> livePixelSlots{0};
struct alignas(std::max_align_t) ArrayAllocation {
  size_t size;
};
void* operator new[](size_t size) {
  auto* header = static_cast<ArrayAllocation*>(std::malloc(sizeof(ArrayAllocation) + (size ? size : 1)));
  if (!header) throw std::bad_alloc();
  header->size = size;
  if (size == TEST_PXC_BYTES) ++livePixelSlots;
  return header + 1;
}
void operator delete[](void* pointer) noexcept {
  if (!pointer) return;
  auto* header = static_cast<ArrayAllocation*>(pointer) - 1;
  if (header->size == TEST_PXC_BYTES) --livePixelSlots;
  std::free(header);
}
void operator delete[](void* pointer, size_t) noexcept { operator delete[](pointer); }

namespace {
constexpr int FONT_ID = 7;
constexpr int IMAGE_W = 40, IMAGE_H = 32;
SdCardFont* decodingFont = nullptr;
unsigned decodeCalls = 0, rejectedForGlyphs = 0, rejectedForPixelCache = 0;
bool failCodec = false, skipCache = false;

uint8_t imagePixel(int x, int y) { return static_cast<uint8_t>((x / 3 + y / 5 + x * y) % 4); }
void writeCache(const std::string& path, int width = IMAGE_W, int height = IMAGE_H) {
  std::ofstream out(TestFs::map(path.c_str()), std::ios::binary);
  const uint16_t w = width, h = height;
  out.write(reinterpret_cast<const char*>(&w), 2);
  out.write(reinterpret_cast<const char*>(&h), 2);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; x += 4) {
      uint8_t b = 0;
      for (int i = 0; i < 4 && x + i < width; ++i) b |= imagePixel(x + i, y) << (6 - i * 2);
      out.put(static_cast<char>(b));
    }
  }
}

class MemoryLimitedDecoder final : public ImageToFramebufferDecoder {
 public:
  bool decodeToFramebuffer(const std::string&, GfxRenderer& renderer, const RenderConfig& config) override {
    ++decodeCalls;
    if (decodingFont && decodingFont->getEpdFont()->data->bitmap) {
      ++rejectedForGlyphs;
      return false;
    }
    if (livePixelSlots.load() != 0) {
      ++rejectedForPixelCache;
      return false;
    }
    if (failCodec) return false;
    DirectPixelWriter pixels;
    pixels.init(renderer);
    for (int y = 0; y < config.maxHeight; ++y) {
      pixels.beginRow(config.y + y);
      for (int x = 0; x < config.maxWidth; ++x) pixels.writePixel(config.x + x, imagePixel(x, y));
    }
    if (!skipCache) writeCache(config.cachePath, config.maxWidth, config.maxHeight);
    return true;
  }
  bool getDimensions(const std::string&, ImageDimensions& size) const override {
    size = {IMAGE_W, IMAGE_H};
    return true;
  }
  const char* getFormatName() const override { return "memory-limited test image"; }
};
MemoryLimitedDecoder decoder;

void put32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}
// Real CPFONT v4, two-bit nonuniform glyphs, nonzero AV kerning and fi ligature.
std::vector<uint8_t> fontBytes() {
  constexpr size_t count = 95, glyphs = 76, left = glyphs + count * 16;
  constexpr size_t right = left + 6, matrix = right + 6, ligatures = matrix + 4, bitmap = ligatures + 8;
  std::vector<uint8_t> bytes(bitmap, 0);
  memcpy(bytes.data(), "CPFONT", 6);
  bytes[8] = 4;
  bytes[10] = bytes[12] = 1;
  put32(bytes, 36, 1);
  put32(bytes, 40, count);
  bytes[44] = 14;
  bytes[45] = 12;
  bytes[49] = bytes[51] = bytes[53] = bytes[54] = 2;
  bytes[55] = 1;
  put32(bytes, 56, 64);
  put32(bytes, 64, 32);
  put32(bytes, 68, 126);
  for (size_t i = 0; i < count; ++i) {
    EpdGlyph glyph{};
    glyph.width = 6 + i % 3;
    glyph.height = 7 + i % 4;
    glyph.top = glyph.height;
    glyph.advanceX = (glyph.width + 1) * 16;
    glyph.dataLength = (glyph.width * glyph.height * 2 + 7) / 8;
    glyph.dataOffset = bytes.size() - bitmap;
    if (i == 0) glyph.width = glyph.height = glyph.dataLength = 0;
    memcpy(bytes.data() + glyphs + i * 16, &glyph, sizeof(glyph));
    for (unsigned n = 0; n < glyph.dataLength; ++n) bytes.push_back(static_cast<uint8_t>(i * 37 + n * 53));
  }
  bytes[left] = bytes[right] = 'A';
  bytes[left + 2] = bytes[right + 2] = 1;
  bytes[left + 3] = bytes[right + 3] = 'V';
  bytes[left + 5] = bytes[right + 5] = 2;
  bytes[matrix + 1] = static_cast<uint8_t>(-24);
  bytes[matrix + 2] = static_cast<uint8_t>(-16);
  put32(bytes, ligatures, ('f' << 16) | 'i');
  put32(bytes, ligatures + 4, 'F');
  return bytes;
}

class ReaderImageMemory : public testing::Test {
 protected:
  void SetUp() override {
    char dir[] = "/tmp/reader-image-XXXXXX";
    ASSERT_NE(mkdtemp(dir), nullptr);
    TestFs::root = dir;
    std::filesystem::create_directory(TestFs::root + "/.crosspoint");
    auto bytes = fontBytes();
    std::ofstream out(TestFs::root + "/test.cpfont", std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    out.close();
    ASSERT_TRUE(font.load("/test.cpfont"));
    renderer.begin();
    renderer.setOrientation(GfxRenderer::LandscapeClockwise);
    renderer.setFontCacheManager(&cache);
    renderer.insertFont(FONT_ID, EpdFontFamily(font.getEpdFont()));
    renderer.registerSdCardFont(FONT_ID, &font);
    decodingFont = &font;
    decodeCalls = rejectedForGlyphs = rejectedForPixelCache = 0;
    failCodec = skipCache = false;
    ImageBlock::clearRenderFailures();
    ImageBlock::releaseRenderCache();
    std::ofstream(TestFs::root + "/one.png").put('1');
    std::ofstream(TestFs::root + "/two.png").put('2');
    std::ofstream(TestFs::root + "/three.png").put('3');
  }
  void TearDown() override {
    ImageBlock::releaseRenderCache();
    ImageBlock::clearRenderFailures();
    decodingFont = nullptr;
    std::filesystem::remove_all(TestFs::root);
  }
  void addText(Page& page, int y) {
    auto block = std::make_unique<TextBlock>(
        std::vector<std::string>{"AVATAR", "office", "VAV"}, std::vector<int16_t>{0, 80, 150},
        std::vector<EpdFontFamily::Style>(3, EpdFontFamily::REGULAR), std::vector<uint8_t>{}, std::vector<uint16_t>{});
    page.elements.push_back(std::make_unique<PageLine>(std::move(block), 10, y));
  }
  Page makePage(bool images = true, bool multiple = false) {
    Page page;
    page.elements.reserve(6);
    addText(page, 10);
    if (images)
      page.elements.push_back(
          std::make_unique<PageImage>(std::make_unique<ImageBlock>("/one.png", "", IMAGE_W, IMAGE_H), 30, 45));
    addText(page, 95);
    if (multiple) {
      page.elements.push_back(
          std::make_unique<PageImage>(std::make_unique<ImageBlock>("/two.png", "", IMAGE_W, IMAGE_H), 120, 135));
      page.elements.push_back(
          std::make_unique<PageImage>(std::make_unique<ImageBlock>("/three.png", "", IMAGE_W, IMAGE_H), 220, 180));
    }
    return page;
  }
  using Pixels = std::array<std::vector<uint8_t>, 3>;
  Pixels render(const Page& page, bool aa = true, bool queued = false) {
    ReaderPageRenderer::Options options;
    options.fontId = FONT_ID;
    options.textAntiAliasing = aa;
    options.marginLeft = options.marginTop = 4;
    options.nextTurnQueued = [](void* context) { return *static_cast<bool*>(context); };
    options.context = &queued;
    renderer.clearScreen();
    std::fill(panel.lsb_.begin(), panel.lsb_.end(), 0);
    std::fill(panel.msb_.begin(), panel.msb_.end(), 0);
    int untilFull = 3;
    ReaderPageRenderer::render(renderer, page, options, untilFull);
    return {panel.fb_, panel.lsb_, panel.msb_};
  }
  void removeImageCaches() {
    ImageBlock::releaseRenderCache();
    for (const auto* name : {"one", "two", "three"}) std::filesystem::remove(TestFs::root + "/" + name + ".pxc");
  }
  HalDisplay panel;
  GfxRenderer renderer{panel};
  FontCacheManager cache{renderer.getFontMap(), renderer.getSdCardFonts()};
  SdCardFont font;
};

TEST_F(ReaderImageMemory, ColdImageMatchesCachedPixelsWithTextAndAllGrayscaleOrientations) {
  auto page = makePage();
  for (const auto orientation : {GfxRenderer::Portrait, GfxRenderer::LandscapeClockwise, GfxRenderer::PortraitInverted,
                                 GfxRenderer::LandscapeCounterClockwise}) {
    renderer.setOrientation(orientation);
    for (bool inverted : {false, true}) {
      panel.setInverted(inverted);
      for (bool aa : {false, true}) {
        removeImageCaches();
        writeCache("/one.pxc");
        const auto expected = render(page, aa);
        removeImageCaches();
        // Prior UI/text residue must also be shed before the first decoder.
        ASSERT_EQ(font.prewarm("AVATAR office VAV", 1), 1);  // Fixture omits U+FFFD.
        ASSERT_NE(font.getEpdFont()->data->bitmap, nullptr);
        const auto before = decodeCalls;
        EXPECT_EQ(render(page, aa), expected);
        EXPECT_EQ(decodeCalls - before, 1U);
        EXPECT_EQ(rejectedForGlyphs, 0U);
      }
    }
  }
}

TEST_F(ReaderImageMemory, MixedCachedAndTwoColdImagesMatchWarmReference) {
  auto page = makePage(true, true);
  for (const auto* name : {"one", "two", "three"}) writeCache(std::string("/") + name + ".pxc");
  const auto expected = render(page);
  // Keep both the first image's disk cache and the previous render's RAM slot.
  ASSERT_EQ(livePixelSlots.load(), 1U);
  std::filesystem::remove(TestFs::root + "/two.pxc");
  std::filesystem::remove(TestFs::root + "/three.pxc");
  EXPECT_EQ(render(page), expected);
  EXPECT_EQ(decodeCalls, 2U);
  EXPECT_EQ(rejectedForGlyphs, 0U);
  EXPECT_EQ(rejectedForPixelCache, 0U);
  EXPECT_EQ(render(page), expected);
  EXPECT_EQ(decodeCalls, 2U);
}

TEST_F(ReaderImageMemory, CodecFailureIsAttemptedOnceAcrossBwAndGrayAndRerender) {
  auto page = makePage();
  failCodec = true;
  const auto pixels = render(page);
  EXPECT_EQ(decodeCalls, 1U);
  EXPECT_EQ(rejectedForGlyphs, 0U);
  EXPECT_FALSE(std::filesystem::exists(TestFs::root + "/one.pxc"));
  EXPECT_EQ(render(page), pixels);
  EXPECT_EQ(decodeCalls, 1U);
}

TEST_F(ReaderImageMemory, MissingCacheAfterSuccessfulDecodeRemainsBoundedAndDoesNotClaimCachedSuccess) {
  auto page = makePage();
  skipCache = true;
  render(page);
  // Successful prepass without a persisted cache still follows the existing BW
  // failure/suppression path. It must not loop or report the image cached.
  EXPECT_EQ(decodeCalls, 2U);
  EXPECT_EQ(rejectedForGlyphs, 1U);
  EXPECT_FALSE(std::filesystem::exists(TestFs::root + "/one.pxc"));
  render(page);
  EXPECT_EQ(decodeCalls, 2U);
}

TEST_F(ReaderImageMemory, WarmAndTextOnlyPathsAndQueuedTurnRetainNormalPixels) {
  auto text = makePage(false);
  const auto expected = render(text);
  EXPECT_EQ(render(text), expected);
  EXPECT_EQ(decodeCalls, 0U);
  auto page = makePage();
  writeCache("/one.pxc");
  const auto queued = render(page, true, true);
  removeImageCaches();
  EXPECT_EQ(render(page, true, true), queued);
  EXPECT_EQ(decodeCalls, 1U);
  EXPECT_EQ(rejectedForGlyphs, 0U);
}

TEST_F(ReaderImageMemory, ColdImagePreservesAdvancesAndIncrementalSectionBytes) {
  std::filesystem::copy_file(LONG_PARAGRAPH_EPUB, TestFs::root + "/book.epub");
  auto epub = std::make_shared<Epub>("/book.epub", "/.crosspoint");
  ASSERT_TRUE(epub->load(true, false));
  SectionLayout layout;
  layout.fontId = FONT_ID;
  layout.lineCompression = 1;
  layout.viewportWidth = 180;
  layout.viewportHeight = 60;
  layout.embeddedStyle = true;
  Section reference(epub, 2, renderer);
  ASSERT_TRUE(layout.startBuild(reference));
  ASSERT_TRUE(reference.buildSomeMore(0));
  const auto bytesFor = [](Section& section) {
    std::vector<std::vector<uint8_t>> all;
    for (unsigned i = 0; i < section.pageCount; ++i) {
      auto p = section.loadPage(i);
      if (!p) return std::vector<std::vector<uint8_t>>{};
      HalFile out;
      if (!Storage.openFileForWrite("TEST", "/page.bin", out) || !p->serialize(out))
        return std::vector<std::vector<uint8_t>>{};
      out.close();
      std::ifstream in(TestFs::root + "/page.bin", std::ios::binary);
      all.emplace_back(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    return all;
  };
  const auto expected = bytesFor(reference);
  ASSERT_GT(expected.size(), 1U);
  reference.clearCache();
  Section partial(epub, 2, renderer);
  ASSERT_TRUE(layout.startBuild(partial));
  ASSERT_TRUE(partial.buildSomeMore(1));
  ASSERT_TRUE(partial.isBuilding());
  ASSERT_EQ(font.buildAdvanceTable("AVATAR office VAV", 1), 0);
  const auto a = font.getAdvance('A', 0);
  ASSERT_GT(a, 0);
  render(makePage());
  EXPECT_TRUE(font.hasAdvanceTable());
  EXPECT_EQ(font.getAdvance('A', 0), a);
  ASSERT_TRUE(partial.buildSomeMore(0));
  EXPECT_EQ(bytesFor(partial), expected);
}
}  // namespace

// No production factory/PNG/JPEG codec is linked into this deterministic target.
class JpegToFramebufferConverter {};
class PngToFramebufferConverter {};
std::unique_ptr<JpegToFramebufferConverter> ImageDecoderFactory::jpegDecoder;
std::unique_ptr<PngToFramebufferConverter> ImageDecoderFactory::pngDecoder;
ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string&) { return &decoder; }
bool ImageDecoderFactory::isFormatSupported(const std::string&) { return true; }
bool PngToBmpConverter::pngFileToBmpStream(HalFile&, Print&, bool, bool) { return false; }
bool PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(HalFile&, Print&, int, int) { return false; }
bool JpegToBmpConverter::jpegFileToBmpStream(HalFile&, Print&, bool, bool) { return false; }
bool JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(HalFile&, Print&, int, int) { return false; }
