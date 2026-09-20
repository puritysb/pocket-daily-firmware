#include "HeapMap.h"

#ifdef ENABLE_DEV_REMOTE_FLASH

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstring>

namespace PocketDaily::HeapMap {

namespace {
int line(char*& p, size_t& left, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  const int n = vsnprintf(p, left, fmt, args);
  va_end(args);
  if (n <= 0 || static_cast<size_t>(n) >= left) return 0;
  p += n;
  left -= static_cast<size_t>(n);
  return n;
}
}  // namespace

size_t render(char* out, size_t cap) {
  char* p = out;
  size_t left = cap;

  line(p, left, "# heap capabilities\n");
  const uint32_t capsList[] = {MALLOC_CAP_8BIT, MALLOC_CAP_32BIT, MALLOC_CAP_INTERNAL, MALLOC_CAP_DMA};
  for (uint32_t caps : capsList) {
    multi_heap_info_t info;
    heap_caps_get_info(&info, caps);
    line(p, left, "caps=%08x total=%lu free=%lu largest=%lu minFree=%lu\n", static_cast<unsigned long>(caps),
         static_cast<unsigned long>(info.total_free_bytes + info.total_allocated_bytes),
         static_cast<unsigned long>(info.total_free_bytes), static_cast<unsigned long>(info.largest_free_block),
         static_cast<unsigned long>(info.minimum_free_bytes));
  }

  line(p, left, "# tasks (stack high-water = never-used bytes)\n");
  const size_t maxTasks = 24;
  TaskStatus_t tasks[maxTasks];
  UBaseType_t count = uxTaskGetSystemState(tasks, maxTasks, nullptr);
  for (UBaseType_t i = 0; i < count; i++) {
    line(p, left, "task=%-16s stackFree=%lu prio=%lu core=%lu\n", tasks[i].pcTaskName,
         static_cast<unsigned long>(tasks[i].usStackHighWaterMark * sizeof(StackType_t)),
         static_cast<unsigned long>(tasks[i].uxCurrentPriority), static_cast<unsigned long>(tasks[i].xCoreID));
  }

  line(p, left, "# uptime=%lu ms\n", static_cast<unsigned long>(esp_timer_get_time() / 1000));
  return cap - left;
}

}  // namespace PocketDaily::HeapMap

#else

namespace PocketDaily::HeapMap {
size_t render(char*, size_t) { return 0; }
}  // namespace PocketDaily::HeapMap

#endif
