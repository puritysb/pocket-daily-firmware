#include <GfxRenderer.h>
#include <PocketUIHost.h>
#include <SdCardFont.h>

#include <array>
#include <cstdio>
#include <fstream>
#include <memory>

#include "pocket_daily/ContentCard.h"
#include "pocket_daily/ContentChecksum.h"
#include "pocket_daily/ContentImageRenderer.h"
#include "pocket_daily/ContentPageRenderer.h"
#include "pocket_daily/ContentTextLayout.h"

namespace {
bool paint(GfxRenderer& renderer, SdCardFont& font) {
  constexpr const char* sample = "AV To ffi fi fl 오늘 한 줄 읽기 かな 日本語\n긴 한글 문장의 줄바꿈을 확인합니다.";
  renderer.insertFont(1, EpdFontFamily(font.getEpdFont(0), font.getEpdFont(1)));
  renderer.registerSdCardFont(1, &font);
  if (!PocketDaily::Content::checkLayoutText(sample, &renderer, [](void* context, const char* chunk) {
        auto& r = *static_cast<GfxRenderer*>(context);
        const int missingMetrics = r.ensureSdCardFontReady(1, chunk, 3);
        const int missingBitmaps = r.prewarmSdCardFont(1, chunk, 3);
        if (missingMetrics || missingBitmaps)
          std::fprintf(stderr, "Text preflight failed: %d/%d\n", missingMetrics, missingBitmaps);
        return missingMetrics == 0 && missingBitmaps == 0;
      })) {
    std::fprintf(stderr, "Font coverage failed\n");
    return false;
  }
  renderer.clearScreen();
  for (const auto style : {EpdFontFamily::REGULAR, EpdFontFamily::BOLD}) {
    struct Context {
      GfxRenderer& renderer;
      EpdFontFamily::Style style;
      int y;
    } context{renderer, style, style == EpdFontFamily::REGULAR ? 40 : 240};
    const PocketDaily::Content::TextPainter painter{&context,
                                                    [](void* p, const char* line) {
                                                      const auto& c = *static_cast<Context*>(p);
                                                      return c.renderer.getTextWidth(1, line, c.style);
                                                    },
                                                    [](void* p, const char* line, unsigned i) {
                                                      const auto& c = *static_cast<Context*>(p);
                                                      c.renderer.drawText(1, 20, c.y + i * 40, line, true, c.style);
                                                    }};
    PocketDaily::Content::drawWrappedText(sample, renderer.getScreenWidth() - 40, 4, painter);
  }
  renderer.displayBuffer();
  return !font.boundedReadFailed();
}
}  // namespace

PocketDaily::Content::ContentCard sampleCard(bool withImage) {
  using namespace PocketDaily::Content;
  ContentCard card{};  // host-only fixture, not an embedded stack allocation
  std::snprintf(card.card.title, sizeof(card.card.title), "%s", "오늘 한 줄");
  std::snprintf(card.card.question, sizeof(card.card.question), "%s", "무엇을 배웠나요?\nAV To ffi fi fl かな 日本語");
  std::snprintf(card.card.context, sizeof(card.card.context), "%s", "천천히 읽고 기록하세요.");
  if (withImage) std::strcpy(card.imagePath, "pattern.pbm");
  return card;
}

bool paintPage(GfxRenderer& renderer, SdCardFont& font, bool empty, bool withImage, unsigned layout) {
  using namespace PocketDaily::Content;
  auto card = sampleCard(withImage);
  card.layout = static_cast<CardLayout>(layout);
  const ContentPageOptions options{1, 12, 8, 4, "Pocket", "No cards yet", {"Back", "", "Previous", "Next"}};
  const char* texts[]{card.card.title,      card.card.question, card.card.context, options.emptyTitle,
                      options.emptyMessage, options.labels[0],  options.labels[2], options.labels[3]};
  // Cached mode's prewarm replaces its per-page mini kerning matrix, so the
  // reference must prewarm the whole page once, not replace it per text field.
  // Host-only collection; BoundedUI itself never needs this string/page cache.
  std::string normalized;
  normalized.reserve(1024);
  for (const char* text : texts) {
    if (!checkLayoutText(text, &normalized, [](void* context, const char* chunk) {
          static_cast<std::string*>(context)->append(chunk);
          return true;
        }))
      return false;
    normalized += ' ';
  }
  if (renderer.ensureSdCardFontReady(1, normalized.c_str(), 3) != 0 ||
      renderer.prewarmSdCardFont(1, normalized.c_str(), 3) != 0)
    return false;
  struct Images {
    GfxRenderer& renderer;
    unsigned calls = 0;
  } context{renderer};
  const ContentPageImagePainter images{
      &context, [](void* value, const char* name, int x, int y, int width, int height) {
        if (std::strcmp(name, "pattern.pbm") != 0) return false;
        auto& c = *static_cast<Images*>(value);
        ++c.calls;
        static constexpr uint8_t bytes[]{'P', '4', '\n', '8', ' ', '4', '\n', 0xFF, 0x81, 0x81, 0xFF};
        const ManifestSource source{nullptr, sizeof(bytes), [](void*, size_t offset, uint8_t* output, size_t count) {
                                      if (offset > sizeof(bytes) || count > sizeof(bytes) - offset) return false;
                                      std::memcpy(output, bytes + offset, count);
                                      return true;
                                    }};
        return renderContentImage(c.renderer, source, x, y, width, height) == ImageResult::Ok;
      }};
  if (!renderContentPage(renderer, empty ? nullptr : &card, options, images) || font.boundedReadFailed() ||
      context.calls != (withImage ? 1U : 0U)) {
    std::fprintf(stderr, "Page render failed, lineHeight=%d fontReadFailed=%d\n", renderer.getLineHeight(1),
                 font.boundedReadFailed());
    return false;
  }
  renderer.displayBuffer();
  return true;
}

