#include "StackReport.h"

#if defined(ENABLE_DEV_REMOTE_FLASH) || defined(ENABLE_HEAP_MAP)

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstring>

namespace PocketDaily::StackReport {

namespace {
int line(char* out, size_t cap, size_t used, const char* fmt, ...) {
  if (used >= cap) return 0;
  va_list args;
  va_start(args, fmt);
  const int n = vsnprintf(out + used, cap - used, fmt, args);
  va_end(args);
  return n > 0 ? n : 0;
}
}  // namespace

size_t render(char* out, size_t cap) {
  size_t used = 0;
  // Task handles are not exported broadly; walk by name using xTaskGetHandle
  // (Arduino ESP32 supports it) for the known heavyweights. The render task
  // belongs to ActivityManager but its name is stable.
  static const char* const names[] = {"server_tcp", "server_ssl", "ws",   "IDLE0", "idle0",
                                      "Tmr Svc",    "arduino",    "loop", "main",  "render"};
  used += line(out, cap, used, "# stack high-water (free bytes)\n");
  used += line(out, cap, used, "caller %lu\n",
               static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
  for (const char* name : names) {
    TaskHandle_t handle = xTaskGetHandle(name);
    if (!handle) continue;
    used += line(out, cap, used, "%s %lu\n", name,
                 static_cast<unsigned long>(uxTaskGetStackHighWaterMark(handle) * sizeof(StackType_t)));
    if (used >= cap - 48) break;
  }
  return used < cap ? used : cap - 1;
}

}  // namespace PocketDaily::StackReport

#else

namespace PocketDaily::StackReport {
size_t render(char*, size_t) { return 0; }
}  // namespace PocketDaily::StackReport

#endif
