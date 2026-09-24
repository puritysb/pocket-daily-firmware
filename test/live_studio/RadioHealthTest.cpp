#include <gtest/gtest.h>
#include <unistd.h>

#include "pocket_daily/boot/DevBootReturn.h"
#include "pocket_daily/web/ExactRouteDispatch.h"
#include "pocket_daily/web/Profile.h"
#include "pocket_daily/web/RadioHealthPolicy.h"
#include "pocket_daily/web/SocketReceive.h"
#include "pocket_daily/web/TransferFocus.h"
#include "pocket_daily/web/WifiBufferBudget.h"

namespace {

TEST(ExactRouteDispatch, ExactPathAndMethodInvokeOnlyOneHandler) {
  using PocketDaily::Web::ExactRouteDispatch;
  int calls = 0;
  ExactRouteDispatch routes("/api/pocket/v1/preferences", 2);
  routes.on("/api/pocket/v1/preferences", 1, [&] { ++calls; });
  routes.on("/api/pocket/v1/preference", 2, [&] { ++calls; });
  EXPECT_FALSE(routes.handled());
  routes.on("/api/pocket/v1/preferences", 2, [&] { ++calls; });
  routes.on("/api/pocket/v1/preferences", 2, [&] { calls += 100; });
  EXPECT_EQ(calls, 1);
  EXPECT_TRUE(routes.handled());
}

TEST(ExactRouteDispatch, UnknownPathsStayUnhandledAndBorrowedLengthIsRespected) {
  using PocketDaily::Web::ExactRouteDispatch;
  for (auto uri : {"", "/API/status", "/api/status/", "/api/status?x=1", "/api/status-extra"}) {
    ExactRouteDispatch routes(uri, 1);
    routes.on("/api/status", 1, [] { FAIL() << "Non-exact route accepted"; });
    EXPECT_FALSE(routes.handled());
  }
  const char data[] = "/api/status-not-in-view";
  ExactRouteDispatch routes(std::string_view(data, 11), 1);
  int calls = 0;
  routes.on("/api/status", 1, [&] { ++calls; });
  EXPECT_EQ(calls, 1);
}

using PocketDaily::RadioHealth::AssociationPolicy;

TEST(CompanionProfile, SharedWiFiSeparatesAppResourcesFromBrowserAndPrivateAP) {
  using namespace PocketDaily::Web;
  EXPECT_EQ(selectProfile(false, true, true), Profile::COMPANION);
  EXPECT_EQ(selectProfile(false, false, true), Profile::COMPANION);
  EXPECT_FALSE(hasBrowserRoutes(Profile::COMPANION));
  EXPECT_EQ(selectProfile(true, true, true), Profile::POCKET_SYNC);
  EXPECT_EQ(selectProfile(false, true), Profile::FILE_TRANSFER);
  EXPECT_EQ(selectProfile(false, false), Profile::FULL);
  EXPECT_TRUE(hasBrowserRoutes(Profile::FILE_TRANSFER));
  EXPECT_TRUE(hasBrowserRoutes(Profile::FULL));
}

TEST(TransferFocus, HoldsThroughReplyAndWaitsBetweenFiles) {
  PocketDaily::Web::TransferFocus focus;
  EXPECT_TRUE(focus.allowsListener(0));
  focus.begin();
  EXPECT_FALSE(focus.allowsListener(60000));
  focus.end(60000);
  EXPECT_FALSE(focus.allowsListener(64999));
  EXPECT_TRUE(focus.allowsListener(65000));
  focus.begin();
  EXPECT_FALSE(focus.allowsListener(70000));
  focus.end(70000);
  EXPECT_FALSE(focus.allowsListener(74999));
  EXPECT_TRUE(focus.allowsListener(75000));
}

TEST(CompanionProfile, BothSyncBearersReserveResourcesForTransfers) {
  using namespace PocketDaily::Web;
  for (const bool x3 : {false, true}) {
    for (const bool privateAp : {false, true}) {
      const auto profile = selectProfile(privateAp, x3, true);
      EXPECT_TRUE(isSyncProfile(profile));
      EXPECT_FALSE(hasBrowserRoutes(profile));
      EXPECT_FALSE(allowsLivePush(profile, privateAp));
      // Even a mismatched mode flag must not enable a Sync listener.
      EXPECT_FALSE(allowsLivePush(profile, !privateAp));
    }
  }
  for (const auto profile : {Profile::FULL, Profile::FILE_TRANSFER}) {
    EXPECT_FALSE(isSyncProfile(profile));
    EXPECT_TRUE(allowsLivePush(profile, false));
    EXPECT_FALSE(allowsLivePush(profile, true));
  }
}

TEST(TransferFocus, CooldownSurvivesMillisWrap) {
  PocketDaily::Web::TransferFocus focus;
  focus.begin();
  focus.end(UINT32_MAX - 1000);
  EXPECT_FALSE(focus.allowsListener(3998));
  EXPECT_TRUE(focus.allowsListener(3999));
}

TEST(AssociationPolicy, ConnectedIdleNeverAbandons) {
  AssociationPolicy policy;
  for (const uint32_t now : {0u, 10000u, 30000u, 600000u, UINT32_MAX}) {
    const auto state = policy.observe(true, now);
    EXPECT_FALSE(state.repaint);
    EXPECT_FALSE(state.abandon);
    EXPECT_TRUE(policy.cleanAssociation());
  }
}

TEST(AssociationPolicy, DisconnectionAtZeroKeepsFullGracePeriod) {
  AssociationPolicy policy;
  EXPECT_TRUE(policy.observe(false, 0).repaint);
  EXPECT_FALSE(policy.cleanAssociation());
  EXPECT_FALSE(policy.observe(false, 1000).repaint);
  EXPECT_FALSE(policy.observe(false, AssociationPolicy::ABANDON_MS).abandon);
  EXPECT_TRUE(policy.observe(false, AssociationPolicy::ABANDON_MS + 1).abandon);
}

TEST(AssociationPolicy, RecoveryResetsGracePeriodAndRepaintsOnce) {
  AssociationPolicy policy;
  policy.observe(false, 100);
  EXPECT_TRUE(policy.observe(true, 300000).repaint);
  EXPECT_TRUE(policy.cleanAssociation());
  EXPECT_FALSE(policy.observe(true, 300001).repaint);
  EXPECT_TRUE(policy.observe(false, 400000).repaint);
  EXPECT_FALSE(policy.observe(false, 700000).abandon);
  EXPECT_TRUE(policy.observe(false, 700001).abandon);
}

TEST(AssociationPolicy, TimerWrapDoesNotShortenGracePeriod) {
  AssociationPolicy policy;
  const uint32_t start = UINT32_MAX - 1000;
  policy.observe(false, start);
  EXPECT_FALSE(policy.observe(false, start + AssociationPolicy::ABANDON_MS).abandon);
  EXPECT_TRUE(policy.observe(false, start + AssociationPolicy::ABANDON_MS + 1).abandon);
}

TEST(AssociationPolicy, HealthyObservationAfterLongGapNeverAbandons) {
  AssociationPolicy policy;
  policy.observe(false, 100);
  const auto recovered = policy.observe(true, 900000);
  EXPECT_TRUE(recovered.repaint);
  EXPECT_FALSE(recovered.abandon);
}

}  // namespace

