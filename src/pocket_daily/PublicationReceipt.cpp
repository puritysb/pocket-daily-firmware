#include "PublicationReceipt.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

namespace PocketDaily::Publication {
namespace {
constexpr char TEMP_PATH[] = "/.crosspoint/pocket-publication.tmp";
constexpr size_t MAX_PATH_BYTES = 512;

void header(const Request& request, uint8_t (&out)[12]) {
  memcpy(out, "PDC1", 4);
  for (unsigned i = 0; i < 4; ++i) {
    out[4 + i] = static_cast<uint8_t>(request.size >> (8 * i));
    out[8 + i] = static_cast<uint8_t>(request.crc32 >> (8 * i));
  }
}

bool equalBytes(HalFile& file, const void* expected, size_t size) {
  uint8_t bytes[64];
  const auto* cursor = static_cast<const uint8_t*>(expected);
  while (size) {
    const size_t count = std::min(size, sizeof(bytes));
    if (file.read(bytes, count) != static_cast<int>(count) || memcmp(bytes, cursor, count) != 0) return false;
    cursor += count;
    size -= count;
  }
  return true;
}
}  // namespace

bool valid(const Request& request) {
  return request.staging && request.target && request.staging[0] == '/' && request.target[0] == '/' &&
         strlen(request.staging) < MAX_PATH_BYTES && strlen(request.target) < MAX_PATH_BYTES;
}

bool matches(const Request& request) {
  if (!valid(request)) return false;
  HalFile file = Storage.open(RECEIPT_PATH, O_RDONLY);
  uint8_t bytes[12];
  header(request, bytes);
  return file && !file.isDirectory() &&
         file.size() == sizeof(bytes) + strlen(request.staging) + strlen(request.target) + 2 &&
         equalBytes(file, bytes, sizeof(bytes)) && equalBytes(file, request.staging, strlen(request.staging) + 1) &&
         equalBytes(file, request.target, strlen(request.target) + 1);
}

bool save(const Request& request) {
  if (!valid(request) || (!Storage.exists("/.crosspoint") && !Storage.mkdir("/.crosspoint"))) return false;
  uint8_t bytes[12];
  header(request, bytes);
  {
    HalFile file = Storage.open(TEMP_PATH, O_WRITE | O_CREAT | O_TRUNC);
    if (!file || file.write(bytes, sizeof(bytes)) != sizeof(bytes) ||
        file.write(request.staging, strlen(request.staging) + 1) != strlen(request.staging) + 1 ||
        file.write(request.target, strlen(request.target) + 1) != strlen(request.target) + 1) {
      LOG_ERR("PUB", "Could not write publication receipt");
      return false;
    }
    file.close();  // Release the path before rename.
  }
  if ((Storage.exists(RECEIPT_PATH) && !Storage.remove(RECEIPT_PATH)) || !Storage.rename(TEMP_PATH, RECEIPT_PATH) ||
      !matches(request)) {
    LOG_ERR("PUB", "Could not verify publication receipt");
    return false;
  }
  return true;
}
}  // namespace PocketDaily::Publication
