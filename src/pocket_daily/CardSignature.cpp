#include "CardSignature.h"

namespace PocketDaily {
namespace {
uint32_t signatureByte(uint32_t hash, uint8_t value) { return (hash ^ value) * 16777619u; }

uint32_t signatureText(uint32_t hash, const char* text, size_t capacity) {
  for (size_t i = 0; i < capacity && text[i] != '\0'; ++i) {
    hash = signatureByte(hash, static_cast<uint8_t>(text[i]));
  }
  return signatureByte(hash, 0);  // preserve field boundaries
}
}  // namespace

uint32_t cardSignature(uint32_t hash, const Card& card) {
  hash = signatureText(hash, card.cardId, sizeof(card.cardId));
  hash = signatureText(hash, card.module, sizeof(card.module));
  hash = signatureText(hash, card.actionClass, sizeof(card.actionClass));
  hash = signatureText(hash, card.title, sizeof(card.title));
  hash = signatureText(hash, card.question, sizeof(card.question));
  hash = signatureText(hash, card.context, sizeof(card.context));
  uint8_t count = card.choiceCount;
  if (count > 3) count = 3;
  hash = signatureByte(hash, count);
  for (uint8_t i = 0; i < count; ++i) {
    hash = signatureText(hash, card.choices[i].id, sizeof(card.choices[i].id));
    hash = signatureText(hash, card.choices[i].label, sizeof(card.choices[i].label));
  }
  return hash;
}

uint32_t cardsSignature(uint32_t hash, const Card (&cards)[CARD_CAP], uint8_t count) {
  if (count > CARD_CAP) count = CARD_CAP;
  hash = signatureByte(hash, count);
  for (uint8_t i = 0; i < count; ++i) hash = cardSignature(hash, cards[i]);
  return hash;
}
}  // namespace PocketDaily
