#pragma once

#include <cstddef>
#include <cstdint>

namespace PocketDaily::Web {
// Arduino WebServer closes every response itself, so the reader is the active
// closer and lwIP keeps each PCB in TIME_WAIT for 2*MSL (120s, ~256B of
// heap, up to 16). On X3 Sync the companion heartbeat plus one Apply burst
// held 2-4KiB exactly when display admission needed it. Sync sessions drop
// this server's own TIME_WAIT PCBs; lwIP's tcp_abandon frees a TIME_WAIT PCB
// without sending anything. See docs/sync-route-memory.md for the trade-off.

// Pure list walk. `next` is read before abort because abort unlinks and frees.
template <typename Pcb, typename Abort>
size_t purgeTimeWaitList(Pcb* head, const uint16_t portA, const uint16_t portB, Abort abort) {
  size_t purged = 0;
  for (Pcb* pcb = head; pcb;) {
    Pcb* next = pcb->next;
    if (pcb->local_port == portA || pcb->local_port == portB) {
      abort(pcb);
      ++purged;
    }
    pcb = next;
  }
  return purged;
}

// Device: walks tcp_tw_pcbs under the TCPIP core lock. Host builds return 0.
size_t purgeServerTimeWait(uint16_t httpPort, uint16_t streamPort);

// Rate-limited owner on the server loop. No allocation, task or wait.
class ServerTimeWaitPurge {
 public:
  using PurgeFn = size_t (*)(uint16_t, uint16_t);
  static constexpr uint32_t INTERVAL_MS = 100;

  explicit ServerTimeWaitPurge(PurgeFn purge = purgeServerTimeWait) : purge_(purge) {}

  // `force` (pending presentation admission) bypasses the interval so the
  // heap sample that follows never includes this server's closed sockets.
  void service(const uint32_t now, const uint16_t httpPort, const uint16_t streamPort, const bool force) {
    if (!force && ran_ && now - last_ < INTERVAL_MS) return;
    ran_ = true;
    last_ = now;
    purged_ += static_cast<uint32_t>(purge_(httpPort, streamPort));
  }
  uint32_t purged() const { return purged_; }

 private:
  PurgeFn purge_;
  uint32_t last_ = 0;
  uint32_t purged_ = 0;
  bool ran_ = false;
};
}  // namespace PocketDaily::Web
