#include "PocketProfile.h"

#include <algorithm>
#include <bit>
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

bool SleepArtworkSelection::contains(const uint8_t id) const {
  return id < SLEEP_ARTWORK_COUNT && (bits[id / 8] & (1u << (id % 8)));
}

bool SleepArtworkSelection::insert(const uint8_t id) {
  if (id >= SLEEP_ARTWORK_COUNT || contains(id)) return false;
  bits[id / 8] |= static_cast<uint8_t>(1u << (id % 8));
  return true;
}

uint8_t SleepArtworkSelection::count() const {
  uint8_t total = 0;
  for (auto byte : bits) total += std::popcount(byte);
  return total;
}

uint8_t SleepArtworkSelection::at(uint8_t rank) const {
  for (uint8_t id = 0; id < SLEEP_ARTWORK_COUNT; ++id) {
    if (!contains(id)) continue;
    if (rank == 0) return id;
    --rank;
  }
  return NO_SLEEP_ARTWORK;
}

namespace {
SleepArtworkSelection visibleArtwork(const Profile& profile) {
  if (profile.sleepMode != SleepMode::Brief) return {};

  const bool legacy = profile.sleepCount && isSleepArtwork(profile.sleepSections[0]);
  auto stored = profile.sleepArtwork;
  if (legacy) {
    for (uint8_t i = 0; i < profile.sleepCount && i < SLEEP_SECTION_CAP; ++i) {
      stored.insert(static_cast<uint8_t>(profile.sleepSections[i]) - static_cast<uint8_t>(SleepSection::Woodcut));
    }
  }

  // Keep the original IDs readable in saved profiles. Skip artwork printed
  // with a sample date or time, and show Woodcut for a fully retired selection.
  constexpr uint8_t retired[] = {1, 2, 3, 19, 44};
  const bool hasArtwork = stored.count() > 0;
  for (auto id : retired) stored.bits[id / 8] &= static_cast<uint8_t>(~(1u << (id % 8)));
  if (hasArtwork && !stored.count()) stored.insert(0);
  return stored;
}
}  // namespace

uint8_t Profile::sleepArtworkCount() const { return visibleArtwork(*this).count(); }

uint8_t Profile::sleepArtworkAt(const uint8_t rank) const { return visibleArtwork(*this).at(rank); }

bool Profile::shows(const HomeItem item) const {
  for (uint8_t i = 0; i < homeCount; ++i)
    if (homeItems[i] == item) return true;
  return false;
}

bool Profile::usesRecentBooks() const {
  for (uint8_t i = 0; i < homeCount; ++i)
    if (DailyProfile::usesRecentBooks(homeItems[i])) return true;
  return false;
}

bool Profile::sleeps(const SleepSection section) const {
  for (uint8_t i = 0; i < sleepCount; ++i)
    if (sleepSections[i] == section) return true;
  return false;
}

Profile defaults() {
  Profile p;
  p.homeItems[0] = HomeItem::ReadingDashboard;
  p.homeCount = 1;
  p.sleepSections[0] = SleepSection::Reading;
  p.sleepCount = 1;
  return p;
}

bool operator==(const Profile& a, const Profile& b) {
  if (a.homeCount != b.homeCount || a.sleepCount != b.sleepCount || a.dailyWord != b.dailyWord ||
      a.weather != b.weather || a.nextEvent != b.nextEvent || a.sleepMode != b.sleepMode ||
      a.sleepArtwork != b.sleepArtwork)
    return false;
  for (uint8_t i = 0; i < a.homeCount && i < HOME_ITEM_CAP; ++i)
    if (a.homeItems[i] != b.homeItems[i]) return false;
  for (uint8_t i = 0; i < a.sleepCount && i < SLEEP_SECTION_CAP; ++i)
    if (a.sleepSections[i] != b.sleepSections[i]) return false;
  return true;
}

