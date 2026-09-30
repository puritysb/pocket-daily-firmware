// Link-time fake for the card page painter (src/pocket_daily/ContentPage.cpp),
// recording what ContentPresentation asked to draw.
#include "PresentationFakes.h"
#include "pocket_daily/ContentPage.h"

namespace PocketDaily::Content {
bool drawContentPage(GfxRenderer&, const ContentCard* card, const char*, int, const char* const labels[4]) {
  PresentationFake::labels.assign(labels, labels + 4);
  ++PresentationFake::draws;
  PresentationFake::titles.push_back(card ? card->card.title[0] : '-');
  return PresentationFake::drawSucceeds;
}
}  // namespace PocketDaily::Content
