#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "pocket_daily/ContentCard.h"

namespace {
using namespace PocketDaily::Content;
std::vector<uint8_t> cardFixture() {
  std::vector<uint8_t> bytes(512, 0);
  memcpy(bytes.data(), "PDCT", 4);
  bytes[4] = 1;
  bytes[6] = 16;
  bytes[9] = 2;
  strcpy(reinterpret_cast<char*>(bytes.data() + 16), "morning-1");
  strcpy(reinterpret_cast<char*>(bytes.data() + 49), "오늘");
  strcpy(reinterpret_cast<char*>(bytes.data() + 74), "한 줄 읽기\nRead one line.");
  strcpy(reinterpret_cast<char*>(bytes.data() + 235), "가볍게 시작하세요.");
  strcpy(reinterpret_cast<char*>(bytes.data() + 427), "sun.pbm");
  // Shared independently generated golden: see companion ContentCardTests.
  bytes[508] = 0x8D;
  bytes[509] = 0x91;
  bytes[510] = 0xC2;
  bytes[511] = 0x56;
  return bytes;
}
void fixCrc(std::vector<uint8_t>& bytes) {
  uint32_t crc = UINT32_MAX;
  for (size_t i = 0; i < 508; ++i) {
    crc ^= bytes[i];
    for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0u);
  }
  crc ^= UINT32_MAX;
  for (unsigned i = 0; i < 4; ++i) bytes[508 + i] = static_cast<uint8_t>(crc >> (8 * i));
}
bool readCard(void* context, size_t offset, uint8_t* out, size_t size) {
  const auto& bytes = *static_cast<std::vector<uint8_t>*>(context);
  if (size > 192 || offset > bytes.size() || size > bytes.size() - offset) return false;
  memcpy(out, bytes.data() + offset, size);
  return true;
}
CardResult decode(std::vector<uint8_t>& bytes) {
  ContentCard output;
  memset(&output, 0xFF, sizeof(output));
  const auto result = decodeContentCard({&bytes, bytes.size(), readCard}, output);
  if (result != CardResult::Ok) {
    const ContentCard empty{};
    EXPECT_EQ(memcmp(&empty, &output, sizeof(output)), 0);
  }
  return result;
}

TEST(ContentCard, SwiftGoldenMapsOnlyReadingData) {
  auto bytes = cardFixture();
  ContentCard output;
  memset(&output, 0xFF, sizeof(output));
  ASSERT_EQ(decodeContentCard({&bytes, bytes.size(), readCard}, output), CardResult::Ok);
  EXPECT_STREQ(output.card.cardId, "app:morning-1");
  EXPECT_STREQ(output.card.title, "오늘");
  EXPECT_STREQ(output.card.question, "한 줄 읽기\nRead one line.");
  EXPECT_STREQ(output.card.context, "가볍게 시작하세요.");
  EXPECT_STREQ(output.imagePath, "sun.pbm");
  EXPECT_STREQ(output.card.module, "app");
  EXPECT_STREQ(output.card.actionClass, "info");
  EXPECT_EQ(output.card.choiceCount, 0);
  for (const auto& choice : output.card.choices) {
    EXPECT_STREQ(choice.id, "");
    EXPECT_STREQ(choice.label, "");
  }
}

TEST(ContentCard, EveryTruncationAndSingleByteMutationRejected) {
  const auto original = cardFixture();
  for (size_t i = 0; i < original.size(); ++i) {
    auto bytes = original;
    bytes.resize(i);
    EXPECT_NE(decode(bytes), CardResult::Ok) << i;
    bytes = original;
    bytes[i] ^= 0x80;
    EXPECT_NE(decode(bytes), CardResult::Ok) << i;
  }
}

