#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# run_windows.py -- load a Project Mu UEFI FD (or a raw EL1 test image) as an EL1 guest under the m1n1
# hypervisor's Windows personality (the open EL2 runtime). Interface: m5pro-windows docs/CONTRACT.md;
# image facts: docs/BUILD.md "Load requirements"; plan: docs/M0-RUNTIME-PLAN.md.
#
# Pipeline:
#   1. proxy up, HV.init() (stock m1n1 hypervisor bring-up; starts the secondaries into the spin table)
#   2. prune ANS/NVMe, SEP and the proxy USB from the guest ADT (contract s1) and leave their MMIO
#      unmapped in stage 2 (regs read from the unmodified ADT, so nodes HV.setup_adt already removed
#      are still covered); EL2 keeps every DART
#   3. P_HV_WIN_CAPS handshake, P_HV_WIN_CONFIGURE, P_HV_WIN_PL011
#   4. memory plan (below), stage-2 RAM = exactly /chosen/m5w,memory plus the framebuffer, load the
#      image, write boot_args
#   5. publish /chosen/m5w,* (m5w,epoch last, right before the ADT upload)
#   6. HV.start(); report the exit reason (P_HV_WIN_STATUS)
#
# Memory plan (CONTRACT s1/s8, M0-RUNTIME-PLAN open items 2 and 3). Holes inside iBoot's boot_args range:
#   - iBoot's kernel-area data, m1n1 itself, the m1n1 heap and the proxy heap, plus the stock 32 MiB pad:
#     [boot_args.phys_base, heap_top + 32 MiB)
#   - every /chosen/carveout-memory-map region except the ones that contain m1n1's own base (those are
#     iBoot's plan for the XNU kernel area, ordinary RAM; m1n1 runs inside them). MCC TZ registers are
#     not used: m1n1 has no mcc,t6050 driver, so HV.unmap_carveouts() finds nothing on this SoC.
#   - the MTE tag storage, [TagOffset_EL2, top of boot_args RAM) (the top 1/32 of DRAM on T6050)
#   - the framebuffer, if iBoot put it inside the range (it is mapped separately)
#   - no SPRR pool: Windows mode never runs SPTM emulation
# m5w,memory = the rest, 16 KiB aligned, ranges under 2 MiB dropped. boot_args.phys_base/mem_size = the
# one m5w,memory range that contains the Mu handoff block; the ADT, boot_args and the FD are placed at
# its bottom (below the handoff block when they fit), and the FD must end 128 MiB below its top.
#
# Entry registers: hv_enter_guest(x0, x1, x2, x3, entry) erets with x0..x3 from the host, x4 = the entry
# address (ELR is loaded from x4), x5 = 0, x6 = DAIF|EL1h, x7 = the CPU index; nothing else is defined.
# Mu's SEC takes boot_args from x0 when the header has revision 1-3 and version 1-3 (m1n1 copies iBoot's
# boot_args: revision 3, version 2), and only falls back to x4 otherwise, where it would find the FD's
# own header (revision 0x0001, version 0x1000: rejected). Decision: leave hv_enter_guest alone; x0 is
# validated by this launcher before start. If Mu ever needs x4 = boot_args, the change is one line in
# hv_asm.S (after `msr elr_el2, x4`: `mov x4, x0`), which is harmless for macOS/Linux guests.
#
# Modes:
#   default / --m0   Mu FD (sha256 pinned to the canonical M0 FD unless --sha256/--no-verify), m5w,* props
#   --raw-guest      a raw EL1 test image (win_smoke/smoke.bin): entry offset 0, x0 = boot_args,
#                    x1 = uart0 (vuart) base, x2 = PL011 base or 0, x3 = 0x4d355753 ("M5WS");
#                    only the basic m5w,* props (memory, fd-base/size, cpus, counter-hz, epoch)

import argparse
import datetime
import hashlib
import os
import pathlib
import platform
import struct
import subprocess
import sys
import threading
import time

sys.path.append(str(pathlib.Path(__file__).resolve().parents[1]))

# must match src/hv/win/hv_win.h
HV_WIN_MAGIC = 0x57454c32
HV_WIN_IRQ_AIC_PASSTHROUGH = 1
HV_WIN_IRQ_VGIC3 = 2
HV_WIN_F_PSCI_SMC = 1 << 0
HV_WIN_F_RESERVE_CPU = 1 << 1
HV_WIN_F_KEEP_STEAL = 1 << 2
HV_WIN_CAP_VGIC3 = 1 << 2
HV_WIN_EXIT = {0: "none", 1: "SYSTEM_OFF", 2: "SYSTEM_RESET"}

