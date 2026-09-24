# Transport measurement baseline — 2026-09-21

## Payload integrity gate — 2026-09-22

New CLI trials use deterministic SHAKE256 output seeded with the public string
`pocket-transfer-benchmark-v1`, identified as `shake256-pocket-bench-v1` in logs.
The prefix is stable across requested lengths, but 256/512/1024/4096-byte blocks
at different offsets are distinct in the tested 16KiB fixture. This replaces
the old repeated 0..255 pattern: a duplicated or reordered equal-size block of
that old pattern could leave the bytes unchanged, making the test insensitive
to that class of transport defect. Historical exact-byte checks remain valid
for their original payload; do not upgrade them to arbitrary-content fidelity.

24 benchmark tests pass (`build/nonrepeating-payload-tests.log`), including
same-length duplicated/reordered 4KiB blocks rejected by the real readback SHA
gate. Payload bytes are never printed. No hardware trial was performed with
this new pattern yet; all physical logs below used the older repeating data.

## Outbound investigation tooling — 2026-09-22

Resource-lifetime exclusion audit: `CrossPointWebServer::handleClient()` calls
stream service, then synchronous HTTP handling, then WS loop and Live Studio
tick. `resumeListener()` is called from that tick, not from an independent
task. Consequently the current code cannot start a suspended WS listener in
the middle of a blocked download handler. `UploadStreamServer::reset()` ends
focus after file/socket cleanup; `TransferFocus` then requires five quiet
seconds, and listener resumption additionally requires16KiB free heap.
This excludes mid-handler listener restart as a direct explanation from the
inspected execution order, not every optional-service memory interaction.
The five-second policy is still a heuristic across HTTP requests, not an
explicit upload/commit/readback transaction lease. No new firmware correction
or installation is justified by this exclusion alone.

Optional `--download-receive-buffer N` (literal IPv4, 1024..65536) now sets
SO_RCVBUF on only the readback socket before connect, retaining urllib HTTP
handling. It logs both requested and OS-reported effective values; neither is
a packet-capture measurement of the advertised TCP window. Default behavior,
upload, system network settings and reader firmware are unchanged. Invalid
parameters fail before device access. 22 tests pass, including socket ordering,
failure cleanup and a real loopback HTTP/SHA test. The test server bypasses
unrelated reverse-DNS lookup. Evidence: `build/receive-buffer-tests.log`.

One frozen-baseline 16 KiB trial requested4096 but macOS reported effective35900,
so it did **not** establish a 4 KiB receive-window condition. Upload/CRC/commit
passed, but its maximum ACK wait was11.0383s before the receive-buffer change
was even applied. Readback declared16384, returned10240 matching-prefix bytes,
then EOF at10.8533s; cleanup passed. Log:
`build/frozen-baseline-rcvbuf4k-16k.log`. No firmware/reconnect/global network
changes. This does not validate or refute the small-window hypothesis. The
upload ACK delay also means stalls are not conclusively outbound-HTTP-only.
Do not promote this experimental option to the app or change OS-wide tuning
based on this run.

The 16 KiB failure log cannot identify the received prefix: Python
HTTPResponse.read(4096) may consume fewer than 4096 bytes and then throw before
returning that partial block. The host benchmark now uses read1(4096), updating
its SHA/count after each returned fragment. On failure it reports only received
and expected byte counts, read count, elapsed time and exception type, then
rethrows the original error. It does not log filenames, device identity or file
contents, and it retains the exact downloaded-size/SHA acceptance gate.
18 benchmark tests pass, including a three-byte prefix followed by timeout and
one-byte fragmented successful delivery. No hardware request was made for this
tooling correction; old missing byte counts remain unknown.

Outbound lifecycle inspection found that the current Arduino WebServer releases
its ordinary HTTP client after the handler; NetworkClient.clear() discards RX
data, not a TX drain. NetworkClient.write() can return partial progress after
bounded retries. The download handler handles partial/zero body writes, while
framework header/chunk writes do not propagate failure to the application.
These are mechanisms to inspect, not proof of the observed timeout's cause.
No SDK patch, firmware change, transport switch or installation follows from
this inspection alone.

## Frozen-baseline follow-up — 2026-09-22

