#pragma once
#include <Arduino.h>

#include <algorithm>
#include <stdexcept>
#include <vector>

// Host-only display adapter. All drawing uses the production
// GfxRenderer directly; hardware-only paths fail instead of inventing pixels.
class HalDisplay {
 public:
  enum RefreshMode { FULL_REFRESH, HALF_REFRESH, FAST_REFRESH };
  static constexpr uint16_t DISPLAY_WIDTH = 800, DISPLAY_HEIGHT = 480, DISPLAY_WIDTH_BYTES = 100;
  static constexpr uint32_t BUFFER_SIZE = 48000;
  explicit HalDisplay(uint16_t width = DISPLAY_WIDTH, uint16_t height = DISPLAY_HEIGHT)
      : width_(width), height_(height), bytes_(checkedSize(width, height), 0xA5) {
    clearScreen();
  }
  uint8_t* getFrameBuffer() const { return bytes_.data() + 16; }
  uint16_t getDisplayWidth() const { return width_; }
  uint16_t getDisplayHeight() const { return height_; }
  uint16_t getDisplayWidthBytes() const { return width_ / 8; }
  uint32_t getBufferSize() const { return uint32_t(width_ / 8) * height_; }
  void clearScreen(uint8_t color = 0xFF) const { std::fill_n(getFrameBuffer(), getBufferSize(), color); }
  void displayBuffer(RefreshMode = FAST_REFRESH, bool = false) { ++presentations; }
  bool guardsIntact() const {
    return std::all_of(bytes_.begin(), bytes_.begin() + 16, [](auto b) { return b == 0xA5; }) &&
           std::all_of(bytes_.end() - 16, bytes_.end(), [](auto b) { return b == 0xA5; });
  }
  void drawImage(const uint8_t*, uint16_t, uint16_t, uint16_t, uint16_t) const { unsupported(); }
  void drawImageTransparent(const uint8_t*, uint16_t, uint16_t, uint16_t, uint16_t) const { unsupported(); }
  void displayGrayscaleBase(RefreshMode, bool) { unsupported(); }
  void preconditionGrayscale() { unsupported(); }
  void preconditionGrayscale(uint16_t, uint16_t, uint16_t, uint16_t) { unsupported(); }
  void copyGrayscaleLsbBuffers(const uint8_t*) { unsupported(); }
  void copyGrayscaleMsbBuffers(const uint8_t*) { unsupported(); }
  void displayGrayBuffer(bool) { unsupported(); }
  void writeGrayscalePlaneStrip(bool, const uint8_t*, uint16_t, uint16_t) { unsupported(); }
  bool supportsStripGrayscale() const { return false; }
  void cleanupGrayscaleBuffers(const uint8_t*) { unsupported(); }
  unsigned presentations = 0;

 private:
  // Host heap, not device DRAM: at most512KiB +32B guards, owned for the panel
  // lifetime. Too large for stack; no allocation while rasterizing.
  static size_t checkedSize(uint16_t width, uint16_t height) {
    if (!width || width % 8 || !height || width > 2048 || height > 2048)
      throw std::invalid_argument("Invalid host panel dimensions");
    return size_t(width / 8) * height + 32;
  }
  [[noreturn]] static void unsupported() { throw std::logic_error("Unsupported host display operation"); }
  uint16_t width_, height_;
  mutable std::vector<uint8_t> bytes_;
};
