#include "pocket_daily/web/UploadStreamServer.h"

#include <Arduino.h>
#include <HalSystem.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_task_wdt.h>

#include <algorithm>

#include "network/FirmwareFlasher.h"

namespace {
constexpr unsigned long POCKET_STREAM_IDLE_TIMEOUT_MS = 30 * 1000;
constexpr unsigned long POCKET_STREAM_REPLY_GRACE_MS = 1000;
// Header reads and the allocation-free fallback use this static buffer. An
// active transfer batches into the flasher's idle 4 KiB static staging buffer
// so each SD flush is a sector-aligned multi-block write instead of a
// sub-sector read-modify-write, with no heap involved; 4 KiB also keeps one
// activity-loop pass short enough for the physical buttons.
constexpr size_t POCKET_STREAM_READ_BYTES = 768;
constexpr size_t POCKET_STREAM_BATCH_BYTES = firmware_flash::STAGING_BUFFER_BYTES;
uint8_t pocketStreamReadBuffer[POCKET_STREAM_READ_BYTES];
}  // namespace

namespace PocketDaily::Web {

void UploadStreamServer::begin(const Config& config, const Host* host) {
  config_ = config;
  host_ = host;
  if (!host_ || !host_->normalizeWebPath || !host_->noteClientActivity) {
    LOG_ERR("PUPLOAD", "Upload stream started without a wired host");
    return;
  }
  // X3/X4 Pocket uploads use one persistent, allocation-light connection.
  server_ = makeUniqueNoThrow<NetworkServer>(config_.port, 1);
  if (server_) {
    server_->setNoDelay(true);
    server_->begin();
    LOG_INF("WEB", "Pocket upload stream listening on port %u", (unsigned)config_.port);
  } else {
    LOG_ERR("WEB", "Could not allocate Pocket upload stream listener");
  }
}

void UploadStreamServer::stop() {
  reset(true);
  discardResume();
  if (server_) {
    server_->stop();
    server_.reset();
  }
}

void UploadStreamServer::noteClientActivity() const {
  if (host_ && host_->noteClientActivity) host_->noteClientActivity(host_->self);
}

void UploadStreamServer::beginTransferFocus() const {
  if (host_ && host_->beginTransferFocus) host_->beginTransferFocus(host_->self);
}

void UploadStreamServer::endTransferFocus() const {
  if (host_ && host_->endTransferFocus) host_->endTransferFocus(host_->self);
}

void UploadStreamServer::suspendLoopWatchdog(const char* breadcrumb) const {
  // X3 SD cluster allocation or a multi-sector flush can occasionally block a
  // valid storage call longer than the private Nearby Sync loop-watchdog
  // window. Keep the watchdog around network parsing, but suspend this task's
  // registration only while the synchronous storage call is in progress. File
  // Transfer in STA mode never enrols the loop task, which is why the same
  // large image is reliable there.
  if (!config_.watchdogOwned) return;
  HalSystem::setCrashBreadcrumb(breadcrumb);
  disableLoopWDT();
}

void UploadStreamServer::resumeLoopWatchdog() const {
  if (!config_.watchdogOwned) return;
  enableLoopWDT();
  esp_task_wdt_reset();
  HalSystem::setCrashBreadcrumb("nearby:upload-stream");
}

void UploadStreamServer::discardResume() {
  if (!resume_.path.isEmpty() && Storage.exists(resume_.path.c_str())) {
    Storage.remove(resume_.path.c_str());
  }
  resume_.clear();
}

void UploadStreamServer::reset(const bool removePartial) {
  if (file_) file_.close();
  if (removePartial && !fullPath_.isEmpty() && fullPath_ != resume_.path) {
    Storage.remove(fullPath_.c_str());
  }
  if (client_) client_.stop();
  client_ = NetworkClient();
  phase_ = Phase::IDLE;
  headerLength_ = 0;
  header_[0] = '\0';
  fullPath_ = "";
  expected_ = 0;
  received_ = 0;
  crc32_ = PocketDaily::UploadStream::CRC32_INITIAL;
  lastActivity_ = 0;
  batch_ = nullptr;
  batchFill_ = 0;
}

void UploadStreamServer::fail(const char* message, const bool removePartial) {
  LOG_ERR("PUPLOAD", "%s", message);
  batch_ = nullptr;
  batchFill_ = 0;
  if (file_) {
    suspendLoopWatchdog("nearby:upload-close");
    file_.close();
    resumeLoopWatchdog();
  }
  if (removePartial && !fullPath_.isEmpty()) {
    Storage.remove(fullPath_.c_str());
    if (resume_.path == fullPath_) resume_.clear();
  }
  staged_.success = false;
  staged_.error = message;
  char response[112];
  const size_t length = PocketDaily::UploadStream::formatErrorReply(response, sizeof(response), message);
  if (client_ && length > 0) {
    client_.write(reinterpret_cast<const uint8_t*>(response), length);
  }
  phase_ = Phase::REPLIED;
  lastActivity_ = millis();
}

void UploadStreamServer::suspend(const char* message) {
  endTransferFocus();
  // Transport failure while payload was flowing. Flush what already arrived,
  // keep the hidden staging file on the card and remember the verified prefix
  // so a reconnecting companion sends only the remainder. RAM cost is one path
  // string plus three integers; the bytes themselves stay on the SD card.
  const bool flushed = flushBatch();
  batch_ = nullptr;
  batchFill_ = 0;
  if (file_) {
    suspendLoopWatchdog("nearby:upload-close");
    file_.close();
    resumeLoopWatchdog();
  }
  if (flushed && received_ > 0 && received_ < expected_) {
    resume_.path = fullPath_;
    resume_.expected = expected_;
    resume_.received = received_;
    resume_.crc32 = crc32_;
    LOG_INF("PUPLOAD", "%s; retaining %u of %u bytes of %s for resume", message, (unsigned)received_,
            (unsigned)expected_, fullPath_.c_str());
  } else {
    LOG_ERR("PUPLOAD", "%s", message);
    if (!fullPath_.isEmpty()) Storage.remove(fullPath_.c_str());
    resume_.clear();
  }
  staged_.success = false;
  staged_.error = message;
  char response[112];
  const size_t length = PocketDaily::UploadStream::formatErrorReply(response, sizeof(response), message);
  if (client_ && client_.connected() && length > 0) {
    client_.write(reinterpret_cast<const uint8_t*>(response), length);
  }
  phase_ = Phase::REPLIED;
  lastActivity_ = millis();
}

void UploadStreamServer::removeStaleStagingFiles(const String& directory, const String& keepName) const {
  // A reboot or power loss during a transfer leaves a hidden `.pocket-*.part`
  // file that may hold a preallocated multi-megabyte span. Sweep only the
  // destination directory of the new transfer. `.pocket-backup.part` is never
  // touched: a commit uses it to preserve the previously published file.
  constexpr size_t MAX_VICTIMS = 4;
  constexpr int MAX_ENTRIES = 256;
  for (int round = 0; round < 3; ++round) {
    String victims[MAX_VICTIMS];
    size_t victimCount = 0;
    {
      HalFile dir = Storage.open(directory.c_str());
      if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return;
      }
      for (int i = 0; i < MAX_ENTRIES && victimCount < MAX_VICTIMS; ++i) {
        HalFile entry = dir.openNextFile();
        if (!entry) break;
        char name[96] = {};
        entry.getName(name, sizeof(name));
        const bool isDir = entry.isDirectory();
        entry.close();
        if (isDir || strncmp(name, ".pocket-", 8) != 0) continue;
        const size_t nameLength = strlen(name);
        if (nameLength < 14 || strcmp(name + nameLength - 5, ".part") != 0) continue;
        if (keepName == name || strcmp(name, ".pocket-backup.part") == 0) continue;
        victims[victimCount++] = name;
      }
      dir.close();
    }
    if (victimCount == 0) return;
    for (size_t i = 0; i < victimCount; ++i) {
      const String path = directory == "/" ? "/" + victims[i] : directory + "/" + victims[i];
      if (Storage.remove(path.c_str())) LOG_INF("PUPLOAD", "Removed stale staging file %s", path.c_str());
    }
    if (victimCount < MAX_VICTIMS) return;
  }
}

