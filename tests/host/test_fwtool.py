from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tools'))
from fwtool import ResponseReader, request_info
from protocol import Frame, encode

PAYLOAD=b'\0'+struct.pack('<IHHHIIHB',0x04070001,0,1,0,0x08020000,0xE0000,256,1)
GOOD=encode(Frame(0x81,1,PAYLOAD))

class FakePort:
    def __init__(self,responses):
        self.responses=responses
        self.writes=[]
        self.pending=bytearray()
    @property
    def in_waiting(self): return min(3,len(self.pending))
    def write(self,data):
        self.writes.append(data)
        i=len(self.writes)-1
        if i<len(self.responses): self.pending.extend(self.responses[i])
        return len(data)
    def read(self,count):
        result=bytes(self.pending[:count])
        del self.pending[:count]
        return result

class TransportTests(unittest.TestCase):
    def test_all_splits(self):
        for split in range(len(GOOD)+1):
            reader=ResponseReader()
            frames=reader.feed(GOOD[:split],1)+reader.feed(GOOD[split:],1.001)
            self.assertEqual(frames,[Frame(0x81,1,PAYLOAD)])
    def test_corruption_noise_and_timeout(self):
        bad=bytearray(GOOD); bad[-1]^=1
        r=ResponseReader()
        frames=r.feed(b'APP text\r\n'+bad+GOOD,1)
        self.assertEqual(frames,[Frame(0x81,1,PAYLOAD)])
        r.feed(GOOD[:10],2)
        r.feed(b'',2.11)
        self.assertEqual(r.feed(GOOD,2.12),[Frame(0x81,1,PAYLOAD)])
    def test_oversize_and_buffer_bound(self):
        r=ResponseReader()
        bad=b'\xa5\x5a\x01\x81\x01\x00\xff\xff'
        self.assertEqual(r.feed(bad+GOOD,1),[Frame(0x81,1,PAYLOAD)])
        for i in range(1000): r.feed(b'garbage'*38,2+i*.001)
        self.assertLessEqual(len(r.buffer),272)
    def test_retry_identical(self):
        port=FakePort([b'',GOOD])
        ticks=iter(i*.001 for i in range(10000))
        with patch('fwtool.time.monotonic',side_effect=lambda:next(ticks)):
            result=request_info(port,timeout=.1)
        self.assertEqual(result,(0x04070001,0,1,0,0x08020000,0xE0000,256,1))
        self.assertEqual(len(port.writes),2)
        self.assertEqual(port.writes[0],port.writes[1])
    def test_unrelated_and_malformed(self):
        port=FakePort([encode(Frame(0x81,2,PAYLOAD))+encode(Frame(0x82,1,PAYLOAD))+
                       encode(Frame(0x81,1,b'\0'))+GOOD])
        self.assertEqual(request_info(port)[-1],1)
    def test_status_error(self):
        with self.assertRaisesRegex(RuntimeError,'status=5'):
            request_info(FakePort([encode(Frame(0x81,1,b'\x05'))]))
    def test_exhaustion(self):
        port=FakePort([])
        ticks=iter(i*.01 for i in range(1000))
        with patch('fwtool.time.monotonic',side_effect=lambda:next(ticks)):
            with self.assertRaises(TimeoutError): request_info(port,timeout=.05)
        self.assertEqual(len(port.writes),4)
        self.assertEqual(len(set(port.writes)),1)

if __name__=='__main__': unittest.main()
