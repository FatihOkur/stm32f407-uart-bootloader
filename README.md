# STM32F407 UART Bootloader and Firmware Update System

A custom bootloader for the STM32F407 Discovery board, with a Python host tool for UART firmware updates. The bootloader validates the complete application image before committing it and again before execution. Incomplete, corrupted, or uncommitted images stay in recovery mode and can be replaced over UART.

The project covers application relocation, internal flash programming, a binary serial protocol, retry handling, persistent image metadata, application handoff, and fault-recovery testing on real hardware.

**Current versions:** bootloader `0.4.0`, demo application `1.0.0`, wire protocol `1`.

## Contents

- [Implemented features](#implemented-features)
- [Hardware and tools](#hardware-and-tools)
- [Wiring](#wiring)
- [Firmware configuration](#firmware-configuration)
- [Memory layout](#memory-layout)
- [Repository structure](#repository-structure)
- [Build and first installation](#build-and-first-installation)
- [Update and recovery workflow](#update-and-recovery-workflow)
- [Protocol and retry behavior](#protocol-and-retry-behavior)
- [Package and persistent metadata](#package-and-persistent-metadata)
- [Validation and test results](#validation-and-test-results)
- [Transfer measurements](#transfer-measurements)
- [Troubleshooting](#troubleshooting)
- [Limitations](#limitations)
- [Further documentation](#further-documentation)

## Implemented features

- Separate bootloader and application CubeIDE projects with independent vector tables.
- UART updates in chunks of up to 256 application bytes.
- Framed binary messages with CRC32, sequence numbers, bounds checks, and parser timeout handling.
- Application-relative flash access, word alignment checks, erased-area checks, and write readback.
- Full-image CRC verification and vector validation before persistent commit.
- A dedicated metadata sector with a commit marker programmed last.
- Boot-time revalidation of metadata and the complete stored image.
- USER-button entry into the bootloader, even when a valid application exists.
- Cached responses to repeated requests, avoiding repeated erase/write operations when an ACK is lost.
- Python tools for packaging, inspection, transfer, fault injection, and timing reports.
- Native C tests, Python tests, and hardware recovery checks.

This is a **single-application-slot** design. An update replaces the previous application. Recovery means uploading a valid image again; it does not mean rolling back to a retained copy.

## Hardware and tools

### Hardware used

| Component | Purpose |
| --- | --- |
| STM32F407G-DISC1 Discovery, STM32F407VGT6 | Target MCU and onboard ST-LINK programmer/debugger |
| CP2102 USB-to-TTL serial adapter, 3.3 V logic | UART connection between the PC and USART2 |
| Jumper wires | TX, RX, and common ground |
| Two PC USB connections | Discovery ST-LINK power/debug and CP2102 serial |
| Optional 24 MHz, 8-channel USB logic analyzer | UART signal inspection; not required for the update workflow |

The ESP32 is not used by this project. The onboard ST-LINK connection and the CP2102 connection serve different purposes: ST-LINK provisions/debugs the bootloader, while the CP2102 carries the custom update protocol.

### Development environment

| Tool | Version / setting used |
| --- | --- |
| STM32CubeIDE | 1.19.0 |
| STM32CubeF4 firmware package | 1.28.3 in both projects |
| STM32CubeProgrammer | 2.23.0 |
| GNU Arm toolchain bundled with CubeIDE | 13.3.rel1 |
| Python | 3.14.3 |
| pyserial | 3.5, pinned in `tools/requirements.txt` |
| Host OS | Windows 11 |
| Serial terminal | PuTTY |
| Optional analyzer software | PulseView |

The examples below use PowerShell and `COM8`. Replace the port with the one assigned to your adapter. Run commands from the repository root.

## Wiring

### Required UART connections

Power the Discovery through its **ST-LINK USB connector**. Connect the CP2102 to the PC through its own USB connector.

| CP2102 pin | Discovery MCU signal | Direction / function |
| --- | --- | --- |
| TX / TXD | **PA3 — USART2_RX** | PC transmits to the STM32 |
| RX / RXD | **PA2 — USART2_TX** | STM32 transmits to the PC |
| GND | **GND** | Common signal reference |
| 5V | **Not connected** | Discovery is powered through ST-LINK USB |
| 3V3 | **Not connected** | Not needed for this wiring arrangement |

TX and RX are crossed. Use the Discovery header labels for **PA2**, **PA3**, and **GND**; these are MCU signal names, not connector-position numbers.

The serial signals must use **3.3 V logic levels**. Leaving the adapter's power pins disconnected does not itself establish its TX logic voltage; use a known 3.3 V-compatible adapter configuration.

```text
PC USB ── ST-LINK USB ── Discovery power and SWD programming
PC USB ── CP2102
             TXD ───────── PA3 / USART2_RX
             RXD ───────── PA2 / USART2_TX
             GND ───────── GND
             5V, 3V3       not connected
```

### Onboard signals

| Signal | MCU pin | Use |
| --- | --- | --- |
| USER / B1 | PA0 | Active-high input; held during reset to stay in the bootloader |
| RESET | NRST | Restart the MCU |
| Green LED / LD4 | PD12 | Application heartbeat |
| Red LED / LD5 | PD14 | Bootloader status |
| SWDIO | PA13 | Onboard ST-LINK debug connection |
| SWCLK | PA14 | Onboard ST-LINK debug connection |

No extra wires are needed for the onboard buttons, LEDs, or ST-LINK SWD connection.

### Optional logic analyzer

For an analyzer labeled **CH1 through CH8**, the following mapping can be used:

| Analyzer connection | Signal |
| --- | --- |
| CH1 | PA2, STM32 TX |
| CH2 | PA3, STM32 RX |
| GND | Discovery GND |

Decode each line as UART, **115200 baud, 8 data bits, no parity, 1 stop bit**, idle high. Connect channels as inputs only. A powered-off analyzer is not needed for normal operation; disconnect unused probes when not capturing.

The performance results in this README come from host timestamps, **not logic-analyzer captures**.

## Firmware configuration

### Clock tree — both projects

| Setting | Value |
| --- | --- |
| Clock source | Internal HSI, 16 MHz |
| PLL | Disabled |
| SYSCLK / HCLK | 16 MHz / 16 MHz |
| AHB prescaler | 1 |
| APB1 / APB2 prescalers | 1 / 1 |
| PCLK1 / PCLK2 | 16 MHz / 16 MHz |
| TIM2 input clock | 16 MHz |
| Flash latency | 0 wait states |
| HAL timebase | SysTick, 1 ms |

These are the active generated-code settings. Unused oscillator or PLL values retained in a CubeMX configuration do not mean the firmware runs from those sources. The measurements were made at **16 MHz**, not the MCU's maximum supported clock.

### USART2 — both projects

| Setting | Value |
| --- | --- |
| Mode | Asynchronous, transmit and receive |
| TX / RX | PA2 / PA3 |
| GPIO alternate function | AF7, USART2 |
| GPIO mode | Alternate-function push-pull |
| GPIO pull | No pull |
| GPIO speed setting | Very high |
| Baud rate | 115200 |
| Data / parity / stop | 8 bits / none / 1 bit |
| Hardware flow control | None |
| Oversampling | 16 |

The bootloader polls USART2 receive status in its main loop and uses blocking HAL transmission for replies. UART DMA and UART receive interrupts are not used. The GPIO speed setting is an output configuration, not the UART baud rate.

### TIM2 — demo application only

| Setting | Value |
| --- | --- |
| Clock source | Internal clock |
| Counter mode | Up |
| Prescaler (`PSC`) | 15999 |
| Auto-reload / period (`ARR`) | 499 |
| Update interrupt | Enabled |
| NVIC TIM2 priority values | 0, 0 |
| PWM/output channels | Not used |

```text
Counter frequency = 16,000,000 / (15999 + 1) = 1,000 Hz
Update period     = (499 + 1) / 1,000       = 0.5 seconds
```

The interrupt toggles the green LED every 500 ms: a full on/off cycle takes one second. The application also prints `APP 1.0.0 | running` approximately once per second.

### GPIO and startup details

USER is read by polling PA0 during boot selection. Although the generated board configuration contains an EXTI input configuration for the button, boot selection does not rely on an EXTI interrupt.

The board preset retains some unused peripheral pin assignments. These should not be interpreted as implemented audio, USB, SPI, or other application features.

## Memory layout

The STM32F407VGT6 provides 1 MiB of internal flash. The project partitions it as follows; end addresses are inclusive.

| Region | Sectors | Start | End | Capacity |
| --- | --- | --- | --- | --- |
| Bootloader | 0–3 | `0x08000000` | `0x0800FFFF` | 64 KiB |
| Persistent image metadata | 4 | `0x08010000` | `0x0801FFFF` | 64 KiB sector; 40-byte record |
| Application | 5–11 | `0x08020000` | `0x080FFFFF` | 896 KiB / 917,504 bytes |

Sectors 0–3 are 16 KiB each, sector 4 is 64 KiB, and sectors 5–11 are 128 KiB each. Metadata has its own erase sector so its validity can be removed before changing the application.

Main SRAM is `0x20000000`–`0x2001FFFF` (128 KiB). An initial descending stack may start at `0x20020000`. The 64 KiB CCM RAM region is not used by this design, and the image validator does not accept it as the initial stack region.

### Linker and vector-table configuration

| Project | Flash origin | Flash length | `.isr_vector` | Preprocessor settings |
| --- | --- | --- | --- | --- |
| `bootloader` | `0x08000000` | 64 KiB | `0x08000000` | `USER_VECT_TAB_ADDRESS`, `VECT_TAB_OFFSET=0x00000000U` |
| `demo_app` | `0x08020000` | 896 KiB | `0x08020000` | `USER_VECT_TAB_ADDRESS`, `VECT_TAB_OFFSET=0x00020000U` |

The projects use their `STM32F407VGTX_FLASH.ld` linker scripts. The application must be linked at its actual flash address; copying an application linked at `0x08000000` to the application region is not sufficient.

The bootloader validates the stored image, shuts down its UART/clock state, disables and clears interrupt state, sets `VTOR`, loads the application's initial stack pointer, and branches to its Thumb reset handler. Interrupts remain masked across the final branch; the demo application explicitly enables interrupts before `HAL_Init()`.

After regenerating code, verify the linker configuration, vector offset definitions, and custom startup code before rebuilding.

## Repository structure

```text
firmware/
  bootloader/             Bootloader CubeIDE project and HAL/CMSIS dependencies
    Core/Src/
      main.c              Startup decision, UART loop, application handoff
      protocol.c          Framing, CRC32, streaming parser
      flash_if.c          Bounded application flash operations
      boot_commands.c     Command dispatch, update state, duplicate-request cache
      image_store.c       Persistent metadata, image validation, commit
      flash_selftest.c    Earlier flash-layer diagnostic implementation
  demo_app/               Relocated application, TIM2 heartbeat, UART status
  shared/                 Reserved for future shared firmware definitions
tools/
  protocol.py             Python wire codec
  fwtool.py               GET_INFO client
  fwpackage.py            Firmware package creation and inspection
  fwtransfer.py           Transfer, commit, retry injection, timing reports
  fwfault.py              Controlled invalid-image scenarios
  transfer_metrics.py     Host timing and statistics
  requirements.txt        Python dependency pin
tests/host/               Native C tests/mocks and Python unit tests
docs/                     Detailed protocol and implementation-stage notes
artifacts/                Generated packages and measurement reports
```

Generated `.bin` and `.fwp` artifacts are ignored in `artifacts/`; generate a package locally when reproducing the project. Build outputs under `Debug/` are not a substitute for rebuilding from source.

## Build and first installation

### 1. Import and build the firmware

1. Open STM32CubeIDE 1.19.0.
2. Use **File → Import → General → Existing Projects into Workspace** and select the repository's `firmware` directory.
3. Import both `bootloader` and `demo_app`.
4. Ensure STM32CubeF4 **1.28.3** is available. The corresponding `.ioc` files record this version.
5. Refresh each project and build its **Debug** configuration. Refresh is important after adding source files outside the IDE.
6. Check the generated map files: bootloader `.isr_vector` must be at `0x08000000`, application `.isr_vector` at `0x08020000`.

Expected ELF outputs:

```text
firmware/bootloader/Debug/bootloader.elf
firmware/demo_app/Debug/demo_app.elf
```

### 2. Prepare Python and the application package

```powershell
py -m pip install -r tools/requirements.txt
New-Item -ItemType Directory -Force artifacts | Out-Null
```

Generate the application binary from its ELF. Run the following with CubeIDE's GNU Arm toolchain `bin` directory on `PATH`, or invoke that directory's `arm-none-eabi-objcopy.exe` by its full path:

```powershell
arm-none-eabi-objcopy -O binary --gap-fill 0xFF firmware/demo_app/Debug/demo_app.elf firmware/demo_app/Debug/demo_app.bin
py tools/fwpackage.py pack --input firmware/demo_app/Debug/demo_app.bin --output artifacts/demo_app-1.0.0.fwp --version 1.0.0
py tools/fwpackage.py inspect artifacts/demo_app-1.0.0.fwp
```

The package tool validates size and vectors and includes header/image CRCs. Existing output files are not overwritten: use a new filename when generating another package.

The tested `1.0.0` build contained **11,812 image bytes**, CRC32 **`0xA90E29E5`**, and a **11,848-byte package**. These are identifiers of that particular build, not requirements for every subsequent build.

### 3. Program the bootloader through ST-LINK

1. Connect the Discovery's ST-LINK USB connector.
2. Open STM32CubeProgrammer, choose **ST-LINK / SWD**, and connect.
3. Select `firmware/bootloader/Debug/bootloader.elf` in the programming/download view.
4. Program it with verification enabled. The ELF contains the flash address.
5. Keep the board in its normal flash-boot configuration.

Full-chip erase is not part of a normal application update. If preserving an installed application, avoid erasing its sectors or the metadata sector while provisioning the bootloader.

The demo application is installed using the UART workflow below. Programming its ELF alone does not create the committed metadata required for automatic boot. A `.fwp` file is a host package, **not** a directly flashable binary.

## Update and recovery workflow

### Enter the bootloader

1. Close PuTTY and any other program using the CP2102 COM port.
2. Hold **USER**.
3. Press and release **RESET**, keeping USER held.
4. Release USER after approximately two seconds.

The firmware waits 750 ms before sampling USER. Holding it through this point keeps the device in the bootloader until the next reset. The red LED stays on and the green LED stays off.

Without USER, a valid committed image starts automatically. A missing or invalid image leaves the board in the bootloader automatically.

**Start a fresh reset session before each independent host-tool invocation.** Each tool starts its own sequence numbering at 1; the board retains its current sequence/cache state until reset. In particular, reset into the bootloader again between a standalone `info` command and a transfer.

### Inspect the device

```powershell
py tools/fwtool.py info --port COM8
```

Expected identifying fields:

```text
Target ID: 0x04070001
Bootloader: 0.4.0
Application base: 0x08020000
Application capacity: 917504 bytes (896 KiB)
Max chunk: 256 bytes
```

`0x04070001` is this project's protocol target identifier, not the STM32 silicon device-ID register.

| Validation value | Meaning | Eligible for automatic boot? |
| --- | --- | --- |
| 0 | No usable application vectors | No |
| 1 | Vectors appear sane, but committed full-image validation has not succeeded | No |
| 2 | Persistent metadata and complete image CRC verified | Yes |

### Transfer and commit

Reset into the bootloader again, then run:

```powershell
py tools/fwtransfer.py --port COM8 --package artifacts/demo_app-1.0.0.fwp --commit
```

The tool checks the local package and device, starts the update, writes consecutive chunks, and requests a commit. Successful completion includes:

```text
TRANSFER | PASS | all chunks written and read back
COMMIT | PASS | size=11812 crc32=0xA90E29E5
VALIDATION | 2 | metadata + full flash image CRC verified
BOOT | ready | press RESET without USER to verify and start application
```

`--commit` is required to make the new image bootable. Transferring all bytes without committing deliberately leaves it uncommitted.

Commit does not immediately jump into the application. After the command exits:

1. Open PuTTY with **115200 baud, 8N1, no flow control**.
2. Press RESET **without USER**.
3. Observe the green LED blinking and repeated `APP 1.0.0 | running` messages.

The current bootloader is binary-only and does not print the historical `BOOT | stage ...` text banners.

### What happens inside an update

1. **BEGIN:** check target and size, invalidate the old image by erasing/verifying the metadata sector, then erase the required application sectors.
2. **WRITE:** accept only the expected next offset, program the chunk, verify its bytes, and return the next offset.
3. **END:** require the declared byte count, validate vectors, calculate CRC over the complete flash image, and compare it with the declared CRC.
4. **Commit:** program/read back metadata, write the commit marker last, and validate the stored result before acknowledging success.
5. **Reset:** validate stored metadata and the complete image again before application handoff.

Application writes are word-aligned. A final short word is padded with `0xFF`; padding is excluded from the declared image size and image CRC. An update erases only the required application sectors; unused tail sectors may retain old contents, but they are outside the committed image.

### Recover after an interrupted or rejected update

Reset the board. If no valid committed image exists, it remains in the bootloader without needing USER. Run the original package transfer again with `--commit`, then reset without USER and check the heartbeat.

Recovery restarts the transfer from byte zero. There is no persistent resume offset and no automatic rollback.

## Protocol and retry behavior

All multi-byte integers are little-endian.

```text
SOF       Version  Command  Sequence  Payload length  Payload     CRC32
A5 5A     u8       u8       u16       u16             0..260 B    u32
```

Frame length is `12 + payload_length`, up to 272 bytes. CRC32 covers the version through the end of the payload, excluding SOF and the CRC field itself. The CRC is CRC-32/ISO-HDLC, compatible with Python `binascii.crc32`; the check value for `123456789` is `0xCBF43926`.

A response uses `request_command | 0x80`, echoes the sequence, and begins its payload with a status byte.

| ID | Command | Purpose |
| --- | --- | --- |
| `0x01` | GET_INFO | Identity, version, capacity, chunk size, image validation |
| `0x02` | BEGIN | Declare target, version, image size/CRC; prepare flash |
| `0x03` | WRITE | Application-relative offset plus up to 256 bytes |
| `0x04` | END | Validate and commit the received image |
| `0x05` | GET_STATUS | Volatile update state, byte counts, HAL error |
| `0x06` | ABORT | Reserved; not implemented |
| `0x07` | REBOOT | Reserved; not implemented |
| `0x08` | GET_DIAGNOSTICS | Erase/write call, cache-hit, and sequence-rejection counters |

Status codes are `OK=0`, `BAD_COMMAND=1`, `BAD_PAYLOAD=2`, `BAD_STATE=3`, `RANGE=4`, `SEQUENCE=5`, `FLASH=6`, `IMAGE_CRC=7`, `TARGET=8`, `INCOMPLETE=9`, and `VECTOR=10`.

The receiver uses a bounded streaming parser with a 100 ms incomplete-frame gap timeout. Malformed frames are rejected before dispatch. The host uses stop-and-wait: only one command is outstanding at a time.

The board caches the most recent accepted request and its response. Repeating that exact request returns the cached response without executing its flash operation again. Reusing its sequence with different content, or sending an unexpected sequence, is rejected. This is a one-request cache, not a general history or reconnect/resume mechanism.

Default host timeouts are 500 ms for ordinary commands, 45 seconds for BEGIN, and 30 seconds for END and GET_INFO. Up to three retries follow the initial attempt. Exhausted retries stop with an uncertain transaction outcome; the host does not silently start another erase.

Update states are IDLE, PREPARING, RECEIVING, VERIFYING, COMMITTED, and ERROR. They are volatile. After reset, session state returns to IDLE even if persistent metadata describes a valid application.

See [the protocol specification](docs/protocol.md) for complete payload layouts.

## Package and persistent metadata

### Host `.fwp` package

The package consists of a **36-byte header** followed by the raw application bytes. The header contains `F407` magic, format/header size, application base, target ID, semantic version, reserved field, image size, image CRC32, and header CRC32. The header CRC covers its first 32 bytes.

Packages contain no timestamp, so identical input bytes and version produce identical package contents. The host rejects malformed headers, wrong target/address, invalid size, bad CRCs, and invalid vectors before transferring.

### On-device metadata

The metadata sector stores a separate **40-byte record**:

| Offset | Field |
| --- | --- |
| 0 | `FWM1` magic, 4 bytes |
| 4 | Format version, `u16` |
| 6 | Record size, `u16` |
| 8 | Target ID, `u32` |
| 12 | Application base, `u32` |
| 16 | Image size, `u32` |
| 20 | Image CRC32, `u32` |
| 24 | Firmware major/minor/patch, three `u16` values |
| 30 | Reserved, `u16` |
| 32 | Metadata CRC32 over bytes 0–31 |
| 36 | `CMIT` marker, programmed last |

Before execution, validation requires the expected metadata fields and marker, metadata CRC, sane size, valid vectors bounded by the actual image, and the matching complete image CRC. A plausible vector table alone is insufficient.

Initial MSP must be 8-byte aligned and within the supported main-SRAM stack range. The reset vector must select Thumb execution and point into the declared image. These checks do not prove that arbitrary firmware will execute correctly.

If power disappears after a complete commit but before the host receives the ACK, the image may already be valid. A missing ACK alone cannot establish whether commit happened; reset and inspect the board before deciding how to recover.

## Validation and test results

### Tests performed on the Discovery board

| Scenario | Observed result |
| --- | --- |
| Valid package transfer and commit | All chunks read back; full image CRC accepted; validation 2 |
| Normal reset after commit | Application UART messages and green LED heartbeat |
| USER held during reset | Bootloader stays active despite a valid application |
| Ignore BEGIN, first WRITE, and final WRITE ACKs | Retry succeeded; one application erase call, 47 write calls, three cache hits |
| Partial image, 4096 of 11812 bytes | END rejected with `INCOMPLETE (9)`; reset did not boot it |
| Corrupted image with original expected CRC | END rejected with `IMAGE_CRC (7)`; reset did not boot it |
| Invalid vectors with matching image CRC | END rejected with `VECTOR (10)`; validation 0; reset did not boot it |
| Complete image without END/commit | Validation 1; reset did not boot it |
| Power cycle after an acknowledged partial transfer | Incomplete image stayed blocked after power returned |
| Upload original valid package after each fault | Commit succeeded; application ran after reset |

The power-cycle check was performed **between acknowledged chunks**. Cutting power during an active flash erase/program operation has not been validated. These results do not establish arbitrary brownout tolerance.

The earlier flash-layer diagnostic also passed range/alignment rejection, erased-destination checks, write/readback, padding, boundary, cleanup, region-preservation, and flash-lock checks. Its old ASCII `t` trigger is not part of the current binary bootloader interface.

### Reproduce controlled fault tests

These commands intentionally replace the installed application with an invalid or uncommitted image. Keep a known-good package available and run **one scenario at a time**, starting each with a fresh bootloader reset:

```powershell
py tools/fwfault.py partial --port COM8 --package artifacts/demo_app-1.0.0.fwp
py tools/fwfault.py bad-crc --port COM8 --package artifacts/demo_app-1.0.0.fwp
py tools/fwfault.py bad-vector --port COM8 --package artifacts/demo_app-1.0.0.fwp
py tools/fwfault.py uncommitted --port COM8 --package artifacts/demo_app-1.0.0.fwp
```

After each scenario, reset without USER and confirm that the application does not run. Then reset for a fresh session, upload the original package with `fwtransfer.py --commit`, and verify normal boot. The package file itself is not modified by fault injection.

For the repeat-request test, start a fresh bootloader session and run:

```powershell
py tools/fwtransfer.py --port COM8 --package artifacts/demo_app-1.0.0.fwp --retry-test --commit
```

This deliberately ignores selected ACKs at the host. The injected BEGIN timeout can take 45 seconds. It tests protocol retry behavior rather than injecting electrical UART noise.

### Host-side tests

The recorded development validation includes **55 passing Python tests**, plus native C checks of parser, flash, command, update, and metadata behavior. These tests use simulated peers and flash/HAL mocks; they complement hardware tests.

The Python suite includes a bridge to the actual C protocol implementation. Build it first with a **native Windows GCC**, not `arm-none-eabi-gcc`:

```powershell
New-Item -ItemType Directory -Force tests/host/build | Out-Null
gcc -std=c11 -O2 -Wall -Wextra -Werror -I firmware/bootloader/Core/Inc tests/host/test_protocol.c firmware/bootloader/Core/Src/protocol.c -o tests/host/build/test_protocol.exe
py -m unittest discover -s tests/host -p "test_*.py"
```

For the native metadata/flash/update integration test:

```powershell
gcc -std=c11 -O2 -Wall -Wextra -Werror -I tests/host/flash_mock -I firmware/bootloader/Core/Inc tests/host/test_metadata.c firmware/bootloader/Core/Src/flash_if.c firmware/bootloader/Core/Src/image_store.c firmware/bootloader/Core/Src/protocol.c firmware/bootloader/Core/Src/boot_commands.c -o tests/host/build/test_metadata.exe
.\tests\host\build\test_metadata.exe
```

The flash-address mocks use Windows virtual memory APIs; those native tests are not currently a portable Linux test harness.

## Transfer measurements

Measurements used the same **11,812-byte** image, 47 WRITE chunks, UART 115200 8N1, HSI 16 MHz, and the Debug firmware build. Timings are measured by Python on the host and include USB, OS scheduling, UART, protocol processing, and board work.

| Report | BEGIN prepare/erase | END verify/commit | Data phase | Data goodput | Total session | Retries |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| [timing-02](artifacts/timing-02.json) | 1732.046 ms | 446.970 ms | 4.752338 s | 2485.5 B/s | 7.399328 s | 6 |
| [timing-03](artifacts/timing-03.json) | 1737.925 ms | 446.930 ms | 1.579209 s | 7479.7 B/s | 4.231537 s | 0 |
| [timing-04](artifacts/timing-04.json) | 1812.547 ms | 446.933 ms | 1.581401 s | 7469.3 B/s | 4.310532 s | 0 |

The two retry-free runs achieved approximately **7.47 kB/s of application data** during the data phase. The slower successful run is retained because six retries materially affected throughput; their underlying cause has not been established. `timing-01.json` records an interrupted attempt and is excluded from successful-run comparisons.

Data goodput is image bytes divided by data-phase duration, excluding BEGIN and END. Total time also includes the tool's status/info transactions. END timing combines CRC verification, metadata commit/readback, and transport; it is not an isolated CRC benchmark.

For comparison, a WRITE request carries `N + 16` wire bytes and its ACK carries 17. With stop-and-wait and 8N1:

```text
Ideal data-phase wire time = (image_size + 33 * ceil(image_size / 256)) * 10 / 115200
                          = approximately 1.160 seconds for this image
```

That reference excludes device processing and USB/host gaps. Host-observed latencies are not MCU execution times, interrupt jitter measurements, or worst-case guarantees. WRITE statistics also include a shorter final chunk; JSON reports provide separate full-chunk groups.

To record another run, reset into the bootloader and choose a **new** report filename:

```powershell
py tools/fwtransfer.py --port COM8 --package artifacts/demo_app-1.0.0.fwp --commit --report artifacts/timing-05.json
```

Do not use `--retry-test` for an ordinary performance baseline. Verify application startup after measurements. See [measurement methodology](docs/step15-measurements.md) for timing boundaries and statistics.

## Troubleshooting

| Symptom | Check / action |
| --- | --- |
| COM port cannot be opened | Close PuTTY and other serial programs; check the assigned port in Device Manager. |
| Host receives no response | Check crossed TX/RX, common ground, UART settings, and USER/reset entry. The running application does not implement the update protocol. |
| `SEQUENCE (5)` after using another tool | Reset into the bootloader before starting a new standalone tool session. |
| Red LED on, green off | Expected while in the bootloader. Inspect validation using a fresh session; invalid/uncommitted images intentionally stay here. |
| Validation is 1 after all bytes were sent | Byte transfer alone does not commit an image. Use the normal transfer with `--commit`. |
| Application ELF programmed directly but no automatic boot | A raw application does not establish persistent commit metadata. Install the package with `--commit`. |
| `File exists` for a report or package | Choose a new output filename. Exclusive creation prevents overwriting an existing artifact. Report creation fails before serial access. |
| No readable bootloader banner in PuTTY | Expected: current bootloader traffic is binary. Use the Python tools. |
| Old `BOOT | stage 6` text appears | Check the bootloader build/output selected for programming; that is an earlier diagnostic image. |
| `.map` file opens with “application not found” | Open it with a text editor or CubeIDE's text editor; it is a text file. |
| Added C source is not linked | Refresh the CubeIDE project, rebuild, and inspect its generated build inputs. |
| `arm-none-eabi-objcopy` not found | Use the full path to CubeIDE's bundled executable or add its directory to PATH. |

## Limitations

- Single-slot replacement: no A/B images, rollback, or preservation of the previous application during update.
- No persistent resume after reset; recovery restarts the transfer.
- CRC detects accidental corruption but does not authenticate firmware. There are no signatures, encryption, secure boot, or anti-rollback enforcement.
- Firmware version is recorded, not used to forbid downgrades.
- Flash bounds are enforced by this firmware's software layer; this is not a claim of option-byte write protection or isolation from arbitrary application code.
- Active erase/program brownout testing and exhaustive power-cut timing coverage remain future work.
- No implemented ABORT or REBOOT command; reset is performed with the board button.
- No RTOS, UART DMA, interrupt-driven receive, or logic-analyzer timing evidence in the current implementation.
- A valid CRC and plausible vectors establish structural/integrity checks, not functional correctness of the uploaded application.

## Further documentation

- [Binary protocol](docs/protocol.md)
- [Flash access layer and original self-test](docs/step6-flash.md)
- [GET_INFO implementation](docs/step8-get-info.md)
- [Firmware packaging](docs/step9-firmware-package.md)
- [Chunk transfer](docs/step10-transfer.md)
- [Retry handling](docs/step11-retries.md)
- [Persistent commit](docs/step12-commit.md)
- [Verified boot and handoff](docs/step13-verified-boot.md)
- [Fault injection and recovery](docs/step14-fault-recovery.md)
- [Measurement methodology and results](docs/step15-measurements.md)

The numbered notes describe development milestones and may show older intermediate behavior. This README describes the current `0.4.0` bootloader workflow.
