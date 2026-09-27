#pragma once
// Host stand-in: no laid-out section cache exists, so ProgressMapper keeps the
// streamed intra-spine position instead of refining it through page LUTs.
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

class Epub;
class GfxRenderer;

class Section {
 public:
  Section(const std::shared_ptr<Epub>&, int, GfxRenderer&) {}
  std::optional<uint16_t> getCachedPageCount() const { return std::nullopt; }
  std::optional<uint16_t> getPageForListItemIndex(uint16_t) const { return std::nullopt; }
  std::optional<uint16_t> getPageForAnchor(const std::string&) const { return std::nullopt; }
  std::optional<uint16_t> getPageForParagraphIndex(uint16_t) const { return std::nullopt; }
};
