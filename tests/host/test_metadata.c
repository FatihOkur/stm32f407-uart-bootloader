/* Production flash_if + image_store + dispatcher, memory-mapped flash/mock HAL. */
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "stm32f4xx_hal.h"
#include "flash_if.h"
#include "image_store.h"
#include "boot_commands.h"
MockFlash mock_flash;
uint32_t mock_primask;
static unsigned meta_erases,app_erases,meta_writes,app_writes;
static unsigned fail_meta_word;
static int fail_meta_erase,partial_marker;
static BootCommands state;
static ProtocolFrame q,r;
static uint8_t image[261];
static uint32_t crc;
static const uint16_t version[3]={1,2,3};
static uint8_t *ptr(uint32_t addr) { return (uint8_t *)(uintptr_t)addr; }
HAL_StatusTypeDef HAL_FLASH_Unlock(void) { FLASH->CR&=~FLASH_CR_LOCK; return HAL_OK; }
HAL_StatusTypeDef HAL_FLASH_Lock(void) { FLASH->CR|=FLASH_CR_LOCK; return HAL_OK; }
uint32_t HAL_FLASH_GetError(void) { return 0x20; }
HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *e,uint32_t *failed)
{
    assert(!(FLASH->CR&FLASH_CR_LOCK) && e->VoltageRange==FLASH_VOLTAGE_RANGE_3);
    *failed=0xFFFFFFFF;
    if(e->Sector==4) {
        assert(e->NbSectors==1); meta_erases++;
        if(fail_meta_erase) { *failed=4; return HAL_ERROR; }
        memset(ptr(FLASH_META_BASE),0xFF,FLASH_META_SIZE);
    } else {
        assert(e->Sector>=5 && e->Sector<=11 && e->NbSectors<=12-e->Sector);
        /* Old committed metadata MUST be absent before the first app erase. */
        for(unsigned i=0;i<FLASH_META_RECORD_SIZE;i++) assert(ptr(FLASH_META_BASE)[i]==0xFF);
        app_erases++;
        memset(ptr(FLASH_APP_BASE+(e->Sector-5)*FLASH_APP_SECTOR_SIZE),0xFF,e->NbSectors*FLASH_APP_SECTOR_SIZE);
    }
    return HAL_OK;
}
HAL_StatusTypeDef HAL_FLASH_Program(uint32_t type,uint32_t address,uint64_t value)
{
    assert(!(FLASH->CR&FLASH_CR_LOCK) && type==FLASH_TYPEPROGRAM_WORD && !(address&3));
    assert((address>=FLASH_META_BASE && address<=FLASH_META_BASE+36) ||
           (address>=FLASH_APP_BASE && address<=FLASH_APP_BASE+FLASH_APP_SIZE-4));
    uint32_t word=(uint32_t)value;
    for(unsigned i=0;i<4;i++) assert(ptr(address)[i]==0xFF);
    if(address<FLASH_APP_BASE) {
        meta_writes++;
        if(fail_meta_word && meta_writes==fail_meta_word) {
            if(partial_marker) { ptr(address)[0]=(uint8_t)word; ptr(address)[1]=(uint8_t)(word>>8); }
            return HAL_ERROR;
        }
    } else app_writes++;
    memcpy(ptr(address),&word,4);
    if(address>=FLASH_META_BASE && address<FLASH_META_BASE+36) assert(!Image_StoredValid());
    return HAL_OK;
}
static void put32(uint8_t *p,uint32_t x)
{ for(unsigned i=0;i<4;i++) p[i]=(uint8_t)(x>>(8*i)); }
static void dispatch(uint8_t cmd,uint16_t length,uint8_t expected)
{
    q.command=cmd; q.length=length; q.sequence=state.next_sequence;
    BootCommands_Handle(&state,&q,1,&r); assert(r.payload[0]==expected);
}
static void fresh(void)
{
    memset(ptr(FLASH_META_BASE),0xFF,FLASH_META_SIZE+FLASH_APP_SIZE);
    BootCommands_Init(&state); memset(&q,0,sizeof(q));
    meta_erases=app_erases=meta_writes=app_writes=0;
    fail_meta_word=0; fail_meta_erase=partial_marker=0;
}
static void begin(uint32_t image_crc)
{
    memset(q.payload,0,20); put32(q.payload,0x04070001);
    q.payload[4]=1; q.payload[6]=2; q.payload[8]=3;
    put32(q.payload+12,sizeof(image)); put32(q.payload+16,image_crc);
    dispatch(CMD_BEGIN_UPDATE,20,0);
}
static void upload(void)
{
    put32(q.payload,0); memcpy(q.payload+4,image,256); dispatch(CMD_WRITE_CHUNK,260,0);
    put32(q.payload,256); memcpy(q.payload+4,image+256,5); dispatch(CMD_WRITE_CHUNK,9,0);
}
int main(void)
{
    void *mem=VirtualAlloc((void*)0x08000000,0x100000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    assert(mem==(void*)0x08000000); memset(mem,0x35,0x10000);
    FLASH->CR=FLASH_CR_LOCK; FLASH->ACR=FLASH_ACR_ICEN|FLASH_ACR_DCEN;
    memset(image,0x42,sizeof(image)); put32(image,0x20020000); put32(image+4,FLASH_APP_BASE+9);
    crc=Protocol_Crc32(0,image,sizeof(image));
    uint32_t boot_msp=1,boot_reset=1;
    fresh();
    assert(!Image_GetBootVectors(0,&boot_msp,&boot_reset));
    assert(boot_msp==0 && boot_reset==0);
    assert(!Image_GetBootVectors(0,NULL,&boot_reset));
    assert(!Image_GetBootVectors(0,&boot_msp,NULL));
    /* Sane vectors without committed metadata must never authorize boot. */
    memcpy(ptr(FLASH_APP_BASE),image,sizeof(image));
    assert(!Image_GetBootVectors(0,&boot_msp,&boot_reset));
    begin(crc);
    assert(meta_erases==1 && app_erases==1);
    dispatch(CMD_END_UPDATE,0,PROTO_STATUS_INCOMPLETE);
    assert(state.update_state==2 && meta_writes==0);
    upload(); dispatch(CMD_END_UPDATE,1,PROTO_STATUS_BAD_PAYLOAD);
    assert(!Image_GetBootVectors(0,&boot_msp,&boot_reset));
    dispatch(CMD_END_UPDATE,0,0);
    assert(state.update_state==4 && meta_writes==10 && Image_StoredValid());
    assert(Image_GetBootVectors(0,&boot_msp,&boot_reset));
    assert(boot_msp==0x20020000 && boot_reset==FLASH_APP_BASE+9);
    assert(!Image_GetBootVectors(1,&boot_msp,&boot_reset));
    assert(boot_msp==0 && boot_reset==0);
    BootCommands_Handle(&state,&q,1,&r); /* Lost END reply: no repeated metadata writes. */
    assert(r.payload[0]==0 && meta_writes==10);
    BootCommands_Init(&state); /* Reset RAM, retain flash. */
    dispatch(CMD_GET_INFO,0,0); assert(r.payload[21]==2);
    assert(Image_GetBootVectors(0,&boot_msp,&boot_reset));
    assert(Image_StoredValid());
    /* Final flash word padding is outside the declared image CRC. */
    ptr(FLASH_APP_BASE)[sizeof(image)]=0;
    assert(Image_StoredValid());
    ptr(FLASH_APP_BASE)[sizeof(image)]=0xFF;
    uint8_t record[40]; memcpy(record,ptr(FLASH_META_BASE),40);
    /* Every single-bit mutation in metadata is rejected, including marker. */
    for(unsigned i=0;i<40;i++) for(unsigned bit=0;bit<8;bit++) {
        ptr(FLASH_META_BASE)[i]^=(uint8_t)(1U<<bit); assert(!Image_StoredValid());
        assert(!Image_GetBootVectors(0,&boot_msp,&boot_reset));
        assert(boot_msp==0 && boot_reset==0);
        ptr(FLASH_META_BASE)[i]^=(uint8_t)(1U<<bit);
    }
    ptr(FLASH_APP_BASE)[100]^=1; assert(!Image_StoredValid());
    assert(!Image_GetBootVectors(0,&boot_msp,&boot_reset));
    ptr(FLASH_APP_BASE)[100]^=1;
    /* Corrupt size even with corrected record CRC must not read out of bounds. */
    put32(ptr(FLASH_META_BASE)+16,0xFFFFFFFF);
    put32(ptr(FLASH_META_BASE)+32,Protocol_Crc32(0,ptr(FLASH_META_BASE),32));
    assert(!Image_StoredValid()); memcpy(ptr(FLASH_META_BASE),record,40);
    assert(Image_StoredValid());
    unsigned old_app_erases=app_erases;
    fail_meta_erase=1;
    memset(q.payload,0,20); put32(q.payload,0x04070001); put32(q.payload+12,sizeof(image));
    dispatch(CMD_BEGIN_UPDATE,20,PROTO_STATUS_FLASH);
    assert(app_erases==old_app_erases && Image_StoredValid()); /* Old image untouched. */
    fresh(); begin(crc^1); upload(); dispatch(CMD_END_UPDATE,0,PROTO_STATUS_IMAGE_CRC);
    assert(state.update_state==5 && !Image_StoredValid() && meta_writes==0);
    /* Recover without manually clearing flash: only reset RAM/session and retry. */
    BootCommands_Init(&state);
    assert(!Image_GetBootVectors(0,&boot_msp,&boot_reset));
    begin(crc); upload(); dispatch(CMD_END_UPDATE,0,0);
    assert(Image_GetBootVectors(0,&boot_msp,&boot_reset));
    /* Interrupt after first acknowledged chunk, reset, then recover normally. */
    BootCommands_Init(&state); begin(crc);
    put32(q.payload,0); memcpy(q.payload+4,image,256); dispatch(CMD_WRITE_CHUNK,260,0);
    BootCommands_Init(&state);
    assert(!Image_GetBootVectors(0,&boot_msp,&boot_reset));
    begin(crc); upload(); dispatch(CMD_END_UPDATE,0,0);
    assert(Image_GetBootVectors(0,&boot_msp,&boot_reset));
    fresh(); begin(crc); upload(); ptr(FLASH_APP_BASE)[0]=1;
    dispatch(CMD_END_UPDATE,0,PROTO_STATUS_VECTOR); assert(meta_writes==0);
    assert(!Image_GetBootVectors(0,&boot_msp,&boot_reset));
    /* Failure/cut before every metadata word: no valid record, flash locked. */
    for(unsigned cut=1;cut<=10;cut++) {
        fresh(); begin(crc); upload(); fail_meta_word=cut;
        dispatch(CMD_END_UPDATE,0,PROTO_STATUS_FLASH);
        assert(!Image_StoredValid() && state.update_state==5 && (FLASH->CR&FLASH_CR_LOCK));
        assert(!Image_GetBootVectors(0,&boot_msp,&boot_reset));
    }
    fresh(); begin(crc); upload(); fail_meta_word=10; partial_marker=1;
    dispatch(CMD_END_UPDATE,0,PROTO_STATUS_FLASH); assert(!Image_StoredValid());
    fresh();
    uint8_t word[4]={0};
    assert(Flash_MetadataWrite(40,word,4)==FLASH_IF_RANGE);
    assert(Flash_MetadataWrite(0xFFFFFFFF,word,4)==FLASH_IF_RANGE);
    assert(Flash_MetadataWrite(1,word,4)==FLASH_IF_ALIGNMENT);
    assert(Flash_MetadataWrite(0,word,3)==FLASH_IF_ALIGNMENT);
    assert(Flash_MetadataWrite(0,NULL,4)==FLASH_IF_ARGUMENT);
    assert(Flash_MetadataWrite(0,word,0)==FLASH_IF_ARGUMENT);
    assert(meta_writes==0);
    /* Maximum-size image CRC stays within the application area. */
    memset(ptr(FLASH_APP_BASE),0x42,FLASH_APP_SIZE);
    memcpy(ptr(FLASH_APP_BASE),image,8);
    assert(Image_Commit(FLASH_APP_SIZE,Protocol_Crc32(0,ptr(FLASH_APP_BASE),FLASH_APP_SIZE),version)==PROTO_STATUS_OK);
    assert(Image_StoredValid());
    assert(Image_Commit(FLASH_APP_SIZE+1,0,version)==PROTO_STATUS_RANGE);
    /* Metadata erase never touches adjacent boot or application bytes. */
    memset(ptr(FLASH_META_BASE),0x35,FLASH_META_SIZE);
    memset(ptr(FLASH_APP_BASE),0x36,FLASH_APP_SIZE);
    assert(Flash_MetadataErase()==FLASH_IF_OK);
    for(unsigned i=0;i<0x10000;i++) assert(ptr(0x08000000)[i]==0x35);
    for(unsigned i=0;i<FLASH_APP_SIZE;i++) assert(ptr(FLASH_APP_BASE)[i]==0x36);
    assert(FLASH->CR&FLASH_CR_LOCK);
    assert(VirtualFree(mem,0,MEM_RELEASE));
    (void)version;
    puts("metadata + END_UPDATE: ALL PASS");
}
