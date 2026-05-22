#include "flash_if.h"
#include "image_store.h"
#include <assert.h>
#include <string.h>
unsigned update_erases, update_writes;
int update_fail_erase, update_fail_write;
uint8_t update_memory[FLASH_APP_SIZE];
FlashIfStatus Flash_AppErase(uint32_t offset,uint32_t length)
{
    assert(offset==0 && length>=8 && length<=FLASH_APP_SIZE);
    update_erases++;
    if(update_fail_erase) return FLASH_IF_HAL_ERROR;
    memset(update_memory,0xFF,((length-1)/FLASH_APP_SECTOR_SIZE+1)*FLASH_APP_SECTOR_SIZE);
    return FLASH_IF_OK;
}
FlashIfStatus Flash_AppWrite(uint32_t offset,const uint8_t *data,uint32_t length)
{
    assert(offset%4==0 && length>0 && offset<FLASH_APP_SIZE && length<=FLASH_APP_SIZE-offset);
    update_writes++;
    if(update_fail_write) return FLASH_IF_HAL_ERROR;
    for(uint32_t i=0;i<length;i++) assert(update_memory[offset+i]==0xFF);
    memcpy(update_memory+offset,data,length);
    return FLASH_IF_OK;
}
uint32_t Flash_LastHalError(void) { return 0x20U; }
/* Command-unit tests stub the image layer; test_metadata uses production code. */
FlashIfStatus Flash_MetadataErase(void) { return FLASH_IF_OK; }
uint8_t Image_StoredValid(void) { return 0U; }
ProtocolStatus Image_Commit(uint32_t size,uint32_t crc,const uint16_t version[3])
{ (void)size; (void)crc; (void)version; return PROTO_STATUS_OK; }
