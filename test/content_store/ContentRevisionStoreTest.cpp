#include <HalStorage.h>
#include <gtest/gtest.h>
#include <mbedtls/sha256.h>

#include <array>
#include <cstdio>

#include "pocket_daily/ContentActiveStore.h"
#include "pocket_daily/ContentChecksum.h"
#include "pocket_daily/ContentRevisionStore.h"
#include "pocket_daily/ContentSealStore.h"
#include "pocket_daily/ContentViewState.h"

namespace {
using namespace PocketDaily::Content;
using Bytes = std::vector<uint8_t>;
std::array<uint8_t, 32> sha(const Bytes& data) {
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  std::array<uint8_t, 32> digest{};
  EXPECT_EQ(mbedtls_sha256_starts(&context, 0), 0);
  EXPECT_EQ(mbedtls_sha256_update(&context, data.data(), data.size()), 0);
  EXPECT_EQ(mbedtls_sha256_finish(&context, digest.data()), 0);
  mbedtls_sha256_free(&context);
  return digest;
}
void put32(Bytes& bytes, size_t offset, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}
void crc(Bytes& bytes) {
  put32(bytes, bytes.size() - 4, contentCrcUpdate(UINT32_MAX, bytes.data(), bytes.size() - 4) ^ UINT32_MAX);
}
Bytes card() {
  Bytes data(512, 0);
  memcpy(data.data(), "PDCT", 4);
  data[4] = 1;
  data[6] = 16;
  data[9] = 2;
  strcpy(reinterpret_cast<char*>(data.data() + 16), "morning-1");
  strcpy(reinterpret_cast<char*>(data.data() + 49), "오늘");
  strcpy(reinterpret_cast<char*>(data.data() + 74), "한 줄 읽기\nRead one line.");
  strcpy(reinterpret_cast<char*>(data.data() + 235), "가볍게 시작하세요.");
  strcpy(reinterpret_cast<char*>(data.data() + 427), "sun.pbm");
  crc(data);
  return data;
}
class ContentStore : public testing::Test {
 protected:
  std::map<std::string, Bytes> files;
  std::string revision;
  void SetUp() override {
    FakeSD::reset();
    files["card-00-morning-1.card"] = card();
    files["sun.pbm"] = {80, 52, 10, 57, 32, 50, 10, 0xAA, 0x80, 0x55, 0};
  }
  std::string path(const std::string& name) { return std::string(CONTENT_ROOT) + "/" + revision + "/" + name; }
  void install(bool clearStorage = true, bool layout = false) {
    Bytes manifest(20 + 104 * files.size(), 0);
    memcpy(manifest.data(), "PDCM", 4);
    manifest[4] = 1;
    manifest[6] = 16;
    manifest[8] = static_cast<uint8_t>(files.size());
    manifest[10] = layout ? 5 : 1;
    put32(manifest, 12, static_cast<uint32_t>(manifest.size()));
    size_t index = 0;
    for (const auto& [name, bytes] : files) {
      const size_t offset = 16 + index++ * 104;
      const bool image = name.ends_with(".pbm");
      manifest[offset] = image ? 2 : 1;
      if (image) manifest[10] |= 2;
      put32(manifest, offset + 4, static_cast<uint32_t>(bytes.size()));
      const auto digest = sha(bytes);
      memcpy(manifest.data() + offset + 8, digest.data(), digest.size());
      memcpy(manifest.data() + offset + 40, name.data(), name.size());
    }
    crc(manifest);
    revision.clear();
    for (const auto byte : sha(manifest)) {
      char hex[3];
      snprintf(hex, sizeof(hex), "%02x", byte);
      revision += hex;
    }
    if (clearStorage) FakeSD::files.clear();
    for (const auto& [name, bytes] : files) FakeSD::files[path(name)] = bytes;
    FakeSD::files[path("manifest.pdcm")] = manifest;
  }
  RevisionResult verify(uint16_t capabilities = 3) {
    RevisionInfo info{{99, 99, 99}, 99};
    const auto before = FakeSD::files;
    const auto result = verifyRevision(revision.c_str(), capabilities, info);
    EXPECT_EQ(before, FakeSD::files);  // actual production verifier never writes
    if (result != RevisionResult::Ok) {
      EXPECT_EQ(info.cardCount, 0);
      EXPECT_EQ(info.manifest.fileCount, 0);
      EXPECT_EQ(info.manifest.contentBytes, 0u);
    }
    return result;
  }
  void stage() {
    const auto published = std::string(CONTENT_ROOT) + "/" + revision;
    const auto staged = std::string(CONTENT_STAGING_ROOT) + "/" + revision;
    ASSERT_TRUE(Storage.rename(published.c_str(), staged.c_str()));
  }
  std::string stagedPath(const std::string& name) {
    return std::string(CONTENT_STAGING_ROOT) + "/" + revision + "/" + name;
  }
  void duplicateStaging() {
    for (const auto& [name, bytes] : files) FakeSD::files[stagedPath(name)] = bytes;
    FakeSD::files[stagedPath("manifest.pdcm")] = FakeSD::files.at(path("manifest.pdcm"));
  }
};

TEST_F(ContentStore, RepeatedSealReclaimsOnlyVerifiedDuplicateStaging) {
  install();
  ActiveRevision active;
  ASSERT_EQ(activateRevision(revision.c_str(), 3, active), ActiveResult::Ok);
  const auto published = FakeSD::files;
  for (int i = 0; i < 4; ++i) {
    duplicateStaging();
    RevisionInfo info;
    ASSERT_EQ(sealRevision(revision.c_str(), 3, info), SealResult::Ok);
    EXPECT_EQ(info.cardCount, 1);
    EXPECT_EQ(FakeSD::files, published);
    EXPECT_FALSE(Storage.exists(stagedPath("").c_str()));
  }
  ActiveRevision recovered;
  ASSERT_EQ(recoverActiveRevision(3, recovered), ActiveResult::Ok);
  EXPECT_STREQ(recovered.revision, active.revision);
  EXPECT_EQ(recovered.generation, active.generation);
}

TEST_F(ContentStore, DuplicateCleanupPreservesUnverifiedAndUnexpectedStaging) {
  for (int failure = 0; failure < 4; ++failure) {
    install();
    duplicateStaging();
    if (failure == 0) FakeSD::files[stagedPath("sun.pbm")][0] ^= 1;
    if (failure == 1) FakeSD::files[stagedPath("manifest.pdcm")][0] ^= 1;
    if (failure == 2) FakeSD::files[stagedPath("unrelated.txt")] = {};
    if (failure == 3) FakeSD::files[stagedPath("nested/keep.txt")] = {1, 2, 3};
    const auto before = FakeSD::files;
    RevisionInfo info;
    EXPECT_EQ(sealRevision(revision.c_str(), 3, info), SealResult::Ok);
    EXPECT_EQ(info.cardCount, 1);
    EXPECT_EQ(FakeSD::files, before);
  }
}

TEST_F(ContentStore, DuplicateCleanupNeverRunsAgainstInvalidPublishedRevision) {
  install();
  duplicateStaging();
  FakeSD::files[path("sun.pbm")][0] ^= 1;
  const auto before = FakeSD::files;
  RevisionInfo info;
  EXPECT_EQ(sealRevision(revision.c_str(), 3, info), SealResult::ExistingInvalid);
  EXPECT_EQ(FakeSD::files, before);
}

TEST_F(ContentStore, DuplicateCleanupAcceptsOnlyVerifiedRemainingSubset) {
  for (int missing = 1; missing <= 2; ++missing) {
    install();
    const auto published = FakeSD::files;
    duplicateStaging();
    FakeSD::files.erase(stagedPath("card-00-morning-1.card"));
    if (missing == 2) FakeSD::files.erase(stagedPath("sun.pbm"));
    PreparedRevision partial;
    ASSERT_EQ(inspectStagedRevision(revision.c_str(), 3, partial), RevisionResult::Ok);
    EXPECT_EQ(partial.verifiedCount, 2 - missing);
    EXPECT_EQ(partial.verifiedBytes, missing == 1 ? files.at("sun.pbm").size() : 0);
    const auto safePartial = FakeSD::files;
    // An unverified extra, even zero bytes, prevents any cleanup.
    FakeSD::files[stagedPath("keep.txt")] = {};
    const auto withExtra = FakeSD::files;
    RevisionInfo info;
    EXPECT_EQ(sealRevision(revision.c_str(), 3, info), SealResult::Ok);
    EXPECT_EQ(FakeSD::files, withExtra);
    FakeSD::files = safePartial;
    EXPECT_EQ(sealRevision(revision.c_str(), 3, info), SealResult::Ok);
    EXPECT_EQ(FakeSD::files, published);
  }
}

TEST_F(ContentStore, DuplicateRemovalFailureCannotInvalidatePublishedContent) {
  install();
  ActiveRevision active;
  ASSERT_EQ(activateRevision(revision.c_str(), 3, active), ActiveResult::Ok);
  const auto published = FakeSD::files;
  duplicateStaging();
  FakeSD::failRemove = stagedPath("sun.pbm");
  RevisionInfo info;
  EXPECT_EQ(sealRevision(revision.c_str(), 3, info), SealResult::Ok);
  EXPECT_EQ(info.cardCount, 1);
  for (const auto& [name, bytes] : published) EXPECT_EQ(FakeSD::files.at(name), bytes);
  EXPECT_TRUE(FakeSD::files.count(stagedPath("sun.pbm")));
  EXPECT_TRUE(FakeSD::files.count(stagedPath("manifest.pdcm")));
  EXPECT_FALSE(FakeSD::files.count(stagedPath("card-00-morning-1.card")));
  FakeSD::failRemove.clear();
  // A later explicit seal verifies every remaining file against the published
  // manifest and resumes deletion, without requiring already-deleted assets.
  EXPECT_EQ(sealRevision(revision.c_str(), 3, info), SealResult::Ok);
  EXPECT_EQ(FakeSD::files, published);
  ActiveRevision recovered;
  ASSERT_EQ(recoverActiveRevision(3, recovered), ActiveResult::Ok);
  EXPECT_EQ(recovered.generation, active.generation);
}

TEST_F(ContentStore, SealPublishesVerifiedStagingWithoutActivationAndIsIdempotent) {
  install();
  const auto expected = FakeSD::files;
  stage();
  RevisionInfo info;
  ASSERT_EQ(verifyStagedRevision(revision.c_str(), 3, info), RevisionResult::Ok);
  ASSERT_EQ(sealRevision(revision.c_str(), 3, info), SealResult::Ok);
  EXPECT_EQ(info.cardCount, 1);
  EXPECT_EQ(FakeSD::files, expected);
  FakeSD::failRename = true;
  EXPECT_EQ(sealRevision(revision.c_str(), 3, info), SealResult::Ok);
  EXPECT_EQ(FakeSD::files, expected);
  ActiveRevision active;
  EXPECT_EQ(recoverActiveRevision(3, active), ActiveResult::NoActive);
}

TEST_F(ContentStore, LayoutCapabilityMatchesBytesAndSurvivesActivationAndViewLoad) {
  install(true, true);
  EXPECT_EQ(verify(SUPPORTED_CAPABILITIES), RevisionResult::InvalidManifest);
  auto& bytes = files["card-00-morning-1.card"];
  bytes[4] = 2;
  bytes[491] = 2;
  crc(bytes);
  install();
  EXPECT_EQ(verify(SUPPORTED_CAPABILITIES), RevisionResult::InvalidManifest);
  install(true, true);
  EXPECT_EQ(verify(3), RevisionResult::InvalidManifest);
  EXPECT_EQ(verify(SUPPORTED_CAPABILITIES), RevisionResult::Ok);
  stage();
  RevisionInfo info;
  ASSERT_EQ(sealRevision(revision.c_str(), SUPPORTED_CAPABILITIES, info), SealResult::Ok);
  ActiveRevision active;
  ASSERT_EQ(activateRevision(revision.c_str(), SUPPORTED_CAPABILITIES, active), ActiveResult::Ok);
  RevisionCards loaded;
  ASSERT_EQ(loadRevisionCards(revision.c_str(), SUPPORTED_CAPABILITIES, loaded), RevisionResult::Ok);
  ASSERT_EQ(loaded.count, 1);
  EXPECT_EQ(loaded.cards[0].layout, CardLayout::SideBySide);
  ContentViewState view;
  EXPECT_EQ(view.load(revision.c_str()), ContentViewState::LoadResult::Ready);
}

TEST_F(ContentStore, SealRejectsUndeclaredFilesAndSubdirectoriesBeforeMoving) {
  for (const char* extra : {"leftover.part", "nested/hidden.bin"}) {
    install();
    stage();
    const auto staged = std::string(CONTENT_STAGING_ROOT) + "/" + revision + "/" + extra;
    FakeSD::files[staged] = {};  // Even a zero-byte extra must be refused.
    const auto before = FakeSD::files;
    RevisionInfo info;
    EXPECT_EQ(sealRevision(revision.c_str(), 3, info), SealResult::InvalidCandidate);
    EXPECT_EQ(FakeSD::files, before);
    EXPECT_EQ(info.manifest.fileCount, 0);
    EXPECT_EQ(info.cardCount, 0);
  }
}

TEST_F(ContentStore, SealRefusesIncompleteInventoryAndAllowsExplicitRetry) {
  install();
  stage();
  const auto before = FakeSD::files;
  RevisionInfo info;
  FakeSD::directoryReadError = true;
  EXPECT_EQ(sealRevision(revision.c_str(), 3, info), SealResult::InvalidCandidate);
  EXPECT_EQ(FakeSD::files, before);
  FakeSD::directoryReadError = false;
  FakeSD::unknownFormat = true;
  EXPECT_EQ(sealRevision(revision.c_str(), 3, info), SealResult::InvalidCandidate);
  EXPECT_EQ(FakeSD::files, before);
  FakeSD::unknownFormat = false;
  FakeSD::deletedDirectorySlots = 509;  // Three files leave no budget for confirming end.
  EXPECT_EQ(sealRevision(revision.c_str(), 3, info), SealResult::InvalidCandidate);
  EXPECT_EQ(FakeSD::files, before);
  FakeSD::deletedDirectorySlots = 508;
  EXPECT_EQ(sealRevision(revision.c_str(), 3, info), SealResult::Ok);
}

TEST_F(ContentStore, ExistingPublishedInventoryMustAlsoMatchExactly) {
  install();
  FakeSD::files[path("unexpected.bin")] = {};
  const auto before = FakeSD::files;
  RevisionInfo info;
  EXPECT_EQ(sealRevision(revision.c_str(), 3, info), SealResult::ExistingInvalid);
  EXPECT_EQ(FakeSD::files, before);
  EXPECT_EQ(info.manifest.fileCount, 0);
}

TEST_F(ContentStore, SealReadbackAlsoRejectsUnexpectedPublishedFiles) {
  install();
  stage();
  FakeSD::extraAfterRename = true;
  RevisionInfo info;
  EXPECT_EQ(sealRevision(revision.c_str(), 3, info), SealResult::ReadbackFailed);
  EXPECT_EQ(info.manifest.fileCount, 0);
  EXPECT_TRUE(FakeSD::files.count(path("unexpected.part")));
  EXPECT_EQ(sealRevision(revision.c_str(), 3, info), SealResult::ExistingInvalid);
  ActiveRevision active;
  EXPECT_EQ(recoverActiveRevision(3, active), ActiveResult::NoActive);
}

TEST_F(ContentStore, RetirementKeepsBothRecoverySlotsAndReclaimsOnlyEvictedRevision) {
  ActiveRevision active;
  RetiredRevision retired;
  std::array<std::string, 3> revisions;
  for (size_t i = 0; i < revisions.size(); ++i) {
    files["sun.pbm"][7] = static_cast<uint8_t>('A' + i);
    install(false);
    revisions[i] = revision;
    ASSERT_EQ(activateRevision(revision.c_str(), 3, active, nullptr, &retired), ActiveResult::Ok);
    if (i < 2) EXPECT_STREQ(retired.revision, "");
  }
  ASSERT_STREQ(retired.revision, revisions[0].c_str());
  const auto before = FakeSD::files;
  for (size_t i = 1; i < 3; ++i) EXPECT_EQ(retireRevision(revisions[i].c_str(), "", 3), RetirementResult::Protected);
  EXPECT_EQ(retireRevision(retired.revision, retired.revision, 3), RetirementResult::Protected);
  EXPECT_EQ(retireRevision(retired.revision, nullptr, 3), RetirementResult::Unavailable);
  EXPECT_EQ(retireRevision(retired.revision, "bad", 3), RetirementResult::Unavailable);
  EXPECT_EQ(FakeSD::files, before);
  ASSERT_EQ(retireRevision(retired.revision, "", 3), RetirementResult::Ok);
  const std::string removed = std::string(CONTENT_ROOT) + "/" + revisions[0] + "/";
  for (const auto& [name, bytes] : before) {
    if (name.starts_with(removed))
      EXPECT_FALSE(FakeSD::files.count(name));
    else
      EXPECT_EQ(FakeSD::files.at(name), bytes);
  }
  ASSERT_EQ(recoverActiveRevision(3, active), ActiveResult::Ok);
  EXPECT_STREQ(active.revision, revisions[2].c_str());
  FakeSD::files[std::string(CONTENT_ROOT) + "/" + revisions[2] + "/sun.pbm"][0] ^= 1;
  ASSERT_EQ(recoverActiveRevision(3, active), ActiveResult::Ok);
  EXPECT_STREQ(active.revision, revisions[1].c_str());
}

TEST_F(ContentStore, RetirementRefusesUncertainRecordsAndResumesFailedAssetRemoval) {
  install();
  const auto old = revision;
  ActiveRevision active;
  RetiredRevision retired;
  ASSERT_EQ(activateRevision(revision.c_str(), 3, active), ActiveResult::Ok);
  for (char value : {'A', 'B'}) {
    files["sun.pbm"][7] = value;
    install(false);
    ASSERT_EQ(activateRevision(revision.c_str(), 3, active, nullptr, &retired), ActiveResult::Ok);
  }
  const auto before = FakeSD::files;
  for (const char* slot : {"/.crosspoint/content-active.0", "/.crosspoint/content-active.1"}) {
    FakeSD::files.erase(slot);
    EXPECT_EQ(retireRevision(old.c_str(), "", 3), RetirementResult::Unavailable);
    FakeSD::files = before;
    FakeSD::files[slot][0] ^= 1;
    const auto damaged = FakeSD::files;
    EXPECT_EQ(retireRevision(old.c_str(), "", 3), RetirementResult::Unavailable);
    EXPECT_EQ(FakeSD::files, damaged);
    FakeSD::files = before;
    FakeSD::failReadOpen = slot;
    EXPECT_EQ(retireRevision(old.c_str(), "", 3), RetirementResult::Unavailable);
    FakeSD::failReadOpen.clear();
    EXPECT_EQ(FakeSD::files, before);
  }
  // A false absence report must not hide either protected record.
  FakeSD::falseMissing = {"/.crosspoint/content-active.0", "/.crosspoint/content-active.1"};
  EXPECT_EQ(retireRevision(active.revision, "", 3), RetirementResult::Protected);
  EXPECT_EQ(FakeSD::files, before);
  FakeSD::falseMissing.clear();
  const auto root = std::string(CONTENT_ROOT) + "/" + old + "/";
  FakeSD::failRemove = root + "sun.pbm";
  EXPECT_EQ(retireRevision(old.c_str(), "", 3), RetirementResult::RemoveFailed);
  EXPECT_TRUE(FakeSD::files.count(root + "manifest.pdcm"));
  EXPECT_FALSE(FakeSD::files.count(root + "card-00-morning-1.card"));
  FakeSD::failRemove.clear();
  EXPECT_EQ(retireRevision(old.c_str(), "", 3), RetirementResult::Ok);
  ASSERT_EQ(recoverActiveRevision(3, active), ActiveResult::Ok);
  EXPECT_STREQ(active.revision, revision.c_str());
}

TEST_F(ContentStore, RetirementNeverRecursivelyDeletesUnknownFilesOrInvalidManifest) {
  install();
  const auto old = revision;
  ActiveRevision active;
  RetiredRevision retired;
  ASSERT_EQ(activateRevision(revision.c_str(), 3, active), ActiveResult::Ok);
  for (char value : {'A', 'B'}) {
    files["sun.pbm"][7] = value;
    install(false);
    ASSERT_EQ(activateRevision(revision.c_str(), 3, active, nullptr, &retired), ActiveResult::Ok);
  }
  const auto root = std::string(CONTENT_ROOT) + "/" + old + "/";
  const auto before = FakeSD::files;
  FakeSD::files[root + "manifest.pdcm"][0] ^= 1;
  const auto corrupt = FakeSD::files;
  EXPECT_EQ(retireRevision(old.c_str(), "", 3), RetirementResult::InvalidManifest);
  EXPECT_EQ(FakeSD::files, corrupt);
  FakeSD::files = before;
  FakeSD::files[root + "user/notes.txt"] = {1, 2, 3};
  EXPECT_EQ(retireRevision(old.c_str(), "", 3), RetirementResult::RemoveFailed);
  EXPECT_EQ(FakeSD::files.at(root + "user/notes.txt"), (Bytes{1, 2, 3}));
  const auto after = FakeSD::files;
  EXPECT_EQ(retireRevision("../", "", 3), RetirementResult::InvalidRevision);
  EXPECT_EQ(FakeSD::files, after);
  // Idempotent activation must not produce a stale retirement candidate.
  ASSERT_EQ(activateRevision(revision.c_str(), 3, active, nullptr, &retired), ActiveResult::Ok);
  EXPECT_STREQ(retired.revision, "");
}

TEST_F(ContentStore, RepeatedActivationAndRetirementRetainsTwoRevisionsWithoutGrowth) {
  ActiveRevision active;
  RetiredRevision retired;
  for (unsigned i = 0; i < 20; ++i) {
    files["sun.pbm"][7] = static_cast<uint8_t>(i);
    install(false);
    ASSERT_EQ(activateRevision(revision.c_str(), 3, active, nullptr, &retired), ActiveResult::Ok);
    if (retired.revision[0]) ASSERT_EQ(retireRevision(retired.revision, "", 3), RetirementResult::Ok);
    EXPECT_LE(FakeSD::files.size(), 8u);  // two slots + two (manifest/card/image) revisions
    ASSERT_EQ(recoverActiveRevision(3, active), ActiveResult::Ok);
    EXPECT_STREQ(active.revision, revision.c_str());
  }
  files["sun.pbm"][7] = 21;
  install(false);
  strcpy(retired.revision, "stale output");
  FakeSD::writeBudget = 10;
  EXPECT_EQ(activateRevision(revision.c_str(), 3, active, nullptr, &retired), ActiveResult::WriteFailed);
  EXPECT_STREQ(retired.revision, "");
}

TEST_F(ContentStore, PreparationReportsOnlyActualCandidateHashes) {
  install();
  stage();
  PreparedRevision info;
  unsigned callbacks = 0;
  // Noncapturing hook models endpoint watchdog feeds without adding SDK calls.
  static unsigned progressCalls;
  progressCalls = 0;
  auto progress = [] { ++progressCalls; };
  ASSERT_EQ(inspectStagedRevision(revision.c_str(), 3, info, progress), RevisionResult::Ok);
  EXPECT_GT(progressCalls, callbacks);
  EXPECT_EQ(info.verifiedMask, 3);
  EXPECT_EQ(info.manifest.fileCount, 2);
  const auto image = std::string(CONTENT_STAGING_ROOT) + "/" + revision + "/sun.pbm";
  FakeSD::files[image].back() ^= 1;
  auto before = FakeSD::files;
  ASSERT_EQ(inspectStagedRevision(revision.c_str(), 3, info), RevisionResult::Ok);
  EXPECT_EQ(info.verifiedMask, 1);
  EXPECT_EQ(FakeSD::files, before);
  FakeSD::files.erase(image);
  before = FakeSD::files;
  ASSERT_EQ(inspectStagedRevision(revision.c_str(), 3, info), RevisionResult::Ok);
  EXPECT_EQ(info.verifiedMask, 1);
  EXPECT_EQ(FakeSD::files, before);
}

TEST_F(ContentStore, PreparationReusesUnchangedImageButNotChangedCard) {
  install();
  const auto old = revision;
  const auto oldFiles = FakeSD::files;
  files["card-00-morning-1.card"][235] = 'A';
  crc(files["card-00-morning-1.card"]);
  install(false);
  stage();
  const auto root = std::string(CONTENT_STAGING_ROOT) + "/" + revision + "/";
  FakeSD::files.erase(root + "sun.pbm");
  FakeSD::files.erase(root + "card-00-morning-1.card");
  PreparedRevision info;
  ASSERT_EQ(prepareStagedRevision(revision.c_str(), old.c_str(), 3, info), RevisionResult::Ok);
  EXPECT_EQ(info.verifiedMask, 2);
  EXPECT_EQ(FakeSD::files.at(root + "sun.pbm"), files.at("sun.pbm"));
  EXPECT_FALSE(FakeSD::files.count(root + "card-00-morning-1.card"));
  for (const auto& [name, bytes] : oldFiles) EXPECT_EQ(FakeSD::files.at(name), bytes);
  const auto before = FakeSD::files;
  ASSERT_EQ(prepareStagedRevision(revision.c_str(), old.c_str(), 3, info), RevisionResult::Ok);
  EXPECT_EQ(FakeSD::files, before);
}

TEST_F(ContentStore, ReuseFailuresNeverAcknowledgePartialOrCorruptCopies) {
  install();
  const auto published = FakeSD::files;
  const auto root = std::string(CONTENT_STAGING_ROOT) + "/" + revision + "/";
  for (size_t cutoff = 0; cutoff < files.at("sun.pbm").size(); ++cutoff) {
    FakeSD::files = published;
    FakeSD::files[root + "manifest.pdcm"] = published.at(path("manifest.pdcm"));
    FakeSD::files[root + "card-00-morning-1.card"] = files.at("card-00-morning-1.card");
    FakeSD::writeBudget = cutoff;
    PreparedRevision info;
    ASSERT_EQ(prepareStagedRevision(revision.c_str(), revision.c_str(), 3, info), RevisionResult::CopyFailed);
    EXPECT_EQ(info.verifiedMask, 0) << cutoff;
    EXPECT_EQ(info.manifest.fileCount, 0);
    for (const auto& [name, bytes] : published) EXPECT_EQ(FakeSD::files.at(name), bytes);
  }
  FakeSD::writeBudget = std::numeric_limits<size_t>::max();
  FakeSD::files.erase(root + "sun.pbm");
  FakeSD::corruptWrite = true;
  PreparedRevision info;
  ASSERT_EQ(prepareStagedRevision(revision.c_str(), revision.c_str(), 3, info), RevisionResult::CopyFailed);
  EXPECT_EQ(info.verifiedMask, 0);
}

TEST_F(ContentStore, ReuseCannotTruncateExistingCandidateOrEscapePublishedRoot) {
  install();
  const auto root = std::string(CONTENT_STAGING_ROOT) + "/" + revision + "/";
  FakeSD::files[root + "manifest.pdcm"] = FakeSD::files.at(path("manifest.pdcm"));
  FakeSD::files[root + "sun.pbm"] = {1, 2, 3};
  FakeSD::falseMissing = {root + "sun.pbm"};
  PreparedRevision info;
  ASSERT_EQ(prepareStagedRevision(revision.c_str(), revision.c_str(), 3, info), RevisionResult::CopyFailed);
  EXPECT_EQ(info.verifiedMask, 0);
  EXPECT_EQ(FakeSD::files.at(root + "sun.pbm"), Bytes({1, 2, 3}));
  const auto before = FakeSD::files;
  EXPECT_EQ(prepareStagedRevision(revision.c_str(), "../bad", 3, info), RevisionResult::InvalidRevision);
  EXPECT_EQ(info.verifiedMask, 0);
  EXPECT_EQ(FakeSD::files, before);
}

TEST_F(ContentStore, ReuseCreateFailureStopsPreparationWithoutMutatingPublishedContent) {
  install();
  const auto root = std::string(CONTENT_STAGING_ROOT) + "/" + revision + "/";
  FakeSD::files[root + "manifest.pdcm"] = FakeSD::files.at(path("manifest.pdcm"));
  const auto before = FakeSD::files;
  FakeSD::failWriteOpen = true;
  PreparedRevision info{{99, 99, 99}, 0xffff};
  EXPECT_EQ(prepareStagedRevision(revision.c_str(), revision.c_str(), 3, info), RevisionResult::CopyFailed);
  EXPECT_EQ(info.verifiedMask, 0);
  EXPECT_EQ(info.manifest.fileCount, 0);
  EXPECT_EQ(FakeSD::files, before);
}

TEST_F(ContentStore, PreparationRequiresVerifiedManifestAndClearsOutputOnFailure) {
  install();
  stage();
  PreparedRevision info{{99, 99, 99}, 0xffff};
  EXPECT_EQ(inspectStagedRevision(revision.c_str(), 1, info), RevisionResult::InvalidManifest);
  EXPECT_EQ(info.verifiedMask, 0);
  EXPECT_EQ(info.manifest.fileCount, 0);
  const auto manifest = std::string(CONTENT_STAGING_ROOT) + "/" + revision + "/manifest.pdcm";
  FakeSD::files[manifest].back() ^= 1;
  EXPECT_EQ(inspectStagedRevision(revision.c_str(), 3, info), RevisionResult::ManifestHash);
  EXPECT_EQ(info.verifiedMask, 0);
}

TEST_F(ContentStore, PreparationDoesNotClaimUnreadableFilesOrPublishedCopies) {
  install();
  PreparedRevision info;
  EXPECT_EQ(inspectStagedRevision(revision.c_str(), 3, info), RevisionResult::MissingManifest);
  stage();
  FakeSD::failReadOpen = std::string(CONTENT_STAGING_ROOT) + "/" + revision + "/card-00-morning-1.card";
  EXPECT_EQ(inspectStagedRevision(revision.c_str(), 3, info), RevisionResult::Ok);
  EXPECT_EQ(info.verifiedMask, 2);
  FakeSD::readBudget = 0;
  EXPECT_EQ(inspectStagedRevision(revision.c_str(), 3, info), RevisionResult::ManifestHash);
  EXPECT_EQ(info.verifiedMask, 0);
}

TEST_F(ContentStore, SealRejectsInvalidCandidateWithoutMutation) {
  install();
  stage();
  const auto stagedImage = std::string(CONTENT_STAGING_ROOT) + "/" + revision + "/sun.pbm";
  FakeSD::files[stagedImage].back() ^= 1;
  const auto before = FakeSD::files;
  RevisionInfo info;
  EXPECT_EQ(sealRevision(revision.c_str(), 3, info), SealResult::InvalidCandidate);
  EXPECT_EQ(info.cardCount, 0);
  EXPECT_EQ(FakeSD::files, before);
  EXPECT_EQ(sealRevision("../bad", 3, info), SealResult::InvalidRevision);
  FakeSD::available = false;
  EXPECT_EQ(sealRevision(revision.c_str(), 3, info), SealResult::Unavailable);
}

TEST_F(ContentStore, SealNeverOverwritesExistingInvalidRevision) {
  install();
  stage();
  FakeSD::files[path("manifest.pdcm")] = {0};
  const auto before = FakeSD::files;
  RevisionInfo info;
  EXPECT_EQ(sealRevision(revision.c_str(), 3, info), SealResult::ExistingInvalid);
  EXPECT_EQ(FakeSD::files, before);
}

TEST_F(ContentStore, SealKeepsOldActiveUntilExplicitActivation) {
  install();
  const auto oldRevision = revision;
  ActiveRevision active;
  ASSERT_EQ(activateRevision(revision.c_str(), 3, active), ActiveResult::Ok);
  files["sun.pbm"][7] ^= 0x80;
  install(false);
  stage();
  RevisionInfo info;
  ASSERT_EQ(sealRevision(revision.c_str(), 3, info), SealResult::Ok);
  ASSERT_EQ(recoverActiveRevision(3, active), ActiveResult::Ok);
  EXPECT_STREQ(active.revision, oldRevision.c_str());
  ASSERT_EQ(activateRevision(revision.c_str(), 3, active), ActiveResult::Ok);
  EXPECT_STREQ(active.revision, revision.c_str());
  EXPECT_EQ(active.generation, 2u);
  RevisionInfo oldInfo;
  EXPECT_EQ(verifyRevision(oldRevision.c_str(), 3, oldInfo), RevisionResult::Ok);
}

TEST_F(ContentStore, FailedSealMovePreservesCandidateAndCanRetry) {
  install();
  stage();
  const auto before = FakeSD::files;
  FakeSD::failRename = true;
  RevisionInfo info;
  EXPECT_EQ(sealRevision(revision.c_str(), 3, info), SealResult::MoveFailed);
  EXPECT_EQ(info.cardCount, 0);
  EXPECT_EQ(FakeSD::files, before);
  FakeSD::failRename = false;
  EXPECT_EQ(sealRevision(revision.c_str(), 3, info), SealResult::Ok);
}

TEST_F(ContentStore, AmbiguousSealMoveAndReadbackResolveByPublishedVerification) {
  install();
  stage();
  FakeSD::failAfterRename = true;
  RevisionInfo info;
  EXPECT_EQ(sealRevision(revision.c_str(), 3, info), SealResult::MoveFailed);
  EXPECT_EQ(info.cardCount, 0);
  EXPECT_EQ(sealRevision(revision.c_str(), 3, info), SealResult::Ok);
  FakeSD::failAfterRename = false;
  stage();
  FakeSD::failReadOpen = path("manifest.pdcm");
  EXPECT_EQ(sealRevision(revision.c_str(), 3, info), SealResult::ReadbackFailed);
  EXPECT_EQ(info.cardCount, 0);
  FakeSD::failReadOpen.clear();
  EXPECT_EQ(sealRevision(revision.c_str(), 3, info), SealResult::Ok);
}

TEST_F(ContentStore, CompleteRevisionAndEmptyRevision) {
  install();
  EXPECT_EQ(revision, "77a7e4638640e81c9e380134ea28581928d55d001c0e625f492b7c106e5e2b8b");
  EXPECT_EQ(verify(), RevisionResult::Ok);
  RevisionInfo info;
  ASSERT_EQ(verifyRevision(revision.c_str(), 3, info), RevisionResult::Ok);
  EXPECT_EQ(info.cardCount, 1);
  EXPECT_EQ(info.manifest.fileCount, 2);
  EXPECT_EQ(info.manifest.contentBytes, 523u);
  files.clear();
  install();
  EXPECT_EQ(verify(), RevisionResult::Ok);
}
TEST_F(ContentStore, RejectsMissingAndMutatedFiles) {
  install();
  FakeSD::files.erase(path("sun.pbm"));
  EXPECT_EQ(verify(), RevisionResult::MissingFile);
  install();
  FakeSD::files[path("sun.pbm")].back() ^= 0x80;
  EXPECT_EQ(verify(), RevisionResult::FileHash);
  install();
  FakeSD::files[path("manifest.pdcm")].back() ^= 1;
  EXPECT_EQ(verify(), RevisionResult::ManifestHash);
}
TEST_F(ContentStore, CorrectHashesDoNotBypassSemanticValidation) {
  files["sun.pbm"].back() = 1;  // nonzero unused pixel bits
  install();
  EXPECT_EQ(verify(), RevisionResult::InvalidImage);
  files.erase("sun.pbm");
  files["card-00-morning-1.card"][49] = 0x80;  // invalid UTF-8, valid manifest hash
  crc(files["card-00-morning-1.card"]);
  install();
  EXPECT_EQ(verify(), RevisionResult::InvalidCard);
}
TEST_F(ContentStore, DuplicateIDsAndMissingOrUnusedReferencesRejected) {
  files["card-01-morning-1.card"] = card();
  install();
  EXPECT_EQ(verify(), RevisionResult::DuplicateCard);
  files.erase("card-01-morning-1.card");
  files.erase("sun.pbm");
  install();
  EXPECT_EQ(verify(), RevisionResult::MissingImage);
  files.clear();
  files["sun.pbm"] = {80, 52, 10, 49, 32, 49, 10, 0};
  install();
  EXPECT_EQ(verify(), RevisionResult::UnusedImage);
}
TEST_F(ContentStore, UnsupportedCapabilityAndReadFailuresDoNotWrite) {
  install();
  EXPECT_EQ(verify(1), RevisionResult::InvalidManifest);
  FakeSD::readBudget = 0;
  EXPECT_EQ(verify(), RevisionResult::ManifestHash);
  FakeSD::readBudget = std::numeric_limits<size_t>::max();
  FakeSD::failReadOpen = path("manifest.pdcm");
  EXPECT_EQ(verify(), RevisionResult::MissingManifest);
  FakeSD::available = false;
  EXPECT_EQ(verify(), RevisionResult::Unavailable);
}
TEST_F(ContentStore, RevisionNameCannotEscapeDirectory) {
  install();
  for (const char* name : {"", "../evil", "A000000000000000000000000000000000000000000000000000000000000000"}) {
    RevisionInfo info;
    EXPECT_EQ(verifyRevision(name, 3, info), RevisionResult::InvalidRevision);
  }
  EXPECT_FALSE(validRevision(nullptr));
}

TEST_F(ContentStore, ActivationRecoveryAndIdempotency) {
  install();
  ActiveRevision active;
  EXPECT_EQ(recoverActiveRevision(3, active), ActiveResult::NoActive);
  ASSERT_EQ(activateRevision(revision.c_str(), 3, active), ActiveResult::Ok);
  EXPECT_EQ(active.generation, 1u);
  EXPECT_STREQ(active.revision, revision.c_str());
  const auto before = FakeSD::files;
  ASSERT_EQ(activateRevision(revision.c_str(), 3, active), ActiveResult::Ok);
  EXPECT_EQ(FakeSD::files, before);
  ASSERT_EQ(recoverActiveRevision(3, active), ActiveResult::Ok);
  EXPECT_EQ(active.generation, 1u);
  EXPECT_EQ(active.content.cardCount, 1);
}

TEST_F(ContentStore, CorruptMetadataIsNotReportedAsEmptyActiveState) {
  install();
  FakeSD::files["/.crosspoint/content-active.0"] = {0, 1, 2};
  ActiveRevision active;
  EXPECT_EQ(recoverActiveRevision(3, active), ActiveResult::VerifyFailed);
  EXPECT_EQ(active.generation, 0u);
  FakeSD::files["/.crosspoint/content-active.1"] = {3, 4};
  EXPECT_EQ(recoverActiveRevision(3, active), ActiveResult::VerifyFailed);
}

TEST_F(ContentStore, WatchdogProgressReachesSealActivateAndRecoveryReads) {
  install();
  stage();
  static unsigned calls;
  calls = 0;
  auto progress = [] { ++calls; };
  RevisionInfo sealed;
  ASSERT_EQ(sealRevision(revision.c_str(), 3, sealed, progress), SealResult::Ok);
  EXPECT_GT(calls, 0u);
  calls = 0;
  ActiveRevision active;
  ASSERT_EQ(activateRevision(revision.c_str(), 3, active, progress), ActiveResult::Ok);
  EXPECT_GT(calls, 0u);
  calls = 0;
  ASSERT_EQ(recoverActiveRevision(3, active, progress), ActiveResult::Ok);
  EXPECT_GT(calls, 0u);
  EXPECT_STREQ(active.revision, revision.c_str());
}

TEST_F(ContentStore, EveryInterruptedActiveRecordRetainsPreviousRevision) {
  install();
  const auto previous = revision;
  ActiveRevision active;
  ASSERT_EQ(activateRevision(revision.c_str(), 3, active), ActiveResult::Ok);
  files.clear();  // empty content is a real new revision, not absence
  install(false);
  const auto original = FakeSD::files.at("/.crosspoint/content-active.0");
  for (size_t cutoff = 0; cutoff < 76; ++cutoff) {
    SCOPED_TRACE(cutoff);
    FakeSD::writeBudget = cutoff;
    EXPECT_EQ(activateRevision(revision.c_str(), 3, active), ActiveResult::WriteFailed);
    EXPECT_EQ(active.generation, 0u);
    EXPECT_EQ(FakeSD::files.at("/.crosspoint/content-active.0"), original);
    ASSERT_EQ(recoverActiveRevision(3, active), ActiveResult::Ok);
    EXPECT_STREQ(active.revision, previous.c_str());
  }
  FakeSD::writeBudget = std::numeric_limits<size_t>::max();
  ASSERT_EQ(activateRevision(revision.c_str(), 3, active), ActiveResult::Ok);
  ASSERT_EQ(recoverActiveRevision(3, active), ActiveResult::Ok);
  EXPECT_EQ(active.generation, 2u);
  EXPECT_EQ(active.content.cardCount, 0);
  EXPECT_STREQ(active.revision, revision.c_str());
}

TEST_F(ContentStore, BrokenNewestRevisionFallsBackAndNextCommitProtectsFallback) {
  install();
  const auto previous = revision;
  ActiveRevision active;
  ASSERT_EQ(activateRevision(revision.c_str(), 3, active), ActiveResult::Ok);
  memset(files["card-00-morning-1.card"].data() + 49, 0, 25);
  strcpy(reinterpret_cast<char*>(files["card-00-morning-1.card"].data() + 49), "New");
  crc(files["card-00-morning-1.card"]);
  install(false);
  ASSERT_EQ(activateRevision(revision.c_str(), 3, active), ActiveResult::Ok);
  FakeSD::files[path("sun.pbm")].back() ^= 1;
  ASSERT_EQ(recoverActiveRevision(3, active), ActiveResult::Ok);
  EXPECT_STREQ(active.revision, previous.c_str());
  const auto fallback = FakeSD::files.at("/.crosspoint/content-active.0");
  files.clear();
  install(false);
  ASSERT_EQ(activateRevision(revision.c_str(), 3, active), ActiveResult::Ok);
  EXPECT_EQ(active.generation, 3u);
  EXPECT_EQ(FakeSD::files.at("/.crosspoint/content-active.0"), fallback);
  FakeSD::files[path("manifest.pdcm")].back() ^= 1;
  ASSERT_EQ(recoverActiveRevision(3, active), ActiveResult::Ok);
  EXPECT_STREQ(active.revision, previous.c_str());
}

TEST_F(ContentStore, FailedReadbackIsAmbiguousAndRecoveryResolvesIt) {
  install();
  ActiveRevision active;
  ASSERT_EQ(activateRevision(revision.c_str(), 3, active), ActiveResult::Ok);
  files.clear();
  install(false);
  FakeSD::failReadOpen = "/.crosspoint/content-active.1";
  EXPECT_EQ(activateRevision(revision.c_str(), 3, active), ActiveResult::ReadbackFailed);
  EXPECT_EQ(active.generation, 0u);
  const auto before = FakeSD::files;
  EXPECT_EQ(activateRevision(revision.c_str(), 3, active), ActiveResult::ReadFailed);
  EXPECT_EQ(FakeSD::files, before);
  FakeSD::failReadOpen.clear();
  ASSERT_EQ(recoverActiveRevision(3, active), ActiveResult::Ok);
  EXPECT_EQ(active.generation, 2u);
  EXPECT_STREQ(active.revision, revision.c_str());
  EXPECT_EQ(activateRevision(revision.c_str(), 3, active), ActiveResult::Ok);
  EXPECT_EQ(FakeSD::files, before);
}

TEST_F(ContentStore, InvalidCandidateNeverWritesActivationRecord) {
  install();
  ActiveRevision active;
  ASSERT_EQ(activateRevision(revision.c_str(), 3, active), ActiveResult::Ok);
  const auto before = FakeSD::files;
  const std::string missing(64, '0');
  EXPECT_EQ(activateRevision(missing.c_str(), 3, active), ActiveResult::VerifyFailed);
  EXPECT_EQ(FakeSD::files, before);
  EXPECT_EQ(active.generation, 0u);
}

TEST_F(ContentStore, FalseMissingActiveSlotCannotBeTruncated) {
  install();
  ActiveRevision active;
  ASSERT_EQ(activateRevision(revision.c_str(), 3, active), ActiveResult::Ok);
  const auto originalRevision = revision;
  files.clear();
  install(false);
  const auto before = FakeSD::files;
  FakeSD::falseMissing = {"/.crosspoint/content-active.0"};
  EXPECT_EQ(activateRevision(revision.c_str(), 3, active), ActiveResult::WriteFailed);
  EXPECT_EQ(active.generation, 0u);
  EXPECT_EQ(FakeSD::files, before);
  FakeSD::falseMissing.clear();
  ASSERT_EQ(recoverActiveRevision(3, active), ActiveResult::Ok);
  EXPECT_STREQ(active.revision, originalRevision.c_str());
}

TEST_F(ContentStore, FalseMissingNewestOrBothSlotsPreservesAllRecords) {
  install();
  ActiveRevision active;
  ASSERT_EQ(activateRevision(revision.c_str(), 3, active), ActiveResult::Ok);
  files.clear();
  install(false);
  ASSERT_EQ(activateRevision(revision.c_str(), 3, active), ActiveResult::Ok);
  const auto newestRevision = revision;
  files["card-00-morning-1.card"] = card();
  memset(files["card-00-morning-1.card"].data() + 427, 0, 64);
  crc(files["card-00-morning-1.card"]);
  install(false);
  const auto before = FakeSD::files;
  FakeSD::falseMissing = {"/.crosspoint/content-active.1"};
  EXPECT_EQ(activateRevision(revision.c_str(), 3, active), ActiveResult::WriteFailed);
  EXPECT_EQ(FakeSD::files, before);
  FakeSD::falseMissing.push_back("/.crosspoint/content-active.0");
  EXPECT_EQ(activateRevision(revision.c_str(), 3, active), ActiveResult::WriteFailed);
  EXPECT_EQ(FakeSD::files, before);
  FakeSD::falseMissing.clear();
  ASSERT_EQ(recoverActiveRevision(3, active), ActiveResult::Ok);
  EXPECT_STREQ(active.revision, newestRevision.c_str());
  EXPECT_EQ(active.generation, 2u);
}

TEST_F(ContentStore, EveryCorruptedRecordByteFallsBackToPrevious) {
  install();
  const auto previous = revision;
  ActiveRevision active;
  ASSERT_EQ(activateRevision(revision.c_str(), 3, active), ActiveResult::Ok);
  files.clear();
  install(false);
  ASSERT_EQ(activateRevision(revision.c_str(), 3, active), ActiveResult::Ok);
  const auto record = FakeSD::files.at("/.crosspoint/content-active.1");
  for (size_t offset = 0; offset < record.size(); ++offset) {
    FakeSD::files["/.crosspoint/content-active.1"] = record;
    FakeSD::files["/.crosspoint/content-active.1"][offset] ^= 0x80;
    ASSERT_EQ(recoverActiveRevision(3, active), ActiveResult::Ok) << offset;
    EXPECT_STREQ(active.revision, previous.c_str());
  }
}

TEST_F(ContentStore, LoadsVerifiedCardTextAndImageReferenceWithoutWriting) {
  install();
  const auto before = FakeSD::files;
  RevisionCards cards;
  ASSERT_EQ(loadRevisionCards(revision.c_str(), 3, cards), RevisionResult::Ok);
  ASSERT_EQ(cards.count, 1);
  EXPECT_STREQ(cards.cards[0].card.cardId, "app:morning-1");
  EXPECT_STREQ(cards.cards[0].card.title, "오늘");
  EXPECT_STREQ(cards.cards[0].card.module, "app");
  EXPECT_EQ(cards.cards[0].card.choiceCount, 0);
  EXPECT_STREQ(cards.cards[0].imagePath, "sun.pbm");
  EXPECT_EQ(FakeSD::files, before);
}

TEST_F(ContentStore, LateImageFailureClearsPreviouslyDecodedCard) {
  install();
  RevisionCards cards;
  ASSERT_EQ(loadRevisionCards(revision.c_str(), 3, cards), RevisionResult::Ok);
  FakeSD::files[path("sun.pbm")].back() ^= 1;
  EXPECT_EQ(loadRevisionCards(revision.c_str(), 3, cards), RevisionResult::FileHash);
  EXPECT_EQ(cards.count, 0);
  for (const auto& card : cards.cards) {
    EXPECT_EQ(card.card.cardId[0], 0);
    EXPECT_EQ(card.card.title[0], 0);
    EXPECT_EQ(card.imagePath[0], 0);
  }
}

TEST_F(ContentStore, InvalidLoadClearsExistingOutputAndEmptyRevisionLoadsEmpty) {
  install();
  RevisionCards cards;
  ASSERT_EQ(loadRevisionCards(revision.c_str(), 3, cards), RevisionResult::Ok);
  EXPECT_EQ(loadRevisionCards("../bad", 3, cards), RevisionResult::InvalidRevision);
  EXPECT_EQ(cards.count, 0);
  EXPECT_EQ(cards.cards[0].card.cardId[0], 0);
  files.clear();
  install();
  EXPECT_EQ(loadRevisionCards(revision.c_str(), 3, cards), RevisionResult::Ok);
  EXPECT_EQ(cards.count, 0);
}

TEST_F(ContentStore, ContentViewPinsVerifiedSelectionAndReusesSingleSnapshot) {
  install();
  ActiveRevision active;
  ASSERT_EQ(activateRevision(revision.c_str(), 3, active), ActiveResult::Ok);
  const auto before = FakeSD::files;
  ContentViewState view;
  ASSERT_EQ(view.load(revision.c_str()), ContentViewState::LoadResult::Ready);
  ASSERT_NE(view.cards(), nullptr);
  EXPECT_EQ(view.cards()->count, 1);
  EXPECT_STREQ(view.revision(), revision.c_str());
  EXPECT_EQ(view.generation(), active.generation);
  const auto* allocation = view.cards();
  EXPECT_EQ(view.load(view.revision()), ContentViewState::LoadResult::Ready);
  EXPECT_EQ(view.cards(), allocation);
  EXPECT_EQ(FakeSD::files, before);
  view.reset();
  EXPECT_EQ(view.cards(), nullptr);
  EXPECT_EQ(view.generation(), 0u);
  EXPECT_STREQ(view.revision(), "");
}

TEST_F(ContentStore, ContentViewRefusesFallbackForExplicitPresentationTarget) {
  install();
  const auto previous = revision;
  ActiveRevision active;
  ASSERT_EQ(activateRevision(revision.c_str(), 3, active), ActiveResult::Ok);
  files.clear();
  install(false);
  ASSERT_EQ(activateRevision(revision.c_str(), 3, active), ActiveResult::Ok);
  // The newest active directory becomes unreadable, so offline recovery may
  // show the older valid selection, but a live request must not claim it is new.
  FakeSD::files[path("manifest.pdcm")].back() ^= 1;
  ContentViewState view;
  ASSERT_EQ(view.load(), ContentViewState::LoadResult::Ready);
  EXPECT_STREQ(view.revision(), previous.c_str());
  EXPECT_EQ(view.load(revision.c_str()), ContentViewState::LoadResult::TargetChanged);
  EXPECT_EQ(view.cards(), nullptr);
  EXPECT_EQ(view.generation(), 0u);
  EXPECT_STREQ(view.revision(), "");
}

TEST_F(ContentStore, ContentViewDistinguishesEmptySelectionFromNoActive) {
  ContentViewState view;
  EXPECT_EQ(view.load(), ContentViewState::LoadResult::NoActive);
  files.clear();
  install();
  ActiveRevision active;
  ASSERT_EQ(activateRevision(revision.c_str(), 3, active), ActiveResult::Ok);
  EXPECT_EQ(view.load(revision.c_str()), ContentViewState::LoadResult::Empty);
  EXPECT_EQ(view.cards(), nullptr);
  EXPECT_STREQ(view.revision(), revision.c_str());
  EXPECT_EQ(view.generation(), 1u);
}

TEST_F(ContentStore, ContentViewFailureDropsPreviousCardsAndMetadata) {
  install();
  ActiveRevision active;
  ASSERT_EQ(activateRevision(revision.c_str(), 3, active), ActiveResult::Ok);
  ContentViewState view;
  ASSERT_EQ(view.load(), ContentViewState::LoadResult::Ready);
  EXPECT_EQ(view.load("../bad"), ContentViewState::LoadResult::InvalidTarget);
  EXPECT_EQ(view.cards(), nullptr);
  ASSERT_EQ(view.load(), ContentViewState::LoadResult::Ready);
  FakeSD::files[path("sun.pbm")].back() ^= 1;
  EXPECT_EQ(view.load(), ContentViewState::LoadResult::Unavailable);
  EXPECT_EQ(view.cards(), nullptr);
  EXPECT_EQ(view.generation(), 0u);
  EXPECT_STREQ(view.revision(), "");
}

TEST_F(ContentStore, GenerationExhaustionNeverWraps) {
  install();
  ActiveRevision active;
  ASSERT_EQ(activateRevision(revision.c_str(), 3, active), ActiveResult::Ok);
  auto& record = FakeSD::files.at("/.crosspoint/content-active.0");
  put32(record, 4, UINT32_MAX);
  crc(record);
  ASSERT_EQ(recoverActiveRevision(3, active), ActiveResult::Ok);
  EXPECT_EQ(active.generation, UINT32_MAX);
  EXPECT_EQ(activateRevision(revision.c_str(), 3, active), ActiveResult::Ok);  // idempotent
  files.clear();
  install(false);
  const auto before = FakeSD::files;
  EXPECT_EQ(activateRevision(revision.c_str(), 3, active), ActiveResult::GenerationExhausted);
  EXPECT_EQ(FakeSD::files, before);
}
}  // namespace

