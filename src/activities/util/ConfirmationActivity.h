#pragma once
#include <functional>
#include <string>

#include "activities/Activity.h"
#include "components/OptionPopup.h"

class ConfirmationActivity : public Activity {
 private:
  // Input data
  std::string heading;
  std::string body;

  OptionPopup confirmPopup;
#ifdef ENABLE_DEV_REMOTE_FLASH
  bool developerAccept = false;
#endif

 public:
  ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& heading,
                       const std::string& body);

#ifdef ENABLE_DEV_REMOTE_FLASH
  void acceptAfterRenderForDeveloperTest() { developerAccept = true; }
#endif
  void onEnter() override;
  void loop() override;
  void render(RenderLock&& lock) override;
};