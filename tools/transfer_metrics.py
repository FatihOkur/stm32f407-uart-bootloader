"""Host-observed timings: USB/OS/Python/wire/device work are all included."""
from datetime import datetime, timezone
import math
import platform
import statistics
import time


def distribution(values):
    if not values:
        return None
    ordered = sorted(values)
    return {'count': len(values), 'min_ms': ordered[0], 'mean_ms': statistics.mean(values),
            'median_ms': statistics.median(values),
            'p95_ms': ordered[math.ceil(.95*len(values))-1], 'max_ms': ordered[-1]}


class TransferMetrics:
    def __init__(self, clock=None):
        self.clock = clock or time.perf_counter_ns
        self.started_utc = datetime.now(timezone.utc).isoformat()
        self.start_ns = None
        self.elapsed_ns = None
        self.data_ns = None
        self.status = 'not_started'
        self.error = None
        self.records = []
        self.metadata = {'python': platform.python_version(), 'os': platform.platform(),
                         'baud': 115200, 'framing': '8N1', 'max_chunk_bytes': 256}

    def start(self, image_size, image_crc, firmware_version, commit, retry_test):
        self.metadata.update(image_size=image_size, image_crc32=f'0x{image_crc:08X}',
                             firmware_version='.'.join(map(str, firmware_version)),
                             commit_requested=commit, fault_injection=retry_test)
        self.status = 'running'
        self.start_ns = self.clock()

    def record(self, command, sequence, payload_size, elapsed_ns, attempts, outcome, status):
        self.records.append({'command': int(command), 'sequence': sequence,
                             'request_payload_bytes': payload_size,
                             'elapsed_ms': elapsed_ns/1_000_000, 'attempts': attempts,
                             'retries': max(attempts-1, 0), 'outcome': outcome, 'status': status})

    def finish(self, status, error=None):
        self.status = status
        self.error = error
        if self.start_ns is not None:
            self.elapsed_ns = self.clock()-self.start_ns

    def report(self):
        successful = self.status in ('committed', 'transferred_not_committed')
        writes = [r for r in self.records if r['command']==3 and r['outcome']=='ok']
        no_retry = [r for r in writes if r['attempts']==1]
        size = self.metadata.get('image_size', 0)
        count = (size+255)//256
        wire_bytes = size+33*count  # request: N+16; response: 17, stop-and-wait
        wire_seconds = wire_bytes*10/115200
        data_seconds = self.data_ns/1e9 if self.data_ns is not None else None
        total_seconds = self.elapsed_ns/1e9 if self.elapsed_ns is not None else None
        return {
            'schema_version': 1, 'started_utc': self.started_utc, 'result': self.status,
            'error': self.error, 'environment': dict(self.metadata),
            'measurement_scope': 'Host perf_counter_ns; includes USB/OS scheduling, Python, wire time and device work. Not MCU execution time.',
            'total_seconds': total_seconds, 'data_transfer_seconds': data_seconds,
            'data_goodput_bytes_per_second': size/data_seconds if successful and data_seconds and data_seconds>0 else None,
            'whole_update_goodput_bytes_per_second': size/total_seconds if successful and total_seconds and total_seconds>0 else None,
            'command_count': len(self.records), 'retry_count': sum(r['retries'] for r in self.records),
            'write_transactions_ms': distribution([r['elapsed_ms'] for r in writes]),
            'write_transactions_without_retry_ms': distribution([r['elapsed_ms'] for r in no_retry]),
            'full_chunk_transactions_without_retry_ms': distribution([
                r['elapsed_ms'] for r in no_retry if r['request_payload_bytes']==260]),
            'ideal_data_phase': {'chunks': count, 'request_plus_ack_bytes': wire_bytes,
                                 'wire_only_seconds_no_retries': wire_seconds,
                                 'uart_bytes_per_second_8n1': 11520},
            'commands': list(self.records),
        }

    def print_summary(self, output=print):
        report = self.report()
        output('MEASURE | host-observed timings (USB + OS + UART + device)')
        for command, name in [(2, 'BEGIN erase/prepare'), (4, 'END verify/commit')]:
            records = [r for r in self.records if r['command']==command]
            if records:
                output(f"MEASURE | {name}: {sum(r['elapsed_ms'] for r in records):.3f} ms")
        if self.data_ns is not None:
            output(f"MEASURE | data transfer: {self.data_ns/1e9:.6f} s")
        if report['data_goodput_bytes_per_second'] is not None:
            output(f"MEASURE | data goodput: {report['data_goodput_bytes_per_second']:.1f} B/s")
        stats = report['write_transactions_without_retry_ms']
        if stats:
            output(f"MEASURE | WRITE without retry: n={stats['count']} min={stats['min_ms']:.3f} mean={stats['mean_ms']:.3f} p95={stats['p95_ms']:.3f} max={stats['max_ms']:.3f} ms")
        if report['total_seconds'] is not None:
            output(f"MEASURE | total: {report['total_seconds']:.6f} s | retries={report['retry_count']}")
