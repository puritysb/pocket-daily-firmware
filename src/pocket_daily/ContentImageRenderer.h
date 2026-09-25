#pragma once
#include "ContentImage.h"

class GfxRenderer;
namespace PocketDaily::Content {
// Shared device/host image composition. No allocation or display call. Source
// ownership and immutability remain with the caller. A late failure clears the
// bounded target region, never leaving partial image pixels certified as valid.
ImageResult renderContentImage(const GfxRenderer& renderer, const ManifestSource& source, int x, int y, int width,
                               int height);
// Height renderContentImage would draw in the same box; 0 when it would not draw.
int contentImageHeight(const GfxRenderer& renderer, const ManifestSource& source, int x, int y, int width, int height);
}  // namespace PocketDaily::Content
