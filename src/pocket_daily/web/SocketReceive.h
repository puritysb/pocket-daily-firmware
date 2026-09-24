#pragma once

#include <cerrno>
#include <cstddef>

#ifdef ARDUINO
#include <lwip/sockets.h>
#else
#include <sys/socket.h>
#endif

namespace PocketDaily::Web {
enum class ReceiveState { Data, Pending, Closed, Failed };
struct ReceiveResult {
  ReceiveState state;
  size_t count;
};

// Never allocates or waits. A zero-byte recv is orderly EOF, not "connected".
// The upload plane exclusively owns reads on this descriptor: do not mix this
// with NetworkClient::read/peek/clear, whose private buffer could retain bytes.
inline ReceiveResult receiveSocket(const int fd, void* out, const size_t capacity, const bool peek = false) {
  const auto count = recv(fd, out, capacity, MSG_DONTWAIT | (peek ? MSG_PEEK : 0));
  if (count > 0) return {ReceiveState::Data, static_cast<size_t>(count)};
  if (count == 0) return {ReceiveState::Closed, 0};
  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return {ReceiveState::Pending, 0};
  return {ReceiveState::Failed, 0};
}
}  // namespace PocketDaily::Web
