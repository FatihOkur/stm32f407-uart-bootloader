# Step 14: incomplete/corrupt image rejection and recovery

Firmware remains 0.4.0; no firmware rebuild/programming is needed. The test helper
is tools/fwfault.py. It validates the source FWP locally, checks target, memory map,
bootloader version and fresh IDLE state, then uses the ordinary update protocol to
replace application data with a deliberately incomplete/invalid/uncommitted image.
It never modifies the source package, bootloader sectors or option bytes. BEGIN
still erases metadata sector 4 and required application sectors as designed.

## Cases

| Case | Injected condition | Required END response | State | Validation |
| --- | --- | --- | --- | --- |
| partial | Send only first 4096 bytes, then attempt END | INCOMPLETE (9) | RECEIVING=2 | 1 |
| bad-crc | Flip final application byte, announce original image CRC | IMAGE_CRC (7) | ERROR=5 | 1 |
| bad-vector | Set initial MSP to zero, announce matching modified image CRC | VECTOR (10) | ERROR=5 | 0 |
| uncommitted | Send all original bytes but do not send END | Not sent | RECEIVING=2 | 1 |

All transport frames have valid CRCs. bad-crc specifically tests the on-board
full-image check, not packet parsing or local FWP rejection. bad-vector recomputes
image CRC to isolate vector rejection. Value 1 means plausible vectors, not a
committed image and never authorization to boot. Original FWP remains usable for
recovery. A malformed local FWP is rejected before opening the serial port.

The helper checks exact error status (not any exception), final counts/state,
validation and erase/write API counts. A timeout, wrong error or unexpected ACK
fails the test. Fault setup PASS does not assert reset/power-cycle behavior; the
user must observe that on hardware before recording an end-to-end PASS.

## Initial hardware check: partial image

Close PuTTY. Hold USER, press/release RESET and release USER after two seconds.
This starts a fresh bootloader session even if a valid image was running. From
repository root:

```powershell
py tools/fwfault.py partial --port COM8 --package artifacts/demo_app-1.0.0.fwp
```

Expected:

```text
FAULT | partial | replacing application; original package unchanged
BEGIN | OK | metadata invalidated and application area prepared
WRITE | 4096/11812 bytes acknowledged
END | expected rejection | INCOMPLETE (9)
VALIDATION | 1 | not committed; must not boot
FAULT SETUP | PASS | reset/power-cycle behavior still requires hardware observation
RECOVERY | reset, then upload the original package with fwtransfer.py --commit
```

Then press RESET WITHOUT USER. The red LED should stay on and green stay off.
The card should remain accessible without USER:

```powershell
py tools/fwtool.py info --port COM8
```

Expected bootloader 0.4.0, validation 1, not 2. PuTTY should show no NEW APP output
after that reset; do not mistake text from an earlier session for new output.
No application boot is expected until recovery.

## Recovery after each case

Press RESET to start a fresh sequence/session (invalid app stays in bootloader).
Close PuTTY and run:

```powershell
py tools/fwtransfer.py --port COM8 --package artifacts/demo_app-1.0.0.fwp --commit
```

Expect COMMIT PASS and validation 2. Open PuTTY, press RESET without USER and
confirm APP 1.0.0 | running and green blinking. No CubeProgrammer is needed for
recovery. Do not reinstall the application ELF manually: it bypasses metadata.

## Remaining cases (perform one at a time after instructions)

Start each with a fresh USER+RESET bootloader session; do not issue unrelated
serial commands before the test. Substitute bad-crc, bad-vector or uncommitted
for partial in the command. After each, reset without USER and confirm boot is
blocked, then recover with the original package and confirm application operation.

## Power-cycle check at a known incomplete boundary

After a partial setup finishes and closes COM8, power-cycle the incomplete image.
To actually remove board power without another signal connection supplying it:
disconnect CP2102 TX/RX leads (and any attached analyzer signal probes), then remove
ST-LINK USB. Remove other board power sources if any; LEDs must go off. Reconnect
ST-LINK power and CP2102 USB as needed before restoring TX/RX to the same PA3/PA2
connections. Keep power pins unconnected and common ground as before. Do not hold
USER while powering back up. Confirm red/no app and GET_INFO validation 1, then
recover with the original package. Recheck COM number if USB re-enumerates.

This checks persistence of an incomplete image across real power loss BETWEEN
acknowledged chunks. It is not a brownout test during a flash word program/sector
erase. Active-operation power cuts and timing coverage are not claimed by this
controlled test and must be recorded separately if performed.

## Evidence and status

- Step 13 hardware normal reset -> APP and USER override -> validation 2: PASS.
- All 48 Python tests pass. New cases check partial boundary, actual modified bytes,
  matching CRC with invalid vectors, absent END, wrong ACK/rejection, timeouts,
  parameter bounds before I/O and old-firmware rejection before erase.
- Native integration tests pass with production flash/metadata/boot decision code.
  Added recovery from CRC rejection and interrupted transfer using only session
  reset + normal BEGIN/WRITE/END, without manually clearing simulated flash.
- Physical partial/reset/recovery: PASS (INCOMPLETE 9, validation 1, no boot, recovered APP).
- Physical bad-crc/reset/recovery: PASS (IMAGE_CRC 7, validation 1, no boot, recovered APP).
- Physical bad-vector/reset/recovery: PASS (VECTOR 10, validation 0, no boot, recovered APP).
- Physical full-image-without-commit/reset/recovery: PASS (validation 1, no boot, recovered APP).
- Incomplete-image power cycle/recovery: PASS. User explicitly confirmed ST-LINK power removal/all LEDs off, restart without USER, validation 1, then successful commit and running APP/green LED.

No board mutation was performed by the agent while preparing these tools. Physical
results above were recorded from user observations. The
agreed step-14 cases and final recovery are complete; active-operation power cuts remain outside tested coverage.
