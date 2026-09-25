#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "HalStorage.h"
#include "src/pocket_daily/PocketGlance.h"
#include "src/pocket_daily/PocketGlanceStore.h"

using namespace PocketDaily;
using namespace PocketDaily::AppGlance;

namespace {
// What the companion (pocket-daily ReaderGlance.requestBody) sends: sorted
// keys, weather with a five-day forecast, and today's remaining events.
const std::string kValid =
    R"({"events":[{"endHm":"10:00","startHm":"09:30","title":"Team standup"},)"
    R"({"endHm":"","startHm":"","title":"서울 출장"}],)"
    R"("savedEpoch":1790350200,"schema":1,"syncedHm":"00:30","utcOffsetMinutes":540,)"
    R"("weather":{"code":61,"days":[)"
    R"({"code":61,"date":"2026-09-26","maxC":21,"minC":12,"rainProbability":80,"summary":"Rain"},)"
    R"({"code":0,"date":"2026-09-27","maxC":23,"minC":11,"rainProbability":10,"summary":"Clear"},)"
    R"({"code":3,"date":"2026-09-28","maxC":19,"minC":13,"rainProbability":-1,"summary":"Cloudy"},)"
    R"({"code":2,"date":"2026-09-29","maxC":22,"minC":10,"rainProbability":20,"summary":"Part cloudy"},)"
    R"({"code":85,"date":"2026-09-30","maxC":-2,"minC":-9,"rainProbability":100,"summary":"Snow shower"}],)"
    R"("place":"Seoul","rainEndHm":"17:00","rainProbability":80,"rainStartHm":"14:00","summary":"Rain",)"
    R"("tempC":18,"todayMaxC":21,"todayMinC":12}})";

bool parse(const std::string& json, Snapshot& out, std::string* reason = nullptr, bool* oom = nullptr) {
  const char* error = nullptr;
  bool outOfMemory = false;
  const bool ok = parseJson(json.data(), json.size(), out, error, outOfMemory);
  if (reason && error) *reason = error;
  if (oom) *oom = outOfMemory;
  return ok;
}

bool accepts(const std::string& json) {
  Snapshot s;
  return parse(json, s);
}

std::string replaced(std::string text, const std::string& from, const std::string& to) {
  const auto at = text.find(from);
  EXPECT_NE(at, std::string::npos) << from;
  if (at == std::string::npos) return text;
  return text.replace(at, from.size(), to);
}

// kValid with the weather object swapped for `weather`.
std::string withWeather(const std::string& weather) {
  const auto start = kValid.find(R"("weather":)");
  return kValid.substr(0, start) + R"("weather":)" + weather + "}";
}

Snapshot validSnapshot() {
  Snapshot s;
  EXPECT_TRUE(parse(kValid, s));
  return s;
}

std::vector<uint8_t> encoded(const Snapshot& s) {
  uint8_t bytes[RECORD_BYTES];
  EXPECT_TRUE(encodeRecord(s, bytes));
  return {bytes, bytes + RECORD_BYTES};
}

class GlanceStore : public ::testing::Test {
 protected:
  void SetUp() override { FakeSD::reset(); }
  void TearDown() override { FakeSD::reset(); }
};
}  // namespace

TEST(GlanceJson, ParsesTheCompanionDocument) {
  Snapshot s;
  std::string reason;
  ASSERT_TRUE(parse(kValid, s, &reason)) << reason;
  EXPECT_EQ(s.savedEpoch, 1790350200u);
  EXPECT_EQ(s.utcOffsetMinutes, 540);
  EXPECT_STREQ(s.syncedHm, "00:30");
  const Glance& g = s.glance;
  EXPECT_TRUE(g.valid);
  ASSERT_TRUE(g.weather.valid);
  EXPECT_STREQ(g.weather.place, "Seoul");
  EXPECT_EQ(g.weather.code, 61);
  EXPECT_EQ(g.weather.tempC, 18);
  EXPECT_STREQ(g.weather.summary, "Rain");
  EXPECT_EQ(g.weather.todayMinC, 12);
  EXPECT_EQ(g.weather.todayMaxC, 21);
  EXPECT_STREQ(g.weather.rainStartHm, "14:00");
  EXPECT_STREQ(g.weather.rainEndHm, "17:00");
  EXPECT_EQ(g.weather.rainProbability, 80);
  ASSERT_EQ(g.weather.dayCount, 5);
  EXPECT_STREQ(g.weather.days[3].summary, "Part cloudy");
  EXPECT_EQ(g.weather.days[4].minC, -9);
  EXPECT_EQ(g.weather.days[2].rainProbability, -1);
  // Tomorrow is the second forecast day.
  EXPECT_STREQ(g.weather.tomorrow.date, "2026-09-27");
  EXPECT_EQ(g.weather.tomorrow.maxC, 23);
  ASSERT_EQ(g.eventCount, 2);
  EXPECT_STREQ(g.events[0].startHm, "09:30");
  EXPECT_STREQ(g.events[1].title, "서울 출장");
  EXPECT_EQ(g.events[1].startHm[0], '\0');
  EXPECT_EQ(g.usageCount, 0);
  EXPECT_EQ(g.wrapupCount, 0);
}

