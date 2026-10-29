#include <EpdFontData.h>
#include <FreeInkUIGfxRenderer.h>
#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <PocketUIHost.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <vector>

#include "components/icons/headerIcons.h"
#include "fontIds.h"
#include "host/HostHome.h"
#include "pocket_daily/ContentChecksum.h"
#include "pocket_daily/preview/LayoutInputs.h"

namespace {
using namespace PocketDaily;
Preview::LayoutInputs sample() {
  Preview::LayoutInputs in;
  strcpy(in.deviceId, "A1B2C3D4");
  in.profileGeneration = 7;
  in.physicalWidth = 800;
  in.physicalHeight = 480;
  in.fontPointSize = 12;
  strcpy(in.fontFamily, "PocketSansWorld");
  in.metrics = {5, 45, 10, 20, 30};
  auto& h = in.header;
  h.fonts[0] = SMALL_FONT_ID;
  h.fonts[1] = UI_12_FONT_ID;
  h.fonts[2] = UI_12_FONT_ID;
  h.fonts[3] = SMALL_FONT_ID;
  const auto tokens = freeink::ui::defaultThemeTokens();
  h.titleText = tokens.titleText;
  h.titleText.align = freeink::ui::TextAlign::Center;
  h.subtitleText = tokens.smallText;
  h.normalStyle = tokens.popup.normal;
  h.batteryPercent = 67;
  strcpy(h.batteryLabel, "67%");
  strcpy(h.clock, "10:28");
  in.book.valid = true;
  in.book.percent = 38;
  strcpy(in.book.title, "Ein langes Buch ueber das genaue Lesen und langsame Denken");
  strcpy(in.book.author, "Example Author");
  in.glance.clear();
  strcpy(in.word.cardId, "local:example:7");
  strcpy(in.word.module, "local");
  strcpy(in.word.title, "Actual daily word");
  strcpy(in.word.question, "observe");
  strcpy(in.word.context, "notice something carefully");
  EXPECT_TRUE(Preview::setLayoutStrings(in, Home::Strings{}, "Sleep preview"));
  return in;
}
std::vector<uint8_t> encode(const Preview::LayoutInputs& in) {
  std::vector<uint8_t> bytes(Preview::MAX_BYTES);
  const auto count = Preview::encodeLayoutInputs(in, bytes.data(), bytes.size());
  EXPECT_GT(count, 0u);
  bytes.resize(count);
  return bytes;
}
void checksum(std::vector<uint8_t>& bytes) {
  const auto crc = ~Content::contentCrcUpdate(UINT32_MAX, bytes.data(), bytes.size() - 4);
  for (int i = 0; i < 4; ++i) bytes[bytes.size() - 4 + i] = crc >> (8 * i);
}
std::vector<uint8_t> tinyFont() {
  constexpr unsigned count = 95, glyphStart = 76, bitmapStart = glyphStart + count * 16;
  std::vector<uint8_t> bytes(bitmapStart + count, 0);
  const auto put = [&](size_t offset, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = value >> (i * 8);
  };
  memcpy(bytes.data(), "CPFONT", 6);
  bytes[8] = 4;
  bytes[12] = 1;
  put(36, 1);
  put(40, count);
  bytes[44] = 12;
  bytes[45] = 10;
  put(56, 64);
  put(64, 32);
  put(68, 126);
  for (unsigned i = 0; i < count; ++i) {
    EpdGlyph glyph{};
    glyph.width = 2;
    glyph.height = 2;
    glyph.advanceX = 48;
    glyph.dataLength = 1;
    glyph.dataOffset = i;
    memcpy(bytes.data() + glyphStart + i * 16, &glyph, sizeof(glyph));
    bytes[bitmapStart + i] = 0xC0;
  }
  return bytes;
}
pdui_profile profile() { return {{1, 2, 0, 0}, 2, 1, 0, 1, 0, {1, 2, 3, 4}, 4}; }
struct Context {
  pdui_context* value = nullptr;
  Context() {
    const auto bytes = tinyFont();
    EXPECT_EQ(pdui_create(800, 480, 0, bytes.data(), bytes.size(), &value), PDUI_OK);
  }
  ~Context() { pdui_destroy(value); }
  std::vector<uint8_t> pixels() {
    std::vector<uint8_t> p(48000);
    EXPECT_EQ(pdui_copy_frame(value, p.data(), p.size()), PDUI_OK);
    return p;
  }
};
std::vector<uint8_t> render(Context& c, const Preview::LayoutInputs& in, bool brief = false,
                            const pdui_profile* proposed = nullptr) {
  const auto bytes = encode(in);
  const auto p = profile();
  const int result = brief ? pdui_render_reader_brief(c.value, proposed ? proposed : &p, bytes.data(), bytes.size(),
                                                      in.contentRevision, 2)
                           : pdui_render_reader_home(c.value, proposed ? proposed : &p, bytes.data(), bytes.size(),
                                                     in.contentRevision);
  EXPECT_EQ(result, PDUI_OK);
  return c.pixels();
}
}  // namespace

