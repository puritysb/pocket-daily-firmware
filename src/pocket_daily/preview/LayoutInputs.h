#pragma once
#include <cstddef>
#include <cstdint>

#include "components/themes/HeaderPainter.h"
#include "pocket_daily/PocketAgenda.h"
#include "pocket_daily/PocketGlance.h"
#include "pocket_daily/home/HomeInputs.h"

namespace PocketDaily::Preview {
inline constexpr size_t MAX_BYTES = 8192;
inline constexpr size_t LABEL_COUNT = 25;
inline constexpr size_t LABEL_BYTES = 192;
// Cold request-scoped input only; never retained on device, never contains a
// framebuffer, path, credential or the proposed profile. Cards are associated
// by immutable revision and fetched by the companion's existing content reader.
struct LayoutInputs {
  uint16_t schema = 1;
  uint32_t coverKey = 0, coverSize = 0, coverCrc = 0;
  char deviceId[9]{};
  char contentRevision[65]{};
  uint32_t profileGeneration = 0;
  uint32_t contentGeneration = 0;
  uint16_t physicalWidth = 0;
  uint16_t physicalHeight = 0;
  uint16_t fontPointSize = 0;
  char fontFamily[32]{};
  uint8_t cardCount = 0;
  bool isX3 = false;
  bool sleepCover = true;
  bool glanceStale = false;
  Home::Metrics metrics{};
  HeaderPainter::Inputs header{};
  Home::OpenBook book{};
  Home::BookSummary recentBooks[Home::RECENT_CAP]{};
  uint8_t recentCount = 0;
  struct Cover {
    uint32_t key = 0, size = 0, crc = 0;
  };
  Cover recentCovers[Home::RECENT_CAP]{};
  Agenda::Snapshot agenda{};
  AppGlance::Snapshot glance{};
  Card word{};
  Home::DeskClock clock;
  char labels[LABEL_COUNT][LABEL_BYTES]{};
};
// Strict bounded versioned little-endian codec. No alignment casts, allocation,
// callbacks, SD or drawing. Any error rejects the whole document.
size_t encodeLayoutInputs(const LayoutInputs& in, uint8_t* bytes, size_t capacity);
bool decodeLayoutInputs(const uint8_t* bytes, size_t size, LayoutInputs& out);
bool validLayoutInputs(const LayoutInputs& in);
void clearLayoutInputs(LayoutInputs& out);
Home::Strings layoutStrings(const LayoutInputs& in);
bool setLayoutStrings(LayoutInputs& in, const Home::Strings& strings, const char* sleepPreview);
}  // namespace PocketDaily::Preview
