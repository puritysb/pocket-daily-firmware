#include <HalStorage.h>
#include <gtest/gtest.h>

#include "pocket_daily/PublicationReceipt.h"
namespace Pub = PocketDaily::Publication;
namespace {
const Pub::Request FIRST{"/Books/.pocket-first.part", "/Books/book.epub", 1937, 0xdeadbeef};
const Pub::Request NEXT{"/.pocket-second.part", "/update.bin", 6178192, 0x12345678};
}  // namespace
TEST(PublicationReceipt, ExactRequestSurvivesReloadAndRejectsEveryDamagedByte) {
  FakeSD::reset();
  EXPECT_FALSE(Pub::matches(FIRST));
  ASSERT_TRUE(Pub::save(FIRST));
  const auto saved = FakeSD::files[Pub::RECEIPT_PATH];
  EXPECT_TRUE(Pub::matches(FIRST));
  EXPECT_FALSE(Pub::matches(NEXT));
  for (size_t i = 0; i < saved.size(); ++i) {
    FakeSD::files[Pub::RECEIPT_PATH] = saved;
    FakeSD::files[Pub::RECEIPT_PATH][i] ^= 1;
    EXPECT_FALSE(Pub::matches(FIRST)) << i;
  }
  FakeSD::files[Pub::RECEIPT_PATH] = saved;
  FakeSD::files[Pub::RECEIPT_PATH].pop_back();
  EXPECT_FALSE(Pub::matches(FIRST));
}
TEST(PublicationReceipt, OnlyLatestPublicationIsRemembered) {
  FakeSD::reset();
  ASSERT_TRUE(Pub::save(FIRST));
  ASSERT_TRUE(Pub::save(NEXT));
  EXPECT_FALSE(Pub::matches(FIRST));
  EXPECT_TRUE(Pub::matches(NEXT));
  auto changed = NEXT;
  changed.size++;
  EXPECT_FALSE(Pub::matches(changed));
  changed = NEXT;
  changed.crc32 ^= 1;
  EXPECT_FALSE(Pub::matches(changed));
}
TEST(PublicationReceipt, FailedWritesNeverConfirmNewPublication) {
  for (int fault = 0; fault < 4; ++fault) {
    FakeSD::reset();
    ASSERT_TRUE(Pub::save(FIRST));
    if (fault == 0) FakeSD::writeBudget = 13;
    if (fault == 1) FakeSD::failRename = true;
    if (fault == 2) FakeSD::corruptWrite = true;
    if (fault == 3) FakeSD::failRemove = Pub::RECEIPT_PATH;
    EXPECT_FALSE(Pub::save(NEXT));
    EXPECT_FALSE(Pub::matches(NEXT));
  }
}
TEST(PublicationReceipt, RejectsUnsupportedPathsBeforePublication) {
  EXPECT_TRUE(Pub::valid(FIRST));
  auto invalid = FIRST;
  invalid.staging = nullptr;
  EXPECT_FALSE(Pub::valid(invalid));
  invalid = FIRST;
  invalid.target = "relative.epub";
  EXPECT_FALSE(Pub::valid(invalid));
  std::string path(511, 'a');
  path[0] = '/';
  invalid.target = path.c_str();
  EXPECT_TRUE(Pub::valid(invalid));
  path += 'a';
  invalid.target = path.c_str();
  EXPECT_FALSE(Pub::valid(invalid));
}
