#pragma once
#include <cstddef>
#include <cstdint>
namespace PocketDaily::Home {
bool openBookCoverPath(char* path, size_t capacity);
uint32_t coverPathKey(const char* path);
bool coverMetadata(uint32_t& key, uint32_t& size, uint32_t& crc);
}  // namespace PocketDaily::Home
