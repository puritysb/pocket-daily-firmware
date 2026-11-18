#pragma once

namespace PocketDaily::Content {
// Main-task input ownership. None means continue servicing the server, never
// wait/spin for the render task. Keep one action for each consumed Back edge.
class ContentSessionInput {
 public:
  enum class Action { None, DismissView, ExitSession, Next, Previous };
  Action update(bool visible, bool drawing, bool navigationAllowed, bool back, bool next, bool previous,
                bool homePreview = false) {
    if (visible && back && pendingBack_ == Action::None) {
      pendingBack_ = homePreview ? Action::ExitSession : Action::DismissView;
    }
    if (pendingBack_ != Action::None) {
      if (drawing) return Action::None;
      const auto action = pendingBack_;
      pendingBack_ = Action::None;
      return action;
    }
    if (!visible) return back ? Action::ExitSession : Action::None;
    if (drawing || !navigationAllowed) return Action::None;
    if (next) return Action::Next;
    if (previous) return Action::Previous;
    return Action::None;
  }

 private:
  Action pendingBack_ = Action::None;
};
}  // namespace PocketDaily::Content
