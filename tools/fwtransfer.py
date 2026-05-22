"""Transfer firmware and optionally commit; reset to boot a verified image on 0.4.0."""
import argparse
from contextlib import nullcontext
import json
from pathlib import Path
import struct
import sys
import time
from protocol import Command, Frame, encode
from transfer_metrics import TransferMetrics
from fwtool import ResponseReader
from fwpackage import (decode_package, read_bounded, HEADER_SIZE, APP_CAPACITY,
                       APP_BASE, TARGET_ID)


class CommandRejected(RuntimeError):
    """A well-formed, matching device error response (not a transport timeout)."""
    def __init__(self, command, sequence, status):
        self.command = command
        self.sequence = sequence
        self.status = status
        super().__init__(f'Command 0x{command:02X} rejected: status={status}. '
                         'Stop; reset before starting a new session.')


class Client:
    def __init__(self, port, ignore_response=None, metrics=None):
        self.port = port
        self.sequence = 1
        self.ignore_response = ignore_response
        self.metrics = metrics
        self.attempts = 0

    def exchange(self, command, payload=b'', response_size=1, timeout=0.5):
        if self.metrics is None:
            return self._exchange(command, payload, response_size, timeout)
        started = self.metrics.clock()
        sequence = self.sequence
        self.attempts = 0
        outcome, status = 'error', None
        try:
            result = self._exchange(command, payload, response_size, timeout)
            outcome, status = 'ok', 0
            return result
        except CommandRejected as exc:
            outcome, status = 'rejected', exc.status
            raise
        finally:
            self.metrics.record(command, sequence, len(payload), self.metrics.clock()-started,
                                self.attempts, outcome, status)

    def _exchange(self, command, payload, response_size, timeout):
        packet = encode(Frame(command, self.sequence, payload))
        reader = ResponseReader()
        for attempt in range(4):
            self.attempts = attempt+1
            if self.port.write(packet) != len(packet):
                raise OSError('Incomplete serial write; transfer outcome uncertain')
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                data = self.port.read(min(max(self.port.in_waiting, 1), 272))
                for frame in reader.feed(data, time.monotonic()):
                    if frame.command != command | 0x80 or frame.sequence != self.sequence:
                        continue
                    if not frame.payload:
                        continue
                    status = frame.payload[0]
                    if len(frame.payload) != (1 if status else response_size):
                        continue
                    if not status and self.ignore_response is not None and self.ignore_response(command, frame):
                        continue  # Simulated lost ACK: preserve sequence, wait for normal timeout.
                    # SEQUENCE rejects do not consume the board's expected sequence.
                    if status != 5:
                        self.sequence = (self.sequence + 1) & 0xffff
                    if status:
                        raise CommandRejected(command, frame.sequence, status)
                    return frame.payload[1:]
        raise TimeoutError(f'Command 0x{command:02X}: no valid response after 4 attempts; '
                           'outcome uncertain. Stop and reset into bootloader before retrying.')


class LostAckTest:
    def __init__(self, image_size, progress):
        self.progress = progress
        self.pending = {('begin', 0), ('write', min(256, image_size)), ('write', image_size)}
        self.dropped = 0

    def __call__(self, command, frame):
        if command == Command.BEGIN_UPDATE:
            key = ('begin', 0)
        elif command == Command.WRITE_CHUNK:
            key = ('write', struct.unpack('<I', frame.payload[1:])[0])
        else:
            return False
        if key not in self.pending:
            return False
        self.pending.remove(key)
        self.dropped += 1
        wait = '45 seconds' if key[0] == 'begin' else '0.5 seconds'
        self.progress(f'INJECT | ignoring {key[0]} ACK (sequence={frame.sequence}, next_offset={key[1]}); timeout {wait}')
        return True


