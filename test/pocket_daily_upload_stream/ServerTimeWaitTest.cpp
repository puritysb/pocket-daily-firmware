#include <gtest/gtest.h>

#include <vector>

#include "src/pocket_daily/web/ServerTimeWait.h"

using namespace PocketDaily::Web;

namespace {
// Mirrors lwIP: abort unlinks the PCB from the TIME_WAIT list and frees it.
struct Pcb {
  uint16_t local_port;
  Pcb* next = nullptr;
  bool freed = false;
};

struct List {
  std::vector<Pcb> nodes;
  Pcb* head = nullptr;
  explicit List(std::initializer_list<uint16_t> ports) : nodes(ports.size()) {
    size_t i = 0;
    for (const auto port : ports) nodes[i++].local_port = port;
    for (size_t k = 0; k + 1 < nodes.size(); ++k) nodes[k].next = &nodes[k + 1];
    head = nodes.empty() ? nullptr : &nodes.front();
  }
  void abort(Pcb* pcb) {
    Pcb** link = &head;
    while (*link != pcb) link = &(*link)->next;
    *link = pcb->next;
    pcb->next = reinterpret_cast<Pcb*>(0x1);  // freed memory must not be followed
    pcb->freed = true;
  }
  size_t size() const {
    size_t n = 0;
    for (const Pcb* p = head; p; p = p->next) ++n;
    return n;
  }
};

size_t calls = 0;
size_t fakePurge(uint16_t http, uint16_t stream) {
  EXPECT_EQ(http, 80u);
  EXPECT_EQ(stream, 82u);
  ++calls;
  return 2;
}
}  // namespace

TEST(ServerTimeWait, PurgesOnlyThisServersPortsAndNeverFollowsFreedNodes) {
  List list{80, 5000, 82, 80, 443};
  const auto purged = purgeTimeWaitList(list.head, 80, 82, [&](Pcb* pcb) { list.abort(pcb); });
  EXPECT_EQ(purged, 3u);
  ASSERT_EQ(list.size(), 2u);
  EXPECT_EQ(list.head->local_port, 5000u);
  EXPECT_EQ(list.head->next->local_port, 443u);
  EXPECT_TRUE(list.nodes[0].freed);
  EXPECT_FALSE(list.nodes[1].freed);
  EXPECT_TRUE(list.nodes[3].freed);
}

TEST(ServerTimeWait, EmptyAndUnrelatedListsAreUntouched) {
  List empty{};
  EXPECT_EQ(purgeTimeWaitList(empty.head, 80, 82, [&](Pcb* pcb) { empty.abort(pcb); }), 0u);
  List other{5000, 443};
  EXPECT_EQ(purgeTimeWaitList(other.head, 80, 82, [&](Pcb* pcb) { other.abort(pcb); }), 0u);
  EXPECT_EQ(other.size(), 2u);
}

TEST(ServerTimeWait, RateLimitsUnlessAdmissionIsPending) {
  calls = 0;
  ServerTimeWaitPurge purge(fakePurge);
  purge.service(1000, 80, 82, false);
  EXPECT_EQ(calls, 1u);
  purge.service(1000 + ServerTimeWaitPurge::INTERVAL_MS - 1, 80, 82, false);
  EXPECT_EQ(calls, 1u);
  purge.service(1000 + ServerTimeWaitPurge::INTERVAL_MS - 1, 80, 82, true);  // admission sample follows
  EXPECT_EQ(calls, 2u);
  purge.service(1000 + 2 * ServerTimeWaitPurge::INTERVAL_MS, 80, 82, false);
  EXPECT_EQ(calls, 3u);
  EXPECT_EQ(purge.purged(), 6u);
}

TEST(ServerTimeWait, IntervalSurvivesMillisWrap) {
  calls = 0;
  ServerTimeWaitPurge purge(fakePurge);
  purge.service(UINT32_MAX - 10, 80, 82, false);
  purge.service(5, 80, 82, false);  // 16ms later across the wrap
  EXPECT_EQ(calls, 1u);
  purge.service(ServerTimeWaitPurge::INTERVAL_MS, 80, 82, false);
  EXPECT_EQ(calls, 2u);
}

TEST(ServerTimeWait, HostBuildHasNoLwipAccess) { EXPECT_EQ(purgeServerTimeWait(80, 82), 0u); }
