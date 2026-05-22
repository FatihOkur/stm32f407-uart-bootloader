from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tools'))
from fwpackage import Package, APP_BASE, APP_CAPACITY, TARGET_ID
from protocol import Frame, encode, decode
from fwtransfer import transfer, Client


class Board:
    def __init__(self):
        self.pending=bytearray(); self.requests=[]; self.last=None; self.reply=None
        self.state=0; self.size=0; self.received=0; self.erases=0; self.writes=0
        self.version=(0,2,0); self.target=TARGET_ID
        self.cache_hits=0; self.sequence_rejects=0
        self.validation=1; self.commit_error=0; self.drop_end=False; self.commits=0
        self.drop_begin=False; self.drop_write=False; self.fail_write=False
        self.wrong_offset=False; self.memory=bytearray()
    @property
    def in_waiting(self): return min(len(self.pending),11)
    def read(self,n):
        result=bytes(self.pending[:n]); del self.pending[:n]; return result
    def write(self,wire):
        q=decode(wire); self.requests.append(q)
        if q==self.last:
            self.cache_hits+=1
            self.pending.extend(self.reply); return len(wire)
        payload=b'\0'; drop=False
        if q.command==1:
            payload+=struct.pack('<IHHHIIHB',self.target,*self.version,APP_BASE,APP_CAPACITY,256,self.validation)
        elif q.command==5:
            payload+=struct.pack('<BIII',self.state,self.received,self.size,0)
        elif q.command==8:
            payload+=struct.pack('<IIII',self.erases,self.writes,self.cache_hits,self.sequence_rejects)
        elif q.command==2:
            self.size=struct.unpack_from('<I',q.payload,12)[0]
            self.state=2; self.erases+=1; self.memory=bytearray()
            drop=self.drop_begin; self.drop_begin=False
        elif q.command==3:
            if self.fail_write: payload=b'\x06'
            else:
                offset,=struct.unpack_from('<I',q.payload)
                assert offset==self.received
                self.memory.extend(q.payload[4:]); self.received=len(self.memory); self.writes+=1
                payload+=struct.pack('<I',self.received+(4 if self.wrong_offset else 0))
                drop=self.drop_write; self.drop_write=False
        elif q.command==4:
            if self.commit_error:
                payload=bytes([self.commit_error])
            else:
                self.commits+=1; self.state=4; self.validation=2
                drop=self.drop_end; self.drop_end=False
        else: raise AssertionError('unexpected/commit command')
        self.last=q; self.reply=encode(Frame(q.command|0x80,q.sequence,payload))
        if not drop: self.pending.extend(self.reply)
        return len(wire)


