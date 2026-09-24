#include <HalStorage.h>
#include <gtest/gtest.h>

#include "src/agentdeck/deck_store.h"

namespace {
namespace Deck = PocketDaily::DeckStore;
constexpr const char* slot0 = "/.crosspoint/pocket-daily-deck.0.bin";
constexpr const char* slot1 = "/.crosspoint/pocket-daily-deck.1.bin";
constexpr const char* oldPath = "/.crosspoint/pocket-daily-deck.bin";

class DeckStoreTest : public testing::Test {
 protected:
  void SetUp() override {
    FakeSD::reset();
    first.glance.clear();
    first.pocketCount = 1;
    first.savedEpoch = 100;
    strcpy(first.pocketCards[0].title, "Original");
    second = first;
    second.savedEpoch = 200;
    strcpy(second.pocketCards[0].title, "Edited");
  }
  Deck::Snapshot first{};
  Deck::Snapshot second{};
  Deck::Snapshot loaded{};
};

TEST_F(DeckStoreTest, AlternatesSlotsAndLoadsNewest) {
  ASSERT_TRUE(Deck::save(first));
  const auto original = FakeSD::files.at(slot0);
  ASSERT_TRUE(Deck::save(second));
  EXPECT_EQ(original, FakeSD::files.at(slot0));
  ASSERT_TRUE(Deck::load(loaded));
  EXPECT_STREQ(loaded.pocketCards[0].title, "Edited");
  EXPECT_EQ(loaded.savedEpoch, 200u);
  ASSERT_TRUE(Deck::save(first));
  ASSERT_TRUE(Deck::load(loaded));
  EXPECT_STREQ(loaded.pocketCards[0].title, "Original");
}

TEST_F(DeckStoreTest, EveryInterruptedWritePreservesPreviousSnapshot) {
  ASSERT_TRUE(Deck::save(first));
  const auto original = FakeSD::files.at(slot0);
  for (size_t cutoff = 0; cutoff < original.size(); ++cutoff) {
    SCOPED_TRACE(cutoff);
    FakeSD::writeBudget = cutoff;
    EXPECT_FALSE(Deck::save(second));
    EXPECT_EQ(original, FakeSD::files.at(slot0));
    ASSERT_TRUE(Deck::load(loaded));
    EXPECT_STREQ(loaded.pocketCards[0].title, "Original");
  }
}

TEST_F(DeckStoreTest, CorruptionAndTrailingBytesFallBack) {
  ASSERT_TRUE(Deck::save(first));
  ASSERT_TRUE(Deck::save(second));
  const auto edited = FakeSD::files.at(slot1);
  for (size_t offset = 0; offset < edited.size(); ++offset) {
    SCOPED_TRACE(offset);
    FakeSD::files[slot1] = edited;
    FakeSD::files[slot1][offset] ^= 0x80;
    ASSERT_TRUE(Deck::load(loaded));
    EXPECT_STREQ(loaded.pocketCards[0].title, "Original");
  }
  FakeSD::files[slot1] = edited;
  FakeSD::files[slot1].push_back(0);
  ASSERT_TRUE(Deck::load(loaded));
  EXPECT_STREQ(loaded.pocketCards[0].title, "Original");
}

TEST_F(DeckStoreTest, InterruptedOverwriteOfOlderSlotPreservesNewest) {
  ASSERT_TRUE(Deck::save(first));
  ASSERT_TRUE(Deck::save(second));
  const auto newest = FakeSD::files.at(slot1);
  for (size_t cutoff = 0; cutoff < newest.size(); ++cutoff) {
    SCOPED_TRACE(cutoff);
    FakeSD::writeBudget = cutoff;
    EXPECT_FALSE(Deck::save(first));
    EXPECT_EQ(newest, FakeSD::files.at(slot1));
    ASSERT_TRUE(Deck::load(loaded));
    EXPECT_STREQ(loaded.pocketCards[0].title, "Edited");
  }
}

TEST_F(DeckStoreTest, CompletedWriteWithFailedReadbackIsResolvedByLoad) {
  FakeSD::readBudget = 0;
  EXPECT_FALSE(Deck::save(first));
  // A false return is not proof that no bytes committed. Once SD reads recover,
  // boot may select the complete checksummed slot; it must not erase it blindly.
  FakeSD::readBudget = std::numeric_limits<size_t>::max();
  ASSERT_TRUE(Deck::load(loaded));
  EXPECT_STREQ(loaded.pocketCards[0].title, "Original");
}

TEST_F(DeckStoreTest, ReadFailuresNeverAuthorizeOverwritingUnreadableSlot) {
  ASSERT_TRUE(Deck::save(first));
  ASSERT_TRUE(Deck::save(second));
  const auto before = FakeSD::files;
  FakeSD::failReadOpen = slot1;
  EXPECT_FALSE(Deck::save(first));
  EXPECT_EQ(before, FakeSD::files);
  ASSERT_TRUE(Deck::load(loaded));
  EXPECT_STREQ(loaded.pocketCards[0].title, "Original");
  FakeSD::failReadOpen.clear();
  FakeSD::readBudget = 0;
  EXPECT_FALSE(Deck::save(first));
  EXPECT_EQ(before, FakeSD::files);
}

TEST_F(DeckStoreTest, OpenFailureAndCorruptWritesAreRejected) {
  ASSERT_TRUE(Deck::save(first));
  FakeSD::failWriteOpen = true;
  EXPECT_FALSE(Deck::save(second));
  FakeSD::failWriteOpen = false;
  FakeSD::corruptWrite = true;
  EXPECT_FALSE(Deck::save(second));
  ASSERT_TRUE(Deck::load(loaded));
  EXPECT_STREQ(loaded.pocketCards[0].title, "Original");
}

TEST_F(DeckStoreTest, V6IsReadWithoutMutationAndSurvivesFirstSlotFailure) {
  ASSERT_TRUE(Deck::save(first));
  auto old = FakeSD::files.at(slot0);
  old[4] = 6;
  old.erase(old.begin() + 12, old.begin() + 16);  // v7 generation
  old.resize(old.size() - 4);                     // v7 CRC
  FakeSD::files.clear();
  FakeSD::files[oldPath] = old;
  ASSERT_TRUE(Deck::load(loaded));
  EXPECT_STREQ(loaded.pocketCards[0].title, "Original");
  EXPECT_EQ(FakeSD::files.size(), 1u);
  FakeSD::writeBudget = 30;
  EXPECT_FALSE(Deck::save(second));
  ASSERT_TRUE(Deck::load(loaded));
  EXPECT_STREQ(loaded.pocketCards[0].title, "Original");
  EXPECT_EQ(FakeSD::files.at(oldPath), old);
  FakeSD::writeBudget = std::numeric_limits<size_t>::max();
  ASSERT_TRUE(Deck::save(second));
  ASSERT_TRUE(Deck::load(loaded));
  EXPECT_STREQ(loaded.pocketCards[0].title, "Edited");
  EXPECT_EQ(FakeSD::files.at(oldPath), old);
}

TEST_F(DeckStoreTest, InvalidCountsCannotModifyStorage) {
  ASSERT_TRUE(Deck::save(first));
  const auto before = FakeSD::files;
  second.count = AgentDeckCfg::SESSIONS_CAP + 1;
  EXPECT_FALSE(Deck::save(second));
  second.count = 0;
  second.pocketCount = PocketDaily::CARD_CAP + 1;
  EXPECT_FALSE(Deck::save(second));
  EXPECT_EQ(before, FakeSD::files);
}

TEST_F(DeckStoreTest, MissingOrInvalidDataLeavesEmptySnapshot) {
  loaded = second;
  EXPECT_FALSE(Deck::load(loaded));
  EXPECT_EQ(loaded.pocketCount, 0);
  EXPECT_FALSE(loaded.glance.valid);
  FakeSD::files[slot0] = {1, 2, 3};
  loaded = second;
  EXPECT_FALSE(Deck::load(loaded));
  EXPECT_EQ(loaded.pocketCount, 0);
  FakeSD::available = false;
  EXPECT_FALSE(Deck::save(first));
  EXPECT_FALSE(Deck::load(loaded));
}

TEST_F(DeckStoreTest, FullPayloadAndEmptyDeckRoundTrip) {
  first.count = AgentDeckCfg::SESSIONS_CAP;
  first.pocketCount = PocketDaily::CARD_CAP;
  first.glance.valid = true;
  strcpy(first.glance.weather.place, "Seoul");
  strcpy(first.serverHm, "12:34");
  strcpy(first.deckSig, "abc123");
  for (auto& record : first.records) strcpy(record.activity, "Retained legacy display");
  first.pocketCards[2].choiceCount = 3;
  strcpy(first.pocketCards[2].choices[2].label, "Last choice");
  ASSERT_TRUE(Deck::save(first));
  ASSERT_TRUE(Deck::load(loaded));
  EXPECT_EQ(memcmp(&first, &loaded, sizeof(first)), 0);
  Deck::Snapshot empty{};
  empty.glance.clear();
  ASSERT_TRUE(Deck::save(empty));
  ASSERT_TRUE(Deck::load(loaded));
  EXPECT_EQ(loaded.pocketCount, 0);
  EXPECT_EQ(loaded.count, 0);
  EXPECT_FALSE(loaded.glance.valid);
}

TEST_F(DeckStoreTest, GenerationExhaustionNeverWrapsOrOverwrites) {
  ASSERT_TRUE(Deck::save(first));
  auto& file = FakeSD::files.at(slot0);
  std::fill(file.begin() + 12, file.begin() + 16, 0xFF);
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < file.size() - 4; ++i) {
    crc ^= file[i];
    for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0u);
  }
  crc ^= 0xFFFFFFFFu;
  memcpy(file.data() + file.size() - 4, &crc, sizeof(crc));
  const auto before = FakeSD::files;
  ASSERT_TRUE(Deck::load(loaded));
  EXPECT_FALSE(Deck::save(second));
  EXPECT_EQ(before, FakeSD::files);
}
}  // namespace
