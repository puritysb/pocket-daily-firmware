#include "ContentActiveStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

#include "ContentChecksum.h"

namespace PocketDaily::Content {
namespace {
constexpr const char* SLOTS[] = {"/.crosspoint/content-active.0", "/.crosspoint/content-active.1"};
constexpr size_t RECORD_BYTES = 76;
struct Record {
  uint32_t generation = 0;
  char revision[65]{};
};
enum class ReadResult { Missing, Invalid, Failed, Valid };
uint32_t u32(const uint8_t* bytes) {
  return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
}
void put32(uint8_t* bytes, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) bytes[i] = static_cast<uint8_t>(value >> (8 * i));
}

ReadResult readRecord(int slot, Record& record, bool requirePresent = false) {
  record = {};
  if (!requirePresent && !Storage.exists(SLOTS[slot])) return ReadResult::Missing;
  HalFile file = Storage.open(SLOTS[slot], O_RDONLY);
  if (!file) return ReadResult::Failed;
  if (file.size() != RECORD_BYTES) return ReadResult::Invalid;
  uint8_t bytes[RECORD_BYTES];
  if (file.read(bytes, sizeof(bytes)) != sizeof(bytes)) return ReadResult::Failed;
  if (memcmp(bytes, "PDCA", 4) || !u32(bytes + 4) ||
      u32(bytes + 72) != (contentCrcUpdate(UINT32_MAX, bytes, 72) ^ UINT32_MAX))
    return ReadResult::Invalid;
  memcpy(record.revision, bytes + 8, 64);
  if (!validRevision(record.revision)) return ReadResult::Invalid;
  record.generation = u32(bytes + 4);
  return ReadResult::Valid;
}

int newest(const Record (&records)[2], const ReadResult (&results)[2]) {
  if (results[0] != ReadResult::Valid && results[1] != ReadResult::Valid) return -1;
  return results[1] == ReadResult::Valid &&
                 (results[0] != ReadResult::Valid || records[1].generation > records[0].generation)
             ? 1
             : 0;
}

int recover(const Record (&records)[2], const ReadResult (&results)[2], uint16_t capabilities, ActiveRevision& active,
            void (*progress)()) {
  ReadResult candidates[] = {results[0], results[1]};
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    const int slot = newest(records, candidates);
    if (slot < 0) break;
    if (verifyRevision(records[slot].revision, capabilities, active.content, progress) == RevisionResult::Ok) {
      active.generation = records[slot].generation;
      memcpy(active.revision, records[slot].revision, sizeof(active.revision));
      return slot;
    }
    candidates[slot] = ReadResult::Invalid;
  }
  active = {};
  return -1;
}

ActiveResult writeRecord(int slot, uint32_t generation, const char* revision, bool createOnly) {
  uint8_t bytes[RECORD_BYTES]{};
  memcpy(bytes, "PDCA", 4);
  put32(bytes + 4, generation);
  memcpy(bytes + 8, revision, 64);
  put32(bytes + 72, contentCrcUpdate(UINT32_MAX, bytes, 72) ^ UINT32_MAX);
  if (!Storage.mkdir("/.crosspoint") && !Storage.exists("/.crosspoint")) return ActiveResult::WriteFailed;
  {
    // exists() cannot distinguish all I/O errors from absence. Never truncate
    // a record we did not actually inspect: exclusive creation fails if that
    // supposedly missing slot exists when the filesystem opens it.
    HalFile file = Storage.open(SLOTS[slot], O_WRITE | O_CREAT | (createOnly ? O_EXCL : O_TRUNC));
    if (!file || file.write(bytes, sizeof(bytes)) != sizeof(bytes)) return ActiveResult::WriteFailed;
  }
  Record readback;
  if (readRecord(slot, readback) != ReadResult::Valid || readback.generation != generation ||
      strcmp(readback.revision, revision) != 0)
    return ActiveResult::ReadbackFailed;
  return ActiveResult::Ok;
}

