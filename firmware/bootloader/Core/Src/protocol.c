#include "protocol.h"
#include <string.h>

static uint16_t get16(const uint8_t *p)
{ return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put16(uint8_t *p, uint16_t value)
{ p[0]=(uint8_t)value; p[1]=(uint8_t)(value >> 8); }
static void put32(uint8_t *p, uint32_t value)
{
    p[0]=(uint8_t)value; p[1]=(uint8_t)(value >> 8);
    p[2]=(uint8_t)(value >> 16); p[3]=(uint8_t)(value >> 24);
}

uint32_t Protocol_Crc32(uint32_t previous, const uint8_t *data, size_t length)
{
    uint32_t crc = previous ^ 0xFFFFFFFFUL;
    for (size_t i=0; i<length; i++) {
        crc ^= data[i];
        for (unsigned bit=0; bit<8; bit++)
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320UL : 0U);
    }
    return crc ^ 0xFFFFFFFFUL;
}

void Protocol_Init(ProtocolParser *parser) { memset(parser, 0, sizeof(*parser)); }

void Protocol_Expire(ProtocolParser *parser, uint32_t now_ms)
{
    /* Unsigned subtraction handles HAL_GetTick rollover. Poll at least every
     * 100 ms when idle; never leave a parser unpolled for a full 32-bit wrap.
     */
    if (parser->used && (uint32_t)(now_ms-parser->last_byte_ms)>=PROTOCOL_RX_GAP_MS) {
        parser->used=0U;
        parser->timeouts++;
    }
}

static void discard(ProtocolParser *parser, uint16_t count)
{
    parser->used=(uint16_t)(parser->used-count);
    memmove(parser->buffer, parser->buffer+count, parser->used);
}

static void parse_buffer(ProtocolParser *parser, ProtocolOnFrame callback, void *context)
{
    while (parser->used) {
        const uint8_t *b=parser->buffer;
        if (b[0]!=PROTOCOL_SOF0) { discard(parser,1U); continue; }
        if (parser->used<2U) return;
        if (b[1]!=PROTOCOL_SOF1) { discard(parser,1U); continue; }
        if (parser->used<PROTOCOL_HEADER_SIZE) return;
        uint16_t length=get16(b+6);
        if (length>PROTOCOL_MAX_PAYLOAD) {
            parser->length_errors++;
            discard(parser,1U);
            continue;
        }
        uint16_t total=(uint16_t)(PROTOCOL_HEADER_SIZE+length+4U);
        if (parser->used<total) return;
        if (Protocol_Crc32(0U,b+2,6U+length)!=get32(b+8U+length)) {
            parser->crc_errors++;
            discard(parser,1U); /* Search retained bytes for a later candidate. */
            continue;
        }
        if (b[2]!=PROTOCOL_VERSION) {
            parser->version_errors++;
            discard(parser,total);
            continue;
        }
        ProtocolFrame frame;
        frame.command=b[3];
        frame.sequence=get16(b+4);
        frame.length=length;
        if (length) memcpy(frame.payload,b+8,length);
        discard(parser,total);
        parser->frames++;
        if (callback) callback(&frame,context);
    }
}

void Protocol_Feed(ProtocolParser *parser, const uint8_t *data, size_t length,
                   uint32_t now_ms, ProtocolOnFrame callback, void *context)
{
    Protocol_Expire(parser,now_ms);
    if (data==NULL && length!=0U) return;
    for (size_t i=0; i<length; i++) {
        /* Complete/invalid candidates are consumed on every byte. */
        if (parser->used>=PROTOCOL_MAX_FRAME) {
            parser->length_errors++;
            parser->used=0U;
        }
        parser->buffer[parser->used++]=data[i];
        parser->last_byte_ms=now_ms;
        parse_buffer(parser,callback,context);
    }
}

size_t Protocol_Encode(const ProtocolFrame *frame, uint8_t *out, size_t capacity)
{
    if (frame==NULL || out==NULL || frame->length>PROTOCOL_MAX_PAYLOAD) return 0U;
    size_t total=PROTOCOL_HEADER_SIZE+frame->length+4U;
    if (capacity<total) return 0U;
    out[0]=PROTOCOL_SOF0; out[1]=PROTOCOL_SOF1;
    out[2]=PROTOCOL_VERSION; out[3]=frame->command;
    put16(out+4,frame->sequence);
    put16(out+6,frame->length);
    if (frame->length) memcpy(out+8,frame->payload,frame->length);
    put32(out+8+frame->length,Protocol_Crc32(0U,out+2,6U+frame->length));
    return total;
}
