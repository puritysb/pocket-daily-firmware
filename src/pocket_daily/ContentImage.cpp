#include "ContentImage.h"

namespace PocketDaily::Content {
namespace {
ImageResult dimension(const ManifestSource& source, size_t& offset, uint8_t delimiter, uint16_t& value) {
  value = 0;
  for (unsigned digit = 0; digit < 4; ++digit) {
    uint8_t byte = 0;
    if (offset >= source.size) return ImageResult::Shape;
    if (!source.read(source.context, offset++, &byte, 1)) return ImageResult::ReadFailed;
    if (byte == delimiter && digit) return ImageResult::Ok;
    if (digit == 3 || byte < '0' || byte > '9' || (!digit && byte == '0')) return ImageResult::Dimensions;
    value = value * 10 + byte - '0';
    if (value > 512) return ImageResult::Dimensions;
  }
  return ImageResult::Dimensions;
}
}  // namespace

ImageResult validateContentImage(const ManifestSource& source, ImageInfo& info) {
  info = {};
  if (!source.read || source.size < 8 || source.size > 32779) return ImageResult::Shape;
  uint8_t magic[3];
  if (!source.read(source.context, 0, magic, sizeof(magic))) return ImageResult::ReadFailed;
  if (magic[0] != 'P' || magic[1] != '4' || magic[2] != '\n') return ImageResult::Shape;
  size_t offset = 3;
  uint16_t width = 0;
  uint16_t height = 0;
  auto result = dimension(source, offset, ' ', width);
  if (result != ImageResult::Ok) return result;
  result = dimension(source, offset, '\n', height);
  if (result != ImageResult::Ok) return result;
  const uint16_t rowBytes = (width + 7) / 8;
  if (source.size != offset + size_t(rowBytes) * height) return ImageResult::Shape;
  uint8_t row[64];
  const uint8_t unusedMask = width % 8 ? static_cast<uint8_t>((1u << (8 - width % 8)) - 1) : 0;
  for (uint16_t y = 0; y < height; ++y) {
    if (!source.read(source.context, offset + size_t(y) * rowBytes, row, rowBytes)) return ImageResult::ReadFailed;
    if (row[rowBytes - 1] & unusedMask) return ImageResult::Padding;
  }
  info = {width, height, rowBytes, static_cast<uint16_t>(offset)};
  return ImageResult::Ok;
}
void fitContentImage(const ImageInfo& info, uint16_t maxWidth, uint16_t maxHeight, uint16_t& width, uint16_t& height) {
  width = info.width < maxWidth ? info.width : maxWidth;
  height = info.width ? static_cast<uint32_t>(width) * info.height / info.width : 0;
  if (!height) height = 1;
  if (height > maxHeight) {
    height = maxHeight;
    width = info.height ? static_cast<uint32_t>(height) * info.width / info.height : 0;
    if (!width) width = 1;
  }
}

ImageResult rasterizeContentImage(const ManifestSource& source, uint16_t maxWidth, uint16_t maxHeight,
                                  const ImageSink& sink) {
  if (!sink.pixel || !maxWidth || !maxHeight) return ImageResult::Dimensions;
  ImageInfo info;
  const auto result = validateContentImage(source, info);
  if (result != ImageResult::Ok) return result;
  uint16_t width = 0, height = 0;
  fitContentImage(info, maxWidth, maxHeight, width, height);
  uint8_t row[64];
  for (uint16_t y = 0; y < height; ++y) {
    const uint16_t sourceY = static_cast<uint32_t>(y) * info.height / height;
    if (!source.read(source.context, info.rasterOffset + size_t(sourceY) * info.rowBytes, row, info.rowBytes))
      return ImageResult::ReadFailed;
    for (uint16_t x = 0; x < width; ++x) {
      const uint16_t sourceX = static_cast<uint32_t>(x) * info.width / width;
      sink.pixel(sink.context, x, y, (row[sourceX / 8] & (0x80u >> (sourceX % 8))) != 0);
    }
  }
  return ImageResult::Ok;
}
}  // namespace PocketDaily::Content
