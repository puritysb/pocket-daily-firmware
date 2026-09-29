#pragma once

namespace PocketDaily::FirmwareIdentity {

// Product releases use pocket-v tags. This marker lets the companion
// distinguish this lineage from the older 1.6.6/1.7.0 test distributions.
inline constexpr int LINEAGE = 1;
inline constexpr char CROSSPOINT_BASE[] = "1.6.5";
inline constexpr char CROSSPOINT_COMMIT[] = "93e98bb78702e29868a16a13b80c40e6b36ccdff";

}  // namespace PocketDaily::FirmwareIdentity
