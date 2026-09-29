#include "HalSystem.h"

#include <string>

#include "Arduino.h"
#include "HalStorage.h"
#include "Logging.h"
#include "esp_debug_helpers.h"
#include "esp_memory_utils.h"
#include "esp_private/esp_cpu_internal.h"
#include "esp_private/esp_system_attr.h"
#include "esp_private/panic_internal.h"
#include "esp_task_wdt.h"
#if !__riscv
#include <xtensa_context.h>  // XtExcFrame for the stack capture below
#endif

#define MAX_PANIC_STACK_DEPTH 32
#define PANIC_CAPTURE_MAGIC 0x50414E49u

void HalSystem::feedWatchdogIfRegistered() {
  if (esp_task_wdt_status(nullptr) == ESP_OK) esp_task_wdt_reset();
}

RTC_NOINIT_ATTR char panicMessage[256];
RTC_NOINIT_ATTR HalSystem::StackFrame panicStack[MAX_PANIC_STACK_DEPTH];
RTC_NOINIT_ATTR char crashBreadcrumb[96];
RTC_NOINIT_ATTR uint32_t crashBreadcrumbMagic;
// Snapshot of the previous boot's last breadcrumb, taken before clearPanic().
static char previousBootBreadcrumb[96];
static constexpr uint32_t CRASH_BREADCRUMB_MAGIC = 0x43504243;  // CPBC
// RTC_NOINIT is uninitialized on cold boot, so only this exact marker proves a
// panic diagnostic was captured before the reset.
RTC_NOINIT_ATTR volatile uint32_t panicCaptureMarker;

extern "C" {

void __real_panic_abort(const char* message);
void __real_panic_print_backtrace(const void* frame, int core);

static DRAM_ATTR const char PANIC_REASON_UNKNOWN[] = "(unknown panic reason)";
void IRAM_ATTR __wrap_panic_abort(const char* message) {
  if (!message) message = PANIC_REASON_UNKNOWN;
  // IRAM-safe bounded copy (strncpy is not IRAM-safe in panic context)
  int i = 0;
  for (; i < (int)sizeof(panicMessage) - 1 && message[i]; i++) {
    panicMessage[i] = message[i];
  }
  panicMessage[i] = '\0';
  panicCaptureMarker = PANIC_CAPTURE_MAGIC;

  __real_panic_abort(message);
}

void IRAM_ATTR __wrap_panic_print_backtrace(const void* frame, int core) {
  if (!frame) {
    __real_panic_print_backtrace(frame, core);
    return;
  }

  for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
    panicStack[i].sp = 0;
  }

  // Stack window dump, mirroring components/esp_system/port/arch/*/panic_arch.c.
  // Hardware exceptions never reach __wrap_panic_abort, so on both
  // architectures this dump is the only diagnostic a crash leaves on-device.
#if __riscv
  const uint32_t sp = (uint32_t)((RvExcFrame*)frame)->sp;
#else
  const uint32_t sp = (uint32_t)((XtExcFrame*)frame)->a1;
#endif
  constexpr uint32_t captureBytes = 1024;
  if (!esp_stack_ptr_is_sane(sp) || sp > UINT32_MAX - captureBytes ||
      !esp_ptr_in_dram(reinterpret_cast<const void*>(sp + captureBytes - 1))) {
    __real_panic_print_backtrace(frame, core);
    return;
  }
  const int per_line = 8;
  int depth = 0;
  for (int x = 0; x < captureBytes; x += per_line * sizeof(uint32_t)) {
    uint32_t* spp = (uint32_t*)(sp + x);
    panicStack[depth].sp = sp + x;
    for (int y = 0; y < per_line; y++) {
      panicStack[depth].spp[y] = spp[y];
    }

    depth++;
    if (depth >= MAX_PANIC_STACK_DEPTH) {
      break;
    }
  }
  panicCaptureMarker = PANIC_CAPTURE_MAGIC;

  __real_panic_print_backtrace(frame, core);
}
}

namespace HalSystem {

namespace {
constexpr const char* CRASH_REPORT_PATH = "/crash_report.txt";
constexpr const char* CRASH_REPORT_HISTORY[] = {
    "/crash_report.1.txt",
    "/crash_report.2.txt",
    "/crash_report.3.txt",
};

void rotateCrashReports() {
  // Preserve the most recent four reports. Fixed paths avoid timestamps (the
  // RTC may not be valid at panic reboot) and keep recovery deterministic.
  Storage.remove(CRASH_REPORT_HISTORY[2]);
  for (int i = 2; i > 0; --i) {
    if (Storage.exists(CRASH_REPORT_HISTORY[i - 1])) {
      Storage.rename(CRASH_REPORT_HISTORY[i - 1], CRASH_REPORT_HISTORY[i]);
    }
  }
  if (Storage.exists(CRASH_REPORT_PATH)) Storage.rename(CRASH_REPORT_PATH, CRASH_REPORT_HISTORY[0]);
}

const char* resetReasonName(const esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:
      return "power-on";
    case ESP_RST_EXT:
      return "external pin";
    case ESP_RST_SW:
      return "software restart";
    case ESP_RST_PANIC:
      return "panic";
    case ESP_RST_INT_WDT:
      return "interrupt watchdog";
    case ESP_RST_TASK_WDT:
      return "task watchdog";
    case ESP_RST_WDT:
      return "watchdog";
    case ESP_RST_DEEPSLEEP:
      return "deep-sleep wake";
    case ESP_RST_BROWNOUT:
      return "brownout";
    case ESP_RST_SDIO:
      return "SDIO";
    case ESP_RST_USB:
      return "USB";
    case ESP_RST_JTAG:
      return "JTAG";
    case ESP_RST_EFUSE:
      return "eFuse error";
    case ESP_RST_PWR_GLITCH:
      return "power glitch";
    case ESP_RST_CPU_LOCKUP:
      return "CPU lockup";
    case ESP_RST_UNKNOWN:
    default:
      return "unknown";
  }
}
}  // namespace

