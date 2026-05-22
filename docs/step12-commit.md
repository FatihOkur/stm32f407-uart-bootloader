# Step 12: full-image verification and persistent commit

Bootloader 0.3.0 adds END_UPDATE and metadata in sector 4. Automatic application
handoff remains disabled; enabling verified boot is step 13. ABORT/REBOOT are not
implemented. The .fwp host package format is unchanged.

## Lifecycle and interruption handling

After validating a BEGIN request, erase ALL metadata sector 4 and verify it is FF.
Only then erase the required application sectors. If metadata erase fails, no
application erase/write is attempted. A cut during metadata erase can leave old
metadata valid, invalid or erased, but the old application has not been touched.
Once metadata erase completes, a partial replacement image cannot have a committed
record. This is a single-slot updater: there is no rollback image.

WRITE behavior is unchanged. END accepts an empty payload only in RECEIVING state.
An incomplete transfer returns INCOMPLETE and stays RECEIVING. A complete transfer
enters VERIFYING, checks initial vectors against actual image size, and computes
CRC-32/ISO-HDLC over the exact declared image bytes read from flash in 256-byte
blocks. Word padding is excluded. CRC/vector failure enters ERROR without writing
metadata. Invalid metadata must never be treated as sufficient for boot just
because application vectors look sane.

After successful image verification, write metadata bytes 0..35 (including record
CRC), read back through the flash layer, then program/read back the commit marker
at offset 36 as a separate final word. Require metadata and full-image validation
again before replying OK and entering COMMITTED. Flash errors enter ERROR.

The same END request retransmitted with the same sequence returns its cached reply
without writing metadata again. If power is lost after the marker is fully written
but before the reply, the operation can already be committed. A timeout/error is
not proof that flash did not change. After reset, GET_INFO revalidates persistent
flash independently of RAM session state. Reset loses the transport/session state;
it does not automatically resume a transfer. New BEGIN always starts from scratch.

The ordering guarantees are logical software invariants; host fault injection is
not a measurement of analog flash behavior under brownout. Physical power-cut
testing is a later step. Exact marker, metadata CRC, bounds/vectors and image CRC
must all validate. CRC protects against accidental corruption, not malicious code.

## Metadata v1 (40 bytes at 0x08010000)

All integers little-endian; sector size is 65536 bytes, only the record is programmed.

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | Magic FWM1 (0x314D5746) |
| 4 | 2 | Record format = 1 |
| 6 | 2 | Record size = 40 |
| 8 | 4 | Project target = 0x04070001 |
| 12 | 4 | Application base = 0x08020000 |
| 16 | 4 | Image size |
| 20 | 4 | Image CRC32 |
| 24 | 6 | Version major/minor/patch, three u16 |
| 30 | 2 | Reserved = 0 |
| 32 | 4 | CRC32 of bytes [0,32) |
| 36 | 4 | Commit marker CMIT (0x54494D43), programmed LAST |

flash_if has separate MetadataErase and MetadataWrite APIs. MetadataWrite accepts
only word-aligned ranges within the 40-byte record; it cannot address the boot or
application. Existing app-offset APIs retain their original bounds. Both APIs lock
flash, reset accelerator caches after mutations and verify results. The application
erase/write diagnostic counters still exclude metadata operations (unchanged wire
contract); native tests separately count actual metadata HAL word programs.

GET_INFO app_validation=2 means valid metadata AND a fresh full-image CRC/vector
check. It can report 1 for sane vectors with invalid/absent metadata; 1 never means
committed or authorized to boot. GET_STATUS update_state is the volatile current
session: after reset it returns IDLE even with a valid committed record.

## Tools and timing

--commit opts into END_UPDATE on the Python transfer tool. Without it, transfer
still ends with data received but no commit. Commit requires 0.3.0 and is refused
on older bootloaders before erasing. Normal transfers and retry tests remain
compatible with the supported older 0.2.x versions. --retry-test can be combined
with --commit; its three deliberate drops still target BEGIN/first/final WRITE.

GET_INFO now may calculate CRC over up to 896 KiB. Host budgets for GET_INFO and
END are 30 seconds rather than the older short query timeout. BEGIN remains 45 s;
ordinary commands 500 ms. These are provisional budgets, not measured worst-case
execution guarantees for maximum-size images at 16 MHz.

## Hardware procedure

1. Close PuTTY. Load the new firmware/bootloader/Debug/bootloader.elf with normal
   sector erase/download/verification using CubeProgrammer. No full-chip erase.
2. Disconnect programmer, keep ST-LINK power and CP2102 attached. Press RESET.
   USER is not needed in this intermediate firmware; red remains on.
3. From repository root:

```powershell
py tools/fwtransfer.py --port COM8 --package artifacts/demo_app-1.0.0.fwp --commit
```

BEGIN erases metadata sector 4 plus application sectors required by the image.
After normal WRITE progress, expect:

```text
TRANSFER | PASS | all chunks written and read back
END | verifying full flash image CRC and committing metadata
COMMIT | PASS | size=11812 crc32=0xA90E29E5
VALIDATION | 2 | metadata + full flash image CRC verified
BOOT | disabled until step 13
```

4. Press RESET again, then run:

```powershell
py tools/fwtool.py info --port COM8
```

Expect Bootloader 0.3.0 and Application validation: 2 - metadata + image CRC verified.
This shows validity survives RAM/session reset. APP text is NOT expected yet.
No regeneration of the existing .fwp is needed. Do not load demo_app.elf afterward:
direct debugger/programmer changes bypass metadata lifecycle.

Future CubeIDE builds: refresh the project (F5) so new image_store.c is included.
The supplied ELF was compiled and explicitly linked with the new module.

## Validation

- Strict native C tests use production flash_if/image_store/protocol/dispatcher
  with memory-mapped simulated flash and HAL fault injection.
- Verified metadata-before-app erase ordering, failed metadata erase preserving
  app, incomplete END, wrong CRC, invalid vectors, correct commit, duplicate END
  with no repeated programming, and persistent validity after clearing RAM state.
- Every single-bit change in the 40-byte record rejected; application corruption
  rejected; out-of-range size rejected even with corrected record CRC.
- Failure before each of 10 metadata word programs and partial marker rejected;
  flash stays locked. Word padding excluded. Maximum-size image validation passed.
- Metadata API bounds/alignment tested and adjacent boot/application regions
  remain unchanged by sector-4 erase. Existing native flash/command tests pass.
- All 38 Python tests pass, including commit success, old-firmware refusal before
  erase, CRC error, lost END ACK without recommit, and missing persistent validation.
- ARM Debug build passes; .isr_vector=08000000; flash footprint=17312 bytes.
- Hardware commit and reset-persistence tests: PASS; user reported validation=2 and explicitly confirmed RESET between transfer and GET_INFO.
- Step 11 hardware retry test passed: erase=1, write=47, cache hits=3, rejects=0.

Native integration test command (GCC on PATH, repository root):

```powershell
gcc -std=c11 -O2 -Wall -Wextra -Werror -I tests/host/flash_mock -I firmware/bootloader/Core/Inc tests/host/test_metadata.c firmware/bootloader/Core/Src/flash_if.c firmware/bootloader/Core/Src/image_store.c firmware/bootloader/Core/Src/protocol.c firmware/bootloader/Core/Src/boot_commands.c -o tests/host/build/test_metadata.exe
./tests/host/build/test_metadata.exe
py -m unittest discover -s tests/host -p 'test_*.py'
```
