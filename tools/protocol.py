"""Protocol v1 byte codec, no serial port access and no update commands yet."""
from dataclasses import dataclass
from enum import IntEnum
import binascii
import struct

SOF = b"\xa5\x5a"
VERSION = 1
MAX_PAYLOAD = 260
MAX_FRAME = 272


class Command(IntEnum):
    GET_INFO = 1
    BEGIN_UPDATE = 2
    WRITE_CHUNK = 3
    END_UPDATE = 4
    GET_STATUS = 5
    ABORT = 6
    REBOOT = 7
    GET_DIAGNOSTICS = 8


@dataclass(frozen=True)
class Frame:
    command: int
    sequence: int
    payload: bytes = b""


def encode(frame: Frame) -> bytes:
    if not 0 <= frame.command <= 255:
        raise ValueError("command must fit uint8")
    if not 0 <= frame.sequence <= 65535:
        raise ValueError("sequence must fit uint16")
    if len(frame.payload) > MAX_PAYLOAD:
        raise ValueError("payload too large")
    body = struct.pack("<BBHH", VERSION, frame.command, frame.sequence, len(frame.payload))
    body += frame.payload
    return SOF + body + struct.pack("<I", binascii.crc32(body))


def decode(packet: bytes) -> Frame:
    """Decode exactly one complete frame. Stream buffering is caller-owned."""
    if len(packet) < 12 or packet[:2] != SOF:
        raise ValueError("invalid frame start/size")
    version, command, sequence, length = struct.unpack_from("<BBHH", packet, 2)
    if length > MAX_PAYLOAD or len(packet) != 12 + length:
        raise ValueError("invalid payload length")
    expected, = struct.unpack_from("<I", packet, 8 + length)
    if binascii.crc32(packet[2:8 + length]) != expected:
        raise ValueError("CRC mismatch")
    if version != VERSION:
        raise ValueError("unsupported protocol version")
    return Frame(command, sequence, bytes(packet[8:8 + length]))