ActiveResult activate(const char* revision, uint16_t capabilities, ActiveRevision& active, void (*progress)(),
                      RetiredRevision* retired) {
  RevisionInfo candidate;
  if (verifyRevision(revision, capabilities, candidate, progress) != RevisionResult::Ok)
    return ActiveResult::VerifyFailed;
  Record records[2];
  const ReadResult results[] = {readRecord(0, records[0]), readRecord(1, records[1])};
  if (results[0] == ReadResult::Failed || results[1] == ReadResult::Failed) return ActiveResult::ReadFailed;
  const int latest = newest(records, results);
  const int protectedSlot = recover(records, results, capabilities, active, progress);
  // Existing metadata without any verifiable content may mean transient SD
  // read failure; don't guess which copy is disposable.
  if (latest >= 0 && protectedSlot < 0) return ActiveResult::VerifyFailed;
  if (protectedSlot >= 0 && strcmp(active.revision, revision) == 0) return ActiveResult::Ok;
  const uint32_t generation = latest < 0 ? 0 : records[latest].generation;
  if (generation == UINT32_MAX) return ActiveResult::GenerationExhausted;
  const int targetSlot = protectedSlot == 0 ? 1 : 0;
  const auto result = writeRecord(targetSlot, generation + 1, revision, results[targetSlot] == ReadResult::Missing);
  if (result != ActiveResult::Ok) return result;
  active.generation = generation + 1;
  memcpy(active.revision, revision, sizeof(active.revision));
  active.content = candidate;
  if (retired && results[targetSlot] == ReadResult::Valid &&
      strcmp(records[targetSlot].revision, active.revision) != 0 &&
      (protectedSlot < 0 || strcmp(records[targetSlot].revision, records[protectedSlot].revision) != 0)) {
    memcpy(retired->revision, records[targetSlot].revision, sizeof(retired->revision));
  }
  return ActiveResult::Ok;
}
}  // namespace

ActiveResult recoverActiveRevision(uint16_t capabilities, ActiveRevision& active, void (*progress)()) {
  active = {};
  if (!Storage.ready()) return ActiveResult::ReadFailed;
  Record records[2];
  const ReadResult results[] = {readRecord(0, records[0]), readRecord(1, records[1])};
  if (recover(records, results, capabilities, active, progress) >= 0) return ActiveResult::Ok;
  if (results[0] == ReadResult::Failed || results[1] == ReadResult::Failed) return ActiveResult::ReadFailed;
  return results[0] == ReadResult::Missing && results[1] == ReadResult::Missing ? ActiveResult::NoActive
                                                                                : ActiveResult::VerifyFailed;
}

ActiveResult activateRevision(const char* revision, uint16_t capabilities, ActiveRevision& active, void (*progress)(),
                              RetiredRevision* retired) {
  active = {};
  if (retired) *retired = {};
  const auto result = activate(revision, capabilities, active, progress, retired);
  if (result != ActiveResult::Ok) {
    active = {};
    LOG_ERR("CONTENT", "Activation failed: %u", static_cast<unsigned>(result));
  }
  return result;
}

RetirementResult retireRevision(const char* revision, const char* displayedRevision, uint16_t capabilities,
                                void (*progress)()) {
  if (!validRevision(revision)) return RetirementResult::InvalidRevision;
  if (!displayedRevision || (displayedRevision[0] && !validRevision(displayedRevision)))
    return RetirementResult::Unavailable;
  if (strcmp(revision, displayedRevision) == 0) return RetirementResult::Protected;
  if (!Storage.ready()) return RetirementResult::Unavailable;
  // Do not classify exists()==false as absence when authorizing destruction.
  // Even a genuinely missing/corrupt slot prevents automatic retirement.
  Record record;
  for (int slot = 0; slot < 2; ++slot) {
    if (readRecord(slot, record, true) != ReadResult::Valid) return RetirementResult::Unavailable;
    if (strcmp(record.revision, revision) == 0) return RetirementResult::Protected;
  }
  return removeRetiredRevisionFiles(revision, capabilities, progress);
}
}  // namespace PocketDaily::Content
