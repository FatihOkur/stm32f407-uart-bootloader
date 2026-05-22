#include "flash_if.h"
#include "stm32f4xx_hal.h"
#include <string.h>

static uint32_t last_hal_error;

static FlashIfStatus check_range(uint32_t offset, uint32_t length)
{
    if (length == 0U) return FLASH_IF_ARGUMENT;
    /* Subtract only after checking offset: neither addition nor wraparound. */
    if ((offset >= FLASH_APP_SIZE) || (length > FLASH_APP_SIZE - offset))
        return FLASH_IF_RANGE;
    return FLASH_IF_OK;
}

/* Invalidate the F4 FLASH accelerator caches, not an M7-style core cache.
 * Register sequence is atomic with respect to interrupts; original state wins.
 */
static void sync_flash_caches(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    uint32_t enabled = FLASH->ACR & (FLASH_ACR_ICEN | FLASH_ACR_DCEN);
    __DSB();
    __HAL_FLASH_INSTRUCTION_CACHE_DISABLE();
    __HAL_FLASH_DATA_CACHE_DISABLE();
    __HAL_FLASH_INSTRUCTION_CACHE_RESET();
    __HAL_FLASH_DATA_CACHE_RESET();
    if ((enabled & FLASH_ACR_ICEN) != 0U) __HAL_FLASH_INSTRUCTION_CACHE_ENABLE();
    if ((enabled & FLASH_ACR_DCEN) != 0U) __HAL_FLASH_DATA_CACHE_ENABLE();
    __DSB();
    __ISB();
    __set_PRIMASK(primask);
}

static FlashIfStatus begin_flash(void)
{
    if (HAL_FLASH_Unlock() != HAL_OK) {
        last_hal_error = HAL_FLASH_GetError();
        (void)HAL_FLASH_Lock();
        return FLASH_IF_HAL_ERROR;
    }
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                           FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);
    return FLASH_IF_OK;
}

static FlashIfStatus finish_flash(FlashIfStatus status)
{
    if (HAL_FLASH_Lock() != HAL_OK) {
        last_hal_error |= HAL_FLASH_GetError();
        status = FLASH_IF_HAL_ERROR;
    }
    sync_flash_caches();
    return status;
}

FlashIfStatus Flash_AppIsErased(uint32_t offset, uint32_t length)
{
    FlashIfStatus status = check_range(offset, length);
    if (status != FLASH_IF_OK) return status;
    const volatile uint8_t *source = (const volatile uint8_t *)(FLASH_APP_BASE + offset);
    for (uint32_t i = 0U; i < length; i++) {
        if (source[i] != 0xFFU) return FLASH_IF_NOT_ERASED;
    }
    return FLASH_IF_OK;
}

FlashIfStatus Flash_AppVerify(uint32_t offset, const uint8_t *data, uint32_t length)
{
    if (data == NULL) return FLASH_IF_ARGUMENT;
    FlashIfStatus status = check_range(offset, length);
    if (status != FLASH_IF_OK) return status;
    const volatile uint8_t *source = (const volatile uint8_t *)(FLASH_APP_BASE + offset);
    for (uint32_t i = 0U; i < length; i++) {
        if (source[i] != data[i]) return FLASH_IF_VERIFY_ERROR;
    }
    return FLASH_IF_OK;
}

FlashIfStatus Flash_AppErase(uint32_t offset, uint32_t length)
{
    last_hal_error = 0U;
    FlashIfStatus status = check_range(offset, length);
    if (status != FLASH_IF_OK) return status;
    if ((offset % FLASH_APP_SECTOR_SIZE) != 0U) return FLASH_IF_ALIGNMENT;

    /* F407VG app comprises seven equal 128 KiB sectors, numbered 5..11. */
    uint32_t count = ((length - 1U) / FLASH_APP_SECTOR_SIZE) + 1U;
    FLASH_EraseInitTypeDef erase = {0};
    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.Sector = FLASH_SECTOR_5 + (offset / FLASH_APP_SECTOR_SIZE);
    erase.NbSectors = count;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
    uint32_t failed_sector = 0xFFFFFFFFUL;

    status = begin_flash();
    if (status != FLASH_IF_OK) return status;
    if (HAL_FLASHEx_Erase(&erase, &failed_sector) != HAL_OK ||
        failed_sector != 0xFFFFFFFFUL) {
        last_hal_error = HAL_FLASH_GetError();
        status = FLASH_IF_HAL_ERROR;
    }
    status = finish_flash(status);
    if (status != FLASH_IF_OK) return status;

    /* Verify the entire rounded erase area, not only the requested bytes. */
    return Flash_AppIsErased(offset, count * FLASH_APP_SECTOR_SIZE) == FLASH_IF_OK
           ? FLASH_IF_OK : FLASH_IF_VERIFY_ERROR;
}

