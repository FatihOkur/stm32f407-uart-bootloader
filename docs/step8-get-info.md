# Step 8: GET_INFO over UART

Only information reading is implemented. No request in this stage erases or writes flash.
The previous step 6 hardware flash tests passed before this change.

## Files

- main.c: binary RX loop, parser callback, reply transmission; same USER/reset handoff.
- boot_commands.h/c: HAL-independent GET_INFO and sequence/duplicate cache.
- tools/fwtool.py: serial info client, response stream parser and timeout/retry handling.
- tools/requirements.txt: pyserial dependency.
- tests/host/test_boot_commands.c and test_fwtool.py: dispatcher/transport tests.

## Board procedure

1. Close PuTTY (COM8 must be free).
2. Program firmware/bootloader/Debug/bootloader.elf using CubeProgrammer SWD.
   Use normal sector erase/download with verification, NOT full-chip erase.
   The ELF was built by the agent. demo_app does not need rebuilding/reprogramming.
3. Disconnect CubeProgrammer. Keep ST-LINK USB supplying power, connect CP2102 USB.
4. Hold USER, press/release RESET, keep USER pressed for 2 seconds, then release.
   Red LED remains lit, green LED remains off. No BOOT text is expected anymore.
5. Open PowerShell in repository root and run:

```powershell
py -m pip install -r tools/requirements.txt
py tools/fwtool.py info --port COM8
```

Expected:
```text
GET_INFO | OK
Target ID: 0x04070001
Bootloader: 0.1.0
Application base: 0x08020000
Application capacity: 917504 bytes (896 KiB)
Max chunk: 256 bytes
Application validation: 1 - vectors sane only (image CRC not checked)
```

Target ID is a project constant. Validation 1 checks vector plausibility only.
The client sends sequence 1; each CLI invocation starts a new host session. Reset
into bootloader before starting a new session. Within a call, up to three identical
retransmissions follow 500 ms response timeouts. No pyserial install is needed again.

After GET_INFO, optionally open PuTTY (115200 8N1, no flow control), press RESET
without USER and confirm APP 1.0.0 output/green LED. There is no BOOT banner.

If serial access is denied, close PuTTY/other serial monitor. If no reply, confirm
the new ELF was downloaded and reset into USER bootloader mode. Do not send `t`.

## Future CubeIDE builds

Refresh bootloader in Project Explorer (F5) before building so the newly added
Core/Src/boot_commands.c is included by the generated makefiles. The agent built
this ELF with the existing make pattern and explicitly linked that extra object;
generated makefiles were not edited.

Hardware GET_INFO and reset-to-application passed, confirmed by user logs. Step 8 is complete.
