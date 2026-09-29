#include "NetworkModeSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

namespace {
constexpr StrId POCKET_ITEMS[] = {StrId::STR_POCKET_WIFI, StrId::STR_POCKET_DIRECT, StrId::STR_CHECK_UPDATES};
constexpr StrId POCKET_DESCS[] = {StrId::STR_POCKET_WIFI_DESC, StrId::STR_POCKET_DIRECT_DESC,
                                  StrId::STR_POCKET_UPDATE_DESC};
constexpr UIIcon POCKET_ICONS[] = {UIIcon::Wifi, UIIcon::Hotspot, UIIcon::Settings};
constexpr NetworkMode POCKET_MODES[] = {NetworkMode::JOIN_NETWORK, NetworkMode::CREATE_HOTSPOT,
                                        NetworkMode::CHECK_FOR_UPDATES};
constexpr NetworkMode TRANSFER_MODES[] = {NetworkMode::JOIN_NETWORK, NetworkMode::CONNECT_CALIBRE,
                                          NetworkMode::CREATE_HOTSPOT, NetworkMode::USB_DRIVE};
constexpr StrId menuItems[NetworkModeSelectionActivity::MENU_ITEM_COUNT] = {
    StrId::STR_JOIN_NETWORK,
    StrId::STR_CALIBRE_WIRELESS,
    StrId::STR_CREATE_HOTSPOT,
#if FREEINK_CAP_USB_MSC
    StrId::STR_USB_DRIVE,
#endif
};
constexpr StrId menuDescs[NetworkModeSelectionActivity::MENU_ITEM_COUNT] = {
    StrId::STR_JOIN_DESC,
    StrId::STR_CALIBRE_DESC,
    StrId::STR_HOTSPOT_DESC,
#if FREEINK_CAP_USB_MSC
    StrId::STR_USB_DRIVE_DESC,
#endif
};
constexpr UIIcon menuIcons[NetworkModeSelectionActivity::MENU_ITEM_COUNT] = {
    UIIcon::Wifi,
    UIIcon::Library,
    UIIcon::Hotspot,
#if FREEINK_CAP_USB_MSC
    UIIcon::Usb,
#endif
};
}  // namespace

NetworkModeSelectionActivity::NetworkModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                           bool pocketSync)
    : UiListActivity("NetworkModeSelection", renderer, mappedInput), pocketSync(pocketSync) {
  // Entirely static, so built once here rather than every buildScreen() call.
  for (int i = 0; i < listCount(); i++) {
    fui::ListItem item;
    item.label = I18N.get(pocketSync ? POCKET_ITEMS[i] : menuItems[i]);
    item.subtitle = I18N.get(pocketSync ? POCKET_DESCS[i] : menuDescs[i]);
    item.icon = listIconFor(pocketSync ? POCKET_ICONS[i] : menuIcons[i], 32);  // subtitle rows carry the larger icon
    item.actionValue = static_cast<int16_t>(i);
    rowItems_[i] = item;
  }
}

int NetworkModeSelectionActivity::listCount() const {
#if FREEINK_CAP_USB_MSC
  return pocketSync ? 3 : MENU_ITEM_COUNT;
#else
  return MENU_ITEM_COUNT;
#endif
}

const char* NetworkModeSelectionActivity::headerTitle() const {
  return pocketSync ? tr(STR_POCKET_CONNECT_TITLE) : tr(STR_FILE_TRANSFER);
}

void NetworkModeSelectionActivity::activateIndex(const int index) {
  // Selection leaves this screen; a lingering flash would gray an unrelated
  // element on the next render.
  if (index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  nav.selected = index;

  onModeSelected(pocketSync ? POCKET_MODES[index] : TRANSFER_MODES[index]);
}

void NetworkModeSelectionActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // rowItems_ was built once in the constructor and is reused here on every
  // repaint.
  fui::ListProps props;
  props.items = rowItems_;
  props.count = static_cast<uint16_t>(listCount());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
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
