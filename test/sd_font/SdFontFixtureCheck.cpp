#include <HalStorage.h>
#include <SdCardFont.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

namespace {
uint32_t u32(const std::vector<uint8_t>& bytes, size_t offset) {
  return uint32_t(bytes.at(offset)) | uint32_t(bytes.at(offset + 1)) << 8 | uint32_t(bytes.at(offset + 2)) << 16 |
         uint32_t(bytes.at(offset + 3)) << 24;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "Usage: sd_font_fixture_check <local.cpfont>\n");
    return 2;
  }
  std::ifstream input(argv[1], std::ios::binary);
  if (!input) return 2;
  auto& bytes = FakeSD::files["/fixture.cpfont"];
  bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  SdCardFont cached, bounded;
  if (!cached.load("/fixture.cpfont") || !bounded.load("/fixture.cpfont", SdCardFont::LoadMode::BoundedUI)) return 1;
  size_t checked = 0;
  for (unsigned style = 0; style < bytes.at(12); ++style) {
    const size_t toc = 32 + style * 32;
    const uint8_t id = bytes.at(toc);
    const auto* a = cached.getEpdFont(id);
    const auto* b = bounded.getEpdFont(id);
    if (!a || !b) return 1;
    const uint32_t intervals = u32(bytes, toc + 4);
    const uint32_t base = u32(bytes, toc + 24);
    for (uint32_t i = 0; i < intervals; ++i) {
      const uint32_t first = u32(bytes, base + i * 12);
      const uint32_t last = u32(bytes, base + i * 12 + 4);
      for (uint32_t cp = first; cp <= last; ++cp) {
        const auto* ga = a->getGlyph(cp);
        const auto* gb = b->getGlyph(cp);
        if (!ga || !gb || std::memcmp(ga, gb, sizeof(*ga)) != 0) {
          std::fprintf(stderr, "Glyph mismatch style=%u U+%04X\n", id, cp);
          return 1;
        }
        if (ga->dataLength && std::memcmp(cached.getOverflowBitmap(ga), bounded.getOverflowBitmap(gb), ga->dataLength))
          return 1;
        ++checked;
      }
    }
    // Compare sampled shaping through the font API after normal prewarming.
    // This is not exhaustive matrix/ligature coverage.
    const char* sample = "AV To ffi fi fl 오늘 한 줄 읽기 かな 日本語";
    if (cached.prewarm(sample, 1U << id) != 0 || bounded.prewarm(sample, 1U << id) != 0) return 1;
    for (const uint32_t left : {uint32_t('A'), uint32_t('T'), uint32_t('f')}) {
      for (const uint32_t right : {uint32_t('V'), uint32_t('o'), uint32_t('i'), uint32_t('l')}) {
        if (a->getKerning(left, right) != b->getKerning(left, right) ||
            a->getLigature(left, right) != b->getLigature(left, right))
          return 1;
      }
    }
  }
  // Repeat bounds measurement through the production font API. This counts
  // HAL traffic on a fake SD, not elapsed physical SD or app Apply latency.
  // Cached and BoundedUI must agree even after glyph/metadata ring eviction.
  static constexpr std::array samples = {
      "Choose one small thing to finish. Keep the next step simple and start there.",
      "오늘 읽은 내용에서 기억할 한 문장을 골라 보세요. 내 말로 다시 설명해 보세요.",
      "すみません。駅はどこですか。Could you say that again, please? AV To ffi fi fl"};
  for (size_t sampleIndex = 0; sampleIndex < samples.size(); ++sampleIndex) {
    const char* sample = samples[sampleIndex];
    for (unsigned style = 0; style < 2; ++style) {
      if (!cached.hasStyle(style) || !bounded.hasStyle(style)) continue;
      if (cached.prewarm(sample, 1U << style) != 0) return 1;
      int expectedW = 0, expectedH = 0;
      cached.getEpdFont(style)->getTextDimensions(sample, &expectedW, &expectedH);
      bounded.clearCache();
      const auto opens = fontStorage.readOpens;
      const auto reads = FakeFontIO::reads;
      const auto seeks = FakeFontIO::seeks;
      const auto bytesRead = FakeSD::bytesRead;
      for (unsigned pass = 0; pass < 12; ++pass) {
        int width = 0, height = 0;
        bounded.getEpdFont(style)->getTextDimensions(sample, &width, &height);
        if (width != expectedW || height != expectedH || bounded.boundedReadFailed()) return 1;
      }
      std::printf("sample=%zu style=%u passes=12 opens=%u reads=%zu seeks=%zu bytes=%zu dimensions=%dx%d\n",
                  sampleIndex, style, fontStorage.readOpens - opens, FakeFontIO::reads - reads,
                  FakeFontIO::seeks - seeks, FakeSD::bytesRead - bytesRead, expectedW, expectedH);
    }
  }
  std::printf(
      "Verified %zu glyph metrics/bitmaps and sampled shaping; object=%zuB, bounded bitmap cache<=2048B + 256B "
      "replacement.\n",
      checked, sizeof(SdCardFont));
  return 0;
}