def transfer(port, package, progress=print, retry_test=False, commit=False, metrics=None):
    if metrics is not None:
        metrics.start(len(package.image), package.image_crc, package.version, commit, retry_test)
    faults = LostAckTest(len(package.image), progress) if retry_test else None
    client = Client(port, ignore_response=faults, metrics=metrics)
    info = struct.unpack('<IHHHIIHB', client.exchange(Command.GET_INFO, response_size=22, timeout=30.0))
    target, major, minor, patch, base, capacity, chunk, validation = info
    if metrics is not None:
        metrics.metadata.update(bootloader_version=f'{major}.{minor}.{patch}', initial_validation=validation)
    if (target, base, capacity) != (TARGET_ID, APP_BASE, APP_CAPACITY):
        raise ValueError('Board target/memory map does not match package')
    if (major, minor, patch) not in ((0, 2, 0), (0, 2, 1), (0, 3, 0), (0, 4, 0)):
        raise ValueError('Unsupported bootloader version (expected 0.2.x, 0.3.0 or 0.4.0)')
    if retry_test and (major, minor, patch) not in ((0, 2, 1), (0, 3, 0), (0, 4, 0)):
        raise ValueError('Retry diagnostics require bootloader 0.2.1, 0.3.0 or 0.4.0; load the new ELF first')
    if commit and (major, minor, patch) not in ((0, 3, 0), (0, 4, 0)):
        raise ValueError('Commit requires bootloader 0.3.0 or 0.4.0; load the new ELF first')
    if chunk != 256 or validation not in (0, 1, 2):
        raise ValueError('Unexpected GET_INFO capabilities')
    # Confirm fresh session before any destructive operation.
    status = struct.unpack('<BIII', client.exchange(Command.GET_STATUS, response_size=14))
    if status != (0, 0, 0, 0):
        raise ValueError('Board is not idle; reset before a new transfer')
    if retry_test:
        initial = struct.unpack('<IIII', client.exchange(Command.GET_DIAGNOSTICS, response_size=17))
        if initial[0] or initial[1] or initial[3]:
            raise ValueError('Diagnostic counters are not fresh; reset before retry test')
    progress(f'BEGIN | erasing application sectors for {len(package.image)} bytes')
    client.exchange(Command.BEGIN_UPDATE, package.begin_payload(), timeout=45.0)
    data_started = metrics.clock() if metrics is not None else None
    for offset in range(0, len(package.image), chunk):
        data = package.image[offset:offset+chunk]
        reply = client.exchange(Command.WRITE_CHUNK, struct.pack('<I', offset)+data,
                                response_size=5)
        received, = struct.unpack('<I', reply)
        if received != offset+len(data):
            raise ValueError('Incorrect write acknowledgement offset; stopping')
        if received % 4096 == 0 or received == len(package.image):
            progress(f'WRITE | {received}/{len(package.image)} bytes verified')
    if metrics is not None:
        metrics.data_ns = metrics.clock()-data_started
    state, received, declared, hal_error = struct.unpack(
        '<BIII', client.exchange(Command.GET_STATUS, response_size=14))
    if (state, received, declared, hal_error) != (2, len(package.image), len(package.image), 0):
        raise ValueError('Unexpected final transfer state; stopping')
    if retry_test:
        final = struct.unpack('<IIII', client.exchange(Command.GET_DIAGNOSTICS, response_size=17))
        erases, writes, hits, rejects = (after-before for before, after in zip(initial, final))
        expected_writes = (len(package.image)+chunk-1)//chunk
        if faults.pending or erases != 1 or writes != expected_writes or hits < faults.dropped or rejects != 0:
            raise ValueError(f'Retry verification failed: erases={erases}, writes={writes}, cache_hits={hits}, sequence_rejects={rejects}')
        progress(f'DIAG | erase_calls={erases} write_calls={writes} cache_hits={hits} sequence_rejects={rejects}')
        progress(f'RETRY TEST | PASS | {faults.dropped} ACKs ignored; no repeated erase/write')
    progress('TRANSFER | PASS | all chunks written and read back')
    if not commit:
        progress('COMMIT | NOT REQUESTED | image not committed; application will not boot')
        if metrics is not None:
            metrics.finish('transferred_not_committed')
        return
    progress('END | verifying full flash image CRC and committing metadata')
    client.exchange(Command.END_UPDATE, timeout=30.0)
    status = struct.unpack('<BIII', client.exchange(Command.GET_STATUS, response_size=14))
    if status != (4, len(package.image), len(package.image), 0):
        raise ValueError('END acknowledged but COMMITTED status not confirmed')
    info = struct.unpack('<IHHHIIHB', client.exchange(Command.GET_INFO, response_size=22, timeout=30.0))
    if info[-1] != 2:
        raise ValueError('Stored metadata/full-image validation not confirmed')
    if metrics is not None:
        metrics.finish('committed')
    progress(f'COMMIT | PASS | size={len(package.image)} crc32=0x{package.image_crc:08X}')
    progress('VALIDATION | 2 | metadata + full flash image CRC verified')
    if (major, minor, patch) == (0, 4, 0):
        progress('BOOT | ready | press RESET without USER to verify and start application')
    else:
        progress('BOOT | disabled in bootloader 0.3.0')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--package', required=True)
    parser.add_argument('--retry-test', action='store_true',
                        help='ignore BEGIN/first/final WRITE replies and verify board counters; takes about 1 minute')
    parser.add_argument('--commit', action='store_true',
                        help='END_UPDATE: verify image CRC and write persistent metadata (requires 0.3.0/0.4.0)')
    parser.add_argument('--report', type=Path,
                        help='save host timing measurements as a NEW JSON file (parent directory must exist)')
    args = parser.parse_args(argv)
    try:
        # Entire container and image are validated before opening the serial port.
        package = decode_package(read_bounded(args.package, HEADER_SIZE+APP_CAPACITY))
        import serial
        metrics = TransferMetrics() if args.report else None
        if metrics is not None:
            metrics.metadata.update(port=args.port, package_file=str(Path(args.package).resolve()))
        # Reserve a NEW report before serial access so an existing file is never
        # overwritten, and an invalid path cannot cause an unnecessary flash erase.
        with (args.report.open('x', encoding='utf-8') if args.report else nullcontext()) as report_file:
            try:
                with serial.Serial(args.port, 115200, timeout=.02, write_timeout=.5,
                                   xonxoff=False, rtscts=False, dsrdtr=False) as port:
                    port.reset_input_buffer()
                    transfer(port, package, retry_test=args.retry_test, commit=args.commit, metrics=metrics)
            except BaseException as exc:
                if metrics is not None:
                    metrics.finish('failed', f'{type(exc).__name__}: {exc}')
                raise
            finally:
                if report_file is not None:
                    json.dump(metrics.report(), report_file, indent=2, allow_nan=False)
                    report_file.write('\n')
        if metrics is not None:
            metrics.print_summary()
            print(f'REPORT | {args.report.resolve()}')
    except ImportError:
        print('ERROR: install tools/requirements.txt with py -m pip', file=sys.stderr)
        return 1
    except (OSError, RuntimeError, ValueError) as exc:
        print(f'ERROR: {exc}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
