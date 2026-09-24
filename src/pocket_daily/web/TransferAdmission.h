#pragma once

namespace PocketDaily::Web {
// Snapshot of live file writers on the single server loop. A stream's REPLIED
// phase is not a writer: its handle is closed, so HTTP commit can proceed.
struct TransferWriters {
  bool http = false;
  bool websocket = false;
  bool stream = false;
  bool any() const { return http || websocket || stream; }
};

// A rejected multipart request must ignore every subsequent WRITE/END/ABORT
// callback, preserving the other transport's staged receipt and open file.
class UploadAdmission {
 public:
  void begin(bool writerActive) { rejected_ = writerActive; }
  bool rejected() const { return rejected_; }

 private:
  bool rejected_ = false;
};
}  // namespace PocketDaily::Web
