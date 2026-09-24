#include "deck_store.h"

#include <HalStorage.h>

#include <cstring>

#include "agent/AgentLog.h"

namespace PocketDaily {
namespace DeckStore {
namespace {

// Shares the OTA receiver's hidden cache directory.
constexpr const char* kDir = "/.crosspoint";
constexpr const char* kPath = "/.crosspoint/pocket-daily-deck.bin";
constexpr const char* kLegacyPath = "/.crosspoint/agentdeck-deck.bin";
constexpr const char* kSlots[] = {"/.crosspoint/pocket-daily-deck.0.bin", "/.crosspoint/pocket-daily-deck.1.bin"};

// Fixed-layout header. recordSize doubles as the schema version: any change to
// Record's shape invalidates old files instead of misreading them.
struct Header {
  uint32_t magic;
  uint8_t version;
  uint8_t count;
  uint16_t recordSize;
  uint32_t savedEpoch;
};
static_assert(sizeof(Header) == 12, "header layout must stay stable on disk");

constexpr uint32_t kMagic = 0x314B4441;  // "ADK1" (LE)
// v2 (2026-07-31): deckSig + GlanceInfo ride between the header and the
// records. v1 files are simply invalidated — the deck cache is a convenience,
// not data worth migrating.
// v3 (2026-08-04): GlanceInfo grew the events[] schedule block (M9 stage 2) —
// sizeof(GlanceInfo) changed, so v2 files must be invalidated, not misread.
// v4 (2026-08-04): autonomous Pocket cards ride the snapshot.
// v5 (2026-08-24): weather WMO codes and the immutable host sync HH:MM are
// persisted for honest local glyphs/status after an offline reboot.
// v6 (2026-08-24): five compact forecast days are persisted for the offline
// weather ribbon; the larger GlanceInfo shape invalidates v5 safely.
constexpr uint8_t kVersion = 6;
// v7 slots append a generation after Header and a CRC32 after the v6 payload.
// No rename/delete publication: a complete, checked slot is the commit record.
constexpr uint8_t kSlotVersion = 7;

enum class SlotResult { Missing, Invalid, ReadFailure, Valid };
struct SlotInfo {
  uint32_t generation = 0;
  uint32_t crc = 0;
};

uint32_t crcUpdate(uint32_t crc, const void* data, size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) {
    crc ^= bytes[i];
    for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0u);
  }
  return crc;
}

size_t payloadSize(uint8_t count) {
  return sizeof(Snapshot::deckSig) + sizeof(Snapshot::serverHm) + sizeof(Glance) + 1 + sizeof(Snapshot::pocketCards) +
         sizeof(Record) * count;
}

SlotResult inspectSlot(int slot, SlotInfo& info) {
  if (!Storage.exists(kSlots[slot])) return SlotResult::Missing;
  HalFile file = Storage.open(kSlots[slot], O_RDONLY);
  if (!file) return SlotResult::ReadFailure;
  Header header{};
  if (file.size() < sizeof(header)) return SlotResult::Invalid;
  if (file.read(&header, sizeof(header)) != sizeof(header)) return SlotResult::ReadFailure;
  if (header.magic != kMagic || header.version != kSlotVersion || header.recordSize != sizeof(Record) ||
      header.count > AgentDeckCfg::SESSIONS_CAP)
    return SlotResult::Invalid;
  const size_t size = sizeof(header) + sizeof(info.generation) + payloadSize(header.count) + sizeof(info.crc);
  if (file.size() != size) return SlotResult::Invalid;
  if (file.read(&info.generation, sizeof(info.generation)) != sizeof(info.generation)) return SlotResult::ReadFailure;
  if (info.generation == 0) return SlotResult::Invalid;
  if (!file.seek(sizeof(header) + sizeof(info.generation) + sizeof(Snapshot::deckSig) + sizeof(Snapshot::serverHm) +
                 sizeof(Glance)))
    return SlotResult::ReadFailure;
  const int count = file.read();
  if (count < 0) return SlotResult::ReadFailure;
  if (count > CARD_CAP) return SlotResult::Invalid;
  if (!file.seek(0)) return SlotResult::ReadFailure;
  // Bounded scratch: no second 6 KiB snapshot and no permanent RAM buffer.
  uint8_t bytes[128];
  uint32_t crc = 0xFFFFFFFFu;
  size_t remaining = size - sizeof(info.crc);
  while (remaining) {
    const size_t chunk = remaining < sizeof(bytes) ? remaining : sizeof(bytes);
    if (file.read(bytes, chunk) != static_cast<int>(chunk)) return SlotResult::ReadFailure;
    crc = crcUpdate(crc, bytes, chunk);
    remaining -= chunk;
  }
  if (file.read(&info.crc, sizeof(info.crc)) != sizeof(info.crc)) return SlotResult::ReadFailure;
  return info.crc == (crc ^ 0xFFFFFFFFu) ? SlotResult::Valid : SlotResult::Invalid;
}

int newestSlot(const SlotResult (&results)[2], const SlotInfo (&info)[2]) {
  if (results[0] != SlotResult::Valid && results[1] != SlotResult::Valid) return -1;
  return results[1] == SlotResult::Valid && (results[0] != SlotResult::Valid || info[1].generation > info[0].generation)
             ? 1
             : 0;
}

bool writePart(HalFile& file, uint32_t& crc, const void* data, size_t size) {
  if (file.write(data, size) != size) return false;
  crc = crcUpdate(crc, data, size);
  return true;
}

bool readPart(HalFile& file, uint32_t& crc, void* data, size_t size) {
  if (file.read(data, size) != static_cast<int>(size)) return false;
  crc = crcUpdate(crc, data, size);
  return true;
}

void terminatePocket(Card& card) {
  card.cardId[sizeof(card.cardId) - 1] = '\0';
  card.module[sizeof(card.module) - 1] = '\0';
  card.actionClass[sizeof(card.actionClass) - 1] = '\0';
  card.title[sizeof(card.title) - 1] = '\0';
  card.question[sizeof(card.question) - 1] = '\0';
  card.context[sizeof(card.context) - 1] = '\0';
  if (card.choiceCount > 3) card.choiceCount = 3;
  for (uint8_t i = 0; i < card.choiceCount; i++) {
    card.choices[i].id[sizeof(card.choices[i].id) - 1] = '\0';
    card.choices[i].label[sizeof(card.choices[i].label) - 1] = '\0';
  }
}

bool saveSnapshot(const Snapshot& snap) {
  if (!Storage.ready()) return false;
  if (snap.count > AgentDeckCfg::SESSIONS_CAP || snap.pocketCount > CARD_CAP) return false;
  if (!Storage.mkdir(kDir) && !Storage.exists(kDir)) return false;
  SlotInfo info[2];
  const SlotResult results[] = {inspectSlot(0, info[0]), inspectSlot(1, info[1])};
  // A temporarily unreadable slot could be the sole good copy. Fail closed.
  if (results[0] == SlotResult::ReadFailure || results[1] == SlotResult::ReadFailure) return false;
  const int latest = newestSlot(results, info);
  const uint32_t generation = latest < 0 ? 0 : info[latest].generation;
  if (generation == UINT32_MAX) return false;  // No wraparound ambiguity.
  const int target = latest == 0 ? 1 : 0;
  const uint32_t nextGeneration = generation + 1;
  uint32_t crc = 0xFFFFFFFFu;
  {
    // All SD I/O goes through HalStorage so it shares storageMutex with the
    // render task's font reads (see AgentLog for the SdFat two-task hazard).
    HalFile f = Storage.open(kSlots[target], O_WRITE | O_CREAT | O_TRUNC);
    if (!f) return false;
    const Header h{kMagic, kSlotVersion, snap.count, (uint16_t)sizeof(Record), snap.savedEpoch};
    if (!writePart(f, crc, &h, sizeof(h)) || !writePart(f, crc, &nextGeneration, sizeof(nextGeneration))) return false;
    // v2 payload between header and records: sig echo + glance snapshot.
    if (!writePart(f, crc, snap.deckSig, sizeof(snap.deckSig)) ||
        !writePart(f, crc, snap.serverHm, sizeof(snap.serverHm)) ||
        !writePart(f, crc, &snap.glance, sizeof(snap.glance)) ||
        !writePart(f, crc, &snap.pocketCount, sizeof(snap.pocketCount)) ||
        !writePart(f, crc, snap.pocketCards, sizeof(snap.pocketCards)))
      return false;
    const size_t bytes = sizeof(Record) * snap.count;
    if (bytes && !writePart(f, crc, snap.records, bytes)) return false;
    crc ^= 0xFFFFFFFFu;
    if (f.write(&crc, sizeof(crc)) != sizeof(crc)) return false;
  }  // HalFile destructor closes under the mutex
  SlotInfo written;
  return inspectSlot(target, written) == SlotResult::Valid && written.generation == nextGeneration &&
         written.crc == crc;
}

bool readSnapshot(const char* path, uint8_t version, Snapshot& out) {
  memset(&out, 0, sizeof(out));
  out.glance.clear();
  if (!Storage.ready()) return false;
  if (!Storage.exists(path)) return false;
  HalFile f = Storage.open(path, O_RDONLY);
  if (!f) return false;
  Header h{};
  if (f.read(&h, sizeof(h)) != (int)sizeof(h)) return false;
  if (h.magic != kMagic || h.version != version || h.recordSize != sizeof(Record) ||
      h.count > AgentDeckCfg::SESSIONS_CAP) {
    AgentLog::line("DECK", "deck cache rejected (magic/version/shape mismatch)");
    return false;
  }
  const size_t prefix = sizeof(h) + (version == kSlotVersion ? sizeof(uint32_t) : 0);
  const size_t trailer = version == kSlotVersion ? sizeof(uint32_t) : 0;
  if (f.size() != prefix + payloadSize(h.count) + trailer) return false;
  uint32_t crc = crcUpdate(0xFFFFFFFFu, &h, sizeof(h));
  uint32_t generation = 0;
  if (version == kSlotVersion && (!readPart(f, crc, &generation, sizeof(generation)) || generation == 0)) return false;
  if (!readPart(f, crc, out.deckSig, sizeof(out.deckSig)) || !readPart(f, crc, out.serverHm, sizeof(out.serverHm)) ||
      !readPart(f, crc, &out.glance, sizeof(out.glance)) ||
      !readPart(f, crc, &out.pocketCount, sizeof(out.pocketCount)) ||
      !readPart(f, crc, out.pocketCards, sizeof(out.pocketCards)) || out.pocketCount > CARD_CAP) {
    memset(&out, 0, sizeof(out));
    out.glance.clear();
    return false;
  }
  const size_t bytes = sizeof(Record) * h.count;
  if (bytes && !readPart(f, crc, out.records, bytes)) {
    memset(&out, 0, sizeof(out));
    out.glance.clear();
    return false;
  }
  uint32_t storedCrc = 0;
  if (version == kSlotVersion &&
      (f.read(&storedCrc, sizeof(storedCrc)) != sizeof(storedCrc) || storedCrc != (crc ^ 0xFFFFFFFFu)))
    return false;
  out.deckSig[sizeof(out.deckSig) - 1] = '\0';
  out.serverHm[sizeof(out.serverHm) - 1] = '\0';
  for (uint8_t i = 0; i < out.pocketCount; i++) terminatePocket(out.pocketCards[i]);
  // Defensive termination: the file is external input to this boot.
  for (uint8_t i = 0; i < h.count; i++) {
    Record& r = out.records[i];
    r.sid[sizeof(r.sid) - 1] = '\0';
    r.project[sizeof(r.project) - 1] = '\0';
    r.agentType[sizeof(r.agentType) - 1] = '\0';
    r.state[sizeof(r.state) - 1] = '\0';
    r.activity[sizeof(r.activity) - 1] = '\0';
    r.actionClass[sizeof(r.actionClass) - 1] = '\0';
  }
  out.count = h.count;
  out.savedEpoch = h.savedEpoch;
  AgentLog::line("DECK", "deck cache loaded: %u cards epoch=%lu", (unsigned)out.count, (unsigned long)out.savedEpoch);
  return true;
}

}  // namespace

bool save(const Snapshot& snap) {
  const bool saved = saveSnapshot(snap);
  if (!saved) AgentLog::line("DECK", "deck save failed; previous slot retained");
  return saved;
}

bool load(Snapshot& out) {
  // Callers serialize deck access on the activity loop. No other code writes
  // these private files between inspection and loading.
  SlotInfo info[2];
  SlotResult results[] = {inspectSlot(0, info[0]), inspectSlot(1, info[1])};
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    const int latest = newestSlot(results, info);
    if (latest < 0) break;
    if (readSnapshot(kSlots[latest], kSlotVersion, out)) return true;
    results[latest] = SlotResult::ReadFailure;
  }
  // Keep v6 inputs untouched, including during first v7 save or downgrade.
  if (readSnapshot(kPath, kVersion, out) || readSnapshot(kLegacyPath, kVersion, out)) return true;
  memset(&out, 0, sizeof(out));
  out.glance.clear();
  return false;
}

}  // namespace DeckStore
}  // namespace PocketDaily
