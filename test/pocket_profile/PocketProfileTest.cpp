#include <gtest/gtest.h>

#include <string>

#include "HalStorage.h"
#include "src/pocket_daily/PocketProfile.h"
#include "src/pocket_daily/PocketProfileStore.h"

using namespace PocketDaily::DailyProfile;

namespace {
const std::string kValid =
    R"({"schema":1,"home":{"items":["monitor","reading","study"],"dailyWord":false,"weather":"top","nextEvent":false},)"
    R"("sleep":{"mode":"reader","sections":["weather","reading"]}})";

Profile parse(const std::string& json, bool& ok, std::string* reason = nullptr) {
  Profile profile;
  const char* error = nullptr;
  ok = parseJson(json.data(), json.size(), profile, error);
  if (reason && error) *reason = error;
  return profile;
}

std::string replaced(std::string text, const std::string& from, const std::string& to) {
  const auto at = text.find(from);
  EXPECT_NE(at, std::string::npos) << from;
  return text.replace(at, from.size(), to);
}
}  // namespace

TEST(PocketProfile, DefaultsShowReadingAndStudyWithoutRetiredItems) {
  const auto p = defaults();
  ASSERT_TRUE(valid(p));
  ASSERT_EQ(p.homeCount, 2);
  EXPECT_EQ(p.homeItems[0], HomeItem::Reading);
  EXPECT_EQ(p.homeItems[1], HomeItem::Study);
  EXPECT_FALSE(p.shows(HomeItem::Provider));
  EXPECT_FALSE(p.shows(HomeItem::Monitor));
  EXPECT_TRUE(p.dailyWord);
  EXPECT_EQ(p.weather, WeatherPanel::Bottom);
  EXPECT_TRUE(p.nextEvent);
  EXPECT_EQ(p.sleepMode, SleepMode::Brief);
  ASSERT_EQ(p.sleepCount, 4);
  EXPECT_EQ(p.sleepSections[3], SleepSection::Today);
}

TEST(PocketProfile, ParsesAFullDocumentInOrder) {
  bool ok = false;
  const auto p = parse(kValid, ok);
  ASSERT_TRUE(ok);
  ASSERT_EQ(p.homeCount, 3);
  EXPECT_EQ(p.homeItems[0], HomeItem::Monitor);
  EXPECT_EQ(p.homeItems[1], HomeItem::Reading);
  EXPECT_FALSE(p.shows(HomeItem::Provider));
  EXPECT_FALSE(p.dailyWord);
  EXPECT_EQ(p.weather, WeatherPanel::Top);
  EXPECT_FALSE(p.nextEvent);
  EXPECT_EQ(p.sleepMode, SleepMode::Reader);
  ASSERT_EQ(p.sleepCount, 2);
  EXPECT_EQ(p.sleepSections[0], SleepSection::Weather);
}

TEST(PocketProfile, AcceptsTheDailyWordItemAndThePinnedCardSection) {
  bool ok = false;
  const auto p = parse(replaced(replaced(kValid, R"("monitor","reading","study")", R"("word","study")"),
                                R"(["weather","reading"])", R"(["card","reading"])"),
                       ok);
  ASSERT_TRUE(ok);
  EXPECT_EQ(p.homeItems[0], HomeItem::Word);
  EXPECT_EQ(p.sleepSections[0], SleepSection::Card);
  uint8_t bytes[RECORD_BYTES];
  ASSERT_TRUE(encodeRecord(p, 2, bytes));
  Profile decoded;
  uint32_t generation = 0;
  ASSERT_TRUE(decodeRecord(bytes, sizeof(bytes), decoded, generation));
  EXPECT_EQ(decoded, p);
  char out[1024];
  const std::string json(out, writeJson(p, 2, "5B09AF70", out, sizeof(out)));
  EXPECT_NE(json.find(R"("items":["word","study"])"), std::string::npos);
  EXPECT_NE(json.find(R"("homeItems":["reading","study","word"])"), std::string::npos);
  EXPECT_NE(json.find(R"("sleepSections":["reading","study","weather","today","card"])"), std::string::npos);
  // Still at most four Home items and four sleep sections.
  EXPECT_FALSE(
      parse(replaced(kValid, R"("monitor","reading","study")", R"("monitor","reading","study","provider","word")"), ok)
          .homeCount);
}

