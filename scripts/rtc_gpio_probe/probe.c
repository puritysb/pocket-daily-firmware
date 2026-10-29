#include <stdint.h>

#include "sdkconfig.h"
#include "soc/gpio_reg.h"
#include "soc/io_mux_reg.h"
#include "soc/rtc_cntl_reg.h"
typedef uint32_t u32;
#define R(a) (*(volatile u32*)(a))
#define POWER (1u << 3)
#define ENTRY_REG RTC_CNTL_STORE6_REG
#define DEFAULT_ENTRY 0x50000000u
#define THIS_ENTRY ((u32)(uintptr_t)entry)
#define TRACE ((volatile u32*)0x50001a80u)
#define FENCE() __asm__ volatile("fence iorw, iorw" ::: "memory")
static __attribute__((noinline)) void restore_wdt(volatile u32* t) {
  R(RTC_CNTL_WDTWPROTECT_REG) = 0x50d83aa1u;
  R(RTC_CNTL_WDTCONFIG0_REG) &= ~RTC_CNTL_WDT_EN;
  R(RTC_CNTL_WDTCONFIG1_REG) = t[17];
  R(RTC_CNTL_WDTFEED_REG) = RTC_CNTL_WDT_FEED;
  R(RTC_CNTL_WDTCONFIG0_REG) = t[16];
  R(RTC_CNTL_WDTWPROTECT_REG) = 0;
}
static void restore_button(volatile u32* t) {
  R(RTC_CNTL_PAD_HOLD_REG) &= ~POWER;
  R(GPIO_ENABLE_W1TC_REG) = POWER;
  R(GPIO_PIN3_REG) = t[18];
  R(GPIO_FUNC3_OUT_SEL_CFG_REG) = t[19];
}
__attribute__((section(".text.entry"))) void entry(void) {
  ((void (*)(void))DEFAULT_ENTRY)();
  volatile u32* t = TRACE;
  R(ENTRY_REG) = DEFAULT_ENTRY;
  if (t[0] != 0x50445731u || !t[1] || t[31] != 1) return;
  if (t[2] == 1) {
    // This branch is reached only by ROM re-entry after the second sleep.
    t[5] = R(RTC_CNTL_SLP_WAKEUP_CAUSE_REG) & RTC_CNTL_WAKEUP_CAUSE;
    t[6] = R(RTC_CNTL_GPIO_WAKEUP_REG) & RTC_CNTL_GPIO_WAKEUP_STATUS;
    // Restore the ROM's original boot watchdog configuration, not a new app policy.
    restore_wdt(t);
    FENCE();
    t[2] = 2;
    // Keep only GPIO3 held LOW through native power-wake verification. The
    // matched diagnostic app/return bridge releases it through the HAL.
    return;
  }
  if (t[2] != 0) return;
  t[3] = R(RTC_CNTL_SLP_WAKEUP_CAUSE_REG) & RTC_CNTL_WAKEUP_CAUSE;
  t[4] = R(RTC_CNTL_GPIO_WAKEUP_REG) & RTC_CNTL_GPIO_WAKEUP_STATUS;
  t[16] = R(RTC_CNTL_WDTCONFIG0_REG);
  t[17] = R(RTC_CNTL_WDTCONFIG1_REG);
  t[18] = R(GPIO_PIN3_REG);
  t[19] = R(GPIO_FUNC3_OUT_SEL_CFG_REG);
  t[20] = R(RTC_CNTL_PAD_HOLD_REG) & POWER;
  t[21] = R(RTC_CNTL_WAKEUP_STATE_REG);
  // Refuse unknown pin ownership, a held real button, or a non-native reject policy.
  if (t[3] != 8 || t[4] != 0 || t[20] != POWER || (R(GPIO_ENABLE_REG) & POWER) || !(R(GPIO_IN_REG) & POWER) ||
      (t[19] & (GPIO_FUNC3_OUT_SEL_M | GPIO_FUNC3_OUT_INV_SEL | GPIO_FUNC3_OEN_INV_SEL)) != 128 ||
      !(R(IO_MUX_GPIO3_REG) & FUN_IE) || (R(RTC_CNTL_SLP_REJECT_CONF_REG) & RTC_CNTL_SLEEP_REJECT_ENA_M) ||
      !(R(RTC_CNTL_GPIO_WAKEUP_REG) & RTC_CNTL_GPIO_PIN3_WAKEUP_ENABLE)) {
    t[22] = 1;
    t[2] = 4;
    return;
  }
  // Independent fallbacks: a future RTC timer and a bounded RTC watchdog while
  // awake if resleep is rejected. No timer/cause is relabeled as GPIO.
  const u32 before = R(RTC_CNTL_SLP_TIMER0_REG), next = before + 1000000u;
  const u32 high = (R(RTC_CNTL_SLP_TIMER1_REG) & RTC_CNTL_SLP_VAL_HI) + (next < before);
  R(RTC_CNTL_SLP_TIMER0_REG) = next;
  R(RTC_CNTL_SLP_TIMER1_REG) = high;
  R(RTC_CNTL_INT_CLR_REG) = RTC_CNTL_MAIN_TIMER_INT_CLR;
  R(RTC_CNTL_SLP_TIMER1_REG) = high | RTC_CNTL_MAIN_TIMER_ALARM_EN;
  R(RTC_CNTL_WDTWPROTECT_REG) = 0x50d83aa1u;
  R(RTC_CNTL_WDTCONFIG0_REG) &= ~RTC_CNTL_WDT_EN;
  R(RTC_CNTL_WDTCONFIG1_REG) = 2000000u;
  R(RTC_CNTL_WDTFEED_REG) = RTC_CNTL_WDT_FEED;
  R(RTC_CNTL_WDTCONFIG0_REG) =
      (t[16] & ~(RTC_CNTL_WDT_STG0_M | RTC_CNTL_WDT_STG1_M | RTC_CNTL_WDT_STG2_M | RTC_CNTL_WDT_STG3_M)) |
      RTC_CNTL_WDT_EN | (RTC_WDT_STG_SEL_RESET_SYSTEM << RTC_CNTL_WDT_STG0_S);
  R(RTC_CNTL_WDTWPROTECT_REG) = 0;
  // Native deep sleep holds GPIO3 as an input. Release only that proven hold
  // before the owned open-drain actuator is configured; other pins stay held.
  R(RTC_CNTL_PAD_HOLD_REG) &= ~POWER;
  R(GPIO_PIN3_REG) = t[18] | GPIO_PIN3_PAD_DRIVER;
  R(GPIO_OUT_W1TC_REG) = POWER;
  R(GPIO_FUNC3_OUT_SEL_CFG_REG) = 128 | GPIO_FUNC3_OEN_SEL;
  R(GPIO_ENABLE_W1TS_REG) = POWER;
  FENCE();
  unsigned tries = 1000;
  while ((R(GPIO_IN_REG) & POWER) && --tries) {
    __asm__ volatile("nop");
  }
  if (!tries) {
    restore_button(t);
    restore_wdt(t);
    t[22] = 2;
    t[2] = 4;
    return;
  }
  R(RTC_CNTL_PAD_HOLD_REG) |= POWER;
  R(RTC_CNTL_INT_CLR_REG) = RTC_CNTL_SLP_REJECT_INT_CLR | RTC_CNTL_SLP_WAKEUP_INT_CLR;
  R(RTC_CNTL_STATE0_REG) |= RTC_CNTL_SLP_REJECT_CAUSE_CLR;
  t[2] = 1;
  R(ENTRY_REG) = THIS_ENTRY;
  FENCE();
  ((void (*)(void))0x40000034u)();  // Actual pinned C3 ROM set_rtc_memory_crc.
  R(RTC_CNTL_STATE0_REG) |= RTC_CNTL_SLEEP_EN;
  for (;;) {
    __asm__ volatile("wfi");
  }  // GPIO wake, timer fallback or RTC-WDT reset.
}
