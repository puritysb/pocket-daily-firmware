#pragma once
#include <cstddef>
#include <cstdint>

namespace PocketDaily::DevCapture {
enum class WalkState : uint32_t {
  Absent,
  Running,
  Completed,
  BuildFailed,
  TimedOut,
  Interrupted,
  EndOfBook,
  CaptureFailed
};
inline const char* walkStateName(WalkState state) {
  switch (state) {
    case WalkState::Absent:
      return "absent";
    case WalkState::Running:
      return "running";
    case WalkState::Completed:
      return "completed";
    case WalkState::BuildFailed:
      return "build-failed";
    case WalkState::TimedOut:
      return "timed-out";
    case WalkState::Interrupted:
      return "interrupted";
    case WalkState::EndOfBook:
      return "end-of-book";
    case WalkState::CaptureFailed:
      return "capture-failed";
  }
  return "absent";
}
struct WalkEvidence {
  uint32_t run = 0;
  WalkState state = WalkState::Absent;
  uint32_t requested = 0, rendered = 0, turns = 0, spine = 0, page = 0, progress = 0;
  uint32_t step = 0, detail = 0, elapsed = 0, freeHeap = 0, largestBlock = 0, captureReady = 0;
};
inline constexpr uint32_t MAX_WALK_PAGES = 128;
inline constexpr uint32_t WALK_TIMEOUT_MS = 600000;
inline constexpr uint32_t WALK_PAGE_TIMEOUT_MS = 45000;
inline constexpr bool validWalkPages(uint32_t pages) { return pages > 0 && pages <= MAX_WALK_PAGES; }
inline bool canBeginWalk(uint32_t run, uint32_t pages, const WalkEvidence& evidence) {
  return run && validWalkPages(pages) && evidence.run == run && evidence.requested == pages &&
         evidence.state == WalkState::Running;
}

// Only completed page paints reach observe(). Requests and repeated paints never
// count as progress. No page list or per-turn allocation is needed.
class ReaderWalk {
 public:
  WalkEvidence evidence;
  bool begin(uint32_t run, uint32_t pages, uint32_t now) {
    if (!run || !validWalkPages(pages)) return false;
    evidence = {};
    evidence.run = run;
    evidence.requested = pages;
    evidence.state = WalkState::Running;
    started = lastPageAt = now;
    awaiting = true;
    return true;
  }
  void observe(uint32_t spine, uint32_t page, uint32_t progress, uint32_t now) {
    tick(now);
    if (evidence.state != WalkState::Running || !awaiting || !page ||
        (evidence.rendered && (spine < evidence.spine || (spine == evidence.spine && page <= evidence.page))))
      return;
    evidence.spine = spine;
    evidence.page = page;
    evidence.progress = progress;
    ++evidence.rendered;
    evidence.elapsed = now - started;
    lastPageAt = now;
    awaiting = false;
    if (evidence.rendered == evidence.requested) evidence.state = WalkState::Completed;
  }
  bool takeTurn(uint32_t now) {
    tick(now);
    if (evidence.state != WalkState::Running || awaiting || now - lastPageAt < 1000) return false;
    awaiting = true;
    ++evidence.turns;
    return true;
  }
  void turnResult(bool moved) {
    if (evidence.state == WalkState::Running && awaiting && !moved) {
      if (evidence.turns) --evidence.turns;
      evidence.state = WalkState::EndOfBook;
    }
  }
  void fail(uint32_t step, uint32_t detail, uint32_t freeHeap, uint32_t block, uint32_t now) {
    evidence.state = WalkState::BuildFailed;
    evidence.step = step;
    evidence.detail = detail;
    evidence.freeHeap = freeHeap;
    evidence.largestBlock = block;
    evidence.elapsed = now - started;
  }
  void tick(uint32_t now) {
    if (evidence.state != WalkState::Running) return;
    evidence.elapsed = now - started;
    if (now - started >= WALK_TIMEOUT_MS || now - lastPageAt >= WALK_PAGE_TIMEOUT_MS)
      evidence.state = WalkState::TimedOut;
  }

 private:
  uint32_t started = 0, lastPageAt = 0;
  bool awaiting = false;
};

static_assert(sizeof(ReaderWalk) <= 72, "developer walk state must remain bounded");
inline constexpr size_t WALK_RECORD_BYTES = 64;
inline uint32_t walkChecksum(const uint8_t* bytes, size_t size) {
  uint32_t result = 2166136261u;
  for (size_t i = 0; i < size; ++i) result = (result ^ bytes[i]) * 16777619u;
  return result;
}
inline void encodeWalk(const WalkEvidence& e, uint8_t (&out)[WALK_RECORD_BYTES]) {
  const uint32_t fields[] = {0x31574450,  e.run,          static_cast<uint32_t>(e.state),
                             e.requested, e.rendered,     e.turns,
                             e.spine,     e.page,         e.progress,
                             e.step,      e.detail,       e.elapsed,
                             e.freeHeap,  e.largestBlock, e.captureReady};
  for (size_t i = 0; i < 15; ++i)
    for (size_t b = 0; b < 4; ++b) out[i * 4 + b] = static_cast<uint8_t>(fields[i] >> (b * 8));
  const uint32_t check = walkChecksum(out, 60);
  for (size_t b = 0; b < 4; ++b) out[60 + b] = static_cast<uint8_t>(check >> (b * 8));
}
inline bool decodeWalk(const uint8_t* bytes, size_t size, WalkEvidence& e) {
  if (!bytes || size != WALK_RECORD_BYTES) return false;
  uint32_t f[16]{};
  for (size_t i = 0; i < 16; ++i)
    for (size_t b = 0; b < 4; ++b) f[i] |= static_cast<uint32_t>(bytes[i * 4 + b]) << (b * 8);
  if (f[0] != 0x31574450 || !f[1] || f[2] < 1 || f[2] > static_cast<uint32_t>(WalkState::CaptureFailed) ||
      !validWalkPages(f[3]) || f[4] > f[3] || f[5] >= f[3] || f[8] > 100 || f[14] > 1 ||
      f[15] != walkChecksum(bytes, 60) || (f[4] && !f[7]) ||
      (f[2] == static_cast<uint32_t>(WalkState::Completed) && (f[4] != f[3] || f[5] + 1 != f[4] || f[14] != 1)))
    return false;
  e = {f[1], static_cast<WalkState>(f[2]), f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14]};
  return true;
}
}  // namespace PocketDaily::DevCapture
