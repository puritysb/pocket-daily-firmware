#pragma once
#include <string>
namespace obfuscation {
// Test-only deterministic reversible encoding. The store transaction is real;
// hardware-key obfuscation is outside this host test's scope.
inline std::string obfuscateToBase64(const std::string& text) { return "encoded:" + text; }
}  // namespace obfuscation
