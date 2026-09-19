#include <OpdsParser.h>
#include <gtest/gtest.h>

#include <string>

namespace {
std::string entry(const std::string& title = "Book") {
  return "<entry><title>" + title +
         "</title><author><name>Author</name></author><id>id</id>"
         "<link rel='http://opds-spec.org/acquisition' type='application/epub+zip' href='book.epub'/></entry>";
}
void parse(OpdsParser& parser, const std::string& xml, size_t chunk = 7) {
  for (size_t offset = 0; offset < xml.size() && !parser.error(); offset += chunk) {
    parser.write(reinterpret_cast<const uint8_t*>(xml.data() + offset), std::min(chunk, xml.size() - offset));
  }
  parser.flush();
}
}  // namespace

TEST(OpdsParser, ParsesChunkedEntriesAndPagination) {
  OpdsParser parser;
  parse(parser, "<feed><link rel='next' href='page2'/><link rel='previous' href='page0'/>" + entry("첫 책") +
                    entry("Second") + "</feed>");
  ASSERT_FALSE(parser.error());
  ASSERT_EQ(parser.getEntries().size(), 2);
  EXPECT_EQ(parser.getEntries()[0].title, "첫 책");
  EXPECT_EQ(parser.getEntries()[0].href, "book.epub");
  EXPECT_EQ(parser.getEntries()[1].author, "Author");
  EXPECT_EQ(parser.takeNextPageUrl(), "page2");
  EXPECT_EQ(parser.takePrevPageUrl(), "page0");
  ASSERT_TRUE(parser.reserveNavigationEntries());
  auto entries = std::move(parser).getEntries();
  EXPECT_GE(entries.capacity(), entries.size() + 2);
}

TEST(OpdsParser, RejectsOversizedTextWithoutReturningSuccess) {
  OpdsParser parser;
  parse(parser, "<feed>" + entry(std::string(OpdsParser::MAX_FIELD_BYTES + 1, 'a')) + "</feed>");
  EXPECT_TRUE(parser.error());
  EXPECT_TRUE(parser.resourceLimitReached());
}

TEST(OpdsParser, RejectsOversizedLink) {
  OpdsParser parser;
  parse(parser, "<feed><link rel='next' href='" + std::string(OpdsParser::MAX_FIELD_BYTES + 1, 'a') + "'/></feed>");
  EXPECT_TRUE(parser.error());
  EXPECT_TRUE(parser.resourceLimitReached());
}

TEST(OpdsParser, RejectsCatalogOverflowRatherThanSilentlyTruncating) {
  std::string xml = "<feed>";
  for (size_t i = 0; i <= OpdsParser::MAX_ENTRIES; ++i) xml += entry();
  xml += "</feed>";
  OpdsParser parser;
  parse(parser, xml, 256);
  EXPECT_TRUE(parser.error());
  EXPECT_TRUE(parser.resourceLimitReached());
  EXPECT_EQ(parser.getEntries().size(), OpdsParser::MAX_ENTRIES);
}

TEST(OpdsParser, AcceptsCatalogAtLimit) {
  std::string xml = "<feed>";
  for (size_t i = 0; i < OpdsParser::MAX_ENTRIES; ++i) xml += entry();
  xml += "</feed>";
  OpdsParser parser;
  parse(parser, xml, 128);
  EXPECT_FALSE(parser.error());
  EXPECT_TRUE(parser.reserveNavigationEntries());
}

TEST(OpdsParser, RefusesInitialAllocationWhenMemoryIsUnavailable) {
  OpdsParser parser([](size_t) { return false; });
  EXPECT_TRUE(parser.error());
  EXPECT_TRUE(parser.resourceLimitReached());
  EXPECT_EQ(parser.write(uint8_t{'x'}), 0);
  parser.flush();
}

TEST(OpdsParser, StopsWhenEntryVectorCannotGrow) {
  // Permit XML chunks and construction, reject only the first vector reserve.
  const size_t vectorBytes = 8 * sizeof(OpdsEntry);
  static size_t deniedBytes;
  deniedBytes = vectorBytes;
  OpdsParser parser([](size_t bytes) { return bytes != deniedBytes; });
  parse(parser, "<feed>" + entry() + "</feed>");
  EXPECT_TRUE(parser.error());
  EXPECT_TRUE(parser.resourceLimitReached());
  EXPECT_TRUE(parser.getEntries().empty());
}

TEST(OpdsParser, RejectsMalformedXml) {
  OpdsParser parser;
  parse(parser, "<feed><entry></feed>");
  EXPECT_TRUE(parser.error());
}
