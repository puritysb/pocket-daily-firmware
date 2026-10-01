#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
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

// Reader -> app download (docs/reader-files.md): only formats the app reads.
inline bool downloadableReaderFile(std::string_view path) {
  if (!readerFilePath(path)) return false;
  const auto dot = path.rfind('.');
  if (dot == path.npos) return false;
  const auto ext = path.substr(dot);
  return sameAscii(ext, ".epub") || sameAscii(ext, ".txt") || sameAscii(ext, ".md");
}

// Unsigned decimal without sign, spaces or overflow (offset and size queries).
inline bool parseByteCount(std::string_view text, uint64_t& out) {
  if (text.empty() || text.size() > 20) return false;
  uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    const auto digit = static_cast<uint64_t>(c - '0');
    if (value > (UINT64_MAX - digit) / 10) return false;
    value = value * 10 + digit;
  }
  out = value;
  return true;
}

// One bounded socket write per request: the long single readback stalled on the
// X3 (docs/TRANSFER_BENCHMARK.md). Direct (private AP) idles with less heap.
inline constexpr size_t kDownloadPieceSameWifi = 4096;
inline constexpr size_t kDownloadPieceDirect = 1024;
inline constexpr size_t downloadPieceLimit(const bool directSession) {
  return directSession ? kDownloadPieceDirect : kDownloadPieceSameWifi;
}

enum class PieceResult : uint8_t { Ok, SizeChanged, OutOfRange };
struct DownloadPiece {
  PieceResult result;
  size_t length;
};
inline DownloadPiece planDownloadPiece(const uint64_t fileSize, const uint64_t expectedSize, const uint64_t offset,
                                       const size_t maxPiece) {
  if (fileSize != expectedSize) return {PieceResult::SizeChanged, 0};
  if (offset >= fileSize) return {PieceResult::OutOfRange, 0};
  return {PieceResult::Ok, static_cast<size_t>(std::min<uint64_t>(maxPiece, fileSize - offset))};
}

// Exactly `length` bytes at `offset` of a freshly opened file (each request opens the
// file again). False on a failed seek or a short read. A template so the host tests can
// drive the same code with an in-memory file.
template <typename File>
bool readDownloadPiece(File& file, const uint64_t offset, uint8_t* out, const size_t length) {
  return file.seek64(offset) && file.read(out, length) == static_cast<int>(length);
}
}  // namespace PocketDaily::Web
