#pragma once
#include <cstdint>

namespace PocketDaily::Web::DevNetworkFault {
// Fixed one-shot evidence, no allocations or saved settings. Device code is
// compiled only in developer firmware; the policy also runs in host tests.
enum class Point : uint8_t { Receiving, Published };
enum class State : uint8_t { Idle, Armed, Offline, Recovering, Complete, Expired };
struct Trial {
  State state = State::Idle;
  Point point = Point::Receiving;
  uint32_t run = 0, size = 0, armedAt = 0, droppedAt = 0, restoredAt = 0, bytes = 0;
  uint32_t publications = 0;
  bool sawOffline = false;
  bool arm(uint32_t id, uint32_t length, Point where, uint32_t now) {
    if (!id || length < 131072 || length > 2097152 || state == State::Armed || state == State::Offline ||
        state == State::Recovering)
      return false;
    *this = Trial{};
    state = State::Armed;
    point = where;
    run = id;
    size = length;
    armedAt = now;
    return true;
  }
  bool trigger(Point where, uint32_t length, uint32_t received, uint32_t now) {
    if (state != State::Armed || point != where || size != length) return false;
    if (now - armedAt >= 60000) {
      state = State::Expired;
      return false;
    }
    if (where == Point::Receiving && (received < 65536 || received >= length)) return false;
    if (where == Point::Published && received != length) return false;
    state = State::Offline;
    droppedAt = now;
    bytes = received;
    return true;
  }
  bool tick(uint32_t now, bool connected) {
    if (state == State::Armed && now - armedAt >= 60000) state = State::Expired;
    if (state == State::Offline) {
      sawOffline = sawOffline || !connected;
      if (now - droppedAt >= 5000) {
        state = State::Recovering;
        return true;
      }
    } else if (state == State::Recovering && connected) {
      restoredAt = now;
      state = State::Complete;
    }
    return false;
  }
};
#ifdef ENABLE_DEV_REMOTE_FLASH
const Trial& evidence();
bool arm(uint32_t run, uint32_t size, Point point);
bool trigger(Point point, uint32_t size, uint32_t received);
void service();
void cancel();
#endif
}  // namespace PocketDaily::Web::DevNetworkFault
