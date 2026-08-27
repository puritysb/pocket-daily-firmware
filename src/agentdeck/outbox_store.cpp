#include "outbox_store.h"

#include <HalStorage.h>

#include <cstring>

#include "agent/AgentLog.h"

namespace PocketDaily {
namespace OutboxStore {
namespace {

// Shares the hidden cache directory with deck_store / the OTA receiver.
constexpr const char* kDir = "/.crosspoint";
constexpr const char* kPath = "/.crosspoint/pocket-daily-outbox.bin";
constexpr const char* kTmpPath = "/.crosspoint/pocket-daily-outbox.tmp";
constexpr const char* kLegacyPath = "/.crosspoint/agentdeck-outbox.bin";

// Same fixed-layout header idiom as deck_store: recordSize doubles as the
// schema version, so any Record shape change invalidates old files.
struct Header {
  uint32_t magic;
  uint8_t version;
  uint8_t count;
  uint16_t recordSize;
};
static_assert(sizeof(Header) == 8, "header layout must stay stable on disk");

constexpr uint32_t kMagic = 0x314F4441;  // "ADO1" (LE)
constexpr uint8_t kVersion = 2;          // Pocket card_choice adds choiceId to Record

void terminateRecord(Record& r) {
  r.cardId[sizeof(r.cardId) - 1] = '\0';
  r.sessionId[sizeof(r.sessionId) - 1] = '\0';
  r.requestId[sizeof(r.requestId) - 1] = '\0';
  r.action[sizeof(r.action) - 1] = '\0';
  r.choiceId[sizeof(r.choiceId) - 1] = '\0';
  r.decision[sizeof(r.decision) - 1] = '\0';
  r.value[sizeof(r.value) - 1] = '\0';
  r.question[sizeof(r.question) - 1] = '\0';
}

}  // namespace

bool save(const Queue& q) {
  if (!Storage.ready()) return false;
  if (q.count == 0) {
    Storage.remove(kPath);
    return true;
  }
  Storage.mkdir(kDir);
  {
    HalFile f = Storage.open(kTmpPath, O_WRITE | O_CREAT | O_TRUNC);
    if (!f) return false;
    const Header h{kMagic, kVersion, q.count, (uint16_t)sizeof(Record)};
    if (f.write(&h, sizeof(h)) != sizeof(h)) return false;
    const size_t bytes = sizeof(Record) * q.count;
    if (f.write(q.records, bytes) != bytes) return false;
  }
  Storage.remove(kPath);  // FAT rename fails onto an existing file
  if (!Storage.rename(kTmpPath, kPath)) {
    AgentLog::line("OUTBOX", "outbox save rename failed");
    return false;
  }
  return true;
}

bool load(Queue& out) {
  memset(&out, 0, sizeof(out));
  if (!Storage.ready()) return false;
  const bool migrateLegacy = !Storage.exists(kPath) && Storage.exists(kLegacyPath);
  const char* path = migrateLegacy ? kLegacyPath : kPath;
  if (!Storage.exists(path)) return false;
  HalFile f = Storage.open(path, O_RDONLY);
  if (!f) return false;
  Header h{};
  if (f.read(&h, sizeof(h)) != (int)sizeof(h)) return false;
  if (h.magic != kMagic || h.version != kVersion || h.recordSize != sizeof(Record) || h.count > Queue::CAP) {
    AgentLog::line("OUTBOX", "outbox rejected (magic/version/shape mismatch)");
    return false;
  }
  const size_t bytes = sizeof(Record) * h.count;
  if (bytes && f.read(out.records, bytes) != (int)bytes) {
    memset(&out, 0, sizeof(out));
    return false;
  }
  for (uint8_t i = 0; i < h.count; i++) terminateRecord(out.records[i]);
  out.count = h.count;
  f.close();
  if (migrateLegacy) {
    // Never hand a legacy queue to the network until its Pocket Daily copy is
    // durable. Otherwise a transient migration failure followed by a
    // successful POST could resend the same choices on the next boot.
    if (!save(out)) {
      memset(&out, 0, sizeof(out));
      AgentLog::line("OUTBOX", "legacy queue migration deferred");
      return false;
    }
    Storage.remove(kLegacyPath);
    AgentLog::line("OUTBOX", "migrated legacy queue to Pocket Daily");
  }
  return true;
}

bool append(const Record& rec) {
  Queue q;
  load(q);  // missing/rejected file → empty queue, still appendable
  if (q.count >= Queue::CAP) {
    // Drop-oldest: a bounded queue that silently refuses new decisions would
    // invert the Outbox promise ("pressing is never blocked").
    memmove(&q.records[0], &q.records[1], sizeof(Record) * (Queue::CAP - 1));
    q.count = Queue::CAP - 1;
    AgentLog::line("OUTBOX", "outbox full — oldest decision dropped");
  }
  q.records[q.count] = rec;
  terminateRecord(q.records[q.count]);
  q.count++;
  const bool ok = save(q);
  if (ok) AgentLog::line("OUTBOX", "queued %s (%u pending)", rec.action, (unsigned)q.count);
  return ok;
}

uint8_t pendingCount() {
  Queue q;
  return load(q) ? q.count : 0;
}

}  // namespace OutboxStore
}  // namespace PocketDaily
