#include <EpdFontData.h>
#include <PocketUIHost.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "pocket_daily/ContentChecksum.h"
#include "pocket_daily/home/HomeRenderer.h"

// Shared Home / Daily Brief painters through the host ABI (P1-3). These pin
// structure and determinism; pixel parity with the device is a hardware check.
namespace {
std::vector<uint8_t> tinyFont() {
  constexpr unsigned count = 95, glyphStart = 76, bitmapStart = glyphStart + count * 16;
  std::vector<uint8_t> bytes(bitmapStart + count, 0);
  const auto put = [&](size_t offset, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = value >> (i * 8);
  };
  std::memcpy(bytes.data(), "CPFONT", 6);
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
    std::memcpy(bytes.data() + glyphStart + i * 16, &glyph, sizeof(glyph));
    bytes[bitmapStart + i] = 0xC0;
  }
  return bytes;
}

// One PDCT card (docs/content-card-v1.md) as the companion encodes it.
std::vector<uint8_t> pdct(const char* id, const char* title, const char* text, const char* image = "") {
  std::vector<uint8_t> bytes(512, 0);
  std::memcpy(bytes.data(), "PDCT", 4);
  bytes[4] = 1;
  bytes[6] = 16;
  bytes[9] = 2;
  std::strcpy(reinterpret_cast<char*>(bytes.data() + 16), id);
  std::strcpy(reinterpret_cast<char*>(bytes.data() + 49), title);
  std::strcpy(reinterpret_cast<char*>(bytes.data() + 74), text);
  std::strcpy(reinterpret_cast<char*>(bytes.data() + 427), image);
  const uint32_t crc = PocketDaily::Content::contentCrcUpdate(UINT32_MAX, bytes.data(), 508) ^ UINT32_MAX;
  for (unsigned i = 0; i < 4; ++i) bytes[508 + i] = static_cast<uint8_t>(crc >> (8 * i));
  return bytes;
}

// A 64x64 checkerboard PBM, dark enough to count.
std::vector<uint8_t> checkerPbm() {
  const std::string header = "P4\n64 64\n";
  std::vector<uint8_t> bytes(header.begin(), header.end());
  for (int row = 0; row < 64; ++row)
    for (int column = 0; column < 8; ++column) bytes.push_back(((row / 8) + column) % 2 ? 0xFF : 0x00);
  return bytes;
}

pdui_profile defaults() {
  pdui_profile p{};
  p.home_items[0] = 1;
  p.home_items[1] = 2;
  p.home_items[2] = 3;
  p.home_count = 3;
  p.daily_word = 1;
  p.weather = 0;
  p.next_event = 1;
  p.sleep_mode = 0;
  p.sleep_sections[0] = 1;
  p.sleep_sections[1] = 2;
  p.sleep_sections[2] = 3;
  p.sleep_sections[3] = 4;
  p.sleep_count = 4;
  return p;
}

class HomePreview : public testing::TestWithParam<std::pair<uint32_t, uint32_t>> {
 protected:
  std::unique_ptr<pdui_context, decltype(&pdui_destroy)> context{nullptr, pdui_destroy};
  void SetUp() override {
    auto font = tinyFont();
    pdui_context* raw = nullptr;
    // Physical landscape framebuffer, portrait logical page, like the app.
    ASSERT_EQ(pdui_create(GetParam().first, GetParam().second, 0, font.data(), font.size(), &raw), PDUI_OK);
    context.reset(raw);
  }
  std::vector<uint8_t> frame() {
    pdui_frame_info info{};
    EXPECT_EQ(pdui_get_frame_info(context.get(), &info), PDUI_OK);
    std::vector<uint8_t> pixels(info.byte_count);
    EXPECT_EQ(pdui_copy_frame(context.get(), pixels.data(), pixels.size()), PDUI_OK);
    return pixels;
  }
  std::vector<uint8_t> home(const pdui_profile& p, uint32_t samples = PDUI_SAMPLE_ALL, uint32_t selected = 0) {
    EXPECT_EQ(pdui_render_home(context.get(), &p, samples, selected), PDUI_OK);
    return frame();
  }
  std::vector<uint8_t> brief(const pdui_profile& p, uint32_t samples = PDUI_SAMPLE_ALL) {
    EXPECT_EQ(pdui_render_brief(context.get(), &p, samples), PDUI_OK);
    return frame();
  }
  static size_t ink(const std::vector<uint8_t>& pixels) {
    size_t bits = 0;
    for (auto byte : pixels) bits += 8 - __builtin_popcount(byte);  // 0 is black
    return bits;
  }
};
}  // namespace

TEST_P(HomePreview, DefaultHomeAndBriefDrawDeterministically) {
  const auto first = home(defaults());
  EXPECT_GT(ink(first), 2000u);
  EXPECT_EQ(home(defaults()), first);
  const auto sleep = brief(defaults());
  EXPECT_GT(ink(sleep), 2000u);
  EXPECT_NE(sleep, first);
  EXPECT_EQ(brief(defaults()), sleep);
}