The final header/prefix-instrumented 16 KiB observation again passed upload
(three ACKs, final CRC `E81722F0`) and publication. HTTP advertised 16384 bytes,
but only 6144 arrived in five reads, all matching the original prefix; then a
timeout occurred at 41.2737 s. Cleanup identity check and deletion succeeded.
Log: `build/frozen-baseline-prefix-16k.log`. No firmware/reconnect changes.
The failed prefixes are variable (previous8192, now6144), not evidence of a
fixed 8 KiB limit. The advertised full size plus matching prefix rules out
simply serving a shorter file as a complete response on this run, but does
not exclude SD read failure later in the handler. Stream receipt/publication
and outbound body completion must be kept separate. Stop identical repetitions;
further experiments need a distinct causal variable, not another image or
larger payload. Resource/backpressure hypotheses remain unproven.

One read1-instrumented 16 KiB trial again passed all three upload ACKs,
`OK 16384 E81722F0` and publication. Readback returned 8192 bytes in eight reads
then EOF after 11.7182 s, triggering size/SHA rejection (not a read timeout on
this run). Identity-checked cleanup deleted the generated test file. Evidence:
`build/frozen-baseline-read1-16k.log`. No reconnect or firmware change occurred.
The earlier failed run's partial count remains unknown; do not retroactively
assign this 8192-byte result to it.

At pinned upstream `aa994cf7bf8fb3fd0e08c7c264ae1802c818ab86`, handleDownload
already has the same partial-write/zero-write loop and client.clear/file.close
ending; our current block size is 1024 versus upstream 4096. Current framework
NetworkClient::write allows ten one-second select retries. The ~11.7-second
EOF is consistent with a write giving up, but does not prove that branch ran:
early SD read completion or other resource/TCP failure still needs exclusion.
This run did not record advertised Content-Length or prefix equality.
Host-only failure telemetry now also records a bounded numeric Content-Length
when available and whether received bytes match the expected prefix (no raw
content). 18 tests pass in `build/readback-prefix-tests.log`. No additional
hardware trial was made after adding these fields.

A subsequent single 16384-byte trial crossed four credit windows on the same
image and connection. The three intermediate ACKs and final
`OK 16384 E81722F0` passed, followed by verified publication. Upload including
setup took 1.5583 s; payload/final response 0.4981 s; cumulative intermediate
credit wait 0.02719 s (maximum 0.01384 s). The trial then timed out inside
`verify_download` reading the HTTP response body. Downloaded SHA verification
did not finish; end-to-end acceptance **failed**. Exact partial readback byte
count was not recorded and must not be inferred from the traceback.

The cleanup finally-block successfully rechecked identity and deleted only
the generated published benchmark file. No reader reconnect/reset, further
trial or firmware installation was performed. Log:
`build/frozen-baseline-plain-16k.log`. This is direct evidence of successful
multi-window upload alongside unsuccessful HTTP readback, not a general
upload failure or a permanently unreachable device.

The failed readback uses `handleDownload()` in `CrossPointWebServer.cpp`,
which sets Content-Length and sends 1024-byte blocks, unlike the earlier
chunked statistics endpoint. Therefore a missing HTTP chunk terminator alone
cannot explain both failures. The code checks partial/zero body writes, but
does not report the completed byte count to this client. The next causal
investigation should examine outbound HTTP/body delivery, not replace the
working stream receiver or reinstall firmware based on this result.

The next distinct test omitted the failing optional developer-stats endpoint.
One 1024-byte stream trial on the same installed `w998f2f9d` passed:
`RESUME 0`, final `OK 1024 B70B4C26`, verified publication, downloaded SHA256
`785b0751fc2c53dc14a4ce3d800e69ef9ce1009eb327ccf458afe09c242c26c9`,
same-reader/version/uptime checks, and deletion of its uniquely named inert
benchmark file. Log: ignored `build/frozen-baseline-plain-1k.log`.
No firmware, connection-mode change or user action was involved.

Upload including setup was 1.1141 s; the payload/final-reply interval was
0.0262 s, commit 1.4534 s, readback 1.3595 s. Do not quote payload-only speed as
end-to-end throughput. Reported heap was 9280→10360 B and RSSI -59→-60 dBm;
these are observations, not a causal comparison with the stats-enabled trial.
The payload fits below the 4096-byte credit window (`credit_count:0`), so this
does not verify intermediate ACK pacing, large-file reliability or resume.
It does establish that failure of the optional diagnostic response is not
equivalent to failure of the ordinary small-file transfer path.

A single read-only status request subsequently succeeded on installed
`w998f2f9d` in 0.118802 s (uptime1838, heap9100, RSSI-45), without any firmware
or connection-mode change. Thus the previous failures are intermittent, not
proof the device remains unreachable.

