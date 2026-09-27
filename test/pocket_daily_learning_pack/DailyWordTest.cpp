#include <gtest/gtest.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "HalStorage.h"
#include "pocket_daily/home/DailyWord.h"
#include "pocket_daily/learning_pack.h"

// Light learning-pack access (one open, header shape and checksum, one
// record) and the daily word shared by Pocket Daily and the Sync presenter.
namespace {
using PocketDaily::LearningPack::Header;
using PocketDaily::LearningPack::Record;

uint32_t fnv32(const uint8_t* bytes, size_t length) {
  uint32_t hash = 2166136261U;
  for (size_t i = 0; i < length; ++i) {
    hash ^= bytes[i];
    hash *= 16777619U;
  }
  return hash;
}

std::vector<uint8_t> pack(uint32_t count, uint32_t contentVersion = 3) {
  Header header{};
  std::memcpy(header.magic, "PDLP", 4);
  header.formatVersion = PocketDaily::LearningPack::FORMAT_VERSION;
  header.headerSize = sizeof(Header);
  header.recordSize = sizeof(Record);
  header.recordCount = count;
  header.contentVersion = contentVersion;
  header.totalBytes = sizeof(Header) + count * sizeof(Record);
  std::strcpy(header.packageId, PocketDaily::LearningPack::PACKAGE_ID);
  std::strcpy(header.locale, "ko-KR");
  std::strcpy(header.title, "JLPT");
  std::strcpy(header.licenseSpdx, "CC-BY-4.0");
  std::strcpy(header.sourceRevision, "r1");
  std::strcpy(header.attribution, "KANJIDIC");
  // payloadSha256 stays zero: the light path deliberately does not hash.
  header.headerFnv32 = fnv32(reinterpret_cast<const uint8_t*>(&header), offsetof(Header, headerFnv32));
  std::vector<uint8_t> bytes(header.totalBytes);
  std::memcpy(bytes.data(), &header, sizeof(header));
  for (uint32_t i = 0; i < count; ++i) {
    Record record{};
    record.itemId = 0x100 + i;
    std::snprintf(record.glyph, sizeof(record.glyph), "%s", i % 2 ? "漢" : "字");
    std::snprintf(record.primaryWord, sizeof(record.primaryWord), "w%u", i);
    std::snprintf(record.wordReading, sizeof(record.wordReading), "r%u", i);
    std::snprintf(record.meaningEn, sizeof(record.meaningEn), "m%u", i);
    std::snprintf(record.example, sizeof(record.example), "e%u", i);
    std::memcpy(bytes.data() + sizeof(Header) + i * sizeof(Record), &record, sizeof(record));
  }
  return bytes;
}

class DailyWordTest : public testing::Test {
 protected:
  void SetUp() override { FakeSD::reset(); }
  void install(std::vector<uint8_t> bytes) { FakeSD::files[PocketDaily::LearningPack::PACK_PATH] = std::move(bytes); }
  Header header{};
  Record record{};
};

TEST_F(DailyWordTest, LightReadPicksTheDaysRecordAfterOneHeaderCheck) {
  install(pack(5, 9));
  ASSERT_TRUE(PocketDaily::LearningPack::readDayRecord(12, header, record));
  EXPECT_EQ(header.contentVersion, 9U);
  EXPECT_EQ(record.itemId, 0x100U + 12 % 5);
  EXPECT_STREQ(record.primaryWord, "w2");
  // Random access through the full-pack API still reads the same record.
  Record indexed{};
  ASSERT_TRUE(PocketDaily::LearningPack::readRecord(2, indexed));
  EXPECT_EQ(indexed.itemId, record.itemId);
  EXPECT_FALSE(PocketDaily::LearningPack::readRecord(5, indexed));
}

TEST_F(DailyWordTest, LightReadRejectsMissingMalformedOrTruncatedPacks) {
  EXPECT_FALSE(PocketDaily::LearningPack::readDayRecord(0, header, record));  // missing
  auto corrupt = pack(3);
  corrupt[offsetof(Header, title)] ^= 1;  // header checksum no longer matches
  install(corrupt);
  EXPECT_FALSE(PocketDaily::LearningPack::readDayRecord(0, header, record));
  auto truncated = pack(3);
  truncated.resize(truncated.size() - 1);  // totalBytes != file size
  install(truncated);
  EXPECT_FALSE(PocketDaily::LearningPack::readDayRecord(0, header, record));
  auto blank = pack(3);
  std::memset(blank.data() + sizeof(Header), 0, sizeof(Record));  // itemId 0, no glyph
  install(blank);
  EXPECT_FALSE(PocketDaily::LearningPack::readDayRecord(3, header, record));
  EXPECT_TRUE(PocketDaily::LearningPack::readDayRecord(4, header, record));
  FakeSD::readBudget = sizeof(Header) + 10;  // short record read
  EXPECT_FALSE(PocketDaily::LearningPack::readDayRecord(4, header, record));
}

TEST_F(DailyWordTest, LightDailyWordComposesThePackCardLikeTheReader) {
  install(pack(4, 7));
  PocketDaily::Card card{};
  ASSERT_EQ(PocketDaily::Home::lightDailyWord(6, card), PocketDaily::Home::WordSource::Pack);
  EXPECT_STREQ(card.cardId, "local:jp:7:00000102");
  EXPECT_STREQ(card.title, "今日の漢字");
  EXPECT_STREQ(card.question, "字");
  EXPECT_STREQ(card.context, "w2（r2） · m2 · e2");
  EXPECT_STREQ(card.module, "local");
  EXPECT_STREQ(card.actionClass, "day");
  ASSERT_EQ(card.choiceCount, 3);
  EXPECT_STREQ(card.choices[1].id, "next");
  EXPECT_STREQ(card.choices[2].label, "Known");
}

TEST_F(DailyWordTest, LightDailyWordFallsBackToTheBuiltInListOnlyWithoutAReadablePack) {
  PocketDaily::Card card{};
  ASSERT_EQ(PocketDaily::Home::lightDailyWord(16, card), PocketDaily::Home::WordSource::BuiltIn);
  EXPECT_STREQ(card.cardId, "local:jp:16:0");
  EXPECT_STREQ(card.title, "今日の単語");
  EXPECT_STREQ(card.question, "習慣（しゅうかん）");
  EXPECT_STREQ(card.context, "habit · 毎日、本を読む習慣をつける。");
  EXPECT_STREQ(card.module, "local");
  PocketDaily::Card offset{};
  EXPECT_EQ(PocketDaily::Home::builtInDailyWord(16, 1, offset), 1U);
  EXPECT_STREQ(offset.question, "続ける（つづける）");
}

TEST_F(DailyWordTest, DayTurnsOverAtTheCompanionsLocalMidnight) {
  constexpr uint32_t saved = 1790000000;  // after AppGlance::MIN_EPOCH
  // Unset clock: the glance's compose time is the lower bound.
  EXPECT_EQ(PocketDaily::Home::dailyWordEpoch(5, saved), saved);
  EXPECT_EQ(PocketDaily::Home::dailyWordEpoch(saved + 10, 0), saved + 10);
  EXPECT_EQ(PocketDaily::Home::dailyWordEpoch(5, 0), 0U);
  EXPECT_EQ(PocketDaily::Home::dailyWordDay(0, 540), 0U);
  const uint32_t utcMidnight = (saved / 86400) * 86400;
  EXPECT_EQ(PocketDaily::Home::dailyWordDay(utcMidnight - 1, 0), saved / 86400 - 1);
  // UTC+9: the local day starts nine hours before UTC midnight.
  EXPECT_EQ(PocketDaily::Home::dailyWordDay(utcMidnight - 9 * 3600, 540), saved / 86400);
  EXPECT_EQ(PocketDaily::Home::dailyWordDay(utcMidnight - 9 * 3600 - 1, 540), saved / 86400 - 1);
}
}  // namespace
