#include <HalMemory.h>
// Layout tests expose deterministic available RAM; this does not model real heap pressure.
HalMemory::HeapStats HalMemory::getDefaultHeap() { return {200000, 300000, 200000, 100000}; }
HalMemory::HeapStats HalMemory::getInternalHeap() { return getDefaultHeap(); }
HalMemory::HeapStats HalMemory::getPsramHeap() { return {}; }
