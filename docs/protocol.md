# UART bootloader protocol v1

Status (step 8): framing, C parser, binary USART2 transport, GET_INFO dispatcher,
one-response sequence cache and Python serial client implemented. Native tests pass;
physical UART GET_INFO verification is pending. Firmware update commands/state machine
are not implemented. Bootloader version: 0.1.0.

## Transport and packet layout

USART2, PA2 TX / PA3 RX, 115200 baud, 8 data bits, no parity, 1 stop bit, no flow control.
Host uses stop-and-wait: one outstanding request, one matching response.
All multibyte integers use little-endian. Do not transmit a C struct directly.

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 2 | Start bytes A5 5A (SOF) |
| 2 | 1 | Protocol version: 01 |
| 3 | 1 | Command; response uses request command OR 80 hex |
| 4 | 2 | Sequence number |
| 6 | 2 | Payload length N, 0..260 |
| 8 | N | Payload |
| 8+N | 4 | CRC32, little-endian |

Total size = 12+N bytes, maximum 272 bytes. A WRITE_CHUNK payload contains
a 4-byte application-relative offset plus at most 256 bytes of image data.
There is no escaping: a SOF sequence inside a correctly framed payload is ordinary data.

## CRC

CRC-32/ISO-HDLC: polynomial 04C11DB7, reflected polynomial EDB88320,
initial register FFFFFFFF, reflect input/output, final XOR FFFFFFFF.
Coverage: packet bytes [2, 8+N), i.e. version, command, sequence, length and payload.
SOF and CRC itself are excluded. Check vector: ASCII 123456789 -> CBF43926.

Protocol_Crc32(previous, data, size) takes/returns finalized CRC values. Use zero
for previous on the first chunk. Its incremental API matches Python binascii.crc32.
CRC detects corruption; it does not authenticate commands or firmware.

## Commands and payload contracts

GET_INFO is implemented. Other commands remain reserved contracts; they currently
return BAD_COMMAND without flash operations.
Target ID for this project: 04070001 hex (a project constant, not STM32 DBGMCU device ID).
Semantic version = three uint16 values: major, minor, patch.

| Code | Command | Request payload |
| --- | --- | --- |
| 01 | GET_INFO | Empty |
| 02 | BEGIN_UPDATE | 20 bytes: target_id u32; version major/minor/patch u16 each; reserved u16=0; image_size u32; image_crc32 u32 |
| 03 | WRITE_CHUNK | offset u32; 1..256 image bytes |
| 04 | END_UPDATE | Empty |
| 05 | GET_STATUS | Empty |
| 06 | ABORT | Empty |
| 07 | REBOOT | Empty |

BEGIN_UPDATE rejects wrong target, zero/oversized images and nonzero reserved fields
before erasing. A later update layer must invalidate persistent metadata before
erasing application sectors. Protocol framing by itself never erases flash.
WRITE_CHUNK offset is image-relative, must equal the expected next byte offset,
and must be word aligned. A non-final chunk length must be a multiple of four;
only the final chunk can require FF padding. CRC/image_size exclude padding.

Every response repeats the request sequence and starts with a one-byte status.
On nonzero status, v1 response payload consists of that status byte only.
On success, command-specific payloads after the status are:

| Response | Additional success data |
| --- | --- |
| GET_INFO / 81 | target_id u32; bootloader version 3*u16; app_base u32; app_capacity u32; max_chunk u16; app_validation u8 |
| BEGIN_UPDATE / 82 | None; reply only after erase/prepare completes |
| WRITE_CHUNK / 83 | next_expected_offset u32 |
| END_UPDATE / 84 | None; reply only after final verification and commit |
| GET_STATUS / 85 | update_state u8; received_size u32; declared_size u32; last_hal_error u32 |
| ABORT / 86 | None |
| REBOOT / 87 | None; transmit complete response before resetting |

app_validation: 0=no usable vectors, 1=vectors sane only, 2=metadata+CRC verified.
Step 8 must not report state 2 before image validation is implemented.
update_state: 0=IDLE, 1=PREPARING, 2=RECEIVING, 3=VERIFYING, 4=COMMITTED, 5=ERROR.
ABORT ends an active transfer and leaves its partial image invalid. With no active
transfer it must not erase/invalidate an existing committed image.

Status codes: 0 OK, 1 BAD_COMMAND, 2 BAD_PAYLOAD, 3 BAD_STATE, 4 RANGE,
5 SEQUENCE, 6 FLASH, 7 IMAGE_CRC, 8 TARGET, 9 INCOMPLETE, 10 VECTOR.
Host treats unmatched sequence/command and invalid response lengths as unrelated or
malformed responses; they do not acknowledge an outstanding request.

## Parsing and stream recovery (implemented)

- Fixed 272-byte buffer, no dynamic allocation. Bytewise and block input accepted.
- Search for A5 5A, read the header, check length BEFORE copying payload.
- Deliver a frame to the callback only after complete reception, CRC and version checks.
- Unknown commands are framing-valid; the future dispatcher will reject them.
- Oversized length / bad CRC: count the error and search retained bytes for another SOF.
- Unsupported version with a valid CRC: drop the complete frame, count version error.
- At least 100 ms without another byte: discard partial frame and count timeout.
- Call Protocol_Expire periodically even when no UART bytes arrive.
- CRC/length/version failures produce no command and no wire response. A corrupt header
  cannot be trusted for a NACK sequence number. The host retries after its timeout.
