#include <EpdFontData.h>
#include <PocketUIHost.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <future>
#include <memory>
#include <vector>

namespace {
std::vector<uint8_t> asciiFont() {
  constexpr unsigned count = 95, glyphStart = 76, bitmapStart = glyphStart + count * 16;
  std::vector<uint8_t> bytes(bitmapStart + count, 0);
  const auto put = [&](size_t offset, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = value >> (i * 8);
  };
  std::memcpy(bytes.data(), "CPFONT", 6);
  bytes[8] = 4;
  bytes[12] = 1;
  put(36, 1);
  put(40, count);
  bytes[44] = 12;
  bytes[45] = 10;
  put(56, 64);
  put(64, 32);
  put(68, 126);
  for (unsigned i = 0; i < count; ++i) {
    EpdGlyph glyph{};
    glyph.width = 2;
    glyph.height = 2;
    glyph.advanceX = 48;
    glyph.dataLength = 1;
    glyph.dataOffset = i;
    std::memcpy(bytes.data() + glyphStart + i * 16, &glyph, sizeof(glyph));
    bytes[bitmapStart + i] = 0xC0;
  }
  return bytes;
}
class HostAbiValid : public testing::Test {
 protected:
  std::unique_ptr<pdui_context, decltype(&pdui_destroy)> context{nullptr, pdui_destroy};
  pdui_content_options options{12, 8, 4, "Pocket", "Empty", {"Back", "", "Prev", "Next"}};
  void SetUp() override {
    auto bytes = asciiFont();
    pdui_context* raw = nullptr;
    ASSERT_EQ(pdui_create(800, 480, 0, bytes.data(), bytes.size(), &raw), PDUI_OK);
    context.reset(raw);
    // The input goes away here: all later font reads must use the owned copy.
  }
  int32_t render() { return pdui_render_content(context.get(), nullptr, 0, nullptr, 0, &options); }
};
}  // namespace

TEST_F(HostAbiValid, OwnsFontAndExportsOnlyCompletedFrames) {
  pdui_frame_info info{};
  EXPECT_EQ(pdui_get_frame_info(context.get(), &info), PDUI_NO_FRAME);
  ASSERT_EQ(render(), PDUI_OK);
  ASSERT_EQ(pdui_get_frame_info(context.get(), &info), PDUI_OK);
  EXPECT_EQ(info.byte_count, 48000U);
  EXPECT_EQ(info.logical_width, 480U);
  EXPECT_EQ(info.logical_height, 800U);
  std::vector<uint8_t> pixels(48001, 0xA5);
  EXPECT_EQ(pdui_copy_frame(context.get(), pixels.data(), 47999), PDUI_BUFFER_TOO_SMALL);
  EXPECT_TRUE(std::all_of(pixels.begin(), pixels.end(), [](auto b) { return b == 0xA5; }));
  EXPECT_EQ(pdui_copy_frame(context.get(), pixels.data(), pixels.size()), PDUI_OK);
  EXPECT_EQ(pixels.back(), 0xA5);
  EXPECT_TRUE(std::any_of(pixels.begin(), pixels.end() - 1, [](auto b) { return b != 0xFF; }));
  EXPECT_EQ(pdui_render_content(context.get(), nullptr, 0, nullptr, 0, nullptr), PDUI_INVALID_ARGUMENT);
  EXPECT_EQ(pdui_get_frame_info(context.get(), &info), PDUI_NO_FRAME);
  EXPECT_EQ(info.byte_count, 0U);
  std::fill(pixels.begin(), pixels.end(), 0xA5);
  EXPECT_EQ(pdui_copy_frame(context.get(), pixels.data(), pixels.size()), PDUI_NO_FRAME);
  EXPECT_TRUE(std::all_of(pixels.begin(), pixels.end(), [](auto b) { return b == 0xA5; }));
  EXPECT_EQ(render(), PDUI_OK);
}
TEST_F(HostAbiValid, RejectsMalformedChromeBeforeFontOrDrawing) {
  for (const char* invalid : {"\xC0\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xC2", "\x80", "\n", "\xC2\x85"}) {
    std::strcpy(options.labels[0], invalid);
    EXPECT_EQ(render(), PDUI_INVALID_ARGUMENT);
  }
  std::memset(options.labels[0], 'A', sizeof(options.labels[0]));
  EXPECT_EQ(render(), PDUI_INVALID_ARGUMENT);
  std::memset(options.labels[0], 0, sizeof(options.labels[0]));
  std::strcpy(options.labels[0], "Back");
  EXPECT_EQ(render(), PDUI_OK);
}
TEST_F(HostAbiValid, InvalidDocumentsAndGeometryCannotExposeFrames) {
  std::array<uint8_t, 512> invalid{};
  EXPECT_EQ(pdui_render_content(context.get(), invalid.data(), invalid.size(), nullptr, 0, &options),
            PDUI_INVALID_CARD);
  EXPECT_EQ(pdui_render_content(context.get(), nullptr, 512, nullptr, 0, &options), PDUI_INVALID_ARGUMENT);
  EXPECT_EQ(pdui_render_content(context.get(), nullptr, 0, invalid.data(), 1, &options), PDUI_INVALID_IMAGE);
  options.side_padding = INT_MAX;
  EXPECT_EQ(render(), PDUI_RENDER_FAILED);
  pdui_frame_info info{};
  EXPECT_EQ(pdui_get_frame_info(context.get(), &info), PDUI_NO_FRAME);
  options.side_padding = 12;
  EXPECT_EQ(render(), PDUI_OK);
}
TEST_F(HostAbiValid, IndependentContextsCanRenderOnDifferentThreads) {
  auto bytes = asciiFont();
  pdui_context* raw = nullptr;
  ASSERT_EQ(pdui_create(792, 528, 3, bytes.data(), bytes.size(), &raw), PDUI_OK);
  const std::unique_ptr<pdui_context, decltype(&pdui_destroy)> other(raw, pdui_destroy);
  auto future = std::async(std::launch::async, [&] {
    pdui_frame_info info{};
    return pdui_render_content(other.get(), nullptr, 0, nullptr, 0, &options) == PDUI_OK &&
           pdui_get_frame_info(other.get(), &info) == PDUI_OK && info.byte_count == 52272 && info.logical_width == 792;
  });
  EXPECT_EQ(render(), PDUI_OK);
  EXPECT_TRUE(future.get());
}