TEST(GlanceJson, WeatherNullAndNoEventsIsAValidEmptyGlance) {
  Snapshot s;
  const std::string empty = R"({"schema":1,"savedEpoch":1790350200,"syncedHm":"07:40","utcOffsetMinutes":0,)"
                            R"("weather":null,"events":[]})";
  std::string reason;
  ASSERT_TRUE(parse(empty, s, &reason)) << reason;
  EXPECT_FALSE(s.glance.valid);
  EXPECT_FALSE(s.glance.weather.valid);
  EXPECT_EQ(s.glance.weather.tempC, GLANCE_TEMP_NONE);
  EXPECT_EQ(s.glance.eventCount, 0);

  const std::string eventsOnly = replaced(kValid, kValid.substr(kValid.find(R"("weather":)")), R"("weather":null})");
  ASSERT_TRUE(parse(eventsOnly, s, &reason)) << reason;
  EXPECT_TRUE(s.glance.valid);
  EXPECT_FALSE(s.glance.weather.valid);
  EXPECT_EQ(s.glance.eventCount, 2);
}

TEST(GlanceJson, AcceptsBoundaryValues) {
  // Exactly 23/11/48-byte strings, the range limits, empty days, a lone day.
  EXPECT_TRUE(accepts(replaced(kValid, R"("place":"Seoul")", R"("place":")" + std::string(23, 'p') + "\"")));
  EXPECT_TRUE(accepts(replaced(kValid, R"("place":"Seoul")", R"("place":"서울특별시 강남a")")));  // 23 bytes
  EXPECT_TRUE(accepts(replaced(kValid, R"("place":"Seoul")", R"("place":"")")));
  EXPECT_TRUE(accepts(replaced(kValid, R"("summary":"Rain",)", R"("summary":"",)")));
  EXPECT_TRUE(accepts(replaced(kValid, "Team standup", std::string(48, 't'))));
  EXPECT_TRUE(accepts(replaced(kValid, R"("tempC":18)", R"("tempC":-100)")));
  EXPECT_TRUE(accepts(replaced(kValid, R"("tempC":18)", R"("tempC":100)")));
  EXPECT_TRUE(accepts(replaced(kValid, R"("code":61,"days")", R"("code":-1,"days")")));
  EXPECT_TRUE(accepts(replaced(kValid, R"("code":61,"days")", R"("code":99,"days")")));
  EXPECT_TRUE(
      accepts(replaced(kValid, R"("rainProbability":80,"rainStartHm")", R"("rainProbability":-1,"rainStartHm")")));
  EXPECT_TRUE(accepts(replaced(kValid, R"("utcOffsetMinutes":540)", R"("utcOffsetMinutes":-720)")));
  EXPECT_TRUE(accepts(replaced(kValid, R"("utcOffsetMinutes":540)", R"("utcOffsetMinutes":840)")));
  EXPECT_TRUE(accepts(replaced(kValid, R"("savedEpoch":1790350200)", R"("savedEpoch":1700000000)")));
  EXPECT_TRUE(accepts(replaced(kValid, R"("savedEpoch":1790350200)", R"("savedEpoch":4294967295)")));
  EXPECT_TRUE(accepts(replaced(kValid, R"("rainEndHm":"17:00")", R"("rainEndHm":"")")));
  EXPECT_TRUE(accepts(replaced(kValid, R"("syncedHm":"00:30")", R"("syncedHm":"23:59")")));

  Snapshot s;
  const auto daysAt = kValid.find(R"("days":[)");
  const auto daysEnd = kValid.find("}],", daysAt) + 2;
  ASSERT_TRUE(parse(kValid.substr(0, daysAt) + R"("days":[])" + kValid.substr(daysEnd), s));
  EXPECT_EQ(s.glance.weather.dayCount, 0);
  EXPECT_EQ(s.glance.weather.tomorrow.date[0], '\0');
  const auto firstDayEnd = kValid.find("},", daysAt) + 1;
  ASSERT_TRUE(parse(kValid.substr(0, firstDayEnd) + "]" + kValid.substr(daysEnd), s));
  EXPECT_EQ(s.glance.weather.dayCount, 1);
  EXPECT_EQ(s.glance.weather.tomorrow.date[0], '\0');
}

