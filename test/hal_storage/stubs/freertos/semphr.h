#pragma once
namespace FakeSDK {
inline int lockDepth = 0;
}
using SemaphoreHandle_t = void*;
inline constexpr unsigned portMAX_DELAY = ~0u;
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() { return reinterpret_cast<void*>(1); }
inline void xSemaphoreTakeRecursive(SemaphoreHandle_t, unsigned) { ++FakeSDK::lockDepth; }
inline void xSemaphoreGiveRecursive(SemaphoreHandle_t) { --FakeSDK::lockDepth; }
