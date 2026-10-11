#pragma once
#include "WifiSetupTicket.h"
namespace Pocket::NearbySync::WifiSetup {
void beginBoot(bool softwareRestart);
bool wantsJoin();
bool wantsBluetooth();
const Ticket& current();
bool receive(const ParsedCommand& command);
void cancel();
void prepareBluetooth();
bool restartDue();
void complete(Result result);
const char* resultName();
}  // namespace Pocket::NearbySync::WifiSetup
