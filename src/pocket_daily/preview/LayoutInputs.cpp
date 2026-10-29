#include "LayoutInputs.h"

#include <cstring>
#include <new>
#include <type_traits>

#include "fontIds.h"
#include "pocket_daily/ContentChecksum.h"
#include "pocket_daily/TextValidation.h"

namespace PocketDaily::Preview {
namespace {
// The single field walk is the wire order; integers are explicit-width and
// strings carry a u16 byte length excluding NUL. Struct padding is never sent.
class Writer {
 public:
  uint8_t* bytes;
  size_t capacity;
  size_t at = 0;
  bool ok = true;
  void byte(uint8_t v) {
    if (at >= capacity)
      ok = false;
    else if (bytes)
      bytes[at] = v;
    ++at;
  }
  template <typename T>
  void number(const T& v) {
    using U = std::make_unsigned_t<T>;
    for (size_t n = 0; n < sizeof(T); ++n) byte(static_cast<uint8_t>(static_cast<U>(v) >> (8 * n)));
  }
  void flag(const bool& value) { byte(value ? 1 : 0); }
  template <size_t N>
  void text(const char (&value)[N]) {
    const size_t length = strnlen(value, N);
    if (length >= N || !Text::validUtf8(value, length, false)) {
      ok = false;
      return;
    }
    number(static_cast<uint16_t>(length));
    for (size_t i = 0; i < length; ++i) byte(static_cast<uint8_t>(value[i]));
  }
};
class Reader {
 public:
  const uint8_t* bytes;
  size_t size;
  size_t at = 0;
  bool ok = true;
  uint8_t byte() {
    if (at >= size) {
      ok = false;
      return 0;
    }
    return bytes[at++];
  }
  template <typename T>
  void number(T& value) {
    using U = std::make_unsigned_t<T>;
    U raw = 0;
    for (size_t n = 0; n < sizeof(T); ++n) raw |= static_cast<U>(byte()) << (8 * n);
    value = static_cast<T>(raw);
  }
  void flag(bool& value) {
    const auto raw = byte();
    if (raw > 1) ok = false;
    value = raw == 1;
  }
  template <size_t N>
  void text(char (&value)[N]) {
    uint16_t length = 0;
    number(length);
    if (!ok || length >= N || length > size - at ||
        !Text::validUtf8(reinterpret_cast<const char*>(bytes + at), length, false)) {
      ok = false;
      return;
    }
    memset(value, 0, N);
    memcpy(value, bytes + at, length);
    at += length;
  }
};
template <typename IO, typename D>
void day(IO& io, D& d) {
  io.text(d.date);
  io.text(d.summary);
  io.number(d.code);
  io.number(d.minC);
  io.number(d.maxC);
  io.number(d.rainProbability);
}
template <typename IO, typename T>
void style(IO& io, T& t) {
  io.number(t.font);
  io.number(t.align);
  io.number(t.color);
  io.number(t.maxLines);
  io.flag(t.bold);
  io.flag(t.inverted);
  io.number(t.rotation);
}
template <typename IO, typename P>
void paint(IO& io, P& p) {
  io.number(p.kind);
  io.number(p.color);
}
template <typename IO, typename S>
void fields(IO& io, S& s) {
  io.text(s.deviceId);
  io.text(s.contentRevision);
  io.number(s.profileGeneration);
  io.number(s.contentGeneration);
  io.number(s.physicalWidth);
  io.number(s.physicalHeight);
  io.number(s.fontPointSize);
  io.text(s.fontFamily);
  io.number(s.cardCount);
  io.flag(s.isX3);
  io.flag(s.sleepCover);
  io.flag(s.glanceStale);
  // Metrics are encoded as signed32; host and device int are both 32-bit.
  io.number(s.metrics.topPadding);
  io.number(s.metrics.headerHeight);
  io.number(s.metrics.verticalSpacing);
  io.number(s.metrics.contentSidePadding);
  io.number(s.metrics.sideButtonHintsWidth);
  auto& h = s.header;
  for (auto& font : h.fonts) io.number(font);
  style(io, h.titleText);
  style(io, h.subtitleText);
  paint(io, h.normalStyle.background);
  paint(io, h.normalStyle.foreground);
  paint(io, h.normalStyle.border);
  io.number(h.normalStyle.borderWidth);
  io.number(h.normalStyle.radius);
  io.number(h.normalStyle.corners);
  io.number(h.height);
  io.number(h.sidePadding);
  io.number(h.underline);
  io.number(h.titleAlign);
  io.number(h.batteryWidth);
  io.number(h.batteryHeight);
  io.number(h.stripHeight);
  io.number(h.edgeInset);
  io.number(h.actionOffsetY);
  io.number(h.batteryPercent);
  io.flag(h.charging);
  io.flag(h.batteryLeft);
  io.flag(h.clockCentered);
  io.flag(h.showBack);
  io.text(h.batteryLabel);
  io.text(h.clock);
  io.text(s.book.title);
  io.text(s.book.author);
  io.number(s.book.percent);
  io.flag(s.book.valid);
  io.number(s.glance.savedEpoch);
  io.number(s.glance.utcOffsetMinutes);
  io.text(s.glance.syncedHm);
  auto& g = s.glance.glance;
  auto& w = g.weather;
  io.flag(g.valid);
  io.flag(w.valid);
  io.text(w.place);
  io.number(w.code);
  io.number(w.tempC);
  io.text(w.summary);
  io.number(w.todayMinC);
  io.number(w.todayMaxC);
  io.text(w.rainStartHm);
  io.text(w.rainEndHm);
  io.number(w.rainProbability);
  day(io, w.tomorrow);
  io.number(w.dayCount);
  for (auto& d : w.days) day(io, d);
  io.number(g.eventCount);
  for (auto& e : g.events) {
    io.text(e.startHm);
    io.text(e.endHm);
    io.text(e.title);
  }
  io.text(s.word.cardId);
  io.text(s.word.module);
  io.text(s.word.actionClass);
  io.text(s.word.title);
  io.text(s.word.question);
  io.text(s.word.context);
  // Choices are not drawn by Home/Brief and never copied into the snapshot.
  for (size_t i = 0; i < (s.schema == 1 ? 20u : LABEL_COUNT); ++i) io.text(s.labels[i]);
  if (s.schema == 2) {
    io.number(s.coverKey);
    io.number(s.coverSize);
    io.number(s.coverCrc);
  }
}
bool hex(const char* value, size_t length, bool upper) {
  if (strlen(value) != length) return false;
  for (size_t i = 0; i < length; ++i) {
    const char c = value[i];
    if (!((c >= '0' && c <= '9') || (c >= (upper ? 'A' : 'a') && c <= (upper ? 'F' : 'f')))) return false;
  }
  return true;
}
bool validStyle(const freeink::ui::TextStyle& t) {
  return t.font < 4 && static_cast<unsigned>(t.align) <= 2 && static_cast<unsigned>(t.color) <= 4 && t.maxLines >= 1 &&
         t.maxLines <= 4 && static_cast<unsigned>(t.rotation) <= 3;
}
bool validPaint(const freeink::ui::Paint& p) {
  return static_cast<unsigned>(p.kind) <= 2 && static_cast<unsigned>(p.color) <= 4;
}
bool geometry(int n, int low, int high) { return n >= low && n <= high; }
}  // namespace

void clearLayoutInputs(LayoutInputs& out) {
  // Reinitialize in caller-owned storage. Placement construction allocates
  // nothing and cannot create a second ~6 KB aggregate on the task stack.
  out.~LayoutInputs();
  ::new (static_cast<void*>(&out)) LayoutInputs{};
}

bool validLayoutInputs(const LayoutInputs& s) {
  if ((s.schema != 1 && s.schema != 2) || s.coverSize > 131072 || (!s.coverSize && (s.coverKey || s.coverCrc)) ||
      (s.coverSize && !s.coverKey))
    return false;
  // Encoding is also our complete bounded-string validator.
  Writer strings{nullptr, MAX_BYTES};
  fields(strings, s);
  if (!strings.ok || !hex(s.deviceId, 8, true) || (s.contentRevision[0] && !hex(s.contentRevision, 64, false)))
    return false;
  if ((!s.contentRevision[0] && (s.contentGeneration || s.cardCount)) ||
      (s.contentRevision[0] && !s.contentGeneration) || s.cardCount > CARD_CAP)
    return false;
  if (!geometry(s.physicalWidth, 8, 2048) || s.physicalWidth % 8 || !geometry(s.physicalHeight, 8, 2048) ||
      s.fontPointSize > 72 || strcmp(s.fontFamily, "PocketSansWorld") != 0)
    return false;
  const auto& m = s.metrics;
  const auto& h = s.header;
  for (const auto font : h.fonts)
    if (font != SMALL_FONT_ID && font != UI_10_FONT_ID && font != UI_12_FONT_ID) return false;
  if (!validStyle(h.titleText) || !validStyle(h.subtitleText) || !validPaint(h.normalStyle.background) ||
      !validPaint(h.normalStyle.foreground) || !validPaint(h.normalStyle.border) || h.normalStyle.borderWidth > 16 ||
      h.normalStyle.radius > 128 || h.normalStyle.corners > 15)
    return false;
  if (!geometry(m.topPadding, 0, 128) || !geometry(m.headerHeight, 1, 256) || !geometry(m.verticalSpacing, 0, 128) ||
      !geometry(m.contentSidePadding, 0, 128) || !geometry(m.sideButtonHintsWidth, 0, 128) ||
      h.height != m.headerHeight || !geometry(h.sidePadding, 0, 128) || !geometry(h.underline, 0, 16) ||
      !geometry(h.titleAlign, 0, 2) || !geometry(h.batteryWidth, 1, 128) || !geometry(h.batteryHeight, 1, 128) ||
      !geometry(h.stripHeight, 0, 256) || !geometry(h.edgeInset, 0, 128) || !geometry(h.actionOffsetY, -128, 256) ||
      h.batteryPercent > 100 || s.book.percent < -1 || s.book.percent > 100)
    return false;
  // Unset glance is a valid reading state. Rolled glances keep saved metadata;
  // use the existing strict field validator for every non-empty snapshot.
  if (s.glance.savedEpoch) {
    if (!AppGlance::valid(s.glance)) return false;
  } else if (s.glance.glance.valid || s.glance.glance.weather.valid || s.glance.glance.eventCount)
    return false;
  return s.word.cardId[0] && strcmp(s.word.module, "local") == 0;
}

size_t encodeLayoutInputs(const LayoutInputs& in, uint8_t* bytes, size_t capacity) {
  if (!validLayoutInputs(in) || capacity > MAX_BYTES) return 0;
  Writer out{bytes, capacity};
  out.byte('P');
  out.byte('D');
  out.byte('L');
  out.byte('I');
  out.number(in.schema);
  out.number(uint16_t{0});
  fields(out, in);
  if (!out.ok || out.at + 4 > capacity) return 0;
  if (bytes)
    out.number(~Content::contentCrcUpdate(0xFFFFFFFFu, bytes, out.at));
  else
    out.number(uint32_t{0});
  return out.ok ? out.at : 0;
}

bool decodeLayoutInputs(const uint8_t* bytes, size_t size, LayoutInputs& out) {
  clearLayoutInputs(out);
  if (!bytes || size < 12 || size > MAX_BYTES || memcmp(bytes, "PDLI", 4) != 0 || (bytes[4] != 1 && bytes[4] != 2) ||
      bytes[5] || bytes[6] || bytes[7])
    return false;
  Reader checksum{bytes + size - 4, 4};
  uint32_t expected = 0;
  checksum.number(expected);
  if (expected != ~Content::contentCrcUpdate(0xFFFFFFFFu, bytes, size - 4)) return false;
  out.schema = bytes[4];
  Reader in{bytes + 8, size - 12};
  fields(in, out);
  if (!in.ok || in.at != in.size || !validLayoutInputs(out)) {
    clearLayoutInputs(out);
    return false;
  }
  return true;
}

Home::Strings layoutStrings(const LayoutInputs& in) {
  const auto& l = in.labels;
  return {l[0],  l[1],  l[2],  l[3],  l[4],  l[5],  l[6],  l[7],  l[8], l[9],
          l[10], l[11], l[12], l[13], l[14], l[15], l[16], l[17], l[18]};
}
// Mutates in.labels through its indexed elements; cppcheck misses this write.
// cppcheck-suppress constParameterReference
bool setLayoutStrings(LayoutInputs& in, const Home::Strings& s, const char* sleepPreview) {
  const char* const labels[] = {
      s.pocketDaily, s.continueReading, s.noOpenBook,  s.startReading, s.study, s.myCards,  s.word, s.empty,
      s.weather,     s.noWeather,       s.weatherHint, s.nextEvent,    s.today, s.noEvents, s.home, s.select,
      s.library,     s.articles,        s.sync,        sleepPreview,   "",      ""};
  static_assert(sizeof(labels) / sizeof(labels[0]) == LABEL_COUNT);
  for (size_t i = 0; i < LABEL_COUNT; ++i) {
    if (!labels[i] || strlen(labels[i]) >= LABEL_BYTES) return false;
    memcpy(in.labels[i], labels[i], strlen(labels[i]) + 1);
  }
  return true;
}
}  // namespace PocketDaily::Preview
