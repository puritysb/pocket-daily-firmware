#include <gtest/gtest.h>

#include "src/pocket_daily/CardSignature.h"

namespace {
constexpr uint32_t seed = 2166136261u;

TEST(CardSignature, EverySemanticFieldTriggersChange) {
  const PocketDaily::Card original{};
  const auto initial = PocketDaily::cardSignature(seed, original);
  PocketDaily::Card card{};
  char* fields[] = {card.cardId, card.module, card.actionClass, card.title, card.question, card.context};
  for (char* field : fields) {
    field[0] = 'x';
    EXPECT_NE(initial, PocketDaily::cardSignature(seed, card));
    field[0] = '\0';
  }
  card.choiceCount = 3;
  const auto choices = PocketDaily::cardSignature(seed, card);
  EXPECT_NE(initial, choices);
  for (auto& choice : card.choices) {
    choice.id[0] = 'x';
    EXPECT_NE(choices, PocketDaily::cardSignature(seed, card));
    choice.id[0] = '\0';
    choice.label[0] = 'x';
    EXPECT_NE(choices, PocketDaily::cardSignature(seed, card));
    choice.label[0] = '\0';
  }
}

TEST(CardSignature, IgnoresUnusedBytesAndSlots) {
  PocketDaily::Card cards[PocketDaily::CARD_CAP]{};
  const auto initial = PocketDaily::cardsSignature(seed, cards, 1);
  cards[0].title[4] = 'x';
  cards[0].choices[0].label[0] = 'x';
  cards[1].question[0] = 'x';
  EXPECT_EQ(initial, PocketDaily::cardsSignature(seed, cards, 1));
}

TEST(CardSignature, FieldBoundariesAreDistinct) {
  PocketDaily::Card a{};
  PocketDaily::Card b{};
  strcpy(a.cardId, "ab");
  strcpy(a.module, "c");
  strcpy(b.cardId, "a");
  strcpy(b.module, "bc");
  EXPECT_NE(PocketDaily::cardSignature(seed, a), PocketDaily::cardSignature(seed, b));
}

TEST(CardSignature, CountOrderAndRemovalTriggerChange) {
  PocketDaily::Card cards[PocketDaily::CARD_CAP]{};
  cards[0].cardId[0] = 'a';
  cards[1].cardId[0] = 'b';
  const auto initial = PocketDaily::cardsSignature(seed, cards, 2);
  EXPECT_NE(initial, PocketDaily::cardsSignature(seed, cards, 1));
  EXPECT_NE(initial, PocketDaily::cardsSignature(seed, cards, 0));
  cards[0].cardId[0] = 'b';
  cards[1].cardId[0] = 'a';
  EXPECT_NE(initial, PocketDaily::cardsSignature(seed, cards, 2));
}

TEST(CardSignature, MalformedCountsAndUnterminatedFieldsStayBounded) {
  PocketDaily::Card cards[PocketDaily::CARD_CAP];
  memset(cards, 'x', sizeof(cards));
  for (auto& card : cards) card.choiceCount = 3;
  const auto initial = PocketDaily::cardsSignature(seed, cards, PocketDaily::CARD_CAP);
  for (auto& card : cards) card.choiceCount = 255;
  EXPECT_EQ(initial, PocketDaily::cardsSignature(seed, cards, 255));
}
}  // namespace
