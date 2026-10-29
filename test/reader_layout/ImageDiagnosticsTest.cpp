#include <Epub/ImageDiagnostics.h>
#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string>

namespace {
using namespace epub_image_diagnostics;

TEST(ImageDiagnostics, InactiveAndNewRunsCannotInheritPreviousObservations) {
  Observer observer;
  observer.imageSeen();
  observer.probeStarted(10, 8);
  observer.probeResult(false, false, 5);
  observer.extractionStarted();
  observer.extractionResult(false, 5);
  observer.altFallback();
  observer.imageBlock();
  EXPECT_EQ(observer.value.seen, 0U);
  EXPECT_EQ(observer.value.beforeProbeFreeHeap, 0U);
  EXPECT_EQ(observer.value.probeStreamFailures, 0U);
  EXPECT_EQ(observer.value.extractAttempts, 0U);
  EXPECT_EQ(observer.value.altFallbacks, 0U);
  EXPECT_EQ(observer.value.imageBlocks, 0U);

  observer.begin(7, 1);
  observer.imageSeen();
  observer.altFallback();  // Placeholder mode does not attempt ZIP/decoder work.
  EXPECT_EQ(observer.value.seen, 1U);
  EXPECT_EQ(observer.value.altFallbacks, 1U);
  EXPECT_EQ(observer.value.dimensionFailures, 0U);
  observer.value.complete = 1;
  observer.begin(8, 2);
  EXPECT_EQ(observer.value.run, 8U);
  EXPECT_EQ(observer.value.mode, 2U);
  EXPECT_EQ(observer.value.seen, 0U);
  EXPECT_EQ(observer.value.complete, 0U);
}

TEST(ImageDiagnostics, SeparatesStreamDimensionsExtractionAndLayoutObservations) {
  Observer observer;
  observer.begin(3, 0);
  observer.imageSeen();
  observer.probeStarted(40000, 32000);
  observer.probeResult(false, false, 5);
  observer.extractionStarted();
  observer.extractionResult(false, 5);
  observer.altFallback();
  observer.imageSeen();
  observer.probeStarted(50000, 40000);
  observer.probeResult(true, false, 0);  // Valid stream, unsupported/bad header.
  observer.extractionStarted();
  observer.extractionResult(true, 0);
  observer.imageBlock();  // Layout creation is not painting or decode success.
  EXPECT_EQ(observer.value.seen, 2U);
  EXPECT_EQ(observer.value.probeStreamFailures, 1U);
  EXPECT_EQ(observer.value.dimensionFailures, 2U);
  EXPECT_EQ(observer.value.lastProbeError, 0U);
  EXPECT_EQ(observer.value.extractAttempts, 2U);
  EXPECT_EQ(observer.value.extractFailures, 1U);
  EXPECT_EQ(observer.value.lastExtractError, 0U);
  EXPECT_EQ(observer.value.altFallbacks, 1U);
  EXPECT_EQ(observer.value.imageBlocks, 1U);
  EXPECT_EQ(observer.value.beforeProbeFreeHeap, 50000U);
  EXPECT_EQ(observer.value.beforeProbeLargestBlock, 40000U);
  observer.value.seen = UINT32_MAX;
  observer.imageSeen();
  EXPECT_EQ(observer.value.seen, UINT32_MAX);
}

TEST(ImageDiagnostics, CheckedRecordBindsRunAndDistinguishesIncompleteTerminalWrite) {
  Observer observer;
  observer.begin(123, 0);
  observer.imageSeen();
  observer.probeStarted(65432, 34567);
  observer.probeResult(false, false, 5);
  observer.extractionStarted();
  observer.extractionResult(false, 8);
  observer.altFallback();
  uint8_t bytes[RECORD_BYTES];
  Snapshot decoded;
  encode(observer.value, bytes);
  ASSERT_TRUE(decode(bytes, sizeof(bytes), 123, decoded));
  EXPECT_EQ(decoded.complete, 0U);
  observer.value.complete = 1;
  encode(observer.value, bytes);
  ASSERT_TRUE(decode(bytes, sizeof(bytes), 123, decoded));
  EXPECT_EQ(decoded.complete, 1U);
  EXPECT_EQ(decoded.lastProbeError, 5U);
  EXPECT_EQ(decoded.lastExtractError, 8U);
  EXPECT_EQ(decoded.beforeProbeLargestBlock, 34567U);
  EXPECT_FALSE(decode(bytes, sizeof(bytes), 124, decoded));
  EXPECT_FALSE(decode(bytes, sizeof(bytes), 0, decoded));
  EXPECT_FALSE(decode(nullptr, sizeof(bytes), 123, decoded));
  for (size_t size = 0; size < sizeof(bytes); ++size) EXPECT_FALSE(decode(bytes, size, 123, decoded));
  EXPECT_FALSE(decode(bytes, sizeof(bytes) + 1, 123, decoded));
  for (auto& byte : bytes) {
    byte ^= 1;
    EXPECT_FALSE(decode(bytes, sizeof(bytes), 123, decoded));
    byte ^= 1;
  }
}

TEST(ImageDiagnostics, ValidChecksumDoesNotAdmitInvalidModesErrorsOrCounters) {
  Snapshot value;
  value.run = 1;
  value.seen = 3;
  value.dimensionFailures = 2;
  value.extractAttempts = 1;
  uint8_t bytes[RECORD_BYTES];
  Snapshot decoded;
  for (auto field : {&Snapshot::mode, &Snapshot::lastProbeError, &Snapshot::lastExtractError, &Snapshot::complete,
                     &Snapshot::probeStreamFailures, &Snapshot::dimensionFailures, &Snapshot::extractAttempts,
                     &Snapshot::extractFailures, &Snapshot::altFallbacks, &Snapshot::imageBlocks}) {
    auto invalid = value;
    invalid.*field = 10;
    encode(invalid, bytes);
    EXPECT_FALSE(decode(bytes, sizeof(bytes), 1, decoded));
  }
}

TEST(ImageDiagnostics, JsonMaximumAndTruncationUseReportedLengthWithoutOverflow) {
  Snapshot largest{UINT32_MAX, 2, UINT32_MAX, UINT32_MAX, UINT32_MAX, 9,          UINT32_MAX,
                   UINT32_MAX, 9, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, 0};
  std::array<char, MAX_JSON_BYTES> json{};
  const auto size = formatJson(json.data(), json.size(), &largest);
  ASSERT_GT(size, 0);
  EXPECT_LT(static_cast<size_t>(size), json.size());
  const std::string output(json.data());
  EXPECT_NE(output.find("\"run\":4294967295"), std::string::npos);
  EXPECT_NE(output.find("\"complete\":false"), std::string::npos);
  EXPECT_NE(output.find("\"lastExtractError\":9"), std::string::npos);
  EXPECT_EQ(output.back(), '}');
  EXPECT_EQ(output.size(), static_cast<size_t>(size));
  std::array<char, 9> shortBuffer;
  shortBuffer.fill('!');
  EXPECT_EQ(formatJson(shortBuffer.data(), shortBuffer.size() - 1, &largest), size);
  EXPECT_EQ(shortBuffer[7], '\0');
  EXPECT_EQ(shortBuffer[8], '!');
  EXPECT_EQ(formatJson(json.data(), json.size(), nullptr), 4);
  EXPECT_STREQ(json.data(), "null");
}
}  // namespace
