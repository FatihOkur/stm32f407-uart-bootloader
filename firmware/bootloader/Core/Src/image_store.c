#include "image_store.h"
#include "flash_if.h"

#define META_MAGIC 0x314D5746UL /* FWM1 in little endian */
#define META_COMMIT 0x54494D43UL /* CMIT in little endian */
static uint16_t get16(const uint8_t *p)
{ return (uint16_t)((uint16_t)p[0]|((uint16_t)p[1]<<8)); }
static uint32_t get32(const uint8_t *p)
{ return (uint32_t)get16(p)|((uint32_t)get16(p+2)<<16); }
static void put16(uint8_t *p,uint16_t x)
{ p[0]=(uint8_t)x; p[1]=(uint8_t)(x>>8); }
static void put32(uint8_t *p,uint32_t x)
{ put16(p,(uint16_t)x); put16(p+2,(uint16_t)(x>>16)); }

static ProtocolStatus verify_image(uint32_t size,uint32_t expected_crc)
{
    if(size<8U || size>FLASH_APP_SIZE) return PROTO_STATUS_RANGE;
    const volatile uint8_t *flash=(const volatile uint8_t *)FLASH_APP_BASE;
    uint8_t vectors[8];
    for(unsigned i=0;i<8;i++) vectors[i]=flash[i];
    uint32_t msp=get32(vectors),reset=get32(vectors+4),entry=reset&~1UL;
    if(msp<=0x20000000UL || msp>0x20020000UL || (msp&7U)!=0U ||
       (reset&1U)==0U || entry<FLASH_APP_BASE+8U ||
       entry>FLASH_APP_BASE+size-2U) return PROTO_STATUS_VECTOR;
    uint32_t crc=0U;
    uint8_t block[256];
    for(uint32_t offset=0;offset<size;) {
        uint32_t count=size-offset;
        if(count>sizeof(block)) count=sizeof(block);
        for(uint32_t i=0;i<count;i++) block[i]=flash[offset+i];
        crc=Protocol_Crc32(crc,block,count);
        offset+=count;
    }
    return crc==expected_crc ? PROTO_STATUS_OK : PROTO_STATUS_IMAGE_CRC;
}

uint8_t Image_StoredValid(void)
{
    uint8_t record[FLASH_META_RECORD_SIZE];
    const volatile uint8_t *flash=(const volatile uint8_t *)FLASH_META_BASE;
    for(unsigned i=0;i<sizeof(record);i++) record[i]=flash[i];
    if(get32(record)!=META_MAGIC || get16(record+4)!=1U ||
       get16(record+6)!=FLASH_META_RECORD_SIZE || get32(record+8)!=0x04070001UL ||
       get32(record+12)!=FLASH_APP_BASE || get16(record+30)!=0U ||
       get32(record+36)!=META_COMMIT ||
       Protocol_Crc32(0U,record,32U)!=get32(record+32)) return 0U;
    return verify_image(get32(record+16),get32(record+20))==PROTO_STATUS_OK;
}

uint8_t Image_GetBootVectors(uint8_t force_bootloader,uint32_t *msp,uint32_t *reset)
{
    if(msp==NULL || reset==NULL) return 0U;
    *msp=0U; *reset=0U;
    /* Recovery must remain available even when the existing image is valid. */
    if(force_bootloader || !Image_StoredValid()) return 0U;
    /* No flash mutations/command dispatch occur between validation and handoff. */
    *msp=*(const volatile uint32_t *)FLASH_APP_BASE;
    *reset=*(const volatile uint32_t *)(FLASH_APP_BASE+4U);
    return 1U;
}

ProtocolStatus Image_Commit(uint32_t size,uint32_t crc,const uint16_t version[3])
{
    ProtocolStatus status=verify_image(size,crc);
    if(status!=PROTO_STATUS_OK) return status;
    const volatile uint8_t *flash=(const volatile uint8_t *)FLASH_META_BASE;
    for(unsigned i=0;i<FLASH_META_RECORD_SIZE;i++)
        if(flash[i]!=0xFFU) return PROTO_STATUS_FLASH;
    uint8_t record[FLASH_META_RECORD_SIZE]={0};
    put32(record,META_MAGIC); put16(record+4,1U); put16(record+6,FLASH_META_RECORD_SIZE);
    put32(record+8,0x04070001UL); put32(record+12,FLASH_APP_BASE);
    put32(record+16,size); put32(record+20,crc);
    for(unsigned i=0;i<3;i++) put16(record+24+2*i,version[i]);
    put32(record+32,Protocol_Crc32(0U,record,32U));
    put32(record+36,META_COMMIT);
    /* Header including its CRC is fully written/read back BEFORE the marker.
     * A missing/partial marker cannot validate a partially written record. */
    if(Flash_MetadataWrite(0U,record,36U)!=FLASH_IF_OK) return PROTO_STATUS_FLASH;
    if(Flash_MetadataWrite(36U,record+36,4U)!=FLASH_IF_OK) return PROTO_STATUS_FLASH;
    return Image_StoredValid() ? PROTO_STATUS_OK : PROTO_STATUS_FLASH;
}
