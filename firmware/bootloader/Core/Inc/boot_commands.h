#ifndef BOOT_COMMANDS_H
#define BOOT_COMMANDS_H
#include "protocol.h"

typedef struct {
    uint16_t next_sequence;
    uint8_t cached;
    ProtocolFrame last_request;
    ProtocolFrame last_response;
} BootCommands;

void BootCommands_Init(BootCommands *state);
/* HAL independent. app_validation is 0 or 1 until image CRC validation exists.
 * Request/response/state storage must not overlap. */
void BootCommands_Handle(BootCommands *state, const ProtocolFrame *request,
                         uint8_t app_validation, ProtocolFrame *response);
#endif
