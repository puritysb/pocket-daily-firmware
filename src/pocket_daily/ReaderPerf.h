#pragma once

#include <cstddef>
#include <cstdint>

// Page-turn timing telemetry for the EPUB reader (docs/reader-perf.md).
//
// Every page render the reader performs for a turn (or the first page after
// opening a book) is split into stages and kept in a 16-entry ring plus
// running per-stage averages and maxima. The state is one heap block held only
// while the reader is open (other modes pay nothing) and the page-turn path
// never allocates: a turn costs about 15 millis() reads, two heap probes and a
// few counter reads. The block is written to one small SD
// file when the reader exits (and every SAVE_EVERY_TURNS turns while idle),
// and /api/status reports it as `readerPerf`. It records durations, counters
// and heap figures only, never book text, titles or positions.
namespace PocketDaily::ReaderPerf {

inline constexpr char PATH[] = "/.crosspoint/reader-perf.bin";
inline constexpr uint8_t FORMAT_VERSION = 1;
inline constexpr size_t RING_SIZE = 16;
inline constexpr size_t VERSION_BYTES = 48;
inline constexpr uint16_t SAVE_EVERY_TURNS = 32;

// Order is the wire/file order: append only, bump FORMAT_VERSION otherwise.
enum Stage : uint8_t {
  STAGE_INPUT = 0,     // button handled in loop() -> render() starts (render queue wait)
  STAGE_SECTION,       // open the chapter: cached section file, or lay out up to the page
  STAGE_PAGE,          // read and decode the page from the section file
  STAGE_PREWARM,       // scan the page's glyphs and load them from the SD font
  STAGE_BW_RENDER,     // draw the page into the BW framebuffer
  STAGE_STATUS,        // draw the status bar
  STAGE_BW_REFRESH,    // BW panel update: SPI transfer plus the waveform busy wait
  STAGE_GRAY_RENDER,   // anti-aliasing: strip renders of both planes and their SPI transfer
  STAGE_GRAY_REFRESH,  // anti-aliasing waveform busy wait
  STAGE_GRAY_SYNC,     // controller RAM re-sync from the BW frame after anti-aliasing
  STAGE_SAVE,          // reading-position write
  STAGE_TOTAL,         // button handled (or render start) -> render done
  STAGE_COUNT
};

// Per-turn counters that follow the stage durations.
enum Counter : uint8_t {
  COUNTER_SD_OPENS = 0,  // file opens (each walks the FAT directory path)
  COUNTER_SD_READS,      // HalFile::read calls
  COUNTER_SD_KB,         // KiB read
  COUNTER_GLYPHS,        // glyphs loaded by the page prewarm
  COUNTER_COUNT
};

enum Flag : uint8_t {
  FLAG_CHAPTER = 0x01,   // this render opened a new section (chapter boundary or open)
  FLAG_BUILT = 0x02,     // layout ran during the render (first open of a chapter or its tail)
  FLAG_BACK = 0x04,      // backward turn
  FLAG_HALF = 0x08,      // periodic ghost-cleanup (HALF) refresh
  FLAG_AA = 0x10,        // grayscale anti-aliasing pass ran
  FLAG_IMAGES = 0x20,    // the page has images (double FAST refresh)
  FLAG_PREBUILT = 0x40,  // chapter served from a section the idle pre-build wrote
  FLAG_OPEN = 0x80,      // first page after opening the book (no button)
};

struct TurnRecord {
  uint16_t ms[STAGE_COUNT] = {};  // saturating at 65535
  uint16_t counters[COUNTER_COUNT] = {};
  uint8_t strips = 0;  // grayscale strip passes
  uint8_t flags = 0;
  uint32_t freeHeap = 0;      // lowest free heap sampled during the turn
  uint32_t largestBlock = 0;  // lowest largest free block sampled during the turn
};

// Stage durations followed by the per-turn counters.
inline constexpr size_t AGG_FIELDS = static_cast<size_t>(STAGE_COUNT) + static_cast<size_t>(COUNTER_COUNT);

// Aggregated over every recorded turn (since the file was created for this
// firmware version): stage and counter sums (for averages) and maxima.
struct Totals {
  uint32_t turns = 0;
  uint32_t sum[AGG_FIELDS] = {};
  uint16_t max[AGG_FIELDS] = {};
  uint32_t minFreeHeap = 0;  // 0 = never sampled
  uint32_t minLargestBlock = 0;
};

struct Snapshot {
  char version[VERSION_BYTES] = {};  // firmware that recorded it, NUL-terminated
  Totals totals;
  TurnRecord ring[RING_SIZE];
  uint8_t ringCount = 0;  // valid records
  uint8_t ringNext = 0;   // slot the next record goes to
  // i = 0 is the newest record; nullptr past ringCount.
  const TurnRecord* recent(size_t i) const;
};

// Fixed little-endian file image with a CRC-32 trailer.
inline constexpr size_t RECORD_BYTES = AGG_FIELDS * 2 + 2 + 8;
inline constexpr size_t FILE_BYTES = 16 + VERSION_BYTES + AGG_FIELDS * 6 + 8 + RING_SIZE * RECORD_BYTES + 4;
size_t encode(const Snapshot& snapshot, uint8_t* out, size_t capacity);
bool decode(const uint8_t* bytes, size_t size, Snapshot& snapshot);

// Adds one finished turn to a snapshot (ring + totals).
void append(Snapshot& snapshot, const TurnRecord& record);
// Adds `ms` to the newest record's stage (and to the totals); no-op without records.
void amendNewest(Snapshot& snapshot, Stage stage, uint32_t ms);

// Comma-separated field names of formatRecord(), and of formatTotals() for its
// first STAGE_COUNT + COUNTER_COUNT names.
const char* fieldNames();
// "input,section,...,largestBlock" values of one record.
size_t formatRecord(const TurnRecord& record, char* out, size_t capacity);
// Per-field averages (rounded) or maxima of the stage and counter fields.
size_t formatTotals(const Totals& totals, bool maxima, char* out, size_t capacity);

// ---- Live recorder (reader thread). No-ops when no turn is active. ----
// Starts a turn; inputMs is millis() when loop() handled the button (0: none).
void beginTurn(uint32_t inputMs, uint8_t flags);
bool turnActive();
// Closes `stage`: adds the time since the previous mark (or the turn start).
void mark(Stage stage);
// Restarts the stage clock without charging the elapsed time to any stage.
void skip();
void addFlags(uint8_t flags);
void noteGlyphs(uint32_t glyphs);
void noteStrip();
// Samples free heap and the largest free block, keeping the turn's minimum.
void sampleHeap();
void endTurn();
void abortTurn();
// Charges work deferred past the turn (the idle reading-position write) to the
// newest recorded turn's stage, without changing its total.
void addToLastTurn(Stage stage, uint32_t ms);

// Reader session: open() allocates the recorder (~0.9 KB heap) and continues
// the stored history when this firmware version wrote it; close() saves and
// frees it. Without an open recorder every call below is a no-op.
void open();
void close();
// Writes the recorder to PATH when it changed since the last save.
bool save();
// Turns recorded since the last save, for the periodic idle save.
uint16_t unsavedTurns();
// Reads PATH for /api/status.
bool load(Snapshot& snapshot);

}  // namespace PocketDaily::ReaderPerf