bool UploadStreamServer::createStagingFile(const String& path, const size_t expected) {
  suspendLoopWatchdog("nearby:upload-create");
  if (Storage.exists(path.c_str())) Storage.remove(path.c_str());
  const bool opened = Storage.openFileForWrite("PUPLOAD", path, file_);
  if (opened && expected > 0) {
    // Contiguous clusters turn every later flush into one multi-sector write
    // with no FAT walk in the middle of the transfer, and leave update.bin
    // contiguous for the flasher. Failure is not fatal: SdFat then allocates
    // clusters as data arrives, exactly as before.
    if (!file_.preAllocate(expected)) {
      LOG_DBG("PUPLOAD", "No contiguous span for %u bytes; allocating during transfer", (unsigned)expected);
    }
  }
  resumeLoopWatchdog();
  return opened;
}

bool UploadStreamServer::reopenStagingFile(const String& path, const size_t received) {
  suspendLoopWatchdog("nearby:upload-reopen");
  bool ok = false;
  if (Storage.exists(path.c_str())) {
    file_ = Storage.open(path.c_str(), O_RDWR);
    // A preallocated file already reports its final size; a plain file reports
    // exactly the flushed prefix. Both must at least cover the retained prefix.
    ok = file_ && !file_.isDirectory() && file_.size() >= received && file_.seekSet(received);
    if (!ok && file_) file_.close();
  }
  resumeLoopWatchdog();
  return ok;
}

