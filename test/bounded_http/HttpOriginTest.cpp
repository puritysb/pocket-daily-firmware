#include <gtest/gtest.h>

#include "network/HttpOrigin.h"

TEST(HttpOrigin, SameOriginNormalizesCaseAndDefaultPorts) {
  EXPECT_TRUE(HttpOrigin::same("https://Books.Example/catalog", "HTTPS://books.example:443/book"));
  EXPECT_TRUE(HttpOrigin::same("books.example/catalog", "http://BOOKS.example:80/book"));
  EXPECT_TRUE(HttpOrigin::same("https://[2001:DB8::1]:443/", "https://[2001:db8::1]/book"));
  EXPECT_TRUE(HttpOrigin::same("https://books.example:8443/", "https://books.example:08443/book"));
}
TEST(HttpOrigin, CredentialsNeverFollowForeignOriginOrSchemeOrPort) {
  for (auto target : {"https://cdn.example/book", "https://books.example.evil/book", "https://books.example:444/book",
                      "http://books.example/book", "http://books.example:443/book"}) {
    EXPECT_FALSE(HttpOrigin::same("https://books.example/feed", target)) << target;
  }
}
TEST(HttpOrigin, AbsoluteAcquisitionAndFeedLinksUseConfiguredServerOrigin) {
  constexpr auto server = "https://books.example/catalog";
  EXPECT_TRUE(HttpOrigin::allowsRequest(server, "https://cdn.example/book"));
  EXPECT_FALSE(HttpOrigin::same(server, "https://cdn.example/book"));
  EXPECT_FALSE(HttpOrigin::allowsRequest(server, "http://cdn.example/book"));
  EXPECT_FALSE(HttpOrigin::allowsRequest(server, "http://books.example/book"));
}
TEST(HttpOrigin, EachHopChecksDowngradeAndKeepsCredentialsBoundToOriginalServer) {
  constexpr auto origin = "https://books.example/feed";
  EXPECT_TRUE(HttpOrigin::allowsRequest(origin, "https://cdn.example/book"));
  EXPECT_FALSE(HttpOrigin::same(origin, "https://cdn.example/book"));
  EXPECT_FALSE(HttpOrigin::same(origin, "https://cdn.example/second"));
  EXPECT_FALSE(HttpOrigin::allowsRequest("https://cdn.example/second", "http://other.example/book"));
  EXPECT_TRUE(HttpOrigin::same(origin, "https://books.example/returned"));
  EXPECT_TRUE(HttpOrigin::allowsRequest("http://books.example/", "https://books.example/book"));
  EXPECT_FALSE(HttpOrigin::same("http://books.example/", "https://books.example/book"));
}
TEST(HttpOrigin, RejectsAmbiguousAndMalformedAuthoritiesBeforeRequest) {
  for (auto target :
       {"", "books.example/book", "ftp://books.example/book", "https:///book", "https://user:pass@books.example/",
        "https://books.example@evil.example/", "https://books.example\\@evil.example/", "https://books.example:0/",
        "https://books.example:65536/", "https://books.example:9999999999999/", "https://books.example:/",
        "https://books.example:443:80/", "https://books%2eexample/", "https://[::1]evil/", "https://[]/",
        "https://[hostname]/", "https://books.example/\r\nHost: evil"}) {
    EXPECT_FALSE(HttpOrigin::allowsRequest("https://books.example/", target)) << target;
    EXPECT_FALSE(HttpOrigin::same("https://books.example/", target)) << target;
  }
}
TEST(HttpOrigin, QueryFragmentAndPathNeverChangeOrigin) {
  EXPECT_TRUE(HttpOrigin::same("https://books.example", "https://books.example?next=evil.example"));
  EXPECT_TRUE(HttpOrigin::same("https://books.example/", "https://books.example/#@evil.example"));
  EXPECT_TRUE(HttpOrigin::same("https://books.example/", "https://books.example/path/evil.example"));
}
