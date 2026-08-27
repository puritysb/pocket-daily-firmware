#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace Games {

enum class Direction : uint8_t { Up, Down, Left, Right };

struct Game2048 {
  static constexpr uint8_t SIZE = 4;
  static constexpr uint8_t CELL_COUNT = SIZE * SIZE;

  uint16_t cells[CELL_COUNT] = {};
  uint32_t score = 0;
  uint32_t best = 0;
  bool won = false;
  bool over = false;

  void reset(uint32_t firstRandom, uint32_t secondRandom) {
    const uint32_t savedBest = best;
    *this = {};
    best = savedBest;
    addTile(firstRandom);
    addTile(secondRandom);
  }

  bool move(Direction direction, uint32_t spawnRandom) {
    bool changed = false;

    for (uint8_t outer = 0; outer < SIZE; ++outer) {
      uint16_t packed[SIZE] = {};
      uint8_t packedCount = 0;
      for (uint8_t inner = 0; inner < SIZE; ++inner) {
        const uint16_t value = cells[indexFor(direction, outer, inner)];
        if (value != 0) packed[packedCount++] = value;
      }

      uint16_t merged[SIZE] = {};
      uint8_t mergedCount = 0;
      for (uint8_t i = 0; i < packedCount; ++i) {
        if (i + 1 < packedCount && packed[i] == packed[i + 1]) {
          const uint16_t value = static_cast<uint16_t>(packed[i] * 2U);
          merged[mergedCount++] = value;
          score += value;
          if (value >= 2048) won = true;
          ++i;
        } else {
          merged[mergedCount++] = packed[i];
        }
      }

      for (uint8_t inner = 0; inner < SIZE; ++inner) {
        const uint8_t index = indexFor(direction, outer, inner);
        const uint16_t value = inner < mergedCount ? merged[inner] : 0;
        if (cells[index] != value) {
          cells[index] = value;
          changed = true;
        }
      }
    }

    if (changed) addTile(spawnRandom);
    best = std::max(best, score);
    over = !canMove();
    return changed;
  }

  bool canMove() const {
    for (uint8_t row = 0; row < SIZE; ++row) {
      for (uint8_t col = 0; col < SIZE; ++col) {
        const uint16_t value = cells[row * SIZE + col];
        if (value == 0) return true;
        if (col + 1 < SIZE && value == cells[row * SIZE + col + 1]) return true;
        if (row + 1 < SIZE && value == cells[(row + 1) * SIZE + col]) return true;
      }
    }
    return false;
  }

 private:
  static uint8_t indexFor(Direction direction, uint8_t outer, uint8_t inner) {
    switch (direction) {
      case Direction::Left:
        return static_cast<uint8_t>(outer * SIZE + inner);
      case Direction::Right:
        return static_cast<uint8_t>(outer * SIZE + (SIZE - 1 - inner));
      case Direction::Up:
        return static_cast<uint8_t>(inner * SIZE + outer);
      case Direction::Down:
        return static_cast<uint8_t>((SIZE - 1 - inner) * SIZE + outer);
    }
    return 0;
  }

  void addTile(uint32_t randomValue) {
    uint8_t empty[CELL_COUNT] = {};
    uint8_t emptyCount = 0;
    for (uint8_t i = 0; i < CELL_COUNT; ++i) {
      if (cells[i] == 0) empty[emptyCount++] = i;
    }
    if (emptyCount == 0) return;

    const uint8_t slot = static_cast<uint8_t>(randomValue % emptyCount);
    randomValue /= emptyCount;
    cells[empty[slot]] = (randomValue % 10U == 0U) ? 4 : 2;
  }
};

struct LightsOut {
  static constexpr uint8_t SIZE = 5;
  static constexpr uint8_t CELL_COUNT = SIZE * SIZE;

  uint8_t cells[CELL_COUNT] = {};
  uint8_t cursor = CELL_COUNT / 2;
  uint16_t moves = 0;
  bool solved = false;

  void reset(uint32_t seed) {
    *this = {};
    cursor = CELL_COUNT / 2;

    // Starting from an empty board and applying legal presses guarantees that
    // every generated puzzle has a solution.
    uint32_t state = seed != 0 ? seed : 0x6D2B79F5U;
    for (uint8_t i = 0; i < 12; ++i) {
      state ^= state << 13;
      state ^= state >> 17;
      state ^= state << 5;
      toggleCross(static_cast<uint8_t>(state % CELL_COUNT));
    }
    if (isClear()) toggleCross(CELL_COUNT / 2);
  }

  void moveCursor(Direction direction) {
    const uint8_t row = cursor / SIZE;
    const uint8_t col = cursor % SIZE;
    switch (direction) {
      case Direction::Up:
        if (row > 0) cursor = static_cast<uint8_t>(cursor - SIZE);
        break;
      case Direction::Down:
        if (row + 1 < SIZE) cursor = static_cast<uint8_t>(cursor + SIZE);
        break;
      case Direction::Left:
        if (col > 0) --cursor;
        break;
      case Direction::Right:
        if (col + 1 < SIZE) ++cursor;
        break;
    }
  }

