#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "HalStorage.h"
#include "pocket_daily/PocketAgendaStore.h"
using namespace PocketDaily::Agenda;
namespace {
const char* fixture =
    R"({"schema":1,"days":[{"date":"2028-02-28","events":[],"omittedEventCount":0},{"date":"2028-02-29","events":[{"startHm":"09:30","endHm":"10:45","title":"Leap meeting"},{"startHm":"","endHm":"","title":"휴일"}],"omittedEventCount":4},{"date":"2028-03-01","events":[{"startHm":"13:00","endHm":"15:00","title":"Reading"}],"omittedEventCount":0}]})";
bool parse(const std::string& json, Snapshot& s) {
  const char* e = nullptr;
  bool oom = false;
  return parseJson(json.data(), json.size(), s, e, oom);
}
}  // namespace
TEST(PocketAgenda, IndependentDatesAndEventsRoundTripThroughStorageAndJson) {
  Snapshot s;
  ASSERT_TRUE(parse(fixture, s));
  ASSERT_EQ(s.dayCount, 3);
  EXPECT_EQ(s.days[0].eventCount, 0);
  EXPECT_EQ(s.days[1].date, 20280229u);
  EXPECT_STREQ(s.days[2].events[0].title, "Reading");
  uint8_t bytes[RECORD_BYTES];
  ASSERT_TRUE(encodeRecord(s, bytes));
  Snapshot restored;
  ASSERT_TRUE(decodeRecord(bytes, sizeof(bytes), restored));
  EXPECT_EQ(restored.days[1].omittedEventCount, 4);
  EXPECT_STREQ(restored.days[1].events[1].title, "휴일");
  char json[MAX_BODY_BYTES + 1];
  size_t size = 0;
  ASSERT_TRUE(writeJson(restored, json, sizeof(json), size));
  Snapshot again;
  ASSERT_TRUE(parse(std::string(json, size), again));
  EXPECT_STREQ(again.days[2].events[0].startHm, "13:00");
  FakeSD::reset();
  ASSERT_EQ(save(s), SaveResult::Ok);
  Snapshot loaded;
  ASSERT_TRUE(load(loaded));
  EXPECT_STREQ(loaded.days[2].events[0].title, "Reading");
}
TEST(PocketAgenda, MalformedAndNoncanonicalInputsFailClosed) {
  for (
      const auto* bad :
      {R"({"schema":2,"days":[]})", R"({"schema":1,"days":[],"extra":0})",
       R"({"schema":1,"days":[{"date":"2027-02-29","events":[],"omittedEventCount":0}]})",
       R"({"schema":1,"days":[{"date":"2028-02-29","events":[],"omittedEventCount":-1}]})",
       R"({"schema":1,"days":[{"date":"2028-02-29","events":[{"startHm":"13:00","endHm":"12:00","title":"bad"}],"omittedEventCount":0}]})"}) {
    Snapshot s;
    ASSERT_TRUE(parse(fixture, s));
    EXPECT_FALSE(parse(bad, s));
    EXPECT_EQ(s.dayCount, 0);
  }
  Snapshot s;
  ASSERT_TRUE(parse(fixture, s));
  s.days[2].date = s.days[1].date;
  EXPECT_FALSE(valid(s));
  s.clear();
  s.days[6].events[2].title[1] = 'x';
  EXPECT_FALSE(valid(s));
  ASSERT_TRUE(parse(fixture, s));
  uint8_t bytes[RECORD_BYTES];
  ASSERT_TRUE(encodeRecord(s, bytes));
  bytes[80] ^= 1;
  EXPECT_FALSE(decodeRecord(bytes, sizeof(bytes), s));
  EXPECT_EQ(s.dayCount, 0);
}
TEST(PocketAgenda, FailedPublicationPreservesPriorRecordAndEmptyClears) {
  FakeSD::reset();
  Snapshot original;
  ASSERT_TRUE(parse(fixture, original));
  ASSERT_EQ(save(original), SaveResult::Ok);
  Snapshot changed = original;
  strcpy(changed.days[2].events[0].title, "Changed");
  FakeSD::writeBudget = 10;
  EXPECT_EQ(save(changed), SaveResult::StorageError);
  FakeSD::writeBudget = SIZE_MAX;
  Snapshot loaded;
  ASSERT_TRUE(load(loaded));
  EXPECT_STREQ(loaded.days[2].events[0].title, "Reading");
  FakeSD::failRename = true;
  EXPECT_EQ(save(changed), SaveResult::StorageError);
  FakeSD::failRename = false;
  ASSERT_TRUE(load(loaded));
  EXPECT_STREQ(loaded.days[2].events[0].title, "Reading");
  FakeSD::files[AGENDA_BACKUP_PATH] = FakeSD::files[AGENDA_PATH];
  FakeSD::files[AGENDA_PATH][40] ^= 1;
  ASSERT_TRUE(load(loaded));
  EXPECT_EQ(loaded.days[1].date, 20280229u);
  Snapshot empty;
  ASSERT_EQ(save(empty), SaveResult::Ok);
  ASSERT_TRUE(load(loaded));
  EXPECT_EQ(loaded.dayCount, 0);
}
TEST(PocketAgenda, DayNavigationCrossesLeapMonthYearAndRangeBoundaries) {
  EXPECT_EQ(shiftDate(20280228, 1), 20280229u);
  EXPECT_EQ(shiftDate(20280229, 1), 20280301u);
  EXPECT_EQ(shiftDate(20280301, -1), 20280229u);
  EXPECT_EQ(shiftDate(21000301, -1), 21000228u);
  EXPECT_EQ(shiftDate(20261231, 1), 20270101u);
  EXPECT_EQ(shiftDate(20270101, -1), 20261231u);
  EXPECT_EQ(shiftDate(19700101, -1), 19700101u);
  EXPECT_EQ(shiftDate(99991231, 1), 99991231u);
  EXPECT_EQ(shiftDate(20270229, 1), 0u);
}

