# Sync screen presentation handoff — 2026-09-26

## Current closeout — 2026-09-27

The user installed wa4be8509; three Home presentations returned rendered.
HTTP completion remains approximately 14.2 seconds. Further performance tuning
is deferred as a separate task. Earlier pending-installation statements below
are historical. The next priority is the existing EPUB sample reading acceptance.
See [current shared handoff](../../pocket-daily/docs/CURRENT_HANDOFF.md).

The unfinished Claude changes on `main` (`bca376e7`) were reviewed and
completed in place. All implementation files remain uncommitted. No firmware
was installed, flashed, tagged or released. The existing dirty `open-x4-sdk`
submodule was preserved. This handoff does not replace hardware acceptance.

## Implementation and app boundary

- `ScreenPresentation` prepares the saved Home or Daily Brief after HTTP
  cleanup, under the activity render lock, then paints once on the existing
  render task. Inputs and bounded CJK fonts are released after success or
  failure. The 16 KiB free-heap / 4 KiB largest-block admission floor is
  unchanged; no extra framebuffer or task was added.
- `PresentationSlot` shares ownership with card presentation. A queued or
  drawing request cannot be replaced; an accepted request after completion
  invalidates the other kind's receipt. The contract now describes this
  existing writer-gate behavior rather than promising queued replacement.
- The app at `d9dcfd8` calls the same routes, surfaces and generation, verifies
  the receipt identity, and resolves a lost POST response with reads only.
  `rendered` means the display call returned, not optical confirmation.
- Shared Home inputs preserve the device's current book, app cards, glance
  and daily word. Sync uses placeholder cover art instead of decoding a
  cover. The localized Sleep status identifies the frame as a preview while
  the radio is on. Back dismisses it to Sync; RSSI refreshes do not redraw it.
- Review fixes: decimal generation parsing now rejects uint32 overflow on
  ESP32 as well as signs, whitespace and malformed input, for both profile
  writes and screen requests. The empty-weather labels use the selected CJK
  font and participate in preflight. A named card image that fails to draw
  refuses the screen instead of reporting a successful incomplete frame.

Shared renderer sources changed; rebuild the app's host artifact before
claiming the bundled app preview includes these changes. The app session owns
that artifact update and screenshot capture.

## Local verification

Verified against the final local tree:

- `cmake -S test -B build/host-tests -DCMAKE_BUILD_TYPE=Release`,
  `cmake --build build/host-tests -j 8`,
  `ctest --test-dir build/host-tests --output-on-failure -j 8`: **429/429**.
- `python3 scripts/test_sync_routes.py`: **11/11** route-boundary checks.
- `./scripts/pio.sh run -e default`: **success, no compiler errors or
  warnings**, 2026-09-26 17:56:30 KST. Static RAM 113,288 B; application flash
  5,993,471 B. These are link-time sizes, not runtime heap measurements.
- Strict cppcheck 2.11 with all three fail-on-defect severities: **no defects**
  (the local executable selection workaround is described below).
- `./bin/clang-format-fix -g` plus formatting new C++ files, followed by
  `clang-format --dry-run --Werror` on all 37 changed C++ files: clean.
  `git diff --check`: clean.

Ignored logs are under `build/screen-presentation-verification/`. Host
controller tests use fake storage/font/display boundaries; they do not prove
SD, panel, radio, watchdog or heap behavior on a reader.

Staged, **not installed**: `firmware/update.bin`, 6,007,344 B,
SHA-256 `939b1fe34a63db40caa31c93f761c30fea1319dc3d35cb3c432d4a75648b3b00`.
The archived copy is
`firmware/pocket-daily-1.7.0-default-bca376e7-20260926-175630.bin`.
The filename's SHA is the base commit; it includes the uncommitted changes
described here and is not a release artifact. Check `firmware/LATEST_BUILD.txt`
again before using it, because a subsequent build replaces `update.bin`.

