#pragma once

#include <cstddef>

namespace AgentDeck {
namespace AuthStore {

// Pairing credentials are device secrets and therefore live in ESP32 NVS,
// not in the user-browsable SD filesystem.
bool load(char* token, size_t tokenCap);
bool save(const char* token);

}  // namespace AuthStore
}  // namespace AgentDeck
