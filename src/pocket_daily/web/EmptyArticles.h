#pragma once

namespace PocketDaily::Web {
// Developer recovery only. The caller owns identity/run/render admission;
// the app proves ownership. Never accept an arbitrary directory or recurse.
template <typename StorageType>
int removeEmptyArticles(StorageType& storage) {
  constexpr const char* path = "/Articles";
  if (!storage.ready()) return 503;
  if (!storage.exists(path)) return 404;
  {
    auto directory = storage.open(path);
    if (!directory) return 503;
    if (!directory.isDirectory()) return 409;
    decltype(directory) entry;
    using Read = typename decltype(directory)::DirectoryRead;
    const auto result = directory.openNextEntry(entry);
    // openNextEntry includes hidden entries and distinguishes I/O errors from EOF.
    if (result == Read::Record) return 409;
    if (result != Read::End) return 503;
  }  // Close both handles before removal; rmdir independently rejects new entries.
  return storage.rmdir(path) ? 204 : 409;
}
}  // namespace PocketDaily::Web
