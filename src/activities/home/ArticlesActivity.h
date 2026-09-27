#pragma once
#include <memory>

#include "activities/Activity.h"
#include "articles/ArticleFormat.h"
#include "util/ButtonNavigator.h"

// Bounded index plus only the visible metadata rows. Allocated once while this screen is active;
// under 13 KiB is too large for the task stack, and must not occupy permanent idle/network RAM.
class ArticlesActivity final : public Activity {
  static constexpr size_t LIMIT = 128;
  static constexpr size_t PAGE_ROWS = 8;
  struct Entry {
    char filename[53]{};
    uint64_t savedAt = 0;
  };
  enum class ReadingState : uint8_t { New, Reading, Read, Unavailable };
  struct State {
    Entry entries[LIMIT];
    Articles::Metadata rows[PAGE_ROWS];
    Articles::Metadata scratch;
    ReadingState status[PAGE_ROWS]{};
  };
  static_assert(sizeof(State) <= 13 * 1024, "Article list memory budget");
  std::unique_ptr<State> state;
  ButtonNavigator navigator;
  size_t count = 0;
  size_t selected = 0;
  size_t pageStart = 0;
  bool inputLocked = true;
  bool failed = false;
  bool limited = false;
  size_t rowsPerPage = PAGE_ROWS;
  void load();
  void loadPage();
  void move(int direction);
  void promptDelete(bool readOnly);
  bool deleteEntry(size_t index);
  std::string path(size_t index) const;

 public:
  ArticlesActivity(GfxRenderer& renderer, MappedInputManager& input) : Activity("Articles", renderer, input) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
