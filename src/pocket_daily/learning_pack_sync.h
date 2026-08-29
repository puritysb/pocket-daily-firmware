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

enum class BeginResult : uint8_t { NotAdvertised, Current, Started, Failed };
enum class Step : uint8_t { Idle, Progress, Updated, Retry, Failed };

// Foreground-only cooperative transfer. service() advances one 64 KiB response
// and returns to the main input loop; unattended pulls never start it.
BeginResult begin(const char* ip, uint16_t port, const char* token, const char* board, const Advert& advert);
Step service();
void cancel();
bool active();
uint32_t downloadedBytes();
uint32_t totalBytes();

}  // namespace LearningPackSync
}  // namespace PocketDaily