The prior Claude shell was still running PlatformIO check after its session
limit. Its exact check process was stopped after confirming its command and
parent; no broad process kill was used. PlatformIO's pinned cppcheck 2.11 ARM
mirror repeatedly refused connections. A newer installed cppcheck 2.20 was
also tried, but its additional baseline findings were not suppressed. For a
comparable strict check, cppcheck 2.11 was built from the official
`danmar/cppcheck` tag in ignored `build/check-tools/cppcheck-2.11`; the ignored
`build/pio-native-check` shim only selects that executable through
`CheckToolBase.get_tool_dir`. Project check flags and fail-on-defect levels
remain unchanged. Temporary `platformio.local.ini` overrides were removed.
The successful command was
`PLATFORMIO_BIN="$PWD/build/pio-native-check" ./scripts/pio.sh check --fail-on-defect low --fail-on-defect medium --fail-on-defect high`.
Run PlatformIO check and firmware build sequentially: metadata generation can
clean the shared `.pio/build/default` directory.

## Hardware acceptance still required

Use the user-installed build on X3 in Sync → Join a Network first:

1. Apply Home three times in one session, changing profile generation. Record
   each receipt, free heap and largest block; confirm the physical frame.
2. Apply Daily Brief, including Korean labels and an empty-weather state.
   Confirm the preview status, unchanged connection, and placeholder cover.
3. Press Back: return to Sync without leaving the session. Present a card
   afterwards, then Home again, and verify old receipts return a conflict.
4. Leave it idle for 30 minutes, compare heap/block samples, and confirm RSSI
   updates do not replace the presented frame. Confirm another Apply works.
5. Check the side-button preference independently by changing it in the app,
   applying, reading it back, and verifying physical page direction.

Repeat presentation checks on X4 and the direct-connection bearer, including
all orientations and missing-font/image recovery. Neither these acceptance
rows nor Bluetooth, local Wi-Fi, SD or OTA behavior are claimed from host tests.


## 2026-09-26 19:00 KST — authorized device staging

The user authorized staging while away. `pocket_put.py` published `/update.bin`
through the existing Same Wi-Fi stream, without `--dev-flash`. The reader
acknowledged 6,007,344 bytes and CRC32 `3542B12E`; the atomic commit also verified
size/CRC. The image SHA-256 is the one recorded above; embedded expected version
is `1.7.0-dev-main-bca376e7-webfaff08`.

A subsequent status read still returned `1.7.0-beta.1`, with no
`screenPresentation` capability. This is staging evidence, not installation.
The user must press Back to end Sync; the teardown restart offers the staged
firmware confirmation. The confirmation does not appear just because upload
finished. After confirming installation and reboot, re-enter Same Wi-Fi and
verify the exact version and `screenPresentation: 1` before acceptance tests.

The EPUB fixture was also published as `/Pocket-EPUB-check-c175a49c.epub`:
188,867 bytes, CRC32 `35A81A11`, SHA-256
`c175a49c25ccdd503059d8bad66619274eeb92bc343d749f753af0df1ac450cc`.
It has not been opened on the physical reader.

Read-only preference check: `sideButtonLayout=0` (Up turns back),
`frontButtonFollowOrientation=0`. No preferences were changed. The earlier
Up turns forward acceptance step remains pending; do not mark it verified.
Ignored local transport logs: `build/screen-firmware-upload.log` and
`build/screen-epub-sample-upload.log`. No commit, push, release or flash occurred.


## 2026-09-27 00:07 KST — installed build and device receipts

After the user completed installation/reconnection, status verified the exact
expected `1.7.0-dev-main-bca376e7-webfaff08` version and `screenPresentation: 1`.
With the existing profile generation 3 unchanged, Home was requested three
times, followed by Brief and Home. All five receipts reached `rendered` with
`failure: none`, matching identity/surface/generation. Deferred free-heap samples
were 34,936–35,260 B and largest block 29,684 B; final status free heap 36,280 B.
This is a short presentation repetition test, not three profile-changing Apply
transactions or the 30-minute idle test. The free heap does not meet the guide's
50 KB general target; no overall memory acceptance is claimed.

