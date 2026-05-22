# Step 9: offline firmware packaging

Implemented in tools/fwpackage.py using only the Python standard library.
No serial port or board access is needed. Step 8 GET_INFO and normal application
handoff both passed on hardware according to the user's logs.

## Data flow

ELF -> GNU objcopy -> BIN -> fwpackage pack -> FWP.
ELF contains addresses and debug information. BIN is the raw flash load image,
including initialization values copied into RAM on startup. FWP adds metadata.
The FWP is a host-side container: do not flash it directly with CubeProgrammer.
A later uploader will send its metadata through BEGIN_UPDATE and only its raw
image through WRITE_CHUNK. Persistent boot metadata is a separate later step.

## Format v1

All integers are little-endian; exact package length is 36 + image_size.

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | ASCII F407 magic |
| 4 | 2 | Package format version = 1 |
| 6 | 2 | Header size = 36 |
| 8 | 4 | Load address = 0x08020000 |
| 12 | 4 | Project target ID = 0x04070001 |
| 16 | 2 | Firmware major |
| 18 | 2 | Firmware minor |
| 20 | 2 | Firmware patch |
| 22 | 2 | Reserved = 0 |
| 24 | 4 | Raw image size |
| 28 | 4 | Image CRC32 |
| 32 | 4 | Header CRC32 |
| 36 | image_size | Application bytes |

Both CRCs use CRC-32/ISO-HDLC (binascii.crc32), matching the UART protocol.
Header CRC covers [0,32); image CRC covers [36,36+image_size).
No compression, timestamps or trailing padding: identical inputs give identical
packages. Flash word padding is excluded from image size and CRC.
Package.begin_payload() yields the existing 20-byte BEGIN_UPDATE contract:
target u32, version 3*u16, reserved u16, size u32, image CRC u32.
Package format, application version, UART protocol and bootloader versions are
separate. --version labels this package; it does not edit the APP log text.

## Checks

The tool bounds file reads and checks magic/format/header size, header CRC,
target/address/reserved fields, exact image length, image CRC and vectors.
Initial MSP must be 8-byte aligned, greater than 0x20000000 and at most 0x20020000.
Reset must have the Thumb bit and point inside the image with room for at least a
16-bit instruction. CCM stack is not supported by this project. These checks do
not prove executable correctness or all link addresses. CRC is not authentication.
Existing output files are never overwritten. Choose a new name for another build.
The output directory must already exist.

## Reproduce in PowerShell

Build demo_app in CubeIDE first; then run from repository root:

```powershell
$objcopy = 'C:\ST\STM32CubeIDE_1.19.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.13.3.rel1.win32_1.0.0.202411081344\tools\bin\arm-none-eabi-objcopy.exe'
& $objcopy -O binary --gap-fill 0xFF firmware/demo_app/Debug/demo_app.elf firmware/demo_app/Debug/demo_app.bin
if ($LASTEXITCODE -ne 0) { throw 'objcopy failed' }
New-Item -ItemType Directory -Force artifacts | Out-Null
py tools/fwpackage.py pack --input firmware/demo_app/Debug/demo_app.bin --output artifacts/demo_app-1.0.0-new.fwp --version 1.0.0
py tools/fwpackage.py inspect artifacts/demo_app-1.0.0-new.fwp
```

objcopy uses flash load addresses and FF for gaps. No --pad-to is used.
Regenerate BIN after each application rebuild; size and CRC may then change.
The already created package can be checked without regenerating anything:

```powershell
py tools/fwpackage.py inspect artifacts/demo_app-1.0.0.fwp
```

## Results, 2026-09-24

- Application build checked; vector table remains 0x08020000.
- Real image: 11812 bytes, CRC32 A90E29E5. Package: 11848 bytes, version 1.0.0.
- Real package creation and subsequent inspection passed.
- All 22 Python tests passed (15 existing and 7 packaging tests).
- Tests cover every single-bit corruption of a sample, all truncations, appended
  bytes, incompatible headers with recomputed CRC, version/capacity/vector limits,
  BEGIN payload layout and existing-file preservation.
- No serial command or board programming performed in step 9.

Generated .fwp/.bin artifacts are ignored in artifacts/.gitignore; retain source
and reproduction commands in Git and publish chosen release binaries separately.
