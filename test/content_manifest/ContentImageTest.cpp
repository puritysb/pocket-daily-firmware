#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "pocket_daily/ContentImage.h"

namespace {
using namespace PocketDaily::Content;
bool readImage(void* context, size_t offset, uint8_t* out, size_t bytes) {
  const auto& source = *static_cast<std::vector<uint8_t>*>(context);
  if (bytes > 64 || offset > source.size() || bytes > source.size() - offset) return false;
  memcpy(out, source.data() + offset, bytes);
  return true;
}
ImageResult validate(std::vector<uint8_t> bytes) {
  ImageInfo info{99, 99, 99, 99};
  const auto result = validateContentImage({&bytes, bytes.size(), readImage}, info);
  if (result != ImageResult::Ok) {
    EXPECT_EQ(info.width, 0);
    EXPECT_EQ(info.height, 0);
    EXPECT_EQ(info.rowBytes, 0);
    EXPECT_EQ(info.rasterOffset, 0);
  }
  return result;
}

TEST(ContentImage, SwiftGoldenAndRasterWhitespace) {
  std::vector<uint8_t> bytes{80, 52, 10, 57, 32, 50, 10, 0xAA, 0x80, 0x55, 0};
  ImageInfo info;
  ASSERT_EQ(validateContentImage({&bytes, bytes.size(), readImage}, info), ImageResult::Ok);
  EXPECT_EQ(info.width, 9);
  EXPECT_EQ(info.height, 2);
  EXPECT_EQ(info.rowBytes, 2);
  EXPECT_EQ(info.rasterOffset, 7);
  for (const uint8_t pixel : {10, 35, 32}) {
    EXPECT_EQ(validate({80, 52, 10, 56, 32, 49, 10, pixel}), ImageResult::Ok);
  }
}

TEST(ContentImage, HeaderBoundsCanonicalFormAndExactSize) {
  for (const auto* header : {"P1\n1 1\n", "P4\r\n1 1\n", "P4\n01 1\n", "P4\n0 1\n", "P4\n513 1\n", "P4\n1 0\n",
                             "P4\n1 513\n", "P4\n#hi\n1 1\n", "P4\n1  1\n", "P4\n999999 1\n"}) {
    std::vector<uint8_t> bytes(header, header + strlen(header));
    bytes.push_back(0);
    EXPECT_NE(validate(bytes), ImageResult::Ok) << header;
  }
  const std::vector<uint8_t> golden{80, 52, 10, 57, 32, 50, 10, 0xAA, 0x80, 0x55, 0};
  for (size_t size = 0; size < golden.size(); ++size) {
    auto truncated = golden;
    truncated.resize(size);
    EXPECT_NE(validate(truncated), ImageResult::Ok);
  }
  auto extra = golden;
  extra.push_back(0);
  EXPECT_NE(validate(extra), ImageResult::Ok);
}

TEST(ContentImage, EveryRowHasZeroUnusedBits) {
  EXPECT_EQ(validate({80, 52, 10, 57, 32, 50, 10, 0xAA, 0x81, 0x55, 0}), ImageResult::Padding);
  EXPECT_EQ(validate({80, 52, 10, 57, 32, 50, 10, 0xAA, 0x80, 0x55, 1}), ImageResult::Padding);
}

TEST(ContentImage, MaximumImageUsesBoundedReads) {
  const std::string header = "P4\n512 512\n";
  std::vector<uint8_t> bytes(header.begin(), header.end());
  bytes.resize(header.size() + 32768, 0xFF);
  EXPECT_EQ(validate(bytes), ImageResult::Ok);
}

TEST(ContentImage, ReadFailureAtEveryReadDoesNotPublishDimensions) {
  for (size_t failure = 0; failure < 11; ++failure) {
    struct Input {
      std::vector<uint8_t> data;
      size_t failure;
    } input{{80, 52, 10, 57, 32, 50, 10, 0xAA, 0x80, 0x55, 0}, failure};
    ImageInfo info{99, 99, 99, 99};
    ManifestSource source{&input, input.data.size(), [](void* context, size_t offset, uint8_t* out, size_t size) {
                            auto& input = *static_cast<Input*>(context);
                            return !(input.failure >= offset && input.failure < offset + size) &&
                                   readImage(&input.data, offset, out, size);
                          }};
    EXPECT_EQ(validateContentImage(source, info), ImageResult::ReadFailed);
    EXPECT_EQ(info.width, 0);
    EXPECT_EQ(info.height, 0);
  }
}
TEST(ContentImage, RasterizesGoldenMSBFirstWithoutUpscaling) {
  std::vector<uint8_t> bytes{80, 52, 10, 57, 32, 50, 10, 0xAA, 0x80, 0x55, 0};
  std::vector<bool> pixels;
  const ImageSink sink{&pixels, [](void* context, uint16_t x, uint16_t y, bool black) {
                         auto& pixels = *static_cast<std::vector<bool>*>(context);
                         EXPECT_EQ(pixels.size(), size_t(y) * 9 + x);
                         pixels.push_back(black);
                       }};
  ASSERT_EQ(rasterizeContentImage({&bytes, bytes.size(), readImage}, 512, 512, sink), ImageResult::Ok);
  EXPECT_EQ(pixels, (std::vector<bool>{true, false, true, false, true, false, true, false, true, false, true, false,
                                       true, false, true, false, true, false}));
}

TEST(ContentImage, FitsBoundsAndSamplesNearestSourcePixels) {
  std::vector<uint8_t> bytes{'P', '4', '\n', '4', ' ', '4', '\n', 0xA0, 0, 0x50, 0};
  std::vector<bool> pixels;
  const ImageSink sink{&pixels, [](void* context, uint16_t x, uint16_t y, bool black) {
                         EXPECT_LT(x, 2);
                         EXPECT_LT(y, 2);
                         static_cast<std::vector<bool>*>(context)->push_back(black);
                       }};
  ASSERT_EQ(rasterizeContentImage({&bytes, bytes.size(), readImage}, 3, 2, sink), ImageResult::Ok);
  EXPECT_EQ(pixels, (std::vector<bool>{true, true, false, false}));
}

TEST(ContentImage, MalformedOrEmptyBoundsEmitNoPixels) {
  std::vector<uint8_t> bytes{'P', '4', '\n', '1', ' ', '1', '\n', 0x81};
  unsigned calls = 0;
  const ImageSink sink{&calls, [](void* context, uint16_t, uint16_t, bool) { ++*static_cast<unsigned*>(context); }};
  EXPECT_EQ(rasterizeContentImage({&bytes, bytes.size(), readImage}, 1, 1, sink), ImageResult::Padding);
  bytes.back() = 0x80;
  EXPECT_EQ(rasterizeContentImage({&bytes, bytes.size(), readImage}, 0, 1, sink), ImageResult::Dimensions);
  EXPECT_EQ(rasterizeContentImage({&bytes, bytes.size(), readImage}, 1, 0, sink), ImageResult::Dimensions);
  EXPECT_EQ(calls, 0u);
}

TEST(ContentImage, LateRasterReadFailureIsReportedForCallerCleanup) {
  struct Input {
    std::vector<uint8_t> bytes{'P', '4', '\n', '8', ' ', '2', '\n', 0xFF, 0xFF};
    unsigned rowReads = 0;
  } input;
  unsigned pixels = 0;
  const ManifestSource source{&input, input.bytes.size(), [](void* context, size_t offset, uint8_t* out, size_t count) {
                                auto& input = *static_cast<Input*>(context);
                                if (offset >= 7 && ++input.rowReads == 4) return false;
                                return readImage(&input.bytes, offset, out, count);
                              }};
  const ImageSink sink{&pixels, [](void* context, uint16_t, uint16_t, bool) { ++*static_cast<unsigned*>(context); }};
  EXPECT_EQ(rasterizeContentImage(source, 8, 2, sink), ImageResult::ReadFailed);
  EXPECT_EQ(pixels, 8u);  // first output row existed: caller must clear it
}
}  // namespace
