# Verified content snapshot loading — 2026-09-27

## Current closeout — 2026-09-27

The user installed wa4be8509; three Home presentations returned rendered.
HTTP completion remains approximately 14.2 seconds. Further performance tuning
is deferred as a separate task. Earlier pending-installation statements below
are historical. The next priority is the existing EPUB sample reading acceptance.
See [current shared handoff](../../pocket-daily/docs/CURRENT_HANDOFF.md).

This follow-up removes a redundant verification pass during
`ContentViewState::load`. It is not installed on the reader that completed the
30-minute observation in `SCREEN_PRESENTATION_HANDOFF.md`.

## Change and safety boundary

Previously the view recovered the newest valid active revision, verifying its
manifest and every file, then called `loadRevisionCards` to repeat that complete
verification while collecting the same decoded cards. Recovery can now return
the cards it already decoded. The view no longer performs the second pass.

Each retained verification still checks manifest structure/capabilities, file
and manifest SHA before and after semantic reads, card/image validity, IDs and
image references. Newest-valid fallback and exact requested-revision matching
remain. A failed candidate clears its partial cards before a fallback is
examined; no partial output is published. Snapshot OOM is reported explicitly
and never treated as permission to select an older, smaller revision.

This relies on the existing immutable published directories and the caller's
writer/display exclusion, not on a new cache or trust flag. No HTTP field,
admission threshold, polling policy, display timing or font behavior changed.
The legacy `loadRevisionCards` API remains for independent verified reads.

The same at-most-2,400-byte caller-owned snapshot is allocated lazily on the
first verified card. Empty and NoActive allocate none. Successful reloads reuse
the allocation; failure releases it. Its overlap with the existing verifier
workspace has the same bounded peak as the former second pass. No permanent
buffer, cache, retained SD handle or task was added.

## Evidence and limits

The deterministic fixture compares the old composition (recover then load)
with the new view load. SHA input decreases from **3,004 to 1,502 bytes**, with
both pre/post hashes still present; SD bytes read also decreases. This is a
mechanism result for **one view load**, not a measured reduction for an entire
Apply, HTTP exchange or panel refresh. For example, card presentation admission
has its own independent active-revision check, which was not removed.

Tests cover reuse of one snapshot, absence of snapshot allocations for
Empty/NoActive even when snapshot allocation is refused, OOM without fallback,
post-decode card/manifest mutation, failure after partial card collection,
valid older fallback, stale explicit target rejection and output cleanup.

Local checks:

- Complete host suite: **433/433**; content-store executable: **56/56**.
- Sync route-boundary script: **11/11**.
- Strict cppcheck 2.11: **no defects**, low/medium/high fail-on-defect levels
  unchanged; uses the already-built local executable workaround documented in
  `SCREEN_PRESENTATION_HANDOFF.md`.
- Formatting: all **45** changed C++ files pass `clang-format --dry-run
  --Werror`; `git diff --check` is clean.
- `./scripts/pio.sh run -e default`: **success, zero compiler errors/warnings**,
  2026-09-27 02:52:46 KST. Static RAM **113,288 B** (unchanged), application
  flash **5,993,609 B**. These are link-time sizes, not runtime heap evidence.

Ignored verification logs: `build/content-load-verification/`.
Staged only: `firmware/update.bin`, **6,007,488 B**,
SHA-256 `44400247cf2146a470138fff0f03aaea0cec9c354c874e1d11d206b5cfdd3cbf`.
It reports `1.7.0-dev-main-bca376e7-wa4be8509`. The archived artifact is
`firmware/pocket-daily-1.7.0-default-bca376e7-20260927-025246.bin`.
This is an uncommitted development tree, not a release; subsequent builds
replace `update.bin`, so recheck `firmware/LATEST_BUILD.txt` and its checksum
before using it. No installation is implied by staging.

Real-device duration and runtime heap remain unmeasured for this optimization.
The previously observed approximately 14-second presentation delay could be
dominated by font or panel work; this change does not claim to fix it. Repeat
the same content/surface before and after on explicitly installed builds to
measure that separately. No network/device request, installation, flash,
commit, push or release was performed for this follow-up.


## 2026-09-27 — 새 최적화 빌드 Wi-Fi 전송 완료

사용자의 Same Wi-Fi 재연결 후 기존 기기 ID 일치를 확인하고
`1.7.0-dev-main-bca376e7-wa4be8509` 이미지를 `/update.bin`으로 전송했다.
6,007,488 B / CRC32 `8639E84F`, SHA-256
`44400247cf2146a470138fff0f03aaea0cec9c354c874e1d11d206b5cfdd3cbf`.
기기 수신 검사와 원자적 게시가 성공했다. 전송 로그는 펌웨어 저장소의
`build/content-snapshot-upload.log`. 설치/flash는 호출하지 않았다.
전송 후 실행 버전은 여전히 `1.7.0-dev-main-bca376e7-webfaff08`이다.
사용자가 Back으로 Sync 종료 후 기기 설치 확인을 승인하고 Same Wi-Fi로
다시 연결하면 새 버전 확인과 표시 지연 측정을 이어간다.


## 2026-09-27 03:28 — 최적화 빌드 설치 및 Home 반복 측정

사용자 설치·재연결 후 동일 기기에서 `1.7.0-dev-main-bca376e7-wa4be8509`와
`screenPresentation: 1`을 확인했다. 저장된 generation 4 Home을 변경 없이
3회 요청했고 모두 `rendered` / `failure: none`이었다. 앱과 같은 2초 polling,
10초 GET timeout, 90초 budget에서 완료 응답까지 14.290 / 14.259 / 14.149초,
각각 GET timeout 1회 후 읽기 재시도로 성공했다. POST는 0.108 / 0.177 /
0.025초였다. 이는 패널 자체의 광학 측정이 아닌 HTTP 완료 확인 시간이다.

이전 약 14초와 비교해 전체 지연 개선은 확인되지 않았다. 중복 SHA 읽기
감소는 유효하지만 이번 최적화로 표시 지연을 해결했다고 판단하지 않는다.
폰트/패널 등 단계별 시간 계측 없이는 지배 원인을 특정할 수 없다.

uptime 49→101초로 재시작 없이 이어졌고, 요청 사이 heap은 기준 36,200 B에서
36,100 / 36,120 / 36,128 B였다. receipt heap은 34,900 / 34,496 / 34,516 B,
최대 블록은 29,684 / 29,684 / 28,660 B였다. 짧은 3회 측정으로 장기 안정성이나
누수 부재를 주장하지 않는다. 마지막 화면은 Home이다. 새 빌드의 30분 시험,
물리 EPUB 읽기·버튼 방향·X4/direct 확인은 별도다.

근거: 펌웨어 `build/screen-presentation-verification/optimized-20260927-032818.jsonl`.
