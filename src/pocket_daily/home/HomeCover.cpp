#include "HomeCover.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <RecentBooksStore.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "components/UITheme.h"
#include "pocket_daily/ContentChecksum.h"
#include "pocket_daily/home/HomeInputs.h"
namespace PocketDaily::Home {
bool openBookCoverPath(char* path, size_t capacity) {
  OpenBook book{};
  const auto* recent = readOpenBook(book);
  if (!recent || recent->coverBmpPath.empty()) return false;
  std::string full = recent->coverBmpPath;
  const auto thumb = full.rfind("/thumb_");
  if (thumb != std::string::npos) full.replace(thumb, std::string::npos, "/cover.bmp");
  if (!Storage.exists(full.c_str()))
    full = UITheme::getCoverThumbPath(recent->coverBmpPath, UITheme::getInstance().getMetrics().homeCoverHeight);
  const int count = snprintf(path, capacity, "%s", full.c_str());
  if (count > 0 && static_cast<size_t>(count) < capacity) return true;
  if (capacity) path[0] = 0;
  return false;
}
uint32_t coverPathKey(const char* path) {
  return ~Content::contentCrcUpdate(UINT32_MAX, reinterpret_cast<const uint8_t*>(path), strlen(path));
}
bool coverMetadataForPath(const char* path, uint32_t& key, uint32_t& size, uint32_t& crc) {
  // Cold read-only query. Stream64B; no image-sized allocation or persistent scene.
  HalFile file = Storage.open(path);
  if (!file || !file.size() || file.size() > 131072) return false;
  const uint32_t length = file.size();
  uint32_t sum = UINT32_MAX;
  uint8_t bytes[64];
  for (uint32_t at = 0; at < length;) {
    const size_t n = std::min<size_t>(sizeof(bytes), length - at);
    if (file.read(bytes, n) != static_cast<int>(n)) return false;
    sum = Content::contentCrcUpdate(sum, bytes, n);
    at += n;
  }
  key = coverPathKey(path);
  size = length;
  crc = ~sum;
  return key != 0;
}
bool recentBookCoverPath(unsigned index, char* path, size_t capacity) {
  if (capacity) path[0] = 0;
  const RecentBook* entries[RECENT_CAP]{};
  if (index >= readRecentBookEntries(entries)) return false;
  return resolveBookCoverPath(*entries[index], path, capacity);
}
bool coverMetadata(uint32_t& key, uint32_t& size, uint32_t& crc) {
  char path[160];
  return openBookCoverPath(path, sizeof(path)) && coverMetadataForPath(path, key, size, crc);
}
bool recentCoverMetadata(unsigned index, uint32_t& key, uint32_t& size, uint32_t& crc) {
  char path[RECENT_PATH_BYTES];
  return recentBookCoverPath(index, path, sizeof(path)) && coverMetadataForPath(path, key, size, crc);
}
bool drawCachedCover(GfxRenderer& r, const char* path, int x, int y, int width, int height) {
  if (!path || !path[0] || width <= 4 || height <= 4) return false;
  HalFile file = Storage.open(path, O_RDONLY);
  if (!file) return false;
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok || bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0 ||
      bitmap.getWidth() > 2048 || bitmap.getHeight() > 2048)
    return false;
  const float scale = std::min(float(width - 4) / bitmap.getWidth(), float(height - 4) / bitmap.getHeight());
  const int dw = std::max(1, int(bitmap.getWidth() * scale)), dh = std::max(1, int(bitmap.getHeight() * scale));
  r.drawBitmap(bitmap, x + (width - dw) / 2, y + (height - dh) / 2, dw, dh, 0, 0, true);
  return true;
}
}  // namespace PocketDaily::Home
