#pragma once

#include <stdint.h>

// Physical input loop only. Preserve tap order across slow renders without
// accumulating an unbounded backlog after the user stops pressing buttons.
class ManualTurnQueue {
 public:
  bool empty() const { return count == 0; }
  void clear() { count = 0; }
  bool push(bool forward) {
    if (count == capacity) return false;
    directions[(head + count) % capacity] = forward;
    ++count;
    return true;
  }
  bool pop() {
    const bool forward = directions[head];
    head = (head + 1) % capacity;
    --count;
    return forward;
  }

 private:
  static constexpr uint8_t capacity = 8;
  bool directions[capacity]{};
  uint8_t head = 0;
  uint8_t count = 0;
};
