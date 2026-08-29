#pragma once

#include <cstdint>

namespace PocketDaily {
namespace SegmentedDownload {

// One foreground Surface asset transfer is active at a time. Each service()
// call performs one bounded HTTP response, then returns to the main input loop.
enum class Step : uint8_t { Idle, Progress, Complete, Retry, Failed };

bool begin(const char* ip, uint16_t port, const char* token, const char* board, const char* requestPath,
           const char* tempPath, uint32_t expectedSize, const char* expectedMd5);
Step service();
void cancel();
bool active();
uint32_t downloadedBytes();
uint32_t totalBytes();

}  // namespace SegmentedDownload
}  // namespace PocketDaily
