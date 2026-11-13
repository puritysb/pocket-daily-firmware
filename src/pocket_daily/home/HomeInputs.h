#pragma once

#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>

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

// The local book, daily word, full-screen clock and up to CARD_CAP companion cards.
inline constexpr int ROW_CAP = RECENT_CAP + 2 + PocketDaily::CARD_CAP;

// tr() values for the painters' labels.
Strings deviceStrings();
DeskClock readDeskClock(const AppGlance::Snapshot& glance, time_t now, int16_t companionOffset = INT16_MIN);
// The active theme's metrics.
Metrics deviceMetrics();

// A glance saved on an earlier local day drops that day's schedule and past
// forecast days (docs/pocket-glance-v1.md). Needs a set clock; the app's UTC
// offset gives the local date without a timezone database. Idempotent.
void rollGlanceToToday(PocketDaily::AppGlance::Snapshot& snapshot, time_t now);

// The open book's Home/Brief text: title and author from RECENT_BOOKS (the
// file name when there is no title) and the whole-book percent from the
// seventh byte of its progress.bin (-1 when absent). No allocation.
using OpenBook = BookSummary;
inline constexpr size_t RECENT_PATH_BYTES = 256;
// Resolves the cache template to a bounded existing full cover or themed thumbnail.
bool resolveBookCoverPath(const RecentBook& book, char* path, size_t capacity);
uint8_t readRecentBookEntries(const RecentBook* (&entries)[RECENT_CAP]);
// Reads bounded metadata only; optional paths stay exact or the entry is omitted.
uint8_t readRecentBooks(BookSummary (&books)[RECENT_CAP], char (*paths)[RECENT_PATH_BYTES] = nullptr,
                        char (*covers)[RECENT_PATH_BYTES] = nullptr);
// Fills `out` (cleared first); returns the RECENT_BOOKS entry, or nullptr.
// The book Continue Reading resumes: the open book, otherwise the most recent
// book still on the card (what stock Home shows first). Empty when none.
std::string continueReadingPath();
const RecentBook* readOpenBook(OpenBook& out);

// Draws the PBM image of one of the active companion cards fitted into the box
// (existing PBM path, <=64 B row reads, no heap). Returns the height drawn.
int drawContentCardImage(GfxRenderer& renderer, const PocketDaily::Content::ContentViewState& content,
                         const char* cardId, int x, int y, int width, int height);

}  // namespace PocketDaily::Home
