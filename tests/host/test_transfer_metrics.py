from pathlib import Path
import json
import struct
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tools'))
from fwpackage import Package, APP_BASE, encode_package
from fwtransfer import Client, CommandRejected, main, transfer
from transfer_metrics import TransferMetrics, distribution
from test_fwtransfer import Board


class Clock:
    def __init__(self): self.value=0
    def __call__(self):
        self.value+=1_000_000
        return self.value


class MetricTests(unittest.TestCase):
    def test_distribution(self):
        self.assertIsNone(distribution([]))
        result=distribution(list(range(1,21)))
        self.assertEqual((result['count'],result['median_ms'],result['p95_ms'],result['max_ms']),(20,10.5,19,20))

    def test_successful_transfer_counts_timing_and_bounds(self):
        b=Board(); b.version=(0,4,0); m=TransferMetrics(Clock())
        p=Package((1,0,0),bytes(range(256))+b'abcde')
        transfer(b,p,progress=lambda _:None,commit=True,metrics=m)
        report=m.report()
        self.assertEqual(b.memory,p.image)
        self.assertEqual(report['result'],'committed')
        self.assertEqual(report['command_count'],9)
        self.assertEqual(report['retry_count'],0)
        self.assertEqual(report['write_transactions_without_retry_ms']['count'],2)
        self.assertEqual(report['full_chunk_transactions_without_retry_ms']['count'],1)
        self.assertEqual(report['write_transactions_ms']['p95_ms'],1)
        self.assertAlmostEqual(report['data_goodput_bytes_per_second'],261/report['data_transfer_seconds'])
        self.assertGreaterEqual(report['total_seconds'],sum(r['elapsed_ms'] for r in m.records)/1000)
        self.assertEqual(report['ideal_data_phase']['request_plus_ack_bytes'],261+33*2)
        self.assertAlmostEqual(report['ideal_data_phase']['wire_only_seconds_no_retries'],327*10/115200)
        self.assertEqual(json.loads(json.dumps(report))['result'],'committed')

    def test_retry_count_and_latency_group(self):
        b=Board(); b.drop_write=True; m=TransferMetrics(Clock())
        ticks=iter(i*.01 for i in range(30000))
        with patch('fwtransfer.time.monotonic',side_effect=lambda:next(ticks)):
            transfer(b,Package((1,0,0),b'hello'),progress=lambda _:None,metrics=m)
        report=m.report()
        self.assertEqual(report['retry_count'],1)
        self.assertEqual(report['write_transactions_ms']['count'],1)
        self.assertIsNone(report['write_transactions_without_retry_ms'])
        self.assertEqual(report['result'],'transferred_not_committed')

    def test_rejection_is_recorded_not_pass(self):
        b=Board(); b.version=(0,4,0); b.commit_error=7; m=TransferMetrics(Clock())
        with self.assertRaises(CommandRejected):
            transfer(b,Package((1,0,0),b'hello'),progress=lambda _:None,commit=True,metrics=m)
        m.finish('failed','expected CRC rejection')
        report=m.report()
        self.assertEqual(report['commands'][-1]['outcome'],'rejected')
        self.assertEqual(report['commands'][-1]['status'],7)
        self.assertIsNone(report['data_goodput_bytes_per_second'])

    def test_exhausted_retry_records_four_attempts(self):
        class Silent:
            in_waiting=0
            def write(self,data): return len(data)
            def read(self,n): return b''
        m=TransferMetrics(Clock()); c=Client(Silent(),metrics=m)
        ticks=iter(i*.01 for i in range(10000))
        with patch('fwtransfer.time.monotonic',side_effect=lambda:next(ticks)):
            with self.assertRaises(TimeoutError): c.exchange(1)
        self.assertEqual(m.records[0]['attempts'],4)
        self.assertEqual(m.records[0]['retries'],3)
        self.assertEqual(m.records[0]['outcome'],'error')

    def test_existing_report_prevents_serial_access(self):
        with tempfile.TemporaryDirectory() as tmp:
            source=Path(tmp)/'image.fwp'; report=Path(tmp)/'report.json'
            source.write_bytes(encode_package(struct.pack('<II',0x20020000,APP_BASE+9)+b'abcdefgh',(1,0,0)))
            report.write_text('keep me')
            serial=Mock()
            with patch.dict(sys.modules,{'serial':serial}), patch('sys.stderr'):
                code=main(['--port','COM8','--package',str(source),'--commit','--report',str(report)])
            self.assertEqual(code,1)
            serial.Serial.assert_not_called()
            self.assertEqual(report.read_text(),'keep me')

    def test_serial_failure_saves_failed_report(self):
        with tempfile.TemporaryDirectory() as tmp:
            source=Path(tmp)/'image.fwp'; report=Path(tmp)/'report.json'
            source.write_bytes(encode_package(struct.pack('<II',0x20020000,APP_BASE+9)+b'abcdefgh',(1,0,0)))
            serial=Mock(); serial.Serial.side_effect=OSError('port unavailable')
            with patch.dict(sys.modules,{'serial':serial}), patch('sys.stderr'):
                code=main(['--port','COM8','--package',str(source),'--commit','--report',str(report)])
            self.assertEqual(code,1)
            data=json.loads(report.read_text())
            self.assertEqual(data['result'],'failed')
            self.assertIn('port unavailable',data['error'])
            self.assertIsNone(data['total_seconds'])


if __name__=='__main__': unittest.main()
