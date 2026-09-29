#pragma once

#include <cstdint>
#include <cstring>

namespace FirmwareVersion {

struct Parsed {
  uint16_t major = 0;
  uint16_t minor = 0;
  uint16_t patch = 0;
  char suffix = '\0';
  bool valid = false;
};

inline Parsed parse(const char* text, const bool running) {
  Parsed result;
  if (!text) return result;
  uint16_t* parts[] = {&result.major, &result.minor, &result.patch};
  for (unsigned part = 0; part < 3; ++part) {
    if (*text < '0' || *text > '9') return result;
    unsigned value = 0;
    do {
      value = value * 10 + static_cast<unsigned>(*text++ - '0');
      if (value > 9999) return result;
    } while (*text >= '0' && *text <= '9');
    *parts[part] = static_cast<uint16_t>(value);
    if (part < 2 && *text++ != '.') return result;
  }
  result.suffix = *text;
  result.valid = *text == '\0' || (running && (*text == '-' || *text == '+'));
  return result;
}

// /releases/latest is a stable product tag. The old v1.x tags belong to the
// pre-launch lineage and must never be offered to a new Pocket Daily image.
inline bool isNewerStable(const char* tag, const char* running) {
  constexpr char PREFIX[] = "pocket-v";
  if (!tag || std::strncmp(tag, PREFIX, sizeof(PREFIX) - 1) != 0) return false;
  const Parsed next = parse(tag + sizeof(PREFIX) - 1, false);
  const Parsed current = parse(running, true);
  if (!next.valid || !current.valid) return false;
  if (next.major != current.major) return next.major > current.major;
  if (next.minor != current.minor) return next.minor > current.minor;
  if (next.patch != current.patch) return next.patch > current.patch;
  return current.suffix == '-';  // beta/rc/dev -> stable at the same number
}

}  // namespace FirmwareVersion
