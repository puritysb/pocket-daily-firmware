#pragma once

#include <cstdint>
#include <cstring>
#include <ctime>

#include "pocket_daily/PocketGlance.h"
#include "pocket_daily/home/HomeRenderer.h"
#include "pocket_daily/models.h"

class GfxRenderer;
struct RecentBook;
namespace PocketDaily::Content {
class ContentViewState;
}

// Device-side inputs of the shared Home / Daily Brief painters, used by
// PocketDailyActivity and the Sync screen presenter so the frame shown inside
// Sync cannot drift from the one Pocket Daily paints. Device only (I18n,
// UITheme, APP_STATE, RECENT_BOOKS, HalStorage); the painters themselves stay
// host-compiled in HomeRenderer/HomeDrawing.
namespace PocketDaily::Home {

// The local book plus up to CARD_CAP companion cards.
inline constexpr int ROW_CAP = 1 + PocketDaily::CARD_CAP;

// tr() values for the painters' labels.
Strings deviceStrings();
// The active theme's metrics.
Metrics deviceMetrics();

// A glance saved on an earlier local day drops that day's schedule and past
// forecast days (docs/pocket-glance-v1.md). Needs a set clock; the app's UTC
// offset gives the local date without a timezone database. Idempotent.
void rollGlanceToToday(PocketDaily::AppGlance::Snapshot& snapshot, time_t now);

// The open book's Home/Brief text: title and author from RECENT_BOOKS (the
// file name when there is no title) and the whole-book percent from the
// seventh byte of its progress.bin (-1 when absent). No allocation.
struct OpenBook {
  char title[96];
  char author[80];
  int8_t percent;
  bool valid;
  void clear() {
    memset(this, 0, sizeof(*this));
    percent = -1;
  }
};
// Fills `out` (cleared first); returns the RECENT_BOOKS entry, or nullptr.
const RecentBook* readOpenBook(OpenBook& out);

// Draws the PBM image of one of the active companion cards fitted into the box
// (existing PBM path, <=64 B row reads, no heap). Returns the height drawn.
int drawContentCardImage(GfxRenderer& renderer, const PocketDaily::Content::ContentViewState& content,
                         const char* cardId, int x, int y, int width, int height);

}  // namespace PocketDaily::Home
