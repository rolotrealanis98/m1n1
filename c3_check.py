#!/usr/bin/env python3
"""RUN-SPEC condition C3: offline stale-pixel check at teardown (disp3).

Compares the native scanout with the converter's formula applied to the guest shadow, on the guest rows
the m5w test card (mode 1) never redraws. Any mismatch there is a page whose guest write was never
converted (lost dirty tracking) and is a FAIL.

Dump in the HV shell (after the one FBSTAT read, before p.reboot()):
    g = [p.hv_win_fb_stat(i) for i in (1, 2, 4, 5, 26, 27, 28, 14)]   # shadow native stride height gw gh gs flags
    open("/tmp/c3_geom.txt", "w").write(" ".join(map(str, g)))
    open("/tmp/c3_shadow.bin", "wb").write(iface.readmem(g[0], g[6] * g[5]))
    open("/tmp/c3_native.bin", "wb").write(iface.readmem(g[1], g[2] * g[3]))
then on the host:
    c3_check.py /tmp/c3_geom.txt /tmp/c3_shadow.bin /tmp/c3_native.bin [--margin ROWS] [--exclude A:B ...]

Excluded guest rows by default (the card geometry in harness/sim.c card()): the band
[gh*3/4-40, gh*3/4-40 + gh/4-60) that carries the bar, and the three counter rows y0 in
{170, gh/2+40, gh-gh/28-60}, height gh/28; each widened by --margin rows (default 16).
usage: c3_check.py --selftest   (formula/indexing sanity on synthetic data; not a test of the resident)
"""
import random
import sys
from array import array


def px(p):
    b, g, r = p & 0xFF, (p >> 8) & 0xFF, (p >> 16) & 0xFF
    return ((b << 2) | (b >> 6)) | (((g << 2) | (g >> 6)) << 10) | (((r << 2) | (r >> 6)) << 20)


def excluded_rows(gh, margin, extra):
    by, bh, fh = gh * 3 // 4 - 40, gh // 4 - 60, gh // 28
    spans = [(by, by + bh)] + [(y0, y0 + fh) for y0 in (170, gh // 2 + 40, gh - gh // 28 - 60)] + extra
    rows = set()
    for a, b in spans:
        rows.update(range(max(0, a - margin), min(gh, b + margin)))
    return rows


def check(geom, shadow, native, margin=16, extra=()):
    _sh, _na, stride, height, gw, gh, gs, flags = geom
    scale = 2 if flags & 4 else 1
    assert len(shadow) >= gs * gh and len(native) >= stride * height, "short dump"
    skip = excluded_rows(gh, margin, list(extra))
    bad = checked = 0
    first = None
    for y in range(gh):
        if y in skip:
            continue
        src = array("I", shadow[y * gs:y * gs + gw * 4])
        for dy in range(scale):
            row = array("I", native[(y * scale + dy) * stride:(y * scale + dy) * stride + gw * scale * 4])
            for x in range(gw):
                e = px(src[x])
                for dx in range(scale):
                    checked += 1
                    if row[x * scale + dx] != e:
                        bad += 1
                        if first is None:
                            first = (x, y, src[x], row[x * scale + dx], e)
    return bad, checked, first, len(skip)


def selftest():
    gw, gh, scale = 64, 600, 2
    gs, stride, height = gw * 4, gw * scale * 4 + 32, gh * scale
    rnd = random.Random(1)
    sh = array("I", (rnd.getrandbits(32) for _ in range(gs // 4 * gh)))
    na = array("I", [0x5A5A5A5A] * (stride // 4 * height))
    for y in range(gh):
        for x in range(gw):
            for dy in range(scale):
                for dx in range(scale):
                    na[(y * scale + dy) * stride // 4 + x * scale + dx] = px(sh[y * gs // 4 + x])
    geom = (0, 0, stride, height, gw, gh, gs, 4 | 2)
    ok = check(geom, sh.tobytes(), na.tobytes())[0] == 0
    na[(5 * scale + 1) * stride // 4 + 7 * scale] ^= 1  # row 5 is outside every excluded span
    ok &= check(geom, sh.tobytes(), na.tobytes())[0] == 1
    print("selftest", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def main():
    a = sys.argv[1:]
    if a[:1] == ["--selftest"]:
        return selftest()
    if len(a) < 3:
        print(__doc__)
        return 2
    geom = [int(v) for v in open(a[0]).read().split()]
    margin = int(a[a.index("--margin") + 1]) if "--margin" in a else 16
    extra = [tuple(int(v) for v in a[i + 1].split(":")) for i, v in enumerate(a) if v == "--exclude"]
    bad, checked, first, nskip = check(geom, open(a[1], "rb").read(), open(a[2], "rb").read(), margin, extra)
    print(f"C3: {checked} native pixels checked on {geom[5] - nskip} of {geom[5]} guest rows; mismatches {bad}")
    if first:
        x, y, s, n, e = first
        print(f"first mismatch guest ({x},{y}) shadow {s:#010x} native {n:#010x} expected {e:#010x}")
    print("C3", "PASS" if not bad else "FAIL")
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
