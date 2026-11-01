#pragma once
#include <atomic>
#include <memory>
#include <string>

#include "pocket_daily/ReaderNavigation.h"
struct Activity {
  virtual ~Activity() = default;
  virtual void onEnter() {}
};
class ActivityManager {
 public:
  int renderer = 0, mappedInput = 0;
  std::atomic<ReaderReturn> readerReturn{ReaderReturn::Home}, shell{ReaderReturn::Home};
  std::unique_ptr<Activity> currentActivity, pendingActivity;
  bool browserOpened = false;
  void replaceActivity(std::unique_ptr<Activity>&& a) {
    if (currentActivity)
      pendingActivity = std::move(a);
    else {
      currentActivity = std::move(a);
      currentActivity->onEnter();
    }
  }
  void settle() {
    if (pendingActivity) {
      currentActivity = std::move(pendingActivity);
      currentActivity->onEnter();
    }
  }
  void goToFileBrowser(std::string) { browserOpened = true; }
  const char* readerReturnLabel() const;
  const char* shellReturnLabel() const;
  void launchReader(ReaderReturn, std::string, bool);
  void goToReader(std::string, bool = false);
  void goToReaderFrom(ReaderReturn, std::string, bool = false);
  void resumeReader(std::string);
};
