#pragma once
#include <cstddef>
#include <cstdint>
namespace PocketDaily::TrialWake {
enum class Stage : uint32_t {
  Prepared = 1,
  WaitingIdle,
  SleepCommitted,
  ObservingResume,
  TurnPending,
  Complete,
  Failed
};
struct Record {
  uint32_t magic = 0x50445731, run = 0;
  Stage stage = Stage::Prepared;
  uint32_t timeoutMs = 0, idleMs = 0, resetReason = 0, wakeCause = 0;
  uint32_t positionMatched = 0, pageAdvanced = 0;
  uint32_t checksum = 0;
};
inline uint32_t checksum(const Record& r) {
  const auto* p = reinterpret_cast<const uint8_t*>(&r);
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < offsetof(Record, checksum); ++i) h = (h ^ p[i]) * 16777619u;
  return h;
}
inline bool valid(const Record& r) {
  return r.magic == 0x50445731 && r.run && static_cast<uint32_t>(r.stage) >= 1 && static_cast<uint32_t>(r.stage) <= 7 &&
         r.positionMatched <= 1 && r.pageAdvanced <= 1 && r.checksum == checksum(r);
}
inline bool permitsIdleObservation(bool readerReady, uint32_t timeoutMs, uint32_t idleMs) {
  return readerReady && timeoutMs == 60000 && idleMs >= timeoutMs;
}
inline bool permitsResume(const Record& r, uint32_t run, bool deepSleep, bool timer) {
  return valid(r) && r.run == run && r.stage == Stage::SleepCommitted && r.timeoutMs == 60000 &&
         r.idleMs >= r.timeoutMs && deepSleep && timer;
}
}  // namespace PocketDaily::TrialWake
