#pragma once
// Host stand-in for the firmware Epub: reads a real EPUB (zip + OPF spine) with
// zlib so ChapterXPathResolver/ProgressMapper stream the same XHTML bytes the
// reader would. Only the members those streamers use are provided.
#include <Print.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

class Epub {
 public:
  struct SpineEntry {
    std::string href;  // full path inside the zip, like the firmware's spine cache
    uint32_t cumulativeSize = 0;
  };

  explicit Epub(std::string path);
  bool load();
  // In-memory book: each string is one spine item's XHTML.
  static std::shared_ptr<Epub> fromSpine(const std::vector<std::string>& items);

  int getSpineItemsCount() const { return static_cast<int>(spine.size()); }
  SpineEntry getSpineItem(int index) const {
    return (index >= 0 && index < getSpineItemsCount()) ? spine[index] : SpineEntry{};
  }
  bool readItemContentsToStream(const std::string& href, Print& out, size_t chunkSize) const;
  bool readSpineItemToStream(int index, Print& out, size_t chunkSize) const {
    return readItemContentsToStream(getSpineItem(index).href, out, chunkSize);
  }
  bool getItemSize(const std::string& href, size_t* size) const;
  size_t getCumulativeSpineItemSize(int index) const { return getSpineItem(index).cumulativeSize; }
  size_t getBookSize() const { return spine.empty() ? 0 : spine.back().cumulativeSize; }
  float calculateProgress(int spineIndex, float spineRead) const;

  // Test helper: inflated bytes of one zip entry.
  bool readItem(const std::string& href, std::string& out) const;

 private:
  struct Entry {
    std::string name;
    uint16_t method = 0;
    uint32_t compressedSize = 0;
    uint32_t size = 0;
    uint32_t localHeader = 0;
  };
  std::string path;
  std::vector<uint8_t> bytes;
  std::vector<Entry> entries;
  std::vector<SpineEntry> spine;
  std::map<std::string, std::string> memory;
};
