#pragma once
#include <cstdint>
namespace PocketDaily::RTCProbe {
inline constexpr uint32_t MAGIC = 0x50445731;
inline constexpr uintptr_t TRACE_ADDRESS = 0x50001a80;
inline bool owned(const volatile uint32_t* trace, uint32_t run) {
  return run && trace[0] == MAGIC && trace[1] == run && trace[31] == 1 && trace[2] >= 1 && trace[2] <= 5;
}
inline bool permitsGPIOResume(const volatile uint32_t* trace, uint32_t run, bool deepSleep, bool gpioWake,
                              uint64_t gpioMask) {
  // The harness cannot substitute a timer or software reset for hardware GPIO.
  return owned(trace, run) && trace[2] == 2 && trace[3] == 8 && trace[5] == 4 && trace[6] == 8 && deepSleep &&
         gpioWake && gpioMask == 8;
}
}  // namespace PocketDaily::RTCProbe
