#pragma once

// Persistence boundary only: compile the real CrossPointSettings declaration
// and fail tests if font lifecycle code attempts a settings write.
template <class T>
class PersistableStore {
 public:
  inline static unsigned saveCalls = 0;
  static T& getInstance() {
    static T value;
    return value;
  }
  bool saveToFile() const {
    ++saveCalls;
    return true;
  }
};