TEST(PocketProfile, RejectsAnyInvalidDocumentWithoutPartialResult) {
  const std::string cases[] = {
      replaced(kValid, R"("schema":1)", R"("schema":2)"),
      replaced(kValid, R"("schema":1,)", R"("schema":1,"extra":true,)"),
      replaced(kValid, R"(,"sleep":{"mode":"reader","sections":["weather","reading"]})", ""),
      replaced(kValid, R"("monitor")", R"("clock")"),
      replaced(kValid, R"("monitor","reading","study")", R"("reading","reading")"),
      replaced(kValid, R"("monitor","reading","study")", ""),
      replaced(kValid, R"("monitor","reading","study")", R"("monitor","reading","study","provider","monitor")"),
      replaced(kValid, R"("weather":"top")", R"("weather":1)"),
      replaced(kValid, R"("weather":"top")", R"("weather":"left")"),
      replaced(kValid, R"("nextEvent":false)", R"("nextEvent":"no")"),
      replaced(kValid, R"("dailyWord":false,)", ""),
      replaced(kValid, R"("nextEvent":false)", R"("nextEvent":false,"clock":true)"),
      replaced(kValid, R"("mode":"reader")", R"("mode":"blank")"),
      replaced(kValid, R"(["weather","reading"])", "[]"),
      replaced(kValid, R"(["weather","reading"])", R"(["weather","weather"])"),
      "[]",
      "{",
      "",
  };
  for (const auto& json : cases) {
    bool ok = true;
    std::string reason;
    parse(json, ok, &reason);
    EXPECT_FALSE(ok) << json;
    EXPECT_FALSE(reason.empty()) << json;
  }
}

TEST(PocketProfile, ResponseRoundTripsAndAdvertisesAcceptedIds) {
  bool ok = false;
  const auto p = parse(kValid, ok);
  ASSERT_TRUE(ok);
  char out[1024];
  const auto n = writeJson(p, 7, "5B09AF70", out, sizeof(out));
  ASSERT_GT(n, 0u);
  const std::string json(out, n);
  EXPECT_NE(json.find(R"("generation":7)"), std::string::npos);
  EXPECT_NE(json.find(R"("deviceID":"5B09AF70")"), std::string::npos);
  EXPECT_NE(json.find(R"("homeItems":["reading","study","word"])"), std::string::npos);
  EXPECT_NE(json.find(R"("maxHomeItems":4)"), std::string::npos);
  // The profile part of the response is itself an accepted document.
  const auto home = json.find(R"("home":)");
  const auto caps = json.find(R"(,"capabilities")");
  const std::string document = R"({"schema":1,)" + json.substr(home, caps - home) + "}";
  bool again = false;
  EXPECT_EQ(parse(document, again), p);
  EXPECT_TRUE(again);
  char tiny[64];
  EXPECT_EQ(writeJson(p, 7, "5B09AF70", tiny, sizeof(tiny)), 0u);
}

TEST(PocketProfile, RetiredItemsStillParseAndRoundTripButAreNotAdvertised) {
  // provider/monitor were fed only by the AgentDeck daemon. Stored profiles and
  // documents naming them must keep loading; the reader simply shows nothing.
  bool ok = false;
  const auto p = parse(replaced(kValid, R"("monitor","reading","study")", R"("provider","monitor","reading")"), ok);
  ASSERT_TRUE(ok);
  ASSERT_EQ(p.homeCount, 3);
  EXPECT_EQ(p.homeItems[0], HomeItem::Provider);
  EXPECT_EQ(p.homeItems[1], HomeItem::Monitor);
  uint8_t bytes[RECORD_BYTES];
  ASSERT_TRUE(encodeRecord(p, 3, bytes));
  Profile decoded;
  uint32_t generation = 0;
  ASSERT_TRUE(decodeRecord(bytes, sizeof(bytes), decoded, generation));
  EXPECT_EQ(decoded, p);
  char out[1024];
  const std::string json(out, writeJson(p, 3, "5B09AF70", out, sizeof(out)));
  // The stored items are reported as stored; the capabilities omit them.
  EXPECT_NE(json.find(R"("items":["provider","monitor","reading"])"), std::string::npos) << json;
  const auto caps = json.find(R"("capabilities")");
  ASSERT_NE(caps, std::string::npos);
  EXPECT_EQ(json.find("provider", caps), std::string::npos) << json;
  EXPECT_EQ(json.find("monitor", caps), std::string::npos) << json;
}

