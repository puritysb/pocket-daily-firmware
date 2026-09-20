#pragma once

#include <Arduino.h>
#include <HalStorage.h>
#include <NetworkClient.h>
#include <NetworkServer.h>

#include <cstddef>
#include <cstdint>
#include <memory>

#include "pocket_daily/upload_stream_protocol.h"
#include "pocket_daily/web/Host.h"

namespace PocketDaily::Web {

// The verified-staging ledger both upload planes publish into and
// `/api/pocket/v1/commit` verifies against. `crc32` keeps the running
// (non-finalized) value, exactly as the inherited UploadState did; the commit
// reader finalizes with the same XOR it always used.
struct StagedUpload {
  String fileName;
  String path = "/";
  size_t size = 0;
  uint32_t crc32 = PocketDaily::UploadStream::CRC32_INITIAL;
  bool success = false;
  String error = "";
  bool chunked = false;
  size_t chunkStart = 0;
};

// Pocket's upload data plane: one long-lived TCP connection per transfer on
// its own port. Arduino's WebServer closes every HTTP request, which leaves
// hundreds of lwIP sockets in TIME_WAIT for a multi-megabyte firmware upload
// on the no-PSRAM X3. This listener is deliberately tiny and is serviced
// incrementally (service(), one bounded slice per activity-loop pass) so
// physical buttons remain responsive during SD writes.
//
// All radio/watchdog/repaint reach-in goes through Host; this class knows
// nothing about the inherited web server or the live-studio service.
class UploadStreamServer final {
 public:
  struct Config {
    uint16_t port = 82;
    // True only for the POCKET_SYNC profile: the private-AP activity enrols
    // the loop task in the watchdog, so SD writes must suspend it.
    bool watchdogOwned = false;
  };

  void begin(const Config& config, const Host* host);
  void stop();

  // One bounded activity-loop slice: accept, header parse, payload drain.
  void service();

  // A stream transfer occupies the listener (any phase past IDLE), including
  // the short REPLIED grace window after the final reply.
  bool transferActive() const { return phase_ != Phase::IDLE; }
  bool listening() const { return server_ != nullptr; }
  uint16_t port() const { return config_.port; }

  const StagedUpload& staged() const { return staged_; }
  // The inherited chunked HTTP /upload path publishes its result here at
  // START/END/ABORT so commit sees the same ledger both planes always shared.
  // `staged_` is mutable because handleUpload runs as a const member there.
  void publishHttpStaged(const String& fileName, const String& path, size_t size, uint32_t crc32, bool success,
                         const String& error, bool chunked, size_t chunkStart) const {
    staged_.fileName = fileName;
    staged_.path = path;
    staged_.size = size;
    staged_.crc32 = crc32;
    staged_.success = success;
    staged_.error = error;
    staged_.chunked = chunked;
    staged_.chunkStart = chunkStart;
  }

 private:
  enum class Phase : uint8_t { IDLE, HEADER, DATA, REPLIED };

  // A transport failure (disconnect, idle timeout) keeps the hidden staging
  // file and this verified prefix so the companion can reconnect and send
  // only the remainder. Protocol and SD failures discard it.
  struct ResumeState {
    String path;
    size_t expected = 0;
    size_t received = 0;
    uint32_t crc32 = PocketDaily::UploadStream::CRC32_INITIAL;
    bool valid() const { return received > 0 && !path.isEmpty(); }
    void clear() {
      path = "";
      expected = 0;
      received = 0;
      crc32 = PocketDaily::UploadStream::CRC32_INITIAL;
    }
  };

  bool beginFromHeader();
  bool createStagingFile(const String& path, size_t expected);
  bool reopenStagingFile(const String& path, size_t received);
  void removeStaleStagingFiles(const String& directory, const String& keepName) const;
  bool appendPayload(const uint8_t* data, size_t count);
  bool flushBatch();
  void finish();
  void fail(const char* message, bool removePartial = true);
  void suspend(const char* message);
  void reset(bool removePartial);
  void discardResume();
  void suspendLoopWatchdog(const char* breadcrumb) const;
  void resumeLoopWatchdog() const;
  void noteClientActivity() const;
  void beginTransferFocus() const;
  void endTransferFocus() const;

  Config config_{};
  const Host* host_ = nullptr;
  std::unique_ptr<NetworkServer> server_ = nullptr;
  NetworkClient client_;
  Phase phase_ = Phase::IDLE;
  HalFile file_;
  char header_[320] = {};
  size_t headerLength_ = 0;
  String fullPath_;
  size_t expected_ = 0;
  size_t received_ = 0;  // bytes flushed to SD and covered by crc32_
  uint32_t crc32_ = PocketDaily::UploadStream::CRC32_INITIAL;
  unsigned long lastActivity_ = 0;

  // Socket bytes are batched into sector-aligned SD writes. During a
  // transfer this points at the flasher's idle 4 KiB static staging buffer
  // (firmware_flash::sharedStagingBuffer): the X3 private AP keeps ~6 KB of
  // heap, so a transient allocation would fail exactly where it matters. The
  // small static read buffer remains the fallback so a transfer is never
  // refused for lack of the optimization.
  uint8_t* batch_ = nullptr;
  size_t batchFill_ = 0;

  ResumeState resume_;
  mutable StagedUpload staged_;
};

}  // namespace PocketDaily::Web
