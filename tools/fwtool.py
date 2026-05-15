"""Read-only step 8 client. Run: py tools/fwtool.py info --port COM8."""
import argparse
import struct
import sys
import time
from protocol import SOF, MAX_PAYLOAD, Frame, Command, encode, decode


class ResponseReader:
    def __init__(self):
        self.buffer = bytearray()
        self.last_byte = 0.0

    def feed(self, data, now):
        if self.buffer and now - self.last_byte >= 0.1:
            self.buffer.clear()
        if data:
            self.last_byte = now
            self.buffer.extend(data)
        frames = []
        while self.buffer:
            start = self.buffer.find(SOF)
            if start < 0:
                self.buffer[:] = b'\xa5' if self.buffer[-1] == 0xa5 else b''
                break
            del self.buffer[:start]
            if len(self.buffer) < 8:
                break
            length = int.from_bytes(self.buffer[6:8], 'little')
            if length > MAX_PAYLOAD:
                del self.buffer[0]
                continue
            total = 12 + length
            if len(self.buffer) < total:
                break
            try:
                frame = decode(bytes(self.buffer[:total]))
            except ValueError:
                del self.buffer[0]
                continue
            del self.buffer[:total]
            frames.append(frame)
        return frames


def request_info(port, sequence=1, timeout=0.5, retries=3):
    packet = encode(Frame(Command.GET_INFO, sequence))
    reader = ResponseReader()
    for attempt in range(retries + 1):
        if port.write(packet) != len(packet):
            raise OSError('Incomplete serial write')
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            # Bounded read; serial timeout must be much shorter than RX gap.
            data = port.read(min(max(port.in_waiting, 1), 272))
            for frame in reader.feed(data, time.monotonic()):
                if frame.command != 0x81 or frame.sequence != sequence:
                    continue
                if not frame.payload:
                    continue
                if frame.payload[0] != 0:
                    if len(frame.payload) != 1:
                        continue
                    raise RuntimeError(f'GET_INFO status={frame.payload[0]}; '
                                       'reset into bootloader and retry')
                if len(frame.payload) != 22:
                    continue
                values = struct.unpack('<IHHHIIHB', frame.payload[1:])
                if values[-1] not in (0, 1, 2):
                    continue
                return values
    raise TimeoutError('No valid GET_INFO response after 4 attempts. '
                       'Check COM port and reset with USER held for 2 seconds.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['info'])
    parser.add_argument('--port', required=True)
    args = parser.parse_args()
    try:
        import serial
    except ImportError:
        parser.exit(1, 'pyserial missing. Run: py -m pip install -r tools/requirements.txt\n')
    try:
        # PuTTY must release the port. Hardware flow control is disabled.
        with serial.Serial(args.port, 115200, timeout=0.02, write_timeout=0.5,
                           xonxoff=False, rtscts=False, dsrdtr=False) as port:
            port.reset_input_buffer()
            info = request_info(port)
        target, major, minor, patch, base, capacity, chunk, validation = info
        print('GET_INFO | OK')
        print(f'Target ID: 0x{target:08X}')
        print(f'Bootloader: {major}.{minor}.{patch}')
        print(f'Application base: 0x{base:08X}')
        print(f'Application capacity: {capacity} bytes ({capacity // 1024} KiB)')
        print(f'Max chunk: {chunk} bytes')
        labels = ['no usable vectors', 'vectors sane only (image CRC not checked)',
                  'metadata + image CRC verified']
        print(f'Application validation: {validation} - {labels[validation]}')
    except (OSError, RuntimeError, TimeoutError) as exc:
        print(f'ERROR: {exc}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
