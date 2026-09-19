#pragma once

#include <cstdint>

// LS-2 live frame capture state (`docs/live-studio-v1.md`).
//
// The render task captures the 1-bit framebuffer to a transient BMP after a
// completed render pass while a companion is subscribed; the web server's
// activity loop notices and pushes a `frame` event. All state here is atomic
// because the two sides live on different FreeRTOS tasks. The single-slot
// semantic is deliberate: a newer frame replaces an unfetched older one.
namespace PocketDaily::LiveFrameCapture {

inline constexpr char LIVE_FRAME_PATH[] = "/.crosspoint/live-frame.bmp";

// Called by the web server when a live-studio subscription starts or stops.
// `minIntervalMs` is clamped against the documented floor by the caller.
void setRequested(bool on, uint32_t minIntervalMs);
bool requested();

// Called from the render task after each completed render pass. Cheap when
// nobody subscribed; otherwise gated by the capture policy (heap floor and
// spacing) and writes the BMP row-by-row without a framebuffer allocation.
void maybeCapture(const uint8_t* framebuffer, int width, int height);

uint32_t seq();
uint32_t bytes();

// Web-server side: atomically read-and-clear the "a new frame landed" flag.
bool consumeReady();

// Test/reset hook used when the server stops.
void clear();

}  // namespace PocketDaily::LiveFrameCapture
