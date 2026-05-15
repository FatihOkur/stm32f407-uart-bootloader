"""Offline firmware package creation and validation; never opens a serial port."""
import argparse
import binascii
from dataclasses import dataclass
from pathlib import Path
import re
import struct
import sys

TARGET_ID = 0x04070001
APP_BASE = 0x08020000
APP_CAPACITY = 0xE0000
MAGIC = b'F407'
FORMAT_VERSION = 1
# magic, format, header size, load address, BEGIN_UPDATE payload, header CRC
HEADER = struct.Struct('<4sHHIIHHHHIII')
HEADER_SIZE = HEADER.size  # 36 bytes
BEGIN = struct.Struct('<IHHHHII')


@dataclass(frozen=True)
class Package:
    version: tuple[int, int, int]
    image: bytes

    @property
    def image_crc(self):
        return binascii.crc32(self.image)

    def begin_payload(self):
        return BEGIN.pack(TARGET_ID, *self.version, 0, len(self.image), self.image_crc)


def parse_version(text):
    if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', text):
        raise ValueError('version must be MAJOR.MINOR.PATCH, e.g. 1.0.0')
    version = tuple(map(int, text.split('.')))
    if any(v > 65535 for v in version):
        raise ValueError('each version component must be in 0..65535')
    return version


def validate_image(image):
    if not 8 <= len(image) <= APP_CAPACITY:
        raise ValueError('image size must be 8..917504 bytes')
    msp, reset = struct.unpack_from('<II', image)
    if not 0x20000000 < msp <= 0x20020000 or msp % 8:
        raise ValueError('invalid initial MSP (requires aligned main SRAM stack)')
    # Stricter than the stage 8 boot check: entry must be inside this image.
    if not reset & 1 or not APP_BASE + 8 <= (reset & ~1) <= APP_BASE + len(image) - 2:
        raise ValueError('reset vector must be Thumb code inside image at 0x08020000')


def encode_package(image, version):
    validate_image(image)
    if len(version) != 3 or any(type(v) is not int or not 0 <= v <= 65535 for v in version):
        raise ValueError('invalid version tuple')
    package = Package(tuple(version), bytes(image))
    header = HEADER.pack(MAGIC, FORMAT_VERSION, HEADER_SIZE, APP_BASE,
                         TARGET_ID, *version, 0, len(image), package.image_crc, 0)
    header = header[:-4] + struct.pack('<I', binascii.crc32(header[:-4]))
    return header + image


def decode_package(data):
    if len(data) < HEADER_SIZE:
        raise ValueError('truncated package header')
    magic, fmt, size, base, target, major, minor, patch, reserved, length, crc, hcrc = HEADER.unpack_from(data)
    if magic != MAGIC or fmt != FORMAT_VERSION or size != HEADER_SIZE:
        raise ValueError('unsupported package magic/format/header size')
    if binascii.crc32(data[:HEADER_SIZE-4]) != hcrc:
        raise ValueError('header CRC mismatch')
    if target != TARGET_ID or base != APP_BASE or reserved != 0:
        raise ValueError('wrong target/load address or nonzero reserved field')
    if not 8 <= length <= APP_CAPACITY or len(data) != HEADER_SIZE + length:
        raise ValueError('invalid image size or truncated/trailing package data')
    image = bytes(data[HEADER_SIZE:])
    if binascii.crc32(image) != crc:
        raise ValueError('image CRC mismatch')
    validate_image(image)
    return Package((major, minor, patch), image)


def read_bounded(path, limit):
    with Path(path).open('rb') as stream:
        data = stream.read(limit + 1)
    if len(data) > limit:
        raise ValueError(f'file exceeds maximum size ({limit} bytes)')
    return data


def describe(package):
    print('PACKAGE | OK')
    print(f'Target ID: 0x{TARGET_ID:08X}')
    print('Firmware version: ' + '.'.join(map(str, package.version)))
    print(f'Application base: 0x{APP_BASE:08X}')
    print(f'Image size: {len(package.image)} bytes')
    print(f'Image CRC32: 0x{package.image_crc:08X}')
    print(f'Package size: {HEADER_SIZE + len(package.image)} bytes')
    print('Vectors: sane (not a guarantee of executable correctness)')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    pack = commands.add_parser('pack', help='wrap an application .bin in a .fwp package')
    pack.add_argument('--input', type=Path, required=True)
    pack.add_argument('--output', type=Path, required=True)
    pack.add_argument('--version', required=True)
    inspect = commands.add_parser('inspect', help='validate a package and print its metadata')
    inspect.add_argument('package', type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == 'pack':
            data = encode_package(read_bounded(args.input, APP_CAPACITY), parse_version(args.version))
            package = decode_package(data)
            # Exclusive create protects both existing output and accidental input=output.
            with args.output.open('xb') as stream:
                stream.write(data)
            print(f'Created: {args.output.resolve()}')
        else:
            package = decode_package(read_bounded(args.package, HEADER_SIZE + APP_CAPACITY))
        describe(package)
    except (OSError, ValueError) as exc:
        print(f'ERROR: {exc}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
