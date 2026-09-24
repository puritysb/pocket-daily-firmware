#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace PocketDaily::Web {

// Local counters only. No history, allocation, logging or network activity.
// Totals saturate rather than wrapping; individual clock intervals use unsigned
// subtraction (valid across one millis/micros wrap).
struct TransferTiming {
  uint32_t totalUs = 0;
  uint32_t maxUs = 0;
  uint32_t calls = 0;
  static uint32_t add(uint32_t a, uint32_t b) { return UINT32_MAX - a < b ? UINT32_MAX : a + b; }
  void record(uint32_t elapsed) {
    totalUs = add(totalUs, elapsed);
    maxUs = std::max(maxUs, elapsed);
    calls = add(calls, 1);
  }
};

struct TransferMetrics {
  enum class Outcome : uint8_t { None, Active, Complete, Interrupted, Rejected, Stopped };
  uint32_t attempt = 0;
  uint32_t expected = 0;
  uint32_t resumed = 0;
  uint32_t socketBytes = 0;
  uint32_t accepted = 0;
  uint32_t elapsedMs = 0;
  uint32_t maxServiceGapMs = 0;
  uint32_t maxReceiveGapMs = 0;
  uint32_t minHeap = 0;
  uint32_t minBlock = 0;
  TransferTiming sdWrite;
  TransferTiming sdClose;
  TransferTiming replyWrite;
  uint32_t startedAt = 0;
  uint32_t lastServiceAt = 0;
  uint32_t lastReceiveAt = 0;
  Outcome outcome = Outcome::None;
  bool payloadStarted = false;

  void start(uint32_t now, uint32_t heap, uint32_t block) {
    const auto next = TransferTiming::add(attempt, 1);
    *this = {};
    attempt = next;
    startedAt = lastServiceAt = now;
    minHeap = heap;
    minBlock = block;
    outcome = Outcome::Active;
  }
  void service(uint32_t now) {
    if (outcome != Outcome::Active) return;
    maxServiceGapMs = std::max(maxServiceGapMs, now - lastServiceAt);
    lastServiceAt = now;
  }
  void payload(uint32_t now, uint32_t size, uint32_t prefix) {
    expected = size;
    resumed = prefix;
    accepted = prefix;
    lastReceiveAt = now;
    payloadStarted = true;
  }
  void receive(uint32_t now, uint32_t bytes) {
    if (outcome != Outcome::Active || !payloadStarted) return;
    maxReceiveGapMs = std::max(maxReceiveGapMs, now - lastReceiveAt);
    lastReceiveAt = now;
    socketBytes = TransferTiming::add(socketBytes, bytes);
  }
  void heap(uint32_t free, uint32_t block) {
    minHeap = std::min(minHeap, free);
    minBlock = std::min(minBlock, block);
  }
  void end(uint32_t now, Outcome result, uint32_t prefix) {
    if (outcome != Outcome::Active) return;
    service(now);
    if (payloadStarted) maxReceiveGapMs = std::max(maxReceiveGapMs, now - lastReceiveAt);
    accepted = prefix;
    elapsedMs = now - startedAt;
    outcome = result;
  }
  // Three bounded JSON chunks, no Arduino/JSON heap allocations here. The
  // WebServer wrapper emits them only after transfer completion, never during it.
  size_t format(char* out, size_t capacity, unsigned section) const {
    int n = -1;
    if (section == 0) {
      n = snprintf(out, capacity,
                   "{\"schema\":1,\"attempt\":%u,\"outcome\":%u,\"expected\":%u,\"resumed\":%u,"
                   "\"socketBytes\":%u,\"accepted\":%u,\"elapsedMs\":%u,",
                   attempt, static_cast<unsigned>(outcome), expected, resumed, socketBytes, accepted, elapsedMs);
    } else if (section == 1) {
      n = snprintf(out, capacity,
                   "\"maxServiceGapMs\":%u,\"maxReceiveGapMs\":%u,\"minHeap\":%u,\"minBlock\":%u,"
                   "\"sdWriteUs\":[%u,%u,%u],",
                   maxServiceGapMs, maxReceiveGapMs, minHeap, minBlock, sdWrite.totalUs, sdWrite.maxUs, sdWrite.calls);
    } else if (section == 2) {
      n = snprintf(out, capacity, "\"sdCloseUs\":[%u,%u,%u],\"replyWriteUs\":[%u,%u,%u]}", sdClose.totalUs,
                   sdClose.maxUs, sdClose.calls, replyWrite.totalUs, replyWrite.maxUs, replyWrite.calls);
    }
    return n > 0 && static_cast<size_t>(n) < capacity ? static_cast<size_t>(n) : 0;
  }
};
static_assert(sizeof(TransferMetrics) <= 96, "Keep developer transfer counters bounded");
}  // namespace PocketDaily::Web
