import binascii
from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[2]/'tools'))
from fwpackage import Package, APP_BASE
from fwfault import prepare, run_fault
from fwtransfer import CommandRejected
from protocol import Frame, encode, decode
from test_fwtransfer import Board


class FaultBoard(Board):
    def __init__(self):
        super().__init__()
        self.version=(0,4,0); self.accept_bad=False; self.end_error=None
    def write(self, wire):
        q=decode(wire)
        if q==self.last:
            return super().write(wire)
        if q.command==2:
            self.expected_crc=struct.unpack_from('<I',q.payload,16)[0]
        elif q.command==1:
            self.validation=1 if len(self.memory)>=8 and struct.unpack_from('<I',self.memory)[0] else 0
        elif q.command==4:
            self.requests.append(q)
            if self.end_error is not None: status=self.end_error
            elif len(self.memory)<self.size: status=9
            elif struct.unpack_from('<I',self.memory)[0]==0: status=10
            elif binascii.crc32(self.memory)!=self.expected_crc: status=7
            else: status=0
            if self.accept_bad: status=0
            self.state=2 if status==9 else (5 if status else 4)
            self.last=q; self.reply=encode(Frame(0x84,q.sequence,bytes([status])))
            self.pending.extend(self.reply)
            return len(wire)
        return super().write(wire)


class FaultTests(unittest.TestCase):
    def package(self):
        data=struct.pack('<II',0x20020000,APP_BASE+9)+bytes(range(256))*20
        return Package((1,0,0),data)

    def test_partial_stops_at_boundary_and_requires_incomplete(self):
        b=FaultBoard(); logs=[]; p=self.package()
        run_fault(b,p,'partial',progress=logs.append)
        self.assertEqual(bytes(b.memory),p.image[:4096])
        self.assertEqual((b.erases,b.writes,b.state),(1,16,2))
        self.assertTrue(any('INCOMPLETE (9)' in line for line in logs))

    def test_bad_crc_actual_changed_data_with_valid_transport(self):
        b=FaultBoard(); p=self.package(); original=p.image; logs=[]
        run_fault(b,p,'bad-crc',progress=logs.append)
        self.assertEqual(p.image,original)
        self.assertEqual(b.memory[:-1],p.image[:-1])
        self.assertNotEqual(b.memory[-1],p.image[-1])
        self.assertEqual(b.expected_crc,p.image_crc)
        self.assertEqual(b.state,5)
        self.assertTrue(any('IMAGE_CRC (7)' in line for line in logs))

    def test_bad_vector_with_matching_image_crc(self):
        b=FaultBoard(); logs=[]
        run_fault(b,self.package(),'bad-vector',progress=logs.append)
        self.assertEqual(b.memory[:4],b'\0'*4)
        self.assertEqual(b.expected_crc,binascii.crc32(b.memory))
        self.assertTrue(any('VECTOR (10)' in line for line in logs))

    def test_uncommitted_never_sends_end(self):
        b=FaultBoard(); p=self.package()
        run_fault(b,p,'uncommitted',progress=lambda _:None)
        self.assertEqual(b.memory,p.image)
        self.assertFalse(any(q.command==4 for q in b.requests))
        self.assertEqual(b.state,2)

    def test_unexpected_acceptance_fails(self):
        b=FaultBoard(); b.accept_bad=True
        with self.assertRaisesRegex(RuntimeError,'accepted'):
            run_fault(b,self.package(),'partial',progress=lambda _:None)

    def test_wrong_rejection_is_not_success(self):
        b=FaultBoard(); b.end_error=3
        with self.assertRaises(CommandRejected) as result:
            run_fault(b,self.package(),'partial',progress=lambda _:None)
        self.assertEqual(result.exception.status,3)

    def test_timeout_is_not_expected_rejection(self):
        class TimeoutBoard(FaultBoard):
            def write(self,wire):
                if decode(wire).command==4: raise TimeoutError('injected transport failure')
                return super().write(wire)
        with self.assertRaises(TimeoutError):
            run_fault(TimeoutBoard(),self.package(),'partial',progress=lambda _:None)

    def test_invalid_partial_preflight_never_touches_port(self):
        p=self.package()
        for size in [0,3,len(p.image),len(p.image)+4]:
            b=FaultBoard()
            with self.assertRaises(ValueError): run_fault(b,p,'partial',size,lambda _:None)
            self.assertEqual(b.requests,[])

    def test_wrong_bootloader_before_erasing(self):
        b=FaultBoard(); b.version=(0,3,0)
        with self.assertRaises(ValueError): run_fault(b,self.package(),'partial',progress=lambda _:None)
        self.assertEqual(b.erases,0)


if __name__=='__main__': unittest.main()
