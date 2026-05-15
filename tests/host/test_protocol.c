#include "protocol.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ProtocolFrame received;
static unsigned received_count;
static void capture(const ProtocolFrame *frame, void *context)
{ (void)context; received=*frame; received_count++; }
static void output_frame(const ProtocolFrame *frame, void *context)
{
    (void)context;
    uint8_t out[PROTOCOL_MAX_FRAME];
    size_t length=Protocol_Encode(frame,out,sizeof(out));
    for (size_t i=0;i<length;i++) printf("%02x",out[i]);
    putchar('\n');
}
static size_t unhex(const char *line, uint8_t *out, size_t capacity)
{
    size_t length=strcspn(line,"\r\n");
    assert((length&1U)==0U && length/2U<=capacity);
    for (size_t i=0;i<length/2U;i++) {
        unsigned value;
        assert(sscanf(line+2U*i,"%2x",&value)==1);
        out[i]=(uint8_t)value;
    }
    return length/2U;
}
int main(int argc, char **argv)
{
    if (argc==2) {
        char line[2048]; uint8_t bytes[1024];
        while (fgets(line,sizeof(line),stdin)) {
            size_t n=unhex(line,bytes,sizeof(bytes));
            if (strcmp(argv[1],"--crc")==0) {
                printf("%08lx\n",(unsigned long)Protocol_Crc32(0,bytes,n));
            } else {
                assert(strcmp(argv[1],"--frames")==0);
                ProtocolParser p; Protocol_Init(&p);
                Protocol_Feed(&p,bytes,n,0,output_frame,NULL);
            }
        }
        return 0;
    }

    assert(Protocol_Crc32(0,(const uint8_t*)"123456789",9)==0xCBF43926UL);
    assert(Protocol_Crc32(0,NULL,0)==0U);
    uint32_t crc=Protocol_Crc32(0,(const uint8_t*)"1234",4);
    assert(Protocol_Crc32(crc,(const uint8_t*)"56789",5)==0xCBF43926UL);
    ProtocolFrame frame={0}; frame.command=CMD_GET_INFO; frame.sequence=0x1234;
    uint8_t wire[PROTOCOL_MAX_FRAME], bad[PROTOCOL_MAX_FRAME];
    size_t n=Protocol_Encode(&frame,wire,sizeof(wire));
    assert(n==12 && wire[4]==0x34 && wire[5]==0x12);
    for (size_t split=0;split<=n;split++) {
        ProtocolParser p; Protocol_Init(&p); received_count=0;
        Protocol_Feed(&p,wire,split,0,capture,NULL);
        Protocol_Feed(&p,wire+split,n-split,1,capture,NULL);
        assert(received_count==1 && received.sequence==0x1234 && received.length==0);
    }

    frame.command=CMD_WRITE_CHUNK; frame.length=PROTOCOL_MAX_PAYLOAD;
    for (unsigned i=0;i<frame.length;i++) frame.payload[i]=(uint8_t)i;
    frame.payload[100]=PROTOCOL_SOF0; frame.payload[101]=PROTOCOL_SOF1;
    n=Protocol_Encode(&frame,wire,sizeof(wire)); assert(n==272);
    for (size_t split=0;split<=n;split++) {
        ProtocolParser p; Protocol_Init(&p); received_count=0;
        Protocol_Feed(&p,wire,split,0,capture,NULL);
        Protocol_Feed(&p,wire+split,n-split,1,capture,NULL);
        assert(received_count==1 && memcmp(received.payload,frame.payload,260)==0);
    }
    ProtocolParser p; Protocol_Init(&p); received_count=0;
    for (size_t i=0;i<n;i++) Protocol_Feed(&p,wire+i,1,(uint32_t)i,capture,NULL);
    assert(received_count==1);
    Protocol_Feed(&p,wire,n,300,capture,NULL);
    assert(received_count==2); /* Parser intentionally does not deduplicate. */
    assert(!Protocol_Encode(&frame,wire,n-1));
    frame.length=261; assert(!Protocol_Encode(&frame,wire,sizeof(wire)));
    frame.length=0; n=Protocol_Encode(&frame,wire,sizeof(wire));
    assert(!Protocol_Encode(NULL,wire,sizeof(wire)));
    assert(!Protocol_Encode(&frame,NULL,sizeof(wire)));

    /* Every single-bit corruption must prevent this empty frame's delivery. */
    for (size_t pos=0;pos<n;pos++) for (unsigned bit=0;bit<8;bit++) {
        memcpy(bad,wire,n); bad[pos]^=(uint8_t)(1U<<bit);
        Protocol_Init(&p); received_count=0;
        Protocol_Feed(&p,bad,n,0,capture,NULL);
        assert(received_count==0);
        Protocol_Expire(&p,100);
        Protocol_Feed(&p,wire,n,101,capture,NULL);
        assert(received_count==1);
    }
    memcpy(bad,wire,n); bad[n-1]^=1U;
    Protocol_Init(&p); received_count=0;
    Protocol_Feed(&p,bad,n,0,capture,NULL);
    Protocol_Feed(&p,wire,n,1,capture,NULL);
    assert(received_count==1 && p.crc_errors==1);

    memcpy(bad,wire,n); bad[6]=0xFF; bad[7]=0xFF;
    Protocol_Init(&p); received_count=0;
    Protocol_Feed(&p,bad,n,0,capture,NULL);
    Protocol_Feed(&p,wire,n,1,capture,NULL);
    assert(received_count==1 && p.length_errors==1);

    memcpy(bad,wire,n); bad[2]=2;
    crc=Protocol_Crc32(0,bad+2,6);
    for (unsigned i=0;i<4;i++) bad[8+i]=(uint8_t)(crc>>(8U*i));
    Protocol_Init(&p); received_count=0;
    Protocol_Feed(&p,bad,n,0,capture,NULL);
    assert(received_count==0 && p.version_errors==1);

    Protocol_Init(&p); received_count=0;
    Protocol_Feed(&p,wire,5,0xFFFFFFE0UL,capture,NULL);
    Protocol_Expire(&p,0x00000043UL); assert(p.used==5 && p.timeouts==0);
    Protocol_Expire(&p,0x00000044UL); assert(p.used==0 && p.timeouts==1);
    Protocol_Feed(&p,wire,n,0x45,capture,NULL); assert(received_count==1);

    /* Missing byte followed by a clean frame, without an intervening timeout. */
    Protocol_Init(&p); received_count=0;
    Protocol_Feed(&p,wire,n-1,0,capture,NULL);
    Protocol_Feed(&p,wire,n,1,capture,NULL); assert(received_count==1);
    /* Plausible bad length waits for timeout; sender must retry a clean frame. */
    memcpy(bad,wire,n); bad[6]=200;
    Protocol_Init(&p); received_count=0;
    Protocol_Feed(&p,bad,n,0,capture,NULL);
    Protocol_Feed(&p,wire,n,1,capture,NULL); assert(received_count==0);
    Protocol_Expire(&p,101);
    Protocol_Feed(&p,wire,n,102,capture,NULL); assert(received_count==1);

    struct { uint32_t before; ProtocolParser parser; uint32_t after; } guarded;
    guarded.before=0x11223344; guarded.after=0x55667788;
    Protocol_Init(&guarded.parser);
    uint8_t noise=0xA5;
    for (unsigned i=0;i<10000;i++) Protocol_Feed(&guarded.parser,&noise,1,i,NULL,NULL);
    received_count=0;
    Protocol_Feed(&guarded.parser,wire,n,10000,capture,NULL);
    assert(received_count==1 && guarded.before==0x11223344 && guarded.after==0x55667788);
    puts("C protocol tests: PASS (fragmentation, CRC, max length, recovery, timeout/wrap, bounds)");
    return 0;
}
