#include "boot_commands.h"
#include "flash_if.h"
#include <string.h>

static void put16(uint8_t *p, uint16_t value)
{ p[0]=(uint8_t)value; p[1]=(uint8_t)(value>>8); }
static void put32(uint8_t *p, uint32_t value)
{ put16(p,(uint16_t)value); put16(p+2,(uint16_t)(value>>16)); }

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
            *response=state->last_response;
            return;
        }
        response->payload[0]=PROTO_STATUS_SEQUENCE;
        return;
    }
    if (request->sequence!=state->next_sequence) {
        response->payload[0]=PROTO_STATUS_SEQUENCE;
        return;
    }
    if (request->command!=CMD_GET_INFO) {
        response->payload[0]=PROTO_STATUS_BAD_COMMAND;
    } else if (request->length!=0U) {
        response->payload[0]=PROTO_STATUS_BAD_PAYLOAD;
    } else {
        response->payload[0]=PROTO_STATUS_OK;
        put32(response->payload+1,0x04070001UL);
        put16(response->payload+5,0U);
        put16(response->payload+7,1U);
        put16(response->payload+9,0U);
        put32(response->payload+11,FLASH_APP_BASE);
        put32(response->payload+15,FLASH_APP_SIZE);
        put16(response->payload+19,256U);
        response->payload[21]=(app_validation!=0U)?1U:0U;
        response->length=22U;
    }
    /* Cache before transport: a lost reply can be retried verbatim. */
    state->last_request.command=request->command;
    state->last_request.sequence=request->sequence;
    state->last_request.length=request->length;
    memcpy(state->last_request.payload,request->payload,request->length);
    state->last_response=*response;
    state->cached=1U;
    state->next_sequence=(uint16_t)(state->next_sequence+1U);
}