One instrumented 1024-byte stream trial was then attempted with repeat=1 and
10 ms pacing. Both initial status and run_trial identity/version status checks
passed; it failed in `read_reader_stats()` while Python waited for another HTTP
chunk-size line. See ignored `build/frozen-baseline-1k.log`. The baseline metrics
read occurs before generating a staging name or calling upload_once, so **no
port-82 payload, staging file, publication or delete was attempted**. This is
an incomplete diagnostic response, not a failed 1 KiB SD transfer. No retry,
restart, firmware upload or user intervention followed.

Source audit: `PocketEndpoints.cpp` transfer-stats sends three bounded JSON
sections and explicitly calls `sendContent("")`. Installed Arduino WebServer
`sendContent` emits the zero-length chunk terminator; there is no source
evidence for a simply omitted terminator. Its chunked path performs separate
size/body/footer writes and ignores write results, but the observation alone
does not identify which write/network/resource failed. Do not install another
instrumentation image on this evidence. The diagnostic endpoint must not be
treated as proof about the unexercised file-transfer path.

## Instrumented image installed; control-plane failures — 2026-09-22

After the user-installed SD bootstrap and the same Join a Network workflow,
status confirmed the same X3 running `w998f2f9d`: uptime 84 seconds, free heap
12824, RSSI -72, and push/port81 advertised. This confirms installation, not
transport acceptance or a causal heap improvement over another uptime/profile.

The first HTTP/1.1 transfer-stats request delivered only 189 JSON bytes before
a 12-second timeout. A subsequent connect failed. One HTTP/1.0 request completed
and reported attempt=0/outcome=0/all transfer counters zero; saved in ignored
`build/stats-http10-response.json`. This single success is not proof that HTTP
version is the cause: a later HTTP/1.0 status connection also timed out.
Two 1 KiB benchmark invocations failed at initial status lookup before any
upload (`build/installed-stats-1k*.log`), so there is no upload timing to attribute.
Three ICMP probes received no replies; later HTTP/1.1 status also failed to connect.
No other local TCP client to the reader was visible via lsof, and Python reported
no configured proxies. These checks do not exclude other LAN clients or locate
radio/router/driver/resource failure. They do show failures before file payload
or SD writes; do not call this an SD-throughput diagnosis.

No hotspot, network-mode switch, further card swap, flash request or new payload
was performed during these observations. The user explicitly requests keeping
the current shared-network connection and minimizing physical intervention.

Implementation plan: companion `docs/IMPLEMENTATION_PLAN.md`, P0/P1. This is
measurement tooling and preliminary evidence, not transport selection/sign-off.

## Reproduction

Run one client at a time while the reader is in File Transfer → Join a Network.
No reader USB, hotspot, or flash is needed. Example (substitute current LAN IP):

```sh
python3 scripts/benchmark_transfer.py --host READER_IP --transport stream --bytes 1024 --delay-ms 10
python3 scripts/benchmark_transfer.py --host READER_IP --transport http --bytes 1024 --delay-ms 10
```

Both paths send the same deterministic payload to unique hidden staging, use
the existing size/CRC-checked commit, download the published inert `.bin` and
compare SHA-256, then delete only that generated published file. No input file
is accepted, no `/update.bin` is used and no flash endpoint is called. An
ambiguous commit is not retried or automatically deleted. Staging/target paths
are printed for inspection. Identity is checked again before cleanup.

Increase to 262144 then about 6 MB only after small trials pass. `--repeat 3`
repeats completed trials, not failed requests. Current HTTP here is the modified
fork implementation, not a pure official-firmware control. HTTP has no SD-credit
ACK per window; applying the same 512-byte/10 ms sender pacing does not make
the internal paths identical. The driver configuration stays unchanged.

JSON reports exclude the device ID. Stream elapsed time starts after RESUME;
HTTP elapsed time starts after TCP connect. `upload_including_setup_seconds`
includes each path's setup. Sending time includes intentional pacing; credit
wait includes reader scheduling/network/SD, not isolated SD time. Commit and
download times are separate. Firmware-side fixed counters are still needed to
attribute these times. No concurrent diagnostics requests run during upload.

## Initial physical observations

Reader: X3, exact firmware `1.6.6-dev-main-b8e38e39-sta-recovery-w67fff974`.
The reader was still running the verified developer installation when work
resumed (uptime 1619 s). No new firmware was installed for these measurements.

