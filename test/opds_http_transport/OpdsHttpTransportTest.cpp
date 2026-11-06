#include <gtest/gtest.h>

#include "network/HttpDownloader.h"
#include "network/OpdsHttpTransport.h"

namespace fo = freeink::opds;

namespace {
fo::HttpAuth catalogAuth() {
  fo::HttpAuth auth;
  auth.username = "reader";
  auth.password = "secret";
  auth.headers.push_back({"X-Proxy", "token"});
  return auth;
}
}  // namespace

// Credentials and custom headers are bound to the saved server; the HTTP layer
// strips them from any request or redirect that leaves that origin
// (docs/http-credential-origin.md).
TEST(OpdsHttpTransport, BindsCredentialsToTheConfiguredServer) {
  OpdsHttpTransport transport;
  transport.setCredentialOrigin("https://books.example.org/opds");
  const auto result = transport.get("https://books.example.org/opds/new", catalogAuth(), "application/atom+xml",
                                    "ko", true, [](const uint8_t*, size_t) { return true; });
  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.status, 200);
  const auto& sent = http_stub::lastOptions;
  EXPECT_EQ(sent.credentialOrigin, "https://books.example.org/opds");
  EXPECT_EQ(sent.username, "reader");
  EXPECT_EQ(sent.password, "secret");
  ASSERT_EQ(sent.headers.size(), 1U);
  EXPECT_EQ(sent.headers[0].first, "X-Proxy");
  EXPECT_EQ(sent.accept, "application/atom+xml");
  EXPECT_EQ(sent.acceptLanguage, "ko");
  EXPECT_TRUE(sent.captureErrorBody);
}

// A cross-origin link inside the feed keeps the server origin, so the HTTP
// layer sees the mismatch and withholds the credentials.
TEST(OpdsHttpTransport, CrossOriginRequestStillCarriesTheServerOrigin) {
  OpdsHttpTransport transport;
  transport.setCredentialOrigin("https://books.example.org/opds");
  transport.get("https://cdn.example.net/cover.jpg", catalogAuth(), nullptr, nullptr, false,
                [](const uint8_t*, size_t) { return true; });
  EXPECT_EQ(http_stub::lastUrl, "https://cdn.example.net/cover.jpg");
  EXPECT_EQ(http_stub::lastOptions.credentialOrigin, "https://books.example.org/opds");
}

TEST(OpdsHttpTransport, UnsetOriginFallsBackToTheRequestUrl) {
  OpdsHttpTransport transport;
  transport.get("https://books.example.org/opds", fo::HttpAuth{}, nullptr, nullptr, false,
                [](const uint8_t*, size_t) { return true; });
  // Empty means "the request URL's own origin" in HttpDownloader::fetchUrl.
  EXPECT_TRUE(http_stub::lastOptions.credentialOrigin.empty());
}
