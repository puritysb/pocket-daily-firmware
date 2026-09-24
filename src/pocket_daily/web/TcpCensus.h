#pragma once

#include <cstddef>

namespace PocketDaily::Web {
// Developer evidence only: counts lwIP TCP PCBs per state (active list plus
// TIME_WAIT list) under the TCPIP core lock. Renders one short line into the
// caller's buffer; no allocation. Returns the byte count, 0 when unavailable.
size_t renderTcpCensus(char* out, size_t cap);
}  // namespace PocketDaily::Web
