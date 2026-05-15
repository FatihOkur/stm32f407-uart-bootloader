#include "boot_commands.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    BootCommands state;
    ProtocolFrame req={0}, reply, first;
    BootCommands_Init(&state);
    req.command=CMD_GET_INFO; req.sequence=1;
    BootCommands_Handle(&state,&req,1,&reply);
    assert(reply.command==0x81 && reply.sequence==1 && reply.length==22);
    const unsigned char expected[]={0,1,0,7,4,0,0,1,0,0,0,0,0,2,8,0,0,14,0,0,1,1};
    assert(memcmp(reply.payload,expected,sizeof(expected))==0);
    first=reply;
    BootCommands_Handle(&state,&req,0,&reply);
    assert(memcmp(&first,&reply,sizeof(reply))==0 && state.next_sequence==2);
    req.length=1; req.payload[0]=42;
    BootCommands_Handle(&state,&req,1,&reply);
    assert(reply.payload[0]==PROTO_STATUS_SEQUENCE && state.next_sequence==2);
    req.sequence=3; req.length=0;
    BootCommands_Handle(&state,&req,1,&reply);
    assert(reply.payload[0]==PROTO_STATUS_SEQUENCE && state.next_sequence==2);
    req.sequence=2; req.length=1;
    BootCommands_Handle(&state,&req,1,&reply);
    assert(reply.payload[0]==PROTO_STATUS_BAD_PAYLOAD && state.next_sequence==3);
    for (unsigned cmd=2;cmd<=7;cmd++) {
        req.sequence=state.next_sequence; req.command=(uint8_t)cmd; req.length=0;
        BootCommands_Handle(&state,&req,1,&reply);
        assert(reply.length==1 && reply.payload[0]==PROTO_STATUS_BAD_COMMAND);
    }
    req.command=CMD_GET_INFO; req.sequence=state.next_sequence;
    BootCommands_Handle(&state,&req,0,&reply);
    assert(reply.payload[21]==0);
    state.next_sequence=65535; req.sequence=65535;
    BootCommands_Handle(&state,&req,1,&reply);
    assert(reply.payload[0]==0 && state.next_sequence==0);
    req.sequence=0;
    BootCommands_Handle(&state,&req,1,&reply);
    assert(reply.payload[0]==0 && state.next_sequence==1);
    puts("boot commands: ALL PASS");
    return 0;
}
