#pragma once
#include <cstddef>
#include <cstdint>
namespace PocketDaily::ReadingShelf {
inline constexpr size_t CAP = 4, PATH_BYTES = 256, RECORD_BYTES = 1036;
inline constexpr char PATH[] = "/.crosspoint/pocket-reading-shelf.bin";
inline constexpr char TEMP_PATH[] = "/.crosspoint/pocket-reading-shelf.tmp";
inline constexpr char BACKUP_PATH[] = "/.crosspoint/pocket-reading-shelf.bak";
struct Selection {
  bool automatic = true;
  uint8_t count = 0;
  char paths[CAP][PATH_BYTES]{};
};
enum class SaveResult : uint8_t { Ok, Invalid, OutOfMemory, StorageError };
bool valid(const Selection& selection);
bool parseJson(const char* json, size_t size, Selection& selection, bool& outOfMemory);
bool encode(const Selection& selection, uint8_t (&bytes)[RECORD_BYTES]);
bool decode(const uint8_t* bytes, size_t size, Selection& selection);
bool load(Selection& selection);
SaveResult save(const Selection& selection);
}  // namespace PocketDaily::ReadingShelf
