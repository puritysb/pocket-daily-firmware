#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "CaptureTicket.h"
#ifdef ENABLE_DEV_REMOTE_FLASH
#include <Epub/ImageDiagnostics.h>
#include <Epub/ImagePaintDiagnostics.h>

#include "LibraryTrial.h"
#include "ReaderWalk.h"
#include "pocket_daily/BuildFailureLog.h"
#endif

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
inline constexpr const char* HOME_PATH = "/.pocket/dev-home.bmp";
inline constexpr const char* HOME_INPUTS = "/.pocket/dev-home.pdli";
inline constexpr const char* SLEEP_INPUTS = "/.pocket/dev-sleep.pdli";
inline constexpr const char* RETURNED_HOME_PATH = "/.pocket/dev-returned-home.bmp";
inline constexpr const char* RETURNED_HOME_INPUTS = "/.pocket/dev-returned-home.pdli";
bool saveHome(uint32_t run, bool returned = false);
uint32_t homeRun(bool returned = false);
bool removeHome(uint32_t run, bool returned = false);
bool saveLayoutInputs(uint32_t run, bool sleep, bool returned = false);
const WakeEvidence& wakeEvidence();
bool saveWake(uint32_t run, uint32_t refreshMs, bool skipped);
bool removeWake(uint32_t run);
bool save(uint32_t run);
uint32_t savedRun();
bool remove(uint32_t run);
bool requestReader(uint32_t run, ReaderAction action = ReaderAction::Capture, uint32_t pages = 0);
bool readerWalkActive();
inline constexpr const char* LIBRARY_ORIGINAL_PATH = "/.pocket/dev-library-original.bmp";
inline constexpr const char* LIBRARY_OWNED_PATH = "/.pocket/dev-library-owned.bmp";
struct LibraryBackupInfo {
  uint32_t bytes = 0;
  bool present = false;
  uint8_t sha[32]{};
};
const char* libraryBackupPath(std::string_view selector);
bool libraryBackupInfo(std::string_view selector, LibraryBackupInfo& info);
int formatLibraryBackup(uint32_t run, char* out, size_t capacity);
int formatLibraryEvidence(uint32_t run, char* out, size_t capacity);
bool requestLibrary(uint32_t run, uint32_t pages, uint32_t bytes, std::string_view reference,
                    std::string_view sourceSha, std::string_view titleSha, std::string_view backupSha);
bool libraryTrialActive();
bool libraryPreparing();
LibraryPhase libraryPhase();
const char* libraryTargetPath();
uint32_t libraryTargetBytes();
bool libraryRowLocated(uint32_t row, std::string_view title);
void libraryEntered(bool validIndex);
bool libraryFrameDrawn(int selected, int first, int drawn, int offset, int itemValue);
bool libraryFrameReady();
bool libraryNextTarget();
bool libraryOpenOwned(std::string_view path);
void libraryFailed(LibraryError error);
uint32_t libraryFrameRun(bool original);
bool acknowledgeLibrary(uint32_t run);
WalkEvidence readerWalkEvidence(uint32_t run);
// Completed bounded walks only. Buffer supplied by the existing HTTP staging pool.
size_t readWalkPerformance(uint32_t run, uint8_t* out, size_t capacity);
void readerPageRendered(uint32_t spine, uint32_t page, uint32_t progress);
void readerTurnResult(bool moved);
bool readerBuildFailed(const BuildFailureLog::Entry& entry);
bool loadWalkFailure(uint32_t run, BuildFailureLog::Entry& entry);
int formatPaintDiagnostics(uint32_t run, char* out, size_t capacity);
bool loadPaintDiagnostics(uint32_t run, epub_paint_diagnostics::Snapshot& out);
bool loadImageDiagnostics(uint32_t run, epub_image_diagnostics::Snapshot& out);
struct ReaderReadiness {
  uint32_t active, pending, fixture;
  bool bookReady;
  bool walkReceiptPresent;
  uint32_t walkReceiptRun;
};
ReaderReadiness readerReadiness(uint32_t run);
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
