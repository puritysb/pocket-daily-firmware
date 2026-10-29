#pragma once
namespace BoardConfig {
enum class Board { XteinkX3, XteinkX3Uc8279, XteinkX4, Other };
struct Config {
  Board board;
};
inline Config ACTIVE{Board::XteinkX3};
}  // namespace BoardConfig
