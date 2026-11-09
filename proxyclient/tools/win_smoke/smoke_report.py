#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# smoke_report.py -- turn a win_smoke run log into answers. Reads run_windows.py's hv.log (lines stamped
# "HH:MM:SS.mmm" by the launcher, guest lines prefixed "TTY> ") or any log holding the "M5W-SMOKE" lines,
# plus RUN.txt next to it when present (HCR_EL2.TSC read-back).
#
#   smoke_report.py RUN_DIR|hv.log
#
# Answers: OPEN-QUESTIONS Q4 (PSCI conduit), Q6 (counters), Q7 (WFI), Q8 (framebuffer), M0-RUNTIME-PLAN
# open item 1 (Apple sysregs) and item 4 (entry registers), the M0 timer path, the vuart (hv_vuart TX and
# the host-to-guest RX self-test), the PL011, the stage-2 plan and EL2-RUNTIME item 6 (PMUv3 accesses:
# trapped to EL2 or UNDEF at EL1).

import datetime
import pathlib
import re
import sys

LINE = re.compile(r"^(?:(\d\d:\d\d:\d\d\.\d{3}) )?(?:TTY> )?M5W-SMOKE (\S+)(.*)$")
KV = re.compile(r"(\S+?)=(\S+)")


def num(v):
    try:
        return int(v, 0)
    except (TypeError, ValueError):
        return None


def load(path):
    path = pathlib.Path(path)
    run = path if path.is_dir() else path.parent
    log = path / "hv.log" if path.is_dir() else path
    recs = []
    for raw in log.read_text(errors="replace").splitlines():
        m = LINE.match(raw.strip())
        if not m:
            continue
        stamp, tag, rest = m.groups()
        t = None
        if stamp:
            h, mi, s = stamp.split(":")
            t = int(h) * 3600 + int(mi) * 60 + float(s)
        recs.append((t, tag, dict(KV.findall(rest))))
    run_txt = (run / "RUN.txt").read_text(errors="replace") if (run / "RUN.txt").exists() else ""
    return recs, run_txt


