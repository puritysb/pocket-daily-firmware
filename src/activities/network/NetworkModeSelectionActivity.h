#pragma once

#include <functional>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// CHECK_FOR_UPDATES is offered only by the Pocket Daily Sync menu.
enum class NetworkMode { JOIN_NETWORK, CONNECT_CALIBRE, CREATE_HOTSPOT, CHECK_FOR_UPDATES };

/**
 * NetworkModeSelectionActivity presents the user with a choice:
 * - "Join a Network" - Connect to an existing WiFi network (STA mode)
 * - "Connect to Calibre" - Use Calibre wireless device transfers
 * - "Create Hotspot" - Create an Access Point that others can connect to (AP mode)
 *
 * The Pocket Daily Sync variant offers "Same Wi-Fi", "Direct connection" and
 * "Check for updates" (the release OTA check) instead.
 *
 * The onModeSelected callback is called with the user's choice.
 * The onCancel callback is called if the user presses back.
 */
class NetworkModeSelectionActivity final : public Activity {
  ButtonNavigator buttonNavigator;

  int selectedIndex = 0;
  const bool pocketSync;
  static constexpr int itemCount() { return 3; }

 public:
  explicit NetworkModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool pocketSync = false)
      : Activity("NetworkModeSelection", renderer, mappedInput), pocketSync(pocketSync) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  void onModeSelected(NetworkMode mode);
  void onCancel();
};
