#pragma once
#include <cstddef>
#include <cstdint>
class GfxRenderer;
namespace PocketDaily::Home {
bool recentBookCoverPath(unsigned index, char* path, size_t capacity);
bool coverMetadataForPath(const char* path, uint32_t& key, uint32_t& size, uint32_t& crc);
bool recentCoverMetadata(unsigned index, uint32_t& key, uint32_t& size, uint32_t& crc);
bool drawCachedCover(GfxRenderer& renderer, const char* path, int x, int y, int width, int height);
bool openBookCoverPath(char* path, size_t capacity);
uint32_t coverPathKey(const char* path);
bool coverMetadata(uint32_t& key, uint32_t& size, uint32_t& crc);
}  // namespace PocketDaily::Home