TEST_P(HomePreview, WeatherPlacementAndNextEventChangeTheFace) {
  auto p = defaults();
  const auto bottom = home(p);
  p.weather = 1;
  const auto top = home(p);
  p.weather = 2;
  const auto off = home(p);
  EXPECT_NE(bottom, top);
  EXPECT_NE(bottom, off);
  EXPECT_NE(top, off);
  EXPECT_LT(ink(off), ink(bottom));
  p = defaults();
  p.next_event = 0;
  EXPECT_NE(home(p), bottom);
}

TEST_P(HomePreview, ItemOrderDecidesThePrimaryPanel) {
  auto readingFirst = defaults();
  auto studyFirst = defaults();
  studyFirst.home_items[0] = 2;
  studyFirst.home_items[1] = 1;
  EXPECT_NE(home(readingFirst), home(studyFirst));
  // Selecting the second item pages the carousel.
  EXPECT_NE(home(readingFirst, PDUI_SAMPLE_ALL, 0), home(readingFirst, PDUI_SAMPLE_ALL, 1));
  // Out-of-range selection clamps instead of failing.
  EXPECT_EQ(pdui_render_home(context.get(), &readingFirst, PDUI_SAMPLE_ALL, 99), PDUI_OK);
}

TEST_P(HomePreview, MonitoringItemAppearsOnlyWithCarriedUsage) {
  auto p = defaults();
  p.home_items[0] = 4;
  p.home_items[1] = 1;
  p.home_count = 2;
  const auto withUsage = home(p, PDUI_SAMPLE_ALL);
  const auto withoutUsage = home(p, PDUI_SAMPLE_ALL & ~PDUI_SAMPLE_USAGE);
  EXPECT_NE(withUsage, withoutUsage);
}

TEST_P(HomePreview, BriefSectionOrderIsHonoured) {
  auto weatherFirst = defaults();
  weatherFirst.sleep_sections[0] = 3;
  weatherFirst.sleep_sections[1] = 1;
  weatherFirst.sleep_sections[2] = 2;
  weatherFirst.sleep_count = 3;
  EXPECT_NE(brief(weatherFirst), brief(defaults()));
}

TEST_P(HomePreview, EmptySamplesStillDrawTheShell) {
  EXPECT_GT(ink(home(defaults(), 0)), 500u);
  EXPECT_GT(ink(brief(defaults(), 0)), 100u);
}

TEST_P(HomePreview, InvalidProfilesAreRejectedAndLeaveNoFrame) {
  ASSERT_EQ(pdui_render_home(context.get(), nullptr, 0, 0), PDUI_INVALID_ARGUMENT);
  auto empty = defaults();
  empty.home_count = 0;
  auto duplicate = defaults();
  duplicate.home_items[1] = 1;
  auto unknown = defaults();
  unknown.home_items[0] = 9;
  auto weather = defaults();
  weather.weather = 3;
  auto sleep = defaults();
  sleep.sleep_count = 0;
  for (const auto& p : {empty, duplicate, unknown, weather, sleep}) {
    EXPECT_EQ(pdui_render_home(context.get(), &p, PDUI_SAMPLE_ALL, 0), PDUI_INVALID_ARGUMENT);
    pdui_frame_info info{};
    EXPECT_EQ(pdui_get_frame_info(context.get(), &info), PDUI_NO_FRAME);
    EXPECT_EQ(pdui_render_brief(context.get(), &p, PDUI_SAMPLE_ALL), PDUI_INVALID_ARGUMENT);
  }
  const auto p = defaults();
  EXPECT_EQ(pdui_render_home(context.get(), &p, 1u << 10, 0), PDUI_INVALID_ARGUMENT);
}

// X3 (528x792 portrait) and X4 (480x800 portrait) physical landscape panels.
// The order and fallback rules shared by the device overview and the preview.
TEST(HomeRowSources, FollowProfileOrderAndSkipItemsWithoutContent) {
  using PocketDaily::Home::RowSource;
  auto profile = PocketDaily::DailyProfile::defaults();  // reading, study, provider
  PocketDaily::Home::RowAvailability all;
  all.book = all.appCards = all.provider = all.monitor = true;
  RowSource out[PocketDaily::DailyProfile::HOME_ITEM_CAP];
  ASSERT_EQ(PocketDaily::Home::homeRowSources(profile, all, out), 3);
  EXPECT_EQ(out[0], RowSource::Reading);
  EXPECT_EQ(out[1], RowSource::AppCards);
  EXPECT_EQ(out[2], RowSource::Provider);

  profile.homeItems[0] = PocketDaily::DailyProfile::HomeItem::Monitor;
  profile.homeItems[1] = PocketDaily::DailyProfile::HomeItem::Study;
  profile.homeCount = 2;
  PocketDaily::Home::RowAvailability none;
  // No cards: the daily word stands in while the profile keeps it...
  ASSERT_EQ(PocketDaily::Home::homeRowSources(profile, none, out), 1);
  EXPECT_EQ(out[0], RowSource::DailyWord);
  // ...and Study adds nothing when it does not; monitoring needs carried data.
  profile.dailyWord = false;
  EXPECT_EQ(PocketDaily::Home::homeRowSources(profile, none, out), 0);
  ASSERT_EQ(PocketDaily::Home::homeRowSources(profile, all, out), 2);
  EXPECT_EQ(out[0], RowSource::Monitor);
  EXPECT_EQ(out[1], RowSource::AppCards);
  // A Word item of its own: Study shows only the app cards, never the word twice.
  profile.dailyWord = true;
  profile.homeItems[0] = PocketDaily::DailyProfile::HomeItem::Word;
  ASSERT_EQ(PocketDaily::Home::homeRowSources(profile, none, out), 1);
  EXPECT_EQ(out[0], RowSource::DailyWord);
  ASSERT_EQ(PocketDaily::Home::homeRowSources(profile, all, out), 2);
  EXPECT_EQ(out[1], RowSource::AppCards);
}