- A corrupted but plausible length can temporarily swallow a following frame. Recovery
  may require the receive-gap timeout followed by a clean retransmission.
- HAL_GetTick rollover is handled with unsigned subtraction. Poll well within a full
  32-bit tick wrap. Parser callback data is temporary; copy anything retained.
- The parser is single-threaded/non-reentrant. Never use blocking flash work directly
  in an ISR callback. Step 8 feeds the parser from the main loop.

Protocol_Encode returns zero for insufficient buffer, oversized payload or NULL
arguments. Python decode accepts exactly one complete frame; fwtool.ResponseReader handles
serial stream buffering, resynchronization and 100 ms partial-response expiry. CRC functions require a valid pointer for nonzero length.

## Sequence, retries and side effects

The sequence cache and GET_INFO retries are implemented. Update/reboot behavior below
remains a contract for later steps.

- After board reset, host starts sequence 1; subsequent new requests increment modulo
  65536. Restarting the host session requires resetting into bootloader in v1.
- Keep exactly one request outstanding. Never send image data before BEGIN_UPDATE ACK.
- Dispatcher accepts the next expected sequence, executes once, then caches the full
  request and response. Completed semantic error responses also consume a sequence.
- Identical retransmission of the immediately previous request returns cached response
  with NO new erase, write or commit. Compare full request, not just its CRC.
- Reused sequence with changed command/payload, or unexpected sequence, yields SEQUENCE
  without executing or advancing the sequence. Parser itself does not deduplicate.
- Identical BEGIN_UPDATE retries must never trigger another erase. While a long operation
  is in progress the host waits; final response is cached when it completes.
- Host initial timeout budget: 500 ms ordinary commands, 45 s BEGIN_UPDATE,
  5 s END_UPDATE; at most three retransmissions of an identical request.
  These are initial budgets to validate by board measurements, not timing guarantees.
- On retry exhaustion, stop and report uncertain outcome. Do not silently begin erasing
  again. Reset/reconnect and explicitly restart the image transfer when appropriate.
- A REBOOT reply can be lost after the board has already reset. Treat a serial timeout
  here as an uncertain reset result, not automatic evidence of a failed firmware write.

## UART integration boundary

main.c now uses exclusive binary bootloader mode. Plain boot logs and the `t`
diagnostic entry point are removed; flash self-test source remains available.
USER held at the 750 ms check keeps the bootloader active until reset. Without USER,
sane application vectors still cause automatic handoff.
USART SR/DR are polled in the main loop; RX errors discard the partial parser state.
HAL configures USART and transmits responses. No RX IRQ/DMA is used. Idle parser
expiry runs continuously. The host must use stop-and-wait, never pipeline requests
while a blocking response is being sent. App text remains a separate mode.

## Validation

2026-09-24: native C tests compiled with -O2 -Wall -Wextra -Werror passed.
Python unittest: 8 test methods passed, including:
- 275 CRC vectors compared between C and Python binascii.
- 261 frame sizes (payload 0..260) roundtripped Python -> C parser/encoder -> Python.
- Every split point of empty and maximum frames; byte-at-a-time input.
- Noise, embedded SOF in payload, back-to-back frames and duplicate sequences.
- Single-bit corruption, oversized length, unsupported version with correct CRC.
- Missing-byte recovery, plausible corrupt-length timeout, and tick rollover.
- Output capacity and parser buffer boundary checks.

Native tests compile protocol.c directly, without HAL mocks. They do not validate UART
electrical behavior, interrupt timing, command handlers or flash updates.

Build/run from repository root, with MinGW GCC and Python on PATH:

```powershell
New-Item -ItemType Directory -Path tests/host/build -Force | Out-Null
gcc -std=c11 -O2 -Wall -Wextra -Werror -I firmware/bootloader/Core/Inc tests/host/test_protocol.c firmware/bootloader/Core/Src/protocol.c -o tests/host/build/test_protocol.exe
python tests/host/test_protocol.py
```

## Step 8 validation (2026-09-24)

- Native boot_commands.c test: exact 22-byte info payload, duplicate cached response,
  changed duplicate/unexpected sequence rejection, malformed payload, unsupported
  commands, invalid vectors and sequence wrap passed.
- Python discovery: all 15 protocol/transport tests passed. Transport tests include
  split responses, noise/bad CRC/oversized length, timeout recovery, wrong command or
  sequence, malformed reply lengths, identical retry, error status and exhaustion.
- ARM Debug build passed; vector table remains 08000000; flash footprint 12972 bytes.
- On-board GET_INFO test: pending user programming/reset/run. Native fake-port tests
  do not establish electrical UART performance.

Client dependency: tools/requirements.txt (pyserial 3.5). No dependency is needed
for codec/transport unit tests. Host Python execution was blocked in the agent
environment; tests ran with the bundled Python runtime. User installs pyserial
in their own Python using the commands in step8-get-info.md.
