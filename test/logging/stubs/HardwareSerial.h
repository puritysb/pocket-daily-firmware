#pragma once
#include <Arduino.h>

#include <functional>
#include <string>
class HardwareSerial : public Print {
 public:
  bool connected = true;
  int capacity = 256;
  int writes = 0;
  std::string output;
  std::function<void()> duringWrite;
  void begin(unsigned long) {}
  operator bool() const { return connected; }
  int availableForWrite() const { return capacity; }
  size_t write(const uint8_t* data, size_t size) override {
    ++writes;
    if (duringWrite) duringWrite();
    output.append(reinterpret_cast<const char*>(data), size);
    return size;
  }
};
extern HardwareSerial& Serial;
