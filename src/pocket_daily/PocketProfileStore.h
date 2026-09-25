#pragma once

#include <cstdint>

#include "PocketProfile.h"

// Two alternating CRC/generation slots (docs/pocket-profile-v1.md). The RAM copy
// is loaded once at boot; Home and the sleep frame read it without SD access.
namespace PocketDaily::DailyProfile {
inline constexpr const char* SLOT_PATHS[] = {"/.crosspoint/pocket-profile.0.bin", "/.crosspoint/pocket-profile.1.bin"};

enum class SaveResult : uint8_t { Ok, Conflict, Invalid, StorageError };

// Newest valid slot wins; none (or both corrupt) means defaults at generation 0.
void loadAtBoot();
const Profile& current();
uint32_t generation();

// Compare-and-swap on the generation the caller last read. Writes the older
// slot, reads it back, and only then replaces the RAM copy.
SaveResult save(const Profile& profile, uint32_t expectedGeneration);

// Host tests only: forget the RAM copy.
void resetForTest();
}  // namespace PocketDaily::DailyProfile
