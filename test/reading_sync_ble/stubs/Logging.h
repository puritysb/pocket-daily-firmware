#pragma once
// Host tests never print firmware logs.
template <typename... Args>
inline void readingSyncHostLog(const char*, Args&&...) {}
#define LOG_ERR(...) readingSyncHostLog(__VA_ARGS__)
#define LOG_DBG(...) readingSyncHostLog(__VA_ARGS__)
#define LOG_INF(...) readingSyncHostLog(__VA_ARGS__)