# Contract defaults (docs/CONTRACT.md s1)
SYNTH_WINDOW_BASE = 0xF_0000_0000
SYNTH_WINDOW_SIZE = 0x4000_0000
GICD_DEFAULT = 0xF_0000_0000
GICR_DEFAULT = 0xF_1000_0000
PL011_DEFAULT = 0xF_2000_0000
PL011_FRAME = 0x10000

# Mu facts (docs/BUILD.md, CONTRACT.md s8)
M0_FD_SHA256 = "9523469d8809ccceab7020db8140b92fffc183ac73d7bdd2ee42f987bd91074a"
MU_HANDOFF_BASE = 0x104_0000_0000   # T6050_BOOTARGS_PTR; the ADT goes to +0x4000 (T6050_ADT_PTR)
MU_HANDOFF_SIZE = 0x404000
MU_TOP_WORKSPACE = 128 << 20        # PcdSystemMemoryUefiRegionSize at the top of boot_args RAM
MU_ADT_MAX = 2 << 20                # MemoryInitPeiLib caps the ADT reservation at 2 MiB
FD_ALIGN = 2 << 20                  # 64 KiB is the minimum, 2 MiB recommended

PAGE = 0x4000
HEAP_PAD = 32 << 20                 # stock HV._prepare_load gap above the proxy heap
MIN_RANGE = 2 << 20
SMOKE_X3_MAGIC = 0x4d355753         # "M5WS"
TAGOFFSET_EL2 = (3, 0, 11, 9, 0)    # Apple IMP-DEF, MTE tag storage base on T6050
TAGOFFSET_ADDR = ((1 << 42) - 1) & ~((1 << 20) - 1)
MPIDR_MASK = 0xff_ffff_ffff
HCR_TSC = 1 << 19


def align_up(v, a=PAGE):
    return (v + a - 1) & ~(a - 1)


def align_down(v, a=PAGE):
    return v & ~(a - 1)


def hexint(s):
    return int(s, 0)


class PlanError(Exception):
    pass


def overlaps(a0, a1, b0, b1):
    return a0 < b1 and b0 < a1


def subtract(universe, holes):
    """[start, end) universe minus a list of (start, end, why) holes -> sorted list of (start, end)."""
    out = [universe]
    for h0, h1, _ in holes:
        nxt = []
        for s, e in out:
            if not overlaps(s, e, h0, h1):
                nxt.append((s, e))
                continue
            if s < h0:
                nxt.append((s, h0))
            if h1 < e:
                nxt.append((h1, e))
        out = nxt
    return sorted(out)


def parse_carveouts(adt):
    """/chosen/carveout-memory-map -> [(start, end, 'region-id-N')]."""
    try:
        node = adt["/chosen/carveout-memory-map"]
    except KeyError:
        return []
    regions = []
    for name, val in node._properties.items():
        if not name.startswith("region-id-"):
            continue
        if isinstance(val, (bytes, bytearray)):
            if len(val) < 16:
                continue
            base, size = struct.unpack("<QQ", bytes(val[:16]))
        else:
            base, size = val[0], val[1]
        if base and size:
            regions.append((base, base + size, name))
    return sorted(regions)


