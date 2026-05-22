# Step 10: BEGIN_UPDATE and WRITE_CHUNK

Bootloader 0.2.0 implements GET_INFO, BEGIN_UPDATE, WRITE_CHUNK and GET_STATUS.
END_UPDATE, ABORT and REBOOT still return BAD_COMMAND. There is NO commit or
on-board full-image CRC validation yet. The image CRC and version from BEGIN are
stored in RAM for the later verification layer.

## Temporary boot policy (essential)

main.c sets BOOT_ALLOW_UNVERIFIED_APP=0. This stage never jumps to the application,
even after reset/power cycling and even if all chunks were successfully written.
The linker removes the unreachable jump routine; the ARM symbol check confirmed
its absence. Do not enable vector-only boot with UART updates. A first chunk may
contain sane vectors while the rest of the image is absent. Persistent metadata
invalidation and full-image validation must replace this gate before boot is enabled.

Consequently USER is optional for this intermediate build; red LED stays on after
reset and APP logs/green blinking are not expected. GET_INFO validation=1 still
means vector sanity only, NOT boot authorization. No persistent update state is
claimed: a reset clears the session; retransfer starts from zero with a fresh erase.
Do not install an older vector-only bootloader over a partially transferred app.

This deliberately postpones persistent metadata work to the planned commit step;
the unconditional no-boot policy is what protects this intermediate stage across
resets. The final product must not rely on a RAM-only RECEIVING flag.

## Device behavior

BEGIN accepts exactly 20 bytes. Before erasing it checks reserved=0, target ID,
size 8..917504 and IDLE state. It stores version/size/CRC, enters PREPARING, erases
only the required sectors starting at sector 5, and enters RECEIVING on success.
ACK is sent after erase and erased-area verification. A new BEGIN during an active
or failed session is rejected; reset before starting over.

WRITE accepts offset u32 plus 1..256 bytes. Offset must equal received_size and be
word aligned; range arithmetic uses subtraction after bounds checks. Every nonfinal
piece must have length divisible by four. flash_if writes and reads back bytes and
FF padding before ACK; received_size advances only on success. ACK returns the next
expected offset. No writes are permitted after all declared bytes have arrived.

The previous request and response are cached in full. An identical retry returns
the cached reply without repeating erase/write. A changed duplicate or unexpected
sequence gets SEQUENCE. Completed semantic errors consume a sequence; framing
errors do not. A flash failure enters ERROR and records HAL error bits; subsequent
writes are blocked. A verification failure can have zero HAL bits: status FLASH
and ERROR state remain authoritative.

GET_STATUS returns state u8, received u32, declared u32, last_hal_error u32 after
its status byte. Fully received data remains RECEIVING until future END_UPDATE.
Boot sectors 0..3 and metadata sector 4 are untouched by UART transfer operations.

## Host tool

tools/fwtransfer.py validates the full FWP before opening the port. It checks board
target, memory map, bootloader 0.2.0, max chunk and IDLE status before BEGIN. It sends
one request at a time, checks matching response command/sequence/length and exact
acknowledged offsets, then checks final status. Up to three identical retries are
allowed; timeout is 45 s for BEGIN and 500 ms for normal commands. On error/uncertain
timeout it stops. There is no automatic restart or commit/reboot command.

## Hardware procedure

The user currently has another project on the board. To test this stage:

1. Close PuTTY. Program the newly built firmware/bootloader/Debug/bootloader.elf
   using CubeProgrammer SWD with normal necessary-sector erase and verification.
   Do not full-chip erase. The demo_app ELF is NOT needed for this test: the transfer
   itself replaces the relevant application sector(s) from the validated package.
2. Disconnect CubeProgrammer. ST-LINK USB powers the board; CP2102 USB is connected.
3. Press RESET. This firmware always remains in bootloader. No USER hold is needed.
4. From repository root run:

```powershell
py tools/fwtransfer.py --port COM8 --package artifacts/demo_app-1.0.0.fwp
```

BEGIN will erase the application sectors required by the image (sector 5 for the
11812-byte demo image), replacing any code stored there by the other project.
Expected final output:

```text
BEGIN | erasing application sectors for 11812 bytes
WRITE | 4096/11812 bytes verified
WRITE | 8192/11812 bytes verified
WRITE | 11812/11812 bytes verified
TRANSFER | PASS | all chunks written and read back
COMMIT | NOT IMPLEMENTED | image CRC not checked on board; application boot disabled
```

Reset alone starts a fresh session and still stays in bootloader. For a repeat test,
reset then rerun the command. Do not expect APP output or bypass the boot gate.
An old 0.1.0 bootloader is detected before BEGIN and rejected. No updates to .ioc or
CubeIDE version are required. Future IDE builds: refresh project (F5) so the added
boot_commands.c participates; the agent-built ELF already includes it explicitly.

## Validation, 2026-09-24

- ARM Debug build passed; vector address 08000000, flash footprint 15116 bytes.
- Native GET_INFO/sequence regression passed with strict GCC warnings.
- Native transfer-command tests passed: invalid BEGIN without erase, duplicate
  BEGIN/WRITE, changed duplicate, wrong offset/size/alignment, final short chunk,
  status counts, reset session loss, erase/program failure and cached failure.
- Python suite: all 27 tests passed, including full transfer, lost BEGIN/WRITE
  replies, incompatible board/version, busy state, flash rejection and bad ACK.
- Command tests mock flash_if; Python tests simulate a serial peer. These do not
  validate on-board timing or physical flash operations. Stage 6 already exercised
  the unchanged flash_if implementation on the board. Stage 10 hardware: PASS (user confirmed all 11812 bytes transferred and verified).

Native test commands from repository root (GCC on PATH):

```powershell
gcc -std=c11 -O2 -Wall -Wextra -Werror -I firmware/bootloader/Core/Inc tests/host/test_update.c tests/host/update_mock.c firmware/bootloader/Core/Src/boot_commands.c -o tests/host/build/test_transfer_commands.exe
./tests/host/build/test_transfer_commands.exe
gcc -std=c11 -O2 -Wall -Wextra -Werror -I firmware/bootloader/Core/Inc tests/host/test_boot_commands.c tests/host/update_mock.c firmware/bootloader/Core/Src/boot_commands.c -o tests/host/build/test_boot_commands.exe
./tests/host/build/test_boot_commands.exe
py -m unittest discover -s tests/host -p 'test_*.py'
```