TEST(GlanceJson, RejectsUnknownAndMissingKeys) {
  std::string reason;
  Snapshot s;
  EXPECT_FALSE(parse(replaced(kValid, R"("schema":1,)", R"("schema":1,"extra":true,)"), s, &reason));
  EXPECT_FALSE(reason.empty());
  EXPECT_FALSE(accepts(replaced(kValid, R"("schema":1,)", "")));
  EXPECT_FALSE(accepts(replaced(kValid, R"("syncedHm":"00:30",)", "")));
  EXPECT_FALSE(accepts(replaced(kValid, R"("place":"Seoul",)", R"("place":"Seoul","wind":3,)")));
  EXPECT_FALSE(accepts(replaced(kValid, R"("tempC":18,)", "")));
  EXPECT_FALSE(accepts(replaced(kValid, R"("summary":"Clear"})", R"("summary":"Clear","uv":1})")));
  EXPECT_FALSE(accepts(replaced(kValid, R"("title":"Team standup"})", R"("title":"Team standup","where":"A"})")));
  EXPECT_FALSE(accepts(replaced(kValid, R"("endHm":"10:00",)", "")));
  // Daemon-only glance blocks are not part of this document.
  EXPECT_FALSE(accepts(replaced(kValid, R"("schema":1,)", R"("schema":1,"usage":[],)")));
}

TEST(GlanceJson, RejectsWrongTypes) {
  EXPECT_FALSE(accepts("[]"));
  EXPECT_FALSE(accepts("not json"));
  EXPECT_FALSE(accepts(replaced(kValid, R"("schema":1,)", R"("schema":"1",)")));
  EXPECT_FALSE(accepts(replaced(kValid, R"("schema":1,)", R"("schema":1.0,)")));
  EXPECT_FALSE(accepts(replaced(kValid, R"("savedEpoch":1790350200)", R"("savedEpoch":"1790350200")")));
  EXPECT_FALSE(accepts(replaced(kValid, R"("savedEpoch":1790350200)", R"("savedEpoch":1790350200.5)")));
  EXPECT_FALSE(accepts(replaced(kValid, R"("tempC":18)", R"("tempC":18.5)")));
  EXPECT_FALSE(accepts(replaced(kValid, R"("tempC":18)", R"("tempC":"18")")));
  EXPECT_FALSE(accepts(replaced(kValid, R"("tempC":18)", R"("tempC":null)")));
  EXPECT_FALSE(accepts(replaced(kValid, R"("place":"Seoul")", R"("place":5)")));
  EXPECT_FALSE(accepts(withWeather("[]")));
  EXPECT_FALSE(accepts(withWeather("\"sunny\"")));
  EXPECT_FALSE(accepts(replaced(kValid, R"("events":[)", R"("events":{"a":[)")));
  EXPECT_FALSE(accepts(replaced(kValid, R"("utcOffsetMinutes":540)", R"("utcOffsetMinutes":true)")));
}

TEST(GlanceJson, RejectsOutOfRangeValues) {
  EXPECT_FALSE(accepts(replaced(kValid, R"("schema":1,)", R"("schema":2,)")));
  EXPECT_FALSE(accepts(replaced(kValid, R"("savedEpoch":1790350200)", R"("savedEpoch":1699999999)")));
  EXPECT_FALSE(accepts(replaced(kValid, R"("savedEpoch":1790350200)", R"("savedEpoch":-1)")));
  EXPECT_FALSE(accepts(replaced(kValid, R"("savedEpoch":1790350200)", R"("savedEpoch":4294967296)")));
  EXPECT_FALSE(accepts(replaced(kValid, R"("utcOffsetMinutes":540)", R"("utcOffsetMinutes":-721)")));
  EXPECT_FALSE(accepts(replaced(kValid, R"("utcOffsetMinutes":540)", R"("utcOffsetMinutes":841)")));
  EXPECT_FALSE(accepts(replaced(kValid, R"("tempC":18)", R"("tempC":101)")));
  EXPECT_FALSE(accepts(replaced(kValid, R"("todayMinC":12)", R"("todayMinC":-101)")));
  EXPECT_FALSE(accepts(replaced(kValid, R"("code":61,"days")", R"("code":100,"days")")));
  EXPECT_FALSE(accepts(replaced(kValid, R"("code":61,"days")", R"("code":-2,"days")")));
  EXPECT_FALSE(
      accepts(replaced(kValid, R"("rainProbability":80,"rainStartHm")", R"("rainProbability":101,"rainStartHm")")));
  EXPECT_FALSE(accepts(replaced(kValid, R"("rainProbability":-1,)", R"("rainProbability":-2,)")));
  EXPECT_FALSE(accepts(replaced(kValid, R"("maxC":-2,)", R"("maxC":128,)")));
}

