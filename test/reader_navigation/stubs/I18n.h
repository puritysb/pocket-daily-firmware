#pragma once
enum TestStringId { STR_EOB_HOME, STR_POCKET_DAILY_APP, STR_ARTICLES, STR_BACK, STR_HOME };
inline const char* tr(TestStringId id) {
  switch (id) {
    case STR_EOB_HOME:
      return "Home";
    case STR_POCKET_DAILY_APP:
      return "Pocket Daily";
    case STR_ARTICLES:
      return "Articles";
    case STR_BACK:
      return "Back";
    case STR_HOME:
      return "Home";
  }
  return "";
}
