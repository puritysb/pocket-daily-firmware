#include "HomeCover.h"

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
bool coverMetadata(uint32_t& key, uint32_t& size, uint32_t& crc) {
  // Cold read-only query. Stream64B; no image-sized allocation or persistent scene.
  char path[160];
  if (!openBookCoverPath(path, sizeof(path))) return false;
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
}  // namespace PocketDaily::Home