| Trial | Observation |
| --- | --- |
| Stream, 1024 B, 10 ms | Upload/CRC/commit/download SHA passed; trial file deleted. Payload/final-ACK interval 0.0288 s; sending+pacing 0.0253 s; final reply 0.00345 s. No intermediate credits for this size. |
| HTTP, 1024 B, first attempt | TCP connection timed out before request transmission. No publication was attempted. |
| HTTP, 1024 B, second attempt | Upload/CRC/commit/download SHA passed; trial file deleted. Post-connect upload interval 0.6998 s; sending+pacing 0.0252 s; final reply 0.6743 s. |
| Stream, 262144 B, two attempts | Status preflight timed out; payload transfer did not start. These are not 256 KiB throughput results. |

The first two successful trials predated adding separate setup/commit/download
timers. Their elapsed fields must not be presented as end-to-end duration.
RSSI and heap varied. Neither a winner nor the cause of slow bulk throughput
can be inferred from these small samples. Intermittent control-plane connection
failure is a new measurement target even without a large transfer in progress.

Local checks: 17 uploader tests and 8 benchmark tests pass, including corrupt/
short/long readback, staging-name restrictions, ambiguous-commit non-deletion,
and refusing cleanup on an identity change. No firmware C++ or Swift source
was changed in this measurement increment.

Next: isolate connection establishment/control-plane servicing, add bounded
reader-side counters, then repeat the same-file comparison. Keep the existing
successful firmware image and recovery path until a successor is verified.

## Follow-up — 2026-09-22

Same installed X3 firmware, same LAN/location, no flash or network switch.
Initial status reported uptime 4482 s, RSSI -59 dBm and free heap 9980 B.

- First 256 KiB stream trial started with `RESUME 0` but received the device's
  `ERROR Upload timed out`. Publication was not attempted. A subsequent single
  status connection timed out. Its unique hidden staging may remain; it was
  not treated as a published file or deleted speculatively.
- Second 256 KiB stream trial, same 10 ms/512 B pacing, received CRC-matching
  `OK 262144 C790BFF6` after 58.490 s. Of 63 credit waits, four waits accounted
  for 50.513 s (10.954, 13.327, 10.625 and 15.607 s). Total credit wait was
  51.771 s. This shows intermittent long stalls, not uniform 4.4 KiB/s service.
- Publication then succeeded, but HTTP download timed out during body reading.
  The published inert test file was deleted after identity-checked cleanup.
  SHA readback and full-trial success were **not** achieved.
- A subsequent 256 KiB HTTP-upload comparison with the same pacing failed in
  `sendall` with `BrokenPipeError`; commit was not reached. See
  `build/bench-http-256k-current.log`. This is a failed transfer, not a measured
  HTTP throughput result. Its hidden staging is unconfirmed and may remain.

Local logs: `build/bench-stream-256k-current.log` and
`build/bench-stream-256k-progress.log`. The timing alone cannot distinguish
radio/TCP, device scheduling or SD stalls. Download failure means the observed
problem is not confined to upload completion; it does not by itself identify a
shared root cause. Do not select a transport or raise timeouts on this evidence.

The uploader now accepts a local progress observer, separating submitted bytes
from validated ACK/OK offsets. The benchmark records those observations without
extra reader requests, plus phase markers and an upload summary before later
verification can fail. Host tests pin timeout/corrupt-reply non-confirmation;
20 uploader and 8 benchmark tests pass. No wire or Swift contract changed.

## Bounded reader instrumentation — 2026-09-22

Developer builds (`ENABLE_DEV_REMOTE_FLASH`) expose the last stream attempt at
`GET /api/pocket/v1/dev/transfer-stats`. Do not request it during an upload:
HTTP is deliberately deferred while receiving. It returns JSON in three chunks
through a 224-byte scratch buffer, without building a JSON document. The stream
owns one <=96-byte metrics record, reset on the next accepted stream connection;
there is no ring buffer, periodic log, task or request during upload. The route
registration itself still has WebServer overhead. Production builds omit both
the record and route. This does not instrument HTTP uploads or downloads.

Schema 1 fields:

- `attempt`: server-lifetime, saturating attempt counter (not a persistent ID).
- `outcome`: 0 none, 1 active, 2 SD-complete, 3 interrupted, 4 rejected, 5 stopped.
  SD-complete is not proof the client received OK, commit or installation.
- `expected`, `resumed`, `socketBytes`, `accepted`: bytes. Socket bytes exclude
  the header and resumed prefix; accepted is the final SD/CRC prefix for this
  attempt, not a power-loss-durable journal or a claim a rejected file survives.
