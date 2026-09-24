#include "UiPack.h"

#include <algorithm>
#include <cstring>

#include "ThemeFieldIds.h"

namespace PocketDaily::LiveStudio {

static uint32_t accumulateCrc32(uint32_t crc, const uint8_t* data, size_t size) {
  for (size_t i = 0; i < size; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return crc;
}

uint32_t uiPackCrc32(const uint8_t* data, size_t size) { return ~accumulateCrc32(0xFFFFFFFFu, data, size); }

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
  struct Memory {
    const uint8_t* data;
  } memory{data};
  const UiPackSource source{&memory, [](void* context, size_t offset, uint8_t* out, size_t count) {
                              const auto* input = static_cast<Memory*>(context)->data;
                              if (!input) return false;
                              memcpy(out, input + offset, count);
                              return true;
                            }};
  return validatePackSource(source, size, info, overrides, overridesCap);
}

static UiPackResult verifyPayload(const UiPackSource& source, size_t size, uint32_t expectedCrc) {
  uint8_t block[128];
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t offset = UIPACK_HEADER_SIZE; offset < size;) {
    const size_t count = std::min(sizeof(block), size - offset);
    if (!source.readAt(source.context, offset, block, count)) return UiPackResult::ReadFail;
    crc = accumulateCrc32(crc, block, count);
    if (source.digestPayload && !source.digestPayload(source.context, block, count)) return UiPackResult::ReadFail;
    offset += count;
  }
  return ~crc == expectedCrc ? UiPackResult::Ok : UiPackResult::BadCrc;
}

UiPackResult validatePackSource(const UiPackSource& source, size_t size, UiPackInfo* info, ThemeOverride* overrides,
                                size_t overridesCap, uint8_t* expectedSha256) {
  if (size < UIPACK_HEADER_SIZE) return UiPackResult::TooSmall;
  uint8_t data[UIPACK_HEADER_SIZE];
  if (!source.readAt || !source.readAt(source.context, 0, data, sizeof(data))) return UiPackResult::ReadFail;
  if (memcmp(data, "PDUI", 4) != 0) return UiPackResult::BadMagic;
  if (data[4] != 1) return UiPackResult::BadVersion;

  const uint16_t themeCount = readU16(data + 72);
  const uint16_t stringCount = readU16(data + 74);
  const uint16_t assetCount = readU16(data + 76);
  const uint32_t payloadLen = readU32(data + 80);
  const uint32_t crc = readU32(data + 84);

  if (themeCount > UIPACK_MAX_THEME_OVERRIDES || stringCount > UIPACK_MAX_STRINGS || assetCount > UIPACK_MAX_ASSETS) {
    return UiPackResult::BadHeaderFields;
  }
  if (payloadLen > UIPACK_MAX_TOTAL || static_cast<size_t>(UIPACK_HEADER_SIZE) + payloadLen != size) {
    return UiPackResult::PayloadSizeMismatch;
  }
  if (overrides != nullptr && overridesCap < themeCount) return UiPackResult::BadRecord;

  const auto integrity = verifyPayload(source, size, crc);
  if (integrity != UiPackResult::Ok) return integrity;
  // SHA-256 lives in the device loader (mbedtls); the pure validator checks
  // CRC only so the host suite needs no crypto.

  // Walk the records.
  size_t off = 0;
  uint8_t record[8];
  for (uint16_t i = 0; i < themeCount; i++) {
    if (off + 7 > payloadLen) return UiPackResult::BadRecord;
    if (!source.readAt(source.context, UIPACK_HEADER_SIZE + off, record, 7)) return UiPackResult::ReadFail;
    const uint16_t fieldId = readU16(record);
    const uint8_t type = record[2];
    if (fieldId >= ThemeField::kFieldCount || type != ThemeField::kFields[fieldId].type) {
      return UiPackResult::UnknownField;
    }
    const uint32_t value = readU32(record + 3);
    // Match the companion encoder even for packs from other LAN/SD clients.
    // IEEE-754 exponent all-ones denotes infinity or NaN, neither a metric.
    if ((type == 2 && value > 1) || (type == 3 && (value & 0x7F800000u) == 0x7F800000u)) {
      return UiPackResult::BadRecord;
    }
    // Resource/domain bounds, mirrored by UiPackEncoder. Cover height is a
    // divisor in Lyra3Covers; 2048 is a pack dimension budget, not a panel size.
    if (fieldId == ThemeField::kHomeCoverHeight && (value == 0 || value > 2048)) {
      return UiPackResult::BadRecord;
    }
    if (fieldId == ThemeField::kPopupTopOffsetRatio) {
      float ratio;
      memcpy(&ratio, &value, sizeof(ratio));
      if (ratio < 0.0f || ratio > 1.0f) return UiPackResult::BadRecord;
    }
    if (overrides != nullptr) {
      overrides[i].fieldId = fieldId;
      overrides[i].type = type;
      overrides[i].value = static_cast<int32_t>(value);
    }
    off += 7;
  }
  for (uint16_t i = 0; i < stringCount; i++) {
    if (off + 7 > payloadLen) return UiPackResult::BadRecord;
    if (!source.readAt(source.context, UIPACK_HEADER_SIZE + off, record, 7)) return UiPackResult::ReadFail;
    const uint16_t len = readU16(record + 5);
    if (len > UIPACK_MAX_STRING_BYTES) return UiPackResult::BadRecord;
    if (off + 7 + len > payloadLen) return UiPackResult::BadRecord;
    off += 7 + len;
  }
  for (uint16_t i = 0; i < assetCount; i++) {
    if (off + 7 > payloadLen) return UiPackResult::BadRecord;
    if (!source.readAt(source.context, UIPACK_HEADER_SIZE + off, record, 7)) return UiPackResult::ReadFail;
    const uint8_t nameLen = record[1];
    if (nameLen == 0 || off + 7 + nameLen > payloadLen) return UiPackResult::BadRecord;
    if (off + 7 + nameLen + 8 > payloadLen) return UiPackResult::BadRecord;
    if (!source.readAt(source.context, UIPACK_HEADER_SIZE + off + 7 + nameLen, record, 8))
      return UiPackResult::ReadFail;
    const uint32_t assetSize = readU32(record);
    if (assetSize > UIPACK_MAX_ASSET_BYTES) return UiPackResult::BadRecord;
    if (off + 7 + nameLen + 8 + assetSize > payloadLen) return UiPackResult::BadRecord;
    off += 7 + nameLen + 8 + assetSize;
  }
  if (off != payloadLen) return UiPackResult::BadRecord;

  if (expectedSha256) memcpy(expectedSha256, data + 88, 32);

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
    case UiPackResult::ReadFail:
      return "read failed";
  }
  return "?";
}

}  // namespace PocketDaily::LiveStudio