bool UploadStreamServer::beginFromHeader() {
  PocketDaily::UploadStream::Request request;
  if (!PocketDaily::UploadStream::parseHeader(header_, headerLength_, request)) return false;

  const String requestedPath = request.path;
  const String normalized = host_->normalizeWebPath(host_->self, requestedPath);
  if (normalized != requestedPath) return false;
  const int slash = normalized.lastIndexOf('/');
  if (slash < 0) return false;
  const String name = normalized.substring(slash + 1);
  const String directory = slash == 0 ? "/" : normalized.substring(0, slash);

  if (host_->httpUploadBusy && host_->httpUploadBusy(host_->self)) return false;  // A legacy HTTP upload owns the shared commit state.
  if (host_->releaseHttpUploadBuffer) host_->releaseHttpUploadBuffer(host_->self);
  staged_.fileName = name;
  staged_.path = directory;
  staged_.size = 0;
  staged_.crc32 = PocketDaily::UploadStream::CRC32_INITIAL;
  staged_.success = false;
  staged_.error = "";
  staged_.chunked = false;

  fullPath_ = normalized;
  expected_ = request.size;
  received_ = 0;
  crc32_ = PocketDaily::UploadStream::CRC32_INITIAL;
  batchFill_ = 0;
  batch_ = firmware_flash::sharedStagingBuffer();

  bool resumed = false;
  if (request.resume && resume_.valid() && resume_.path == normalized && resume_.expected == request.size) {
    resumed = reopenStagingFile(normalized, resume_.received);
    if (resumed) {
      received_ = resume_.received;
      crc32_ = resume_.crc32;
    }
  }
  if (!resumed && !resume_.path.isEmpty()) discardResume();
  resume_.clear();
  if (!resumed) {
    removeStaleStagingFiles(directory, name);
    if (!createStagingFile(normalized, request.size)) return false;
  }

  if (request.resume) {
    // The companion waits for this line before sending any payload byte, so
    // the retained prefix is never overwritten with duplicate data.
    char reply[32];
    const size_t length = PocketDaily::UploadStream::formatResumeReply(reply, sizeof(reply), received_);
    if (length == 0 || client_.write(reinterpret_cast<const uint8_t*>(reply), length) != length) {
      suspend("Upload disconnected");
      beginTransferFocus();
      return true;
    }
  }

  phase_ = Phase::DATA;
  LOG_INF("PUPLOAD", "%s %s (%u/%u bytes, batch=%u)", resumed ? "Resuming" : "Receiving", normalized.c_str(),
          (unsigned)received_, (unsigned)request.size,
          (unsigned)(batch_ ? POCKET_STREAM_BATCH_BYTES : POCKET_STREAM_READ_BYTES));
  if (received_ >= expected_) finish();
  beginTransferFocus();
  return true;
}

bool UploadStreamServer::flushBatch() {
  if (batchFill_ == 0) return true;
  const uint8_t* batch = batch_ ? batch_ : pocketStreamReadBuffer;
  const size_t count = batchFill_;
  suspendLoopWatchdog("nearby:upload-write");
  const size_t written = file_ ? file_.write(batch, count) : 0;
  resumeLoopWatchdog();
  if (written != count) return false;
  // Only flushed bytes count toward the verified prefix a resume may reuse.
  crc32_ = PocketDaily::UploadStream::updateCrc32(crc32_, batch, count);
  received_ += count;
  batchFill_ = 0;
  return true;
}

bool UploadStreamServer::appendPayload(const uint8_t* data, size_t count) {
  uint8_t* batch = batch_ ? batch_ : pocketStreamReadBuffer;
  const size_t capacity = batch_ ? POCKET_STREAM_BATCH_BYTES : POCKET_STREAM_READ_BYTES;
  while (count > 0) {
    const size_t toCopy = std::min(capacity - batchFill_, count);
    // memmove: in the static fallback the payload tail of a header read lives
    // in the same buffer it is being compacted into.
    memmove(batch + batchFill_, data, toCopy);
    batchFill_ += toCopy;
    data += toCopy;
    count -= toCopy;
    if (batchFill_ == capacity && !flushBatch()) return false;
  }
  return true;
}

