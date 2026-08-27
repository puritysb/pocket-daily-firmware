#include "GamesActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>

#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr char SAVE_DIR[] = "/.crosspoint";
constexpr char SAVE_PATH[] = "/.crosspoint/games.bin";
constexpr char SAVE_TEMP_PATH[] = "/.crosspoint/games.bin.tmp";
constexpr char SAVE_BACKUP_PATH[] = "/.crosspoint/games.bin.bak";
constexpr char SAVE_MAGIC[] = {'C', 'P', 'G', 'M'};
constexpr uint16_t SAVE_VERSION = 1;
constexpr uint8_t MENU_COUNT = 3;
constexpr unsigned long LONG_PRESS_MS = 800;

struct SaveData {
  char magic[4] = {};
  uint16_t version = 0;
  uint16_t byteSize = 0;
  Games::Game2048 game2048;
  Games::LightsOut lightsOut;
  Games::Sokoban sokoban;
  uint32_t checksum = 0;
};

static_assert(std::is_trivially_copyable_v<SaveData>);
static_assert(sizeof(SaveData) < 256);

uint32_t checksum(const SaveData& data) {
  constexpr uint32_t FNV_OFFSET = 2166136261U;
  constexpr uint32_t FNV_PRIME = 16777619U;
  const auto* bytes = reinterpret_cast<const uint8_t*>(&data);
  uint32_t hash = FNV_OFFSET;
  for (size_t i = 0; i < offsetof(SaveData, checksum); ++i) {
    hash ^= bytes[i];
    hash *= FNV_PRIME;
  }
  return hash;
}

uint32_t randomWord() {
  const uint32_t timeBits = static_cast<uint32_t>(micros());
  return timeBits ^ static_cast<uint32_t>(random(0x7FFFFFFF));
}

bool valid2048(const Games::Game2048& game) {
  for (uint16_t value : game.cells) {
    if (value != 0 && ((value & (value - 1U)) != 0 || value > 32768U)) return false;
  }
  return true;
}

bool validLights(const Games::LightsOut& game) {
  if (game.cursor >= Games::LightsOut::CELL_COUNT) return false;
  for (uint8_t value : game.cells) {
    if (value > 1) return false;
  }
  return true;
}

bool validSokoban(const Games::Sokoban& game) {
  if (game.level >= Games::Sokoban::LEVEL_COUNT || game.player >= Games::Sokoban::CELL_COUNT) return false;
  constexpr uint8_t VALID_TILES = Games::Sokoban::Wall | Games::Sokoban::Goal | Games::Sokoban::Box;
  for (uint8_t value : game.cells) {
    if ((value & ~VALID_TILES) != 0) return false;
  }
  return (game.cells[game.player] & Games::Sokoban::Wall) == 0;
}

int centeredX(const GfxRenderer& renderer, int fontId, const char* text, int center) {
  return center - renderer.getTextWidth(fontId, text) / 2;
}
}  // namespace

void GamesActivity::onEnter() {
  Activity::onEnter();
  if (!loadState()) initializeState();
  requestUpdate();
}

void GamesActivity::onExit() {
  saveState();
  Activity::onExit();
}

void GamesActivity::initializeState() {
  game2048.reset(randomWord(), randomWord());
  lightsOut.reset(randomWord());
  sokoban.resetLevel(0);
  dirty = true;
}

bool GamesActivity::loadState() {
  SaveData data{};
  const auto readValidSave = [&data](const char* path) {
    HalFile file;
    if (!Storage.openFileForRead("GAME", path, file) || file.size() != sizeof(SaveData)) return false;
    if (file.read(&data, sizeof(data)) != static_cast<int>(sizeof(data))) return false;
    return memcmp(data.magic, SAVE_MAGIC, sizeof(SAVE_MAGIC)) == 0 && data.version == SAVE_VERSION &&
           data.byteSize == sizeof(SaveData) && data.checksum == checksum(data) && valid2048(data.game2048) &&
           validLights(data.lightsOut) && validSokoban(data.sokoban);
  };

  bool loadedBackup = false;
  if (!readValidSave(SAVE_PATH)) {
    if (!readValidSave(SAVE_BACKUP_PATH)) {
      LOG_DBG("GAME", "No valid game save found");
      return false;
    }
    loadedBackup = true;
  }

  game2048 = data.game2048;
  lightsOut = data.lightsOut;
  sokoban = data.sokoban;
  // A backup is a recovery source, not the steady-state path. Mark it dirty
  // so the next normal exit recreates the primary file.
  dirty = loadedBackup;
  return true;
}