TEST(ContentCard, LayoutV2GoldenAndInvalidLayouts) {
  for (const uint8_t layout : {1, 2}) {
    auto bytes = cardFixture();
    bytes[4] = 2;
    bytes[491] = layout;
    fixCrc(bytes);
    const std::vector<uint8_t> golden =
        layout == 1 ? std::vector<uint8_t>{0xe9, 0x45, 0x35, 0xc5} : std::vector<uint8_t>{0x6d, 0x1e, 0xaf, 0x96};
    EXPECT_EQ(std::vector<uint8_t>(bytes.begin() + 508, bytes.end()), golden);
    ContentCard output{};
    ASSERT_EQ(decodeContentCard({&bytes, bytes.size(), readCard}, output), CardResult::Ok);
    EXPECT_EQ(static_cast<uint8_t>(output.layout), layout);
    bytes[4] = 1;
    fixCrc(bytes);
    EXPECT_EQ(decode(bytes), CardResult::Shape);
  }
  for (const uint8_t layout : {0, 3, 255}) {
    auto bytes = cardFixture();
    bytes[4] = 2;
    bytes[491] = layout;
    fixCrc(bytes);
    EXPECT_EQ(decode(bytes), CardResult::Shape);
  }
}

TEST(ContentCard, MalformedUTF8AndControlsRejectedEvenWithCorrectCRC) {
  const std::vector<std::vector<uint8_t>> cases = {{0xC0, 0xAF},
                                                   {0xE0, 0x80, 0xAF},
                                                   {0xED, 0xA0, 0x80},
                                                   {0xF4, 0x90, 0x80, 0x80},
                                                   {0xF0, 0x9F},
                                                   {0x80},
                                                   {0xC2, 0x85},
                                                   {0x7F},
                                                   {'\r'},
                                                   {'\t'},
                                                   {'\n'}};
  for (const auto& text : cases) {
    auto bytes = cardFixture();
    memset(bytes.data() + 49, 0, 25);
    memcpy(bytes.data() + 49, text.data(), text.size());
    fixCrc(bytes);
    EXPECT_EQ(decode(bytes), CardResult::Text);
  }
}

TEST(ContentCard, ExactByteLimitsAndOptionalFields) {
  auto bytes = cardFixture();
  memset(bytes.data() + 16, 'a', 32);
  memset(bytes.data() + 49, 'b', 24);
  memset(bytes.data() + 74, 'c', 160);
  memset(bytes.data() + 235, 'd', 191);
  memset(bytes.data() + 427, 0, 64);
  fixCrc(bytes);
  EXPECT_EQ(decode(bytes), CardResult::Ok);
  bytes[73] = 'x';  // no title terminator
  fixCrc(bytes);
  EXPECT_EQ(decode(bytes), CardResult::Text);
}

TEST(ContentCard, PathIdentifierAndPaddingValidation) {
  auto bytes = cardFixture();
  bytes[16] = '/';
  fixCrc(bytes);
  EXPECT_EQ(decode(bytes), CardResult::Identifier);
  bytes = cardFixture();
  strcpy(reinterpret_cast<char*>(bytes.data() + 427), "../a.pbm");
  fixCrc(bytes);
  EXPECT_EQ(decode(bytes), CardResult::ImagePath);
  bytes = cardFixture();
  bytes[507] = 1;
  fixCrc(bytes);
  EXPECT_EQ(decode(bytes), CardResult::Shape);
  bytes = cardFixture();
  bytes[72] = 'x';  // nonzero after title terminator
  fixCrc(bytes);
  EXPECT_EQ(decode(bytes), CardResult::Text);
}

TEST(ContentCard, ReadFailuresClearEveryPartialOutput) {
  for (const size_t failure : {0, 16, 49, 74, 235, 427, 491, 508}) {
    struct Input {
      std::vector<uint8_t> bytes;
      size_t failure;
    } input{cardFixture(), failure};
    const ManifestSource source{&input, 512, [](void* context, size_t offset, uint8_t* out, size_t size) {
                                  auto& input = *static_cast<Input*>(context);
                                  return offset != input.failure && readCard(&input.bytes, offset, out, size);
                                }};
    ContentCard output;
    memset(&output, 0xFF, sizeof(output));
    EXPECT_EQ(decodeContentCard(source, output), CardResult::ReadFailed);
    const ContentCard empty{};
    EXPECT_EQ(memcmp(&empty, &output, sizeof(output)), 0);
  }
}
}  // namespace
