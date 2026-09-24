#include "ContentPathPolicy.h"

#include "ContentManifest.h"

namespace PocketDaily::Content {
namespace {
char lower(char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
bool prefix(std::string_view path, std::string_view root) {
  if (path.size() < root.size()) return false;
  for (size_t i = 0; i < root.size(); ++i) {
    if (lower(path[i]) != lower(root[i])) return false;
  }
  return path.size() == root.size() || path[root.size()] == '/';
}
}  // namespace

bool isContentStagingPath(std::string_view path) { return prefix(path, "/pocket-daily/content-staging"); }

bool contentStagingRangeAllowed(uint64_t offset, uint64_t bytes) {
  return offset <= MAX_CONTENT_BYTES && bytes <= MAX_CONTENT_BYTES - offset;
}

bool contentWriteWithinBudget(std::string_view path, uint64_t offset, uint64_t bytes) {
  return !isContentStagingPath(path) || contentStagingRangeAllowed(offset, bytes);
}

bool genericContentWriteAllowed(std::string_view path) {
  if (path.empty() || path.front() != '/') return false;
  if (path.size() > 1 && path.back() == '/') path.remove_suffix(1);
  if (path == "/") return false;
  size_t start = 1;
  for (size_t i = 1; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == '/') {
      const auto segment = path.substr(start, i - start);
      if (segment.empty() || segment == "." || segment == ".." || segment.back() == '.' || segment.back() == ' ')
        return false;
      start = i + 1;
      continue;
    }
    const unsigned char c = static_cast<unsigned char>(path[i]);
    // Refuse FAT short-name aliases (~), trailing-dot/space aliases above,
    // NUL/control bytes and invalid FAT separators. No normalization ambiguity.
    if (c < 32 || c == 127 || c == '~' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
        c == '>' || c == '|')
      return false;
  }
  static constexpr std::string_view ROOTS[] = {"/pocket-daily/content", "/.crosspoint/content-active.0",
                                               "/.crosspoint/content-active.1"};
  for (const auto root : ROOTS) {
    if (prefix(path, root) || prefix(root, path)) return false;
  }
  return true;
}
}  // namespace PocketDaily::Content
