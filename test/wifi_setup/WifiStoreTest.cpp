#include <HalStorage.h>
#include <gtest/gtest.h>

#include "WifiCredentialStore.h"

class WifiSetupStore : public testing::Test {
 protected:
  void SetUp() override {
    FakeSD::reset();
    WIFI_STORE.clearAll();
  }
  void saved() { ASSERT_TRUE(WIFI_STORE.saveProvisionedCredential("Old network", "old-pass-123")); }
};
TEST_F(WifiSetupStore, SuccessSurvivesReloadAndUpdatesLastNetwork) {
  saved();
  ASSERT_TRUE(WIFI_STORE.saveProvisionedCredential("New network", "new-pass-987"));
  ASSERT_TRUE(WIFI_STORE.loadFromFile());
  EXPECT_EQ(WIFI_STORE.getCredentialCount(), 2u);
  EXPECT_EQ(WIFI_STORE.findCredential("Old network")->password, "old-pass-123");
  EXPECT_EQ(WIFI_STORE.findCredential("New network")->password, "new-pass-987");
  EXPECT_EQ(WIFI_STORE.getLastConnectedSsid(), "New network");
  EXPECT_FALSE(Storage.exists("/.crosspoint/wifi.setup-old"));
}
TEST_F(WifiSetupStore, ShortWritePreservesDiskAndMemory) {
  saved();
  const auto original = FakeSD::files.at(WifiCredentialStore::getFilePath());
  FakeSD::writeBudget = 20;
  EXPECT_FALSE(WIFI_STORE.saveProvisionedCredential("Old network", "replacement-456"));
  EXPECT_EQ(FakeSD::files.at(WifiCredentialStore::getFilePath()), original);
  EXPECT_EQ(WIFI_STORE.findCredential("Old network")->password, "old-pass-123");
  EXPECT_FALSE(Storage.exists("/.crosspoint/wifi.setup-new"));
}
TEST_F(WifiSetupStore, FailedRenamePreservesPriorCredentials) {
  saved();
  const auto original = FakeSD::files.at(WifiCredentialStore::getFilePath());
  FakeSD::failRename = true;
  EXPECT_FALSE(WIFI_STORE.saveProvisionedCredential("New network", "replacement-456"));
  EXPECT_EQ(FakeSD::files.at(WifiCredentialStore::getFilePath()), original);
  EXPECT_FALSE(WIFI_STORE.hasSavedCredential("New network"));
}
TEST_F(WifiSetupStore, InterruptedReplacementRestoresBackup) {
  saved();
  ASSERT_TRUE(Storage.rename(WifiCredentialStore::getFilePath(), "/.crosspoint/wifi.setup-old"));
  FakeSD::files[WifiCredentialStore::getFilePath()] = {'{'};
  ASSERT_TRUE(WIFI_STORE.loadFromFile());
  EXPECT_EQ(WIFI_STORE.findCredential("Old network")->password, "old-pass-123");
  EXPECT_EQ(WIFI_STORE.getLastConnectedSsid(), "Old network");
}
TEST_F(WifiSetupStore, FullStoreDoesNotEvictOrChangeLastNetwork) {
  for (int i = 0; i < 8; ++i)
    ASSERT_TRUE(WIFI_STORE.saveProvisionedCredential("Network-" + std::to_string(i), "password-123"));
  EXPECT_FALSE(WIFI_STORE.saveProvisionedCredential("Ninth", "password-456"));
  EXPECT_EQ(WIFI_STORE.getLastConnectedSsid(), "Network-7");
  EXPECT_EQ(WIFI_STORE.getCredentialCount(), 8u);
  EXPECT_TRUE(WIFI_STORE.saveProvisionedCredential("Network-0", "updated-456"));
  EXPECT_EQ(WIFI_STORE.getCredentialCount(), 8u);
}
