#pragma once
// Host stand-in: tests assign each book's partial-MD5 digest; calculate() also
// counts calls so tests can see when the record's cached digest was reused.
#include <map>
#include <string>

namespace ReadingSyncTestHost {
inline std::map<std::string, std::string> contentDigests;
inline std::map<std::string, std::string> filenameDigests;
inline int digestComputations = 0;
}  // namespace ReadingSyncTestHost

class KOReaderDocumentId {
 public:
  static std::string calculate(const std::string& path) {
    ++ReadingSyncTestHost::digestComputations;
    const auto it = ReadingSyncTestHost::contentDigests.find(path);
    return it == ReadingSyncTestHost::contentDigests.end() ? std::string() : it->second;
  }
  static std::string calculateFromFilename(const std::string& path) {
    const auto it = ReadingSyncTestHost::filenameDigests.find(path);
    return it == ReadingSyncTestHost::filenameDigests.end() ? std::string() : it->second;
  }
};
