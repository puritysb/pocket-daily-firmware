#pragma once
//
// glance_frame_client.h — M8 stage 2: fetch the daemon's server-rendered
// glance frame (GET /glance-frame, packed 1bpp in PHYSICAL panel space) into
// an SD cache the render task can blit from.
//
// The cache only ever holds a complete frame: the download lands in a tmp
// file and replaces the cache by rename after the byte count matches the
// panel's framebuffer size. Rides the ?sig= conditional (X-Frame-Sig echo) so
// an unchanged frame costs a bodyless 304. On-device renderGlance stays the
// offline/old-daemon fallback — a Failed result must degrade, never blank.
//
#include <cstddef>
#include <cstdint>

namespace AgentDeck {
namespace GlanceFrame {

enum class Fetch : uint8_t {
  Fresh,      // 200 — cache replaced with a size-validated new frame
  Unchanged,  // 304 — cache already matches echoSig
  Failed,     // transport error / old daemon (404) / size mismatch
};

// SD path of the cached frame (complete frames only, physical orientation).
const char* cachePath();

// One bounded conditional GET against ip:port. expectedBytes must be the
// panel's physical framebuffer size (renderer.getBufferSize()) — anything
// else is rejected before it can smear a partial frame across the panel.
// echoSig may be "" (unconditional). On Fresh, sigOut receives X-Frame-Sig;
// echoSig and sigOut may alias (the URL is built before sigOut is written).
// Blocking (~1s LAN); call only from a shallow frame on the big loop stack —
// see the loop-task overflow note on refreshGlanceIfStale.
Fetch fetchToCache(const char* ip, uint16_t port, const char* token, const char* board, size_t expectedBytes,
                   const char* echoSig, char* sigOut, size_t sigCap);

}  // namespace GlanceFrame
}  // namespace AgentDeck
