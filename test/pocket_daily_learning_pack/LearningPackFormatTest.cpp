#include <gtest/gtest.h>

#include <cstddef>
#include <type_traits>

#include "pocket_daily/learning_pack.h"

namespace {

TEST(LearningPackFormat, DiskLayoutIsStable) {
  EXPECT_EQ(sizeof(PocketDaily::LearningPack::Header), 388U);
  EXPECT_EQ(sizeof(PocketDaily::LearningPack::Record), 928U);
  EXPECT_EQ(offsetof(PocketDaily::LearningPack::Header, payloadSha256), 352U);
  EXPECT_EQ(offsetof(PocketDaily::LearningPack::Header, headerFnv32), 384U);
  EXPECT_TRUE(std::is_trivially_copyable_v<PocketDaily::LearningPack::Header>);
  EXPECT_TRUE(std::is_trivially_copyable_v<PocketDaily::LearningPack::Record>);
}

TEST(LearningPackFormat, DeviceContractUsesBoundedSdPath) {
  EXPECT_STREQ(PocketDaily::LearningPack::PACKAGE_ID, "jp-n3-ko");
  EXPECT_STREQ(PocketDaily::LearningPack::PACK_PATH, "/pocket-daily/learning/jp-n3-ko.pdl");
  EXPECT_EQ(PocketDaily::LearningPack::FORMAT_VERSION, 1U);
  EXPECT_EQ(PocketDaily::LearningPack::MAX_PACK_BYTES, 16U * 1024U * 1024U);
}

}  // namespace
