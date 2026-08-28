#pragma once

#include <cstdint>

namespace PocketDaily {
namespace LearningPackSync {

struct Advert {
  char packageId[32] = {0};
  uint32_t contentVersion = 0;
  uint16_t formatVersion = 0;
  uint32_t size = 0;
  char md5[33] = {0};
  char licenseSpdx[32] = {0};
};

enum class Result : uint8_t { NotAdvertised, Current, Updated, Failed };

// Download an advertised pack from the already-authenticated Surface provider.
// This is intentionally called only by an explicit user Sync; unattended wake
// pulls must not spend battery downloading multi-megabyte course assets.
Result sync(const char* ip, uint16_t port, const char* token, const char* board, const Advert& advert);

}  // namespace LearningPackSync
}  // namespace PocketDaily