  void press() {
    if (solved) return;
    toggleCross(cursor);
    ++moves;
    solved = isClear();
  }

  uint8_t litCount() const {
    uint8_t count = 0;
    for (uint8_t cell : cells) count += cell != 0;
    return count;
  }

 private:
  void toggle(uint8_t row, uint8_t col) { cells[row * SIZE + col] ^= 1U; }

  void toggleCross(uint8_t index) {
    const uint8_t row = index / SIZE;
    const uint8_t col = index % SIZE;
    toggle(row, col);
    if (row > 0) toggle(static_cast<uint8_t>(row - 1), col);
    if (row + 1 < SIZE) toggle(static_cast<uint8_t>(row + 1), col);
    if (col > 0) toggle(row, static_cast<uint8_t>(col - 1));
    if (col + 1 < SIZE) toggle(row, static_cast<uint8_t>(col + 1));
  }

  bool isClear() const {
    for (uint8_t cell : cells) {
      if (cell != 0) return false;
    }
    return true;
  }
};

struct Sokoban {
  static constexpr uint8_t WIDTH = 8;
  static constexpr uint8_t HEIGHT = 8;
  static constexpr uint8_t CELL_COUNT = WIDTH * HEIGHT;
  static constexpr uint8_t LEVEL_COUNT = 3;

  enum Tile : uint8_t { Empty = 0, Wall = 1U << 0, Goal = 1U << 1, Box = 1U << 2 };

  uint8_t cells[CELL_COUNT] = {};
  uint8_t player = 0;
  uint8_t level = 0;
  uint16_t moves = 0;
  bool solved = false;

  void resetLevel(uint8_t requestedLevel) {
    *this = {};
    level = static_cast<uint8_t>(requestedLevel % LEVEL_COUNT);
    const char* layout = LEVELS[level];

    for (uint8_t i = 0; i < CELL_COUNT; ++i) {
      switch (layout[i]) {
        case '#':
          cells[i] = Wall;
          break;
        case '.':
          cells[i] = Goal;
          break;
        case '$':
          cells[i] = Box;
          break;
        case '*':
          cells[i] = Goal | Box;
          break;
        case '+':
          cells[i] = Goal;
          player = i;
          break;
        case '@':
          player = i;
          break;
        default:
          break;
      }
    }
    solved = isSolved();
  }

  bool move(Direction direction) {
    if (solved) return false;

    int delta = 0;
    switch (direction) {
      case Direction::Up:
        delta = -WIDTH;
        break;
      case Direction::Down:
        delta = WIDTH;
        break;
      case Direction::Left:
        delta = -1;
        break;
      case Direction::Right:
        delta = 1;
        break;
    }

    const int target = static_cast<int>(player) + delta;
    if (!isValidStep(player, target) || (cells[target] & Wall) != 0) return false;

    if ((cells[target] & Box) != 0) {
      const int beyond = target + delta;
      if (!isValidStep(static_cast<uint8_t>(target), beyond) || (cells[beyond] & (Wall | Box)) != 0) return false;
      cells[target] &= static_cast<uint8_t>(~Box);
      cells[beyond] |= Box;
    }

    player = static_cast<uint8_t>(target);
    ++moves;
    solved = isSolved();
    return true;
  }

  uint8_t boxesRemaining() const {
    uint8_t count = 0;
    for (uint8_t cell : cells) {
      if ((cell & Box) != 0 && (cell & Goal) == 0) ++count;
    }
    return count;
  }

 private:
  inline static constexpr char LEVELS[LEVEL_COUNT][CELL_COUNT + 1] = {
      "########"
      "#      #"
      "#  .   #"
      "#  $   #"
      "#  @   #"
      "#      #"
      "#      #"
      "########",
      "########"
      "# .    #"
      "# $##  #"
      "#  @   #"
      "#      #"
      "#      #"
      "#      #"
      "########",
      "########"
      "#  ..  #"
      "#  $$  #"
      "#  @   #"
      "#      #"
      "#      #"
      "#      #"
      "########",
  };

  static bool isValidStep(uint8_t from, int to) {
    if (to < 0 || to >= CELL_COUNT) return false;
    const int delta = to - static_cast<int>(from);
    if (delta == 1 || delta == -1) return from / WIDTH == to / WIDTH;
    return delta == WIDTH || delta == -WIDTH;
  }

  bool isSolved() const {
    bool foundBox = false;
    for (uint8_t cell : cells) {
      if ((cell & Box) != 0) {
        foundBox = true;
        if ((cell & Goal) == 0) return false;
      }
    }
    return foundBox;
  }
};

}  // namespace Games
