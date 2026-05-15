#ifndef FLASH_SELFTEST_H
#define FLASH_SELFTEST_H

/* Destructive ONLY to initially blank sector 11 (0x080E0000..0x080FFFFF).
 * Never call automatically at startup; user explicitly invokes from boot mode.
 */
void Flash_RunSelfTest(void (*log)(const char *));

#endif