TEST(WifiBufferBudget, BoundsUnlimitedAndBurstBudgets) {
  using namespace PocketDaily::Web::WifiBufferBudget;
  EXPECT_EQ(bounded(0, DYNAMIC_RX), 8);
  EXPECT_EQ(bounded(32, DYNAMIC_RX), 8);
  EXPECT_EQ(bounded(32, DYNAMIC_TX), 8);
  EXPECT_EQ(bounded(4, STATIC_RX), 4);
}

TEST(WifiBufferBudget, PreservesSmallerPositiveLimits) {
  using namespace PocketDaily::Web::WifiBufferBudget;
  EXPECT_EQ(bounded(2, DYNAMIC_RX), 2);
  EXPECT_EQ(bounded(2, RX_BLOCK_ACK), 2);
  EXPECT_LE(RX_BLOCK_ACK, STATIC_RX);
  EXPECT_LE(STATIC_RX, DYNAMIC_RX);
}

TEST(SocketReceive, PendingPeekPayloadAndOrderlyClose) {
  using namespace PocketDaily::Web;
  struct Pair {
    int fd[2] = {-1, -1};
    ~Pair() {
      for (int socket : fd)
        if (socket >= 0) close(socket);
    }
  } pair;
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, pair.fd), 0);
  char output[8] = {};
  EXPECT_EQ(receiveSocket(pair.fd[0], output, sizeof(output)).state, ReceiveState::Pending);
  ASSERT_EQ(send(pair.fd[1], "abc", 3, 0), 3);
  ASSERT_EQ(shutdown(pair.fd[1], SHUT_WR), 0);
  EXPECT_EQ(receiveSocket(pair.fd[0], output, 1, true).count, 1U);
  auto result = receiveSocket(pair.fd[0], output, sizeof(output));
  EXPECT_EQ(result.state, ReceiveState::Data);
  EXPECT_EQ(result.count, 3U);
  EXPECT_STREQ(output, "abc");
  EXPECT_EQ(receiveSocket(pair.fd[0], output, 1, true).state, ReceiveState::Closed);
  EXPECT_EQ(receiveSocket(pair.fd[0], output, sizeof(output)).state, ReceiveState::Closed);
}

TEST(SocketReceive, InvalidDescriptorIsTerminal) {
  char output;
  EXPECT_EQ(PocketDaily::Web::receiveSocket(-1, &output, 1).state, PocketDaily::Web::ReceiveState::Failed);
}
TEST(DevBootReturn, PreservesProfileAndNeverReusesPrivateLease) {
  using namespace PocketDaily;
  EXPECT_EQ(Boot::decodeDevBootMarker("1", 1), Boot::DevBootReturn::FileTransferSta);
  for (auto profile :
       {Web::Profile::FULL, Web::Profile::FILE_TRANSFER, Web::Profile::COMPANION, Web::Profile::POCKET_SYNC}) {
    for (bool ap : {false, true}) {
      const char marker = Boot::devBootMarker(profile, ap);
      const auto expected = Web::isSyncProfile(profile)
                                ? (ap ? Boot::DevBootReturn::SyncMenu : Boot::DevBootReturn::SyncSta)
                                : (ap ? Boot::DevBootReturn::FileTransferMenu : Boot::DevBootReturn::FileTransferSta);
      EXPECT_EQ(Boot::decodeDevBootMarker(&marker, 1), expected);
    }
  }
}

TEST(DevBootReturn, MalformedMarkersCannotStartRadio) {
  using namespace PocketDaily::Boot;
  EXPECT_EQ(decodeDevBootMarker(nullptr, 1), DevBootReturn::None);
  EXPECT_EQ(decodeDevBootMarker("", 0), DevBootReturn::None);
  EXPECT_EQ(decodeDevBootMarker("S\n", 2), DevBootReturn::None);
  EXPECT_EQ(decodeDevBootMarker("x", 1), DevBootReturn::None);
}
