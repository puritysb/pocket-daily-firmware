#include "PocketProfile.h"

#include <cstring>

#include "ContentChecksum.h"

namespace PocketDaily::DailyProfile {
namespace {
template <typename T>
bool distinctKnown(const T* values, uint8_t count, uint8_t cap, uint8_t maxValue) {
  if (count == 0 || count > cap) return false;
  for (uint8_t i = 0; i < count; ++i) {
    const auto v = static_cast<uint8_t>(values[i]);
    if (v < 1 || v > maxValue) return false;
    for (uint8_t j = 0; j < i; ++j)
      if (values[j] == values[i]) return false;
  }
  return true;
}

uint32_t u32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
void put32(uint8_t* p, uint32_t n) {
  for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(n >> (8 * i));
}
uint32_t crc32(const uint8_t* bytes, size_t size) { return ~Content::contentCrcUpdate(0xFFFFFFFFu, bytes, size); }
}  // namespace

bool Profile::shows(const HomeItem item) const {
  for (uint8_t i = 0; i < homeCount; ++i)
    if (homeItems[i] == item) return true;
  return false;
}

bool Profile::sleeps(const SleepSection section) const {
  for (uint8_t i = 0; i < sleepCount; ++i)
    if (sleepSections[i] == section) return true;
  return false;
}

Profile defaults() {
  Profile p;
  p.homeItems[0] = HomeItem::Reading;
  p.homeItems[1] = HomeItem::Study;
  p.homeItems[2] = HomeItem::Provider;
  p.homeCount = 3;
  p.sleepSections[0] = SleepSection::Reading;
  p.sleepSections[1] = SleepSection::Study;
  p.sleepSections[2] = SleepSection::Weather;
  p.sleepSections[3] = SleepSection::Today;
  p.sleepCount = 4;
  return p;
}

bool operator==(const Profile& a, const Profile& b) {
  if (a.homeCount != b.homeCount || a.sleepCount != b.sleepCount || a.dailyWord != b.dailyWord ||
      a.weather != b.weather || a.nextEvent != b.nextEvent || a.sleepMode != b.sleepMode)
    return false;
  for (uint8_t i = 0; i < a.homeCount && i < HOME_ITEM_CAP; ++i)
    if (a.homeItems[i] != b.homeItems[i]) return false;
  for (uint8_t i = 0; i < a.sleepCount && i < SLEEP_SECTION_CAP; ++i)
    if (a.sleepSections[i] != b.sleepSections[i]) return false;
  return true;
}

bool valid(const Profile& p) {
  return distinctKnown(p.homeItems, p.homeCount, HOME_ITEM_CAP, HOME_ITEM_MAX_ID) &&
         distinctKnown(p.sleepSections, p.sleepCount, SLEEP_SECTION_CAP, SLEEP_SECTION_MAX_ID) &&
         static_cast<uint8_t>(p.weather) <= 2 && static_cast<uint8_t>(p.sleepMode) <= 1;
}

bool encodeRecord(const Profile& p, const uint32_t generation, uint8_t (&bytes)[RECORD_BYTES]) {
  if (generation == 0 || !valid(p)) return false;
  memset(bytes, 0, RECORD_BYTES);
  memcpy(bytes, "PDP1", 4);
  put32(bytes + 4, generation);
  bytes[8] = 1;  // schema
  bytes[9] = p.homeCount;
  for (uint8_t i = 0; i < p.homeCount; ++i) bytes[10 + i] = static_cast<uint8_t>(p.homeItems[i]);
  bytes[14] = static_cast<uint8_t>((p.dailyWord ? 1 : 0) | (p.nextEvent ? 2 : 0));
  bytes[15] = static_cast<uint8_t>(p.weather);
  bytes[16] = static_cast<uint8_t>(p.sleepMode);
  bytes[17] = p.sleepCount;
  for (uint8_t i = 0; i < p.sleepCount; ++i) bytes[18 + i] = static_cast<uint8_t>(p.sleepSections[i]);
  put32(bytes + 28, crc32(bytes, 28));
  return true;
}

bool decodeRecord(const uint8_t* bytes, const size_t size, Profile& profile, uint32_t& generation) {
  if (!bytes || size != RECORD_BYTES || memcmp(bytes, "PDP1", 4) != 0 || bytes[8] != 1 ||
      u32(bytes + 28) != crc32(bytes, 28))
    return false;
  Profile p;
  p.homeCount = bytes[9];
  p.sleepCount = bytes[17];
  if (p.homeCount > HOME_ITEM_CAP || p.sleepCount > SLEEP_SECTION_CAP || (bytes[14] & ~3u) != 0) return false;
  for (uint8_t i = 0; i < p.homeCount; ++i) p.homeItems[i] = static_cast<HomeItem>(bytes[10 + i]);
  p.dailyWord = (bytes[14] & 1) != 0;
  p.nextEvent = (bytes[14] & 2) != 0;
  p.weather = static_cast<WeatherPanel>(bytes[15]);
  p.sleepMode = static_cast<SleepMode>(bytes[16]);
  for (uint8_t i = 0; i < p.sleepCount; ++i) p.sleepSections[i] = static_cast<SleepSection>(bytes[18 + i]);
  // Canonical form: unused bytes stay zero, so a record has one encoding.
  uint8_t canonical[RECORD_BYTES];
  const uint32_t gen = u32(bytes + 4);
  if (!valid(p) || !encodeRecord(p, gen, canonical) || memcmp(canonical, bytes, RECORD_BYTES) != 0) return false;
  profile = p;
  generation = gen;
  return true;
}
}  // namespace PocketDaily::DailyProfile
