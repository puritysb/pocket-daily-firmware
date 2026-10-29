#pragma once
namespace PocketDaily::Dev {
constexpr bool timerSleepSupported(bool x3, bool xteink, bool usbPower) { return x3 || (xteink && usbPower); }
}  // namespace PocketDaily::Dev
