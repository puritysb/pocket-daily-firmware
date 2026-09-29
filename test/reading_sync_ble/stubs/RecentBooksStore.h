#pragma once
#include <string>
#include <vector>

struct RecentBook {
  std::string path;
  std::string title;
  std::string author;
  std::string coverBmpPath;
};

class RecentBooksStore {
 public:
  static RecentBooksStore& getInstance() {
    static RecentBooksStore store;
    return store;
  }
  const std::vector<RecentBook>& getBooks() const { return books; }
  std::vector<RecentBook> books;
};
#define RECENT_BOOKS RecentBooksStore::getInstance()