def first(recs, tag, **match):
    # banner lines ("M5W-SMOKE pl011 hello ...") carry no fields: skip them
    for t, g, kv in recs:
        if g == tag and kv and all(kv.get(k) == v for k, v in match.items()):
            return t, kv
    return None, None


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: smoke_report.py RUN_DIR|hv.log")
    recs, run_txt = load(sys.argv[1])
    out = []

    def say(key, text):
        out.append(f"{key:9s} {text}")

    if not recs:
        sys.exit("no M5W-SMOKE lines: the guest never printed (see hv.log for a fault before the first HVC)")

    _, end = first(recs, "end")
    _, fatal = first(recs, "fatal")
    if fatal:
        say("FATAL", f"unexpected {fatal.get('vec')} exception in phase {fatal.get('phase')}: ESR {fatal.get('esr')} "
                     f"ELR {fatal.get('elr')} FAR {fatal.get('far')}")
    last = recs[-1][1]
    say("run", "complete (reached SYSTEM_OFF)" if end else f"INCOMPLETE: last line '{last}' (see below)")

    _, regs = first(recs, "regs")
    if regs:
        say("entry", f"x0 {regs.get('x0')} (boot_args) x1 {regs.get('x1')} x2 {regs.get('x2')} x3 {regs.get('x3')} "
                     f"x4 {regs.get('x4')} x5 {regs.get('x5')} x6 {regs.get('x6')} x7 {regs.get('x7')}")
        say("item4", "x0 = boot_args, x4 = the entry address (hv_enter_guest loads ELR from x4): Mu's SEC must use x0; "
                     "its x4 fallback would see the image header and reject it")
    _, ba = first(recs, "bootargs")
    if ba:
        say("bootargs", f"rev {ba.get('rev')} ver {ba.get('ver')} phys_base {ba.get('phys_base')} mem_size {ba.get('mem_size')} "
                        f"-> Mu x0 check {ba.get('mu_x0')}")

    ids = {}
    for _, g, kv in recs:
        if g == "id":
            ids.update(kv)
    if ids:
        say("id", f"CurrentEL EL{ids.get('el')} SCTLR_EL1 {ids.get('SCTLR_EL1')} MIDR {ids.get('MIDR_EL1')} "
                  f"MPIDR {ids.get('MPIDR_EL1')}")
        say("id", f"PFR0 {ids.get('ID_AA64PFR0_EL1')} (EL3 {ids.get('EL3')}, GIC {ids.get('GIC')}), DFR0 "
                  f"{ids.get('ID_AA64DFR0_EL1')} (PMUVer {ids.get('PMUVer')}), MMFR0 {ids.get('ID_AA64MMFR0_EL1')} "
                  f"(PARange {ids.get('PARange')}, ECV {ids.get('ECV')}), MTE {ids.get('MTE')}, CNTFRQ {ids.get('CNTFRQ_EL0')}")
        pmu = num(ids.get("PMUVer"))
        if pmu:
            say("Q5-hint", f"EL1 sees PMUVer {pmu:#x}: Windows will probe PMUv3 unless EL2 hides it (TID3), GAPS G2")
    _, sre = first(recs, "sysreg", reg="ICC_SRE_EL1")
    if sre:
        say("R1", f"ICC_SRE_EL1 at EL1: {sre.get('read')} {sre.get('val', sre.get('esr'))}. Route R1 (vGIC) stands on "
                  "PROBE-GIC-T6050 (Q1); an UNDEF here means the early-boot ICC window needs EL2-RUNTIME item 5b")

    # Q4
    _, q4 = first(recs, "q4")
    tsc = re.search(r"TSC (set|clear)", run_txt)
    tsc = tsc.group(1) if tsc else "unknown"
    if q4:
        smc = q4.get("smc")
        if smc == "trap":
            say("Q4", f"SMC from EL1 traps to EL2 (HCR_EL2.TSC {tsc}), PSCI answered {q4.get('ret')}: SMC conduit usable; "
                      "keep HVC in the FADT anyway (works either way)")
        elif smc == "undef":
            say("Q4", f"SMC UNDEFs at EL1 (ESR {q4.get('esr')}, HCR_EL2.TSC {tsc}): PSCI conduit must be HVC; "
                      "never set PcdMonitorConduitHvc = 0")
        else:
            say("Q4", f"SMC fell through without effect (HCR_EL2.TSC {tsc}): PSCI conduit must be HVC")
    else:
        say("Q4", "no answer (run stopped earlier)")

    # Q6
    q6 = [kv for _, g, kv in recs if g == "q6"]
    base = next((kv for kv in q6 if "dpct" in kv), None)
    if base:
        freq = num(base.get("freq")) or 1
        dp, dv = num(base.get("dpct")), num(base.get("dvct"))
        drift = ((num(base.get("voff1")) or 0) - (num(base.get("voff0")) or 0) + 2**63) % 2**64 - 2**63
        say("Q6", f"CNTFRQ {freq} Hz; 1 s busy wait: dCNTPCT {dp} dCNTVCT {dv} (EL1 view frozen: {base.get('frozen')}); "
                  f"CNTPCT-CNTVCT offset moved {drift} ticks (read jitter only = no time stealing; "
                  f"millions = CNTVOFF moving under the guest)")
    t0, m0 = first(recs, "q6", mark="0")
    t1, m1 = first(recs, "q6", mark="1")
    if m0 and m1 and t0 is not None and t1 is not None:
        wall = (t1 - t0) % 86400
        err = (wall - 5.0) / 5.0 * 100
        verdict = "within 1 %" if abs(err) <= 1 else "OUTSIDE 1 %"
        say("Q6", f"5 s by the guest counter took {wall:.3f} s by the Studio clock ({err:+.2f} %, {verdict}; USB latency included)")
    elif m0 and m1:
        say("Q6", "5 s marks present but the log has no timestamps (use run_windows.py's hv.log)")

    # timer
    _, tm = first(recs, "timer")
    if tm:
        kind = tm.get("kind")
        if kind == "fiq":
            say("timer", f"CNTP 100 ms arrived as a virtual FIQ after {tm.get('latency_us')} us; guest saw CNTP_CTL "
                         f"{tm.get('ctl_seen')} -> Mu's ENABLE=1/IMASK=0/ISTATUS=1 check: {tm.get('mu_check')}")
        elif kind == "irq":
            say("timer", f"CNTP arrived as an IRQ (not the M0 vFIQ path) after {tm.get('latency_us')} us, ISR {tm.get('isr')}")
        else:
            say("timer", f"NO timer interrupt ({kind}): Mu's TimerDxe would hang (M0 blocked on hv_update_fiq)")

    # open item 1
    for _, g, kv in recs:
        if g != "apple":
            continue
        r = kv.get("read")
        detail = f"read {r}"
        if r == "ok":
            us = num(kv.get("read_us")) or 0
            detail += f" ({us} us: {'Python round trip, add a C handler' if us >= 1000 else 'C/hardware speed'})"
            if "write" in kv:
                wus = num(kv.get("write_us")) or 0
                detail += f", write {kv.get('write')} ({wus} us)"
        else:
            detail += f" ESR {kv.get('esr')}: Mu needs the T6050 skip (Aurora list) or EL2 must trap it"
        say("item1", f"{kv.get('reg')} {kv.get('name')}: {detail}")

    # Q7
    q7 = [kv for _, g, kv in recs if g == "q7"]
    if q7:
        res = next((kv for kv in q7 if "mismatches" in kv), None)
        if res:
            mm = num(res.get("mismatches"))
            verdict = "WFI keeps x1..x28 and SP: no WFI trap needed" if mm == 0 and res.get("sp") == "ok" else \
                f"WFI CLOBBERED {mm} registers (first x{res.get('first')}: {res.get('got')} != {res.get('expect')}), " \
                f"SP {res.get('sp')}: trap WFI (HCR_EL2.TWI) for Windows"
            say("Q7", f"{verdict}; WFI lasted {res.get('wfi_us')} us for a 10 ms timer")
        elif q7[0].get("wfi") == "begin":
            say("Q7", "WFI NEVER RETURNED: the core lost its state (deep WFI); HCR_EL2.TWI is mandatory")
        else:
            say("Q7", f"not run ({q7[0].get('wfi')})")

    # Q8
    _, v = first(recs, "video")
    if v:
        say("Q8", f"framebuffer {v.get('base')} {v.get('width')}x{v.get('height')} stride {v.get('stride')} "
                  f"depth {v.get('depth')} ({v.get('bpp')} bpp)")
    _, fb = first(recs, "fb")
    if fb:
        say("fb", f"64x64 block drawn: {fb.get('drawn')} colour {fb.get('color')} (look for it at the top-left of the panel)")

    _, vu = first(recs, "vuart")
    if vu:
        rx = vu.get("rx")
        stale = num(vu.get("stale")) or 0
        extra = f"; drained {stale} stale byte(s) first" if stale else ""
        if rx is None:
            say("vuart", f"TX banner sent at {vu.get('base')} (smoke build without the RX self-test)")
        elif rx == "none":
            say("vuart", f"TX banner sent; RX NOTHING within 2 s ({vu.get('polls')} UTRSTAT polls{extra}). Without "
                         "--capture-vuart nobody sends the probe; with it (RUN.txt says 'vuart probe: sent'), host "
                         "bytes do not reach UTRSTAT.RXD: check p.iodev_can_read(IODEV.USB_VUART) in the shell")
        elif stale >= 64:
            say("vuart", f"TX banner sent; the drain hit its 64-byte bound and RX read {rx} at once: UTRSTAT.RXD looks "
                         "stuck at 1 (URXH returning whatever), so this proves nothing about host input")
        else:
            c = num(rx)
            ch = repr(chr(c)) if c is not None and 0 <= c < 256 else "?"
            what = "the launcher's probe" if c == ord("K") else "NOT the launcher's 'K' probe"
            say("vuart", f"TX banner sent; RX {rx} ({ch}, {what}) after {vu.get('rx_us')} us and "
                         f"{vu.get('polls')} UTRSTAT polls{extra}: hv_vuart's host-to-guest path (CDC OUT -> ring -> "
                         "UTRSTAT.RXD -> URXH) works")

    _, pl = first(recs, "pl011")
    if pl:
        say("PL011", f"IDs {pl.get('ids')} (pid {pl.get('pid')} cid {pl.get('cid')}), CR {pl.get('cr')}, FR {pl.get('fr')}, "
                     f"IMSC read-back {pl.get('imsc_rb')}, {pl.get('tx')} bytes sent (check vuart.log)")
    for _, g, kv in recs:
        if g == "mem" and "result" in kv:
            say("stage2", f"{kv.get('at')}: {kv.get('result')}")
    probes = [kv.get("at") for _, g, kv in recs if g == "mem" and kv.get("probe") == "start"]
    done = [kv.get("at") for _, g, kv in recs if g == "mem" and "result" in kv]
    for at in probes:
        if at not in done:
            say("stage2", f"{at}: the access never completed (stage-2 fault: see the EL2 dump in hv.log)")

    # EL2-RUNTIME item 6: Windows touches PMUv3 registers without checking PMUVer
    pmu = [kv for _, g, kv in recs if g == "pmu" and "reg" in kv]
    _, pbeg = first(recs, "pmu", probe="begin")
    _, psum = first(recs, "pmu", probe="summary")
    hw = re.search(r"PMU at EL2: ID_AA64DFR0_EL1 (0x[0-9a-f]+).*?FGT (\d+), MDCR_EL2 (0x[0-9a-f]+) \(([^)]*)\)", run_txt)
    if pbeg:
        say("PMU", f"guest view: PMUVer {pbeg.get('dfr0_pmuver')} (EL2 hides PMUv3 under TID3), MMFR0.FGT "
                   f"{pbeg.get('mmfr0_fgt')}" + (f"; at EL2: ID_AA64DFR0_EL1 {hw.group(1)}, FGT {hw.group(2)}, "
                                                      f"MDCR_EL2 {hw.group(3)} ({hw.group(4)})" if hw else ""))
    for kv in pmu:
        r = kv.get("result")
        if r == "trap":
            detail = f"TRAPPED to EL2 (EL2 ESR {kv.get('el2_esr')}, EC {kv.get('el2_ec')}), emulated" + \
                     (f", read {kv.get('val')}" if kv.get("op") == "read" else "")
        elif r == "undef":
            detail = f"UNDEF AT EL1 (ESR {kv.get('esr')}, EC {kv.get('ec')}): EL2 never saw it"
        else:
            detail = f"completed without EL2 (native PMU), {kv.get('val', '')}"
        say("PMU", f"{kv.get('op')} {kv.get('reg')}: {detail}, {kv.get('us')} us")
    if psum:
        verdict = psum.get("verdict")
        meaning = {
            "TRAP": "every access trapped to EL2: Windows' PMU reads are covered by the EL2 emulation, no fallback",
            "UNDEF": "the accesses UNDEF at EL1 before any EL2 trap: Windows' boot manager dies on its first "
                     "PMCCNTR_EL0 read; a fallback is needed (EL2 cannot see these)",
            "NATIVE": "a real PMUv3 answered (the traps did not take effect)",
            "MIXED": "some registers trap, some do not: see the lines above",
        }.get(verdict, "unknown")
        say("PMU", f"VERDICT {verdict}: {meaning} (trap {psum.get('trap')}, undef {psum.get('undef')}, ok {psum.get('ok')}, "
                   f"PMCCNTR monotonic {psum.get('pmccntr_monotonic')})")
    elif pbeg:
        say("PMU", "probe started but no summary: an access wedged the guest (see hv.log)")

    if not end and not fatal:
        say("stopped", f"after '{last}'; the next probe in order is what hung")
    print("\n".join(out))


if __name__ == "__main__":
    main()
