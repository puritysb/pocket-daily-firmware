#pragma once

namespace PocketDaily {

// A transient, exact copy of the Pocket Daily framebuffer captured before the
// clean-heap Nearby Sync restart. The companion reads it over the local link;
// it is removed when the reader returns to Pocket Daily.
inline constexpr char SCREEN_PREVIEW_PATH[] = "/.crosspoint/pocket-screen-preview.bmp";

}  // namespace PocketDaily
