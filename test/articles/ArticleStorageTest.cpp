#include <gtest/gtest.h>

#include "articles/ArticleStorage.h"

namespace {
void put32(std::vector<uint8_t>& bytes, size_t at, uint32_t value) {
  for (size_t i = 0; i < 4; ++i) bytes[at + i] = uint8_t(value >> (8 * i));
}
std::vector<uint8_t> localEntry(const char* name, const std::vector<uint8_t>& data) {
  std::vector<uint8_t> bytes(30, 0);
  put32(bytes, 0, 0x04034b50);
  put32(bytes, 14, Articles::crc32(data.data(), data.size()));
  put32(bytes, 18, data.size());
  put32(bytes, 22, data.size());
  bytes[26] = strlen(name);
  bytes.insert(bytes.end(), name, name + strlen(name));
  bytes.insert(bytes.end(), data.begin(), data.end());
  return bytes;
}
std::vector<uint8_t> sample() {
  const std::string mime = "application/epub+zip";
  auto bytes = localEntry("mimetype", std::vector<uint8_t>(mime.begin(), mime.end()));
  Articles::Metadata metadata;
  memcpy(metadata.bytes, "PDA1", 4);
  memcpy(metadata.bytes + 12, "Article", 7);
  memcpy(metadata.bytes + 269, "example.org", 11);
  const auto entry =
      localEntry(Articles::ENTRY, std::vector<uint8_t>(metadata.bytes, metadata.bytes + Articles::METADATA_BYTES));
  bytes.insert(bytes.end(), entry.begin(), entry.end());
  return bytes;
}
}  // namespace
TEST(ArticleStorage, BoundedEntryReaderAcceptsValidAndRejectsCorruption) {
  ArticleSD::files.clear();
  const auto good = sample();
  ArticleSD::files["book"] = good;
  Articles::Metadata metadata;
  ASSERT_TRUE(Articles::readMetadata("book", metadata));
  EXPECT_STREQ(metadata.title(), "Article");
  for (size_t length = 0; length < good.size(); ++length) {
    ArticleSD::files["book"] = std::vector<uint8_t>(good.begin(), good.begin() + length);
    EXPECT_FALSE(Articles::readMetadata("book", metadata));
  }
  for (size_t offset : {size_t(0), size_t(6), size_t(8), size_t(26), size_t(58), size_t(64), size_t(76), size_t(84),
                        size_t(115), size_t(130)}) {
    ArticleSD::files["book"] = good;
    ArticleSD::files["book"][offset] ^= 1;
    EXPECT_FALSE(Articles::readMetadata("book", metadata));
  }
}
TEST(ArticleStorage, ReadStateRequiresExplicitMarkerAndSurvivesReopening) {
  ArticleSD::files.clear();
  ArticleSD::failWrites = false;
  const std::string path = "/Articles/pd-article-00000000-1111-2222-3333-444444444444.epub";
  EXPECT_FALSE(Articles::isRead(path));
  ArticleSD::failWrites = true;
  EXPECT_FALSE(Articles::markRead(path));
  EXPECT_FALSE(Articles::isRead(path));
  ArticleSD::failWrites = false;
  EXPECT_TRUE(Articles::markRead(path));
  EXPECT_TRUE(Articles::isRead(path));
  EXPECT_EQ(Articles::donePath(path), "/.crosspoint/articles/00000000-1111-2222-3333-444444444444.done");
  EXPECT_FALSE(Articles::markRead("/Books/other.epub"));
  ArticleSD::files[Articles::donePath(path)].clear();
  EXPECT_FALSE(Articles::isRead(path));
}
