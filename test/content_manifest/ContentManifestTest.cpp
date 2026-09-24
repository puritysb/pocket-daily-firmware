#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "pocket_daily/ContentManifest.h"

namespace {
using namespace PocketDaily::Content;
const char* golden =
    "5044434d01001000010001007c00000001000000050000002cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b982461"
    "2e6361726400000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00000000000059feb63f";

std::vector<uint8_t> fixture() {
  std::vector<uint8_t> bytes;
  bytes.reserve(strlen(golden) / 2);
  for (size_t i = 0; golden[i]; i += 2)
    bytes.push_back(static_cast<uint8_t>(std::stoul(std::string(golden + i, 2), nullptr, 16)));
  return bytes;
}
bool read(void* context, size_t offset, uint8_t* output, size_t count) {
  const auto& bytes = *static_cast<std::vector<uint8_t>*>(context);
  if (count > MANIFEST_ENTRY_BYTES || offset > bytes.size() || count > bytes.size() - offset) return false;
  memcpy(output, bytes.data() + offset, count);
  return true;
}
ManifestResult validate(std::vector<uint8_t>& bytes, uint16_t capabilities = 3) {
  ManifestInfo info{99, 99, 99};
  const auto result = validateManifest({&bytes, bytes.size(), read}, capabilities, info);
  if (result != ManifestResult::Ok) {
    EXPECT_EQ(info.fileCount, 0);
    EXPECT_EQ(info.requiredCapabilities, 0);
    EXPECT_EQ(info.contentBytes, 0u);
  }
  return result;
}

void put32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}
void checksum(std::vector<uint8_t>& bytes) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < bytes.size() - 4; ++i) {
    crc ^= bytes[i];
    for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0u);
  }
  put32(bytes, bytes.size() - 4, crc ^ 0xFFFFFFFFu);
}
std::vector<uint8_t> manifest(uint8_t count, FileKind kind, uint32_t size) {
  auto bytes = fixture();
  bytes.resize(20 + MANIFEST_ENTRY_BYTES * count, 0);
  bytes[8] = count;
  bytes[10] = count && kind == FileKind::MonoImage ? 3 : 1;
  put32(bytes, 12, static_cast<uint32_t>(bytes.size()));
  for (unsigned i = 0; i < count; ++i) {
    const size_t start = 16 + MANIFEST_ENTRY_BYTES * i;
    memset(bytes.data() + start, 0, MANIFEST_ENTRY_BYTES);
    bytes[start] = static_cast<uint8_t>(kind);
    put32(bytes, start + 4, size);
    memset(bytes.data() + start + 40, 0, 64);
    snprintf(reinterpret_cast<char*>(bytes.data() + start + 40), 64, "a%02u.%s", i,
             kind == FileKind::Card ? "card" : "pbm");
  }
  checksum(bytes);
  return bytes;
}

TEST(ContentManifest, SwiftGoldenMatches) {
  auto bytes = fixture();
  ManifestInfo info;
  ASSERT_EQ(validateManifest({&bytes, bytes.size(), read}, CAP_CARDS, info), ManifestResult::Ok);
  EXPECT_EQ(info.fileCount, 1);
  EXPECT_EQ(info.requiredCapabilities, CAP_CARDS);
  EXPECT_EQ(info.contentBytes, 5u);
}

TEST(ContentManifest, RejectsEveryTruncationAndEverySingleByteCorruption) {
  const auto original = fixture();
  for (size_t i = 0; i < original.size(); ++i) {
    auto bytes = original;
    bytes.resize(i);
    EXPECT_NE(validate(bytes), ManifestResult::Ok) << i;
    bytes = original;
    bytes[i] ^= 0x80;
    EXPECT_NE(validate(bytes), ManifestResult::Ok) << i;
  }
  auto bytes = original;
  bytes.push_back(0);
  EXPECT_NE(validate(bytes), ManifestResult::Ok);
}

TEST(ContentManifest, RejectsUnsafePathsAndNoncanonicalPadding) {
  for (const auto* path :
       {"../a.card", "/a.card", "dir/a.card", "a\\b.card", "A.card", ".card", "a..card", "-a.card"}) {
    auto bytes = fixture();
    memset(bytes.data() + 56, 0, 64);
    memcpy(bytes.data() + 56, path, strlen(path));
    EXPECT_EQ(validate(bytes), ManifestResult::Entry) << path;
  }
  auto bytes = fixture();
  bytes[119] = 'x';
  EXPECT_EQ(validate(bytes), ManifestResult::Entry);
  memset(bytes.data() + 56, 'x', 64);
  EXPECT_EQ(validate(bytes), ManifestResult::Entry);
}

TEST(ContentManifest, RejectsUnsupportedCapabilitiesAndOversizedPayload) {
  auto bytes = fixture();
  EXPECT_EQ(validate(bytes, 0), ManifestResult::Unsupported);
  bytes[10] = 5;
  EXPECT_EQ(validate(bytes), ManifestResult::Unsupported);
  bytes = fixture();
  bytes[16] = 255;
  EXPECT_EQ(validate(bytes), ManifestResult::Unsupported);
  bytes = fixture();
  memset(bytes.data() + 20, 0xFF, 4);
  EXPECT_EQ(validate(bytes), ManifestResult::TooLarge);
  memset(bytes.data() + 20, 0, 4);
  EXPECT_EQ(validate(bytes), ManifestResult::TooLarge);
}

TEST(ContentManifest, ReadFailureDoesNotPublishInfo) {
  auto bytes = fixture();
  ManifestInfo info{99, 99, 99};
  const ManifestSource source{&bytes, bytes.size(), [](void*, size_t, uint8_t*, size_t) { return false; }};
  EXPECT_EQ(validateManifest(source, 3, info), ManifestResult::ReadFailed);
  EXPECT_EQ(info.fileCount, 0);
}

TEST(ContentManifest, EmptyMaximumAndTotalSizeBounds) {
  auto bytes = manifest(0, FileKind::Card, 1);
  EXPECT_EQ(validate(bytes), ManifestResult::Ok);
  bytes = manifest(3, FileKind::Card, 16 * 1024);
  EXPECT_EQ(validate(bytes), ManifestResult::Ok);
  bytes = manifest(4, FileKind::Card, 1);
  EXPECT_EQ(validate(bytes), ManifestResult::Entry);
  bytes = manifest(16, FileKind::MonoImage, 1);
  EXPECT_EQ(validate(bytes), ManifestResult::Ok);
  EXPECT_EQ(validate(bytes, CAP_CARDS), ManifestResult::Unsupported);
  bytes = manifest(17, FileKind::MonoImage, 1);
  EXPECT_EQ(validate(bytes), ManifestResult::Shape);
  bytes = manifest(4, FileKind::MonoImage, 64 * 1024);
  EXPECT_EQ(validate(bytes), ManifestResult::Ok);
  bytes = manifest(5, FileKind::MonoImage, 64 * 1024);
  EXPECT_EQ(validate(bytes), ManifestResult::TooLarge);
}

TEST(ContentManifest, DuplicateAndReorderedPathsRejectedEvenWithValidChecksum) {
  auto bytes = manifest(2, FileKind::Card, 1);
  memcpy(bytes.data() + 16 + MANIFEST_ENTRY_BYTES + 40, bytes.data() + 56, 64);
  checksum(bytes);
  EXPECT_EQ(validate(bytes), ManifestResult::Order);
  bytes = manifest(2, FileKind::Card, 1);
  bytes[56] = 'z';
  checksum(bytes);
  EXPECT_EQ(validate(bytes), ManifestResult::Order);
}
}  // namespace