TEST(PocketProfile, RecordRoundTripsAndRejectsCorruptionOrNonCanonicalBytes) {
  bool ok = false;
  const auto p = parse(kValid, ok);
  uint8_t bytes[RECORD_BYTES];
  ASSERT_TRUE(encodeRecord(p, 3, bytes));
  Profile decoded;
  uint32_t generation = 0;
  ASSERT_TRUE(decodeRecord(bytes, sizeof(bytes), decoded, generation));
  EXPECT_EQ(decoded, p);
  EXPECT_EQ(generation, 3u);
  EXPECT_FALSE(encodeRecord(p, 0, bytes));

  uint8_t flipped[RECORD_BYTES];
  memcpy(flipped, bytes, sizeof(bytes));
  flipped[10] ^= 1;  // item changed, CRC stale
  EXPECT_FALSE(decodeRecord(flipped, sizeof(flipped), decoded, generation));
  EXPECT_FALSE(decodeRecord(bytes, sizeof(bytes) - 1, decoded, generation));
  memcpy(flipped, bytes, sizeof(bytes));
  flipped[0] = 'X';
  EXPECT_FALSE(decodeRecord(flipped, sizeof(flipped), decoded, generation));
}

class PocketProfileStoreTest : public testing::Test {
 protected:
  void SetUp() override {
    FakeSD::reset();
    resetForTest();
  }
  Profile custom() {
    bool ok = false;
    return parse(kValid, ok);
  }
};

TEST_F(PocketProfileStoreTest, MissingSlotsBootToDefaults) {
  loadAtBoot();
  EXPECT_EQ(current(), defaults());
  EXPECT_EQ(generation(), 0u);
}

TEST_F(PocketProfileStoreTest, SavedProfileSurvivesRebootAndAlternatesSlots) {
  loadAtBoot();
  ASSERT_EQ(save(custom(), 0), SaveResult::Ok);
  EXPECT_EQ(generation(), 1u);
  EXPECT_TRUE(FakeSD::files.count(SLOT_PATHS[0]));
  auto second = custom();
  second.weather = WeatherPanel::Off;
  ASSERT_EQ(save(second, 1), SaveResult::Ok);
  EXPECT_TRUE(FakeSD::files.count(SLOT_PATHS[1]));
  resetForTest();
  loadAtBoot();
  EXPECT_EQ(current(), second);
  EXPECT_EQ(generation(), 2u);
}

TEST_F(PocketProfileStoreTest, StaleGenerationIsAConflictAndChangesNothing) {
  loadAtBoot();
  ASSERT_EQ(save(custom(), 0), SaveResult::Ok);
  EXPECT_EQ(save(defaults(), 0), SaveResult::Conflict);
  EXPECT_EQ(current(), custom());
  EXPECT_EQ(generation(), 1u);
}

TEST_F(PocketProfileStoreTest, FailedOrCorruptWriteKeepsTheProfileInUse) {
  loadAtBoot();
  ASSERT_EQ(save(custom(), 0), SaveResult::Ok);
  FakeSD::failWriteOpen = true;
  EXPECT_EQ(save(defaults(), 1), SaveResult::StorageError);
  FakeSD::failWriteOpen = false;
  FakeSD::corruptWrite = true;
  EXPECT_EQ(save(defaults(), 1), SaveResult::StorageError);
  FakeSD::corruptWrite = false;
  EXPECT_EQ(current(), custom());
  EXPECT_EQ(generation(), 1u);
  // The good slot was never the write target, so a reboot keeps it.
  resetForTest();
  loadAtBoot();
  EXPECT_EQ(current(), custom());
}

TEST_F(PocketProfileStoreTest, CorruptNewestSlotFallsBackToTheOlderOne) {
  loadAtBoot();
  ASSERT_EQ(save(custom(), 0), SaveResult::Ok);
  ASSERT_EQ(save(defaults(), 1), SaveResult::Ok);
  FakeSD::files[SLOT_PATHS[1]][12] ^= 0x40;
  resetForTest();
  loadAtBoot();
  EXPECT_EQ(current(), custom());
  EXPECT_EQ(generation(), 1u);
}

TEST_F(PocketProfileStoreTest, InvalidProfileIsRejectedBeforeStorage) {
  loadAtBoot();
  Profile empty = defaults();
  empty.homeCount = 0;
  EXPECT_EQ(save(empty, 0), SaveResult::Invalid);
  EXPECT_TRUE(FakeSD::files.empty());
}
