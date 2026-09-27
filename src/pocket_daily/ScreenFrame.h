#pragma once

#include <cstdint>
#include <memory>

#include "pocket_daily/ContentViewState.h"
#include "pocket_daily/PocketGlance.h"
#include "pocket_daily/PocketProfile.h"
#include "pocket_daily/ScreenPresentation.h"
#include "pocket_daily/home/HomeDrawing.h"
#include "pocket_daily/home/HomeInputs.h"
#include "pocket_daily/home/HomeRenderer.h"
#include "pocket_daily/models.h"

class GfxRenderer;

// The inputs of one Sync screen paint (docs/pocket-screen-present-v1.md),
// gathered on the main task and drawn on the render task. One heap object per
// presentation, freed after the paint; never an activity member.
namespace PocketDaily::Screen {
struct Frame {
  Surface surface = Surface::None;
  DailyProfile::Profile profile{};
  AppGlance::Snapshot glance{};  // rolled to today
  bool glanceStale = false;
  Content::ContentViewState cards;  // separate <=2400 B snapshot when cards exist
  Home::OpenBook book{};
  // The daily word when it is drawn; otherwise only a placeholder id that
  // keeps the Home row count (carousel position, chevrons) right.
  PocketDaily::Card word{};
  // Home: rows from homeRowSources. Only the selected (first) row is drawn,
  // so only its text is composed; the others carry flags and ids.
  Home::Row rows[Home::ROW_CAP]{};
  int rowCount = 0;
  char firstProject[40]{};
  char firstActivity[HomeDraw::kWrapLineBytes]{};
};

enum class LoadResult : uint8_t { Ready, OutOfMemory, Unavailable };

// Main task, owner's RenderLock held. Allocates and fills one Frame for
// `surface` from the RAM profile, the stored glance, the active My cards, the
// open book and (only when drawn) the daily word. Nothing is retained on
// failure. Does not load fonts.
LoadResult loadFrame(Surface surface, const DailyProfile::Profile& profile, std::unique_ptr<Frame>& out);

// Every string the frame draws through the painters' font resolvers or the
// header, in no particular order. Stops and returns false when `visit` does.
bool visitFrameText(const Frame& frame, void* context, bool (*visit)(void* context, const char* text));

// Render task, RenderLock held: draws the frame without presenting it. Text
// that needs CJK uses `cjkFont` (0: built-in fonts only). False when the frame
// cannot be certified (a header whose font is no longer the checked one).
bool drawFrame(GfxRenderer& renderer, const Frame& frame, int cjkFont);
}  // namespace PocketDaily::Screen
