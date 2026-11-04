# HTTP credential origin safety — 2026-11-04

Local fix for app issue [15](https://github.com/puritysb/pocket-daily/issues/15).
Owner authorized commit/push on2026-11-04. This is not installed on a reader and is not a public release.

GET Basic credentials and all caller-supplied request headers are sent only to
the original scheme/host/effective-port. OPDS feeds and acquisition downloads
explicitly bind that origin to the configured server, so absolute external links
cannot inherit its credentials. HTTPS-to-HTTP transitions are rejected before
the next network request. Same-origin paths retain authentication; public HTTPS
CDN redirects remain possible without credentials. Protocol-relative redirects
use the current scheme. Invalid/userinfo/ambiguous authorities fail closed.
Bare saved OPDS hosts retain their historical HTTP interpretation.

HttpOrigin uses borrowed string_views and allocates no heap. Existing client
configuration and Basic-header construction have bounded non-inlined frames;
actual default/gh_release frame sizes are recorded in verification.json. This
is not a runtime heap/optical/radio measurement or a total call-chain stack claim.
Existing POST behaviour is preserved; the fix does not change device endpoints,
BLE records, EPUB formats or companion transport contracts.

Verification: complete host1194/1194; six new origin tests; default and gh_release
compile/link with zero compiler warnings; strict cppcheck passes. Companion
evidence records four detected host/port/scheme/downgrade mutations. A local
build is not device acceptance. Evidence: build/release-security-20261104/.

Before release: commit/review the exact source, obtain green CI and test the
frozen image on applicable X3/X4 scenarios, including real same-origin OPDS,
HTTPS CDN acquisition without Authorization and refusal of a downgrade. Use
synthetic credentials and an owner-controlled server; never log real passwords.
Do not close the upstream issue as merged or inherit older image sign-off.
