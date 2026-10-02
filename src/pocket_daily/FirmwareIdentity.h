#pragma once

namespace PocketDaily::FirmwareIdentity {

// Product releases use pocket-v tags. The lineage lets the companion tell
// version series apart, because the numbers alone do not order them:
//   none: the old 1.6.6 / 1.7.0 public test images,
//   1:    1.0.0-beta.* and 1.0.0-dev-* built before the series was reset,
//   2:    the 0.x development series (docs/product-versioning.md); a later
//         real 1.0.0 stays in lineage 2 and is newer than everything above.
inline constexpr int LINEAGE = 2;
inline constexpr char CROSSPOINT_BASE[] = "1.6.5";
inline constexpr char CROSSPOINT_COMMIT[] = "93e98bb78702e29868a16a13b80c40e6b36ccdff";

}  // namespace PocketDaily::FirmwareIdentity
