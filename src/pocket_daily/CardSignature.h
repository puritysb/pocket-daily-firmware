#pragma once

#include "models.h"

namespace PocketDaily {

// Local change detection only, not a wire revision or an integrity checksum.
// Hash semantic fields rather than padding, unused slots or bytes after NUL.
uint32_t cardSignature(uint32_t hash, const Card& card);
uint32_t cardsSignature(uint32_t hash, const Card (&cards)[CARD_CAP], uint8_t count);

}  // namespace PocketDaily