bool GamesActivity::saveState() {
  if (!dirty) return true;
  if (!Storage.exists(SAVE_DIR) && !Storage.mkdir(SAVE_DIR)) {
    LOG_ERR("GAME", "Failed to create save directory");
    return false;
  }

  SaveData data{};
  memcpy(data.magic, SAVE_MAGIC, sizeof(SAVE_MAGIC));
  data.version = SAVE_VERSION;
  data.byteSize = sizeof(SaveData);
  data.game2048 = game2048;
  data.lightsOut = lightsOut;
  data.sokoban = sokoban;
  data.checksum = checksum(data);

  HalFile file;
  if (!Storage.openFileForWrite("GAME", SAVE_TEMP_PATH, file)) return false;
  if (file.write(&data, sizeof(data)) != sizeof(data)) {
    LOG_ERR("GAME", "Failed to write game save");
    file.close();
    Storage.remove(SAVE_TEMP_PATH);
    return false;
  }
  file.flush();
  file.close();  // Required before replacing the destination on the same card.

  const bool hadOriginal = Storage.exists(SAVE_PATH);
  if (hadOriginal) {
    if (Storage.exists(SAVE_BACKUP_PATH)) Storage.remove(SAVE_BACKUP_PATH);
    if (!Storage.rename(SAVE_PATH, SAVE_BACKUP_PATH)) {
      LOG_ERR("GAME", "Failed to rotate game save");
      Storage.remove(SAVE_TEMP_PATH);
      return false;
    }
  }
  if (!Storage.rename(SAVE_TEMP_PATH, SAVE_PATH)) {
    LOG_ERR("GAME", "Failed to commit game save");
    if (hadOriginal) Storage.rename(SAVE_BACKUP_PATH, SAVE_PATH);
    Storage.remove(SAVE_TEMP_PATH);
    return false;
  }
  if (Storage.exists(SAVE_BACKUP_PATH)) Storage.remove(SAVE_BACKUP_PATH);
  dirty = false;
  return true;
}

void GamesActivity::loop() {
  if (screen == Screen::Menu) {
    buttonNavigator.onNext([this] {
      menuIndex = ButtonNavigator::nextIndex(menuIndex, MENU_COUNT);
      requestUpdate();
    });
    buttonNavigator.onPrevious([this] {
      menuIndex = ButtonNavigator::previousIndex(menuIndex, MENU_COUNT);
      requestUpdate();
    });

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      saveState();
      onGoHome(HomeMenuItem::GAMES);
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      openSelectedGame();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    returnToMenu();
    return;
  }

  switch (screen) {
    case Screen::Game2048:
      handle2048();
      break;
    case Screen::LightsOut:
      handleLightsOut();
      break;
    case Screen::Sokoban:
      handleSokoban();
      break;
    case Screen::Menu:
      break;
  }
}

void GamesActivity::openSelectedGame() {
  screen = static_cast<Screen>(menuIndex + 1);
  requestUpdate();
}

void GamesActivity::returnToMenu() {
  saveState();
  screen = Screen::Menu;
  requestUpdate();
}

bool GamesActivity::readDirection(Games::Direction& direction) const {
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    direction = Games::Direction::Up;
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    direction = Games::Direction::Down;
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    direction = Games::Direction::Left;
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    direction = Games::Direction::Right;
    return true;
  }
  return false;
}

void GamesActivity::handle2048() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    game2048.reset(randomWord(), randomWord());
    dirty = true;
    requestUpdate();
    return;
  }

  Games::Direction direction;
  if (readDirection(direction) && game2048.move(direction, randomWord())) {
    dirty = true;
    requestUpdate();
  }
}

void GamesActivity::handleLightsOut() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (lightsOut.solved || mappedInput.getHeldTime() >= LONG_PRESS_MS) {
      lightsOut.reset(randomWord());
    } else {
      lightsOut.press();
    }
    dirty = true;
    requestUpdate();
    return;
  }

  Games::Direction direction;
  if (readDirection(direction)) {
    const uint8_t oldCursor = lightsOut.cursor;
    lightsOut.moveCursor(direction);
    if (lightsOut.cursor != oldCursor) {
      dirty = true;
      requestUpdate();
    }
  }
}

void GamesActivity::handleSokoban() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const uint8_t level = sokoban.solved ? static_cast<uint8_t>(sokoban.level + 1U) : sokoban.level;
    sokoban.resetLevel(level);
    dirty = true;
    requestUpdate();
    return;
  }

  Games::Direction direction;
  if (readDirection(direction) && sokoban.move(direction)) {
    dirty = true;
    requestUpdate();
  }
}

