#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "util/ScreenshotInfo.h"

class Activity;    // forward declaration
class RenderLock;  // forward declaration

enum class HomeMenuItem { NONE, FILE_BROWSER, LIBRARY, OPDS_BROWSER, FILE_TRANSFER, POCKET_DAILY, SETTINGS_MENU };

/**
 * ActivityManager
 *
 * This mirrors the same concept of Activity in Android, where an activity represents a single screen of the UI. The
 * manager is responsible for launching activities, and ensuring that only one activity is active at a time.
 *
 * It also provides a stack mechanism to allow activities to launch sub-activities and get back the results when the
 * sub-activity is done. For example, the WebServer activity can launch a WifiSelect activity to let the user choose a
 * wifi network, and get back the selected network when the user is done.
 *
 * Main differences from Android's ActivityManager:
 * - No onPause/onResume, since we don't have a concept of background activities
 * - onActivityResult is implemented via a callback instead of a separate method, for simplicity
 */
// Where leaving a book goes. Stock shells return Home; Pocket Daily and its
// Articles list return to themselves.
enum class ReaderReturn : uint8_t { Home, PocketDaily, Articles };

class ActivityManager {
  friend class RenderLock;

 protected:
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;
  std::vector<std::unique_ptr<Activity>> stackActivities;
  std::unique_ptr<Activity> currentActivity;

  void exitActivity(const RenderLock& lock);
  static void closeExchangeWindowFor(const Activity& next);
  void armExchangeWindowAfterBook();

  // Pending activity to be launched on next loop iteration
  std::unique_ptr<Activity> pendingActivity;
  enum class PendingAction { None, Push, Pop, Replace };
  PendingAction pendingAction = PendingAction::None;

  // Product shell that opened the current book; leaving the book returns there.
  ReaderReturn readerReturn = ReaderReturn::Home;

  // Task to render and display the activity
  TaskHandle_t renderTaskHandle = nullptr;
  static void renderTaskTrampoline(void* param);
  [[noreturn]] virtual void renderTaskLoop();

  // Set by requestUpdateAndWait(); read and cleared by the render task after render completes.
  // Note: only one waiting task is supported at a time
  TaskHandle_t waitingTaskHandle = nullptr;

  // Mutex to protect rendering operations from race conditions
  // Must only be used via RenderLock
  SemaphoreHandle_t renderingMutex = nullptr;

  // Frames the render task completed (Pocket Reading Sync waits for the shell's
  // next frame before raising the radio).
  std::atomic<uint32_t> completedRenders{0};

  // Whether to trigger a render after the current loop()
  // This variable must only be set by the main loop, to avoid race conditions
  std::atomic<bool> requestedUpdate{false};

 public:
  explicit ActivityManager(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : renderer(renderer), mappedInput(mappedInput), renderingMutex(xSemaphoreCreateMutex()) {
    assert(renderingMutex != nullptr && "Failed to create rendering mutex");
    stackActivities.reserve(10);
  }
  ~ActivityManager() { assert(false); /* should never be called */ };

  void begin();
  void loop();

  // Will replace currentActivity and drop all activities on stack
  void replaceActivity(std::unique_ptr<Activity>&& newActivity);

  // goTo... functions are convenient wrapper for replaceActivity()
  void goToFileTransfer(bool autoJoinSavedNetwork = false);
  bool goToPocketDaily();
  void goToPocketNearbySync(bool autoJoinSavedNetwork = false);
  void goToSettings();
  void goToFileBrowser(std::string path = {});
  bool goToArticles();
  void goToUsbDrive();
  void goToLibrary();
  void goToBrowser();
  void goToReader(std::string path, bool allowFastInitialRefresh = false);
  // Open a book from a product shell so that leaveReader() returns to it.
  void goToReaderFrom(ReaderReturn origin, std::string path, bool allowFastInitialRefresh = false);
  // Reopen a book from inside the reader flow (after KOReader sync, the next
  // book from the end-of-book menu): the origin of the current book is kept.
  void resumeReader(std::string path);
  // Leave the book by Back or the end-of-book exit: the opening shell, else Home.
  void leaveReader();
  void goToSleep(bool fromTimeout = false);
  void goToBoot();
  void goToFullScreenMessage(std::string message, EpdFontFamily::Style style = EpdFontFamily::REGULAR);
  void goToCrashReport();
  void goHome(HomeMenuItem initialMenuItem = HomeMenuItem::NONE, bool cleanInitialRefresh = false);

  // This will move current activity to stack instead of deleting it
  void pushActivity(std::unique_ptr<Activity>&& activity);

  // Remove the currentActivity, returning the last one on stack
  // Note: if popActivity() on last activity on the stack, we will goHome()
  void popActivity();

  bool preventAutoSleep() const;
  // Let the current activity own the retained sleep frame (Activity::paintSleepFrame).
  bool paintSleepFrame();
  bool requiresExclusiveStorageLoop() const;
  bool isReaderActivity() const;
  bool handleForcedRefresh();
  // The current screen tolerates a Reading Sync exchange window and no book is
  // open anywhere on the stack.
  bool allowsExchangeWindow() const;
  uint32_t renderCount() const { return completedRenders.load(std::memory_order_acquire); }
  bool skipLoopDelay() const;
  ScreenshotInfo getScreenshotInfo() const;

  // If immediate is true, the update will be triggered immediately.
  // Otherwise, it will be deferred until the end of the current loop iteration.
  void requestUpdate(bool immediate = false);

  // Trigger a render and block until it completes.
  // Must NOT be called from the render task or while holding a RenderLock.
  void requestUpdateAndWait();
};

extern ActivityManager activityManager;  // singleton, to be defined in main.cpp
