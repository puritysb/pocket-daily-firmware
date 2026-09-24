#include "ServerTimeWait.h"

#ifdef ARDUINO
#include <lwip/priv/tcp_priv.h>
#include <lwip/tcp.h>
#include <lwip/tcpip.h>

namespace PocketDaily::Web {
size_t purgeServerTimeWait(const uint16_t httpPort, const uint16_t streamPort) {
  LOCK_TCPIP_CORE();
  const size_t purged = purgeTimeWaitList(tcp_tw_pcbs, httpPort, streamPort, [](tcp_pcb* pcb) { tcp_abort(pcb); });
  UNLOCK_TCPIP_CORE();
  return purged;
}
}  // namespace PocketDaily::Web
#else
namespace PocketDaily::Web {
size_t purgeServerTimeWait(uint16_t, uint16_t) { return 0; }
}  // namespace PocketDaily::Web
#endif
