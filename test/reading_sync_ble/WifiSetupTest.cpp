#include <gtest/gtest.h>

#include "pocket_daily/nearby_sync/WifiSetupTicket.h"

using namespace Pocket::NearbySync;
namespace Setup = Pocket::NearbySync::WifiSetup;
namespace {
bool stage(Setup::Ticket& ticket, const std::string& text) {
  ParsedCommand command;
  return parseCommand(text.data(), text.size(), command) == ParseResult::OK && Setup::stage(ticket, command);
}
constexpr const char* JOIN = "WIFI_JOIN 01ABCDEF 486F6D652057694669 6162636431323334";
}  // namespace

TEST(WifiSetup, DecodesExactSpacesAndCredentialsWithoutTrimming) {
  Setup::Ticket ticket;
  ASSERT_TRUE(stage(ticket, JOIN));
  EXPECT_STREQ(ticket.ssid, "Home WiFi");
  EXPECT_STREQ(ticket.password, "abcd1234");
  EXPECT_STREQ(ticket.requestId, "01ABCDEF");
  EXPECT_EQ(ticket.result, Setup::Result::Pending);
  EXPECT_TRUE(Setup::valid(ticket));
  EXPECT_EQ(Setup::boot(ticket, true), Setup::Route::Join);
  EXPECT_EQ(Setup::boot(ticket, true), Setup::Route::None);
}

TEST(WifiSetup, RejectsMalformedBoundariesAndControlCharacters) {
  for (const auto& body : {"", "48", " 48 -", "48 ", "48 --", "48 31", "00 -", "0A -", "7F -", "4G -", "4a -",
                           "48 6162636431323300", "48 61626364313233FF", "48 - extra"}) {
    Setup::Ticket ticket;
    EXPECT_FALSE(stage(ticket, std::string("WIFI_JOIN 01ABCDEF ") + body)) << body;
  }
  Setup::Ticket ticket;
  EXPECT_FALSE(stage(ticket, "WIFI_JOIN 01abcDEF 48 -"));
  EXPECT_FALSE(stage(ticket, "WIFI_JOIN 01ABCDEF " + std::string(66, '4') + " -"));
  EXPECT_FALSE(stage(ticket, "WIFI_JOIN 01ABCDEF 48 " + std::string(130, '4')));
  ASSERT_TRUE(stage(ticket, "WIFI_JOIN 01ABCDEF " + std::string(64, '4') + " " + std::string(128, '3')));
  EXPECT_EQ(strlen(ticket.ssid), 32u);
  EXPECT_EQ(strlen(ticket.password), 64u);
  EXPECT_FALSE(stage(ticket, "WIFI_JOIN 01ABCDEF 48 " + std::string(128, '7')));
}

TEST(WifiSetup, OpenNetworkIsExplicitAndUnicodeNameRoundTrips) {
  Setup::Ticket ticket;
  ASSERT_TRUE(stage(ticket, "WIFI_JOIN 01ABCDEF EAB08020EB8298 -"));
  EXPECT_STREQ(ticket.ssid, "가 나");
  EXPECT_STREQ(ticket.password, "");
}

TEST(WifiSetup, CorruptionAndUnintentionalBootEraseSecrets) {
  for (bool softwareRestart : {false, true}) {
    Setup::Ticket ticket;
    ASSERT_TRUE(stage(ticket, JOIN));
    if (softwareRestart) ticket.password[2] = 'z';
    EXPECT_EQ(Setup::boot(ticket, softwareRestart), Setup::Route::None);
    EXPECT_EQ(ticket.magic, 0u);
    EXPECT_EQ(ticket.password[0], 0);
    EXPECT_EQ(ticket.ssid[0], 0);
  }
}

TEST(WifiSetup, EveryTerminalResultErasesCredentialsAndKeepsReceipt) {
  for (auto result : {Setup::Result::Saved, Setup::Result::Failed, Setup::Result::SaveFailed}) {
    Setup::Ticket ticket;
    ASSERT_TRUE(stage(ticket, JOIN));
    Setup::finish(ticket, result);
    for (char c : ticket.password) EXPECT_EQ(c, 0);
    for (char c : ticket.ssid) EXPECT_EQ(c, 0);
    EXPECT_STREQ(ticket.requestId, "01ABCDEF");
    EXPECT_EQ(ticket.result, result);
    EXPECT_TRUE(Setup::valid(ticket));
    EXPECT_EQ(Setup::boot(ticket, true), Setup::Route::None);
  }
}

TEST(WifiSetup, CapabilityOnlyAppearsInUsableBluetoothModes) {
  char text[221];
  ASSERT_GT(formatStatus("X3", "01ABCDEF", "dev", false, text, sizeof(text)), 0u);
  EXPECT_NE(strstr(text, "WIFI1"), nullptr);
  ASSERT_GT(formatStatus("X3", "01ABCDEF", "dev", true, text, sizeof(text)), 0u);
  EXPECT_EQ(strstr(text, "WIFI1"), nullptr);
  ASSERT_GT(formatStatus("X3", "01ABCDEF", "dev", true, text, sizeof(text), true), 0u);
  EXPECT_NE(strstr(text, "WIFI1"), nullptr);
  EXPECT_FALSE(allowedInWindow(Verb::WIFI_JOIN));
}
