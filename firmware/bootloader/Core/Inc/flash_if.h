#ifndef FLASH_IF_H
#define FLASH_IF_H

#include <stdint.h>

#define FLASH_APP_BASE        0x08020000UL
#define FLASH_APP_SIZE        0x000E0000UL
#define FLASH_APP_SECTOR_SIZE 0x00020000UL

typedef enum {
    FLASH_IF_OK = 0,
    FLASH_IF_ARGUMENT,
    FLASH_IF_RANGE,
    FLASH_IF_ALIGNMENT,
    FLASH_IF_NOT_ERASED,
    FLASH_IF_HAL_ERROR,
    FLASH_IF_VERIFY_ERROR
} FlashIfStatus;

/* Blocking, main-thread-only API; caller serializes all flash operations.
 * offset is relative to FLASH_APP_BASE, never an absolute address.
 * Erase: offset must be sector aligned; length rounds UP to whole sectors.
 * Write: offset must be word aligned; final partial word pads with 0xFF.
 *        All destination words (including padding) must be erased.
 *        Only the final image chunk may have a non-word-aligned length.
 * Operations are not atomic: HAL/verify errors require abandoning the image.
 * CRC, persistent invalidation and commit belong to the later update layer.
 * Board supply assumption: VDD is 2.7--3.6 V (Discovery nominally 3.3 V).
 */
FlashIfStatus Flash_AppErase(uint32_t offset, uint32_t length);
FlashIfStatus Flash_AppWrite(uint32_t offset, const uint8_t *data, uint32_t length);
FlashIfStatus Flash_AppVerify(uint32_t offset, const uint8_t *data, uint32_t length);
FlashIfStatus Flash_AppIsErased(uint32_t offset, uint32_t length);
uint32_t Flash_LastHalError(void);
const char *Flash_StatusName(FlashIfStatus status);

#endif
