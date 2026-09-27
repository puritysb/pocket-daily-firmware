#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

class Epub;
class Section;

// Reader side of reading-progress v1 (docs/reading-progress-v1.md), called from
// EpubReaderActivity hooks. Nothing here runs in the HTTP handler.
namespace PocketDaily::ReadingProgress {
// Where the book was left, captured while the Section is still loaded: its
// paragraph LUT tells which paragraph the page starts in and how far into it.
struct ExitPosition {
  int spine = 0;
  int page = 0;
  int pageCount = 0;
  bool hasParagraph = false;
  uint16_t paragraph = 0;  // 1-based <p> the page starts in
  float withinParagraph = 0.0f;
};
ExitPosition capturePosition(const Section* section, int spine, int page, int pageCount);

// Persists a record (XPointer, start-of-page percentage, sequence) for the
// companion. Skips unchanged positions; skips the XPointer (reported as null)
// when the largest free block cannot hold the chapter stream. Call after the
// Section is released, before the book's cache directory can move.
void recordPosition(const std::shared_ptr<Epub>& epub, const ExitPosition& position);

// True when another device left a pending position for this book.
bool hasPendingOffer(const Epub& epub);

// Loads the pending offer when it is further than `currentPercentage`
// (discarding it otherwise) and formats the question the reader asks,
// "<device>: <pct>% · Go there?". The offer stays pending until discardOffer().
bool takeFurtherOffer(const Epub& epub, float currentPercentage, std::string& question, std::string& xpointer,
                      float& percentage);
// Drops the pending offer once the question was answered either way.
void discardOffer(const Epub& epub);
}  // namespace PocketDaily::ReadingProgress
