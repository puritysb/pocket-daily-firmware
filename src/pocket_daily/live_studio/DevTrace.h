#pragma once

#ifdef ENABLE_DEV_NETWORK_DIAGNOSTICS

#include <Arduino.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

// Dev-build-only in-RAM trace ring for the live-studio path. USB serial is
// not available on these readers, so stall diagnosis rides on this ring and
// the /api/pocket/v1/dev/live-debug dump instead. A 1 s heartbeat tag gives
// the 48-entry ring a ~48 s window - sized for the observed ~50 s stall.
namespace PocketDaily::DevTrace {

inline constexpr size_t CAPACITY = 48;

enum Tag : uint8_t {
  HEARTBEAT = 0,     // periodic 1 s loop heartbeat
  RENDER_START = 1,  // render task about to render
  RENDER_DONE = 2,   // render() returned (before capture)
  CAPTURE_ENTER = 3,
  CAPTURE_WRITE = 4,  // BMP write starting
  CAPTURE_DONE = 5,   // BMP write finished (aux = 1 on success)
  SEND_START = 6,     // live-studio WS send starting
  SEND_DONE = 7,      // send returned
  RENDER_REQ = 8,     // dev/render endpoint hit
  TICK = 9,           // push tick evaluated (aux = 1 when it sends)
};

struct Entry {
  uint32_t ms;
  uint8_t tag;
  uint16_t aux;
};

inline Entry gEntries[CAPACITY];
inline std::atomic<size_t> gNext{0};

inline void record(uint8_t tag, uint16_t aux = 0) {
  const size_t i = gNext.fetch_add(1, std::memory_order_relaxed) % CAPACITY;
  gEntries[i] = Entry{millis(), tag, aux};
}

}  // namespace PocketDaily::DevTrace

#define DEV_TRACE(tag, ...) PocketDaily::DevTrace::record(tag, ##__VA_ARGS__)

#else

#define DEV_TRACE(tag, ...)

#endif  // ENABLE_DEV_NETWORK_DIAGNOSTICS
