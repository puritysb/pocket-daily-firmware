#include "DirectoryAccounting.h"

static_assert(sizeof(DirectoryAccounting) <= 128, "Directory accounting must remain bounded and allocation-free");

namespace {
uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8); }
uint64_t little(const uint8_t* p, unsigned count) {
  uint64_t value = 0;
  for (unsigned i = 0; i < count; ++i) value |= static_cast<uint64_t>(p[i]) << (8 * i);
  return value;
}
uint16_t sumEntry(uint16_t sum, const uint8_t* record, bool primary) {
  for (unsigned i = 0; i < 32; ++i) {
    if (primary && (i == 2 || i == 3)) continue;
    sum = static_cast<uint16_t>((sum >> 1) | (sum << 15));
    sum = static_cast<uint16_t>(sum + record[i]);
  }
  return sum;
}
}  // namespace

DirectoryAccounting::DirectoryAccounting(HalStorage::FilesystemFormat format, Limits limits)
    : format(format), limits(limits) {
  if (format == HalStorage::FilesystemFormat::Unavailable) current = State::Unsupported;
}

bool DirectoryAccounting::totals(Totals& output) const {
  output = current == State::Complete ? accumulated : Totals{};
  return current == State::Complete;
}

DirectoryAccounting::State DirectoryAccounting::finish() {
  if (current == State::Reading) current = lfn || remaining ? State::Invalid : State::Complete;
  return current;
}

DirectoryAccounting::State DirectoryAccounting::step(HalFile& directory) {
  if (current != State::Reading) return current;
  if (slots == limits.slots) return current = State::LimitExceeded;
  uint8_t record[32];
  const auto result = directory.readDirectoryRecord(record);
  if (result == HalFile::DirectoryRead::Error) return current = State::IoError;
  if (result == HalFile::DirectoryRead::End) {
    ++slots;
    return finish();
  }
  return consume(record);
}

DirectoryAccounting::State DirectoryAccounting::consume(const uint8_t (&record)[32]) {
  if (current != State::Reading) return current;
  if (slots == limits.slots) return current = State::LimitExceeded;
  ++slots;
  if (record[0] == 0) return finish();
  return format == HalStorage::FilesystemFormat::Fat ? fat(record) : exfat(record);
}

DirectoryAccounting::State DirectoryAccounting::add(bool directory, uint64_t bytes) {
  if (directory) {
    if (accumulated.directories == limits.directories) return current = State::LimitExceeded;
    ++accumulated.directories;
  } else {
    if (accumulated.files == limits.files || bytes > limits.bytes - accumulated.bytes)
      return current = State::LimitExceeded;
    ++accumulated.files;
    accumulated.bytes += bytes;
  }
  return current;
}

DirectoryAccounting::State DirectoryAccounting::fat(const uint8_t* r) {
  if (r[0] == 0xe5) return lfn ? current = State::Invalid : current;
  if (r[11] == 0x0f) {
    const uint8_t ordinal = r[0] & 0x1f;
    if ((r[0] & 0xa0) || ordinal == 0 || ordinal > 20 || r[12] || le16(r + 26)) return current = State::Invalid;
    if (r[0] & 0x40) {
      if (lfn) return current = State::Invalid;
      lfn = true;
      remaining = ordinal;
      lfnChecksum = r[13];
    }
    if (!lfn || ordinal != remaining || r[13] != lfnChecksum) return current = State::Invalid;
    --remaining;
    return current;
  }
  const bool hadLfn = lfn;
  if (lfn) {
    uint8_t sum = 0;
    for (unsigned i = 0; i < 11; ++i) sum = static_cast<uint8_t>((sum >> 1) | (sum << 7)) + r[i];
    if (remaining || sum != lfnChecksum) return current = State::Invalid;
    lfn = false;
  }
  if ((r[11] & 0xc0) || (r[11] & 0x18) == 0x18) return current = State::Invalid;
  if (r[11] & 8) return hadLfn ? current = State::Invalid : current;  // Volume label.
  const bool directory = (r[11] & 0x10) != 0;
  const auto bytes = little(r + 28, 4);
  if (directory && bytes != 0) return current = State::Invalid;
  if (r[0] == '.') {
    if (!directory || hadLfn) return current = State::Invalid;
    for (unsigned i = r[1] == '.' ? 2 : 1; i < 11; ++i)
      if (r[i] != ' ') return current = State::Invalid;
    return current;  // Dot entries are not child directories.
  }
  return add(directory, bytes);
}

DirectoryAccounting::State DirectoryAccounting::exfat(const uint8_t* r) {
  if (!remaining) {
    if (!(r[0] & 0x80)) return current;                                // Deleted entry.
    if (r[0] == 0x81 || r[0] == 0x82 || r[0] == 0x83) return current;  // Root metadata, not files.
    if (r[0] != 0x85) return current = State::Unsupported;
    if (r[1] < 2 || r[1] > 18 || (le16(r + 4) & ~0x37u)) return current = State::Invalid;
    remaining = r[1];
    pendingDirectory = (r[4] & 0x10) != 0;
    expectedChecksum = le16(r + 2);
    checksum = sumEntry(0, r, true);
    stream = true;
    return current;
  }
  if (stream) {
    if (r[0] != 0xc0 || !(r[1] & 1) || (r[1] & ~3u) || !r[3]) return current = State::Invalid;
    nameEntries = (r[3] + 14) / 15;
    if (remaining != nameEntries + 1) return current = State::Unsupported;  // Extension entries need a decoder.
    pendingBytes = little(r + 24, 8);
    if (little(r + 8, 8) > pendingBytes) return current = State::Invalid;
    stream = false;
  } else {
    if (r[0] != 0xc1 || !nameEntries) return current = State::Invalid;
    --nameEntries;
  }
  checksum = sumEntry(checksum, r, false);
  if (--remaining) return current;
  if (nameEntries || checksum != expectedChecksum) return current = State::Invalid;
  return add(pendingDirectory, pendingBytes);
}
