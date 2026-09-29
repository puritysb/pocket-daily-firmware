#pragma once
// Host-only compatibility surface for the shared renderer.
#include <Logging.h>

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>

struct HostEsp {
  [[noreturn]] void restart() { throw std::runtime_error("Unexpected device restart"); }
};
inline HostEsp ESP;
