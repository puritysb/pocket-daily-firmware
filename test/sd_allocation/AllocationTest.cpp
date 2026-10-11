#include <SDCardManager.h>
#include <gtest/gtest.h>

// Compile the real SDK header with the X3 SPI configuration. USB raw access is
// intentionally unavailable: the previous implementation incorrectly required it.
SDCardManager::SDCardManager() : initialized(AllocationFake::mounted) {}
FsBlockDeviceInterface* SDCardManager::rawBlockDevice() { return nullptr; }

TEST(SDAllocation, MountedSPIReadsWithoutUSBBlockInterface) {
  AllocationFake::mounted = true;
  AllocationFake::present = true;
  AllocationFake::succeeds = true;
  AllocationFake::calls = 0;
  SDCardManager manager;
  uint8_t bytes[1]{};
  ASSERT_TRUE(manager.readAllocationSector(321, bytes));
  EXPECT_EQ(AllocationFake::sector, 321u);
  EXPECT_EQ(AllocationFake::calls, 1u);
  EXPECT_EQ(bytes[0], 73);
  AllocationFake::succeeds = false;
  EXPECT_FALSE(manager.readAllocationSector(322, bytes));
  AllocationFake::present = false;
  EXPECT_FALSE(manager.readAllocationSector(323, bytes));
  EXPECT_EQ(AllocationFake::calls, 2u);
}

TEST(SDAllocation, UnmountedDoesNotReadCard) {
  AllocationFake::mounted = false;
  AllocationFake::present = true;
  AllocationFake::succeeds = true;
  AllocationFake::calls = 0;
  SDCardManager manager;
  uint8_t bytes[1]{};
  EXPECT_FALSE(manager.readAllocationSector(0, bytes));
  EXPECT_EQ(AllocationFake::calls, 0u);
}
