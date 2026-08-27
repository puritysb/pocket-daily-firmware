#include <gtest/gtest.h>

#include "src/agentdeck/endpoint_candidates.h"
#include "src/pocket_daily/product_identity.h"

TEST(PocketDailyIdentity, PublishesCanonicalSurfaceIdentity) {
  EXPECT_STREQ(PocketDaily::PRODUCT_ID, "io.pocketdaily.reader");
  EXPECT_STREQ(PocketDaily::SURFACE_PROFILE, "portable-reader/v1");
  EXPECT_STREQ(PocketDaily::UPDATE_CHANNEL, "stable");
}

TEST(PocketDailyIdentity, AcceptsOnlyCompleteExactOtaTuple) {
  EXPECT_TRUE(PocketDaily::otaIdentityMatches("io.pocketdaily.reader", "xteink_x3", "stable", "xteink_x3"));
  EXPECT_TRUE(PocketDaily::otaIdentityMatches("io.pocketdaily.reader", "xteink_x4", "stable", "xteink_x4"));

  EXPECT_FALSE(PocketDaily::otaIdentityMatches("", "xteink_x3", "stable", "xteink_x3"));
  EXPECT_FALSE(PocketDaily::otaIdentityMatches("io.pocketdaily.reader", "", "stable", "xteink_x3"));
  EXPECT_FALSE(PocketDaily::otaIdentityMatches("io.pocketdaily.reader", "xteink_x3", "", "xteink_x3"));
  EXPECT_FALSE(PocketDaily::otaIdentityMatches("io.agentdeck.dashboard", "xteink_x3", "stable", "xteink_x3"));
  EXPECT_FALSE(PocketDaily::otaIdentityMatches("io.pocketdaily.reader", "xteink_x4", "stable", "xteink_x3"));
  EXPECT_FALSE(PocketDaily::otaIdentityMatches("io.pocketdaily.reader", "xteink_x3", "beta", "xteink_x3"));
  EXPECT_FALSE(PocketDaily::otaIdentityMatches(nullptr, "xteink_x3", "stable", "xteink_x3"));
}

TEST(PocketDailyIdentity, DoesNotClaimUnimplementedWebSocketInbox) {
  EXPECT_EQ(strstr(PocketDaily::SURFACE_CAPABILITIES, "inbox.ws"), nullptr);
}

TEST(PocketDailyIdentity, NegotiatesPersistedWeatherOutlook) {
  EXPECT_NE(strstr(PocketDaily::SURFACE_CAPABILITIES, "weather.snapshot.read"), nullptr);
}

TEST(PocketDailyIdentity, NegotiatesStandardPartialOtaResume) {
  EXPECT_NE(strstr(PocketDaily::SURFACE_CAPABILITIES, "ota.resume-206"), nullptr);
}

TEST(PocketDailyEndpointCandidates, KeepsUniqueAddressesWithinFixedCapacity) {
  AgentDeck::Net::EndpointCandidates endpoints;
  endpoints.port = 9120;
  EXPECT_TRUE(AgentDeck::Net::endpointCandidateAdd(endpoints, "192.168.68.100"));
  EXPECT_TRUE(AgentDeck::Net::endpointCandidateAdd(endpoints, "192.168.68.60"));
  EXPECT_TRUE(AgentDeck::Net::endpointCandidateAdd(endpoints, "192.168.68.100"));
  EXPECT_EQ(endpoints.count, 2);
  EXPECT_STREQ(endpoints.ips[0], "192.168.68.100");
  EXPECT_STREQ(endpoints.ips[1], "192.168.68.60");
}

TEST(PocketDailyEndpointCandidates, PromotesSuccessfulAddressWithoutLosingFallbacks) {
  AgentDeck::Net::EndpointCandidates endpoints;
  AgentDeck::Net::endpointCandidateAdd(endpoints, "192.168.68.100");
  AgentDeck::Net::endpointCandidateAdd(endpoints, "192.168.68.60");
  EXPECT_TRUE(AgentDeck::Net::endpointCandidatePromote(endpoints, "192.168.68.60"));
  ASSERT_EQ(endpoints.count, 2);
  EXPECT_STREQ(endpoints.ips[0], "192.168.68.60");
  EXPECT_STREQ(endpoints.ips[1], "192.168.68.100");
}

TEST(PocketDailyEndpointCandidates, FullSetEvictsOnlyTailForNewWinner) {
  AgentDeck::Net::EndpointCandidates endpoints;
  AgentDeck::Net::endpointCandidateAdd(endpoints, "10.0.0.1");
  AgentDeck::Net::endpointCandidateAdd(endpoints, "10.0.0.2");
  AgentDeck::Net::endpointCandidateAdd(endpoints, "10.0.0.3");
  AgentDeck::Net::endpointCandidateAdd(endpoints, "10.0.0.4");
  EXPECT_TRUE(AgentDeck::Net::endpointCandidatePromote(endpoints, "10.0.0.9"));
  ASSERT_EQ(endpoints.count, AgentDeck::Net::ENDPOINT_CANDIDATE_CAP);
  EXPECT_STREQ(endpoints.ips[0], "10.0.0.9");
  EXPECT_STREQ(endpoints.ips[1], "10.0.0.1");
  EXPECT_STREQ(endpoints.ips[3], "10.0.0.3");
}
