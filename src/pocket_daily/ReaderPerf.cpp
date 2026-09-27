#include "ReaderPerf.h"

#include <Arduino.h>
#include <HalIoCounters.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <cstring>
#include <memory>

namespace PocketDaily::ReaderPerf {
namespace {
constexpr uint8_t MAGIC[4] = {'P', 'D', 'R', 'P'};
constexpr char TEMP_PATH[] = "/.crosspoint/reader-perf.tmp";

constexpr const char FIELD_NAMES[] =
    "input,section,page,prewarm,bwRender,status,bwRefresh,grayRender,grayRefresh,graySync,save,total,"
    "sdOpens,sdReads,sdKB,glyphs,strips,flags,freeHeap,largestBlock";

uint32_t crc32(const uint8_t* data, const size_t size) {
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}

void put16(uint8_t*& out, const uint16_t value) {
  out[0] = static_cast<uint8_t>(value);
  out[1] = static_cast<uint8_t>(value >> 8);
  out += 2;
}
void put32(uint8_t*& out, const uint32_t value) {
  for (int i = 0; i < 4; ++i) out[i] = static_cast<uint8_t>(value >> (8 * i));
  out += 4;
}
uint16_t get16(const uint8_t*& in) {
  const uint16_t value = static_cast<uint16_t>(in[0] | (in[1] << 8));
  in += 2;
  return value;
}
uint32_t get32(const uint8_t*& in) {
  const uint32_t value = static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
                         (static_cast<uint32_t>(in[2]) << 16) | (static_cast<uint32_t>(in[3]) << 24);
  in += 4;
  return value;
}

uint16_t saturate16(const uint32_t value) { return value > 0xFFFFU ? 0xFFFFU : static_cast<uint16_t>(value); }

// Sequential writer into a caller buffer that never overflows it.
struct Csv {
  char* out;
  size_t capacity;
  size_t used = 0;
  bool ok = true;
  void add(const uint32_t value) {
    if (!ok) return;
    const int n = snprintf(out + used, capacity - used, used ? ",%lu" : "%lu", static_cast<unsigned long>(value));
    if (n < 0 || static_cast<size_t>(n) >= capacity - used) {
      ok = false;
      return;
    }
    used += static_cast<size_t>(n);
  }
  size_t finish() {
    if (!ok) {
      if (capacity) out[0] = '\0';
      return 0;
    }
    return used;
  }
};

// The live recorder. Held on the heap only while the reader is open (open() to
// close()), so the ~0.9 KB costs nothing in other modes (Wi-Fi above all); the
// page-turn path itself never allocates.
struct Live {
  Snapshot snapshot;
  TurnRecord turn;
  bool active = false;
  uint16_t unsaved = 0;
  uint32_t inputMs = 0;
  uint32_t lastMarkMs = 0;
  HalIoCounters ioAtStart{};
};
std::unique_ptr<Live> live;
}  // namespace

const TurnRecord* Snapshot::recent(const size_t i) const {
  if (i >= ringCount) return nullptr;
  return &ring[(ringNext + RING_SIZE - 1 - i) % RING_SIZE];
}

size_t encode(const Snapshot& snapshot, uint8_t* out, const size_t capacity) {
  if (!out || capacity < FILE_BYTES || snapshot.ringCount > RING_SIZE || snapshot.ringNext >= RING_SIZE) return 0;
  memset(out, 0, FILE_BYTES);
  uint8_t* p = out;
  memcpy(p, MAGIC, sizeof(MAGIC));
  p += sizeof(MAGIC);
  *p++ = FORMAT_VERSION;
  *p++ = STAGE_COUNT;
  *p++ = COUNTER_COUNT;
  *p++ = static_cast<uint8_t>(RECORD_BYTES);
  *p++ = snapshot.ringCount;
  *p++ = snapshot.ringNext;
  p += 2;
  put32(p, snapshot.totals.turns);
  memcpy(p, snapshot.version, strnlen(snapshot.version, VERSION_BYTES - 1));
  p += VERSION_BYTES;
  for (size_t i = 0; i < AGG_FIELDS; ++i) {
    put32(p, snapshot.totals.sum[i]);
    put16(p, snapshot.totals.max[i]);
  }
  put32(p, snapshot.totals.minFreeHeap);
  put32(p, snapshot.totals.minLargestBlock);
  for (const TurnRecord& r : snapshot.ring) {
    for (const uint16_t ms : r.ms) put16(p, ms);
    for (const uint16_t c : r.counters) put16(p, c);
    *p++ = r.strips;
    *p++ = r.flags;
    put32(p, r.freeHeap);
    put32(p, r.largestBlock);
  }
  const size_t crcOffset = FILE_BYTES - 4;
  uint8_t* crc = out + crcOffset;
  put32(crc, crc32(out, crcOffset));
  return FILE_BYTES;
}

bool decode(const uint8_t* bytes, const size_t size, Snapshot& snapshot) {
  if (!bytes || size != FILE_BYTES || memcmp(bytes, MAGIC, sizeof(MAGIC)) != 0) return false;
  const uint8_t* crc = bytes + FILE_BYTES - 4;
  if (get32(crc) != crc32(bytes, FILE_BYTES - 4)) return false;
  if (bytes[4] != FORMAT_VERSION || bytes[5] != STAGE_COUNT || bytes[6] != COUNTER_COUNT || bytes[7] != RECORD_BYTES ||
      bytes[8] > RING_SIZE || bytes[9] >= RING_SIZE || bytes[16 + VERSION_BYTES - 1] != '\0') {
    return false;
  }
  // Fully validated above, so the caller's snapshot is only written on success
  // (decoding in place: a Snapshot is ~0.9 KB, too large for a stack copy).
  snapshot.ringCount = bytes[8];
  snapshot.ringNext = bytes[9];
  const uint8_t* p = bytes + 12;
  snapshot.totals.turns = get32(p);
  memcpy(snapshot.version, p, VERSION_BYTES);
  p += VERSION_BYTES;
  for (size_t i = 0; i < AGG_FIELDS; ++i) {
    snapshot.totals.sum[i] = get32(p);
    snapshot.totals.max[i] = get16(p);
  }
  snapshot.totals.minFreeHeap = get32(p);
  snapshot.totals.minLargestBlock = get32(p);
  for (TurnRecord& r : snapshot.ring) {
    for (uint16_t& ms : r.ms) ms = get16(p);
    for (uint16_t& c : r.counters) c = get16(p);
    r.strips = *p++;
    r.flags = *p++;
    r.freeHeap = get32(p);
    r.largestBlock = get32(p);
  }
  return true;
}

void append(Snapshot& snapshot, const TurnRecord& record) {
  snapshot.ring[snapshot.ringNext] = record;
  snapshot.ringNext = static_cast<uint8_t>((snapshot.ringNext + 1) % RING_SIZE);
  if (snapshot.ringCount < RING_SIZE) snapshot.ringCount++;
  Totals& t = snapshot.totals;
  // Sums saturate instead of wrapping so a very long history degrades to a
  // capped average rather than a wrong one.
  if (t.turns == UINT32_MAX) return;
  t.turns++;
  for (size_t i = 0; i < AGG_FIELDS; ++i) {
    const uint16_t v = i < STAGE_COUNT ? record.ms[i] : record.counters[i - STAGE_COUNT];
    t.sum[i] = t.sum[i] > UINT32_MAX - v ? UINT32_MAX : t.sum[i] + v;
    if (v > t.max[i]) t.max[i] = v;
  }
  if (record.freeHeap && (!t.minFreeHeap || record.freeHeap < t.minFreeHeap)) t.minFreeHeap = record.freeHeap;
  if (record.largestBlock && (!t.minLargestBlock || record.largestBlock < t.minLargestBlock)) {
    t.minLargestBlock = record.largestBlock;
  }
}

void amendNewest(Snapshot& snapshot, const Stage stage, const uint32_t ms) {
  if (snapshot.ringCount == 0 || stage >= STAGE_COUNT || stage == STAGE_TOTAL) return;
  TurnRecord& newest = snapshot.ring[(snapshot.ringNext + RING_SIZE - 1) % RING_SIZE];
  const uint16_t before = newest.ms[stage];
  newest.ms[stage] = saturate16(before + ms);
  Totals& t = snapshot.totals;
  const uint32_t added = newest.ms[stage] - before;
  t.sum[stage] = t.sum[stage] > UINT32_MAX - added ? UINT32_MAX : t.sum[stage] + added;
  if (newest.ms[stage] > t.max[stage]) t.max[stage] = newest.ms[stage];
}

const char* fieldNames() { return FIELD_NAMES; }

size_t formatRecord(const TurnRecord& record, char* out, const size_t capacity) {
  if (!out || !capacity) return 0;
  Csv csv{out, capacity};
  for (const uint16_t ms : record.ms) csv.add(ms);
  for (const uint16_t c : record.counters) csv.add(c);
  csv.add(record.strips);
  csv.add(record.flags);
  csv.add(record.freeHeap);
  csv.add(record.largestBlock);
  return csv.finish();
}

size_t formatTotals(const Totals& totals, const bool maxima, char* out, const size_t capacity) {
  if (!out || !capacity) return 0;
  Csv csv{out, capacity};
  for (size_t i = 0; i < AGG_FIELDS; ++i) {
    if (maxima) {
      csv.add(totals.max[i]);
    } else {
      csv.add(totals.turns ? (totals.sum[i] + totals.turns / 2) / totals.turns : 0);
    }
  }
  return csv.finish();
}

// ---- Live recorder ----

void beginTurn(const uint32_t inputMs, const uint8_t flags) {
  if (!live) return;
  const uint32_t now = millis();
  live->turn = TurnRecord{};
  live->turn.flags = flags;
  live->active = true;
  live->inputMs = inputMs && inputMs <= now ? inputMs : now;
  live->lastMarkMs = now;
  live->ioAtStart = halIoCounters;
  live->turn.ms[STAGE_INPUT] = saturate16(now - live->inputMs);
}

bool turnActive() { return live && live->active; }

void mark(const Stage stage) {
  if (!turnActive() || stage >= STAGE_COUNT) return;
  const uint32_t now = millis();
  live->turn.ms[stage] = saturate16(static_cast<uint32_t>(live->turn.ms[stage]) + (now - live->lastMarkMs));
  live->lastMarkMs = now;
}

void skip() {
  if (turnActive()) live->lastMarkMs = millis();
}

void addFlags(const uint8_t flags) {
  if (turnActive()) live->turn.flags |= flags;
}

void noteGlyphs(const uint32_t glyphs) {
  if (turnActive()) live->turn.counters[COUNTER_GLYPHS] = saturate16(live->turn.counters[COUNTER_GLYPHS] + glyphs);
}

void noteStrip() {
  if (turnActive() && live->turn.strips < 0xFF) live->turn.strips++;
}

void sampleHeap() {
  if (!turnActive()) return;
  const uint32_t freeHeap = ESP.getFreeHeap();
  // Walks the free list: sampled twice per turn, never per strip.
  const uint32_t largest = ESP.getMaxAllocHeap();
  TurnRecord& turn = live->turn;
  if (!turn.freeHeap || freeHeap < turn.freeHeap) turn.freeHeap = freeHeap;
  if (!turn.largestBlock || largest < turn.largestBlock) turn.largestBlock = largest;
}

void endTurn() {
  if (!turnActive()) return;
  const uint32_t now = millis();
  TurnRecord& turn = live->turn;
  turn.ms[STAGE_TOTAL] = saturate16(now - live->inputMs);
  const HalIoCounters io = halIoCounters;
  turn.counters[COUNTER_SD_OPENS] = saturate16(io.opens - live->ioAtStart.opens);
  turn.counters[COUNTER_SD_READS] = saturate16(io.readCalls - live->ioAtStart.readCalls);
  turn.counters[COUNTER_SD_KB] = saturate16((io.readBytes - live->ioAtStart.readBytes + 512U) / 1024U);
  append(live->snapshot, turn);
  live->active = false;
  if (live->unsaved < UINT16_MAX) live->unsaved++;
}

void abortTurn() {
  if (live) live->active = false;
}

void addToLastTurn(const Stage stage, const uint32_t ms) {
  if (!live || live->active) return;  // only between turns
  amendNewest(live->snapshot, stage, ms);
  if (live->unsaved == 0) live->unsaved = 1;  // the stored copy is now stale
}

uint16_t unsavedTurns() { return live ? live->unsaved : 0; }

void open() {
  if (live) return;
  // ~0.9 KB for the reader session: a stack copy would exceed the frame budget, and a
  // static block would cost every other mode the same heap.
  live = makeUniqueNoThrow<Live>();
  if (!live) {
    LOG_ERR("PERF", "OOM: %u B page-turn recorder; timings off", static_cast<unsigned>(sizeof(Live)));
    return;
  }
  // Continue this firmware's history; another version's numbers start over.
  if (!load(live->snapshot) || strncmp(live->snapshot.version, CROSSPOINT_VERSION, VERSION_BYTES) != 0) {
    live->snapshot = Snapshot{};
  }
  snprintf(live->snapshot.version, VERSION_BYTES, "%s", CROSSPOINT_VERSION);
}

void close() {
  if (!live) return;
  save();
  live.reset();
}

bool save() {
  if (!live || live->unsaved == 0) return true;
  // FILE_BYTES (844 B) transient: over the 256 B stack budget, needed only for this write.
  auto bytes = makeUniqueNoThrow<uint8_t[]>(FILE_BYTES);
  if (!bytes) {
    LOG_ERR("PERF", "OOM: %u B save buffer", static_cast<unsigned>(FILE_BYTES));
    return false;
  }
  if (encode(live->snapshot, bytes.get(), FILE_BYTES) != FILE_BYTES) return false;
  {
    HalFile out = Storage.open(TEMP_PATH, O_WRITE | O_CREAT | O_TRUNC);
    if (!out || out.write(bytes.get(), FILE_BYTES) != FILE_BYTES) {
      if (out) out.close();
      Storage.remove(TEMP_PATH);
      LOG_ERR("PERF", "Could not write %s", TEMP_PATH);
      return false;
    }
    out.close();  // SdFat must not rename a path that is still open.
  }
  Storage.remove(PATH);
  if (!Storage.rename(TEMP_PATH, PATH)) {
    LOG_ERR("PERF", "Could not rename %s", TEMP_PATH);
    return false;
  }
  live->unsaved = 0;
  return true;
}

bool load(Snapshot& snapshot) {
  HalFile file = Storage.open(PATH, O_RDONLY);
  if (!file) return false;
  if (file.size() != FILE_BYTES) return false;
  // Same transient buffer rationale as save().
  auto bytes = makeUniqueNoThrow<uint8_t[]>(FILE_BYTES);
  if (!bytes) {
    LOG_ERR("PERF", "OOM: %u B load buffer", static_cast<unsigned>(FILE_BYTES));
    return false;
  }
  const bool read = file.read(bytes.get(), FILE_BYTES) == static_cast<int>(FILE_BYTES);
  file.close();
  return read && decode(bytes.get(), FILE_BYTES, snapshot);
}

}  // namespace PocketDaily::ReaderPerf
