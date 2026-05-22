# Step 15: host-observed transfer measurements

Firmware remains 0.4.0. No CubeProgrammer upload or analyzer is needed for these
measurements. tools/fwtransfer.py accepts --report PATH; tools/transfer_metrics.py
records timings with time.perf_counter_ns. Existing transfer behavior is unchanged
when --report is omitted.

## What is measured

- BEGIN transaction: from just before packet encoding/first write through receipt
  and decoding of its accepted response. Includes metadata erase, application
  erase/verification, protocol overhead, USB/serial/OS and Python work.
- Data phase: after BEGIN response, before the WRITE loop, through its final
  acknowledgement check/progress output. Includes framing, responses, readback,
  retries if any, Python loop work and the occasional progress print.
- END transaction: full-image verification PLUS metadata programming/readback and
  subsequent stored-image validation, including transport overhead. These subparts
  cannot be separated using host timings alone.
- Each command transaction duration, command/sequence, request payload size,
  attempts/retries and result. Duration includes all attempts; a single-attempt
  value approximates host-observed request/response latency, not pure wire RTT.
- Total session: before initial GET_INFO through final post-commit GET_INFO
  validation check. Includes preflight/status/info queries and progress printing;
  excludes local FWP file loading/validation, COM opening, physical reset, boot
  validation, final summary printing and JSON disk writing.
- Data goodput: exact unpadded application bytes / measured data phase seconds.
  Whole-update goodput uses total session seconds; result identifies committed
  versus transferred-but-not-committed sessions, so do not compare unlike modes.

All durations include host/USB/OS scheduling and board work. They are NOT MCU
execution times, interrupt jitter, worst-case guarantees or isolated flash speeds.
Commit and CRC timings are combined; isolating them needs device instrumentation
or a separate logic-analyzer measurement. The last packet is shorter, so raw WRITE
statistics mix payload lengths. JSON additionally groups full 256-byte chunks.

## Statistics and theoretical comparison

JSON stores every command record, min/mean/median/p95/max WRITE duration, a group
excluding retried transactions, and a full-chunk/no-retry group. p95 uses nearest
rank ceil(0.95*N), without interpolation. Small-sample percentiles are descriptive,
not a worst-case latency claim. Retried transactions stay in the raw/all groups
and throughput totals. --retry-test is labeled fault_injection=true in JSON and
must not be used for an ordinary performance baseline.

At 115200 baud with 8N1, each wire byte occupies 10 bits: ideal one-direction byte
rate is 11520 B/s. WRITE request is N+16 bytes; its ACK is 17 bytes. For stop-and-wait,
the data phase wire-only lower bound (no processing gaps, no retries) is therefore:

    (image_size + 33 * ceil(image_size/256)) * 10 / 115200 seconds

For this 11812-byte image: 47 chunks, 13363 combined request/ACK bytes, about 1.160 s.
This excludes BEGIN, END and queries; actual transfer is expected to be slower.
The JSON includes this calculated reference, explicitly separate from measurements.

## Hardware baseline procedure

1. Close PuTTY. Hold USER, press/release RESET, release USER after two seconds.
2. From repository root, run (artifacts directory already exists):

```powershell
py tools/fwtransfer.py --port COM8 --package artifacts/demo_app-1.0.0.fwp --commit --report artifacts/timing-01.json
```

Normal transfer/commit output is followed by MEASURE lines for BEGIN, END, data
seconds, data goodput B/s, WRITE min/mean/p95/max and total seconds/retry count.
Choose an unused report filename; timing-01.json already exists in the recorded
test artifacts. Report JSON retains the image CRC/version, bootloader version, baud,
port, host OS/Python and UTC report creation time for reproducibility.

The tool rewrites the same application with a normal verified commit. It does not
restart the board. Repeating a baseline requires a fresh USER+RESET session and a
new report filename (e.g. timing-02.json, timing-03.json). Do not issue an unrelated
serial command between that reset and the transfer. Inspect the first run before
requesting repeats. Use identical package, clock configuration, UART settings and
mode across runs; label any extra load or settings changes.

Report paths are opened exclusively BEFORE serial access. Existing output files
are not overwritten; missing/unwritable parent directories fail before an erase.
After reserving a report, serial/transfer failures write result=failed with an error
and whatever command measurements exist, rather than falsely reporting success.
Interrupted processes/power loss can still leave a partial report file; inspect
the result field and parseability. Failed runs omit success goodput values.

After measurements, open PuTTY and reset WITHOUT USER to verify normal APP output.
No firmware or FWP regeneration is needed.

## Validation and status

- All 55 Python tests pass. New tests verify percentile calculation, payload versus
  wire accounting, measured duration grouping, retry count/exclusion, error capture,
  no serial access for an existing report, and a failed report on COM access error.
- Test clocks/serial peers are simulated; their values are not board measurements.
- Stage 14 hardware fault rejection, reset behavior, power cycle between chunks,
  and recovery after every scenario: PASS, confirmed by user logs/LED observations.
- Active flash erase/program brownout behavior remains untested.
- Real timing baseline and repeated-run comparison: PASS, recorded below.
- Final reset after the measurements: PASS, user confirmed APP 1.0.0 output and
  the green LED heartbeat.
- The root README documents the current system. Remote publication is separate
  from local documentation and is not established by these tests.

## Recorded hardware results

Same 11,812-byte application, CRC32 0xA90E29E5, bootloader 0.4.0, 16 MHz HSI,
115200 baud / 8N1, Windows 11, Python 3.14.3, and CP2102 adapter:

| Report | BEGIN ms | END ms | Data seconds | Data B/s | Total seconds | Retries |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| [timing-02](../artifacts/timing-02.json) | 1732.046 | 446.970 | 4.752338 | 2485.5 | 7.399328 | 6 |
| [timing-03](../artifacts/timing-03.json) | 1737.925 | 446.930 | 1.579209 | 7479.7 | 4.231537 | 0 |
| [timing-04](../artifacts/timing-04.json) | 1812.547 | 446.933 | 1.581401 | 7469.3 | 4.310532 | 0 |

All three completed with commit and validation level 2. The two retry-free runs
give an observed data-phase baseline of approximately 7.47 kB/s. The six-retry run
is retained; the underlying cause of those retries was not established.
timing-01.json records an interrupted attempt and is excluded from successful-run
comparisons. These are small-sample host observations, not worst-case guarantees.
