#pragma once

#include <Memory.h>

#include <cstring>

namespace AgentDeck::Net {

// Caller serializes all operations. Volatile commands belong to one live
// connection, unlike the durable Pocket choice outbox on SD.
class OutboundQueue {
 public:
  static constexpr size_t CAPACITY = 6;
  static constexpr size_t LINE_BYTES = 200;
  enum class Result { Queued, Inactive, Invalid, Full, NoMemory };

  void beginSession() {
    endSession();
    active = true;
  }

  void endSession() {
    active = false;
    lines.reset();
    head = 0;
    count = 0;
  }

  Result push(const char* json) {
    if (!active) return Result::Inactive;
    if (!json || !json[0]) return Result::Invalid;
    size_t length = 0;
    while (length < LINE_BYTES && json[length]) ++length;
    if (length == LINE_BYTES) return Result::Invalid;
    if (count == CAPACITY) return Result::Full;
    // Allocate once per used connection, never per command. A permanent
    // 1200-byte buffer needlessly removes transfer headroom when this optional
    // provider is absent. Failure rejects the command; no truncated fallback.
    if (!lines) lines = makeUniqueNoThrow<char[]>(CAPACITY * LINE_BYTES);
    if (!lines) return Result::NoMemory;
    char* storage = lines.get();
    memcpy(storage + ((head + count) % CAPACITY) * LINE_BYTES, json, length + 1);
    ++count;
    return Result::Queued;
  }

  bool pop(char* output, size_t capacity) {
    if (!active || !count || !output || capacity < LINE_BYTES) return false;
    const char* storage = lines.get();
    memcpy(output, storage + head * LINE_BYTES, LINE_BYTES);
    head = (head + 1) % CAPACITY;
    --count;
    return true;
  }

  bool hasStorage() const { return lines != nullptr; }

 private:
  std::unique_ptr<char[]> lines;
  size_t head = 0;
  size_t count = 0;
  bool active = false;
};

}  // namespace AgentDeck::Net
