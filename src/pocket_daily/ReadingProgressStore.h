#pragma once

#include <cstdint>

#include "ReadingProgress.h"

// SD storage for reading-progress v1. Records and offers live in the book's
// cache directory (.crosspoint/epub_<hash>/), so moving, renaming or clearing a
// book's cache takes them along; the sequence counter is reader-wide.
namespace PocketDaily::ReadingProgress {
inline constexpr char RECORD_FILE[] = "/pocket-reading.bin";
inline constexpr char OFFER_FILE[] = "/pocket-reading-offer.bin";
inline constexpr char SEQUENCE_PATH[] = "/.crosspoint/pocket-reading-seq.bin";

// One encoded record or offer; callers allocate it once (heap) per operation.
struct Scratch {
  uint8_t bytes[MAX_RECORD_BYTES > MAX_OFFER_BYTES ? MAX_RECORD_BYTES : MAX_OFFER_BYTES];
};

bool loadRecord(const char* cachePath, Record& record, Scratch& scratch);
// Written to a temporary file and renamed into place; a torn write never
// replaces a good record.
bool saveRecord(const char* cachePath, const Record& record, Scratch& scratch);
bool loadOffer(const char* cachePath, Offer& offer, Scratch& scratch);
bool saveOffer(const char* cachePath, const Offer& offer, Scratch& scratch);
bool hasOffer(const char* cachePath);
void removeOffer(const char* cachePath);
// Advances and persists the reader-wide counter. A missing or damaged counter
// restarts at 1; the value is informational, never used to order positions.
uint32_t nextSequence();
}  // namespace PocketDaily::ReadingProgress
