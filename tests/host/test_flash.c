/* Windows host regression: executes production flash_if.c against a mock HAL.
 * Models address limits, erase geometry, lock and injected failures, not timing.
 */
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "stm32f4xx_hal.h"
#include "flash_if.h"
#include "flash_selftest.h"

MockFlash mock_flash;
uint32_t mock_primask;
static unsigned unlock_calls, erase_calls, program_calls;
static uint32_t erase_sector, erase_count;
static int fail_erase, fail_program_on, corrupt_program, selftest_pass;

HAL_StatusTypeDef HAL_FLASH_Unlock(void)
{ unlock_calls++; FLASH->CR &= ~FLASH_CR_LOCK; return HAL_OK; }
HAL_StatusTypeDef HAL_FLASH_Lock(void)
{ FLASH->CR |= FLASH_CR_LOCK; return HAL_OK; }
uint32_t HAL_FLASH_GetError(void) { return 0x20U; }
HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *erase,uint32_t *failed)
{
    assert((FLASH->CR & FLASH_CR_LOCK)==0U);
    assert(erase->TypeErase==FLASH_TYPEERASE_SECTORS);
    assert(erase->VoltageRange==FLASH_VOLTAGE_RANGE_3);
    assert(erase->Sector>=5U && erase->Sector<=11U);
    assert(erase->NbSectors>=1U && erase->NbSectors<=12U-erase->Sector);
    erase_calls++; erase_sector=erase->Sector; erase_count=erase->NbSectors;
    if (fail_erase) { *failed=erase->Sector; return HAL_ERROR; }
    memset((void*)(uintptr_t)(FLASH_APP_BASE+(erase->Sector-5U)*FLASH_APP_SECTOR_SIZE),
            0xFF, erase->NbSectors*FLASH_APP_SECTOR_SIZE);
    *failed=0xFFFFFFFFU;
    return HAL_OK;
}
HAL_StatusTypeDef HAL_FLASH_Program(uint32_t type,uint32_t address,uint64_t data)
{
    assert((FLASH->CR & FLASH_CR_LOCK)==0U);
    assert(type==FLASH_TYPEPROGRAM_WORD && (address&3U)==0U);
    assert(address>=FLASH_APP_BASE && address<=FLASH_APP_BASE+FLASH_APP_SIZE-4U);
    program_calls++;
    if (fail_program_on && program_calls==(unsigned)fail_program_on) return HAL_ERROR;
    assert(*(uint32_t*)(uintptr_t)address==0xFFFFFFFFU);
    *(uint32_t*)(uintptr_t)address=(uint32_t)data;
    if (corrupt_program) *(uint32_t*)(uintptr_t)address ^= 1U;
    return HAL_OK;
}
static void logger(const char *message)
{
    fputs(message,stdout);
    assert(strstr(message,"FAIL")==NULL);
    if (strstr(message,"ALL PASS")) selftest_pass=1;
}
int main(void)
{
    void *memory=VirtualAlloc((void*)0x08000000UL,0x100000U,
                              MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    assert(memory==(void*)0x08000000UL);
    memset(memory,0xFF,0x100000U);
    memset(memory,0x35,0x20000U); /* boot + metadata must never be erased */
    FLASH->CR=FLASH_CR_LOCK;
    FLASH->ACR=FLASH_ACR_ICEN|FLASH_ACR_DCEN;
    Flash_RunSelfTest(logger);
    assert(selftest_pass);

    uint8_t data[12]={1,2,3,4,5,6,7,8,9,10,11,12};
    unsigned before=unlock_calls;
    assert(Flash_AppErase(0U,0U)==FLASH_IF_ARGUMENT);
    assert(Flash_AppErase(0U,0xFFFFFFFFU)==FLASH_IF_RANGE);
    assert(Flash_AppWrite(FLASH_APP_SIZE,data,1U)==FLASH_IF_RANGE);
    assert(Flash_AppVerify(FLASH_APP_SIZE-1U,data,2U)==FLASH_IF_RANGE);
    assert(Flash_AppWrite(0U,data,0xFFFFFFFFU)==FLASH_IF_RANGE);
    assert(unlock_calls==before);
    assert(Flash_AppErase(0U,1U)==FLASH_IF_OK);
    assert(erase_sector==5U && erase_count==1U);
    assert(Flash_AppErase(0U,FLASH_APP_SECTOR_SIZE+1U)==FLASH_IF_OK);
    assert(erase_sector==5U && erase_count==2U);
    assert(Flash_AppErase(0U,FLASH_APP_SIZE)==FLASH_IF_OK);
    assert(erase_sector==5U && erase_count==7U);
    assert(*(uint8_t*)0x08000000UL==0x35);
    assert(*(uint8_t*)0x08010000UL==0x35);

    /* Preflight all words: dirty final word must prevent an earlier write. */
    *(uint32_t*)(FLASH_APP_BASE+4U)=0U;
    before=unlock_calls;
    assert(Flash_AppWrite(0U,data,8U)==FLASH_IF_NOT_ERASED);
    assert(unlock_calls==before && *(uint32_t*)FLASH_APP_BASE==0xFFFFFFFFU);
    assert(Flash_AppErase(0U,8U)==FLASH_IF_OK);
    program_calls=0; fail_program_on=2;
    assert(Flash_AppWrite(0U,data,8U)==FLASH_IF_HAL_ERROR);
    assert(FLASH->CR & FLASH_CR_LOCK);
    assert(Flash_LastHalError()==0x20U);
    fail_program_on=0;
    assert(Flash_AppErase(0U,8U)==FLASH_IF_OK);
    corrupt_program=1;
    assert(Flash_AppWrite(0U,data,4U)==FLASH_IF_VERIFY_ERROR);
    assert(FLASH->CR & FLASH_CR_LOCK);
    corrupt_program=0; fail_erase=1;
    assert(Flash_AppErase(0U,8U)==FLASH_IF_HAL_ERROR);
    assert(FLASH->CR & FLASH_CR_LOCK);
    fail_erase=0;
    mock_primask=1U; FLASH->ACR=FLASH_ACR_DCEN;
    assert(Flash_AppErase(0U,8U)==FLASH_IF_OK);
    assert(mock_primask==1U && FLASH->ACR==FLASH_ACR_DCEN);
    assert(Flash_AppWrite(0U,&data[1],7U)==FLASH_IF_OK);
    assert(Flash_AppVerify(0U,&data[1],7U)==FLASH_IF_OK);
    assert(*(uint8_t*)(FLASH_APP_BASE+7U)==0xFFU);
    assert(mock_primask==1U && FLASH->ACR==FLASH_ACR_DCEN);
    assert(VirtualFree(memory,0,MEM_RELEASE));
    puts("HOST | ALL PASS | HAL failure, rounding, bounds, preflight, cache-state tests");
    return 0;
}
