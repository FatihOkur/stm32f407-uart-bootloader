#include "flash_selftest.h"
#include "flash_if.h"
#include "stm32f4xx_hal.h"
#include <stdio.h>

#define TEST_OFFSET (FLASH_APP_SIZE - FLASH_APP_SECTOR_SIZE)

static uint32_t hash_flash(uint32_t address, uint32_t length)
{
    const volatile uint8_t *bytes = (const volatile uint8_t *)address;
    uint32_t hash = 2166136261UL;
    for (uint32_t i = 0U; i < length; i++) hash = (hash ^ bytes[i]) * 16777619UL;
    return hash;
}

static uint32_t expect(void (*log)(const char *), const char *label,
                       FlashIfStatus actual, FlashIfStatus expected)
{
    char line[140];
    (void)snprintf(line, sizeof(line), "TEST | %s | %s (got=%s expected=%s hal=0x%08lx)\r\n",
                   actual == expected ? "PASS" : "FAIL", label,
                   Flash_StatusName(actual), Flash_StatusName(expected),
                   (unsigned long)Flash_LastHalError());
    log(line);
    return actual == expected;
}

void Flash_RunSelfTest(void (*log)(const char *))
{
    uint8_t pattern[17] = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
                           0x88,0x99,0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x42};
    uint8_t tail[7] = {0xE1,0xE2,0xE3,0xE4,0xE5,0xE6,0xE7};
    uint32_t passed = 1U;
    log("TEST | sector 11 only; blank check before any erase\r\n");
    if (Flash_AppIsErased(TEST_OFFSET, FLASH_APP_SECTOR_SIZE) != FLASH_IF_OK) {
        log("TEST | ABORT: sector 11 is not blank; nothing erased\r\n");
        return;
    }

    /* Evidence of unintended writes outside scratch sector; not image CRC. */
    uint32_t protected_before = hash_flash(0x08000000UL, 0x00020000UL);
    uint32_t app_before = hash_flash(FLASH_APP_BASE, TEST_OFFSET);
    passed &= expect(log, "reject absolute boot address",
                     Flash_AppWrite(0x08000000UL, pattern, 4U), FLASH_IF_RANGE);
    passed &= expect(log, "reject overflowing range",
                     Flash_AppWrite(0xFFFFFFFCUL, pattern, 8U), FLASH_IF_RANGE);
    passed &= expect(log, "reject write past app end",
                     Flash_AppWrite(FLASH_APP_SIZE - 4U, pattern, 8U), FLASH_IF_RANGE);
    passed &= expect(log, "reject unaligned write",
                     Flash_AppWrite(TEST_OFFSET + 1U, pattern, 4U), FLASH_IF_ALIGNMENT);
    passed &= expect(log, "reject null data",
                     Flash_AppWrite(TEST_OFFSET, NULL, 4U), FLASH_IF_ARGUMENT);
    passed &= expect(log, "reject zero length",
                     Flash_AppWrite(TEST_OFFSET, pattern, 0U), FLASH_IF_ARGUMENT);
    passed &= expect(log, "reject unaligned erase",
                     Flash_AppErase(TEST_OFFSET + 4U, 4U), FLASH_IF_ALIGNMENT);
    passed &= expect(log, "reject oversized erase",
                     Flash_AppErase(0U, FLASH_APP_SIZE + 1U), FLASH_IF_RANGE);
    if (passed == 0U) {
        log("TEST | FAIL: argument checks; stopping before valid erase\r\n");
        return;
    }

    if (!expect(log, "erase sector 11",
                Flash_AppErase(TEST_OFFSET, FLASH_APP_SECTOR_SIZE), FLASH_IF_OK)) {
        log("TEST | FAIL: erase failed; stop and inspect\r\n");
        return;
    }
    passed &= expect(log, "write 17 bytes and pad to 20",
                     Flash_AppWrite(TEST_OFFSET, pattern, sizeof(pattern)), FLASH_IF_OK);
    passed &= expect(log, "read back pattern",
                     Flash_AppVerify(TEST_OFFSET, pattern, sizeof(pattern)), FLASH_IF_OK);
    passed &= expect(log, "padding is FF",
                     Flash_AppIsErased(TEST_OFFSET + 17U, 3U), FLASH_IF_OK);
    passed &= expect(log, "reject non-erased destination",
                     Flash_AppWrite(TEST_OFFSET, pattern, 4U), FLASH_IF_NOT_ERASED);
    pattern[0] ^= 1U;
    passed &= expect(log, "detect data mismatch",
                     Flash_AppVerify(TEST_OFFSET, pattern, sizeof(pattern)), FLASH_IF_VERIFY_ERROR);
    passed &= expect(log, "write near upper boundary",
                     Flash_AppWrite(FLASH_APP_SIZE - 8U, tail, sizeof(tail)), FLASH_IF_OK);
    passed &= expect(log, "last flash byte remains FF",
                     Flash_AppIsErased(FLASH_APP_SIZE - 1U, 1U), FLASH_IF_OK);

    /* Return scratch sector to erased state so an intentional rerun is possible. */
    passed &= expect(log, "cleanup sector 11",
                     Flash_AppErase(TEST_OFFSET, FLASH_APP_SECTOR_SIZE), FLASH_IF_OK);
    uint32_t unchanged = protected_before == hash_flash(0x08000000UL, 0x00020000UL)
                      && app_before == hash_flash(FLASH_APP_BASE, TEST_OFFSET);
    log(unchanged ? "TEST | PASS | other flash regions unchanged (hash)\r\n"
                  : "TEST | FAIL | other flash regions changed\r\n");
    passed &= unchanged;
    uint32_t locked = (FLASH->CR & FLASH_CR_LOCK) != 0U;
    log(locked ? "TEST | PASS | flash locked\r\n" : "TEST | FAIL | flash not locked\r\n");
    passed &= locked;
    log(passed ? "TEST | ALL PASS | reset to run application\r\n"
               : "TEST | FAIL | save this log; do not proceed\r\n");
}
