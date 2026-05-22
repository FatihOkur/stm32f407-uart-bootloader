# Step 6: application flash access

## Implementation

- `firmware/bootloader/Core/Inc/flash_if.h`: public contract and application region.
- `firmware/bootloader/Core/Src/flash_if.c`: blocking erase/program/readback operations.
- `flash_selftest.h/.c`: explicitly requested scratch-sector diagnostic.
- `main.c`: press USER during reset to remain in bootloader, then send `t` over UART.

The app region is 0x08020000..0x080FFFFF, sectors 5..11. All public offsets are relative to 0x08020000. An absolute address is rejected as out of range. Zero lengths are rejected. Erase offset must align to 128 KiB; erase length rounds up to entire sectors. Write offset must align to four bytes; an incomplete final word pads with 0xFF. Every destination word must be erased before writing, including padding. Non-multiple-of-four lengths are for the final image chunk only; duplicate-packet handling belongs to the later protocol layer.

The implementation assumes 2.7--3.6 V VDD and uses 32-bit programming and voltage range 3. It preserves the original FLASH accelerator cache enable state while invalidating stale data/instructions. Unlock/program/erase paths relock flash on completion or HAL error. Operations are synchronous, non-reentrant and called from main, with the HAL tick running. Single-bank flash operations can stall execution; UART senders must wait for completion before transmitting more data.

There is no CRC, metadata invalidation or commit logic yet. Before any real update erases an existing application, the later update layer must invalidate persistent metadata first. A HAL failure may leave a partially erased/programmed region; do not mark such an image valid. This API does not provide rollback or atomic updates.

## Board test

1. Refresh/rebuild bootloader in CubeIDE if desired. Program only `firmware/bootloader/Debug/bootloader.elf` using CubeProgrammer; verify download. Do not use mass erase. The existing demo app stays in sectors beginning at 5.
2. Disconnect the ST-LINK programming session. Open PuTTY COM8, 115200 8N1, flow control None.
3. Hold USER, press and release RESET, keep USER held approximately two seconds, then release.
4. Wait for `BOOT | diagnostic mode: send t to test blank sector 11; reset to exit`.
5. Send one `t`. If PuTTY buffers local lines, press Enter once. Extra CR/LF is ignored.
6. Expected final line: `TEST | ALL PASS | reset to run application`.
7. Press RESET without USER: app UART messages and green timer LED must return.

Only sector 11, 0x080E0000..0x080FFFFF, is erased/programmed by the valid test operations. An initial full-sector blank check aborts before erasing if any byte is occupied. Do not run this test once an application uses sector 11. The final cleanup erases the scratch data so the test can be intentionally rerun; the test uses two erase cycles. A reset/power failure during the diagnostic can leave scratch nonblank; save the log and inspect instead of bypassing the blank check.

19 checks cover malformed ranges, wraparound, alignment, null/empty data, scratch erase, write/readback, padding, overwrite rejection, deliberate mismatch detection, the upper flash boundary, cleanup, hashes of unchanged non-scratch areas, and the flash lock. Hash comparison is diagnostic evidence, not a cryptographic guarantee.

## Host regression

`tests/host/test_flash.c` compiles the production flash implementation against `tests/host/flash_mock/stm32f4xx_hal.h` on Windows. It maps simulated flash at the MCU addresses and exercises the same board self-test plus sector rounding, maximum capacity, preflight behavior, HAL erase/program failures, readback corruption and cache/interrupt state preservation. The mock cannot validate electrical behavior, actual flash cache coherency or timing.

Example from repository root, with MinGW GCC on PATH:

```powershell
New-Item -ItemType Directory -Path tests/host/build -Force | Out-Null
gcc -std=c11 -Wall -Wextra -Werror -I tests/host/flash_mock -I firmware/bootloader/Core/Inc tests/host/test_flash.c firmware/bootloader/Core/Src/flash_if.c firmware/bootloader/Core/Src/flash_selftest.c -o tests/host/build/test_flash.exe
./tests/host/build/test_flash.exe
```

Never add the mock HAL directory to firmware include paths. Host test executables and build output should not be committed.

## Validation status

- Host regression: PASS (MinGW, actual flash_if.c compiled).
- STM32 Debug build: PASS; flash footprint about 19 KiB, within 64 KiB. New modules are linked into bootloader.elf. Refresh the project in CubeIDE to discover the new source files before the next IDE build.
- Board erase/program/readback: PASS. User supplied all 19 PASS lines and ALL PASS; reset then returned to APP 1.0.0 UART output. Initial scratch-sector blank check had correctly aborted on four zero bytes at 0x080FFFFC. After a full backup and ELF comparison, sector 11 was erased; full readback showed exactly those four bytes changed, and every byte outside sector 11 unchanged.