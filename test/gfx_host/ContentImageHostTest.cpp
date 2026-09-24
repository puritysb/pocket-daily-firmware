#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <array>
#include <climits>

#include "pocket_daily/ContentImageRenderer.h"

using namespace PocketDaily::Content;
namespace {
struct Image {
  std::array<uint8_t, 11> bytes{'P', '4', '\n', '8', ' ', '4', '\n', 0xFF, 0x81, 0x81, 0xFF};
  std::array<unsigned, 11> reads{};
  bool failLate = false;
  ManifestSource source() {
    return {this, bytes.size(), [](void* context, size_t offset, uint8_t* output, size_t count) {
              auto& image = *static_cast<Image*>(context);
              if (offset > image.bytes.size() || count > image.bytes.size() - offset) return false;
              if (offset < image.reads.size() && ++image.reads[offset] == 2 && image.failLate && offset == 8)
                return false;
              std::memcpy(output, image.bytes.data() + offset, count);
              return true;
            }};
  }
};
}  // namespace

TEST(ContentImageHost, ExactPatternMatchesIndependentBitOracleForBothPanelsAndAllRotations) {
  for (const auto [width, height] : {std::pair{800, 480}, {792, 528}}) {
    for (const auto orientation : {GfxRenderer::Portrait, GfxRenderer::LandscapeClockwise,
                                   GfxRenderer::PortraitInverted, GfxRenderer::LandscapeCounterClockwise}) {
      HalDisplay panel(width, height);
      GfxRenderer renderer(panel);
      renderer.begin();
      renderer.setOrientation(orientation);
      Image image;
      ASSERT_EQ(renderContentImage(renderer, image.source(), 10, 12, 8, 4), ImageResult::Ok);
      std::vector<uint8_t> expected(panel.getBufferSize(), 0xFF);
      for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 8; ++x) {
          if (!(image.bytes[7 + y] & (0x80 >> x))) continue;
          const int logicalX = 10 + x, logicalY = 12 + y;
          int px = 0, py = 0;
          switch (orientation) {
            case GfxRenderer::Portrait:
              px = logicalY;
              py = height - 1 - logicalX;
              break;
            case GfxRenderer::LandscapeClockwise:
              px = width - 1 - logicalX;
              py = height - 1 - logicalY;
              break;
            case GfxRenderer::PortraitInverted:
              px = width - 1 - logicalY;
              py = logicalX;
              break;
            case GfxRenderer::LandscapeCounterClockwise:
              px = logicalX;
              py = logicalY;
              break;
          }
          expected[py * (width / 8) + px / 8] &= ~(0x80 >> (px % 8));
        }
      EXPECT_EQ(std::memcmp(expected.data(), panel.getFrameBuffer(), expected.size()), 0);
      EXPECT_TRUE(panel.guardsIntact());
      EXPECT_EQ(panel.presentations, 0U);
    }
  }
}

TEST(ContentImageHost, LateReadFailureClearsPartialRasterButPreservesOutsidePixels) {
  for (const auto [width, height] : {std::pair{800, 480}, {792, 528}}) {
    for (const auto orientation : {GfxRenderer::Portrait, GfxRenderer::LandscapeClockwise,
                                   GfxRenderer::PortraitInverted, GfxRenderer::LandscapeCounterClockwise}) {
      HalDisplay panel(width, height);
      GfxRenderer renderer(panel);
      renderer.begin();
      renderer.setOrientation(orientation);
      renderer.drawPixel(100, 100);
      const std::vector<uint8_t> expected(panel.getFrameBuffer(), panel.getFrameBuffer() + panel.getBufferSize());
      Image image;
      image.failLate = true;
      EXPECT_EQ(renderContentImage(renderer, image.source(), 10, 12, 8, 4), ImageResult::ReadFailed);
      EXPECT_EQ(image.reads[7], 2U);  // first raster row was actually read after validation
      EXPECT_EQ(std::memcmp(expected.data(), panel.getFrameBuffer(), expected.size()), 0);
      EXPECT_TRUE(panel.guardsIntact());
      EXPECT_EQ(panel.presentations, 0U);
    }
  }
}

TEST(ContentImageHost, GeometryRefusalPreservesBufferAndOversizedTargetClipsSafely) {
  HalDisplay panel;
  GfxRenderer renderer(panel);
  renderer.begin();
  renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise);
  panel.clearScreen(0x55);
  Image image;
  EXPECT_EQ(renderContentImage(renderer, image.source(), INT_MAX, 0, 8, 4), ImageResult::Dimensions);
  EXPECT_TRUE(std::all_of(panel.getFrameBuffer(), panel.getFrameBuffer() + panel.getBufferSize(),
                          [](auto b) { return b == 0x55; }));
  panel.clearScreen();
  EXPECT_EQ(renderContentImage(renderer, image.source(), 799, 479, INT_MAX, INT_MAX), ImageResult::Ok);
  EXPECT_TRUE(panel.guardsIntact());
}
