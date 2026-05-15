/* Host-only HAL substitute. Never add this directory to firmware includes. */
#ifndef TEST_STM32_HAL_H
#define TEST_STM32_HAL_H
#include <stdint.h>
typedef enum { HAL_OK=0, HAL_ERROR=1 } HAL_StatusTypeDef;
typedef struct { uint32_t TypeErase, Banks, Sector, NbSectors, VoltageRange; } FLASH_EraseInitTypeDef;
typedef struct { uint32_t ACR, CR; } MockFlash;
extern MockFlash mock_flash;
extern uint32_t mock_primask;
#define FLASH (&mock_flash)
#define FLASH_ACR_ICEN (1U<<9)
#define FLASH_ACR_DCEN (1U<<10)
#define FLASH_CR_LOCK (1U<<31)
#define FLASH_FLAG_EOP 1U
#define FLASH_FLAG_OPERR 2U
#define FLASH_FLAG_WRPERR 4U
#define FLASH_FLAG_PGAERR 8U
#define FLASH_FLAG_PGPERR 16U
#define FLASH_FLAG_PGSERR 32U
#define FLASH_TYPEERASE_SECTORS 0U
#define FLASH_SECTOR_5 5U
#define FLASH_VOLTAGE_RANGE_3 2U
#define FLASH_TYPEPROGRAM_WORD 2U
#define __get_PRIMASK() mock_primask
#define __disable_irq() (mock_primask=1U)
#define __set_PRIMASK(x) (mock_primask=(x))
#define __DSB() ((void)0)
#define __ISB() ((void)0)
#define __HAL_FLASH_INSTRUCTION_CACHE_DISABLE() (FLASH->ACR &= ~FLASH_ACR_ICEN)
#define __HAL_FLASH_DATA_CACHE_DISABLE() (FLASH->ACR &= ~FLASH_ACR_DCEN)
#define __HAL_FLASH_INSTRUCTION_CACHE_ENABLE() (FLASH->ACR |= FLASH_ACR_ICEN)
#define __HAL_FLASH_DATA_CACHE_ENABLE() (FLASH->ACR |= FLASH_ACR_DCEN)
#define __HAL_FLASH_INSTRUCTION_CACHE_RESET() ((void)0)
#define __HAL_FLASH_DATA_CACHE_RESET() ((void)0)
#define __HAL_FLASH_CLEAR_FLAG(x) ((void)(x))
HAL_StatusTypeDef HAL_FLASH_Unlock(void);
HAL_StatusTypeDef HAL_FLASH_Lock(void);
uint32_t HAL_FLASH_GetError(void);
HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef*,uint32_t*);
HAL_StatusTypeDef HAL_FLASH_Program(uint32_t,uint32_t,uint64_t);
#endif