TEST(PocketAgenda, MaximumSevenDaysAndThreeEventsStayBounded) {
  Snapshot s;
  s.dayCount = DAY_CAP;
  uint32_t date = 20261228;
  for (uint8_t i = 0; i < DAY_CAP; ++i) {
    auto& day = s.days[i];
    day.date = date;
    date = shiftDate(date, 1);
    day.eventCount = PocketDaily::Glance::EVENT_CAP;
    for (uint8_t j = 0; j < day.eventCount; ++j) {
      snprintf(day.events[j].title, sizeof(day.events[j].title), "Day %u item %u", i, j);
      strcpy(day.events[j].startHm, "09:00");
      strcpy(day.events[j].endHm, "10:00");
    }
  }
  ASSERT_TRUE(valid(s));
  uint8_t bytes[RECORD_BYTES];
  ASSERT_TRUE(encodeRecord(s, bytes));
  Snapshot loaded;
  ASSERT_TRUE(decodeRecord(bytes, sizeof(bytes), loaded));
  ASSERT_NE(findDay(loaded, 20270103), nullptr);
  EXPECT_STREQ(findDay(loaded, 20270103)->events[2].title, "Day 6 item 2");
  EXPECT_EQ(findDay(loaded, 20270104), nullptr);
  s.dayCount = DAY_CAP + 1;
  EXPECT_FALSE(valid(s));
}

TEST(PocketAgenda, InvalidActivePreservesBackupAcrossFailedReplacement) {
  FakeSD::reset();
  Snapshot s;
  ASSERT_TRUE(parse(R"({"schema":1,"days":[{"date":"2028-02-29","events":[],"omittedEventCount":0}]})", s));
  ASSERT_EQ(save(s), SaveResult::Ok);
  FakeSD::files[AGENDA_BACKUP_PATH] = FakeSD::files[AGENDA_PATH];
  FakeSD::files[AGENDA_PATH][12] ^= 1;
  Snapshot empty;
  FakeSD::failRename = true;
  EXPECT_EQ(save(empty), SaveResult::StorageError);
  FakeSD::failRename = false;
  Snapshot out;
  ASSERT_TRUE(load(out));
  EXPECT_EQ(out.dayCount, 1);
  EXPECT_EQ(out.days[0].date, 20280229u);
}