TEST(GlanceJson, RejectsBadTimesAndDates) {
  EXPECT_FALSE(accepts(replaced(kValid, R"("syncedHm":"00:30")", R"("syncedHm":"")")));
  EXPECT_FALSE(accepts(replaced(kValid, R"("syncedHm":"00:30")", R"("syncedHm":"24:00")")));
  EXPECT_FALSE(accepts(replaced(kValid, R"("syncedHm":"00:30")", R"("syncedHm":"7:40")")));
  EXPECT_FALSE(accepts(replaced(kValid, R"("rainStartHm":"14:00")", R"("rainStartHm":"14:60")")));
  EXPECT_FALSE(accepts(replaced(kValid, R"("startHm":"09:30")", R"("startHm":"9:30am")")));
  EXPECT_FALSE(accepts(replaced(kValid, R"("date":"2026-09-26")", R"("date":"2026-9-26")")));
  EXPECT_FALSE(accepts(replaced(kValid, R"("date":"2026-09-26")", R"("date":"2026-13-01")")));
  EXPECT_FALSE(accepts(replaced(kValid, R"("date":"2026-09-26")", R"("date":"")")));
}

TEST(GlanceJson, RejectsOversizeAndInvalidText) {
  EXPECT_FALSE(accepts(replaced(kValid, R"("place":"Seoul")", R"("place":")" + std::string(24, 'p') + "\"")));
  EXPECT_FALSE(accepts(replaced(kValid, R"("place":"Seoul")", R"("place":"서울특별시 강남구")")));  // 25 bytes
  EXPECT_FALSE(accepts(replaced(kValid, R"("summary":"Rain",)", R"("summary":"Thunderstorm",)")));
  EXPECT_FALSE(accepts(replaced(kValid, R"("summary":"Clear")", R"("summary":"Mostly clear")")));
  EXPECT_FALSE(accepts(replaced(kValid, "Team standup", std::string(49, 't'))));
  // Event titles are required; text must be clean UTF-8 without controls.
  EXPECT_FALSE(accepts(replaced(kValid, R"("title":"Team standup")", R"("title":"")")));
  EXPECT_FALSE(accepts(replaced(kValid, "Team standup", R"(Team\nstandup)")));
  EXPECT_FALSE(accepts(replaced(kValid, "Team standup", R"(Team\tstandup)")));
  EXPECT_FALSE(accepts(replaced(kValid, "Team standup", R"(Team\u0000standup)")));
  EXPECT_FALSE(accepts(replaced(kValid, "Team standup", R"(Team\u0085standup)")));
  EXPECT_FALSE(accepts(replaced(kValid, "Team standup", "Team \xC0\xAF standup")));
  EXPECT_FALSE(accepts(replaced(kValid, "Team standup", "Team \xED\xA0\x80 standup")));
  EXPECT_FALSE(accepts(replaced(kValid, R"("place":"Seoul")", "\"place\":\"Seo\xFFul\"")));
}

TEST(GlanceJson, RejectsTooManyDaysOrEvents) {
  const auto daysAt = kValid.find(R"("days":[)") + 8;
  const auto firstDayEnd = kValid.find("},", daysAt) + 2;
  const std::string sixDays =
      kValid.substr(0, daysAt) + kValid.substr(daysAt, firstDayEnd - daysAt) + kValid.substr(daysAt);
  std::string reason;
  Snapshot s;
  EXPECT_FALSE(parse(sixDays, s, &reason));
  EXPECT_NE(reason.find("at most 5"), std::string::npos) << reason;

  const std::string event = R"({"endHm":"","startHm":"","title":"x"},)";
  const std::string three = replaced(kValid, R"("events":[)", R"("events":[)" + event);
  EXPECT_TRUE(accepts(three));
  EXPECT_FALSE(parse(replaced(three, R"("events":[)", R"("events":[)" + event), s, &reason));
  EXPECT_NE(reason.find("at most 3"), std::string::npos) << reason;
}

