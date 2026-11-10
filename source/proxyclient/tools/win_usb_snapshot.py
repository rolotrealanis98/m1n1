#!/usr/bin/env python3
"""Read-only target DART preflight. Fresh proxy only, exclusive owner, no guest running.
No USB preparation, no controller start, no DART commands or fault clears. Standard
proxy machinery may service the watchdog. Save output outside public repositories.
"""
import argparse
import datetime
import json
from pathlib import Path
import sys


def read_fault_state(read32, base, nsid=None, version=None, *, size):
    """Bounded, read-only version-aware latches. No fault acknowledgment or mask writes."""
    if size < 0x1800:
        raise ValueError('Fault register region too small')
    if version is None:
        version = read32(base + 8) & 0xffff
    if nsid is None:
        nsid = read32(base + 12) & 0x1ff
    if not 0 < nsid <= 256:
        raise ValueError('Unexpected fault bitmap stream count')
    if version >= 0x0202 and size < 0x8000 + 0x40*(nsid-1) + 0x10:
        raise ValueError('v2 per-SID exception registers exceed region')
    state = dict(base=base, version=version, nsid=nsid,
                 legacy_error=read32(base + 0x100),
                 legacy_address=read32(base + 0x170) | (read32(base + 0x174) << 32))
    words = (nsid + 31) // 32
    state['error_streams'] = [read32(base + 0x1c0 + 4*i) for i in range(words)]
    if version < 0x0202:
        state['mask'] = read32(base + 0x104)
        return state
    state['mask'] = read32(base + 0x4060)
    state['pending'] = [read32(base + 0x4000 + 4*i) for i in range(words)]
    state['block_error'] = read32(base + 0x704) if version >= 0x0203 else 0
    state['irq_status'] = read32(base + 0x4204) if version >= 0x0204 else 0
    if version >= 0x0204:
        state['irq_enable'] = read32(base + 0x4200)
        state['sid_enable'] = [read32(base + 0x4220 + 4*i) for i in range(words)]
    state['exceptions'] = []
    for sid in range(nsid):
        if state['pending'][sid // 32] & (1 << (sid % 32)):
            off = base + 0x8000 + 0x40*sid
            state['exceptions'].append(dict(sid=sid, status=read32(off),
                address=read32(off + 8) | (read32(off + 12) << 32)))
    return state


def fault_pending(state):
    if state['legacy_error']:
        return True
    if state['version'] < 0x0202:
        return any(state['error_streams'])
    return bool(any(state['pending']) or state['block_error'] & 0x777 or state['irq_status'])


def negative_fault_matches_versioned(before, states, outside, expected_sids):
    """Require exact per-instance SID, active latch, write/invalid-PTE and buffer address."""
    if len(states) != len(expected_sids) or len(before) != len(states):
        return False
    if any(fault_pending(state) for state in before):
        return False
    for old, new in zip(before, states):
        if any(old[key] != new[key] for key in ('base', 'version', 'nsid')):
            return False
    matched = False
    for state, expected in zip(states, expected_sids):
        if not 0 <= expected < state['nsid']:
            return False
        if state['version'] < 0x0202:
            error = state['legacy_error']
            if not error:
                continue
            if not (error & (1 << 31) and ((error >> 20) & 255) == expected
                    and outside <= state['legacy_address'] < outside + 18):
                return False
            matched = True
            continue
        # Status/address registers can alias; the pending bitmap owns SID identity.
        if state['legacy_error'] or state['block_error'] & 0x777 or state['irq_status']:
            return False
        pending = sum(word << (32*i) for i, word in enumerate(state['pending']))
        if not pending:
            if state['legacy_error'] or state['block_error'] & 0x777 or state['irq_status']:
                return False
            continue
        if pending != 1 << expected:
            return False
        entries = [x for x in state['exceptions'] if x['sid'] == expected]
        if len(entries) != 1:
            return False
        status, address = entries[0]['status'], entries[0]['address']
        if not (status & 0xf == 0 and (status >> 12) & 3 == 3 and status & 0x100
                and outside <= address < outside + 18):
            return False
        matched = True
    return matched


def snapshot_instance(read32, base, size):
    if size < 0x1800:
        raise ValueError('T8110 register region too small')
    params = [read32(base + i * 4) for i in range(4)]
    nsid = params[3] & 0x1ff
    if not 0 < nsid <= 256:
        raise ValueError(f'Unexpected stream count {nsid}; stop, do not guess')
    if params[2] & 0xffff >= 0x0202 and size < 0x8000 + 0x40*nsid:
        raise ValueError('v2 per-SID exception registers exceed region')
    return dict(base=base, size=size, params=params, version=params[2] & 0xffff,
                va_bits=(params[2] >> 16) & 63, pa_bits=(params[2] >> 24) & 63,
                protect=read32(base+0x200), command=read32(base+0x80),
                faults=read_fault_state(read32, base, nsid, params[2] & 0xffff, size=size),
                error=read32(base+0x100),
                fault_address=read32(base+0x170) | (read32(base+0x174) << 32),
                streams=[dict(sid=i, tcr=read32(base+0x1000+4*i),
                              ttbr=read32(base+0x1400+4*i)) for i in range(nsid)])


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--port', type=int, choices=(1,2), default=1)
    ap.add_argument('--output', type=Path, required=True)
    args=ap.parse_args()
    if args.output.exists():
        ap.error('Refusing to overwrite existing evidence')
    sys.path.append(str(Path(__file__).resolve().parents[1]))
    from m1n1.setup import p, u
    from m1n1.adt import load_adt
    from m1n1.proxy import IODEV
    if p.iodev_whoami() == IODEV.USB0 + args.port:
        raise SystemExit('Refusing proxy port')
    adt=load_adt(u.get_adt())
    if adt['/chosen'].chip_id not in (0x6050,0x6051):
        raise SystemExit('Only the reviewed T6050/T6051 layout is supported')
    node=adt[f'/arm-io/dart-usb{args.port}']
    if 'dart,t8110' not in node.compatible:
        raise SystemExit('Expected dart,t8110 compatibility')
    result=dict(utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),port=args.port,
                note='Target-only read-only DART state; no qualification of flush or DMA',
                instances=[snapshot_instance(p.read32,*node.get_reg(i)) for i in range(2)])
    with args.output.open('x') as f:
        json.dump(result,f,indent=2);f.write('\n')
    print(f'Saved target-port snapshot to {args.output}')


if __name__=='__main__':
    main()