TEST_P(HomePreview, MyCardsReplaceTheSampleAndShowTheirImage) {
  auto p = defaults();
  p.home_items[0] = 2;  // My cards first, so the first page is the card
  p.home_items[1] = 1;
  const auto sample = home(p);
  const auto plain = pdct("note", "Reading goal", "Chapter three today");
  const auto withImage = pdct("note", "Reading goal", "Chapter three today", "qr.pbm");
  const auto qr = checkerPbm();
  pdui_card_input cards[1] = {{plain.data(), plain.size(), nullptr, 0}};
  ASSERT_EQ(pdui_set_cards(context.get(), cards, 1), PDUI_OK);
  const auto mine = home(p);
  EXPECT_NE(mine, sample);
  cards[0] = {withImage.data(), withImage.size(), qr.data(), qr.size()};
  ASSERT_EQ(pdui_set_cards(context.get(), cards, 1), PDUI_OK);
  const auto pictured = home(p);
  EXPECT_GT(ink(pictured), ink(mine) + 500) << "The card image is drawn on Home";

  // Invalid input leaves the previous set in place.
  auto broken = withImage;
  broken[60] ^= 1;
  cards[0] = {broken.data(), broken.size(), qr.data(), qr.size()};
  EXPECT_EQ(pdui_set_cards(context.get(), cards, 1), PDUI_INVALID_CARD);
  cards[0] = {withImage.data(), withImage.size(), nullptr, 0};
  EXPECT_EQ(pdui_set_cards(context.get(), cards, 1), PDUI_INVALID_IMAGE) << "Named image missing";
  EXPECT_EQ(pdui_set_cards(context.get(), nullptr, 4), PDUI_INVALID_ARGUMENT);
  EXPECT_EQ(home(p), pictured);
  ASSERT_EQ(pdui_set_cards(context.get(), nullptr, 0), PDUI_OK);
  EXPECT_EQ(home(p), sample);
}

TEST_P(HomePreview, DailyWordItemAndPinnedCardSection) {
  // Word as its own item: without app cards, Study adds nothing extra.
  auto p = defaults();
  p.home_items[0] = 5;
  p.home_count = 1;
  const auto word = home(p, PDUI_SAMPLE_ALL & ~PDUI_SAMPLE_STUDY);
  EXPECT_GT(ink(word), 2000u);
  p.home_items[1] = 2;
  p.home_count = 2;
  // Study with Word present does not repeat the daily word, so paging stays on one item.
  EXPECT_EQ(home(p, PDUI_SAMPLE_ALL & ~PDUI_SAMPLE_STUDY, 1), home(p, PDUI_SAMPLE_ALL & ~PDUI_SAMPLE_STUDY, 0));

  auto sleep = defaults();
  sleep.sleep_sections[0] = 5;
  sleep.sleep_count = 1;
  const auto none = brief(sleep, PDUI_SAMPLE_ALL & ~PDUI_SAMPLE_STUDY);
  const auto withCard = pdct("contact", "If found", "Please call 010-0000-0000", "qr.pbm");
  const auto qr = checkerPbm();
  pdui_card_input cards[1] = {{withCard.data(), withCard.size(), qr.data(), qr.size()}};
  ASSERT_EQ(pdui_set_cards(context.get(), cards, 1), PDUI_OK);
  const auto pinned = brief(sleep, PDUI_SAMPLE_ALL & ~PDUI_SAMPLE_STUDY);
  EXPECT_GT(ink(pinned), ink(none) + 500) << "The pinned card and its image are on the sleep frame";
  // It stays while a book is open, unlike the study section.
  sleep.sleep_sections[1] = 1;
  sleep.sleep_count = 2;
  EXPECT_GT(ink(brief(sleep)), ink(pinned));
}

INSTANTIATE_TEST_SUITE_P(Panels, HomePreview, testing::Values(std::make_pair(792u, 528u), std::make_pair(800u, 480u)));