TEST(GlanceJson, RejectsOversizeBodies) {
  std::string padded = kValid;
  padded.insert(padded.size() - 1, std::string(MAX_BODY_BYTES, ' '));
  EXPECT_GT(padded.size(), MAX_BODY_BYTES);
  std::string reason;
  Snapshot s;
  EXPECT_FALSE(parse(padded, s, &reason));
  EXPECT_NE(reason.find("2048"), std::string::npos);
  EXPECT_FALSE(parse("", s, &reason));
  const char* error = nullptr;
  bool oom = false;
  EXPECT_FALSE(parseJson(nullptr, 10, s, error, oom));
}

TEST(GlanceJson, FailureClearsTheOutputSoNothingIsPartiallyApplied) {
  Snapshot s = validSnapshot();
  ASSERT_TRUE(s.glance.valid);
  EXPECT_FALSE(parse(replaced(kValid, R"("title":"Team standup")", R"("title":"")"), s));
  EXPECT_EQ(s.savedEpoch, 0u);
  EXPECT_FALSE(s.glance.valid);
  EXPECT_FALSE(s.glance.weather.valid);
  EXPECT_EQ(s.glance.eventCount, 0);
  EXPECT_EQ(s.syncedHm[0], '\0');
}

TEST(GlanceRecord, RoundTripsExactly) {
  const Snapshot s = validSnapshot();
  const auto bytes = encoded(s);
  ASSERT_EQ(bytes.size(), RECORD_BYTES);
  EXPECT_EQ(std::string(bytes.begin(), bytes.begin() + 4), "PDGL");
  Snapshot decoded;
  ASSERT_TRUE(decodeRecord(bytes.data(), bytes.size(), decoded));
  EXPECT_EQ(decoded.savedEpoch, s.savedEpoch);
  EXPECT_EQ(decoded.utcOffsetMinutes, s.utcOffsetMinutes);
  EXPECT_STREQ(decoded.syncedHm, s.syncedHm);
  EXPECT_EQ(decoded.glance.weather.dayCount, 5);
  EXPECT_STREQ(decoded.glance.weather.days[4].summary, "Snow shower");
  EXPECT_STREQ(decoded.glance.weather.tomorrow.date, "2026-09-27");
  EXPECT_STREQ(decoded.glance.events[1].title, "서울 출장");
  EXPECT_EQ(encoded(decoded), bytes);

  Snapshot empty;
  ASSERT_TRUE(parse(R"({"schema":1,"savedEpoch":1790350200,"syncedHm":"07:40","utcOffsetMinutes":-300,)"
                    R"("weather":null,"events":[]})",
                    empty));
  const auto emptyBytes = encoded(empty);
  ASSERT_TRUE(decodeRecord(emptyBytes.data(), emptyBytes.size(), decoded));
  EXPECT_FALSE(decoded.glance.valid);
  EXPECT_EQ(decoded.glance.weather.tempC, GLANCE_TEMP_NONE);
  EXPECT_EQ(decoded.utcOffsetMinutes, -300);
}

TEST(GlanceRecord, RejectsCorruptionAndNonCanonicalBytes) {
  const auto bytes = encoded(validSnapshot());
  Snapshot decoded;
  for (size_t i = 0; i < bytes.size(); i += 7) {
    auto corrupt = bytes;
    corrupt[i] ^= 0x40;
    EXPECT_FALSE(decodeRecord(corrupt.data(), corrupt.size(), decoded)) << i;
    EXPECT_FALSE(decoded.glance.valid);
    EXPECT_EQ(decoded.savedEpoch, 0u);
  }
  EXPECT_FALSE(decodeRecord(bytes.data(), bytes.size() - 1, decoded));
  EXPECT_FALSE(decodeRecord(nullptr, RECORD_BYTES, decoded));
  // Invalid snapshots are never encoded.
  Snapshot invalid = validSnapshot();
  invalid.glance.events[0].title[0] = '\0';
  uint8_t out[RECORD_BYTES];
  EXPECT_FALSE(encodeRecord(invalid, out));
  invalid = validSnapshot();
  invalid.glance.usageCount = 1;
  EXPECT_FALSE(encodeRecord(invalid, out));
}