void begin() {
  // Preserve the breadcrumb from the boot that just ended before any clear path
  // wipes it. This is the only trace of a clean restart (no crash report), such
  // as a private-AP startup that returned to the launcher.
  if (crashBreadcrumbMagic == CRASH_BREADCRUMB_MAGIC && crashBreadcrumb[0]) {
    size_t i = 0;
    for (; i < sizeof(previousBootBreadcrumb) - 1 && crashBreadcrumb[i]; ++i)
      previousBootBreadcrumb[i] = crashBreadcrumb[i];
    previousBootBreadcrumb[i] = '\0';
  } else {
    previousBootBreadcrumb[0] = '\0';
  }

  // On a panic reboot, preserve diagnostics until checkPanic() has tried to write them to the SD card.
  // Ordinary boots clear any stale retained diagnostics.
  if (!isRebootFromPanic()) {
    clearPanic();
  } else {
    // Panic reboot: preserve logs and panic info, but clamp logHead in case the
    // panic occurred before begin() ever ran (e.g. in a static constructor).
    // If logHead was out of range, logMessages is also garbage — clear it so
    // getLastLogs() does not dump corrupt data into the crash report.
    if (sanitizeLogHead()) {
      clearLastLogs();
    }
  }
}

void checkPanic() {
  if (isRebootFromCrash()) {
    auto panicInfo = getPanicInfo(true);
    rotateCrashReports();
    auto file = Storage.open(CRASH_REPORT_PATH, O_WRITE | O_CREAT | O_TRUNC);
    if (file) {
      const size_t written = file.write(panicInfo.c_str(), panicInfo.size());
      file.close();
      if (written == panicInfo.size()) {
        // Keep the crash data for CrashActivity, but mark it consumed so a
        // later watchdog reset cannot be mistaken for this panic.
        panicCaptureMarker = 0;
        LOG_INF("SYS", "Dumped panic info to SD card");
      } else {
        LOG_ERR("SYS", "Failed to write complete crash report (%zu of %zu bytes)", written, panicInfo.size());
      }
    } else {
      LOG_ERR("SYS", "Failed to open crash_report.txt for writing");
    }
  }
}

void clearPanic() {
  panicCaptureMarker = 0;
  panicMessage[0] = '\0';
  for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
    panicStack[i].sp = 0;
  }
  crashBreadcrumb[0] = '\0';
  crashBreadcrumbMagic = CRASH_BREADCRUMB_MAGIC;
  clearLastLogs();
}

void setCrashBreadcrumb(const char* value) {
  crashBreadcrumbMagic = 0;
  size_t i = 0;
  if (value) {
    for (; i < sizeof(crashBreadcrumb) - 1 && value[i]; ++i) crashBreadcrumb[i] = value[i];
  }
  crashBreadcrumb[i] = '\0';
  crashBreadcrumbMagic = CRASH_BREADCRUMB_MAGIC;
}

const char* getPreviousBootBreadcrumb() { return previousBootBreadcrumb; }

const char* getResetReasonName() { return resetReasonName(esp_reset_reason()); }

std::string getPanicInfo(bool full) {
  if (!full) {
    if (panicMessage[0]) return panicMessage;
    if (isRebootFromCrash()) return std::string("Reset: ") + resetReasonName(esp_reset_reason());
    return {};
  } else {
    std::string info;

    info += "CrossPoint version: " CROSSPOINT_VERSION;
    // A lockup or hardware watchdog resets without running any panic hook, so
    // the reason and stack come back empty; the reset cause is then the only
    // way to tell those apart from a true panic.
    info += "\n\nReset reason: " + std::string(resetReasonName(esp_reset_reason()));
    info += "\n\nPanic reason: " + std::string(panicMessage);
    if (crashBreadcrumbMagic == CRASH_BREADCRUMB_MAGIC && crashBreadcrumb[0]) {
      info += "\n\nRuntime breadcrumb: " + std::string(crashBreadcrumb);
    }
    info += "\n\nLast logs:\n" + getLastLogs();
    info += "\n\nStack memory:\n";

    auto toHex = [](uint32_t value) {
      char buffer[9];
      snprintf(buffer, sizeof(buffer), "%08X", value);
      return std::string(buffer);
    };
    for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
      if (panicStack[i].sp == 0) {
        break;
      }
      info += "0x" + toHex(panicStack[i].sp) + ": ";
      for (size_t j = 0; j < 8; j++) {
        info += "0x" + toHex(panicStack[i].spp[j]) + " ";
      }
      info += "\n";
    }

    return info;
  }
}

bool isRebootFromPanic() {
  const auto resetReason = esp_reset_reason();
  if (resetReason == ESP_RST_PANIC || resetReason == ESP_RST_CPU_LOCKUP) {
    return true;
  }

  const bool watchdogReset =
      resetReason == ESP_RST_INT_WDT || resetReason == ESP_RST_TASK_WDT || resetReason == ESP_RST_WDT;
  return watchdogReset && panicCaptureMarker == PANIC_CAPTURE_MAGIC;
}

bool isRebootFromCrash() {
  switch (esp_reset_reason()) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
    case ESP_RST_EFUSE:
    case ESP_RST_PWR_GLITCH:
    case ESP_RST_CPU_LOCKUP:
      return true;
    default:
      return false;
  }
}

}  // namespace HalSystem
