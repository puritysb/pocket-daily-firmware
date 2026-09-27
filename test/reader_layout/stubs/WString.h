#pragma once
#include <cstring>
#include <string>
// Minimal Arduino String: not implicitly constructible from std::string, so
// std::string_view overloads stay unambiguous.
class String {
 public:
  String() = default;
  String(const char* s) : s_(s ? s : "") {}
  explicit String(const std::string& s) : s_(s) {}
  const char* c_str() const { return s_.c_str(); }
  size_t length() const { return s_.size(); }
  bool isEmpty() const { return s_.empty(); }
  bool startsWith(const char* p) const { return s_.rfind(p, 0) == 0; }

 private:
  std::string s_;
};