TEST(HostAbi, InvalidCreationClearsOutputAndDoesNotThrow) {
  const uint8_t invalidFont[]{0};
  pdui_context* context = nullptr;
  EXPECT_EQ(pdui_create(800, 480, 0, invalidFont, 1, &context), PDUI_INVALID_FONT);
  EXPECT_EQ(context, nullptr);
  EXPECT_EQ(pdui_create(UINT32_MAX, 480, 0, invalidFont, 1, &context), PDUI_INVALID_ARGUMENT);
  EXPECT_EQ(pdui_create(800, 480, 4, invalidFont, 1, &context), PDUI_INVALID_ARGUMENT);
  EXPECT_EQ(pdui_create(799, 480, 0, invalidFont, 1, &context), PDUI_INVALID_ARGUMENT);
  EXPECT_EQ(pdui_create(800, 0, 0, invalidFont, 1, &context), PDUI_INVALID_ARGUMENT);
  EXPECT_EQ(pdui_create(800, 480, 0, invalidFont, SIZE_MAX, &context), PDUI_INVALID_ARGUMENT);
  EXPECT_EQ(pdui_create(800, 480, 0, invalidFont, 1, nullptr), PDUI_INVALID_ARGUMENT);
  pdui_destroy(nullptr);
}
TEST(HostAbi, NullContextCannotExposePixels) {
  pdui_frame_info info{1, 2, 3, 4, 5, 6, 7};
  EXPECT_EQ(pdui_get_frame_info(nullptr, &info), PDUI_INVALID_ARGUMENT);
  EXPECT_EQ(info.byte_count, 0U);
  uint8_t output = 0xA5;
  EXPECT_EQ(pdui_copy_frame(nullptr, &output, 1), PDUI_INVALID_ARGUMENT);
  EXPECT_EQ(output, 0xA5);
  EXPECT_EQ(pdui_render_content(nullptr, nullptr, 0, nullptr, 0, nullptr), PDUI_INVALID_ARGUMENT);
}
