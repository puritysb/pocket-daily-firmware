#include <gtest/gtest.h>

#include "articles/ArticleFormat.h"

TEST(ArticleFormat, IdentityIsRestrictedToArticlesDirectory) {
  const std::string filename = "pd-article-00000000-1111-2222-3333-444444444444.epub";
  EXPECT_TRUE(Articles::isFilename(filename));
  EXPECT_TRUE(Articles::isPath("/Articles/" + filename));
  EXPECT_FALSE(Articles::isPath("/Books/" + filename));
  EXPECT_FALSE(Articles::isFilename("../" + filename));
  EXPECT_FALSE(Articles::isPath("/Articles/nested/" + filename));
  EXPECT_FALSE(Articles::isFilename("pd-article-00000000-1111-2222-3333-44444444444z.epub"));
}
TEST(ArticleFormat, BoundedMetadataMatchesCompanionLayout) {
  Articles::Metadata data;
  memcpy(data.bytes, "PDA1", 4);
  data.bytes[4] = 232;
  data.bytes[5] = 3;
  memcpy(data.bytes + 12, "한글 article", strlen("한글 article"));
  memcpy(data.bytes + 269, "example.org", 11);
  EXPECT_TRUE(data.valid());
  EXPECT_EQ(data.savedAt(), 1000u);
  EXPECT_STREQ(data.title(), "한글 article");
  EXPECT_STREQ(data.source(), "example.org");
  data.bytes[268] = 1;
  EXPECT_FALSE(data.valid());
}
TEST(ArticleFormat, InvalidUTF8AndControlsAreRejected) {
  for (auto value : {std::string("\xc0\xaf"), std::string("\xed\xa0\x80"), std::string("\xf4\x90\x80\x80"),
                     std::string("bad\ntext")}) {
    uint8_t text[32]{};
    memcpy(text, value.data(), value.size());
    EXPECT_FALSE(Articles::validText(text, sizeof(text)));
  }
  const uint8_t missingTerminator[] = {'a', 'b'};
  EXPECT_FALSE(Articles::validText(missingTerminator, sizeof(missingTerminator)));
}
TEST(ArticleFormat, CRCUsesZIPPolynomial) {
  const uint8_t data[] = "123456789";
  EXPECT_EQ(Articles::crc32(data, 9), 0xcbf43926u);
}
