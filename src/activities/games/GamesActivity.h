#pragma once

#include <cstdint>

#include "activities/Activity.h"
#include "games/GameModels.h"
#include "util/ButtonNavigator.h"

class GamesActivity final : public Activity {
 public:
  explicit GamesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Games", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Screen : uint8_t { Menu, Game2048, LightsOut, Sokoban };

  Screen screen = Screen::Menu;
  uint8_t menuIndex = 0;
  bool dirty = false;
  ButtonNavigator buttonNavigator;
  Games::Game2048 game2048;
  Games::LightsOut lightsOut;
  Games::Sokoban sokoban;

  void openSelectedGame();
  void returnToMenu();
  bool readDirection(Games::Direction& direction) const;
  void handle2048();
  void handleLightsOut();
  void handleSokoban();

  void renderMenu();
  void render2048();
  void renderLightsOut();
  void renderSokoban();
  void drawGameHeader(const char* title, const char* status) const;
  void drawGameHints(const char* confirm) const;

  bool loadState();
  bool saveState();
  void initializeState();
};