The panel was left on Home. Optical confirmation, physical Back, card transition,
30-minute idle, EPUB reading, X4/direct bearer remain pending. Preferences still
read `sideButtonLayout=0`, `frontButtonFollowOrientation=0`; no settings changed.
Evidence: ignored `build/screen-presentation-verification/device-presentation.jsonl`
contains receipts with device identity omitted.


## 2026-09-27 — user optical confirmation

The user confirmed that app Apply changes the physical screen and that normal
Home shows the real book cover. The diagonal cover noticed in Sync is the
specified placeholder, not evidence of lost cover data. Physical Back behavior,
card transitions, changed button mapping, EPUB reading and 30-minute idle are
not inferred from this confirmation. Idle testing awaits re-entry to Same Wi-Fi.


## 2026-09-27 01:59 KST — transitions and idle observation started

On the same installed build, generation 4, card presentation and return to Home
both reached `rendered`; GET of the displaced screen/card receipt returned 409.
Status uptime continued. However, first receipt reads exceeded 8 seconds in two
initial attempts; later read-only recovery confirmed completed renders. A timed
Home probe returned queued in 0.033 s, then after a 2 s wait its receipt GET took
11.108 s. These are observed response delays, not a crash or a clean latency pass.

The observer was aligned with the existing app policy: 2 s poll interval, 10 s
request timeout, 90 s overall confirmation budget, retry reads only. Both card
and Home still logged one timeout before success (~14 s). No firmware change,
new flash or repeated POST recovery was used. Source review suggests content/SD
verification is a candidate; no timing attribution or performance fix is proven.

30-minute observation began at 01:59:55 KST: free heap 36,324 B, uptime 296 s.
The script checks status and the same Home receipt every five minutes, then
requests one Home redraw. It is sampled connectivity/receipt stability, not
optical proof or completely traffic-free idle. Result is pending in ignored
`build/screen-presentation-verification/idle-20260927-app-policy.jsonl`.
The app thread has a follow-up check to record the result and stop afterwards.


## 2026-09-27 02:30 KST — 30-minute observation completed

The existing observer completed without restarting the test. Seven samples over
1,800.4 seconds retained the same installed version, identity, Home generation 4
and rendered receipt. Uptime increased continuously from 296 to 2,096 seconds.
Free heap started at 36,324 B and ended at 36,320 B (delta -4 B); sample range was
36,292–36,336 B. No growing loss is apparent in this short sampled window, but
this does not prove absence of leaks or meet the guide's general 50 KB target.

The final Home request reached `rendered`, `failure: none`, with deferred heap
35,284 B and largest block 29,684 B. It again incurred a 10-second receipt-read
timeout and succeeded on the following read, around 14 seconds after request.
Total observation including final redraw was 1,814.8 seconds. The delay remains
an unresolved performance finding; no transport timeout or busy gate was changed.

Source-only follow-up found that the HTTP handler is paused while presentation
is busy. Candidate costs include repeated whole-revision verification, bounded
font preflight and panel BUSY wait. Exact timing attribution remains unmeasured;
no source optimization or hardware change was made during observation.

This accepts only the sampled Same Wi-Fi connectivity/uptime/receipt checks and
post-idle redraw response. Optical stability throughout the interval, physical
Back, changed button mapping, EPUB reading, X4 and direct connection remain
separate acceptance items. Evidence is
`build/screen-presentation-verification/idle-20260927-app-policy.jsonl`.


## 2026-09-27 — Local content-load optimization follow-up

The redundant second revision verification in ContentViewState was removed.
See [CONTENT_LOAD_HANDOFF.md](CONTENT_LOAD_HANDOFF.md) for integrity boundaries,
433 host tests, 11 route tests, build identity and SHA-256. Apple host artifacts
were rebuilt from the frozen source and imported with fingerprint verification.
The app now explains the Sync cover placeholders in its Apply success message.

A read-only status attempt at 192.168.68.73 timed out before any transfer.
The new wa4be8509 build remains local; no new upload or installation occurred.
The 30-minute hardware evidence above belongs to webfaff08, not this build.
Real-device latency improvement remains unmeasured.


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
