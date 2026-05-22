# Step 11: lost acknowledgement and retry verification

Bootloader 0.2.1 adds GET_DIAGNOSTICS (08, response 88); all previous command
layouts stay unchanged. Request payload is empty. Success payload is status byte
plus four little-endian u32 values: erase_calls, write_calls, cache_hits,
sequence_rejects (17 bytes total). Nonempty requests return BAD_PAYLOAD.

erase_calls/write_calls increment immediately before calls to Flash_AppErase and
Flash_AppWrite, including failed calls. They count operations at the flash API
boundary, NOT sectors or individual programmed words. cache_hits increments when
the exact last request is replayed; sequence_rejects counts changed duplicate or
unexpected-sequence rejection. Counters are volatile, initialized to zero at reset,
and use unsigned 32-bit arithmetic. A duplicate diagnostic request returns its
cached snapshot; use a new sequence to read current counters.

## Failure injection

tools/fwtransfer.py --retry-test performs the normal transfer but ignores these
valid successful replies after the board has executed their commands:

1. BEGIN_UPDATE ACK (normal 45-second timeout).
2. First WRITE_CHUNK ACK (normal 500 ms timeout).
3. Final WRITE_CHUNK ACK (normal 500 ms timeout).

For a one-chunk image, first/final refer to the same ACK and are ignored once.
No wires need disconnecting. This simulates a lost reply at the host acceptance
boundary; it does not simulate electrical noise or power loss. The ordinary Client
timeout path retransmits the identical encoded request/sequence, up to three retries.
The injection hook is used only when --retry-test is selected. Flash code is not
mocked or faulted on the board. Error responses are never deliberately ignored.

The tool reads initial and final diagnostic snapshots and requires exactly one
erase call, ceil(image_size/256) write calls, at least one cache hit per injected
loss, and no sequence rejects. Every scheduled injection must have occurred.
Additional genuine lost replies may produce more cache hits; repeated flash calls
still fail the test. It also checks final received/declared size and HAL status.
Counters verify the dispatcher's API call count; they are not an independent
electrical measurement of the flash controller.

The client now preserves its sequence on a SEQUENCE error because that rejection
does not advance the board's expected sequence. It stops and reports the error;
it does not attempt an automatic session repair. Other completed semantic errors
consume a sequence as before. Exhausted retries stop with an uncertain outcome;
no automatic BEGIN/re-erase is issued.

## Hardware procedure

1. Close PuTTY and program the NEW firmware/bootloader/Debug/bootloader.elf through
   CubeProgrammer SWD with normal sector erase and verification. Do not full erase.
2. Disconnect CubeProgrammer, keep ST-LINK power and CP2102 USB connected.
3. Press RESET to start a fresh session/counters. USER is not required: automatic
   application boot remains disabled until full-image validation/commit exists.
4. From repository root run:

```powershell
py tools/fwtransfer.py --port COM8 --package artifacts/demo_app-1.0.0.fwp --retry-test
```

This test erases/writes the application region again. For the 11812-byte demo image,
expected summary (assuming no additional reply loss) is:

```text
DIAG | erase_calls=1 write_calls=47 cache_hits=3 sequence_rejects=0
RETRY TEST | PASS | 3 ACKs ignored; no repeated erase/write
TRANSFER | PASS | all chunks written and read back
COMMIT | NOT IMPLEMENTED | image CRC not checked on board; application boot disabled
```

INJECT messages and ordinary WRITE progress appear before this summary. The long
pause after ignoring BEGIN is intentional (45 seconds); do not reset or interrupt.
Reset before each separate run. A 0.2.0 board is rejected before any erase in test
mode. Normal transfers remain compatible with 0.2.0 and 0.2.1.

## Validation

Native C command tests pass with -O2 -Wall -Wextra -Werror. They verify counter
values against mock flash calls, duplicate BEGIN/WRITE, changed duplicate errors,
diagnostic snapshot caching, invalid diagnostic payload and counter reset. Existing
range, alignment, session, flash-failure and sequence-wrap tests still pass.

All 33 Python tests pass. New cases cover injected BEGIN/first/final ACK loss,
single-chunk images, refusal to test older firmware, incorrect diagnostic counts,
exactly four identical attempts on exhaustion, and SEQUENCE error preservation.
Tests use simulated time/serial peers; on-board retry test PASSED: user output confirms erase_calls=1, write_calls=47, cache_hits=3 and sequence_rejects=0.

The preceding step 10 hardware test PASSED: all 11812 bytes were acknowledged
after write/readback verification, as confirmed by the user's serial-tool output.
Full-image on-board CRC, commit and application boot remain later steps.
