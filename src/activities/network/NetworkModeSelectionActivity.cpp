#include "NetworkModeSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MENU_ITEM_COUNT = 3;
// Pocket Daily Sync menu: the two companion transports, then the release
// update check (the only Pocket Daily route to it outside Settings).
constexpr StrId POCKET_ITEMS[MENU_ITEM_COUNT] = {StrId::STR_POCKET_WIFI, StrId::STR_POCKET_DIRECT,
                                                 StrId::STR_CHECK_UPDATES};
constexpr StrId POCKET_DESCS[MENU_ITEM_COUNT] = {StrId::STR_POCKET_WIFI_DESC, StrId::STR_POCKET_DIRECT_DESC,
                                                 StrId::STR_POCKET_UPDATE_DESC};
constexpr NetworkMode POCKET_MODES[MENU_ITEM_COUNT] = {NetworkMode::JOIN_NETWORK, NetworkMode::CREATE_HOTSPOT,
                                                       NetworkMode::CHECK_FOR_UPDATES};
constexpr NetworkMode TRANSFER_MODES[MENU_ITEM_COUNT] = {NetworkMode::JOIN_NETWORK, NetworkMode::CONNECT_CALIBRE,
                                                         NetworkMode::CREATE_HOTSPOT};
}  // namespace

void NetworkModeSelectionActivity::onEnter() {
  Activity::onEnter();

  // Reset selection
  selectedIndex = 0;

  // Trigger first update
  requestUpdate();
}

void NetworkModeSelectionActivity::onExit() { Activity::onExit(); }

void NetworkModeSelectionActivity::loop() {
  // Handle back button - cancel
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onCancel();
    return;
  }

  // Handle confirm button - select current option
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    const int index = selectedIndex >= 0 && selectedIndex < MENU_ITEM_COUNT ? selectedIndex : 0;
    onModeSelected(pocketSync ? POCKET_MODES[index] : TRANSFER_MODES[index]);
    return;
  }

  // Handle navigation
  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, itemCount());
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, itemCount());
    requestUpdate();
  });
}

void NetworkModeSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 pocketSync ? tr(STR_POCKET_CONNECT_TITLE) : tr(STR_FILE_TRANSFER));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  // Menu items and descriptions
  static constexpr StrId menuItems[MENU_ITEM_COUNT] = {StrId::STR_JOIN_NETWORK, StrId::STR_CALIBRE_WIRELESS,
                                                       StrId::STR_CREATE_HOTSPOT};
  static constexpr StrId menuDescs[MENU_ITEM_COUNT] = {StrId::STR_JOIN_DESC, StrId::STR_CALIBRE_DESC,
                                                       StrId::STR_HOTSPOT_DESC};
  static constexpr UIIcon menuIcons[MENU_ITEM_COUNT] = {UIIcon::Wifi, UIIcon::Library, UIIcon::Hotspot};
  static constexpr UIIcon pocketIcons[MENU_ITEM_COUNT] = {UIIcon::Wifi, UIIcon::Hotspot, UIIcon::Settings};

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, itemCount(), selectedIndex,
      [this](int index) { return std::string(I18N.get(pocketSync ? POCKET_ITEMS[index] : menuItems[index])); },
      [this](int index) { return std::string(I18N.get(pocketSync ? POCKET_DESCS[index] : menuDescs[index])); },
      [this](int index) { return pocketSync ? pocketIcons[index] : menuIcons[index]; });

  // Draw help text at bottom
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

void NetworkModeSelectionActivity::onModeSelected(NetworkMode mode) {
  setResult(NetworkModeResult{mode});
  finish();
}

void NetworkModeSelectionActivity::onCancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}