FlashIfStatus Flash_AppWrite(uint32_t offset, const uint8_t *data, uint32_t length)
{
    last_hal_error = 0U;
    if (data == NULL) return FLASH_IF_ARGUMENT;
    FlashIfStatus status = check_range(offset, length);
    if (status != FLASH_IF_OK) return status;
    if ((offset & 3U) != 0U) return FLASH_IF_ALIGNMENT;

    /* length is bounded before rounding, so length + 3 cannot overflow. */
    uint32_t padded_length = (length + 3U) & ~3UL;
    status = check_range(offset, padded_length);
    if (status != FLASH_IF_OK) return status;
    status = Flash_AppIsErased(offset, padded_length);
    if (status != FLASH_IF_OK) return status;

    status = begin_flash();
    if (status != FLASH_IF_OK) return status;
    for (uint32_t pos = 0U; pos < length; pos += 4U) {
        uint32_t word = 0xFFFFFFFFUL;
        uint32_t take = length - pos;
        if (take > 4U) take = 4U;
        /* Accept byte-aligned source buffers; never dereference uint32_t*. */
        memcpy(&word, &data[pos], take);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                              FLASH_APP_BASE + offset + pos, word) != HAL_OK) {
            last_hal_error = HAL_FLASH_GetError();
            status = FLASH_IF_HAL_ERROR;
            break;
        }
    }
    status = finish_flash(status);
    if (status != FLASH_IF_OK) return status;
    status = Flash_AppVerify(offset, data, length);
    if (status != FLASH_IF_OK) return status;
    if (padded_length != length &&
        Flash_AppIsErased(offset + length, padded_length - length) != FLASH_IF_OK)
        return FLASH_IF_VERIFY_ERROR;
    return FLASH_IF_OK;
}

FlashIfStatus Flash_MetadataErase(void)
{
    last_hal_error=0U;
    FLASH_EraseInitTypeDef erase={0};
    erase.TypeErase=FLASH_TYPEERASE_SECTORS;
    erase.Sector=FLASH_SECTOR_4;
    erase.NbSectors=1U;
    erase.VoltageRange=FLASH_VOLTAGE_RANGE_3;
    uint32_t failed=0xFFFFFFFFUL;
    FlashIfStatus status=begin_flash();
    if(status!=FLASH_IF_OK) return status;
    if(HAL_FLASHEx_Erase(&erase,&failed)!=HAL_OK || failed!=0xFFFFFFFFUL) {
        last_hal_error=HAL_FLASH_GetError(); status=FLASH_IF_HAL_ERROR;
    }
    status=finish_flash(status);
    if(status!=FLASH_IF_OK) return status;
    const volatile uint8_t *p=(const volatile uint8_t *)FLASH_META_BASE;
    for(uint32_t i=0;i<FLASH_META_SIZE;i++)
        if(p[i]!=0xFFU) return FLASH_IF_VERIFY_ERROR;
    return FLASH_IF_OK;
}

FlashIfStatus Flash_MetadataWrite(uint32_t offset,const uint8_t *data,uint32_t length)
{
    last_hal_error=0U;
    if(data==NULL || length==0U) return FLASH_IF_ARGUMENT;
    if(offset>=FLASH_META_RECORD_SIZE || length>FLASH_META_RECORD_SIZE-offset)
        return FLASH_IF_RANGE;
    if((offset&3U)!=0U || (length&3U)!=0U) return FLASH_IF_ALIGNMENT;
    const volatile uint8_t *p=(const volatile uint8_t *)(FLASH_META_BASE+offset);
    for(uint32_t i=0;i<length;i++) if(p[i]!=0xFFU) return FLASH_IF_NOT_ERASED;
    FlashIfStatus status=begin_flash();
    if(status!=FLASH_IF_OK) return status;
    for(uint32_t i=0;i<length;i+=4U) {
        uint32_t word;
        memcpy(&word,data+i,4U);
        if(HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,FLASH_META_BASE+offset+i,word)!=HAL_OK) {
            last_hal_error=HAL_FLASH_GetError(); status=FLASH_IF_HAL_ERROR; break;
        }
    }
    status=finish_flash(status);
    if(status!=FLASH_IF_OK) return status;
    for(uint32_t i=0;i<length;i++) if(p[i]!=data[i]) return FLASH_IF_VERIFY_ERROR;
    return FLASH_IF_OK;
}

uint32_t Flash_LastHalError(void) { return last_hal_error; }

const char *Flash_StatusName(FlashIfStatus status)
{
    switch (status) {
    case FLASH_IF_OK: return "OK";
    case FLASH_IF_ARGUMENT: return "ARGUMENT";
    case FLASH_IF_RANGE: return "RANGE";
    case FLASH_IF_ALIGNMENT: return "ALIGNMENT";
    case FLASH_IF_NOT_ERASED: return "NOT_ERASED";
    case FLASH_IF_HAL_ERROR: return "HAL_ERROR";
    case FLASH_IF_VERIFY_ERROR: return "VERIFY_ERROR";
    default: return "UNKNOWN";
    }
}
