#include "PocketReadingShelf.h"

#include <cstring>

#include "ContentChecksum.h"
#include "TextValidation.h"
namespace PocketDaily::ReadingShelf {
bool valid(const Selection& s) {
  if (s.count > CAP || (s.automatic ? s.count != 0 : s.count == 0)) return false;
  for (size_t i = 0; i < CAP; ++i) {
    const size_t n = strnlen(s.paths[i], PATH_BYTES);
    if (n == PATH_BYTES) return false;
    for (size_t j = n + 1; j < PATH_BYTES; ++j)
      if (s.paths[i][j]) return false;
    if (i >= s.count) {
      if (n) return false;
      continue;
    }
    const char* p = s.paths[i];
    if (n < 6 || p[0] != '/' || !Text::validUtf8(p, n, false) || strchr(p, '\\') || strstr(p, "//") ||
        strstr(p, "/../") || strstr(p, "/./") || strcmp(p + n - 3, "/..") == 0 || strcmp(p + n - 2, "/.") == 0)
      return false;
    for (size_t j = 0; j < i; ++j)
      if (strcmp(p, s.paths[j]) == 0) return false;
  }
  return true;
}
bool encode(const Selection& s, uint8_t (&bytes)[RECORD_BYTES]) {
  if (!valid(s)) return false;
  memset(bytes, 0, sizeof(bytes));
  memcpy(bytes, "PDSH", 4);
  bytes[4] = 1;
  bytes[5] = s.automatic ? 1 : 0;
  bytes[6] = s.count;
  memcpy(bytes + 8, s.paths, CAP * PATH_BYTES);
  const uint32_t crc = ~Content::contentCrcUpdate(UINT32_MAX, bytes, RECORD_BYTES - 4);
  for (unsigned i = 0; i < 4; ++i) bytes[RECORD_BYTES - 4 + i] = static_cast<uint8_t>(crc >> (i * 8));
  return true;
}
bool decode(const uint8_t* bytes, const size_t size, Selection& s) {
  s = {};
  if (!bytes || size != RECORD_BYTES || memcmp(bytes, "PDSH", 4) || bytes[4] != 1 || bytes[5] > 1 || bytes[7])
    return false;
  const uint32_t crc = ~Content::contentCrcUpdate(UINT32_MAX, bytes, size - 4);
  for (unsigned i = 0; i < 4; ++i)
    if (bytes[size - 4 + i] != static_cast<uint8_t>(crc >> (i * 8))) return false;
  s.automatic = bytes[5] == 1;
  s.count = bytes[6];
  memcpy(s.paths, bytes + 8, CAP * PATH_BYTES);
  if (valid(s)) return true;
  s = {};
  return false;
}
}  // namespace PocketDaily::ReadingShelf