bool valid(const Profile& p) {
  if (!distinctKnown(p.homeItems, p.homeCount, HOME_ITEM_CAP, HOME_ITEM_MAX_ID)) return false;
  if (!distinctKnown(p.sleepSections, p.sleepCount, SLEEP_SECTION_CAP, SLEEP_SECTION_MAX_ID)) return false;
  const bool legacyArtwork = p.sleepMode == SleepMode::Brief && isSleepArtwork(p.sleepSections[0]);
  for (uint8_t i = 0; i < p.sleepCount; ++i)
    if (isSleepArtwork(p.sleepSections[i]) != legacyArtwork) return false;
  if (p.sleepArtwork.count() &&
      (p.sleepMode != SleepMode::Brief || p.sleepCount != 1 || p.sleepSections[0] != SleepSection::Reading))
    return false;
  unsigned clockPages = 0;
  unsigned readingPages = 0;
  for (uint8_t i = 0; i < p.homeCount; ++i)
    if (isReading(p.homeItems[i])) ++readingPages;
  for (uint8_t i = 0; i < p.homeCount; ++i)
    if (isDeskClock(p.homeItems[i])) ++clockPages;
  const bool validCompactClock =
      !p.shows(HomeItem::ClockCompact) || (readingPages == 1 && !p.shows(HomeItem::ReadingDashboard));
  return clockPages <= 1 && readingPages <= 1 && validCompactClock && static_cast<uint8_t>(p.weather) <= 2 &&
         static_cast<uint8_t>(p.sleepMode) <= 1;
}

bool encodeRecord(const Profile& p, const uint32_t generation, uint8_t (&bytes)[RECORD_BYTES]) {
  if (generation == 0 || !valid(p)) return false;
  memset(bytes, 0, RECORD_BYTES);
  memcpy(bytes, "PDP1", 4);
  put32(bytes + 4, generation);
  const bool catalogArtwork = p.sleepArtwork.count() > 0;
  // PDP1 version 2 reuses bytes 18..27 for the catalog selection.
  bytes[8] = catalogArtwork ? 2 : 1;
  bytes[9] = p.homeCount;
  for (uint8_t i = 0; i < p.homeCount; ++i) bytes[10 + i] = static_cast<uint8_t>(p.homeItems[i]);
  bytes[14] = static_cast<uint8_t>((p.dailyWord ? 1 : 0) | (p.nextEvent ? 2 : 0));
  bytes[15] = static_cast<uint8_t>(p.weather);
  bytes[16] = static_cast<uint8_t>(p.sleepMode);
  if (catalogArtwork) {
    std::copy(p.sleepArtwork.bits.begin(), p.sleepArtwork.bits.end(), bytes + 18);
  } else {
    bytes[17] = p.sleepCount;
    for (uint8_t i = 0; i < p.sleepCount; ++i) bytes[18 + i] = static_cast<uint8_t>(p.sleepSections[i]);
  }
  put32(bytes + 28, crc32(bytes, 28));
  return true;
}

bool decodeRecord(const uint8_t* bytes, const size_t size, Profile& profile, uint32_t& generation) {
  if (!bytes || size != RECORD_BYTES || memcmp(bytes, "PDP1", 4) != 0 || (bytes[8] != 1 && bytes[8] != 2) ||
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
  if (bytes[8] == 2) {
    p.sleepCount = 1;
    p.sleepSections[0] = SleepSection::Reading;
    std::copy_n(bytes + 18, p.sleepArtwork.bits.size(), p.sleepArtwork.bits.begin());
  } else {
    for (uint8_t i = 0; i < p.sleepCount; ++i) p.sleepSections[i] = static_cast<SleepSection>(bytes[18 + i]);
  }
  // Canonical form: unused bytes stay zero, so a record has one encoding.
  uint8_t canonical[RECORD_BYTES];
  const uint32_t gen = u32(bytes + 4);
  if (!valid(p) || !encodeRecord(p, gen, canonical) || memcmp(canonical, bytes, RECORD_BYTES) != 0) return false;
  profile = p;
  generation = gen;
  return true;
}
}  // namespace PocketDaily::DailyProfile
