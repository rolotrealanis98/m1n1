#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Decode an S1DG (resident r2.1 synthetic NVMe diagnostics) memory dump.

The block is struct s1_diag inside the resident's `controller` (struct s1_model),
at &controller + S1DG_MODEL_OFFSET. Layout version 1 (see src/hv/win/s1/model.h).
Pure host code: reads bytes/files only, never a device.

    s1dg_decode.py DUMP.bin                 -> JSON summary on stdout
    s1dg_decode.py --elf m1n1-raw.elf       -> controller/S1DG link offsets
"""
import json
import struct
import sys

MAGIC = 0x47443153
VERSION = 1
MODEL_OFFSET = 0x21088
SIZE = 32232
RING, ADMIN_FIRST, MMIO_FIRST = 512, 64, 256
RING_OFF, ADMIN_OFF, MMIO_OFF = 0x3e8, 0x53e8, 0x65e8
TYPES = {1: 'MMIO_R', 2: 'MMIO_W', 3: 'CFG_R', 4: 'CFG_W', 5: 'SQE', 6: 'CQE', 7: 'REFUSAL',
         8: 'FATAL', 9: 'SHUTDOWN', 10: 'RESET', 11: 'ENABLE', 12: 'IRQ_EDGE', 13: 'INVALID_DB',
         14: 'SHN_CLEAR_NO_RESET'}
FATAL_REASONS = {1: 'QSPAN_SQ', 2: 'QSPAN_CQ', 3: 'SQE', 4: 'CQE', 5: 'WALK', 6: 'ENABLE_CC',
                 7: 'ENABLE_BME', 8: 'ENABLE_ADMQ', 9: 'SHN11'}
REFUSAL_REASONS = {1: 'NOT_MAPPED', 2: 'FAULTED', 3: 'NO_BME', 4: 'PROVIDER'}
DB_BITS = ['NOT_READY', 'CFS', 'NO_BME', 'VALUE', 'INDEX', 'QUEUE', 'SHUTDOWN']
# Counters after the 40-byte header, in declaration order: (name, count)
COUNTERS = [('admin_opcode', 32), ('admin_other', 1), ('last_admin_other', 1),
            ('io_opcode', 32), ('io_other', 1), ('last_io_other', 1),
            ('nonzero_status', 1), ('last_err_qid', 1), ('last_err_opcode', 1), ('last_err_cid', 1),
            ('last_err_status', 1), ('writes', 1), ('last_write_lba', 1), ('last_write_nlb', 1),
            ('mmio_width', 8), ('cfg_access', 2), ('mmio_rejected', 1), ('reserved_access', 1),
            ('last_reserved_offset', 1), ('fatal_count', 1), ('first_fatal_reason', 1),
            ('refusals', 8), ('invalid_db', 8), ('max_cq_pending', 2), ('cc_writes', 1),
            ('shn_transitions', 1), ('en_transitions', 1), ('intms_writes', 1), ('intmc_writes', 1),
            ('irq_edges', 1), ('shn_clear_no_reset', 1), ('shutdowns_normal', 1),
            ('shutdowns_abrupt', 1), ('aborted', 1), ('discarded', 1)]
ENTRY = struct.Struct('<IIBBBBHHQQII')
ADMIN = struct.Struct('<64sHHI')
ACCESS = struct.Struct('<IIBBBBIQ')
assert ENTRY.size == 40 and ADMIN.size == 72 and ACCESS.size == 24


class DecodeError(ValueError):
    pass


def decode(blob):
    """Return a dict for an S1DG block (bytes of exactly SIZE, or longer)."""
    if len(blob) < SIZE:
        raise DecodeError('short S1DG block: %d < %d' % (len(blob), SIZE))
    magic, version, ring_entries, entry_size, write_seq, admin_n, mmio_n, struct_size, _ = \
        struct.unpack_from('<IIIIQIIII', blob, 0)
    if magic != MAGIC or version != VERSION or ring_entries != RING or entry_size != 40 \
            or struct_size != SIZE:
        raise DecodeError('not an S1DG v1 block (magic %#x version %d size %d)' % (magic, version, struct_size))
    if admin_n > ADMIN_FIRST or mmio_n > MMIO_FIRST:
        raise DecodeError('corrupt first-log counts')
    out = {'write_seq': write_seq, 'admin_first_count': admin_n, 'mmio_first_count': mmio_n}
    off = 40
    for name, n in COUNTERS:
        vals = list(struct.unpack_from('<%dQ' % n, blob, off))
        off += 8 * n
        out[name] = vals if n > 1 else vals[0]
    if off != RING_OFF:
        raise DecodeError('layout mismatch: counters end at %#x' % off)
    mw = out['mmio_width']
    out['mmio_width'] = {'read': dict(zip((1, 2, 4, 8), mw[:4])), 'write': dict(zip((1, 2, 4, 8), mw[4:]))}
    out['first_fatal_reason_name'] = FATAL_REASONS.get(out['first_fatal_reason'], None)
    out['refusals'] = {REFUSAL_REASONS.get(i, str(i)): v for i, v in enumerate(out['refusals']) if v}
    out['invalid_db'] = {DB_BITS[i]: v for i, v in enumerate(out['invalid_db'][:7]) if v}
    out['admin_opcode'] = {'%#04x' % i: v for i, v in enumerate(out['admin_opcode']) if v}
    out['io_opcode'] = {'%#04x' % i: v for i, v in enumerate(out['io_opcode']) if v}
    # ring: the last min(write_seq, RING) entries, oldest first
    count = min(write_seq, RING)
    ring = []
    for seq in range(write_seq - count, write_seq):
        e = ENTRY.unpack_from(blob, RING_OFF + (seq % RING) * 40)
        if e[0] != (seq & 0xffffffff):
            raise DecodeError('ring entry %d has seq %d' % (seq, e[0]))
        ring.append({'seq': seq, 't': e[1], 'type': TYPES.get(e[2], e[2]), 'width': e[3], 'qid': e[4],
                     'opcode': e[5], 'cid': e[6], 'status': e[7], 'a': e[8], 'b': e[9], 'c': e[10], 'd': e[11]})
    out['ring'] = ring
    admin = []
    for i in range(admin_n):
        sqe, status, completed, dw0 = ADMIN.unpack_from(blob, ADMIN_OFF + i * 72)
        opcode, flags, cid, nsid = struct.unpack_from('<BBHI', sqe, 0)
        cdw10, cdw11, cdw12 = struct.unpack_from('<III', sqe, 40)
        prp1, prp2 = struct.unpack_from('<QQ', sqe, 24)
        admin.append({'opcode': opcode, 'cid': cid, 'nsid': nsid, 'cdw10': cdw10, 'cdw11': cdw11,
                      'cdw12': cdw12, 'prp1': prp1, 'prp2': prp2, 'completed': bool(completed),
                      'status': status, 'dw0': dw0, 'sqe_hex': sqe.hex()})
    out['admin_first'] = admin
    acc = []
    for i in range(mmio_n):
        seq, offset, kind, width, write, ok, csts, value = ACCESS.unpack_from(blob, MMIO_OFF + i * 24)
        acc.append({'seq': seq, 'kind': 'cfg' if kind else 'mmio', 'offset': offset, 'width': width,
                    'write': bool(write), 'ok': bool(ok), 'csts': csts, 'value': value})
    out['mmio_first'] = acc
    return out


def elf_symbols(path, names=('controller', '_base')):
    """Minimal ELF64 little-endian symtab lookup (no dependencies)."""
    data = open(path, 'rb').read()
    if data[:4] != b'\x7fELF' or data[4] != 2 or data[5] != 1:
        raise DecodeError('not an ELF64 LE file')
    shoff, = struct.unpack_from('<Q', data, 0x28)
    shentsize, shnum, _ = struct.unpack_from('<HHH', data, 0x3a)
    secs = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shentsize) for i in range(shnum)]
    found = {}
    for s in secs:
        if s[1] != 2:  # SHT_SYMTAB
            continue
        strtab = secs[s[6]]
        for j in range(s[5] // s[9]):
            st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from('<IBBHQQ', data, s[4] + j * s[9])
            end = data.index(b'\0', strtab[4] + st_name)
            name = data[strtab[4] + st_name:end].decode()
            if name in names:
                found[name] = (st_value, st_size)
    return found


def s1dg_offset_from_elf(path):
    """Offset of the S1DG block from the resident image base (u.base)."""
    sym = elf_symbols(path)
    if 'controller' not in sym:
        raise DecodeError('controller symbol not found')
    base = sym.get('_base', (0, 0))[0]
    value, size = sym['controller']
    if size and size < MODEL_OFFSET + SIZE:
        raise DecodeError('controller symbol too small for S1DG')
    return value - base + MODEL_OFFSET


def main(argv):
    if len(argv) == 3 and argv[1] == '--elf':
        print(json.dumps({'s1dg_offset_from_base': s1dg_offset_from_elf(argv[2])}))
        return 0
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    print(json.dumps(decode(open(argv[1], 'rb').read()), indent=1))
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv))
