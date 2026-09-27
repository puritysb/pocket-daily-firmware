#include <InflateReader.h>
#include <gtest/gtest.h>
#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace {
using Bytes = std::vector<uint8_t>;

// Text-like data whose matches reach back up to the full 32 KiB window, so the
// decoder must copy across every window segment boundary and across the wrap.
Bytes windowSpanningData(const size_t size) {
  Bytes out;
  out.reserve(size);
  uint32_t seed = 0x2545F491;
  auto next = [&seed]() {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
  };
  while (out.size() < size) {
    const uint32_t r = next();
    if (out.size() > 40000 && r % 4 == 0) {
      // Repeat a run from 8 KiB .. 32 KiB back.
      const size_t distance = 8192 + next() % (32768 - 8192 - 300);
      const size_t length = 3 + next() % 255;
      const size_t from = out.size() - distance;
      for (size_t i = 0; i < length && out.size() < size; ++i) out.push_back(out[from + i]);
    } else {
      out.push_back(static_cast<uint8_t>('a' + r % 26));
    }
  }
  return out;
}

Bytes rawDeflate(const Bytes& input) {
  z_stream zs{};
  EXPECT_EQ(deflateInit2(&zs, 9, Z_DEFLATED, -15, 9, Z_DEFAULT_STRATEGY), Z_OK);
  Bytes out(deflateBound(&zs, input.size()));
  zs.next_in = const_cast<Bytes::value_type*>(input.data());
  zs.avail_in = static_cast<uInt>(input.size());
  zs.next_out = out.data();
  zs.avail_out = static_cast<uInt>(out.size());
  EXPECT_EQ(deflate(&zs, Z_FINISH), Z_STREAM_END);
  out.resize(zs.total_out);
  deflateEnd(&zs);
  return out;
}

// ZipFile's pattern: the reader is the first member and input arrives in small reads.
struct StreamCtx {
  InflateReader reader;
  const Bytes* input = nullptr;
  size_t offset = 0;
  uint8_t buffer[97];
};

int feed(uzlib_uncomp* uncomp) {
  auto* ctx = reinterpret_cast<StreamCtx*>(uncomp);
  if (ctx->offset >= ctx->input->size()) return -1;
  const size_t n = std::min(sizeof(ctx->buffer), ctx->input->size() - ctx->offset);
  std::copy_n(ctx->input->begin() + static_cast<long>(ctx->offset), n, ctx->buffer);
  ctx->offset += n;
  uncomp->source = ctx->buffer + 1;
  uncomp->source_limit = ctx->buffer + n;
  return ctx->buffer[0];
}
}  // namespace

TEST(InflateReaderStreaming, WindowIsSegmentedNotOneContiguousBlock) {
  InflateReader reader;
  ASSERT_TRUE(reader.init(true));
  const uzlib_uncomp* raw = reader.raw();
  EXPECT_EQ(raw->dict_ring, nullptr);
  ASSERT_NE(raw->dict_segs, nullptr);
  EXPECT_EQ(raw->dict_size, 32768U);
  EXPECT_EQ(UZLIB_DICT_SEG_SIZE, 8192U);
  reader.deinit();
  EXPECT_EQ(reader.raw()->dict_segs, nullptr);
}

TEST(InflateReaderStreaming, FarBackReferencesDecodeAcrossSegmentsAndWrap) {
  const Bytes original = windowSpanningData(160 * 1024);
  const Bytes compressed = rawDeflate(original);
  ASSERT_LT(compressed.size(), original.size());

  StreamCtx ctx;
  ctx.input = &compressed;
  ASSERT_TRUE(ctx.reader.init(true));
  ctx.reader.setReadCallback(feed);

  Bytes decoded;
  uint8_t chunk[1000];
  for (;;) {
    size_t produced = 0;
    const InflateStatus status = ctx.reader.readAtMost(chunk, sizeof(chunk), &produced);
    ASSERT_NE(status, InflateStatus::Error);
    decoded.insert(decoded.end(), chunk, chunk + produced);
    if (status == InflateStatus::Done) break;
    ASSERT_LE(decoded.size(), original.size());
  }
  EXPECT_EQ(decoded, original);
}

TEST(InflateReaderStreaming, OneShotModeStillUsesTheOutputBuffer) {
  const Bytes original = windowSpanningData(48 * 1024);
  const Bytes compressed = rawDeflate(original);
  InflateReader reader;
  ASSERT_TRUE(reader.init(false));
  EXPECT_EQ(reader.raw()->dict_size, 0U);
  reader.setSource(compressed.data(), compressed.size());
  Bytes decoded(original.size());
  ASSERT_TRUE(reader.read(decoded.data(), decoded.size()));
  EXPECT_EQ(decoded, original);
}
