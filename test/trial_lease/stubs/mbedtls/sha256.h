#pragma once
#include <cstddef>
#include <cstdint>
// Deterministic noncryptographic SDK stand-in; tests guard the source's exact
// byte coverage and refusal paths, not SHA-256's cryptographic implementation.
struct mbedtls_sha256_context {
  uint32_t value = 2166136261u;
};
inline void mbedtls_sha256_init(mbedtls_sha256_context* c) { c->value = 2166136261u; }
inline int mbedtls_sha256_starts(mbedtls_sha256_context* c, int) {
  c->value = 2166136261u;
  return 0;
}
inline int mbedtls_sha256_update(mbedtls_sha256_context* c, const uint8_t* b, size_t n) {
  for (size_t i = 0; i < n; ++i) c->value = (c->value ^ b[i]) * 16777619u;
  return 0;
}
inline int mbedtls_sha256_finish(mbedtls_sha256_context* c, uint8_t* out) {
  for (unsigned i = 0; i < 32; ++i) out[i] = static_cast<uint8_t>((c->value >> (8 * (i % 4))) ^ (i * 7));
  return 0;
}
inline void mbedtls_sha256_free(mbedtls_sha256_context*) {}
