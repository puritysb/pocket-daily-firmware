#pragma once
#include "ContentManifest.h"

namespace PocketDaily::Content {
struct ImageInfo {
  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t rowBytes = 0;
  uint16_t rasterOffset = 0;
};
enum class ImageResult { Ok, Shape, ReadFailed, Dimensions, Padding };
// Canonical single-image P4 subset, <=512x512, <=64-byte row scratch.
// This is semantic validation, not SHA-256 verification or rendering.
ImageResult validateContentImage(const ManifestSource& source, ImageInfo& info);
struct ImageSink {
  void* context;
  void (*pixel)(void*, uint16_t x, uint16_t y, bool black);
};
// Validates before emitting pixels; nearest-neighbor fit, no upscaling.
// <=64B row scratch. Caller owns immutable source and must clear its drawing
// area on a late read failure (a sink may already have received some rows).
ImageResult rasterizeContentImage(const ManifestSource& source, uint16_t maxWidth, uint16_t maxHeight,
                                  const ImageSink& sink);
}  // namespace PocketDaily::Content