void GamesActivity::render(RenderLock&&) {
  renderer.clearScreen();
  switch (screen) {
    case Screen::Menu:
      renderMenu();
      break;
    case Screen::Game2048:
      render2048();
      break;
    case Screen::LightsOut:
      renderLightsOut();
      break;
    case Screen::Sokoban:
      renderSokoban();
      break;
  }
  renderer.displayBuffer();
}

void GamesActivity::renderMenu() {
  static constexpr StrId LABELS[MENU_COUNT] = {StrId::STR_GAME_2048, StrId::STR_GAME_LIGHTS_OUT,
                                               StrId::STR_GAME_SOKOBAN};
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_GAMES_TITLE));

  constexpr int ITEM_HEIGHT = 76;
  constexpr int ITEM_GAP = 18;
  const int listHeight = MENU_COUNT * ITEM_HEIGHT + (MENU_COUNT - 1) * ITEM_GAP;
  const int listTop =
      metrics.headerHeight + (height - metrics.headerHeight - metrics.buttonHintsHeight - listHeight) / 2;
  const int itemX = 28;
  const int itemWidth = width - itemX * 2;

  for (uint8_t i = 0; i < MENU_COUNT; ++i) {
    const int y = listTop + i * (ITEM_HEIGHT + ITEM_GAP);
    const bool selected = i == menuIndex;
    renderer.fillRoundedRect(itemX, y, itemWidth, ITEM_HEIGHT, 12, selected ? Color::Black : Color::White);
    renderer.drawRoundedRect(itemX, y, itemWidth, ITEM_HEIGHT, 2, 12, true);
    const char* label = I18N.get(LABELS[i]);
    const int textX = centeredX(renderer, UI_12_FONT_ID, label, width / 2);
    const int textY = y + (ITEM_HEIGHT - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
    renderer.drawText(UI_12_FONT_ID, textX, textY, label, !selected, EpdFontFamily::BOLD);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void GamesActivity::drawGameHeader(const char* title, const char* status) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, title, status);
}

void GamesActivity::drawGameHints(const char* confirm) const {
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirm, tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
}