- `elapsedMs`: accept-to-terminal time, including setup/close/reply writes.
- `maxServiceGapMs`: maximum interval between service entries (including the
  preceding call's work). `maxReceiveGapMs`: maximum interval between payload
  reads, including first/terminal intervals. These include sender pacing, SD
  work and scheduling; they are not isolated radio-latency measurements.
- `sdWriteUs`, `sdCloseUs`, `replyWriteUs`: `[total, maximum, calls]`, microseconds.
  Reply timing measures the local write call, not remote delivery. All totals
  saturate at UINT32_MAX; individual durations handle one clock wrap.
- `minHeap`, `minBlock`: sampled at accept and after each SD write, not a
  continuous or DMA-specific watermark.

Interpret jointly: large receive gaps with short service/write intervals narrow
the next investigation toward socket/radio/sender waiting; long SD write/close
times implicate storage work; long service gaps unexplained by those operations
require scheduler/other-loop-work investigation. None alone proves a root cause.
Compare instrumented and uninstrumented trials before attributing improvements.

Host tests cover terminal-record retention, new-attempt reset, byte accounting,
clock wrap, saturation and maximum-width JSON bounds. Hardware results must name
the exact installed image; the old `w67fff974` does not expose this endpoint.

Once that endpoint is installed, opt in with:

```sh
python3 scripts/benchmark_transfer.py --host READER_IP --transport stream --bytes 262144 --delay-ms 10 --reader-stats
```

The option reads a baseline before upload and one record after the upload socket
has closed, including after a failed upload. It requires the next attempt number,
matching expected size and a terminal outcome, plus same-reader/version and an
uptime check. No requests run concurrently with payload transmission. Saturated
attempt counters and active baseline records stop the trial before writing.
One client at a time remains mandatory; attempt/size are not cryptographic IDs.
Metric responses are capped at 2048 bytes and only validated schema fields are
reported. Unavailable/stale/malformed diagnostics are not silently attributed to
the trial and do not replace its original upload exception. Upload duration ends
before these optional observations, so their time is not counted as upload time.

All benchmark paths now recheck identity/version immediately before publication,
independently of optional diagnostics. This adds one post-upload control request;
do not treat earlier trial end-to-end timings as identical protocols. No commit
is issued when that identity/version check fails. Local tests: 16 benchmark and
20 uploader cases pass (`build/reader-stats-python-tests.log`). These host tests
do not establish physical availability of the new reader endpoint.

### Passive host TCP evidence during developer image delivery

While the upload to the still-installed `w67fff974` was running, macOS `nettop`
observed only the owned uploader process, without reader requests. Three samples
five seconds apart are in ignored `build/transfer-metrics-host-tcp.csv`:

| Sample | bytes in | bytes out | retransmitted bytes |
| --- | ---: | ---: | ---: |
| 1 | 331 | 534645 | 404508 |
| 2 | 342 | 548781 | 413524 |
| 3 | 342 | 566013 | 430756 |

Between samples 2 and 3, outgoing and retransmitted counts both increased by
17232 bytes while incoming bytes did not increase. This is direct evidence of
TCP retransmission during a stall, not just an application ACK wait. It cannot
locate the loss or distinguish radio loss, receiver resource starvation, delayed
ACKs or scheduling. The socket still existed; this is not an installation or
whole-transfer success claim. Packet capture was unavailable due to local BPF
permissions; no capture or elevated-permission workaround was performed.

### Delivery attempt terminated — 2026-09-22

The instrumented 6,034,384-byte image did not finish uploading. Attempt 1 timed
out connecting, attempt 2 returned `ERROR Upload timed out`, and attempt 3
resumed at 6,044 bytes before a socket timeout. The last reported SD-acknowledged
offset was 788,380 bytes, not the terminal retained prefix (progress is sampled).
The uploader process is gone and its log ends with the error and staging-resume
hint (`build/transfer-metrics-deploy.log`). No commit/flash success was reported.
After termination, a single status request confirmed the same X3 still running
`w67fff974`, uptime 6931 seconds, RSSI -57 and free heap 8700 bytes. The metrics
endpoint therefore remains unavailable; the newer image is not installed.
Do not repeat the same large delivery as if it were still running or promote
this run as a successful benchmark. Device-counter collection requires a
successful bootstrap first; SD remains the user-approved alternative when
physically available. The staging file was not deleted or claimed durable.
