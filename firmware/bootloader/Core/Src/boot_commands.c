#include "boot_commands.h"
#include "flash_if.h"
#include "image_store.h"
#include <string.h>

static void put16(uint8_t *p, uint16_t value)
{ p[0]=(uint8_t)value; p[1]=(uint8_t)(value>>8); }
static void put32(uint8_t *p, uint32_t value)
{ put16(p,(uint16_t)value); put16(p+2,(uint16_t)(value>>16)); }
static uint16_t get16(const uint8_t *p)
{ return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1]<<8)); }
static uint32_t get32(const uint8_t *p)
{ return (uint32_t)get16(p) | ((uint32_t)get16(p+2)<<16); }

static void flash_failed(BootCommands *state, ProtocolFrame *response)
{
    state->update_state=5U;
    state->last_hal_error=Flash_LastHalError();
    response->payload[0]=PROTO_STATUS_FLASH;
}

static void execute(BootCommands *s, const ProtocolFrame *q,
                    uint8_t app_validation, ProtocolFrame *r)
{
    const uint8_t *p=q->payload;
    switch(q->command) {
    case CMD_GET_INFO:
        if(q->length) { r->payload[0]=PROTO_STATUS_BAD_PAYLOAD; break; }
        put32(r->payload+1,0x04070001UL);
        put16(r->payload+5,0U); put16(r->payload+7,4U); put16(r->payload+9,0U);
        put32(r->payload+11,FLASH_APP_BASE); put32(r->payload+15,FLASH_APP_SIZE);
        put16(r->payload+19,256U);
        r->payload[21]=Image_StoredValid()?2U:((app_validation!=0U)?1U:0U);
        r->length=22U;
        break;
    case CMD_BEGIN_UPDATE: {
        if(q->length!=20U || get16(p+10)!=0U) {
            r->payload[0]=PROTO_STATUS_BAD_PAYLOAD; break;
        }
        if(get32(p)!=0x04070001UL) { r->payload[0]=PROTO_STATUS_TARGET; break; }
        uint32_t size=get32(p+12);
        if(size<8U || size>FLASH_APP_SIZE) { r->payload[0]=PROTO_STATUS_RANGE; break; }
        if(s->update_state!=0U) { r->payload[0]=PROTO_STATUS_BAD_STATE; break; }
        /* Invalidate persistent authorization before touching application flash.
         * Subsequent reset cannot boot a partial image without a valid commit. */
        s->image_size=size; s->image_crc=get32(p+16); s->received=0U;
        for(unsigned i=0;i<3;i++) s->version[i]=get16(p+4+2*i);
        s->last_hal_error=0U; s->update_state=1U;
        /* Fully erase/verify old metadata before touching any application byte. */
        if(Flash_MetadataErase()!=FLASH_IF_OK) { flash_failed(s,r); break; }
        s->erase_calls++;
        if(Flash_AppErase(0U,size)!=FLASH_IF_OK) { flash_failed(s,r); break; }
        s->update_state=2U;
        break;
    }
    case CMD_WRITE_CHUNK: {
        if(q->length<5U || q->length>260U) { r->payload[0]=PROTO_STATUS_BAD_PAYLOAD; break; }
        if(s->update_state!=2U) { r->payload[0]=PROTO_STATUS_BAD_STATE; break; }
        uint32_t offset=get32(p), length=q->length-4U;
        if(offset!=s->received || (offset&3U)!=0U || offset>=s->image_size ||
           length>s->image_size-offset) { r->payload[0]=PROTO_STATUS_RANGE; break; }
        if(length!=s->image_size-offset && (length&3U)!=0U) {
            r->payload[0]=PROTO_STATUS_BAD_PAYLOAD; break;
        }
        /* Flash_AppWrite verifies bytes and final FF padding before ACK. */
        s->write_calls++;
        if(Flash_AppWrite(offset,p+4,length)!=FLASH_IF_OK) { flash_failed(s,r); break; }
        s->received+=length;
        put32(r->payload+1,s->received); r->length=5U;
        break;
    }
    case CMD_END_UPDATE: {
        if(q->length) { r->payload[0]=PROTO_STATUS_BAD_PAYLOAD; break; }
        if(s->update_state!=2U) { r->payload[0]=PROTO_STATUS_BAD_STATE; break; }
        if(s->received!=s->image_size) { r->payload[0]=PROTO_STATUS_INCOMPLETE; break; }
        s->update_state=3U;
        ProtocolStatus result=Image_Commit(s->image_size,s->image_crc,s->version);
        if(result==PROTO_STATUS_FLASH) { flash_failed(s,r); break; }
        r->payload[0]=(uint8_t)result;
        s->update_state=(result==PROTO_STATUS_OK)?4U:5U;
        break;
    }
    case CMD_GET_STATUS:
        if(q->length) { r->payload[0]=PROTO_STATUS_BAD_PAYLOAD; break; }
        r->payload[1]=s->update_state;
        put32(r->payload+2,s->received); put32(r->payload+6,s->image_size);
        put32(r->payload+10,s->last_hal_error); r->length=14U;
        break;
    case CMD_GET_DIAGNOSTICS:
        if(q->length) { r->payload[0]=PROTO_STATUS_BAD_PAYLOAD; break; }
        put32(r->payload+1,s->erase_calls); put32(r->payload+5,s->write_calls);
        put32(r->payload+9,s->cache_hits); put32(r->payload+13,s->sequence_rejects);
        r->length=17U;
        break;
    default:
        r->payload[0]=PROTO_STATUS_BAD_COMMAND;
        break;
    }
}

void BootCommands_Init(BootCommands *state)
{
    memset(state,0,sizeof(*state));
    state->next_sequence=1U;
}

void BootCommands_Handle(BootCommands *state, const ProtocolFrame *request,
                         uint8_t app_validation, ProtocolFrame *response)
{
    memset(response,0,sizeof(*response));
    response->command=request->command|0x80U;
    response->sequence=request->sequence;
    response->length=1U;
    if (request->length>PROTOCOL_MAX_PAYLOAD) {
        response->payload[0]=PROTO_STATUS_BAD_PAYLOAD;
        return;
    }
    if (state->cached && request->sequence==state->last_request.sequence) {
        const ProtocolFrame *last=&state->last_request;
        if (request->command==last->command && request->length==last->length &&
            memcmp(request->payload,last->payload,request->length)==0) {
            state->cache_hits++;
            *response=state->last_response;
            return;
        }
        state->sequence_rejects++;
        response->payload[0]=PROTO_STATUS_SEQUENCE;
        return;
    }
    if (request->sequence!=state->next_sequence) {
        state->sequence_rejects++;
        response->payload[0]=PROTO_STATUS_SEQUENCE;
        return;
    }
    execute(state,request,app_validation,response);
    /* Cache before transport: a lost reply can be retried verbatim. */
    state->last_request.command=request->command;
    state->last_request.sequence=request->sequence;
    state->last_request.length=request->length;
    memcpy(state->last_request.payload,request->payload,request->length);
    state->last_response=*response;
    state->cached=1U;
    state->next_sequence=(uint16_t)(state->next_sequence+1U);
}
