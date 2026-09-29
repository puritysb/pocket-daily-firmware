#pragma once
// Host test X3 panel model: real framebuffer, strip grayscale composed into
// full planes for hashing, SPI byte and refresh counters. No waveform timing.
#include <Arduino.h>
#include <GrayscaleCapabilities.h>

#include <cstdint>
#include <cstring>
#include <vector>

struct PanelCounters {
  unsigned fast = 0, half = 0, full = 0, gray = 0, strips = 0, cleanups = 0;
  uint64_t spiBytes = 0;
};

class HalDisplay {
 public:
  enum RefreshMode { FULL_REFRESH, HALF_REFRESH, FAST_REFRESH };
  using GrayscaleMode = freeink::GrayscaleMode;
  using GrayscaleCapabilities = freeink::GrayscaleCapabilities;
  using GrayscaleBase = freeink::GrayscaleBase;
  using GrayscaleEncoding = freeink::GrayscaleEncoding;
  GrayscaleCapabilities grayscaleCapabilities(GrayscaleMode mode = GrayscaleMode::Overlay) const {
    if (mode != GrayscaleMode::Overlay) return {};
    return {GrayscaleEncoding::OverlayMasks, GrayscaleBase::Separate, supportsStripGrayscale(), false, false};
  }
  bool displayGrayscaleBase(GrayscaleMode mode, RefreshMode fallback, bool off = false);
  bool isInverted() const { return inverted_; }
  void setInverted(bool value) { inverted_ = value; }
  void displayBufferAsync(RefreshMode mode);
  void waitRefreshComplete() {}
  bool supportsAsyncRefresh() const { return false; }
  uint8_t* lendFrameBufferStorage(uint32_t* size) {
    *size = 0;
    return nullptr;
  }
  void returnFrameBufferStorage() {}
  static constexpr uint16_t DISPLAY_WIDTH = 792, DISPLAY_HEIGHT = 528, DISPLAY_WIDTH_BYTES = 99;
  static constexpr uint32_t BUFFER_SIZE = 99u * 528u;
  explicit HalDisplay(uint16_t = DISPLAY_WIDTH, uint16_t = DISPLAY_HEIGHT)
      : fb_(BUFFER_SIZE, 0xFF), lsb_(BUFFER_SIZE, 0), msb_(BUFFER_SIZE, 0) {}
  uint8_t* getFrameBuffer() const { return const_cast<uint8_t*>(fb_.data()); }
  uint16_t getDisplayWidth() const { return DISPLAY_WIDTH; }
  uint16_t getDisplayHeight() const { return DISPLAY_HEIGHT; }
  uint16_t getDisplayWidthBytes() const { return DISPLAY_WIDTH_BYTES; }
  uint32_t getBufferSize() const { return BUFFER_SIZE; }
  void clearScreen(uint8_t color = 0xFF) const { memset(getFrameBuffer(), color, BUFFER_SIZE); }
  void displayBuffer(RefreshMode mode = FAST_REFRESH, bool = false) {
    if (mode == FAST_REFRESH)
      ++c.fast;
    else if (mode == HALF_REFRESH)
      ++c.half;
    else
      ++c.full;
    c.spiBytes += 2ull * BUFFER_SIZE + 5 * 42;  // DTM2 new frame + DTM1 resync + LUT bank
    ++bwFrames;
  }
  void refreshDisplay(RefreshMode = FAST_REFRESH, bool = false) {}
  void drawImage(const uint8_t*, uint16_t, uint16_t, uint16_t, uint16_t, bool = false) const {}
  void drawImageTransparent(const uint8_t*, uint16_t, uint16_t, uint16_t, uint16_t, bool = false) const {}
  void displayGrayscaleBase(RefreshMode = HALF_REFRESH, bool = false) {}
  void preconditionGrayscale() {}
  void preconditionGrayscale(uint16_t, uint16_t, uint16_t, uint16_t) {}
  void copyGrayscaleLsbBuffers(const uint8_t* b) { memcpy(lsb_.data(), b, BUFFER_SIZE); }
  void copyGrayscaleMsbBuffers(const uint8_t* b) { memcpy(msb_.data(), b, BUFFER_SIZE); }
  void copyGrayscaleBuffers(const uint8_t* a, const uint8_t* b) {
    copyGrayscaleLsbBuffers(a);
    copyGrayscaleMsbBuffers(b);
  }
  void displayGrayBuffer(bool = false) {
    ++c.gray;
    c.spiBytes += 5 * 42;
    grayShown = true;
  }
  void writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* rows, uint16_t y0, uint16_t n) {
    ++c.strips;
    c.spiBytes += uint64_t(n) * DISPLAY_WIDTH_BYTES + 12;
    memcpy((lsbPlane ? lsb_ : msb_).data() + uint32_t(y0) * DISPLAY_WIDTH_BYTES, rows,
           uint32_t(n) * DISPLAY_WIDTH_BYTES);
  }
  bool supportsStripGrayscale() const { return strips; }
  void cleanupGrayscaleBuffers(const uint8_t*) {
    ++c.cleanups;
    c.spiBytes += 2ull * BUFFER_SIZE;
  }
  void deepSleep() {}

  static uint64_t hash(const std::vector<uint8_t>& v) {
    uint64_t h = 1469598103934665603ull;
    for (uint8_t b : v) h = (h ^ b) * 1099511628211ull;
    return h;
  }
  PanelCounters c;
  uint64_t bwHash = 0, lsbHash = 0, msbHash = 0;
  unsigned bwFrames = 0;
  bool strips = true;
  bool grayShown = false;
  // Hashes of the frame and planes as displayed (taken after the turn, outside timed stages).
  void snapshotHashes() {
    bwHash = hash(fb_);
    if (grayShown) {
      lsbHash = hash(lsb_);
      msbHash = hash(msb_);
    } else {
      lsbHash = msbHash = 0;
    }
    grayShown = false;
  }
  bool inverted_ = false;
  std::vector<uint8_t> fb_, lsb_, msb_;
};

inline bool HalDisplay::displayGrayscaleBase(GrayscaleMode mode, RefreshMode fallback, bool off) {
  if (mode != GrayscaleMode::Overlay) return false;
  displayGrayscaleBase(fallback, off);
  return true;
}
inline void HalDisplay::displayBufferAsync(RefreshMode mode) { displayBuffer(mode); }
