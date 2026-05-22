"""Stage 14 controlled destructive application tests; bootloader 0.4.0 required.

Leaves the application UNCOMMITTED. Recover using fwtransfer.py --commit after reset.
No direct flash writes, bootloader changes, or corruption of the source FWP file.
"""
import argparse
import binascii
import struct
import sys
from fwpackage import (APP_BASE, APP_CAPACITY, TARGET_ID, HEADER_SIZE,
                       decode_package, read_bounded)
from fwtransfer import Client, CommandRejected
from protocol import Command


def prepare(package, case, after_bytes):
    image = package.image
    crc = package.image_crc
    limit = len(image)
    if case == 'partial':
        if not 0 < after_bytes < len(image) or after_bytes % 4:
            raise ValueError('--after-bytes must be word aligned, positive and smaller than the image')
        limit = after_bytes
    elif case == 'bad-crc':
        # Keep vectors intact; change actual data but announce the original CRC.
        if len(image) <= 8:
            raise ValueError('bad-crc requires image data after the initial vectors')
        image = image[:-1] + bytes([image[-1] ^ 1])
    elif case == 'bad-vector':
        # Invalid MSP with a matching IMAGE CRC isolates vector rejection.
        image = b'\0\0\0\0' + image[4:]
        crc = binascii.crc32(image)
    elif case != 'uncommitted':
        raise ValueError('unknown fault case')
    begin = struct.pack('<IHHHHII', TARGET_ID, *package.version, 0, len(image), crc)
    return image, begin, limit


def run_fault(port, package, case, after_bytes=4096, progress=print):
    image, begin, limit = prepare(package, case, after_bytes)
    client = Client(port)
    info = struct.unpack('<IHHHIIHB', client.exchange(Command.GET_INFO, response_size=22, timeout=30))
    if info[:7] != (TARGET_ID, 0, 4, 0, APP_BASE, APP_CAPACITY, 256) or info[-1] not in (0, 1, 2):
        raise ValueError('Fault test requires expected target/map and bootloader 0.4.0')
    status = struct.unpack('<BIII', client.exchange(Command.GET_STATUS, response_size=14))
    if status != (0, 0, 0, 0):
        raise ValueError('Reset into bootloader before starting a new fault test')
    initial = struct.unpack('<IIII', client.exchange(Command.GET_DIAGNOSTICS, response_size=17))
    if initial[0] or initial[1] or initial[3]:
        raise ValueError('Diagnostic counters are not fresh; reset first')
    progress(f'FAULT | {case} | replacing application; original package unchanged')
    client.exchange(Command.BEGIN_UPDATE, begin, timeout=45)
    progress('BEGIN | OK | metadata invalidated and application area prepared')
    for offset in range(0, limit, 256):
        data = image[offset:min(offset+256, limit)]
        reply = client.exchange(Command.WRITE_CHUNK, struct.pack('<I', offset)+data, response_size=5)
        if struct.unpack('<I', reply)[0] != offset+len(data):
            raise ValueError('Incorrect write offset acknowledgement; stop')
    progress(f'WRITE | {limit}/{len(image)} bytes acknowledged')

    errors = {'partial': (9, 'INCOMPLETE', 2), 'bad-crc': (7, 'IMAGE_CRC', 5),
              'bad-vector': (10, 'VECTOR', 5)}
    expected_state = 2
    if case in errors:
        expected, name, expected_state = errors[case]
        try:
            client.exchange(Command.END_UPDATE, timeout=30)
        except CommandRejected as exc:
            if exc.command != Command.END_UPDATE or exc.status != expected:
                raise
            progress(f'END | expected rejection | {name} ({expected})')
        else:
            raise RuntimeError('FAULT TEST FAILED: device accepted an invalid/incomplete image')
    else:
        progress('END | deliberately not sent | complete bytes without commit')
    status = struct.unpack('<BIII', client.exchange(Command.GET_STATUS, response_size=14))
    if status != (expected_state, limit, len(image), 0):
        raise ValueError(f'Unexpected state after fault: {status}')
    info = struct.unpack('<IHHHIIHB', client.exchange(Command.GET_INFO, response_size=22, timeout=30))
    expected_validation = 0 if case == 'bad-vector' else 1
    if info[-1] != expected_validation:
        raise ValueError(f'Unexpected validation: {info[-1]}, expected {expected_validation}')
    final = struct.unpack('<IIII', client.exchange(Command.GET_DIAGNOSTICS, response_size=17))
    if final[0]-initial[0] != 1 or final[1]-initial[1] != (limit+255)//256 or final[3] != initial[3]:
        raise ValueError('Unexpected erase/write/sequence counters')
    progress(f'VALIDATION | {info[-1]} | not committed; must not boot')
    progress('FAULT SETUP | PASS | reset/power-cycle behavior still requires hardware observation')
    progress('RECOVERY | reset, then upload the original package with fwtransfer.py --commit')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('case', choices=['partial', 'bad-crc', 'bad-vector', 'uncommitted'])
    parser.add_argument('--port', required=True)
    parser.add_argument('--package', required=True)
    parser.add_argument('--after-bytes', type=int, default=4096,
                        help='partial case only: acknowledged prefix size (default 4096)')
    args = parser.parse_args(argv)
    try:
        package = decode_package(read_bounded(args.package, HEADER_SIZE+APP_CAPACITY))
        prepare(package, args.case, args.after_bytes)  # Reject bad parameters before opening COM.
        import serial
        with serial.Serial(args.port, 115200, timeout=.02, write_timeout=.5,
                           xonxoff=False, rtscts=False, dsrdtr=False) as port:
            port.reset_input_buffer()
            run_fault(port, package, args.case, args.after_bytes)
    except ImportError:
        print('ERROR: install tools/requirements.txt with py -m pip', file=sys.stderr)
        return 1
    except (OSError, RuntimeError, ValueError) as exc:
        print(f'ERROR: {exc}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
