#pragma once

class GfxRenderer;
namespace PocketDaily::Content {
struct ContentCard;
struct ContentPageOptions {
  int fontId, sidePadding, topPadding, spacing;
  const char* emptyTitle;
  const char* emptyMessage;
  const char* labels[4];
};
struct ContentPageImagePainter {
  void* context = nullptr;
  bool (*draw)(void*, const char* name, int x, int y, int width, int height) = nullptr;
};
// Shared production page drawing. Caller supplies preflighted font/UTF-8 and
// immutable image assets, owns renderer exclusion, checks font I/O after drawing
// and displays/captures only on success. No display call, global theme or storage.
bool renderContentPage(const GfxRenderer& renderer, const ContentCard* card, const ContentPageOptions& options,
                       const ContentPageImagePainter& images = {});
}  // namespace PocketDaily::Content