class TransferTests(unittest.TestCase):
    def run_transfer(self,b):
        package=Package((1,0,0),bytes(range(256))*2+b'abcde')
        transfer(b,package,progress=lambda msg:None)
        return package
    def test_transfer(self):
        b=Board(); p=self.run_transfer(b)
        self.assertEqual(b.memory,p.image)
        self.assertEqual((b.erases,b.writes),(1,3))
    def test_lost_begin_and_write_ack(self):
        b=Board(); b.drop_begin=True; b.drop_write=True
        ticks=iter(i*.01 for i in range(30000))
        with patch('fwtransfer.time.monotonic',side_effect=lambda:next(ticks)):
            self.run_transfer(b)
        self.assertEqual((b.erases,b.writes),(1,3))
        self.assertEqual(sum(q.command==2 for q in b.requests),2)
        self.assertEqual(sum(q.command==3 for q in b.requests),4)
    def test_wrong_board_and_old_bootloader_no_erase(self):
        for field,value in [('target',0),('version',(0,1,0)),('state',2)]:
            b=Board(); setattr(b,field,value)
            with self.assertRaises(ValueError): self.run_transfer(b)
            self.assertEqual(b.erases,0)
    def test_flash_failure_stops(self):
        b=Board(); b.fail_write=True
        with self.assertRaisesRegex(RuntimeError,'status=6'): self.run_transfer(b)
        self.assertEqual(sum(q.command==3 for q in b.requests),1)
    def test_bad_offset_ack_stops(self):
        b=Board(); b.wrong_offset=True
        with self.assertRaisesRegex(ValueError,'offset'): self.run_transfer(b)
        self.assertEqual(b.writes,1)

    def test_stage11_fault_injection(self):
        b=Board(); b.version=(0,2,1)
        p=Package((1,0,0),bytes(range(256))*2+b'abcde')
        ticks=iter(i*.01 for i in range(30000))
        logs=[]
        with patch('fwtransfer.time.monotonic',side_effect=lambda:next(ticks)):
            transfer(b,p,progress=logs.append,retry_test=True)
        self.assertEqual(b.memory,p.image)
        self.assertEqual((b.erases,b.writes,b.cache_hits),(1,3,3))
        self.assertTrue(any('RETRY TEST | PASS' in line for line in logs))
        self.assertEqual(sum('INJECT' in line for line in logs),3)

    def test_small_image_first_and_final_are_same_chunk(self):
        b=Board(); b.version=(0,2,1)
        ticks=iter(i*.01 for i in range(30000))
        with patch('fwtransfer.time.monotonic',side_effect=lambda:next(ticks)):
            transfer(b,Package((1,0,0),b'abcde'),progress=lambda _:None,retry_test=True)
        self.assertEqual((b.erases,b.writes,b.cache_hits),(1,1,2))

    def test_retry_requires_diagnostics_before_erase(self):
        b=Board()
        with self.assertRaisesRegex(ValueError,'0.2.1'):
            transfer(b,Package((1,0,0),b'abcde'),retry_test=True)
        self.assertEqual(b.erases,0)

    def test_false_counters_fail(self):
        class LyingBoard(Board):
            def write(self,wire):
                if decode(wire).command==8 and self.erases:
                    self.erases+=1
                return super().write(wire)
        b=LyingBoard(); b.version=(0,2,1)
        ticks=iter(i*.01 for i in range(30000))
        with patch('fwtransfer.time.monotonic',side_effect=lambda:next(ticks)):
            with self.assertRaisesRegex(ValueError,'Retry verification failed'):
                transfer(b,Package((1,0,0),b'abcde'),progress=lambda _:None,retry_test=True)

    def test_exhaustion_does_not_advance_or_restart(self):
        class SilentPort:
            in_waiting=0
            def __init__(self): self.wires=[]
            def write(self,wire): self.wires.append(wire); return len(wire)
            def read(self,n): return b''
        port=SilentPort(); client=Client(port)
        ticks=iter(i*.01 for i in range(10000))
        with patch('fwtransfer.time.monotonic',side_effect=lambda:next(ticks)):
            with self.assertRaises(TimeoutError): client.exchange(3,b'example')
        self.assertEqual(len(port.wires),4)
        self.assertEqual(len(set(port.wires)),1)
        self.assertEqual(client.sequence,1)

    def test_sequence_error_does_not_consume_sequence(self):
        class RejectBoard(Board):
            def write(self,wire):
                q=decode(wire)
                self.pending.extend(encode(Frame(q.command|0x80,q.sequence,b'\x05')))
                return len(wire)
        client=Client(RejectBoard())
        with self.assertRaisesRegex(RuntimeError,'status=5'): client.exchange(1)
        self.assertEqual(client.sequence,1)

    def test_commit(self):
        b=Board(); b.version=(0,3,0); logs=[]
        transfer(b,Package((1,0,0),bytes(range(256))+b'abcde'),progress=logs.append,commit=True)
        self.assertEqual((b.commits,b.state,b.validation),(1,4,2))
        self.assertTrue(any('COMMIT | PASS' in line for line in logs))
        self.assertTrue(any('disabled in bootloader 0.3.0' in line for line in logs))

    def test_verified_boot_version_and_reset_instruction(self):
        b=Board(); b.version=(0,4,0); logs=[]
        transfer(b,Package((1,0,0),bytes(range(256))+b'abcde'),progress=logs.append,commit=True)
        self.assertEqual((b.commits,b.state,b.validation),(1,4,2))
        self.assertTrue(any('press RESET without USER' in line for line in logs))

    def test_commit_old_firmware_stops_before_erase(self):
        b=Board()
        with self.assertRaisesRegex(ValueError,'Commit requires'):
            transfer(b,Package((1,0,0),b'abcde'),commit=True)
        self.assertEqual(b.erases,0)

    def test_commit_rejected_crc(self):
        b=Board(); b.version=(0,3,0); b.commit_error=7; logs=[]
        with self.assertRaisesRegex(RuntimeError,'status=7'):
            transfer(b,Package((1,0,0),b'abcde'),progress=logs.append,commit=True)
        self.assertEqual(b.commits,0)
        self.assertFalse(any('COMMIT | PASS' in line for line in logs))

    def test_lost_end_ack_not_recommitted(self):
        b=Board(); b.version=(0,3,0); b.drop_end=True
        ticks=iter(i*.01 for i in range(30000))
        with patch('fwtransfer.time.monotonic',side_effect=lambda:next(ticks)):
            transfer(b,Package((1,0,0),b'abcde'),progress=lambda _:None,commit=True)
        self.assertEqual(b.commits,1)
        ends=[q for q in b.requests if q.command==4]
        self.assertEqual(len(ends),2)
        self.assertEqual(ends[0],ends[1])

    def test_commit_missing_persistent_validation_rejected(self):
        class BadValidationBoard(Board):
            def write(self,wire):
                if decode(wire).command==1 and self.state==4: self.validation=1
                return super().write(wire)
        b=BadValidationBoard(); b.version=(0,3,0)
        with self.assertRaisesRegex(ValueError,'validation not confirmed'):
            transfer(b,Package((1,0,0),b'abcde'),progress=lambda _:None,commit=True)


if __name__=='__main__': unittest.main()