def plan_memory(*, phys_base, mem_size, dram_base, dram_size, heap_top, m1n1_base, carveouts, tag_start,
                video, payload_size, devtree_size, mu=True):
    """Pure layout computation; see the header. Raises PlanError with the reason."""
    top = phys_base + mem_size
    u0 = max(phys_base, dram_base)
    u1 = min(top, dram_base + dram_size)
    if u0 >= u1:
        raise PlanError(f"boot_args RAM {phys_base:#x}..{top:#x} is outside DRAM")

    holes = [(phys_base, align_up(heap_top + HEAP_PAD), "iBoot kernel-area data, m1n1, m1n1+proxy heaps, pad")]
    kept = []
    for c0, c1, name in carveouts:
        if not overlaps(c0, c1, u0, u1):
            continue
        if c0 <= m1n1_base < c1:
            kept.append((c0, c1, name))
            continue
        holes.append((align_down(c0), align_up(c1), f"carveout {name}"))
    if tag_start is not None and u0 < tag_start < u1:
        holes.append((align_down(tag_start, 1 << 20), u1, "MTE tag storage (TagOffset_EL2)"))
    fb = None
    if video and video.base and video.stride and video.height:
        fb = (video.base, video.base + align_up(video.stride * video.height))
        if overlaps(fb[0], fb[1], u0, u1):
            holes.append((fb[0], fb[1], "framebuffer"))
    holes.sort()

    ranges = []
    for s, e in subtract((u0, u1), holes):
        s, e = align_up(s), align_down(e)
        if e - s >= MIN_RANGE:
            ranges.append((s, e))
    if not ranges:
        raise PlanError("no guest RAM left after the holes")

    plan = dict(holes=holes, kept=kept, ranges=ranges, fb=fb, universe=(u0, u1))
    h0, h1 = MU_HANDOFF_BASE, MU_HANDOFF_BASE + MU_HANDOFF_SIZE
    adt_sz = align_up(devtree_size)
    if devtree_size > 4 << 20:
        raise PlanError(f"ADT is {devtree_size:#x} bytes; the Mu handoff window holds 4 MiB")

    def place(start):
        adt_base = align_up(start)
        ba = adt_base + adt_sz
        fd = align_up(ba + PAGE, FD_ALIGN)
        return adt_base, ba, fd, fd + align_up(payload_size)

    rng = [r for r in ranges if r[0] <= h0 and h1 <= r[1]]
    if rng:
        rs, re_ = rng[0]
        adt_base, ba, fd, fd_end = place(rs)
        if overlaps(adt_base, fd_end, h0, h1):
            adt_base, ba, fd, fd_end = place(h1)
    elif mu:
        bad = [h for h in holes if overlaps(h[0], h[1], h0, h1)]
        raise PlanError(f"Mu handoff block {h0:#x}..{h1:#x} is not inside one hole-free RAM range; "
                        f"overlapping holes: {[(hex(a), hex(b), w) for a, b, w in bad] or 'outside boot_args RAM'}")
    else:
        rs, re_ = max(ranges, key=lambda r: r[1] - r[0])
        adt_base, ba, fd, fd_end = place(rs)

    plan.update(ba_range=(rs, re_), adt_base=adt_base, bootargs=ba, fd_base=fd, fd_end=fd_end,
                handoff=(h0, h1) if rng else None)
    # Mu: SEC puts its UEFI region (HOBs, boot stack) in the top 128 MiB of phys_base + mem_size
    win = re_ - MU_TOP_WORKSPACE
    if fd_end > win:
        raise PlanError(f"image ends at {fd_end:#x}, inside Mu's top 128 MiB ({win:#x}..{re_:#x})")
    if rng and h1 > win:
        raise PlanError(f"Mu handoff block ends at {h1:#x}, inside Mu's top 128 MiB ({win:#x}..{re_:#x})")
    for a0, a1, what in ((adt_base, adt_base + adt_sz, "ADT"), (ba, ba + PAGE, "boot_args"),
                         (fd, fd_end, "image")):
        if not (rs <= a0 and a1 <= re_):
            raise PlanError(f"{what} {a0:#x}..{a1:#x} is outside boot_args RAM {rs:#x}..{re_:#x}")
        if rng and overlaps(a0, a1, h0, h1):
            raise PlanError(f"{what} {a0:#x}..{a1:#x} overlaps the Mu handoff block")
        for x0, x1, why in holes:
            if overlaps(a0, a1, x0, x1):
                raise PlanError(f"{what} {a0:#x}..{a1:#x} overlaps hole {x0:#x}..{x1:#x} ({why})")
    if fd % (64 << 10):
        raise PlanError(f"image base {fd:#x} is not 64 KiB aligned")
    if fb and any(overlaps(fb[0], fb[1], s, e) for s, e in ranges):
        raise PlanError("framebuffer overlaps guest RAM")
    return plan


def format_plan(plan):
    lines = [f"universe (boot_args RAM in DRAM): {plan['universe'][0]:#x}..{plan['universe'][1]:#x}"]
    for a, b, why in plan["holes"]:
        lines.append(f"  hole  {a:#014x}..{b:#014x} {(b - a) >> 20:6d} MiB  {why}")
    for a, b, name in plan["kept"]:
        lines.append(f"  keep  {a:#014x}..{b:#014x} {(b - a) >> 20:6d} MiB  {name} (contains m1n1: kernel-area plan)")
    for a, b in plan["ranges"]:
        lines.append(f"  RAM   {a:#014x}..{b:#014x} {(b - a) >> 20:6d} MiB  m5w,memory")
    s, e = plan["ba_range"]
    lines.append(f"boot_args phys_base/mem_size: {s:#x} + {e - s:#x} (top {e:#x})")
    lines.append(f"ADT {plan['adt_base']:#x}, boot_args {plan['bootargs']:#x}, image {plan['fd_base']:#x}..{plan['fd_end']:#x}")
    if plan["handoff"]:
        lines.append(f"Mu handoff block {plan['handoff'][0]:#x}..{plan['handoff'][1]:#x}: inside, no overlap")
    if plan["fb"]:
        lines.append(f"framebuffer {plan['fb'][0]:#x}..{plan['fb'][1]:#x} (mapped, not RAM)")
    return "\n".join(lines)


