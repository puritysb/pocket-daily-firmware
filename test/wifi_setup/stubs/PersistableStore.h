#pragma once
#include <ArduinoJson.h>
#include <HalStorage.h>

#include <mutex>
#include <string>

template <class T>
class PersistableStore {
 protected:
  mutable std::mutex storeMutex;
  void requestResave() {}
  static std::string extractPassword(JsonVariantConst entry, bool&, size_t limit, bool& valid) {
    const std::string encoded = entry["password_obf"] | "";
    valid = encoded.starts_with("encoded:") && encoded.size() - 8 <= limit;
    return valid ? encoded.substr(8) : "";
  }
  static bool writeDocToFile(const char* path, const JsonDocument& doc) {
    std::string json;
    serializeJson(doc, json);
    auto file = Storage.open(path, O_WRITE | O_CREAT | O_TRUNC);
    return file && file.write(reinterpret_cast<const uint8_t*>(json.data()), json.size()) == json.size();
  }

 public:
  static T& getInstance() {
    static T value;
    return value;
  }
  bool saveToFile() const {
    std::lock_guard lock(storeMutex);
    JsonDocument doc;
    static_cast<const T*>(this)->toJson(doc);
    return writeDocToFile(T::getFilePath(), doc);
  }
  bool loadFromFile() {
    std::lock_guard lock(storeMutex);
    auto it = FakeSD::files.find(T::getFilePath());
    if (it == FakeSD::files.end()) return false;
    JsonDocument doc;
    if (deserializeJson(doc, it->second.data(), it->second.size())) return false;
    return static_cast<T*>(this)->fromJson(doc.as<JsonVariantConst>());
  }
};
