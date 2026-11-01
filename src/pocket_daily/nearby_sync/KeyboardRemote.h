#pragma once

#include <cstdint>

class GfxRenderer;

// Protocol and lifecycle mutations belong to the loop task, serialized with
// RenderLock. Render-task state queries/buffer access require that same lock;
// a render therefore keeps one mode and its scratch alive through completion.
// renderFinished/requestSlowRender and diagnostic counters are atomic hooks.
// RAM-only opt-in: never persisted, never rearmed on wake or book reopen.
namespace Pocket::KeyboardRemote {
enum class Failure : uint8_t { None, Pairing, Battery, Memory, Busy, Startup };
Failure takeFailure();
void arm();
void stop();
void pauseForLayout();
bool active();
bool preparing();
bool streaming();
bool idle();
uint8_t* streamBuffer();
constexpr uint32_t STREAM_BYTES = 4096;
void requestSlowRender();
void recordMemory();
void recordStreamingTurn();
uint32_t minimumFree();
uint32_t minimumBlock();
uint32_t streamingTurns();
Failure lastFailure();
bool takeActivity();
bool takeRepaint();
void loop(GfxRenderer& renderer, bool pageReady);
int8_t takeTurn();
void turnAccepted(bool accepted);
void renderFinished();
}  // namespace Pocket::KeyboardRemote
