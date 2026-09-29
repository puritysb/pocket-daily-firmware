#pragma once
// Only what ReadingExchange asks of the firmware Epub: its cache directory.
#include <functional>
#include <string>

class Epub {
 public:
  Epub(std::string path, std::string cacheRoot) : path(std::move(path)), cacheRoot(std::move(cacheRoot)) {}
  std::string getCachePath() const { return cacheRoot + "/epub_" + std::to_string(std::hash<std::string>{}(path)); }

 private:
  std::string path;
  std::string cacheRoot;
};
