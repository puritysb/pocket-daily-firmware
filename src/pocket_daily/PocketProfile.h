#pragma once

#include <cstddef>
#include <cstdint>

// Pocket Daily profile v1 (docs/pocket-profile-v1.md): which Home items appear
// and in what order, the weather panel and next-event line, and the sleep frame.
// Pure model and codecs; storage lives in PocketProfileStore.
namespace PocketDaily::DailyProfile {
// Study is the companion's own cards ("My cards"); Word is the firmware's daily
// word from the built-in list or an SD learning pack. Without Word in the
// profile, Study falls back to the daily word when `dailyWord` is set (v1).
enum class HomeItem : uint8_t { Reading = 1, Study = 2, Provider = 3, Monitor = 4, Word = 5 };
enum class WeatherPanel : uint8_t { Bottom = 0, Top = 1, Off = 2 };
enum class SleepMode : uint8_t { Brief = 0, Reader = 1 };
// Card is the first of the companion's cards, shown whether or not a book is
// open (for example contact details for a lost reader, with its image).
enum class SleepSection : uint8_t { Reading = 1, Study = 2, Weather = 3, Today = 4, Card = 5 };
inline constexpr uint8_t HOME_ITEM_MAX_ID = 5;
inline constexpr uint8_t SLEEP_SECTION_MAX_ID = 5;

inline constexpr uint8_t HOME_ITEM_CAP = 4;
inline constexpr uint8_t SLEEP_SECTION_CAP = 4;

struct Profile {
  HomeItem homeItems[HOME_ITEM_CAP]{};
  uint8_t homeCount = 0;
  bool dailyWord = true;
  WeatherPanel weather = WeatherPanel::Bottom;
  bool nextEvent = true;
  SleepMode sleepMode = SleepMode::Brief;
  SleepSection sleepSections[SLEEP_SECTION_CAP]{};
  uint8_t sleepCount = 0;

  bool shows(HomeItem item) const;
  bool sleeps(SleepSection section) const;
};

// Exactly the pre-profile behaviour: reading, study, provider; weather below;
// next event on; Daily Brief with reading, study, weather, today.
Profile defaults();
bool operator==(const Profile& a, const Profile& b);
inline bool operator!=(const Profile& a, const Profile& b) { return !(a == b); }

// Ordered, distinct, known IDs; at least one Home item and one sleep section.
bool valid(const Profile& profile);

// Strict document parser: any unknown key, unknown ID, duplicate, wrong type or
// missing field rejects the whole document (no partial application). `error`
// receives a short static reason on failure.
bool parseJson(const char* json, size_t length, Profile& out, const char*& error);

// Response document with the stored generation and the accepted IDs, so the
// companion never guesses capabilities. Returns bytes written, 0 on overflow.
size_t writeJson(const Profile& profile, uint32_t generation, const char* deviceId, char* out, size_t cap);

// Persisted slot record ("PDP1", generation, payload, CRC32).
inline constexpr size_t RECORD_BYTES = 32;
bool encodeRecord(const Profile& profile, uint32_t generation, uint8_t (&bytes)[RECORD_BYTES]);
bool decodeRecord(const uint8_t* bytes, size_t size, Profile& profile, uint32_t& generation);
}  // namespace PocketDaily::DailyProfile
