#pragma once
#include <cstring>
#include <string>
#include <string_view>
namespace cache_test {
inline bool epub = false, xtc = false, txt = false, markdown = false;
inline int dirty = 0, epubCleared = 0, xtcCleared = 0, txtCleared = 0;
inline void reset() {
  epub = xtc = txt = markdown = false;
  dirty = epubCleared = xtcCleared = txtCleared = 0;
}
}  // namespace cache_test
namespace FsHelpers {
inline bool hasEpubExtension(std::string_view) { return cache_test::epub; }
inline bool hasXtcExtension(std::string_view) { return cache_test::xtc; }
inline bool hasTxtExtension(std::string_view) { return cache_test::txt; }
inline bool hasMarkdownExtension(std::string_view) { return cache_test::markdown; }
}  // namespace FsHelpers
namespace library {
inline void markLibraryIndexDirty() { ++cache_test::dirty; }
}  // namespace library
class Epub {
 public:
  Epub(const std::string&, const char*) {}
  void clearCache() { ++cache_test::epubCleared; }
};
class Txt {
 public:
  Txt(const std::string&, const char*) {}
  void clearCache() { ++cache_test::txtCleared; }
};
class Xtc {
 public:
  Xtc(const std::string&, const char*) {}
  void clearCache() { ++cache_test::xtcCleared; }
};
#define LOG_DBG(...) \
  do {               \
  } while (0)
