#pragma once

class GfxRenderer;

namespace PocketDaily::Content {
struct ContentCard;
struct ManifestSource;

// One card page through an already-loaded bounded SD font; never invokes font
// discovery or switches back to the cached EPUB font path while a live
// session is open. False when the font is gone or a draw failed.
bool drawContentPage(GfxRenderer& renderer, const ContentCard* card, const char* revision, int fontId,
                     const char* const labels[4]);
// One manifest image into the rectangle; logs and returns false on failure.
bool drawContentImage(const GfxRenderer& renderer, const ManifestSource& source, int x, int y, int width, int height);
}  // namespace PocketDaily::Content
