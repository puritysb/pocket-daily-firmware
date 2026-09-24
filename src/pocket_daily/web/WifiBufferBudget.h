#pragma once

namespace PocketDaily::Web::WifiBufferBudget {
// ESP-IDF: zero dynamic RX buffers means unlimited, not disabled. Preserve
// smaller positive SDK budgets, while bounding unlimited or larger defaults.
constexpr int bounded(const int value, const int limit) { return value > 0 && value < limit ? value : limit; }
constexpr int STATIC_RX = 4;
constexpr int DYNAMIC_RX = 8;
constexpr int DYNAMIC_TX = 8;
constexpr int RX_BLOCK_ACK = 4;
static_assert(DYNAMIC_RX >= STATIC_RX);
static_assert(RX_BLOCK_ACK <= STATIC_RX);
}  // namespace PocketDaily::Web::WifiBufferBudget
