#pragma once

#include <cstddef>
#include <cstdint>

// .uipack v1 container (docs/live-studio-v1.md). Pure parsing/validation with
// no Arduino types so the host suite exercises it. Layout (little-endian):
//
// Header (120 bytes):
//   magic[4] "PDUI", version u8 = 1, reserved u8 x3,
//   name char[32] (NUL-padded), packVersion char[16], minFirmware char[16],
//   themeOverrides u16, strings u16, assets u16, reserved u16,
//   payloadLen u32, crc32 u32 (over payload), sha256 u8 x32 (zeros = absent)
//
// Payload records:
//   ThemeOverride: fieldId u16, type u8 (1=int 2=bool 3=float), value s32
//     (float travels as raw bits)
//   String: langIndex u8, keyHash u32, len u16, utf8 bytes (<= 128)
//   Asset (v1: fonts only): kind u8 = 1, nameLen u8, name, size u32,
//     crc32 u32, bytes
namespace PocketDaily::LiveStudio {

inline constexpr uint8_t UIPACK_HEADER_SIZE = 120;
inline constexpr size_t UIPACK_MAX_THEME_OVERRIDES = 96;
inline constexpr size_t UIPACK_MAX_STRINGS = 32;
inline constexpr size_t UIPACK_MAX_ASSETS = 2;
inline constexpr size_t UIPACK_MAX_TOTAL = 4U * 1024U * 1024U;
inline constexpr size_t UIPACK_MAX_STRING_BYTES = 128;
inline constexpr size_t UIPACK_MAX_ASSET_BYTES = 14U * 1024U * 1024U;

enum class UiPackResult : uint8_t {
  Ok = 0,
  TooSmall,
  BadMagic,
  BadVersion,
  BadHeaderFields,
  PayloadSizeMismatch,
  BadCrc,
  BadSha,
  UnknownField,
  BadRecord,
  ReadFail,
};

struct ThemeOverride {
  uint16_t fieldId;
  uint8_t type;
  int32_t value;
};

struct UiPackInfo {
  char name[33];
  char packVersion[17];
  char minFirmware[17];
  size_t themeOverrideCount;
  size_t stringCount;
  size_t assetCount;
};

// Validates the whole container. On Ok, `info` is filled and up to
// `overridesCap` theme overrides are copied to `overrides` (a truncated copy
// is an error: the caller must size for every override).
UiPackResult validatePack(const uint8_t* data, size_t size, UiPackInfo* info, ThemeOverride* overrides,
                          size_t overridesCap);

struct UiPackSource {
  void* context;
  // Caller must keep the source unchanged throughout both validation passes.
  // Exact, bounded read. The validator never requests more than 128 bytes.
  bool (*readAt)(void* context, size_t offset, uint8_t* output, size_t bytes);
  // Optional digest sink, called once for each payload byte during CRC validation.
  bool (*digestPayload)(void* context, const uint8_t* data, size_t bytes) = nullptr;
};
UiPackResult validatePackSource(const UiPackSource& source, size_t size, UiPackInfo* info, ThemeOverride* overrides,
                                size_t overridesCap, uint8_t* expectedSha256 = nullptr);

const char* uiPackResultName(UiPackResult r);

// CRC-32 (IEEE, reflected) - also used by the pack builder and companion app.
uint32_t uiPackCrc32(const uint8_t* data, size_t size);

}  // namespace PocketDaily::LiveStudio
