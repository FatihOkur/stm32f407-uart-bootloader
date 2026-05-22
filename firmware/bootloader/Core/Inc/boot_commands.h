#ifndef BOOT_COMMANDS_H
#define BOOT_COMMANDS_H
#include "protocol.h"

typedef struct {
    uint16_t next_sequence;
    uint8_t cached;
    ProtocolFrame last_request;
    ProtocolFrame last_response;
    uint8_t update_state; /* 0 IDLE, 1 PREPARING, 2 RECEIVING, 5 ERROR */
    uint32_t image_size, image_crc, received, last_hal_error;
    uint16_t version[3];
    /* Session counters count flash API attempts, not individual flash words. */
    uint32_t erase_calls, write_calls, cache_hits, sequence_rejects;
} BootCommands;

void BootCommands_Init(BootCommands *state);
/* No direct HAL dependency; mutations use flash_if. app_validation is 0 or 1
 * until image CRC validation exists (vector sanity does not authorize boot).
 * Request/response/state storage must not overlap. */
void BootCommands_Handle(BootCommands *state, const ProtocolFrame *request,
                         uint8_t app_validation, ProtocolFrame *response);
#endif
