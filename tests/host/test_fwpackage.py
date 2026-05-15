from pathlib import Path
import binascii
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from fwpackage import (APP_BASE, APP_CAPACITY, HEADER_SIZE, encode_package,
                       decode_package, parse_version, main)


def image(size=65, msp=0x20020000, reset=APP_BASE+9):
    return struct.pack('<II', msp, reset) + bytes([0x42]) * (size-8)


class PackageTests(unittest.TestCase):
    def test_roundtrip_and_begin_contract(self):
        raw=image()
        wire=encode_package(raw,(1,2,65535))
        p=decode_package(wire)
        self.assertEqual(wire[HEADER_SIZE:],raw)
        self.assertEqual(len(wire),36+len(raw))
        self.assertEqual(p.version,(1,2,65535))
        self.assertEqual(p.begin_payload(),struct.pack('<IHHHHII',0x04070001,1,2,65535,0,len(raw),binascii.crc32(raw)))
    def test_every_single_bit_corruption(self):
        wire=encode_package(image(),(1,0,0))
        for i in range(len(wire)):
            for bit in range(8):
                bad=bytearray(wire); bad[i]^=1<<bit
                with self.assertRaises(ValueError): decode_package(bad)
    def test_all_truncations_and_trailing(self):
        wire=encode_package(image(),(1,0,0))
        for size in range(len(wire)):
            with self.assertRaises(ValueError): decode_package(wire[:size])
        with self.assertRaises(ValueError): decode_package(wire+b'\xff')
    def test_semantic_header_checks_with_valid_crc(self):
        # CRC alone must not make an incompatible target/format acceptable.
        for offset,value in [(0,0),(4,2),(6,35),(8,0),(12,0),(22,1),(24,0)]:
            bad=bytearray(encode_package(image(),(1,0,0)))
            bad[offset]=value
            if offset in (8,24): bad[offset:offset+4]=b'\0'*4
            bad[32:36]=struct.pack('<I',binascii.crc32(bad[:32]))
            with self.assertRaises(ValueError): decode_package(bad)
    def test_image_boundaries_and_vectors(self):
        self.assertEqual(len(decode_package(encode_package(image(APP_CAPACITY),(0,0,0))).image),APP_CAPACITY)
        for raw in [b'',b'\0'*7,image(APP_CAPACITY+1),image(msp=0x20000000),
                    image(msp=0x20020008),image(msp=0x2001fffc),
                    image(reset=APP_BASE+8),image(reset=0x08000009),
                    image(reset=APP_BASE+65),image(reset=APP_BASE+7)]:
            with self.assertRaises(ValueError): encode_package(raw,(1,0,0))
    def test_version(self):
        self.assertEqual(parse_version('65535.0.1'),(65535,0,1))
        for text in ['1.2','1.2.3.4','-1.0.0','65536.0.0','v1.0.0','1.a.0']:
            with self.assertRaises(ValueError): parse_version(text)
    def test_cli_preserves_existing_files(self):
        with tempfile.TemporaryDirectory() as tmp, patch('sys.stdout'), patch('sys.stderr'):
            src=Path(tmp)/'app.bin'; dst=Path(tmp)/'app.fwp'; src.write_bytes(image())
            args=['pack','--input',str(src),'--output',str(dst),'--version','1.0.0']
            self.assertEqual(main(args),0)
            original=dst.read_bytes()
            self.assertEqual(main(args),1)
            self.assertEqual(dst.read_bytes(),original)
            self.assertEqual(main(['inspect',str(dst)]),0)
            args[4]=str(src)
            self.assertEqual(main(args),1)
            self.assertEqual(src.read_bytes(),image())


if __name__ == '__main__': unittest.main()
