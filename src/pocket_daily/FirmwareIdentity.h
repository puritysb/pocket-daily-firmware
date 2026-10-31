#pragma once

#include <cstdio>
#include <cstring>

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

// What screens show (docs/product-versioning.md "Display"). The embedded
// version (updater comparison, /api/status, tag checks) is unchanged:
//   release   0.1.0                    -> "0.1.0"
//   beta      0.1.0-beta.3             -> "0.1.0 beta 3"
//   candidate 0.1.0-rc+<hash>          -> "0.1.0 RC"
//   developer 0.1.0-dev-<branch>-<commit>[-w<worktree>] -> "0.1.0 dev <commit>"
inline void displayVersion(const char* full, char* out, size_t cap) {
  if (!out || cap == 0) return;
  out[0] = '\0';
  if (!full) return;
  if (const char* beta = strstr(full, "-beta.")) {
    snprintf(out, cap, "%.*s beta %s", static_cast<int>(beta - full), full, beta + 6);
    return;
  }
  if (const char* rc = strstr(full, "-rc+")) {
    snprintf(out, cap, "%.*s RC", static_cast<int>(rc - full), full);
    return;
  }
  const char* dev = strstr(full, "-dev-");
  if (!dev) {
    snprintf(out, cap, "%s", full);
    return;
  }
  const char* end = full + strlen(full);
  const char* last = strrchr(full, '-');
  // Drop the worktree fingerprint ("-w" + 8 hex) when present.
  if (last && last > dev + 4 && last[1] == 'w' && end - last == 10) end = last;
  const char* commit = end;
  while (commit > dev + 5 && commit[-1] != '-') --commit;
  if (commit <= dev + 5 || end - commit < 7) {
    snprintf(out, cap, "%.*s dev", static_cast<int>(dev - full), full);
    return;
  }
  snprintf(out, cap, "%.*s dev %.*s", static_cast<int>(dev - full), full, static_cast<int>(end - commit), commit);
}

}  // namespace PocketDaily::FirmwareIdentity
