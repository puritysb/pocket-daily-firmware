#include <gtest/gtest.h>

#include <algorithm>

#include "src/games/GameModels.h"

namespace {
void clear2048(Games::Game2048& game) {
  std::fill(std::begin(game.cells), std::end(game.cells), 0);
  game.score = 0;
  game.won = false;
  game.over = false;
}
}  // namespace

TEST(Game2048, MergesEachTileOnlyOnce) {
  Games::Game2048 game;
  clear2048(game);
  game.cells[0] = 2;
  game.cells[1] = 2;
  game.cells[2] = 2;
  game.cells[3] = 2;

  ASSERT_TRUE(game.move(Games::Direction::Left, 7));
  EXPECT_EQ(game.cells[0], 4);
  EXPECT_EQ(game.cells[1], 4);
  EXPECT_EQ(game.score, 8U);
}

TEST(Game2048, DoesNotMergeAResultTwiceInOneMove) {
  Games::Game2048 game;
  clear2048(game);
  game.cells[0] = 2;
  game.cells[1] = 2;
  game.cells[2] = 4;

  ASSERT_TRUE(game.move(Games::Direction::Left, 9));
  EXPECT_EQ(game.cells[0], 4);
  EXPECT_EQ(game.cells[1], 4);
  EXPECT_EQ(game.score, 4U);
}

TEST(Game2048, DetectsAFullBoardWithNoLegalMove) {
  Games::Game2048 game;
  clear2048(game);
  constexpr uint16_t FULL[Games::Game2048::CELL_COUNT] = {
      2, 4, 2, 4, 4, 2, 4, 2, 2, 4, 2, 4, 4, 2, 4, 2,
  };
  std::copy(std::begin(FULL), std::end(FULL), std::begin(game.cells));

  EXPECT_FALSE(game.move(Games::Direction::Left, 0));
  EXPECT_TRUE(game.over);
  EXPECT_FALSE(game.canMove());
}

TEST(LightsOut, PressTogglesTheSelectedCross) {
  Games::LightsOut game;
  std::fill(std::begin(game.cells), std::end(game.cells), 0);
  game.cursor = 12;

  game.press();

  EXPECT_EQ(game.litCount(), 5);
  EXPECT_EQ(game.cells[12], 1);
  EXPECT_EQ(game.cells[7], 1);
  EXPECT_EQ(game.cells[11], 1);
  EXPECT_EQ(game.cells[13], 1);
  EXPECT_EQ(game.cells[17], 1);
}

TEST(LightsOut, ClearingTheFinalCrossSolvesThePuzzle) {
  Games::LightsOut game;
  std::fill(std::begin(game.cells), std::end(game.cells), 0);
  game.cursor = 0;
  game.cells[0] = 1;
  game.cells[1] = 1;
  game.cells[5] = 1;

  game.press();

  EXPECT_TRUE(game.solved);
  EXPECT_EQ(game.litCount(), 0);
}

TEST(Sokoban, PushesABoxOntoAGoal) {
  Games::Sokoban game;
  game.resetLevel(0);

  ASSERT_TRUE(game.move(Games::Direction::Up));

  EXPECT_TRUE(game.solved);
  EXPECT_EQ(game.player, 27);
  EXPECT_EQ(game.cells[19] & (Games::Sokoban::Box | Games::Sokoban::Goal), Games::Sokoban::Box | Games::Sokoban::Goal);
}

TEST(Sokoban, RejectsMovementIntoAWall) {
  Games::Sokoban game;
  game.resetLevel(0);
  game.player = 9;

  EXPECT_FALSE(game.move(Games::Direction::Left));
  EXPECT_EQ(game.player, 9);
  EXPECT_EQ(game.moves, 0);
}
