#include "boot_commands.h"
#include "flash_if.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
extern unsigned update_erases,update_writes;
extern int update_fail_erase,update_fail_write;
extern uint8_t update_memory[];
static BootCommands s;
static ProtocolFrame q,r;
static void put32(uint8_t *p,uint32_t x)
{ for(unsigned i=0;i<4;i++) p[i]=(uint8_t)(x>>(8*i)); }
static void send(uint8_t cmd,uint16_t length,uint8_t expected)
{
    q.command=cmd; q.length=length; q.sequence=s.next_sequence;
    BootCommands_Handle(&s,&q,0,&r);
    assert(r.payload[0]==expected);
}
static void begin_payload(uint32_t size)
{
    memset(&q,0,sizeof(q));
    put32(q.payload,0x04070001); q.payload[4]=1;
    put32(q.payload+12,size); put32(q.payload+16,0x12345678);
}
int main(void)
{
    BootCommands_Init(&s);
    send(CMD_WRITE_CHUNK,5,PROTO_STATUS_BAD_STATE);
    begin_payload(261); q.payload[0]=0;
    send(CMD_BEGIN_UPDATE,20,PROTO_STATUS_TARGET);
    begin_payload(261); q.payload[10]=1;
    send(CMD_BEGIN_UPDATE,20,PROTO_STATUS_BAD_PAYLOAD);
    begin_payload(0); send(CMD_BEGIN_UPDATE,20,PROTO_STATUS_RANGE);
    begin_payload(FLASH_APP_SIZE+1); send(CMD_BEGIN_UPDATE,20,PROTO_STATUS_RANGE);
    assert(update_erases==0 && update_writes==0);
    assert(s.erase_calls==0 && s.write_calls==0);
    begin_payload(261); send(CMD_BEGIN_UPDATE,20,0);
    assert(s.update_state==2 && s.image_size==261 && s.image_crc==0x12345678 && s.version[0]==1);
    BootCommands_Handle(&s,&q,0,&r); /* Lost BEGIN ACK: no second erase. */
    assert(r.payload[0]==0 && update_erases==1);
    assert(s.erase_calls==1 && s.cache_hits==1);
    q.payload[4]=2; BootCommands_Handle(&s,&q,0,&r);
    assert(r.payload[0]==PROTO_STATUS_SEQUENCE && update_erases==1);
    assert(s.sequence_rejects==1);
    begin_payload(261); send(CMD_BEGIN_UPDATE,20,PROTO_STATUS_BAD_STATE);
    memset(q.payload,0,sizeof(q.payload)); put32(q.payload,4);
    send(CMD_WRITE_CHUNK,8,PROTO_STATUS_RANGE);
    put32(q.payload,0); send(CMD_WRITE_CHUNK,7,PROTO_STATUS_BAD_PAYLOAD);
    send(CMD_WRITE_CHUNK,4,PROTO_STATUS_BAD_PAYLOAD);
    assert(update_writes==0);
    memset(q.payload+4,0x55,256); send(CMD_WRITE_CHUNK,260,0);
    assert(s.received==256 && update_writes==1 && r.length==5);
    BootCommands_Handle(&s,&q,0,&r);
    assert(r.payload[0]==0 && update_writes==1);
    q.payload[4]^=1; BootCommands_Handle(&s,&q,0,&r);
    assert(r.payload[0]==PROTO_STATUS_SEQUENCE && update_writes==1);
    put32(q.payload,256); send(CMD_WRITE_CHUNK,10,PROTO_STATUS_RANGE);
    memset(q.payload+4,0x66,5); send(CMD_WRITE_CHUNK,9,0);
    assert(s.received==261 && update_writes==2 && update_memory[260]==0x66 && update_memory[261]==0xFF);
    assert(s.erase_calls==1 && s.write_calls==2 && s.cache_hits==2 && s.sequence_rejects==2);
    send(CMD_WRITE_CHUNK,9,PROTO_STATUS_RANGE);
    send(CMD_GET_STATUS,0,0);
    assert(r.length==14 && r.payload[1]==2 && r.payload[2]==5 && r.payload[3]==1);
    send(CMD_END_UPDATE,0,PROTO_STATUS_OK);
    assert(s.update_state==4);
    send(CMD_GET_DIAGNOSTICS,1,PROTO_STATUS_BAD_PAYLOAD);
    send(CMD_GET_DIAGNOSTICS,0,0);
    assert(r.length==17 && r.payload[1]==1 && r.payload[5]==2 && r.payload[9]==2 && r.payload[13]==2);
    ProtocolFrame snapshot=r;
    BootCommands_Handle(&s,&q,0,&r);
    assert(memcmp(&snapshot,&r,sizeof(r))==0 && s.cache_hits==3);
    /* Reset loses the session. Cannot continue writing without a fresh BEGIN. */
    BootCommands_Init(&s); send(CMD_WRITE_CHUNK,8,PROTO_STATUS_BAD_STATE);
    assert(s.erase_calls==0 && s.write_calls==0 && s.cache_hits==0 && s.sequence_rejects==0);
    update_fail_erase=1; begin_payload(16); send(CMD_BEGIN_UPDATE,20,PROTO_STATUS_FLASH);
    assert(s.update_state==5 && s.received==0 && s.last_hal_error==0x20);
    assert(s.erase_calls==1 && s.write_calls==0);
    send(CMD_WRITE_CHUNK,8,PROTO_STATUS_BAD_STATE);
    update_fail_erase=0; BootCommands_Init(&s);
    begin_payload(16); send(CMD_BEGIN_UPDATE,20,0);
    memset(q.payload,0,8); update_fail_write=1;
    send(CMD_WRITE_CHUNK,8,PROTO_STATUS_FLASH);
    unsigned before=update_writes;
    BootCommands_Handle(&s,&q,0,&r);
    assert(r.payload[0]==PROTO_STATUS_FLASH && update_writes==before);
    assert(s.update_state==5 && s.received==0);
    assert(s.write_calls==1 && s.cache_hits==1);
    puts("update commands: ALL PASS");
}
