#include <HardwareSerial.h>
#include <gtest/gtest.h>

HardwareSerial serialInstance;
HardwareSerial& Serial = serialInstance;

#include <Logging.h>

class LoggingTest : public testing::Test {
 protected:
  void SetUp() override {
    serialInstance = HardwareSerial{};
    clearLastLogs();
  }
};

TEST_F(LoggingTest, FullOrDisconnectedOutputDoesNotCallTheBlockingWriter) {
  serialInstance.capacity = 0;
  for (int i = 0; i < 10000; ++i) logPrintf("DBG", "TEST", "glyph %d\n", i);
  EXPECT_EQ(serialInstance.writes, 0);
  EXPECT_NE(getLastLogs().find("glyph 9999"), std::string::npos);
  serialInstance.capacity = 256;
  serialInstance.connected = false;
  logPrintf("INF", "TEST", "disconnected\n");
  EXPECT_EQ(serialInstance.writes, 0);
  EXPECT_NE(getLastLogs().find("disconnected"), std::string::npos);
}

TEST_F(LoggingTest, CapacityMustFitTheWholeRecordAndCanRecover) {
  const std::string expected = "[123] [INF] [TEST] ready\n";
  serialInstance.capacity = static_cast<int>(expected.size()) - 1;
  logPrintf("INF", "TEST", "ready\n");
  EXPECT_EQ(serialInstance.writes, 0);
  serialInstance.capacity = static_cast<int>(expected.size());
  logPrintf("INF", "TEST", "ready\n");
  EXPECT_EQ(serialInstance.writes, 1);
  EXPECT_EQ(serialInstance.output, expected);
}

TEST_F(LoggingTest, ReentrantOutputIsDroppedWithoutLosingTheInternalRecord) {
  serialInstance.duringWrite = [] { logPrintf("DBG", "NESTED", "nested\n"); };
  logPrintf("INF", "TEST", "outer\n");
  EXPECT_EQ(serialInstance.writes, 1);
  EXPECT_EQ(serialInstance.output, "[123] [INF] [TEST] outer\n");
  EXPECT_NE(getLastLogs().find("nested"), std::string::npos);
  EXPECT_NE(getLastLogs().find("outer"), std::string::npos);
  serialInstance.duringWrite = {};
  logPrintf("INF", "TEST", "next\n");
  EXPECT_EQ(serialInstance.writes, 2);
}

TEST_F(LoggingTest, TruncatedRecordsNeverExceedTheExistingBound) {
  std::string longText(1000, 'a');
  logPrintf("INF", "TEST", "%s", longText.c_str());
  EXPECT_EQ(serialInstance.writes, 1);
  EXPECT_EQ(serialInstance.output.size(), 255u);
}
