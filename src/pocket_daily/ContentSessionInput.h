#pragma once

namespace PocketDaily::Content {
// Main-task input ownership. None means continue servicing the server, never
// wait/spin for the render task. A consumed Back edge cannot later exit a session.
class ContentSessionInput {
 public:
  enum class Action { None, DismissView, ExitSession, Next, Previous };
  Action update(bool visible, bool drawing, bool navigationAllowed, bool back, bool next, bool previous) {
    if (visible && back) dismissPending_ = true;
    if (dismissPending_) {
      if (drawing) return Action::None;
      dismissPending_ = false;
      return Action::DismissView;
    }
    if (!visible) return back ? Action::ExitSession : Action::None;
    if (drawing || !navigationAllowed) return Action::None;
    if (next) return Action::Next;
    if (previous) return Action::Previous;
    return Action::None;
  }

 private:
  bool dismissPending_ = false;
};
}  // namespace PocketDaily::Content
