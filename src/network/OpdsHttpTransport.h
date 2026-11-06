#pragma once
#include <OpdsTransport.h>

/**
 * Firmware implementation of the SDK OpdsClient's transport: streaming GET and
 * form POST over HttpDownloader (certificate-verified esp_http_client), and URL joining
 * through UrlUtils. Credentials and custom headers go only to the configured
 * server origin (docs/http-credential-origin.md).
 */
class OpdsHttpTransport final : public freeink::opds::OpdsTransport {
 public:
  freeink::opds::HttpResult get(const std::string& url, const freeink::opds::HttpAuth& auth, const char* accept,
                                const char* acceptLanguage, bool captureErrorBody,
                                const freeink::opds::OpdsDataSink& onChunk) override;

  bool postForm(const std::string& url, const std::string& body, std::string& outResponse, int& outStatus) override;

  std::string resolveUrl(const std::string& base, const std::string& ref) override;

  // The saved server URL; requests to any other origin carry no credentials.
  void setCredentialOrigin(const std::string& origin) { credentialOrigin = origin; }

 private:
  std::string credentialOrigin;
};