void GamesActivity::render2048() {
  char status[64];
  snprintf(status, sizeof(status), tr(STR_GAME_SCORE_BEST_FORMAT), static_cast<unsigned long>(game2048.score),
           static_cast<unsigned long>(game2048.best));
  drawGameHeader(tr(STR_GAME_2048), status);

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const int availableHeight = height - metrics.headerHeight - metrics.buttonHintsHeight - 80;
  const int boardSize = std::min(width - 40, availableHeight);
  const int gap = 8;
  const int cellSize = (boardSize - gap * (Games::Game2048::SIZE - 1)) / Games::Game2048::SIZE;
  const int actualSize = cellSize * Games::Game2048::SIZE + gap * (Games::Game2048::SIZE - 1);
  const int boardX = (width - actualSize) / 2;
  const int boardY = metrics.headerHeight + 60;

  for (uint8_t row = 0; row < Games::Game2048::SIZE; ++row) {
    for (uint8_t col = 0; col < Games::Game2048::SIZE; ++col) {
      const uint16_t value = game2048.cells[row * Games::Game2048::SIZE + col];
      const int x = boardX + col * (cellSize + gap);
      const int y = boardY + row * (cellSize + gap);
      const bool inverted = value >= 8;
      renderer.fillRoundedRect(x, y, cellSize, cellSize, 8, inverted ? Color::Black : Color::White);
      renderer.drawRoundedRect(x, y, cellSize, cellSize, 2, 8, true);
      if (value != 0) {
        char valueText[8];
        snprintf(valueText, sizeof(valueText), "%u", static_cast<unsigned>(value));
        const int textX = centeredX(renderer, UI_12_FONT_ID, valueText, x + cellSize / 2);
        const int textY = y + (cellSize - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
        renderer.drawText(UI_12_FONT_ID, textX, textY, valueText, !inverted, EpdFontFamily::BOLD);
      }
    }
  }

  if (game2048.over || game2048.won) {
    renderer.drawCenteredText(UI_12_FONT_ID, boardY + actualSize + 18,
                              game2048.over ? tr(STR_GAME_OVER) : tr(STR_GAME_WON), true, EpdFontFamily::BOLD);
  }
  drawGameHints(tr(STR_GAME_NEW));
}

void GamesActivity::renderLightsOut() {
  char status[64];
  snprintf(status, sizeof(status), tr(STR_GAME_LIGHTS_FORMAT), static_cast<unsigned>(lightsOut.moves),
           static_cast<unsigned>(lightsOut.litCount()));
  drawGameHeader(tr(STR_GAME_LIGHTS_OUT), status);

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const int availableHeight = height - metrics.headerHeight - metrics.buttonHintsHeight - 90;
  const int boardSize = std::min(width - 48, availableHeight);
  const int gap = 10;
  const int cellSize = (boardSize - gap * (Games::LightsOut::SIZE - 1)) / Games::LightsOut::SIZE;
  const int actualSize = cellSize * Games::LightsOut::SIZE + gap * (Games::LightsOut::SIZE - 1);
  const int boardX = (width - actualSize) / 2;
  const int boardY = metrics.headerHeight + 64;

  for (uint8_t row = 0; row < Games::LightsOut::SIZE; ++row) {
    for (uint8_t col = 0; col < Games::LightsOut::SIZE; ++col) {
      const uint8_t index = row * Games::LightsOut::SIZE + col;
      const int x = boardX + col * (cellSize + gap);
      const int y = boardY + row * (cellSize + gap);
      const bool lit = lightsOut.cells[index] != 0;
      renderer.fillRoundedRect(x, y, cellSize, cellSize, 9, lit ? Color::Black : Color::White);
      renderer.drawRoundedRect(x, y, cellSize, cellSize, index == lightsOut.cursor ? 4 : 2, 9, true);
      if (index == lightsOut.cursor) {
        renderer.drawRect(x + 7, y + 7, cellSize - 14, cellSize - 14, 2, !lit);
      }
    }
  }

  if (lightsOut.solved) {
    renderer.drawCenteredText(UI_12_FONT_ID, boardY + actualSize + 18, tr(STR_GAME_SOLVED), true, EpdFontFamily::BOLD);
  }
  drawGameHints(lightsOut.solved ? tr(STR_GAME_NEW) : tr(STR_GAME_TOGGLE_NEW));
}

void GamesActivity::renderSokoban() {
  char status[64];
  snprintf(status, sizeof(status), tr(STR_GAME_LEVEL_MOVES_FORMAT), static_cast<unsigned>(sokoban.level + 1U),
           static_cast<unsigned>(sokoban.moves));
  drawGameHeader(tr(STR_GAME_SOKOBAN), status);

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const int availableHeight = height - metrics.headerHeight - metrics.buttonHintsHeight - 80;
  const int boardSize = std::min(width - 24, availableHeight);
  const int cellSize = boardSize / Games::Sokoban::WIDTH;
  const int actualSize = cellSize * Games::Sokoban::WIDTH;
  const int boardX = (width - actualSize) / 2;
  const int boardY = metrics.headerHeight + 54;

  for (uint8_t row = 0; row < Games::Sokoban::HEIGHT; ++row) {
    for (uint8_t col = 0; col < Games::Sokoban::WIDTH; ++col) {
      const uint8_t index = row * Games::Sokoban::WIDTH + col;
      const uint8_t tile = sokoban.cells[index];
      const int x = boardX + col * cellSize;
      const int y = boardY + row * cellSize;
      renderer.drawRect(x, y, cellSize, cellSize);

      if ((tile & Games::Sokoban::Wall) != 0) {
        renderer.fillRectDither(x + 1, y + 1, cellSize - 2, cellSize - 2, Color::DarkGray);
      } else {
        if ((tile & Games::Sokoban::Goal) != 0) {
          const int inset = std::max(5, cellSize / 3);
          renderer.drawRect(x + inset, y + inset, cellSize - inset * 2, cellSize - inset * 2, 2, true);
        }
        if ((tile & Games::Sokoban::Box) != 0) {
          const int inset = 7;
          const bool onGoal = (tile & Games::Sokoban::Goal) != 0;
          renderer.fillRect(x + inset, y + inset, cellSize - inset * 2, cellSize - inset * 2, onGoal);
          renderer.drawRect(x + inset, y + inset, cellSize - inset * 2, cellSize - inset * 2, 3, true);
          renderer.drawLine(x + inset, y + inset, x + cellSize - inset - 1, y + cellSize - inset - 1, !onGoal);
          renderer.drawLine(x + cellSize - inset - 1, y + inset, x + inset, y + cellSize - inset - 1, !onGoal);
        }
      }

      if (index == sokoban.player) {
        const int inset = std::max(8, cellSize / 3);
        renderer.fillRoundedRect(x + inset, y + inset, cellSize - inset * 2, cellSize - inset * 2, 4, Color::Black);
      }
    }
  }

  if (sokoban.solved) {
    renderer.drawCenteredText(UI_12_FONT_ID, boardY + actualSize + 14, tr(STR_GAME_SOLVED), true, EpdFontFamily::BOLD);
  }
  drawGameHints(sokoban.solved ? tr(STR_GAME_NEXT) : tr(STR_GAME_RESTART));
}