// Read-only companion access (GET /api/pocket/v1/content/file) resolves only
// leaf files of one published revision.
TEST(ContentPublishedFilePath, ResolvesOnlyManifestShapedLeavesOfARevision) {
  const std::string revision(64, 'a');
  char out[128];
  ASSERT_TRUE(publishedFilePath(revision.c_str(), "manifest.pdcm", out, sizeof(out)));
  EXPECT_EQ(std::string(out), std::string(CONTENT_ROOT) + "/" + revision + "/manifest.pdcm");
  EXPECT_TRUE(publishedFilePath(revision.c_str(), "card-00-morning-1.card", out, sizeof(out)));
  EXPECT_TRUE(publishedFilePath(revision.c_str(), "sun.pbm", out, sizeof(out)));

  const char* rejected[] = {"",           "../content-active.0", "a/b.card", "Sun.pbm",  "sun.png",
                            "-lead.card", "manifest.pdcm.bak",   ".card",    "sun..pbm", "sun.pbm/"};
  for (const char* name : rejected) {
    EXPECT_FALSE(publishedFilePath(revision.c_str(), name, out, sizeof(out))) << name;
    EXPECT_STREQ(out, "") << name;
  }
  // The companion's real names (card-00-<32-hex id>.card) and the longest leaf
  // the manifest allows must fit the endpoint's buffer.
  char endpoint[PUBLISHED_PATH_BYTES];
  EXPECT_TRUE(
      publishedFilePath(revision.c_str(), "card-00-9d7b3c1c480a4c7cac3a39e56edee2a1.card", endpoint, sizeof(endpoint)));
  const std::string longest = std::string(58, 'a') + ".card";
  ASSERT_EQ(longest.size(), 63u);
  EXPECT_TRUE(publishedFilePath(revision.c_str(), longest.c_str(), endpoint, sizeof(endpoint)));
  EXPECT_EQ(std::string(endpoint), std::string(CONTENT_ROOT) + "/" + revision + "/" + longest);
  const std::string longName = std::string(64, 'a') + ".card";
  EXPECT_FALSE(publishedFilePath(revision.c_str(), longName.c_str(), out, sizeof(out)));
  EXPECT_FALSE(publishedFilePath(std::string(64, 'A').c_str(), "sun.pbm", out, sizeof(out)));
  EXPECT_FALSE(publishedFilePath("abc", "sun.pbm", out, sizeof(out)));
  EXPECT_FALSE(publishedFilePath(revision.c_str(), nullptr, out, sizeof(out)));
  char tiny[16];
  EXPECT_FALSE(publishedFilePath(revision.c_str(), "sun.pbm", tiny, sizeof(tiny)));
  EXPECT_STREQ(tiny, "");
}
