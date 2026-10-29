#pragma once
#include <FreeInkUICore.h>

#include <cstdint>

class GfxRenderer;
namespace HeaderPainter {
// Everything the passive BaseTheme header consumes. No pointers, frame pixels,
// settings reads or guessed coordinates cross the host/device boundary.
struct Inputs {
  int32_t fonts[4]{};
  freeink::ui::TextStyle titleText{};
  freeink::ui::TextStyle subtitleText{};
  freeink::ui::BoxStyle normalStyle{};
  int16_t height = 45;
  int16_t sidePadding = 20;
  int16_t underline = 2;
  int16_t titleAlign = 0;
  int16_t batteryWidth = 15;
  int16_t batteryHeight = 12;
  int16_t stripHeight = 20;
  int16_t edgeInset = 24;
  int16_t actionOffsetY = 0;
  uint8_t batteryPercent = 0;
  bool charging = false;
  bool batteryLeft = false;
  bool clockCentered = false;
  bool showBack = false;
  char batteryLabel[8]{};
  char clock[10]{};
};
// Same painter called by BaseTheme and the native connected-reader preview.
// The SDK owns title reserves, typography, battery and clock geometry.
void paintHeader(const GfxRenderer& renderer, const Inputs& inputs, int x, int y, int width, int height,
                 const char* title, const char* subtitle);
}  // namespace HeaderPainter