void UploadStreamServer::finish() {
  endTransferFocus();
  if (file_) {
    suspendLoopWatchdog("nearby:upload-close");
    file_.close();
    resumeLoopWatchdog();
  }
  batch_ = nullptr;
  batchFill_ = 0;
  staged_.size = received_;
  staged_.crc32 = crc32_;
  staged_.success = received_ == expected_;
  if (!staged_.success) {
    fail("Upload size mismatch");
    return;
  }

  const uint32_t finalizedCrc = PocketDaily::UploadStream::finalizeCrc32(crc32_);
  char response[48];
  const size_t length =
      PocketDaily::UploadStream::formatOkReply(response, sizeof(response), received_, finalizedCrc);
  if (length > 0) client_.write(reinterpret_cast<const uint8_t*>(response), length);
  phase_ = Phase::REPLIED;
  lastActivity_ = millis();
  LOG_INF("PUPLOAD", "Received %s (%u bytes, crc32=%08lX)", fullPath_.c_str(), (unsigned)received_,
          (unsigned long)finalizedCrc);
}

void UploadStreamServer::service() {
  if (!server_) return;

  if (phase_ == Phase::IDLE) {
    if (!server_->hasClient()) return;
    client_ = server_->accept();
    if (!client_) return;
    client_.setNoDelay(true);
    phase_ = Phase::HEADER;
    headerLength_ = 0;
    lastActivity_ = millis();
    noteClientActivity();
    LOG_INF("PUPLOAD", "Client connected from %s", client_.remoteIP().toString().c_str());
  }

  if (phase_ == Phase::REPLIED) {
    if (!client_.connected() || millis() - lastActivity_ >= POCKET_STREAM_REPLY_GRACE_MS) {
      reset(false);
    }
    return;
  }

  const bool receivingPayload = phase_ == Phase::DATA;
  if (millis() - lastActivity_ >= POCKET_STREAM_IDLE_TIMEOUT_MS) {
    if (receivingPayload) {
      suspend("Upload timed out");
    } else {
      fail("Upload timed out");
    }
    return;
  }

  const int available = client_.available();
  if (available <= 0) {
    if (!client_.connected()) {
      if (receivingPayload) {
        suspend("Upload disconnected");
      } else {
        fail("Upload disconnected");
      }
    }
    return;
  }

  if (phase_ == Phase::HEADER) {
    const size_t wanted = std::min(static_cast<size_t>(available), POCKET_STREAM_READ_BYTES);
    const int count = client_.read(pocketStreamReadBuffer, wanted);
    if (count <= 0) return;
    lastActivity_ = millis();
    noteClientActivity();

    size_t offset = 0;
    while (offset < static_cast<size_t>(count) && phase_ == Phase::HEADER) {
      if (headerLength_ + 1 >= sizeof(header_)) {
        fail("Upload header too large", false);
        return;
      }
      header_[headerLength_++] = static_cast<char>(pocketStreamReadBuffer[offset++]);
      header_[headerLength_] = '\0';
      if (PocketDaily::UploadStream::headerStatus(header_, headerLength_, sizeof(header_)) ==
          PocketDaily::UploadStream::HeaderStatus::Complete) {
        if (!beginFromHeader()) {
          fail("Invalid upload header");
          return;
        }
      }
    }

    if (phase_ != Phase::DATA || offset >= static_cast<size_t>(count)) return;
    const size_t leftover = static_cast<size_t>(count) - offset;
    if (leftover > expected_ - received_ - batchFill_) {
      fail("Upload overflow");
      return;
    }
    if (!appendPayload(pocketStreamReadBuffer + offset, leftover)) {
      fail("SD write failed");
      return;
    }
  } else {
    // Drain the socket straight into the batch. One activity-loop pass moves
    // at most one batch, so physical buttons are sampled between SD writes.
    uint8_t* batch = batch_ ? batch_ : pocketStreamReadBuffer;
    const size_t capacity = batch_ ? POCKET_STREAM_BATCH_BYTES : POCKET_STREAM_READ_BYTES;
    const size_t outstanding = expected_ - received_ - batchFill_;
    if (static_cast<size_t>(available) > outstanding) {
      fail("Upload overflow");
      return;
    }
    const size_t wanted = std::min({static_cast<size_t>(available), capacity - batchFill_, outstanding});
    const int count = client_.read(batch + batchFill_, wanted);
    if (count <= 0) return;
    lastActivity_ = millis();
    noteClientActivity();
    batchFill_ += static_cast<size_t>(count);
    if (batchFill_ == capacity && !flushBatch()) {
      fail("SD write failed");
      return;
    }
  }

  if (received_ + batchFill_ >= expected_) {
    if (!flushBatch()) {
      fail("SD write failed");
      return;
    }
    finish();
  }
}

}  // namespace PocketDaily::Web
