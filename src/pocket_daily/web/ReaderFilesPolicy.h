#pragma once
#include <string_view>

namespace PocketDaily::Web {
inline bool sameAscii(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    const char c = a[i] >= 'A' && a[i] <= 'Z' ? a[i] + ('a' - 'A') : a[i];
    if (c != b[i]) return false;
  }
  return true;
}
inline bool readerFilePath(std::string_view path) {
  if (path.empty() || path.size() > 192 || path.front() != '/') return false;
  if (path == "/") return true;
  size_t start = 1;
  while (start < path.size()) {
    const auto end = path.find('/', start);
    const auto part = path.substr(start, end == path.npos ? path.size() - start : end - start);
    if (part.empty() || part.front() == '.' || part.back() == '.' || part.back() == ' ' ||
        sameAscii(part, "crash_report.txt") || sameAscii(part, "pocket-daily") ||
        sameAscii(part, "system volume information"))
      return false;
    for (const unsigned char c : part)
      if (c < 32 || c == 127 || c == '\\' || c == ':' || c == '%') return false;
    if (end == path.npos) return true;
    start = end + 1;
  }
  return false;
}
inline bool deletableReaderFile(std::string_view path) {
  if (!readerFilePath(path)) return false;
  const auto dot = path.rfind('.');
  if (dot == path.npos) return false;
  const auto ext = path.substr(dot);
  return sameAscii(ext, ".epub") || sameAscii(ext, ".txt") || sameAscii(ext, ".md") || sameAscii(ext, ".xtc");
}
}  // namespace PocketDaily::Web
