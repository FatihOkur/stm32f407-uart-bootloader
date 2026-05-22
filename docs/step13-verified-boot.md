# Step 13: verified application handoff

Bootloader 0.4.0 enables application handoff at startup. It reuses the validated
step 5 transition routine and step 12 persistent image validator. No package or
metadata format change; the application committed in step 12 remains compatible.

## Boot decision

After board/UART initialization, red LED is on and the existing 750 ms USER window
remains. USER is sampled once at the end of that window, before a potentially long
CRC computation. Holding USER forces bootloader mode, even with a valid image.
Releasing USER after entering that mode does not trigger a jump; reset is required.

Image_GetBootVectors clears its outputs and refuses boot if USER is held, metadata
is missing/corrupt/uncommitted, target/size/address is incompatible, vectors are
invalid, or CRC of actual application flash bytes differs. There is NO vector-only
fallback. Only after full validation does it return application MSP/reset vector.
The boot decision happens before UART command processing, with no intervening flash
mutation. Missing/partial updates automatically stay in binary protocol mode.

main.c invokes Boot_JumpToApp only through that decision. The raw vector sanity
helper remains exclusively for GET_INFO diagnostic value 1. Value 1 never authorizes
execution; value 2 includes metadata + fresh full-image CRC/vector checks.

The handoff deinitializes USART and clocks while HAL ticks still work, disables
interrupts/SysTick, clears NVIC and EXTI pending state, sets VTOR=08020000 and loads
application MSP in a naked assembly tail before branching to the Thumb reset entry.
Application SystemInit keeps its own VTOR offset 20000; main enables IRQs before
HAL_Init and starts TIM2. The branch tail was inspected in the built ARM ELF.

Boot mode remains silent/binary on UART. APP text belongs to the running application.
CRC verification adds time beyond the 750 ms window; it is proportional to image size.
This is integrity validation, not cryptographic secure boot or image authentication.

## Update workflow

With a committed image, ordinary reset now starts it. Enter bootloader by holding
USER, pressing/releasing RESET and keeping USER held for approximately two seconds.
Then release USER and run a serial command with PuTTY closed. Reset into USER mode
before a new host session if sequence numbers from another command would conflict.

END_UPDATE still does not jump immediately: the host receives its reply and can
read status/info. A later reset without USER performs fresh validation and starts
the application. tools/fwtransfer.py supports 0.4.0; after --commit it now prints
the reset instruction. ABORT/REBOOT protocol commands remain unimplemented.

## Hardware checks

1. Close PuTTY. Load ONLY firmware/bootloader/Debug/bootloader.elf with normal
   necessary-sector erase/download/verification. No full-chip erase: retain the
   committed sector-4 metadata and application from step 12. No app ELF or FWP
   regeneration/retransfer is required for this check.
2. Disconnect CubeProgrammer, open PuTTY COM8 / 115200 / 8N1 / no flow control.
3. Press/release RESET without USER. Expect red during boot, then green blinking
   and repeating APP 1.0.0 | running. No BOOT text banner is expected.
4. Close PuTTY. Hold USER, press/release RESET, release USER two seconds later.
   Red should remain on and green off. Then run from repository root:

```powershell
py tools/fwtool.py info --port COM8
```

Expect Bootloader 0.4.0 and Application validation: 2 - metadata + image CRC verified.
This proves USER can retain a valid application in update mode.
5. Reopen PuTTY and press RESET without USER; APP should run again.

If normal reset stays red, enter/keep bootloader mode and inspect GET_INFO. Value 1
means plausible vectors but failed/absent committed validation. Do not weaken the
boot gate. If the previous metadata/application was erased by the programmer,
retransfer the existing FWP with --commit in a fresh USER bootloader session, then
reset without USER. A raw demo_app ELF by itself does not create valid metadata.

## Validation

- Native integration tests use production metadata/CRC/boot-decision code against
  mapped flash and mock HAL. Valid committed image returns exact MSP/reset vector;
  USER overrides it and clears outputs. Fresh/missing metadata and plausible raw
  vectors without commit reject boot. Corrupted metadata/image and partial commit
  reject boot. Validity survives RAM/session reset. Null output pointers rejected.
- Strict C GET_INFO regression passes with reported version 0.4.0.
- All 39 Python tests pass, including 0.4.0 commit and correct reset instruction.
- ARM Debug build passes; vector table 08000000; flash footprint 18760 bytes.
  Image_GetBootVectors, Boot_JumpToApp and stack-free Boot_BranchToReset are linked.
- App VTOR setting and early IRQ enable were checked; application code unchanged.
- Step 12 hardware commit and post-RESET validation=2: PASS (user confirmed RESET).
- Step 13 hardware normal boot/USER override: PASS; user confirmed APP output after normal reset and GET_INFO 0.4.0 / validation 2 after USER reset.

These host tests cannot execute an ARM handoff. Negative physical image corruption
and interrupted-update recovery tests belong to step 14; do not claim those hardware
cases passed based on native tests alone.
