#include "ContentImageRenderer.h"

#include <GfxRenderer.h>

#include <algorithm>

namespace PocketDaily::Content {
ImageResult renderContentImage(const GfxRenderer& renderer, const ManifestSource& source, int x, int y, int width,
                               int height) {
  if (x < 0 || y < 0 || x >= renderer.getScreenWidth() || y >= renderer.getScreenHeight() || width <= 0 || height <= 0)
    return ImageResult::Dimensions;
  width = std::min({width, renderer.getScreenWidth() - x, 512});
  height = std::min({height, renderer.getScreenHeight() - y, 512});
  struct Target {
    const GfxRenderer& renderer;
    int x, y;
  } target{renderer, x, y};
  const ImageSink sink{&target, [](void* context, uint16_t px, uint16_t py, bool black) {
                         const auto& t = *static_cast<Target*>(context);
                         t.renderer.drawPixel(t.x + px, t.y + py, black);
                       }};
  const auto result = rasterizeContentImage(source, width, height, sink);
  if (result != ImageResult::Ok) renderer.fillRect(x, y, width, height, false);
  return result;
}
}  // namespace PocketDaily::Content
