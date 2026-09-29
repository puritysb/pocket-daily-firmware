#pragma once
// Geometry only: legacy X3/X4 reader bezel insets; no board IO.
namespace BoardConfig {
struct ViewableInsets {
  int top = 9, right = 3, bottom = 3, left = 3;
};
struct HostBoard {
  ViewableInsets viewableInsets;
};
inline constexpr HostBoard ACTIVE{};
}  // namespace BoardConfig
