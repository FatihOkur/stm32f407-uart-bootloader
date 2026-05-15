"""Runs against the actual C parser via its native test executable."""
from pathlib import Path
import binascii
import random
import struct
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from protocol import Frame, Command, encode, decode

BRIDGE = ROOT / "tests/host/build/test_protocol.exe"


class ProtocolTests(unittest.TestCase):
    def bridge(self, mode, rows):
        result = subprocess.run([str(BRIDGE), mode],
                                input="\n".join(row.hex() for row in rows) + "\n",
                                text=True, capture_output=True, check=True)
        return result.stdout.splitlines()

    def test_c_regression(self):
        subprocess.run([str(BRIDGE)], check=True, capture_output=True)

    def test_get_info_layout(self):
        packet = encode(Frame(Command.GET_INFO, 1))
        self.assertEqual(packet[:8], bytes.fromhex("a5 5a 01 01 01 00 00 00"))
        self.assertEqual(len(packet), 12)
        self.assertEqual(decode(packet), Frame(1, 1))

    def test_crc_cross_language(self):
        rng = random.Random(407)
        rows = [b"", b"123456789"] + [rng.randbytes(n) for n in range(273)]
        self.assertEqual(self.bridge("--crc", rows),
                         [f"{binascii.crc32(row):08x}" for row in rows])

    def test_frame_cross_language(self):
        rng = random.Random(20260924)
        frames = [Frame(rng.randrange(256), rng.randrange(65536), rng.randbytes(n))
                  for n in range(261)]
        packets = [encode(frame) for frame in frames]
        self.assertEqual(self.bridge("--frames", packets), [p.hex() for p in packets])
        self.assertEqual([decode(p) for p in packets], frames)

    def test_coalesced_frames_and_noise(self):
        a, b = encode(Frame(1, 7)), encode(Frame(0x81, 7, b"\0"))
        self.assertEqual(self.bridge("--frames", [b"noise\xa5" + a + b]),
                         [a.hex(), b.hex()])

    def test_single_bit_corruptions(self):
        packet = encode(Frame(3, 12, bytes(range(32))))
        for index in range(len(packet)):
            for bit in range(8):
                bad = bytearray(packet)
                bad[index] ^= 1 << bit
                with self.assertRaises(ValueError):
                    decode(bad)

    def test_input_limits(self):
        for frame in (Frame(1, -1), Frame(1, 65536), Frame(256, 1), Frame(1, 1, b"x" * 261)):
            with self.assertRaises(ValueError):
                encode(frame)
        packet = encode(Frame(1, 1))
        for bad in (b"", packet[:-1], packet+b"x"):
            with self.assertRaises(ValueError):
                decode(bad)

    def test_unsupported_version_with_correct_crc(self):
        bad = bytearray(encode(Frame(1, 1)))
        bad[2] = 2
        struct.pack_into("<I", bad, 8, binascii.crc32(bad[2:8]))
        with self.assertRaisesRegex(ValueError, "version"):
            decode(bad)
        self.assertEqual(self.bridge("--frames", [bad]), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