class Tee:
    """stdout/stderr to the terminal and to a log, each log line stamped with the Studio clock."""

    def __init__(self, stream, log):
        self.stream, self.log, self.bol = stream, log, True

    def write(self, s):
        self.stream.write(s)
        for part in s.splitlines(keepends=True):
            if self.bol:
                self.log.write(datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3] + " ")
            self.log.write(part)
            self.bol = part.endswith("\n")
        return len(s)

    def flush(self):
        self.stream.flush()
        self.log.flush()

    def __getattr__(self, name):
        return getattr(self.stream, name)


def capture_serial(dev, path, stop):
    import serial
    with open(path, "ab") as f:
        while not stop.is_set():
            try:
                s = serial.Serial(dev, 115200, timeout=0.5)
                s.dtr = True
            except Exception:
                time.sleep(0.5)
                continue
            while not stop.is_set():
                try:
                    d = s.read(4096)
                except Exception:
                    break
                if d:
                    f.write(d)
                    f.flush()


def secondary_device(dev):
    """The m1n1 USB device's second CDC ACM interface (hv_vuart / PL011 pipe) next to the proxy one."""
    if dev and dev.startswith("/dev/cu.usbmodem") and dev.endswith("1"):
        return dev[:-1] + "3"
    if dev and dev.startswith("/dev/ttyACM") and dev[11:].isdigit():
        return f"/dev/ttyACM{int(dev[11:]) + 1}"
    return None


def git_describe(path):
    try:
        return subprocess.run(["git", "-C", str(path), "describe", "--always", "--dirty", "--tags"],
                              capture_output=True, text=True, timeout=10).stdout.strip()
    except Exception:
        return "?"


def parse_args():
    parser = argparse.ArgumentParser(description="Boot a Mu UEFI FD (or a raw EL1 image) under the Windows EL2 runtime")
    parser.add_argument("image", type=pathlib.Path, help="raw Mu FD, or the raw guest with --raw-guest")
    parser.add_argument("--m0", action="store_true",
                        help="the M0-RUNTIME-PLAN invocation: --route aic -C 1 -E 0x0 -l m0.log, pinned FD hash")
    parser.add_argument("--raw-guest", action="store_true", help="raw EL1 test image (win_smoke/smoke.bin)")
    parser.add_argument("--sha256", help=f"expected sha256 of the image (Mu default: canonical M0 FD {M0_FD_SHA256[:16]}...)")
    parser.add_argument("--no-verify", action="store_true", help="skip the hash check (development only)")
    parser.add_argument("-E", "--entry-offset", type=hexint, default=None, help="entry = image base + this (default 0)")
    parser.add_argument("--route", choices=("aic", "gic"), default=None,
                        help="interrupt route: aic = native AIC passthrough (M0, single CPU, default); gic = emulated GICv3")
    parser.add_argument("-C", "--cpus", type=int, default=None, help="guest vCPU count (default 1)")
    parser.add_argument("--counter-hz", type=int, default=None, help="counter frequency to advertise (default CNTFRQ_EL0)")
    parser.add_argument("--psci-smc", action="store_true", help="also accept PSCI over SMC (sets HCR_EL2.TSC)")
    parser.add_argument("--reserve-cpu", action="store_true", help="keep the top CPU in host context")
    parser.add_argument("--synthetic-window", type=hexint, default=SYNTH_WINDOW_BASE)
    parser.add_argument("--synthetic-size", type=hexint, default=SYNTH_WINDOW_SIZE)
    parser.add_argument("--gicd", type=hexint, default=GICD_DEFAULT)
    parser.add_argument("--gicr", type=hexint, default=GICR_DEFAULT)
    parser.add_argument("--pl011", type=hexint, default=PL011_DEFAULT, help="emulated PL011 base (0 = none)")
    parser.add_argument("--pl011-gsiv", type=hexint, default=None, help="PL011 interrupt GSIV (published as m5w,pl011-gsiv)")
    parser.add_argument("--ramdisk", type=pathlib.Path, help="optional appended WinPE RAM disk")
    parser.add_argument("--ramdisk-offset", type=hexint, default=None,
                        help="RAM-disk offset from the FD base (default: the FD header's image_size, PcdFdSize)")
    parser.add_argument("--run-dir", type=pathlib.Path, default=None,
                        help="run directory for logs and hashes (default runs/<stamp>-<label> in the cwd)")
    parser.add_argument("--label", default=None, help="run label (default m0 / smoke / mu)")
    parser.add_argument("--capture-vuart", action="store_true",
                        help="record the second CDC port (vuart + PL011) to <run>/vuart.log; do not combine with a terminal on that port")
    parser.add_argument("--vuart-dev", default=None, help="second CDC port (default derived from M1N1DEVICE)")
    parser.add_argument("-l", "--logfile", type=pathlib.Path, help="hv trace/TTY log (relative paths go into the run dir)")
    parser.add_argument("-S", "--shell", action="store_true", help="drop to the hypervisor shell before start")
    parser.add_argument("--after", choices=("shell", "reboot", "none"), default="shell",
                        help="what to do when the guest exits (default: hypervisor shell)")
    parser.add_argument("boot_args", default=[], nargs="*")
    args = parser.parse_args()

    if args.m0:
        if args.raw_guest or args.psci_smc or args.ramdisk or args.reserve_cpu \
                or args.route not in (None, "aic") or args.cpus not in (None, 1) or args.entry_offset not in (None, 0):
            parser.error("--m0 is the fixed M0-RUNTIME-PLAN invocation (aic route, 1 CPU, entry 0, no SMC, no RAM disk)")
        if args.logfile is None:
            args.logfile = pathlib.Path("m0.log")
    args.route = args.route or "aic"
    args.cpus = args.cpus if args.cpus is not None else 1
    args.entry_offset = args.entry_offset or 0
    if args.route == "aic" and args.cpus != 1:
        parser.error("the aic route is single-CPU at M0")
    if args.raw_guest and (args.ramdisk or args.entry_offset):
        parser.error("--raw-guest takes no RAM disk and enters at offset 0")
    if args.label is None:
        args.label = "m0" if args.m0 else ("smoke" if args.raw_guest else "mu")
    if args.pl011 and (args.pl011 % PL011_FRAME or not (args.synthetic_window <= args.pl011 and
                       args.pl011 + PL011_FRAME <= args.synthetic_window + args.synthetic_size)):
        parser.error("--pl011 must be a 64 KiB aligned frame inside the synthetic window")
    return parser, args