std::array<uint8_t, 512> encodeSampleCard(bool withImage, unsigned layout) {
  const auto card = sampleCard(withImage);
  std::array<uint8_t, 512> bytes{};
  std::memcpy(bytes.data(), "PDCT", 4);
  bytes[4] = layout ? 2 : 1;
  bytes[491] = layout;
  bytes[6] = 16;
  bytes[9] = 2;
  std::memcpy(bytes.data() + 16, "fixture", 7);
  std::memcpy(bytes.data() + 49, card.card.title, sizeof(card.card.title));
  std::memcpy(bytes.data() + 74, card.card.question, sizeof(card.card.question));
  std::memcpy(bytes.data() + 235, card.card.context, sizeof(card.card.context));
  std::memcpy(bytes.data() + 427, card.imagePath, sizeof(card.imagePath));
  const auto crc = PocketDaily::Content::contentCrcUpdate(0xFFFFFFFFu, bytes.data(), 508) ^ 0xFFFFFFFFu;
  for (unsigned i = 0; i < 4; ++i) bytes[508 + i] = crc >> (8 * i);
  return bytes;
}

bool checkABI(pdui_context* context, const HalDisplay& reference, unsigned orientation, bool empty, bool withImage,
              unsigned layout) {
  pdui_content_options options{12, 8, 4, "Pocket", "No cards yet", {"Back", "", "Previous", "Next"}};
  auto card = encodeSampleCard(withImage, layout);
  const uint8_t image[]{'P', '4', '\n', '8', ' ', '4', '\n', 0xFF, 0x81, 0x81, 0xFF};
  const auto render = [&] {
    return pdui_render_content(context, empty ? nullptr : card.data(), empty ? 0 : card.size(),
                               withImage ? image : nullptr, withImage ? sizeof(image) : 0, &options);
  };
  const auto status = render();
  if (status != PDUI_OK) {
    std::fprintf(stderr, "C ABI render failed: %d empty=%d image=%d\n", status, empty, withImage);
    return false;
  }
  pdui_frame_info info{};
  if (pdui_get_frame_info(context, &info) != PDUI_OK || info.physical_width != reference.getDisplayWidth() ||
      info.physical_height != reference.getDisplayHeight() || info.row_bytes != reference.getDisplayWidthBytes() ||
      info.byte_count != reference.getBufferSize() || info.orientation != orientation ||
      info.logical_width != (orientation % 2 ? info.physical_width : info.physical_height) ||
      info.logical_height != (orientation % 2 ? info.physical_height : info.physical_width))
    return false;
  std::vector<uint8_t> output(info.byte_count + 16, 0xA5);
  if (pdui_copy_frame(context, output.data(), info.byte_count - 1) != PDUI_BUFFER_TOO_SMALL ||
      !std::all_of(output.begin(), output.end(), [](auto b) { return b == 0xA5; }) ||
      pdui_copy_frame(context, output.data(), info.byte_count) != PDUI_OK ||
      std::memcmp(output.data(), reference.getFrameBuffer(), info.byte_count) ||
      !std::all_of(output.end() - 16, output.end(), [](auto b) { return b == 0xA5; }))
    return false;
  // A failed request must hide the previous successful frame and not copy any
  // bytes. The next explicit, valid render must recover without re-creation.
  std::memset(options.labels[0], 'A', sizeof(options.labels[0]));
  if (render() != PDUI_INVALID_ARGUMENT || pdui_get_frame_info(context, &info) != PDUI_NO_FRAME || info.byte_count)
    return false;
  std::fill(output.begin(), output.end(), 0xA5);
  if (pdui_copy_frame(context, output.data(), output.size()) != PDUI_NO_FRAME ||
      !std::all_of(output.begin(), output.end(), [](auto b) { return b == 0xA5; }))
    return false;
  std::memset(options.labels[0], 0, sizeof(options.labels[0]));
  std::strcpy(options.labels[0], "Back");
  if (render() != PDUI_OK) return false;
  if (!empty) {
    card[508] ^= 1;
    if (render() != PDUI_INVALID_CARD || pdui_get_frame_info(context, &info) != PDUI_NO_FRAME) return false;
    card[508] ^= 1;
  }
  if (withImage &&
      pdui_render_content(context, card.data(), card.size(), image, sizeof(image) - 1, &options) != PDUI_INVALID_IMAGE)
    return false;
  return render() == PDUI_OK;
}

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "Usage: gfx_font_fixture_check <local.cpfont>\n");
    return 2;
  }
  std::ifstream input(argv[1], std::ios::binary);
  if (!input) return 2;
  input.seekg(0, std::ios::end);
  const auto size = input.tellg();
  if (size <= 0 || size > 64 * 1024 * 1024) return 2;
  input.seekg(0);
  std::vector<uint8_t> bytes;
  // Host-only fixture materialization, bounded to64MiB; no device SD access.
  bytes.resize(static_cast<size_t>(size));
  input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (input.gcount() != static_cast<std::streamsize>(bytes.size()) || input.bad() ||
      input.peek() != std::char_traits<char>::eof())
    return 2;
  const PocketUIHost::Asset assets[]{{"/fixture.cpfont", bytes}};
  const PocketUIHost::AssetScope assetScope(assets);
  SdCardFont cached, bounded;
  if (!cached.load("/fixture.cpfont") || !bounded.load("/fixture.cpfont", SdCardFont::LoadMode::BoundedUI)) return 1;
  unsigned compared = 0;
  for (const auto [width, height] : {std::pair{800, 480}, {792, 528}}) {
    for (const auto orientation : {GfxRenderer::Portrait, GfxRenderer::LandscapeClockwise,
                                   GfxRenderer::PortraitInverted, GfxRenderer::LandscapeCounterClockwise}) {
      HalDisplay a(width, height), b(width, height);
      GfxRenderer ra(a), rb(b);
      ra.begin();
      rb.begin();
      ra.setOrientation(orientation);
      rb.setOrientation(orientation);
      pdui_context* rawContext = nullptr;
      if (pdui_create(width, height, orientation, bytes.data(), bytes.size(), &rawContext) != PDUI_OK) return 1;
      const std::unique_ptr<pdui_context, decltype(&pdui_destroy)> context(rawContext, pdui_destroy);
      pdui_frame_info unrendered{};
      if (pdui_get_frame_info(context.get(), &unrendered) != PDUI_NO_FRAME) return 1;
      if (!paint(ra, cached) || !paint(rb, bounded) || !a.guardsIntact() || !b.guardsIntact()) return 1;
      if (std::memcmp(a.getFrameBuffer(), b.getFrameBuffer(), a.getBufferSize()) != 0) {
        std::fprintf(stderr, "Raster mismatch %dx%d orientation=%d\n", width, height, int(orientation));
        return 1;
      }
      if (std::all_of(a.getFrameBuffer(), a.getFrameBuffer() + a.getBufferSize(),
                      [](auto byte) { return byte == 0xFF; }))
        return 1;
      ++compared;
      for (unsigned variant = 0; variant < 5; ++variant) {
        const bool empty = variant == 1, withImage = variant >= 2;
        const unsigned layout = variant >= 3 ? variant - 2 : 0;
        if (!paintPage(ra, cached, empty, withImage, layout) || !paintPage(rb, bounded, empty, withImage, layout) ||
            !a.guardsIntact() || !b.guardsIntact() ||
            std::memcmp(a.getFrameBuffer(), b.getFrameBuffer(), a.getBufferSize()) != 0 ||
            std::all_of(a.getFrameBuffer(), a.getFrameBuffer() + a.getBufferSize(),
                        [](auto byte) { return byte == 0xFF; })) {
          std::fprintf(stderr, "Content page mismatch/failure %dx%d orientation=%d empty=%d\n", width, height,
                       int(orientation), empty);
          std::fprintf(stderr, "Line heights: %d / %d\n", ra.getLineHeight(1), rb.getLineHeight(1));
          unsigned differences = 0;
          for (unsigned i = 0; i < a.getBufferSize(); ++i)
            if (a.getFrameBuffer()[i] != b.getFrameBuffer()[i]) {
              if (differences < 5)
                std::fprintf(stderr, "byte %u: %02x / %02x\n", i, a.getFrameBuffer()[i], b.getFrameBuffer()[i]);
              ++differences;
            }
          std::fprintf(stderr, "Differing bytes: %u\n", differences);
          return 1;
        }
        if (!checkABI(context.get(), b, orientation, empty, withImage, layout)) return 1;
        ++compared;
      }
    }
  }
  std::printf(
      "Compared %u production-rasterized text/content/empty/image frames: Cached == BoundedUI, regular/bold "
      "Latin/Korean/Japanese; guards "
      "intact. C ABI matches40 page frames; refusal/recovery checked. Host-only, not physical-panel parity.\n",
      compared);
}
