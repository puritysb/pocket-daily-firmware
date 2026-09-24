#include "TcpCensus.h"

#ifdef ENABLE_DEV_REMOTE_FLASH
#include <lwip/priv/tcp_priv.h>
#include <lwip/tcpip.h>

#include <cstdio>

namespace PocketDaily::Web {
size_t renderTcpCensus(char* out, const size_t cap) {
  // tcp_state order: CLOSED, LISTEN, SYN_SENT, SYN_RCVD, ESTABLISHED,
  // FIN_WAIT_1, FIN_WAIT_2, CLOSE_WAIT, CLOSING, LAST_ACK, TIME_WAIT.
  unsigned counts[11]{};
  unsigned timeWait = 0;
  LOCK_TCPIP_CORE();
  for (const tcp_pcb* pcb = tcp_active_pcbs; pcb; pcb = pcb->next)
    if (pcb->state < 11) ++counts[pcb->state];
  for (const tcp_pcb* pcb = tcp_tw_pcbs; pcb; pcb = pcb->next) ++timeWait;
  UNLOCK_TCPIP_CORE();
  const int n = snprintf(out, cap,
                         "established=%u synRcvd=%u finWait1=%u finWait2=%u closeWait=%u closing=%u lastAck=%u "
                         "timeWait=%u",
                         counts[ESTABLISHED], counts[SYN_RCVD], counts[FIN_WAIT_1], counts[FIN_WAIT_2],
                         counts[CLOSE_WAIT], counts[CLOSING], counts[LAST_ACK], timeWait);
  return n > 0 && static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : 0;
}
}  // namespace PocketDaily::Web
#else
namespace PocketDaily::Web {
size_t renderTcpCensus(char*, size_t) { return 0; }
}  // namespace PocketDaily::Web
#endif
