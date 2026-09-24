#pragma once

#include <string_view>

namespace PocketDaily::Web {
// Request-scoped borrowed URI, no ownership/heap or permanent route objects.
// Exact method/path matching preserves WebServer::on semantics used by Pocket.
class ExactRouteDispatch {
 public:
  ExactRouteDispatch(std::string_view path, int method) : path_(path), method_(method) {}
  template <typename Handler>
  void on(const char* path, int method, Handler handler) {
    if (!handled_ && method_ == method && path_ == path) {
      handled_ = true;
      handler();
    }
  }
  bool handled() const { return handled_; }

 private:
  std::string_view path_;
  int method_;
  bool handled_ = false;
};
}  // namespace PocketDaily::Web
