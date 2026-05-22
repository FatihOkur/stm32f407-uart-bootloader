#ifndef BOOT_PROTOCOL_H
#define BOOT_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define PROTOCOL_VERSION 1U
#define PROTOCOL_SOF0 0xA5U
#define PROTOCOL_SOF1 0x5AU
#define PROTOCOL_MAX_PAYLOAD 260U
#define PROTOCOL_HEADER_SIZE 8U
#define PROTOCOL_MAX_FRAME (PROTOCOL_HEADER_SIZE + PROTOCOL_MAX_PAYLOAD + 4U)
#define PROTOCOL_RX_GAP_MS 100U

typedef enum {
    CMD_GET_INFO=0x01, CMD_BEGIN_UPDATE=0x02, CMD_WRITE_CHUNK=0x03,
    CMD_END_UPDATE=0x04, CMD_GET_STATUS=0x05, CMD_ABORT=0x06, CMD_REBOOT=0x07,
    CMD_GET_DIAGNOSTICS=0x08
} ProtocolCommand;

typedef enum {
    PROTO_STATUS_OK=0, PROTO_STATUS_BAD_COMMAND=1, PROTO_STATUS_BAD_PAYLOAD=2,
    PROTO_STATUS_BAD_STATE=3, PROTO_STATUS_RANGE=4, PROTO_STATUS_SEQUENCE=5,
    PROTO_STATUS_FLASH=6, PROTO_STATUS_IMAGE_CRC=7, PROTO_STATUS_TARGET=8,
    PROTO_STATUS_INCOMPLETE=9, PROTO_STATUS_VECTOR=10
} ProtocolStatus;

typedef struct {
    uint8_t command;
    uint16_t sequence;
    uint16_t length;
    uint8_t payload[PROTOCOL_MAX_PAYLOAD];
} ProtocolFrame;

typedef struct {
    uint8_t buffer[PROTOCOL_MAX_FRAME];
    uint16_t used;
    uint32_t last_byte_ms;
    uint32_t frames, crc_errors, length_errors, version_errors, timeouts;
} ProtocolParser;

/* Frame pointer is valid only during the callback. No flash/UART HAL dependency.
 * Do not recursively call Feed on the same parser from its callback.
 */
typedef void (*ProtocolOnFrame)(const ProtocolFrame *frame, void *context);

/* Incremental CRC takes/returns finalized CRC values; initial previous is zero.
 * ISO-HDLC: poly 0x04C11DB7 (reflected 0xEDB88320), init/xorout FFFFFFFF.
 */
uint32_t Protocol_Crc32(uint32_t previous, const uint8_t *data, size_t length);
void Protocol_Init(ProtocolParser *parser);
void Protocol_Expire(ProtocolParser *parser, uint32_t now_ms);
void Protocol_Feed(ProtocolParser *parser, const uint8_t *data, size_t length,
                   uint32_t now_ms, ProtocolOnFrame callback, void *context);
/* Returns encoded length, or zero for invalid arguments/insufficient capacity.
 * Input frame and output storage must not overlap. Structures are NOT wire data.
 */
size_t Protocol_Encode(const ProtocolFrame *frame, uint8_t *out, size_t capacity);

#endif
