#pragma once
#include <cstdint>
#include <string>

#include "CaptureTicket.h"

// Explicit developer experiments only; no production control surface.
namespace PocketDaily::DevCapture {
inline constexpr const char* PATH = "/.pocket/dev-screen.bmp";
#ifdef ENABLE_DEV_REMOTE_FLASH
inline constexpr const char* WAKE_PATH = "/.pocket/dev-wake.bmp";
struct WakeEvidence {
  uint32_t run = 0;
  uint32_t refreshMs = 0;
  bool skipped = false;
};
const WakeEvidence& wakeEvidence();
bool saveWake(uint32_t run, uint32_t refreshMs, bool skipped);
bool removeWake(uint32_t run);
bool save(uint32_t run);
uint32_t savedRun();
bool remove(uint32_t run);
bool requestReader(uint32_t run, ReaderAction action = ReaderAction::Capture);
std::string exchangeFixture();
bool clearFixture(uint32_t run);
bool acceptOffer();
void offerAccepted();
bool turnPage();
bool continuitySucceeded(uint32_t run);
bool beginReader();
bool recoverReader();
bool readerActive();
void readerLoop();
#else
inline bool readerActive() { return false; }
#endif
}  // namespace PocketDaily::DevCapture
