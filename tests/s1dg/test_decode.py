#!/usr/bin/env python3
"""Host tests for proxyclient/tools/s1dg_decode.py on a real S1DG dump produced by
the actual r2.1 model (tests/r2/test_r2.c after the StorNVMe-like trace)."""
import importlib.util, json, struct, sys, unittest
from pathlib import Path
R = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('s1dg_decode', R / 'source/proxyclient/tools/s1dg_decode.py')
d = importlib.util.module_from_spec(spec); spec.loader.exec_module(d)
DUMP = Path(sys.argv.pop(1)) if len(sys.argv) > 1 and sys.argv[1].endswith('.dump') else None
ELF = R / 'source/build/m1n1-raw.elf'

class Decode(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.blob = DUMP.read_bytes(); cls.x = d.decode(cls.blob)
    def test_header(self):
        self.assertEqual(len(self.blob), d.SIZE)
        self.assertEqual(self.x['write_seq'], 177); self.assertEqual(self.x['admin_first_count'], 18)
        self.assertEqual(self.x['mmio_first_count'], 82)
    def test_counters(self):
        x = self.x
        self.assertEqual(x['admin_opcode']['0x06'], 5); self.assertEqual(x['io_opcode']['0x02'], 3)
        self.assertEqual(x['writes'], 1); self.assertEqual(x['last_write_nlb'], 1)
        self.assertEqual(x['nonzero_status'], 3); self.assertEqual(x['last_err_status'], 7)
        self.assertEqual(x['shutdowns_normal'], 1); self.assertEqual(x['refusals'], {}); self.assertEqual(x['invalid_db'], {})
        self.assertIsNone(x['first_fatal_reason_name'])
        self.assertGreater(x['mmio_width']['read'][4], 0); self.assertGreater(x['mmio_width']['write'][8], 0)
    def test_admin_first(self):
        a = self.x['admin_first']
        self.assertEqual((a[0]['opcode'], a[0]['cdw10'], a[0]['completed'], a[0]['status']), (6, 1, True, 0))
        self.assertEqual((a[2]['nsid'], a[2]['status']), (0xffffffff, 0x0b))
        self.assertEqual((a[13]['opcode'], a[13]['status']), (0x0c, 7))
        self.assertEqual(a[0]['sqe_hex'][:2], '06')
    def test_ring(self):
        ring = self.x['ring']
        self.assertEqual([e['seq'] for e in ring], list(range(177)))
        types = {e['type'] for e in ring}
        self.assertTrue({'CFG_W', 'CFG_R', 'MMIO_R', 'MMIO_W', 'SQE', 'CQE', 'ENABLE', 'RESET', 'SHUTDOWN', 'IRQ_EDGE'} <= types)
        sqe = [e for e in ring if e['type'] == 'SQE' and e['qid'] == 1 and e['opcode'] == 1][0]
        self.assertEqual(sqe['c'], 1)  # nsid
        shut = [e for e in ring if e['type'] == 'SHUTDOWN'][0]
        self.assertEqual((shut['a'], shut['b'], shut['c']), (1, 0, 0))
    def test_first_access(self):
        f = self.x['mmio_first'][0]
        self.assertEqual((f['kind'], f['offset'], f['write'], f['ok'], f['value']), ('cfg', 0x10, True, True, 0x30000000))
    def test_rejects_bad(self):
        b = bytearray(self.blob); b[0] ^= 1
        with self.assertRaises(d.DecodeError): d.decode(bytes(b))
        with self.assertRaises(d.DecodeError): d.decode(self.blob[:-1])
        b = bytearray(self.blob); struct.pack_into('<I', b, d.RING_OFF + 5 * 40, 99)
        with self.assertRaises(d.DecodeError): d.decode(bytes(b))
        b = bytearray(self.blob); struct.pack_into('<I', b, 24, 65)
        with self.assertRaises(d.DecodeError): d.decode(bytes(b))
    def test_json_roundtrip(self):
        json.dumps(self.x)
    @unittest.skipUnless(ELF.exists(), 'r2.1 ELF not built yet')
    def test_elf_offset(self):
        off = d.s1dg_offset_from_elf(str(ELF))
        self.assertEqual(off % 8, 0); self.assertGreater(off, d.MODEL_OFFSET)

if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0], '-v'])
