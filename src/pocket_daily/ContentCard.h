#pragma once

#include "ContentManifest.h"
#include "models.h"

namespace PocketDaily::Content {
inline constexpr size_t CONTENT_CARD_BYTES = 512;
enum class CardLayout : uint8_t { TextFirst = 0, ImageFirst = 1, SideBySide = 2 };
struct ContentCard {
  Card card;
  char imagePath[64];
  CardLayout layout;
};
enum class CardResult { Ok, Shape, ReadFailed, Text, Identifier, ImagePath, Checksum };

// Caller owns output storage (not a second stack-resident Card). No allocation.
// On failure output is zeroed. No provider actions are accepted from the file.
CardResult decodeContentCard(const ManifestSource& source, ContentCard& output);
}  // namespace PocketDaily::Content
