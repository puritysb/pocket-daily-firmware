#pragma once

#include <cstdlib>
#include <cstring>

// Plain-language reason for a failed update check or install. The update
// screen shows the reason and, when this reader cannot download over HTTPS,
// points to the companion app, which downloads the release and sends it over
// the local transfer path instead. Free of ESP-IDF types for the host tests.
namespace OtaFailure {

enum class Kind {
  Memory,   // TLS or buffers could not be allocated on this reader
  Network,  // DNS/TCP/TLS connection or read failed
  Server,   // the release server answered unexpectedly
  Release,  // the release information could not be read
  Install,  // the image download/write failed; the running firmware is kept
};

// Values mirrored from ESP-IDF/mbedTLS so this header needs neither.
inline constexpr int kEspErrNoMem = 0x101;              // ESP_ERR_NO_MEM
inline constexpr int kMbedtlsSslAllocFailed = 0x7F00;   // -MBEDTLS_ERR_SSL_ALLOC_FAILED
inline constexpr int kMbedtlsMpiAllocFailed = 0x0010;   // -MBEDTLS_ERR_MPI_ALLOC_FAILED
inline constexpr int kMbedtlsX509AllocFailed = 0x2880;  // -MBEDTLS_ERR_X509_ALLOC_FAILED

// OtaUpdater error values this classifier reads.
inline constexpr int kUpdaterHttpError = 2;
inline constexpr int kUpdaterJsonParseError = 3;
inline constexpr int kUpdaterOomError = 6;

// esp-tls reports the mbedTLS code with either sign; compare magnitudes.
inline bool isTlsAllocation(const int tlsCode) {
  const int code = std::abs(tlsCode);
  return code == kMbedtlsSslAllocFailed || code == kMbedtlsMpiAllocFailed || code == kMbedtlsX509AllocFailed;
}

// `stage`/`code`/`tlsCode` come from HttpDownloader::lastFailure() for the
// check; `installing` is true when the failure came from installUpdate().
inline Kind classify(const int updaterError, const char* stage, const int code, const int tlsCode,
                     const bool installing) {
  if (updaterError == kUpdaterOomError || isTlsAllocation(tlsCode) || code == kEspErrNoMem) return Kind::Memory;
  if (installing) return Kind::Install;
  if (updaterError == kUpdaterJsonParseError) return Kind::Release;
  if (stage) {
    if (strcmp(stage, "buffer") == 0 || strcmp(stage, "reserve") == 0 || strcmp(stage, "redirect oom") == 0)
      return Kind::Memory;
    if (strcmp(stage, "status") == 0 || strcmp(stage, "redirect") == 0 || strcmp(stage, "redirects") == 0)
      return Kind::Server;
  }
  return Kind::Network;
}

}  // namespace OtaFailure