TEST_F(GlanceStore, SavesAtomicallyAndReloads) {
  Snapshot missing = validSnapshot();
  EXPECT_FALSE(load(missing));
  EXPECT_EQ(missing.savedEpoch, 0u);

  const Snapshot s = validSnapshot();
  ASSERT_EQ(save(s), SaveResult::Ok);
  EXPECT_EQ(FakeSD::files.count(GLANCE_PATH), 1u);
  EXPECT_EQ(FakeSD::files.count(GLANCE_TEMP_PATH), 0u);
  EXPECT_EQ(FakeSD::files.count(GLANCE_BACKUP_PATH), 0u);
  Snapshot loaded;
  ASSERT_TRUE(load(loaded));
  EXPECT_EQ(encoded(loaded), encoded(s));

  // A second save replaces the first and leaves no temp or backup behind.
  Snapshot next;
  ASSERT_TRUE(parse(replaced(kValid, R"("savedEpoch":1790350200)", R"("savedEpoch":1790353800)"), next));
  ASSERT_EQ(save(next), SaveResult::Ok);
  ASSERT_TRUE(load(loaded));
  EXPECT_EQ(loaded.savedEpoch, 1790353800u);
  EXPECT_EQ(FakeSD::files.size(), 1u);
}

TEST_F(GlanceStore, FailedWritesKeepThePreviousGlance) {
  const Snapshot s = validSnapshot();
  ASSERT_EQ(save(s), SaveResult::Ok);
  Snapshot next;
  ASSERT_TRUE(parse(replaced(kValid, R"("savedEpoch":1790350200)", R"("savedEpoch":1790353800)"), next));

  FakeSD::corruptWrite = true;  // the read-back differs from what was encoded
  EXPECT_EQ(save(next), SaveResult::StorageError);
  FakeSD::corruptWrite = false;
  Snapshot loaded;
  ASSERT_TRUE(load(loaded));
  EXPECT_EQ(loaded.savedEpoch, s.savedEpoch);
  EXPECT_EQ(FakeSD::files.count(GLANCE_TEMP_PATH), 0u);

  FakeSD::writeBudget = 100;  // short write
  EXPECT_EQ(save(next), SaveResult::StorageError);
  FakeSD::writeBudget = std::numeric_limits<size_t>::max();
  ASSERT_TRUE(load(loaded));
  EXPECT_EQ(loaded.savedEpoch, s.savedEpoch);

  FakeSD::failRename = true;
  EXPECT_EQ(save(next), SaveResult::StorageError);
  FakeSD::failRename = false;
  ASSERT_TRUE(load(loaded));
  EXPECT_EQ(loaded.savedEpoch, s.savedEpoch);

  FakeSD::failWriteOpen = true;
  EXPECT_EQ(save(next), SaveResult::StorageError);
  FakeSD::failWriteOpen = false;
  ASSERT_TRUE(load(loaded));
  EXPECT_EQ(loaded.savedEpoch, s.savedEpoch);

  Snapshot invalid = next;
  invalid.savedEpoch = 5;
  EXPECT_EQ(save(invalid), SaveResult::Invalid);
  ASSERT_TRUE(load(loaded));
  EXPECT_EQ(loaded.savedEpoch, s.savedEpoch);
}

TEST_F(GlanceStore, CorruptActiveFileFallsBackToTheBackupOrNothing) {
  const Snapshot s = validSnapshot();
  ASSERT_EQ(save(s), SaveResult::Ok);
  // Power lost after the active record moved to the backup: the backup loads.
  FakeSD::files[GLANCE_BACKUP_PATH] = FakeSD::files[GLANCE_PATH];
  FakeSD::files[GLANCE_PATH][40] ^= 1;
  Snapshot loaded;
  ASSERT_TRUE(load(loaded));
  EXPECT_EQ(loaded.savedEpoch, s.savedEpoch);

  FakeSD::files.erase(GLANCE_BACKUP_PATH);
  EXPECT_FALSE(load(loaded));
  EXPECT_FALSE(loaded.glance.valid);
  FakeSD::files[GLANCE_PATH].resize(RECORD_BYTES - 1);
  EXPECT_FALSE(load(loaded));

  // A later good save replaces the corrupt file.
  ASSERT_EQ(save(s), SaveResult::Ok);
  ASSERT_TRUE(load(loaded));
  EXPECT_EQ(loaded.savedEpoch, s.savedEpoch);
}
