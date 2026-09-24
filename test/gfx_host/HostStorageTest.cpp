#include <HalStorage.h>
#include <gtest/gtest.h>

#include <array>
#include <future>
#include <limits>

using PocketUIHost::Asset;
using PocketUIHost::AssetScope;

TEST(HostStorage, ReadsBoundedImmutableAssetsAndRejectsUnknownPaths) {
  const std::array<uint8_t, 3> bytes{10, 20, 30};
  const Asset assets[]{{"/font", bytes}, {"/empty", {}}};
  const AssetScope scope(assets);
  HalFile file;
  ASSERT_TRUE(Storage.openFileForRead("test", "/font", file));
  EXPECT_EQ(file.size(), 3U);
  EXPECT_EQ(file.read(nullptr, 1), -1);
  EXPECT_EQ(file.read(), 10);
  EXPECT_FALSE(file.seekSet(4));
  EXPECT_FALSE(file.seekCur(std::numeric_limits<size_t>::max()));
  EXPECT_EQ(file.read(), 20);  // Failed seeks/reads preserve position.
  EXPECT_TRUE(file.seekCur(1));
  EXPECT_EQ(file.read(), -1);
  EXPECT_TRUE(file.seekSet(0));
  std::array<uint8_t, 5> output{99, 99, 99, 99, 99};
  EXPECT_EQ(file.read(output.data(), output.size()), 3);
  EXPECT_EQ(output, (std::array<uint8_t, 5>{10, 20, 30, 99, 99}));
  EXPECT_TRUE(Storage.openFileForRead("test", "/empty", file));
  EXPECT_TRUE(static_cast<bool>(file));
  EXPECT_EQ(file.read(nullptr, 0), 0);
  EXPECT_FALSE(Storage.openFileForRead("test", "/etc/passwd", file));
  EXPECT_FALSE(static_cast<bool>(file));
  EXPECT_EQ(file.read(), -1);
  EXPECT_FALSE(Storage.openFileForRead("test", nullptr, file));
}

TEST(HostStorage, NestedScopesRestoreBindingAndKeepOpenHandlesIndependent) {
  const std::array<uint8_t, 1> a{1}, b{2};
  const Asset outer[]{{"/font", a}}, inner[]{{"/font", b}};
  HalFile first, second;
  EXPECT_FALSE(Storage.openFileForRead("test", "/font", first));
  {
    const AssetScope scope(outer);
    ASSERT_TRUE(Storage.openFileForRead("test", "/font", first));
    {
      const AssetScope nested(inner);
      ASSERT_TRUE(Storage.openFileForRead("test", "/font", second));
      EXPECT_EQ(first.read(), 1);
      EXPECT_EQ(second.read(), 2);
    }
    ASSERT_TRUE(Storage.openFileForRead("test", "/font", first));
    EXPECT_EQ(first.read(), 1);
  }
  EXPECT_FALSE(Storage.openFileForRead("test", "/font", first));
}

TEST(HostStorage, DuplicateNamesFailClosed) {
  const std::array<uint8_t, 1> bytes{1};
  const Asset assets[]{{"/font", bytes}, {"/font", bytes}};
  const AssetScope scope(assets);
  HalFile file;
  EXPECT_FALSE(Storage.openFileForRead("test", "/font", file));
}

TEST(HostStorage, ThreadsCannotSeeOtherContextsBindings) {
  const std::array<uint8_t, 1> bytes{1};
  const Asset assets[]{{"/font", bytes}};
  const AssetScope scope(assets);
  auto result = std::async(std::launch::async, [] {
    HalFile file;
    const bool hidden = !Storage.openFileForRead("test", "/font", file);
    const std::array<uint8_t, 1> own{2};
    const Asset other[]{{"/font", own}};
    const AssetScope nested(other);
    return hidden && Storage.openFileForRead("test", "/font", file) && file.read() == 2;
  });
  EXPECT_TRUE(result.get());
  HalFile file;
  ASSERT_TRUE(Storage.openFileForRead("test", "/font", file));
  EXPECT_EQ(file.read(), 1);
}
