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
constexpr const char* kTmpPath = "/.crosspoint/pocket-daily-deck.tmp";
constexpr const char* kLegacyPath = "/.crosspoint/agentdeck-deck.bin";

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

}  // namespace

bool save(const Snapshot& snap) {
  if (!Storage.ready()) return false;
  Storage.mkdir(kDir);
  {
    // All SD I/O goes through HalStorage so it shares storageMutex with the
    // render task's font reads (see AgentLog for the SdFat two-task hazard).
    HalFile f = Storage.open(kTmpPath, O_WRITE | O_CREAT | O_TRUNC);
    if (!f) return false;
    const Header h{kMagic, kVersion, snap.count, (uint16_t)sizeof(Record), snap.savedEpoch};
    if (f.write(&h, sizeof(h)) != sizeof(h)) return false;
    // v2 payload between header and records: sig echo + glance snapshot.
    if (f.write(snap.deckSig, sizeof(snap.deckSig)) != sizeof(snap.deckSig)) return false;
    if (f.write(snap.serverHm, sizeof(snap.serverHm)) != sizeof(snap.serverHm)) return false;
    if (f.write(&snap.glance, sizeof(snap.glance)) != (int)sizeof(snap.glance)) return false;
    const uint8_t pocketCount = snap.pocketCount > CARD_CAP ? CARD_CAP : snap.pocketCount;
    if (f.write(&pocketCount, sizeof(pocketCount)) != sizeof(pocketCount)) return false;
    if (f.write(snap.pocketCards, sizeof(snap.pocketCards)) != sizeof(snap.pocketCards)) return false;
    const size_t bytes = sizeof(Record) * snap.count;
    if (bytes && f.write(snap.records, bytes) != bytes) return false;
  }  // HalFile destructor closes under the mutex
  Storage.remove(kPath);  // FAT rename fails onto an existing file
  if (!Storage.rename(kTmpPath, kPath)) {
    AgentLog::line("DECK", "deck save rename failed");
    return false;
  }
  return true;
}

bool load(Snapshot& out) {
  memset(&out, 0, sizeof(out));
  out.glance.clear();
  if (!Storage.ready()) return false;
  const bool migrateLegacy = !Storage.exists(kPath) && Storage.exists(kLegacyPath);
  const char* path = migrateLegacy ? kLegacyPath : kPath;
  if (!Storage.exists(path)) return false;
  HalFile f = Storage.open(path, O_RDONLY);
  if (!f) return false;
  Header h{};
  if (f.read(&h, sizeof(h)) != (int)sizeof(h)) return false;
  if (h.magic != kMagic || h.version != kVersion || h.recordSize != sizeof(Record) ||
      h.count > AgentDeckCfg::SESSIONS_CAP) {
    AgentLog::line("DECK", "deck cache rejected (magic/version/shape mismatch)");
    return false;
  }
  if (f.read(out.deckSig, sizeof(out.deckSig)) != (int)sizeof(out.deckSig) ||
      f.read(out.serverHm, sizeof(out.serverHm)) != (int)sizeof(out.serverHm) ||
      f.read(&out.glance, sizeof(out.glance)) != (int)sizeof(out.glance) ||
      f.read(&out.pocketCount, sizeof(out.pocketCount)) != (int)sizeof(out.pocketCount) ||
      f.read(out.pocketCards, sizeof(out.pocketCards)) != (int)sizeof(out.pocketCards) || out.pocketCount > CARD_CAP) {
    memset(&out, 0, sizeof(out));
    out.glance.clear();
    return false;
  }
  out.deckSig[sizeof(out.deckSig) - 1] = '\0';
  out.serverHm[sizeof(out.serverHm) - 1] = '\0';
  for (uint8_t i = 0; i < out.pocketCount; i++) terminatePocket(out.pocketCards[i]);
  const size_t bytes = sizeof(Record) * h.count;
  if (bytes && f.read(out.records, bytes) != (int)bytes) {
    memset(&out, 0, sizeof(out));
    out.glance.clear();
    return false;
  }
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
  f.close();
  if (migrateLegacy && save(out)) {
    Storage.remove(kLegacyPath);
    AgentLog::line("DECK", "migrated legacy cache to Pocket Daily");
  }
  AgentLog::line("DECK", "deck cache loaded: %u cards epoch=%lu", (unsigned)out.count, (unsigned long)out.savedEpoch);
  return true;
}

}  // namespace DeckStore
}  // namespace PocketDaily
