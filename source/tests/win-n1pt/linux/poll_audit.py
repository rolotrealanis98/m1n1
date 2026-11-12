#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""tests/win-n1pt: every POLL Linux made in capture cap2 (cold_train port part and the phase-1 cycle) against the polls
in src/pcie_t6050.c's N1 port paths: same register, mask, expect and condition, and a timeout >= Linux's.

  poll_audit.py table pcie-cycle.txt > linux-polls.tsv     (regenerate the committed table from the capture)
  poll_audit.py check linux-polls.tsv src/pcie_t6050.c     (the test: exit 1 on any mismatch)

Polls in `?` space (unresolved addresses: the shared PHY / RC of cold_train, m1n1's stages A/S) are listed, not checked.
C polls without a Linux counterpart must be in DEVIATIONS with their reason.
"""
import re
import sys

COND = {'(v&mask)==expect': 'eq', '(v&mask)!=expect': 'ne', '(v&mask)==0||(v&expect)==0': 'either-zero'}
# the functions n1pt's PREP (N1_TRAIN_LINUX port part) and PORT_CYCLE / PARK run; stage_b/stage_e/stage_e2 belong to the
# old mode-4 port path, which n1pt no longer runs
FUNCS = ('n1_l2', 'n1_perst_assert', 'n1_port_disable', 'n1_port_enable', 'n1_port_train', 'stage_c', 'stage_c2',
         'stage_d', 'stage_e3', 'stage_f', 'stage_g', 'pcie_t6050_n1_park_any')
# Linux function -> the C functions that implement it
IMPL = {'reset_root_port': ('n1_l2',), 'port_disable': ('n1_port_disable', 'pcie_t6050_n1_park_any'),
        'port_enable': ('n1_port_enable', 'stage_c', 'stage_c2'), 'port_train': ('n1_port_train', 'stage_e3'),
        'port_configure': ('stage_d',), 'cold_train': ()}
# C polls with no Linux poll at that register/condition: (region, offset, mask, expect, cond) -> reason
DEVIATIONS = {
    ('port', 0x804, 0x4, 0x4, 'or-0x820-bit2-or-idle'):
        'port_disable first wait (runs w24c/d/e): Linux skips it with STATUS bit 2 set, else waits on 0x820 bit 2; here '
        'STATUS bit 2 OR 0x820 bit 2 OR 0x820 idle (the next poll\'s condition), 10 ms; Linux would wait on 0x820 bit 2',
}


def table(path):
    print('# phase\tfunc\tregion\toffset\tmask\texpect\tcond\ttimeout_us\tseq  (capture cap2 POLL lines)')
    phase = 'cold'
    for ln in open(path):
        if 'CYCLE BEGIN' in ln:
            phase = 'cycle'
        m = re.match(r'\s*(\d+)\s+\S+\s+\S+\s+(\S+)\s+POLL\s+(\S+)\+0x([0-9a-f]+) = 0x[0-9a-f]+ ret (-?\d+) \(\d+ us of (\d+)\)\s+'
                     r'stop when (\S+) mask 0x([0-9a-f]+) expect 0x([0-9a-f]+)', ln)
        if not m:
            continue
        seq, func, region, off, ret, tmo, cond, mask, exp = m.groups()
        region = {'phylane': 'lane', 'dbi': 'rp', 'pcicfg': 'rp'}.get(region, region)
        print(f'{phase}\t{func}\t{region}\t0x{int(off, 16):x}\t0x{int(mask, 16):x}\t0x{int(exp, 16):x}\t{COND[cond]}\t{tmo}\t{seq}')


def c_polls(src):
    text = open(src).read()
    defs = {m.group(1): int(m.group(2), 16) for m in re.finditer(r'#define\s+(\w+)\s+0x([0-9a-fA-F]+)', text)}

    def num(e):
        e = re.sub(r'BIT\((\d+)\)', r'(1<<\1)', e)
        e = re.sub(r'\b([A-Z_][A-Z0-9_]+)\b', lambda m: str(defs[m.group(1)]), e)
        return eval(e, {'__builtins__': {}})

    def addr(e):
        e = e.strip()
        m = re.match(r'(p2|pp->ctl|pr\.ctl|pr\.lane|pp->lane|pr\.rp|pp->rp|rp|r\.\w+)(?:\s*\+\s*(.+))?$', e)
        if not m:
            return None, None
        base, off = m.group(1), num(m.group(2)) if m.group(2) else 0
        region = {'p2': 'port', 'pp->ctl': 'port', 'pr.ctl': 'port', 'pr.lane': 'lane', 'pp->lane': 'lane',
                  'pr.rp': 'rp', 'pp->rp': 'rp', 'rp': 'rp'}.get(base, '?')
        return region, off

    bodies = {}
    for f in FUNCS:
        m = re.search(r'\n(?:static )?int ' + f + r'\([^)]*\)\n\{(.*?)\n\}\n', text, re.S)
        bodies[f] = m.group(1) if m else ''
    out = []
    for f in FUNCS:
        m = re.search(r'\n(?:static )?int ' + f + r'\([^)]*\)\n\{(.*?)\n\}\n', text, re.S)
        if not m:
            raise SystemExit(f'poll_audit: function {f} not found in {src}')
        body = m.group(1)
        for c in re.finditer(r'\b(poll_set|poll_clear|wait32)\(', body):
            kind, i, depth, args, cur = c.group(1), c.end(), 1, [], ''
            while depth:
                ch = body[i]
                if ch == '(':
                    depth += 1
                elif ch == ')':
                    depth -= 1
                    if not depth:
                        break
                if ch == ',' and depth == 1:
                    args.append(cur)
                    cur = ''
                else:
                    cur += ch
                i += 1
            args.append(cur)
            region, off = addr(args[1])
            if kind == 'wait32':
                mask, want, cond, tmo = num(args[2]), num(args[3]), 'eq' if args[4].strip() == 'true' else 'ne', args[5].strip()
            else:
                mask = num(args[2])
                want, cond, tmo = (mask if kind == 'poll_set' else 0), 'eq', args[3].strip()
            m2 = re.match(r'^(.*)\?\s*(\d+)\s*:\s*(\d+)$', tmo)   # flag ? a : b -> the shorter one
            if m2:
                ms = min(int(m2.group(2)), int(m2.group(3)))
            elif re.match(r'^[a-z_]+$', tmo):   # a parameter: the value the N1 paths pass (stage_e3's link_ms)
                vals = [int(v) for g in FUNCS for v in re.findall(f + r'\(&pr, (\d+)\)', bodies.get(g, ''))]
                ms = min(vals) if vals else None
            else:
                ms = num(tmo)
            out.append((f, region, off, mask, want, cond, ms))
        # hand-written loops with their own timeout
        if re.search(r'while \(\(\(st = read32\(p2 \+ 0x820\)\) & 0x1ff\) && \(st & 0xff0000\)\)', body):
            t = re.search(r'timeout_calculate\((\d+)\);\s*u32 st;\s*while \(\(\(st = read32\(p2 \+ 0x820\)\)', body)
            out.append((f, 'port', 0x820, 0x1ff, 0xff0000, 'either-zero', int(t.group(1)) / 1000 if t else None))
        for w in re.finditer(r'while \(!\(read32\(p2 \+ PORT_STATUS\) & BIT\(2\)\)[^{]*\{', body):
            # the port_disable first wait as a hand-written loop: with or without the idle exit
            idle = '(st & 0x1ff) && (st & 0xff0000)' in w.group(0)
            out.append((f, 'port', 0x804, 0x4, 0x4, 'or-0x820-bit2-or-idle' if idle else 'or-0x820-bit2', 10))
    return out


def check(tsv, src):
    lin = []
    for ln in open(tsv):
        if ln.startswith('#') or not ln.strip():
            continue
        ph, func, region, off, mask, exp, cond, tmo, seq = ln.split('\t')
        lin.append((ph, func, region, int(off, 16), int(mask, 16), int(exp, 16), cond, int(tmo), seq.strip()))
    cp = c_polls(src)
    bad = 0
    print(f'{"phase":6} {"linux func":22} {"reg":>12} {"mask":>10} {"expect":>10} {"cond":12} {"linux us":>9}  C')
    for ph, func, region, off, mask, exp, cond, tmo, seq in lin:
        if region == '?':
            print(f'{ph:6} {func:22} {"?+%x" % off:>12} {mask:#10x} {exp:#10x} {cond:12} {tmo:9}  (unresolved space: m1n1 stages A/S, not checked)')
            continue
        impl = IMPL[func.split('/')[0]]
        hits = [c for c in cp if c[1:6] == (region, off, mask, exp, cond) and c[0] in impl]
        if not hits:
            print(f'{ph:6} {func:22} {region + "+%x" % off:>12} {mask:#10x} {exp:#10x} {cond:12} {tmo:9}  MISSING in C')
            bad += 1
            continue
        weak = [c for c in hits if not isinstance(c[6], (int, float)) or c[6] * 1000 < tmo]
        ok = len(weak) < len(hits) and all(isinstance(c[6], (int, float)) and c[6] * 1000 >= tmo for c in hits)
        txt = ', '.join(f'{c[0]} {c[6]} ms' for c in hits)
        print(f'{ph:6} {func:22} {region + "+%x" % off:>12} {mask:#10x} {exp:#10x} {cond:12} {tmo:9}  {"ok" if ok else "TIMEOUT SHORT"}: {txt}')
        bad += not ok
    matched = {(r, o, m, e, c) for _, _, r, o, m, e, c, _, _ in lin}
    for c in cp:
        key = c[1:6]
        if key in matched:
            continue
        if key in DEVIATIONS and DEVIATIONS[key]:
            print(f'C-only {c[0]:22} {c[1] + "+%x" % c[2]:>12} {c[3]:#10x} {c[4]:#10x} {c[5]:12} {c[6]} ms  deviation: {DEVIATIONS[key]}')
        else:
            print(f'C-only {c[0]:22} {c[1] + "+%x" % c[2]:>12} {c[3]:#10x} {c[4]:#10x} {c[5]:12} {c[6]} ms  NOT IN LINUX and not a listed deviation')
            bad += 1
    print(f'poll audit: {len(lin)} Linux polls, {len(cp)} C polls, {bad} problems')
    return 1 if bad else 0


if __name__ == '__main__':
    if sys.argv[1] == 'table':
        table(sys.argv[2])
    else:
        sys.exit(check(sys.argv[2], sys.argv[3]))
