#pragma once
#include <cstdint>

// Explicit developer experiments only; no production control surface.
namespace PocketDaily::DevCapture {
inline constexpr const char* PATH = "/.pocket/dev-screen.bmp";
#ifdef ENABLE_DEV_REMOTE_FLASH
bool save(uint32_t run);
uint32_t savedRun();
bool remove(uint32_t run);
bool requestReader(uint32_t run);
bool beginReader();
bool recoverReader();
bool readerActive();
void readerLoop();
#else
inline bool readerActive() { return false; }
#endif
}  // namespace PocketDaily::DevCapture