def main():
    parser, args = parse_args()

    stamp = time.strftime("%Y%m%d-%H%M%S")
    run_dir = args.run_dir or pathlib.Path("runs") / f"{stamp}-{args.label}"
    run_dir.mkdir(parents=True, exist_ok=True)
    os.chmod(run_dir, 0o700)
    hv_log = open(run_dir / "hv.log", "a")
    sys.stdout = Tee(sys.stdout, hv_log)
    sys.stderr = Tee(sys.stderr, hv_log)
    run_txt = open(run_dir / "RUN.txt", "a")

    def record(line):
        print(line)
        run_txt.write(line + "\n")
        run_txt.flush()

    record(f"run_windows.py {' '.join(sys.argv[1:])}")
    record(f"date {datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds')} host {platform.node()}")
    record(f"launcher tree {git_describe(pathlib.Path(__file__).resolve().parents[2])}")
    record(f"run dir {run_dir.resolve()}")

    image = args.image.read_bytes()
    digest = hashlib.sha256(image).hexdigest()
    record(f"image {args.image.resolve()} size {len(image):#x} sha256 {digest}")
    with open(run_dir / "SHA256SUMS", "a") as f:
        f.write(f"{digest}  {args.image.resolve()}\n")
    want = args.sha256.lower() if args.sha256 else (None if args.raw_guest else M0_FD_SHA256)
    if not args.no_verify and want and digest != want:
        record(f"REFUSED: image hash mismatch, expected {want}")
        raise SystemExit(2)
    if not args.no_verify and not want:
        record("image hash not pinned (raw guest): recorded only")

    payload_size = len(image)
    if args.raw_guest:
        payload_size = max(payload_size, 1 << 20)  # room for the test image's .bss and stack
    else:
        # Linux arm64 Image header (BUILD.md): code0/code1, text_offset, image_size, magic "ARM\x64" at 0x38
        if len(image) < 0x40 or image[0x38:0x3c] != b"ARM\x64":
            parser.error("not a Mu FD: no arm64 Image header magic at 0x38")
        text_offset, image_size = struct.unpack("<QQ", image[8:24])
        marker = image[0x100:0x110] if len(image) >= 0x110 else b""
        record(f"Mu FD header: text_offset {text_offset:#x} (not applied), image_size {image_size:#x}, "
               f"marker {marker.decode('ascii', 'replace')!r}")
        payload_size = max(payload_size, image_size)

    payload = image
    rd = None
    if args.ramdisk:
        rd = args.ramdisk.read_bytes()
        rd_off = args.ramdisk_offset if args.ramdisk_offset is not None else payload_size
        if rd_off < len(image):
            parser.error("ramdisk-offset overlaps the FD")
        payload = image + b"\0" * (rd_off - len(image)) + rd
        payload_size = len(payload)
        with open(run_dir / "SHA256SUMS", "a") as f:
            f.write(f"{hashlib.sha256(rd).hexdigest()}  {args.ramdisk.resolve()}\n")

    from m1n1.proxy import M1N1Proxy, UartInterface, IODEV
    from m1n1.proxyutils import ProxyUtils, bootstrap_port
    from m1n1.utils import irange
    from m1n1.shell import run_shell
    from m1n1.sysreg import CNTFRQ_EL0, HCR_EL2
    from m1n1.hv import HV
    from m1n1.hv.types import TraceMode
    from m1n1.adt import load_adt
    from m1n1.tgtypes import BootArgs_r1, BootArgs_r2, BootArgs_r3

    stop_capture = threading.Event()
    if args.capture_vuart:
        vdev = args.vuart_dev or secondary_device(os.environ.get("M1N1DEVICE"))
        if not vdev:
            parser.error("--capture-vuart needs --vuart-dev or M1N1DEVICE=/dev/cu.usbmodem<serial>1")
        threading.Thread(target=capture_serial, args=(vdev, run_dir / "vuart.log", stop_capture), daemon=True).start()
        record(f"capturing the second CDC port {vdev} to vuart.log")

    iface = UartInterface()
    p = M1N1Proxy(iface, debug=False)
    bootstrap_port(iface, p)
    u = ProxyUtils(p, heap_size=128 * 1024 * 1024, m1n1_heap=128 * 1024 * 1024)

    img = iface.readmem(u.base, 0x140000)
    i = img.find(b"##m1n1_ver##")
    ver = img[i + 12:img.index(b"\0", i)].decode() if i >= 0 else "?"
    orig_adt = load_adt(u.get_adt())
    record(f"m1n1 {ver} base {u.base:#x} chip {orig_adt['/chosen'].chip_id:#x}")

    hv = HV(iface, p, u)
    hv.init()
    if args.logfile:
        lf = args.logfile if args.logfile.is_absolute() else run_dir / args.logfile
        hv.set_logfile(lf.open("w"))
    else:
        hv.set_logfile((run_dir / "trace.log").open("w"))
    if args.boot_args:
        hv.set_bootargs(" ".join(args.boot_args))

    counter_hz = args.counter_hz if args.counter_hz is not None else u.mrs(CNTFRQ_EL0)
    record(f"counter {counter_hz} Hz")

    # --- 2. prune the internal SSD, SEP and the proxy USB (contract s1; M0-RAMBOOT s5) ---------------------
    usb_idx = hv.iodev - IODEV.USB0
    exact = {"ans", "sart-ans", "dart-ans", "iop-ans-nub", "sep", "dart-sep",
             f"usb-drd{usb_idx}", f"dart-usb{usb_idx}", f"atc-phy{usb_idx}"}

    def doomed(name):
        n = name.lower()
        return n in exact or bool({"ans", "nvme"} & set(n.split("-")))

    pruned_mmio = []

    def collect(node, path):
        for child in list(node):
            cpath = f"{path}/{child.name}"
            if doomed(child.name):
                try:
                    for idx in range(len(child.reg)):
                        base, size = child.get_reg(idx)
                        if size:
                            pruned_mmio.append((base, size, cpath))
                except Exception:
                    pass
                yield cpath
            else:
                yield from collect(child, cpath)

    for path in list(collect(orig_adt, "")):
        try:
            del hv.adt[path]
            record(f"pruned ADT node {path}")
        except KeyError:
            record(f"ADT node {path} already removed (HV.setup_adt); MMIO still reserved")
    for base, size, path in pruned_mmio:
        hv.add_tracer(irange(base, size), "WIN-PRUNED", TraceMode.RESERVED)
        print(f"  unmapped {base:#x}..{base + size:#x} ({path})")

    # synthetic window: must not collide with MMIO or DRAM
    sw0, sw1 = args.synthetic_window, args.synthetic_window + args.synthetic_size
    for r in orig_adt["/arm-io"].ranges:
        if overlaps(sw0, sw1, r.parent_addr, r.parent_addr + r.size):
            raise SystemExit(f"synthetic window {sw0:#x}..{sw1:#x} overlaps /arm-io range {r.parent_addr:#x}+{r.size:#x}")
    chosen = orig_adt["/chosen"]
    dram_base = getattr(chosen, "dram_base", u.ba.phys_base & ~0xffffffff)
    dram_size = getattr(chosen, "dram_size", u.ba.mem_size_actual)
    if overlaps(sw0, sw1, dram_base, dram_base + dram_size):
        raise SystemExit("synthetic window overlaps DRAM")

    # --- 3. EL2 personality -------------------------------------------------------------------------------
    caps = p.hv_win_caps()
    magic, abi, feats = caps >> 32, (caps >> 16) & 0xffff, caps & 0xffff
    record(f"P_HV_WIN_CAPS: magic {magic:#x} abi {abi} feats {feats:#x}")
    if magic != HV_WIN_MAGIC:
        raise SystemExit("this m1n1 build has no Windows EL2 runtime (wrong caps magic)")
    if args.route == "gic" and not (feats & HV_WIN_CAP_VGIC3):
        raise SystemExit("GICv3 emulation (route gic) is not built/ready in this firmware; use --route aic")

    irq_model = HV_WIN_IRQ_VGIC3 if args.route == "gic" else HV_WIN_IRQ_AIC_PASSTHROUGH
    gicd = args.gicd if args.route == "gic" else 0
    gicr = args.gicr if args.route == "gic" else 0
    flags = (HV_WIN_F_PSCI_SMC if args.psci_smc else 0) | (HV_WIN_F_RESERVE_CPU if args.reserve_cpu else 0)
    rc = p.hv_win_configure(irq_model, gicd, gicr, args.cpus, flags)
    if rc != 0:
        raise SystemExit(f"P_HV_WIN_CONFIGURE failed ({rc:#x})")
    hcr = u.mrs(HCR_EL2)
    tsc = bool(hcr & HCR_TSC)
    record(f"EL2 configured: route {args.route}, {args.cpus} vCPU(s), flags {flags:#x}, HCR_EL2 {hcr:#x} "
           f"(TSC {'set' if tsc else 'clear'}{' - WRITE IGNORED, SMC will not trap' if args.psci_smc and not tsc else ''})")

    if args.pl011:
        rc = p.hv_win_pl011(args.pl011, args.pl011_gsiv or 0)
        if rc != 0:
            raise SystemExit(f"P_HV_WIN_PL011 failed ({rc})")
        hv.add_tracer(irange(args.pl011, PL011_FRAME), "WIN-PL011", TraceMode.RESERVED)
        record(f"PL011 emulated at {args.pl011:#x}, GSIV {args.pl011_gsiv or 0}")

    # --- 4. memory plan, stage 2, load -------------------------------------------------------------------
    # re-read boot_args: anything m1n1 took with top_of_memory_alloc() during hv_init shrank mem_size
    ba = iface.readstruct(u.ba_addr, {2: BootArgs_r2, 3: BootArgs_r3}.get(u.ba_rev, BootArgs_r1))
    try:
        tag = u.mrs(TAGOFFSET_EL2) & TAGOFFSET_ADDR
        tag_note = f"TagOffset_EL2 -> tag storage at {tag:#x}"
    except Exception as e:
        tag = align_down(ba.phys_base + ba.mem_size - dram_size // 32, 1 << 20)
        tag_note = f"TagOffset_EL2 unreadable ({e}); assuming the top 1/32 of DRAM at {tag:#x}"
    record(tag_note)
    try:
        plan = plan_memory(phys_base=ba.phys_base, mem_size=ba.mem_size, dram_base=dram_base,
                           dram_size=dram_size, heap_top=u.heap_top, m1n1_base=u.base,
                           carveouts=parse_carveouts(orig_adt), tag_start=tag, video=ba.video,
                           payload_size=payload_size, devtree_size=ba.devtree_size, mu=not args.raw_guest)
    except PlanError as e:
        raise SystemExit(f"memory plan refused: {e}")
    text = format_plan(plan)
    print(text)
    (run_dir / "memory-plan.txt").write_text(text + "\n")

    for s, e in plan["ranges"]:
        hv.add_tracer(irange(s, e - s), "WIN-RAM", TraceMode.OFF)
    if plan["fb"]:
        hv.add_tracer(irange(plan["fb"][0], plan["fb"][1] - plan["fb"][0]), "WIN-FB", TraceMode.OFF)

    rs, re_ = plan["ba_range"]
    hv.ram_base, hv.ram_size = dram_base, dram_size
    hv.phys_base, hv.mem_size = rs, re_ - rs
    hv.adt_base = plan["adt_base"]
    hv.guest_base = plan["fd_base"]
    hv.bootargs_off = plan["bootargs"] - plan["fd_base"]
    hv.entry = plan["fd_base"] + args.entry_offset
    hv._load_executable_raw(payload, hv.guest_base, "smoke" if args.raw_guest else "Mu FD")
    mm = hv.adt["chosen"]["memory-map"]
    mm.DeviceTree = (hv.adt_base, align_up(u.ba.devtree_size))
    mm.BootArgs = (plan["bootargs"], PAGE)
    hv._write_boot_args(plan["fd_end"])
    record(f"image at {hv.guest_base:#x} entry {hv.entry:#x}; boot_args {plan['bootargs']:#x} = "
           f"phys_base {rs:#x} mem_size {re_ - rs:#x} rev {hv.tba.revision} ver {hv.tba.version}")
    if not (1 <= hv.tba.revision <= 3 and 1 <= hv.tba.version <= 3):
        raise SystemExit("boot_args revision/version outside 1-3: Mu's SEC would not accept x0")

    # --- 5. /chosen/m5w,* ------------------------------------------------------------------------------
    def put(name, value):
        hv.adt["chosen"]._properties[name] = value

    put("m5w,memory", b"".join(struct.pack("<QQ", s, e - s) for s, e in plan["ranges"]))
    put("m5w,fd-base", struct.pack("<Q", hv.guest_base))
    put("m5w,fd-size", struct.pack("<Q", len(image)))
    put("m5w,counter-hz", struct.pack("<Q", counter_hz))
    if rd is not None:
        put("m5w,ramdisk-base", struct.pack("<Q", hv.guest_base + rd_off))
        put("m5w,ramdisk-size", struct.pack("<Q", len(rd)))
    if not args.raw_guest:
        put("m5w,synthetic-window", struct.pack("<QQ", args.synthetic_window, args.synthetic_size))
        if args.pl011:
            put("m5w,pl011", struct.pack("<Q", args.pl011))
            if args.pl011_gsiv is not None:
                put("m5w,pl011-gsiv", struct.pack("<I", args.pl011_gsiv))
        if args.route == "gic":
            put("m5w,gic", struct.pack("<QQ", args.gicd, args.gicr))

    # CPUs the guest may bring up (contract s2): real MPIDRs in ADT order, boot CPU first, minus the host CPU
    cpus = []
    for node in hv.adt["cpus"]:
        v = p.hv_win_cpu_mpidr(node.cpu_id)
        if not v:
            continue
        cpus.append((node.cpu_id, node.name, v & MPIDR_MASK, bool(v >> 63), bool((v >> 62) & 1)))
    cpus.sort(key=lambda c: not c[3])
    usable = [c for c in cpus if not c[4]]
    for cid, name, mpidr, boot, res in cpus:
        print(f"  {name}: cpu-id {cid} MPIDR {mpidr:#x}{' boot' if boot else ''}{' reserved-for-host' if res else ''}")
    if len(usable) < args.cpus:
        raise SystemExit(f"only {len(usable)} usable CPUs for {args.cpus} vCPUs")
    put("m5w,cpus", b"".join(struct.pack("<Q", c[2]) for c in usable[:args.cpus]))
    record(f"m5w,cpus: {', '.join(hex(c[2]) for c in usable[:args.cpus])}")

    if args.raw_guest:
        vuart = orig_adt["/arm-io/uart0"].get_reg(0)[0]
        regs = (vuart, args.pl011 or 0, SMOKE_X3_MAGIC)
        hv_start = p.hv_start
        p.hv_start = lambda entry, x0, *rest: hv_start(entry, x0, *regs)
        record(f"raw guest registers: x0 boot_args, x1 vuart {regs[0]:#x}, x2 PL011 {regs[1]:#x}, x3 {regs[2]:#x}")

    if args.shell:
        run_shell(hv.shell_locals, "Entering hypervisor shell. Type ^D to start the guest.")

    epoch = int(time.time())
    put("m5w,epoch", struct.pack("<Q", epoch))
    blob = hv.adt.build()
    if len(blob) > align_up(u.ba.devtree_size):
        raise SystemExit(f"guest ADT grew to {len(blob):#x} bytes, more than iBoot's {u.ba.devtree_size:#x}")
    if not args.raw_guest and len(blob) > MU_ADT_MAX:
        raise SystemExit(f"guest ADT is {len(blob):#x} bytes; Mu reserves at most {MU_ADT_MAX:#x}")
    adt_out = run_dir / "adt-guest.bin"
    adt_out.write_bytes(blob)
    os.chmod(adt_out, 0o600)
    record(f"hand-off: m5w,epoch {epoch} ({datetime.datetime.fromtimestamp(epoch, datetime.timezone.utc).isoformat()}), "
           f"guest ADT {len(blob):#x} bytes")

    # --- 6. run -------------------------------------------------------------------------------------------
    hv.start()

    status = p.hv_win_status()
    stats = p.hv_win_pl011_stats() if args.pl011 else 0
    record(f"guest exit: {HV_WIN_EXIT.get(status >> 32, status >> 32)} ({status >> 32}), PSCI calls {status & 0xffffffff}, "
           f"PL011 tx {stats & 0xffffffff} rx {(stats >> 32) & 0xffff} dropped {stats >> 48}")
    stop_capture.set()

    if args.after == "shell":
        run_shell(hv.shell_locals, "Guest exited. Hypervisor shell: p.reboot() resets the Mac (next boot = the fallback).")
    elif args.after == "reboot":
        p.reboot()
        return
    p.smp_stop_secondaries(True)
    p.sleep(True)


if __name__ == "__main__":
    main()
