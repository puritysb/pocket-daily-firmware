#pragma once
// Host tests never print firmware logs.
template <typename... Args>
inline void readingProgressHostLog(const char*, Args&&...) {}
#define LOG_ERR(...) readingProgressHostLog(__VA_ARGS__)
#define LOG_DBG(...) readingProgressHostLog(__VA_ARGS__)
#define LOG_INF(...) readingProgressHostLog(__VA_ARGS__)
