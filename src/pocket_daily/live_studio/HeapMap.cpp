#include "HeapMap.h"

#if POCKET_HEAP_MAP_ENABLED

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>

namespace PocketDaily::HeapMap {

size_t renderCaps(char* out, size_t cap) {
  size_t used = 0;
  const uint32_t capsList[] = {MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL, MALLOC_CAP_DMA};
  used += static_cast<size_t>(snprintf(out + used, cap - used, "# caps total/free/largest/minEver\n"));
  for (uint32_t caps : capsList) {
    multi_heap_info_t info;
    heap_caps_get_info(&info, caps);
    used += static_cast<size_t>(
        snprintf(out + used, cap - used, "caps=%08lx %lu %lu %lu %lu\n", static_cast<unsigned long>(caps),
                 static_cast<unsigned long>(info.total_free_bytes + info.total_allocated_bytes),
                 static_cast<unsigned long>(info.total_free_bytes), static_cast<unsigned long>(info.largest_free_block),
                 static_cast<unsigned long>(info.minimum_free_bytes)));
  }
  return used < cap ? used : cap - 1;
}

size_t queryTasks(uintptr_t* tasks, size_t maxTasks) {
  // The caller's storage must hold TaskStatus_t; uintptr_t keeps the header
  // free of FreeRTOS includes.
  static_assert(sizeof(uintptr_t) > 0, "storage unit");
  TaskStatus_t* status = reinterpret_cast<TaskStatus_t*>(tasks);
  return uxTaskGetSystemState(status, maxTasks, nullptr);
}

size_t renderTaskBatch(const uintptr_t* tasks, size_t count, size_t from, size_t batch, char* out, size_t cap) {
  const TaskStatus_t* status = reinterpret_cast<const TaskStatus_t*>(tasks);
  size_t used = 0;
  if (from == 0) {
    used += static_cast<size_t>(snprintf(out + used, cap - used, "# tasks stackFree/prio\n"));
  }
  for (size_t i = from; i < count && i < from + batch; i++) {
    used +=
        static_cast<size_t>(snprintf(out + used, cap - used, "%-16s %lu %lu\n", status[i].pcTaskName,
                                     static_cast<unsigned long>(status[i].usStackHighWaterMark * sizeof(StackType_t)),
                                     static_cast<unsigned long>(status[i].uxCurrentPriority)));
    if (used >= cap - 64) break;
  }
  return used < cap ? used : cap - 1;
}

}  // namespace PocketDaily::HeapMap

#else

namespace PocketDaily::HeapMap {
size_t renderCaps(char*, size_t) { return 0; }
size_t queryTasks(uintptr_t*, size_t) { return 0; }
size_t renderTaskBatch(const uintptr_t*, size_t, size_t, size_t, char*, size_t) { return 0; }
}  // namespace PocketDaily::HeapMap

#endif
