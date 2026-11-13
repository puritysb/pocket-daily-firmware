#include <HalStorage.h>
#include <gtest/gtest.h>

#include <cstring>

#include "pocket_daily/PocketReadingShelf.h"
using namespace PocketDaily::ReadingShelf;
TEST(ReadingShelf, DistinctOrderedPathsRoundTripAndRejectCorruption) {
  Selection s;
  s.automatic = false;
  s.count = 3;
  strcpy(s.paths[0], "/Books/Third.epub");
  strcpy(s.paths[1], "/First.epub");
  strcpy(s.paths[2], "/둘째.epub");
  uint8_t bytes[RECORD_BYTES];
  ASSERT_TRUE(encode(s, bytes));
  Selection out;
  ASSERT_TRUE(decode(bytes, sizeof(bytes), out));
  EXPECT_FALSE(out.automatic);
  EXPECT_EQ(out.count, 3);
  for (int i = 0; i < 4; ++i) EXPECT_STREQ(out.paths[i], s.paths[i]);
  bytes[300] ^= 1;
  EXPECT_FALSE(decode(bytes, sizeof(bytes), out));
  EXPECT_TRUE(out.automatic);
  EXPECT_EQ(out.count, 0);
  EXPECT_FALSE(decode(bytes, sizeof(bytes) - 1, out));
  strcpy(s.paths[1], s.paths[0]);
  EXPECT_FALSE(valid(s));
  strcpy(s.paths[1], "/Books/../private.epub");
  EXPECT_FALSE(valid(s));
  strcpy(s.paths[1], "/Good.epub");
  s.paths[3][5] = 'x';
  EXPECT_FALSE(valid(s));
}
TEST(ReadingShelf, AtomicSaveKeepsPreviousAndAutoClearsManualSelection) {
  FakeSD::reset();
  Selection s;
  s.automatic = false;
  s.count = 1;
  strcpy(s.paths[0], "/First.epub");
  ASSERT_EQ(save(s), SaveResult::Ok);
  strcpy(s.paths[0], "/Second.epub");
  FakeSD::failRename = true;
  EXPECT_EQ(save(s), SaveResult::StorageError);
  FakeSD::failRename = false;
  Selection out;
  ASSERT_TRUE(load(out));
  EXPECT_STREQ(out.paths[0], "/First.epub");
  ASSERT_EQ(save(s), SaveResult::Ok);
  ASSERT_TRUE(load(out));
  EXPECT_STREQ(out.paths[0], "/Second.epub");
  s = {};
  ASSERT_EQ(save(s), SaveResult::Ok);
  ASSERT_TRUE(load(out));
  EXPECT_TRUE(out.automatic);
  EXPECT_EQ(out.count, 0);
}
TEST(ReadingShelf, InvalidActiveKeepsVerifiedBackupAcrossFailedReplacement) {
  FakeSD::reset();
  Selection s;
  s.automatic = false;
  s.count = 1;
  strcpy(s.paths[0], "/Saved.epub");
  ASSERT_EQ(save(s), SaveResult::Ok);
  FakeSD::files[BACKUP_PATH] = FakeSD::files[PATH];
  FakeSD::files[PATH][12] ^= 1;
  Selection replacement;
  FakeSD::failRename = true;
  EXPECT_EQ(save(replacement), SaveResult::StorageError);
  FakeSD::failRename = false;
  Selection out;
  ASSERT_TRUE(load(out));
  EXPECT_STREQ(out.paths[0], "/Saved.epub");
}
TEST(ReadingShelf, JsonRejectsMalformedSelectionsWithoutPartialState) {
  Selection s;
  bool oom = false;
  const char* validJson = R"({"schema":1,"automatic":false,"paths":["/Third.epub","/First.epub"]})";
  ASSERT_TRUE(parseJson(validJson, strlen(validJson), s, oom));
  EXPECT_STREQ(s.paths[1], "/First.epub");
  for (const char* bad :
       {R"({"schema":1,"automatic":true,"paths":["/First.epub"]})", R"({"schema":1,"automatic":false,"paths":[]})",
        R"({"schema":1,"automatic":1,"paths":[]})", R"({"schema":1,"automatic":false,"paths":["/a.epub","/a.epub"]})",
        R"({"schema":1,"automatic":false,"paths":["/a\u0000.epub"]})",
        R"({"schema":1,"automatic":true,"paths":[],"unknown":0})"}) {
    EXPECT_FALSE(parseJson(bad, strlen(bad), s, oom));
    EXPECT_TRUE(s.automatic);
    EXPECT_EQ(s.count, 0);
  }
}
