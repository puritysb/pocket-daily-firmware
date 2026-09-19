#include "UiPack.h"

#include <cstring>

#include "ThemeFieldIds.h"

namespace PocketDaily::LiveStudio {

uint32_t uiPackCrc32(const uint8_t* data, size_t size) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < size; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}

namespace {

uint32_t readU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t readU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

// NUL-padded fixed field copy.
void copyField(char* dst, size_t cap, const uint8_t* src, size_t len) {
  const size_t n = len < cap - 1 ? len : cap - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

}  // namespace

UiPackResult validatePack(const uint8_t* data, size_t size, UiPackInfo* info, ThemeOverride* overrides,
                          size_t overridesCap) {
  if (size < UIPACK_HEADER_SIZE) return UiPackResult::TooSmall;
  if (memcmp(data, "PDUI", 4) != 0) return UiPackResult::BadMagic;
  if (data[4] != 1) return UiPackResult::BadVersion;

  const uint16_t themeCount = readU16(data + 64);
  const uint16_t stringCount = readU16(data + 66);
  const uint16_t assetCount = readU16(data + 68);
  const uint32_t payloadLen = readU32(data + 72);
  const uint32_t crc = readU32(data + 76);
  (void)data;  // sha at offset 80 checked by the device loader

  if (themeCount > UIPACK_MAX_THEME_OVERRIDES || stringCount > UIPACK_MAX_STRINGS || assetCount > UIPACK_MAX_ASSETS) {
    return UiPackResult::BadHeaderFields;
  }
  if (payloadLen > UIPACK_MAX_TOTAL || static_cast<size_t>(UIPACK_HEADER_SIZE) + payloadLen != size) {
    return UiPackResult::PayloadSizeMismatch;
  }
  if (overrides != nullptr && overridesCap < themeCount) return UiPackResult::BadRecord;

  const uint8_t* payload = data + UIPACK_HEADER_SIZE;
  if (uiPackCrc32(payload, payloadLen) != crc) return UiPackResult::BadCrc;
  // SHA-256 lives in the device loader (mbedtls); the pure validator checks
  // CRC only so the host suite needs no crypto.

  // Walk the records.
  size_t off = 0;
  for (uint16_t i = 0; i < themeCount; i++) {
    if (off + 7 > payloadLen) return UiPackResult::BadRecord;
    const uint16_t fieldId = readU16(payload + off);
    const uint8_t type = payload[off + 2];
    if (fieldId >= ThemeField::kFieldCount || type != ThemeField::kFields[fieldId].type) {
      return UiPackResult::UnknownField;
    }
    if (overrides != nullptr) {
      overrides[i].fieldId = fieldId;
      overrides[i].type = type;
      overrides[i].value = static_cast<int32_t>(readU32(payload + off + 3));
    }
    off += 7;
  }
  for (uint16_t i = 0; i < stringCount; i++) {
    if (off + 7 > payloadLen) return UiPackResult::BadRecord;
    const uint16_t len = readU16(payload + off + 5);
    if (len > UIPACK_MAX_STRING_BYTES) return UiPackResult::BadRecord;
    if (off + 7 + len > payloadLen) return UiPackResult::BadRecord;
    off += 7 + len;
  }
  for (uint16_t i = 0; i < assetCount; i++) {
    if (off + 7 > payloadLen) return UiPackResult::BadRecord;
    const uint8_t nameLen = payload[off + 1];
    if (nameLen == 0 || off + 7 + nameLen > payloadLen) return UiPackResult::BadRecord;
    if (off + 7 + nameLen + 8 > payloadLen) return UiPackResult::BadRecord;
    const uint32_t assetSize = readU32(payload + off + 7 + nameLen);
    if (assetSize > UIPACK_MAX_ASSET_BYTES) return UiPackResult::BadRecord;
    if (off + 7 + nameLen + 8 + assetSize > payloadLen) return UiPackResult::BadRecord;
    off += 7 + nameLen + 8 + assetSize;
  }
  if (off != payloadLen) return UiPackResult::BadRecord;

  if (info != nullptr) {
    copyField(info->name, sizeof(info->name), data + 8, 32);
    copyField(info->packVersion, sizeof(info->packVersion), data + 40, 16);
    copyField(info->minFirmware, sizeof(info->minFirmware), data + 56, 16);
    info->themeOverrideCount = themeCount;
    info->stringCount = stringCount;
    info->assetCount = assetCount;
  }
  return UiPackResult::Ok;
}

const char* uiPackResultName(UiPackResult r) {
  switch (r) {
    case UiPackResult::Ok:
      return "ok";
    case UiPackResult::TooSmall:
      return "too small";
    case UiPackResult::BadMagic:
      return "bad magic";
    case UiPackResult::BadVersion:
      return "bad version";
    case UiPackResult::BadHeaderFields:
      return "bad header counts";
    case UiPackResult::PayloadSizeMismatch:
      return "payload size mismatch";
    case UiPackResult::BadCrc:
      return "crc mismatch";
    case UiPackResult::BadSha:
      return "sha256 mismatch";
    case UiPackResult::UnknownField:
      return "unknown field";
    case UiPackResult::BadRecord:
      return "malformed record";
  }
  return "?";
}

}  // namespace PocketDaily::LiveStudio
