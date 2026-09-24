#pragma once
#include <cstddef>
#include <cstdint>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
using mbedtls_sha256_context = CC_SHA256_CTX;
inline int mbedtls_sha256_starts(mbedtls_sha256_context* ctx, int) { return CC_SHA256_Init(ctx) == 1 ? 0 : -1; }
inline int mbedtls_sha256_update(mbedtls_sha256_context* ctx, const uint8_t* data, size_t size) {
  return CC_SHA256_Update(ctx, data, static_cast<CC_LONG>(size)) == 1 ? 0 : -1;
}
inline int mbedtls_sha256_finish(mbedtls_sha256_context* ctx, uint8_t* out) {
  return CC_SHA256_Final(out, ctx) == 1 ? 0 : -1;
}
#else
#include <openssl/sha.h>
using mbedtls_sha256_context = SHA256_CTX;
inline int mbedtls_sha256_starts(mbedtls_sha256_context* ctx, int) { return SHA256_Init(ctx) == 1 ? 0 : -1; }
inline int mbedtls_sha256_update(mbedtls_sha256_context* ctx, const uint8_t* data, size_t size) {
  return SHA256_Update(ctx, data, size) == 1 ? 0 : -1;
}
inline int mbedtls_sha256_finish(mbedtls_sha256_context* ctx, uint8_t* out) {
  return SHA256_Final(out, ctx) == 1 ? 0 : -1;
}
#endif
inline void mbedtls_sha256_init(mbedtls_sha256_context*) {}
inline void mbedtls_sha256_free(mbedtls_sha256_context*) {}
