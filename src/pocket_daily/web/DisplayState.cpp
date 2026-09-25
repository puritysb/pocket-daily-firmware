#include "DisplayState.h"

#include <cstdio>

namespace PocketDaily::Web {
namespace {
class Writer {
 public:
  Writer(char* out, size_t cap) : out_(out), cap_(cap) {
    if (cap_) out_[0] = '\0';
  }
  void raw(const char* text) {
    while (*text) put(*text++);
  }
  void number(long value) {
    char digits[16];
    snprintf(digits, sizeof(digits), "%ld", value);
    raw(digits);
  }
  // JSON string with RFC 8259 escaping; UTF-8 bytes pass through unchanged.
  void string(const char* text) {
    put('"');
    for (const auto* p = reinterpret_cast<const unsigned char*>(text ? text : ""); *p; ++p) {
      if (*p == '"' || *p == '\\') {
        put('\\');
        put(static_cast<char>(*p));
      } else if (*p < 0x20) {
        char escaped[7];
        snprintf(escaped, sizeof(escaped), "\\u%04x", *p);
        raw(escaped);
      } else {
        put(static_cast<char>(*p));
      }
    }
    put('"');
  }
  size_t finish() {
    if (overflow_ || used_ >= cap_) return 0;
    out_[used_] = '\0';
    return used_;
  }

 private:
  void put(char c) {
    if (used_ + 1 >= cap_) {
      overflow_ = true;
      return;
    }
    out_[used_++] = c;
  }
  char* out_;
  size_t cap_;
  size_t used_ = 0;
  bool overflow_ = false;
};
}  // namespace

const char* themeName(const uint8_t uiTheme) {
  switch (uiTheme) {
    case 0:
      return "classic";
    case 1:
      return "lyra";
    case 2:
      return "lyra3covers";
    case 3:
      return "roundedraff";
    default:
      return "unknown";
  }
}

size_t writeDisplayJson(const DisplayInputs& in, char* out, const size_t cap) {
  if (!out || !cap || in.orientation > 3) return 0;
  Writer w(out, cap);
  w.raw("{\"schema\":1,\"deviceID\":");
  w.string(in.deviceId);
  w.raw(",\"theme\":");
  w.string(in.theme);
  w.raw(",\"orientation\":");
  w.number(in.orientation);
  w.raw(",\"font\":{\"family\":");
  w.string(in.fontFamily);
  w.raw(",\"pointSize\":");
  w.number(in.fontPointSize);
  w.raw("},\"contentPage\":{\"sidePadding\":");
  w.number(in.sidePadding);
  w.raw(",\"topPadding\":");
  w.number(in.topPadding);
  w.raw(",\"spacing\":");
  w.number(in.spacing);
  w.raw(",\"title\":");
  w.string(in.title);
  w.raw(",\"empty\":");
  w.string(in.empty);
  w.raw(",\"labels\":[");
  for (int i = 0; i < 4; ++i) {
    if (i) w.raw(",");
    w.string(in.labels[i]);
  }
  w.raw("]}}");
  return w.finish();
}
}  // namespace PocketDaily::Web