TEST(LayoutInputs, GoldenMetadataAndCanonicalRoundTrip) {
  auto in = sample();
  auto bytes = encode(in);
  Preview::LayoutInputs decoded;
  ASSERT_TRUE(Preview::decodeLayoutInputs(bytes.data(), bytes.size(), decoded));
  EXPECT_EQ(bytes, encode(decoded));
  pdui_layout_inputs_info info{};
  ASSERT_EQ(pdui_get_layout_inputs_info(bytes.data(), bytes.size(), &info), PDUI_OK);
  EXPECT_STREQ(info.device_id, "A1B2C3D4");
  EXPECT_STREQ(info.content_revision, "");
  EXPECT_EQ(info.card_count, 0u);
  EXPECT_EQ(info.profile_generation, 7u);
  EXPECT_EQ(info.physical_width, 800u);
  EXPECT_EQ(info.physical_height, 480u);
  EXPECT_EQ(info.font_point_size, 12u);
  EXPECT_STREQ(info.font_family, "PocketSansWorld");
  if (const char* path = std::getenv("PDUI_LAYOUT_FIXTURE")) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    ASSERT_TRUE(out.good());
  }
}
TEST(LayoutInputs, EveryTruncationAndEverySingleByteFaultRejects) {
  const auto good = encode(sample());
  Preview::LayoutInputs out;
  for (size_t n = 0; n < good.size(); ++n) EXPECT_FALSE(Preview::decodeLayoutInputs(good.data(), n, out)) << n;
  for (size_t n = 0; n < good.size(); ++n) {
    auto bad = good;
    bad[n] ^= 1;
    EXPECT_FALSE(Preview::decodeLayoutInputs(bad.data(), bad.size(), out)) << n;
  }
}
TEST(LayoutInputs, ChecksummedInvalidVersionUtf8TrailingAndFieldRangesReject) {
  const auto good = encode(sample());
  Preview::LayoutInputs out;
  for (const size_t at : {size_t(4), size_t(6), size_t(10)}) {
    auto bad = good;
    bad[at] = 0xFF;
    checksum(bad);
    EXPECT_FALSE(Preview::decodeLayoutInputs(bad.data(), bad.size(), out));
  }
  auto bad = good;
  bad.insert(bad.end() - 4, 0);
  checksum(bad);
  EXPECT_FALSE(Preview::decodeLayoutInputs(bad.data(), bad.size(), out));
  auto in = sample();
  in.header.batteryPercent = 101;
  EXPECT_FALSE(Preview::validLayoutInputs(in));
  in = sample();
  in.header.titleText.font = 5;
  EXPECT_FALSE(Preview::validLayoutInputs(in));
  in = sample();
  in.header.normalStyle.background.kind = freeink::ui::PaintKind::Bitmap;
  EXPECT_FALSE(Preview::validLayoutInputs(in));
  in = sample();
  in.cardCount = 1;
  EXPECT_FALSE(Preview::validLayoutInputs(in));
  in = sample();
  in.book.percent = 101;
  EXPECT_FALSE(Preview::validLayoutInputs(in));
  in = sample();
  in.labels[0][0] = '\n';
  EXPECT_FALSE(Preview::validLayoutInputs(in));
}
TEST(LayoutInputs, RejectedDecodeKeepsInfoOutputUntouchedAndInvalidatesFrame) {
  Context c;
  auto in = sample();
  render(c, in);
  auto bytes = encode(in);
  bytes.back() ^= 1;
  pdui_layout_inputs_info info;
  memset(&info, 0x45, sizeof(info));
  const auto old = info;
  EXPECT_EQ(pdui_get_layout_inputs_info(bytes.data(), bytes.size(), &info), PDUI_INVALID_ARGUMENT);
  EXPECT_EQ(memcmp(&info, &old, sizeof(info)), 0);
  const auto p = profile();
  EXPECT_EQ(pdui_render_reader_home(c.value, &p, bytes.data(), bytes.size(), ""), PDUI_INVALID_ARGUMENT);
  pdui_frame_info frame{};
  EXPECT_EQ(pdui_get_frame_info(c.value, &frame), PDUI_NO_FRAME);
}
TEST(LayoutInputs, GeometryRevisionCountWakeAndFontMismatchReject) {
  Context c;
  auto in = sample();
  const auto p = profile();
  auto bytes = encode(in);
  EXPECT_EQ(pdui_render_reader_home(c.value, &p, bytes.data(), bytes.size(), "bad"), PDUI_INVALID_ARGUMENT);
  EXPECT_EQ(pdui_render_reader_brief(c.value, &p, bytes.data(), bytes.size(), "", 3), PDUI_INVALID_ARGUMENT);
  in.physicalWidth = 792;
  bytes = encode(in);
  EXPECT_EQ(pdui_render_reader_home(c.value, &p, bytes.data(), bytes.size(), ""), PDUI_INVALID_ARGUMENT);
  in = sample();
  strcpy(in.contentRevision, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  in.contentGeneration = 9;
  in.cardCount = 1;
  bytes = encode(in);
  EXPECT_EQ(pdui_render_reader_home(c.value, &p, bytes.data(), bytes.size(), in.contentRevision),
            PDUI_INVALID_ARGUMENT);
  in = sample();
  in.fontPointSize = 14;
  bytes = encode(in);
  EXPECT_EQ(pdui_render_reader_home(c.value, &p, bytes.data(), bytes.size(), ""), PDUI_INVALID_FONT);
}
TEST(LayoutInputs, ActualBookEmptyStateAndDailyWordAffectPixelsIndependently) {
  Context c;
  auto in = sample();
  const auto home = render(c, in);
  const auto brief = render(c, in, true);
  in.book.valid = false;
  EXPECT_NE(home, render(c, in));
  EXPECT_NE(brief, render(c, in, true));
  in = sample();
  in.book.percent = 81;
  EXPECT_NE(home, render(c, in));
  auto p = profile();
  p.home_items[0] = 5;
  p.home_count = 1;
  const auto word = render(c, in, false, &p);
  strcpy(in.word.question, "a different actual reader word");
  EXPECT_NE(word, render(c, in, false, &p));
  EXPECT_EQ(render(c, in, false, &p), render(c, in, false, &p));
}
TEST(LayoutInputs, ActualGlanceStalenessEventsAndResolvedLabelsHaveNegativeControls) {
  Context c;
  auto in = sample();
  auto p = profile();
  p.sleep_sections[0] = 3;
  p.sleep_sections[1] = 4;
  p.sleep_count = 2;
  const auto empty = render(c, in, true, &p);
  auto& g = in.glance;
  g.savedEpoch = 1792288800;
  g.utcOffsetMinutes = 540;
  strcpy(g.syncedHm, "12:20");
  g.glance.valid = true;
  g.glance.eventCount = 1;
  strcpy(g.glance.events[0].startHm, "18:00");
  strcpy(g.glance.events[0].title, "An actual appointment");
  auto& w = g.glance.weather;
  w.valid = true;
  strcpy(w.place, "Example Town");
  w.code = 2;
  w.tempC = 16;
  w.todayMinC = 9;
  w.todayMaxC = 19;
  w.rainProbability = 30;
  strcpy(w.summary, "Cloudy");
  w.dayCount = 2;
  strcpy(w.days[0].date, "2026-10-18");
  strcpy(w.days[1].date, "2026-10-19");
  for (int i = 0; i < 2; ++i) {
    w.days[i].code = i;
    w.days[i].minC = 8 + i;
    w.days[i].maxC = 18 + i;
    w.days[i].rainProbability = 20 + i * 30;
  }
  w.tomorrow = w.days[1];
  ASSERT_TRUE(Preview::validLayoutInputs(in));
  const auto weather = render(c, in, true, &p);
  EXPECT_NE(empty, weather);
  in.glanceStale = true;
  EXPECT_NE(weather, render(c, in, true, &p));
  in.glanceStale = false;
  memset(g.glance.events[0].title, 0, sizeof(g.glance.events[0].title));
  strcpy(g.glance.events[0].title, "Changed appointment");
  EXPECT_NE(weather, render(c, in, true, &p));
  strcpy(in.labels[19], "A different preview label");
  EXPECT_NE(weather, render(c, in, true, &p));
}
TEST(LayoutInputs, HeaderBatteryClockAndStyleAreActualInputs) {
  Context c;
  auto in = sample();
  const auto baseline = render(c, in);
  in.header.batteryPercent = 14;
  strcpy(in.header.batteryLabel, "14%");
  EXPECT_NE(baseline, render(c, in));
  in = sample();
  strcpy(in.header.clock, "11:59");
  EXPECT_NE(baseline, render(c, in));
  in = sample();
  in.header.batteryLeft = true;
  EXPECT_NE(baseline, render(c, in));
  in = sample();
  in.header.titleText.align = freeink::ui::TextAlign::Left;
  EXPECT_NE(baseline, render(c, in));
  in = sample();
  in.header.underline = 0;
  EXPECT_NE(baseline, render(c, in));
}

// Independent transcription of the pre-extraction BaseTheme header: actual
// theme tokens/status values, original UI bindings and FreeInkUI component.
// Protects against manufacturing host parity by changing the reader's pixels.
TEST(LayoutInputs, SharedHeaderMatchesOriginalThemePathAcrossVariants) {
  namespace fui = freeink::ui;
  for (int variant = 0; variant < 8; ++variant) {
    HalDisplay a(800, 480), b(800, 480);
    GfxRenderer ra(a), rb(b);
    ra.begin();
    rb.begin();
    PocketUIHost::registerUiFonts(ra);
    PocketUIHost::registerUiFonts(rb);
    auto h = sample().header;
    h.titleText.align = static_cast<fui::TextAlign>(variant % 3);
    h.underline = variant % 4;
    h.batteryLeft = variant & 1;
    h.clockCentered = variant & 2;
    h.charging = variant & 4;
    h.showBack = variant == 7;
    if (variant == 3) h.batteryLabel[0] = 0;
    if (variant == 4) h.clock[0] = 0;
    const int height = variant == 5 ? 20 : 45;
    const char* title = variant == 6 ? "한국어 제목" : "Pocket Daily";
    fui::GfxRendererFrame<1> ui(ra, UI_10_FONT_ID, UI_12_FONT_ID, UI_12_FONT_ID);
    auto tokens = fui::themeTokensForLineHeight(ui.target.lineHeight(fui::GfxRendererTarget::FONT_BODY));
    tokens.headerTitleAlign = h.titleText.align;
    tokens.headerSidePadding = h.sidePadding;
    tokens.headerUnderline = h.underline;
    ui.target.setFont(fui::GfxRendererTarget::FONT_SMALL, SMALL_FONT_ID);
    ui.target.setFont(fui::GfxRendererTarget::FONT_LABEL, SMALL_FONT_ID);
    fui::HeaderProps props;
    props.title = title;
    props.rightLabel = "Meta";
    auto& status = props.status;
    status.battery.text.font = fui::GfxRendererTarget::FONT_LABEL;
    status.showBattery = true;
    status.battery.percent = h.batteryPercent;
    status.battery.charging = h.charging;
    status.battery.label = h.batteryLabel[0] ? h.batteryLabel : nullptr;
    status.battery.glyphWidth = h.batteryWidth;
    status.battery.glyphHeight = h.batteryHeight;
    status.battery.gap = 4;
    status.batteryLeft = h.batteryLeft;
    status.edgeInset = h.edgeInset;
    status.stripHeight = h.stripHeight;
    status.clockCentered = h.clockCentered;
    status.clockText = h.clock[0] ? h.clock : nullptr;
    props.leadingSize = 48;
    props.trailingSize = 48;
    props.actionOffsetY = h.actionOffsetY;
    props.titleOffsetY = height < h.height ? 0 : h.stripHeight / 2;
    if (h.showBack) {
      props.leadingIcon = fui::bitmapFromIcon(icon_header_back_32);
      props.leadingAction = 1;
    }
    props.borderEdges = fui::EdgeBottom;
    props.titleText = tokens.titleText;
    props.titleText.align = tokens.headerTitleAlign;
    props.subtitleText = tokens.smallText;
    props.styles = tokens.popup;
    props.sidePadding = tokens.headerSidePadding;
    if (props.styles.normal.border.kind == fui::PaintKind::None && tokens.headerUnderline > 0) {
      props.styles.normal.border = fui::Paint::solid(fui::Color::Black);
      props.styles.normal.borderWidth = tokens.headerUnderline;
    }
    fui::header(ui.frame, {0, 5, 480, static_cast<int16_t>(height)}, props);
    HeaderPainter::paintHeader(rb, h, 0, 5, 480, height, title, "Meta");
    EXPECT_EQ(memcmp(a.getFrameBuffer(), b.getFrameBuffer(), 48000), 0) << variant;
  }
}
TEST(LayoutInputs, ExactAndSampleModesCanAlternateWithoutDestructorAllocations) {
  Context c;
  const auto font = tinyFont();
  ASSERT_EQ(pdui_set_fallback_font(c.value, font.data(), font.size()), PDUI_OK);
  const auto p = profile();
  ASSERT_EQ(pdui_render_home(c.value, &p, PDUI_SAMPLE_ALL, 0), PDUI_OK);
  const auto old = c.pixels();
  const auto exact = render(c, sample());
  EXPECT_NE(old, exact);
  ASSERT_EQ(pdui_render_home(c.value, &p, PDUI_SAMPLE_ALL, 0), PDUI_OK);
  EXPECT_EQ(old, c.pixels());
  EXPECT_EQ(exact, render(c, sample()));
}

TEST(LayoutInputs, DeviceSchemaAndAsleepWakeAreDistinctFromSync) {
  Context c;
  auto in = sample();
  in.schema = 2;
  strcpy(in.labels[20], "Sleeping");
  strcpy(in.labels[21], "WAKE");
  auto bytes = encode(in);
  pdui_layout_asset_info info{};
  ASSERT_EQ(pdui_get_layout_asset_info(bytes.data(), bytes.size(), &info), PDUI_OK);
  EXPECT_EQ(info.schema, 2u);
  EXPECT_EQ(info.size, 0u);
  const auto p = profile();
  const auto sync = render(c, in, true);
  ASSERT_EQ(pdui_render_reader_device(c.value, &p, bytes.data(), bytes.size(), "", 1, 2, nullptr, 0), PDUI_OK);
  const auto sleep = c.pixels();
  EXPECT_NE(sync, sleep);
  ASSERT_EQ(pdui_render_reader_device(c.value, &p, bytes.data(), bytes.size(), "", 1, 3, nullptr, 0), PDUI_OK);
  EXPECT_NE(sleep, c.pixels());
  in.coverSize = 64;
  in.coverKey = 45;
  in.coverCrc = 123;
  bytes = encode(in);
  ASSERT_EQ(pdui_render_reader_device(c.value, &p, bytes.data(), bytes.size(), "", 0, 2, nullptr, 0),
            PDUI_INVALID_IMAGE);
  pdui_frame_info frame{};
  EXPECT_EQ(pdui_get_frame_info(c.value, &frame), PDUI_NO_FRAME);
  in.coverSize = 131073;
  EXPECT_EQ(Preview::encodeLayoutInputs(in, nullptr, Preview::MAX_BYTES), 0u);
}

TEST(LayoutInputs, DeviceCoverRasterChangesPixelsAndChangedBytesCannotPublish) {
  Context c;
  auto in = sample();
  in.schema = 2;
  const auto p = profile();
  auto bytes = encode(in);
  ASSERT_EQ(pdui_render_reader_device(c.value, &p, bytes.data(), bytes.size(), "", 0, 2, nullptr, 0), PDUI_OK);
  const auto placeholder = c.pixels();
  std::vector<uint8_t> bmp(190, 0);
  const auto put = [&](size_t at, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bmp[at + i] = value >> (i * 8);
  };
  bmp[0] = 'B';
  bmp[1] = 'M';
  put(2, bmp.size());
  put(10, 62);
  put(14, 40);
  put(18, 32);
  put(22, 32);
  bmp[26] = 1;
  bmp[28] = 1;
  put(34, 128);
  put(46, 2);
  bmp[58] = bmp[59] = bmp[60] = 255;
  for (size_t i = 62; i < bmp.size(); ++i) bmp[i] = static_cast<uint8_t>(i * 37 + 11);
  in.coverSize = bmp.size();
  in.coverKey = 42;
  in.coverCrc = ~Content::contentCrcUpdate(UINT32_MAX, bmp.data(), bmp.size());
  bytes = encode(in);
  ASSERT_EQ(pdui_render_reader_device(c.value, &p, bytes.data(), bytes.size(), "", 0, 2, bmp.data(), bmp.size()),
            PDUI_OK);
  EXPECT_NE(placeholder, c.pixels());
  bmp.back() ^= 1;
  EXPECT_EQ(pdui_render_reader_device(c.value, &p, bytes.data(), bytes.size(), "", 0, 2, bmp.data(), bmp.size()),
            PDUI_INVALID_IMAGE);
  pdui_frame_info info{};
  EXPECT_EQ(pdui_get_frame_info(c.value, &info), PDUI_NO_FRAME);
  Preview::LayoutInputs decoded;
  for (size_t n = 0; n < bytes.size(); ++n) EXPECT_FALSE(Preview::decodeLayoutInputs(bytes.data(), n, decoded));
}

TEST(LayoutInputs, CompactBoldBookTitleKeepsTheProgressGapClear) {
  Context c;
  auto in = sample();
  in.schema = 2;
  in.book.percent = 1;
  strcpy(in.book.title, "Menschliches, Allzumenschliches: Ein Buch Fuer Freie Geister (Erster Band)");
  auto p = profile();
  p.sleep_sections[0] = 1;
  p.sleep_count = 1;
  auto bytes = encode(in);
  ASSERT_EQ(pdui_render_reader_device(c.value, &p, bytes.data(), bytes.size(), "", 1, 0, nullptr, 0), PDUI_OK);
  const auto title = c.pixels();
  in.book.title[0] = 0;
  bytes = encode(in);
  ASSERT_EQ(pdui_render_reader_device(c.value, &p, bytes.data(), bytes.size(), "", 1, 0, nullptr, 0), PDUI_OK);
  const auto empty = c.pixels();
  HalDisplay display(800, 480);
  GfxRenderer renderer(display);
  renderer.begin();
  PocketUIHost::registerUiFonts(renderer);
  const int right = 480 - in.metrics.contentSidePadding - renderer.getTextWidth(UI_12_FONT_ID, "1%");
  int collisions = 0;
  for (int x = right - 8; x < 480 - in.metrics.contentSidePadding; ++x) {
    for (int y = 60; y < 200; ++y) {
      const size_t index = (479 - x) * 100 + y / 8;
      const auto mask = uint8_t(0x80 >> (y % 8));
      collisions += (title[index] & mask) != (empty[index] & mask);
    }
  }
  EXPECT_EQ(collisions, 0) << "Bold title spills into the progress clearance";
}
