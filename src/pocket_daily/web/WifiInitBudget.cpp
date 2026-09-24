// Project-owned linker boundary; never patch the installed Arduino framework.
// Only env:sta_recovery links this experimental policy into esp_wifi_init.
#ifdef POCKET_BOUNDED_WIFI_BUFFERS
#include <esp_wifi.h>

#include "pocket_daily/web/WifiBufferBudget.h"

extern "C" esp_err_t __real_esp_wifi_init(const wifi_init_config_t* config);

extern "C" esp_err_t __wrap_esp_wifi_init(const wifi_init_config_t* config) {
  if (!config) return __real_esp_wifi_init(config);
  // Initialization is synchronous. A bounded stack copy preserves ABI magic,
  // OS/crypto callbacks, feature flags and every unrelated SDK setting.
  static_assert(sizeof(wifi_init_config_t) < 256);
  wifi_init_config_t budget = *config;
  using namespace PocketDaily::Web::WifiBufferBudget;
  budget.static_rx_buf_num = bounded(budget.static_rx_buf_num, STATIC_RX);
  budget.dynamic_rx_buf_num = bounded(budget.dynamic_rx_buf_num, DYNAMIC_RX);
  if (budget.tx_buf_type == 1) budget.dynamic_tx_buf_num = bounded(budget.dynamic_tx_buf_num, DYNAMIC_TX);
  budget.rx_ba_win = bounded(budget.rx_ba_win, bounded(budget.static_rx_buf_num, RX_BLOCK_ACK));
  return __real_esp_wifi_init(&budget);
}
#endif
