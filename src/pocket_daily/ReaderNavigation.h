#pragma once

#include <cstdint>

// A book's Back target and the enclosing shell are independent (Articles may
// be entered from either shell). Both must survive a sleep/reset boundary.
enum class ReaderReturn : uint8_t { Home, PocketDaily, Articles };
struct ReaderNavigation {
  ReaderReturn reader = ReaderReturn::Home;
  ReaderReturn shell = ReaderReturn::Home;
};
inline constexpr ReaderNavigation legacyReaderNavigation(bool pocket) {
  const auto shell = pocket ? ReaderReturn::PocketDaily : ReaderReturn::Home;
  return {shell, shell};
}
inline constexpr bool validReaderNavigation(uint8_t reader, uint8_t shell) {
  return reader <= static_cast<uint8_t>(ReaderReturn::Articles) &&
         shell <= static_cast<uint8_t>(ReaderReturn::PocketDaily);
}
// GPIO PowerButton can also describe a cold battery boot. A saved clean sleep
// or the legacy retained-frame marker, not the button alone, establishes resume.
inline constexpr bool hasSleepNavigation(bool cleanSleep, bool sleepWake, bool showBootScreen) {
  return cleanSleep || (sleepWake && !showBootScreen);
}

inline constexpr ReaderReturn wakeReaderOrigin(ReaderNavigation saved, bool sleptInReader) {
  return sleptInReader ? saved.reader : saved.shell;
}
