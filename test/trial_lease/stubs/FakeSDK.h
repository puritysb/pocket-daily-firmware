#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NVS_NOT_FOUND = 10;
using nvs_handle_t = unsigned;
constexpr int NVS_READONLY = 0, NVS_READWRITE = 1;
struct esp_partition_t {
  int type;
  int subtype;
  uint32_t address;
  uint32_t size;
  const char* label;
};
constexpr int ESP_PARTITION_TYPE_APP = 0, ESP_PARTITION_SUBTYPE_APP_OTA_0 = 16, ESP_PARTITION_SUBTYPE_APP_OTA_1 = 17;
struct esp_timer_create_args_t {
  void (*callback)(void*) = nullptr;
  void* arg = nullptr;
  const char* name = nullptr;
};
using esp_timer_handle_t = void*;
namespace Fake {
extern std::vector<uint8_t> blob, staged, flash[2];
extern bool namespaceExists, failCommit, failTimer, failSelect, failRead;
extern unsigned writes, selections, resets, timerStarts;
extern uint32_t running, selected;
extern uint64_t timerMicros;
extern void (*callback)(void*);
struct Restart {};
void reset();
}  // namespace Fake
void delay(unsigned);
void esp_restart();
int esp_reset_reason();
int esp_sleep_get_wakeup_cause();
const esp_partition_t* esp_ota_get_running_partition();
const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t*);
const esp_partition_t* esp_partition_find_first(int, int, const char*);
int esp_partition_read(const esp_partition_t*, size_t, void*, size_t);
int esp_timer_create(const esp_timer_create_args_t*, esp_timer_handle_t*);
int esp_timer_start_once(esp_timer_handle_t, uint64_t);
int nvs_open(const char*, int, nvs_handle_t*);
int nvs_get_blob(nvs_handle_t, const char*, void*, size_t*);
int nvs_set_blob(nvs_handle_t, const char*, const void*, size_t);
int nvs_commit(nvs_handle_t);
int nvs_erase_key(nvs_handle_t, const char*);
void nvs_close(nvs_handle_t);

constexpr int ESP_RST_SW = 3, ESP_RST_DEEPSLEEP = 8, ESP_SLEEP_WAKEUP_TIMER = 4;
namespace Fake {
extern int resetReason, wakeCause;
}
