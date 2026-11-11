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
#   3. P_HV_WIN_CAPS handshake; the vCPU list (= /chosen/m5w,cpus) and the synthetic-window frames
#      (GICD, GICR, PL011) are fixed before EL2 is configured; P_HV_WIN_CONFIGURE with that list as its
#      sixth argument, P_HV_WIN_PL011, then one P_HV_WIN_MAP_IRQ per /chosen/m5w,irq-map pair (route gic)
#   4. memory plan (below), stage-2 RAM = exactly /chosen/m5w,memory plus the framebuffer, load the
#      image, write boot_args; the synthetic-window frames must stay clear of every m5w,memory range
#   5. publish /chosen/m5w,* (m5w,epoch last, right before the ADT upload: <u64 seconds><u64 CNTPCT_EL0
#      at that instant>, the 16-byte form Mu's GetTime uses as its base)
#   6. HV.start(); report the exit reason (P_HV_WIN_STATUS) and, with --capture-vuart, the last vuart lines
#      (the capture also answers win_smoke's vuart RX self-test: 'K' 200 ms after the smoke's vuart banner)
#
# --pmu-rewrite el2 (default off; m5pro-windows docs/PMU-EL2-REWRITE.md): right before the guest starts,
# after HV.start() has mapped stage 2, one P_HV_WIN_PMU_REWRITE per m5w,memory range makes guest RAM
# non-executable in stage 2; EL2 then rewrites the PMUv3 accesses of each 16 KiB page at its first
# instruction fetch. P_HV_WIN_PMU_STATS is read at guest exit (RUN.txt line, pmu-rewrite.txt).
#
# USB host (--usb-host N, m5pro-windows docs/USB-HOST.md): the Type-C port of /arm-io/usb-drdN goes to the
# guest as a plain xHCI controller. Never the proxy's port. Stage 2 keeps exactly the controller's register
# window (usb-drdN reg[0]: xHCI + DWC3 globals) passthrough; every DART, every usb-drd/atc-phy/dart-usb block
# of every port and the SPMI buses of the USB-PD controllers are left unmapped (EL2 owns them). After the
# memory plan, P_HV_WIN_USB_PREP (0xc1d) puts the PD controller in S0, stops m1n1's gadget on the port, gives
# both DART instances a DVA == PA map over m5w,memory only, brings the USB2 PHY up in host mode (PIPE on the
# dummy producer: high speed only) and the DWC3 into the host role with the xHC reset and halted. The
# controller's interrupt (usb-drdN interrupts[0]) joins m5w,irq-map as --usb-gsiv (route gic), and
# /chosen/m5w,xhci = <u64 base><u64 size><u32 gsiv><u32 flags> tells Mu where it is (flags: bit 0 DMA
# coherent, bit 1 USB2 only, bit 2 no connect in UEFI (--usb-no-connect), bits 15:8 the port index; gsiv 0 = no
# interrupt, route aic).
#
# Interrupt routes (CONTRACT s3/s9, MU-M1.md section 8):
#   aic  M0 and m1-aic: the guest drives the real AIC itself (HCR_EL2.IMO = 0), one vCPU, no m5w,gic.
#   gic  m1-gic and Windows: EL2 emulates GICv3 (HCR_EL2.IMO/FMO/TID3 set, ID_AA64PFR0_EL1.GIC = 1 in the
#        guest's view), publishes m5w,gic and routes the m5w,irq-map AIC lines to SPIs. EL2 owns the AIC:
#        it is left out of stage 2, Mu's m1-gic profile never touches it.
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
# M1a RAM disk (m5pro-windows docs/MU-M1A.md): --ramdisk takes the .ntasi file from tools/windows/
# make-ramdisk-image.sh (Aurora's 4 KiB NTASI header + a GPT/FAT32 disk) and appends it at exactly fd_base + the FD
# header's image_size (= PcdFdSize, where Mu's SEC probes). The file is checked before the proxy is touched (header
# fields and CRCs, length = header + image, image <= 1 GiB, GPT partition 1 = the only basic-data partition with
# the NTASI unique GUID), uploaded in 16 MiB compressed chunks (one gzip of ~600 MiB of WIM would need a heap block
# larger than the 128 MiB proxy heap), and published as m5w,ramdisk-base (the header) / m5w,ramdisk-size (the file
# length), which Mu's J716sRamdiskDxe compares with what SEC found. --mu-profile m1a requires it.
#
# Modes:
#   default / --m0   Mu FD, sha256 pinned to the known image of --mu-profile (m0 by default; m1-gic, m1-aic, m1a)
#                    unless --sha256/--no-verify; m5w,* props. --m0 = profile m0 on the M0 invocation.
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
import zlib

sys.path.append(str(pathlib.Path(__file__).resolve().parents[1]))

# must match src/hv/win/hv_win.h
HV_WIN_MAGIC = 0x57454c32
HV_WIN_IRQ_AIC_PASSTHROUGH = 1
HV_WIN_IRQ_VGIC3 = 2
HV_WIN_F_PSCI_SMC = 1 << 0
HV_WIN_F_RESERVE_CPU = 1 << 1
HV_WIN_F_KEEP_STEAL = 1 << 2
HV_WIN_F_NO_WFI_TRAP = 1 << 3  # smp2d: disable EL2 WFI trapping (control; default is trapping ON for >1 vCPU)
HV_WIN_CAP_VGIC3 = 1 << 2
HV_WIN_CAP_PMU_RW = 1 << 4
HV_WIN_CAP_FB_SHADOW = 1 << 5
HV_WIN_PMU_MAGIC = 0x504d5552          # "PMUR", P_HV_WIN_PMU_STATS(0) >> 32
HV_WIN_PMU_STATS = ("id", "pages", "ranges", "tables", "scans", "pages_rewritten", "insns", "insns_pmccntr",
                    "insns_mrs", "insns_msr", "write_flips", "walk_flips", "pins", "pin_full", "sweeps",
                    "unpins", "spurious", "ipa_fail", "not_ours", "stuck")   # src/hv/win/hv_win.h order
HV_WIN_PMU_LOG_SIZE = 256
HV_WIN_PMU_STAT_LOG_IPA, HV_WIN_PMU_STAT_LOG_OP, HV_WIN_PMU_STAT_LOG_ELR = 0x100, 0x200, 0x300
GIC_SPI_MIN, GIC_SPI_MAX = 32, 1019  # GICD_TYPER.ITLinesNumber 31 (hv_vgic3.c)
HV_WIN_EXIT = {0: "none", 1: "SYSTEM_OFF", 2: "SYSTEM_RESET"}

# Contract defaults (docs/CONTRACT.md s1)
SYNTH_WINDOW_BASE = 0xF_0000_0000
SYNTH_WINDOW_SIZE = 0x4000_0000
GICD_DEFAULT = 0xF_0000_0000
GICR_DEFAULT = 0xF_1000_0000
PL011_DEFAULT = 0xF_2000_0000
PL011_FRAME = 0x10000
GICD_FRAME = 0x10000
MAX_VCPUS = 24                      # src/smp_asm.h MAX_CPUS: EL2's vCPU table and vGIC frame limit
GICR_STRIDE = 0x20000               # RD_base + SGI_base per vCPU, GICR frame i = m5w,cpus[i]

# Mu facts (docs/BUILD.md, CONTRACT.md s8). Known images by profile (BUILD.md table; all 0x1e00000 bytes,
# build/mu-out/MacBookPro16M5/<mu-commit>/j716s-<profile>/artifacts/J716MACBOOKPRO16M5_EFI.fd) and the
# interrupt route each one runs on.
MU_IMAGES = {
    "m0": ("aic", "0385facf4b9d57c6ab07ac598f38165b8a7adc1fe3613fc222403af39dfacbf5"),
    "m1-gic": ("gic", "2322e8152ecb3bf918a963bdb34a144c747bc62c2045a832cb4444cd4459b4f0"),
    "m1-aic": ("aic", "a46428a340507ff672f7f4f79c73ab78c792bce06411935bf966afc7d06cd6c2"),
    "m1a": ("gic", "e1d162b11ce07bf6b88908ea8c1a6cb0526e4cac2dd8d82d551c607f12a5d897"),
    # USB host profiles (docs/USB-HOST.md; --usb-host), m5pro-windows 12a245f + the USB host driver,
    # J716S_PMU_FALLBACK=FALSE
    "m1-gic-usb": ("gic", "fefa29702b69f186710865ba1a0be39d23bb4e4f192e541fd2c23c76c79fe0ea"),
    "m1-aic-usb": ("aic", "cf16807d703d4e6075d900e6ab65b2b499fdcf341e64455e7f5891f6f22fd26c"),
}
MU_NEEDS_RAMDISK = {"m1a"}
USB_PROFILES = ("m1-gic-usb", "m1-aic-usb")
MU_HANDOFF_BASE = 0x104_0000_0000   # T6050_BOOTARGS_PTR; the ADT goes to +0x4000 (T6050_ADT_PTR)
MU_HANDOFF_SIZE = 0x404000
MU_TOP_WORKSPACE = 128 << 20        # PcdSystemMemoryUefiRegionSize at the top of boot_args RAM
MU_ADT_MAX = 2 << 20                # MemoryInitPeiLib caps the ADT reservation at 2 MiB
FD_ALIGN = 2 << 20                  # 64 KiB is the minimum, 2 MiB recommended

# Aurora's appended RAM disk (third_party/mu Silicon/Apple/AppleSiliconPkg/Include/AppendedRamdisk.h, version 2)
NTASI_SIGNATURE = b"NTASIRDK"
NTASI_VERSION = 2
NTASI_HEADER_SIZE = 0x1000
NTASI_MAX_IMAGE = 0x4000_0000                       # NTASI_APPENDED_RAMDISK_MAX_IMAGE_SIZE
NTASI_PART_GUID = bytes.fromhex("5341544e2d4942479414000000000002")  # 4e544153-492d-4742-9414-000000000002
GPT_BASIC_DATA = bytes.fromhex("a2a0d0ebe5b9334487c068b6b72699c7")   # ebd0a0a2-b9e5-4433-87c0-68b6b72699c7
RAMDISK_CHUNK = 16 << 20                            # compressed upload unit (the proxy heap is 128 MiB)

PAGE = 0x4000
HEAP_PAD = 32 << 20                 # stock HV._prepare_load gap above the proxy heap
MIN_RANGE = 2 << 20
SMOKE_X3_MAGIC = 0x4d355753         # "M5WS"
TAGOFFSET_EL2 = (3, 0, 11, 9, 0)    # Apple IMP-DEF, MTE tag storage base on T6050
TAGOFFSET_ADDR = ((1 << 42) - 1) & ~((1 << 20) - 1)
MPIDR_MASK = 0xff_ffff_ffff
ID_AA64DFR0 = (3, 0, 0, 5, 0)
ID_AA64MMFR0 = (3, 0, 0, 7, 0)
MDCR_EL2 = (3, 4, 1, 1, 1)
MDCR_TPMCR, MDCR_TPM = 1 << 5, 1 << 6
HDFGRTR_EL2 = (3, 4, 3, 1, 4)       # FEAT_FGT only
HDFGWTR_EL2 = (3, 4, 3, 1, 5)
HCR_FMO = 1 << 3
HCR_IMO = 1 << 4
HCR_TID3 = 1 << 18
HCR_TSC = 1 << 19

# P_HV_WIN_USB_PREP (src/usb.h USB_HOST_F_* / USB_HOST_INFO_* / USB_HOST_E_*)
USB_HOST_F_DART_BYPASS = 1 << 0
USB_HOST_F_LEGACY_PHY = 1 << 1
USB_HOST_F_NO_HCRST = 1 << 2
USB_HOST_F_LOCK_DART = 1 << 3
USB_HOST_F_KEEP_PD = 1 << 4
USB_HOST_F_PHY_BEFORE_RESET = 1 << 5
USB_HOST_ERRORS = {-1: "bad port or flags", -2: "not a T6050/T6051", -3: "no usb-drd/atc-phy/dart-usb set in the ADT",
                   -4: "the proxy's or the vuart's port, or a guest is running",
                   -5: "identity ranges invalid, outside DRAM or over m1n1", -6: "USB-PD controller not in S0",
                   -7: "an ADT tunable is missing or malformed", -8: "PIPE not on the dummy producer",
                   -9: "DART locked, too narrow, or refused the tables", -10: "USB2 PHY did not reach the run state",
                   -11: "DWC3 core init failed", -12: "xHC missing or its reset failed"}
M5W_XHCI_F_COHERENT = 1 << 0
M5W_XHCI_F_USB2_ONLY = 1 << 1
M5W_XHCI_F_NO_CONNECT = 1 << 2
USB_GSIV_BASE = 64                  # default --usb-gsiv: 64 + port index


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


def check_ntasi(rd):
    """The --ramdisk file as Mu's SEC, BootRamdiskHelperDxe and J716sRamdiskDxe will judge it. Returns
    (image_size, image_crc32); raises PlanError with the reason (checked before the proxy is touched)."""
    if len(rd) < NTASI_HEADER_SIZE + 34 * 512 or rd[:8] != NTASI_SIGNATURE:
        raise PlanError("not an NTASI RAM-disk file (no 'NTASIRDK' header): pass the .ntasi file from "
                        "tools/windows/make-ramdisk-image.sh, not the raw .img")
    _, version, hsize, isize, icrc, hcrc = struct.unpack_from("<QIIQII", rd, 0)
    if version != NTASI_VERSION or hsize != NTASI_HEADER_SIZE:
        raise PlanError(f"NTASI header version {version} / size {hsize:#x}; Mu takes version 2, 0x1000")
    if zlib.crc32(rd[:28]) != hcrc:
        raise PlanError("NTASI header CRC32 mismatch")
    if not 512 <= isize <= NTASI_MAX_IMAGE:
        raise PlanError(f"RAM-disk image {isize:#x} bytes; Mu takes 512 B .. 1 GiB")
    if len(rd) != hsize + isize or len(rd) % 0x1000:
        raise PlanError(f"file is {len(rd):#x} bytes, header + image = {hsize + isize:#x} (must be equal, 4 KiB "
                        "multiple: m5w,ramdisk-size must equal what SEC reserves)")
    if zlib.crc32(memoryview(rd)[hsize:]) != icrc:
        raise PlanError("RAM-disk image CRC32 mismatch (BootRamdiskHelperDxe would refuse it)")
    disk = memoryview(rd)[hsize:]
    if bytes(disk[510:512]) != b"\x55\xaa" or bytes(disk[512:520]) != b"EFI PART":
        raise PlanError("RAM-disk image has no GPT (BootRamdiskHelperDxe only selects HD(1,GPT,...))")
    lba, count, esize = struct.unpack_from("<QII", disk, 512 + 72)
    if not (2 <= lba and 0 < count <= 4096 and 128 <= esize <= 1024 and lba * 512 + count * esize <= len(disk)):
        raise PlanError("RAM-disk GPT entry array out of bounds")
    basic = [i for i in range(count) if bytes(disk[lba * 512 + i * esize:lba * 512 + i * esize + 16]) == GPT_BASIC_DATA]
    first = bytes(disk[lba * 512 + 16:lba * 512 + 32])
    if basic != [0] or first != NTASI_PART_GUID:
        raise PlanError("RAM-disk GPT: partition 1 must be the only basic-data partition, unique GUID "
                        "4e544153-492d-4742-9414-000000000002 (what BootRamdiskHelperDxe selects)")
    return isize, icrc


def parse_irq_map(specs):
    """--irq-map / --irq-map-file text -> [(aic_line, gsiv)]; pairs 'line:gsiv' separated by commas,
    whitespace or newlines, '#' starts a comment. Each gsiv an SPI 32..1019; lines and gsivs unique."""
    pairs = []
    for spec in specs:
        for raw in spec.splitlines():
            for tok in raw.split("#", 1)[0].replace(",", " ").split():
                line, sep, gsiv = tok.partition(":")
                try:
                    line, gsiv = int(line, 0), int(gsiv, 0)
                except ValueError:
                    raise ValueError(f"irq map entry {tok!r} is not aic_line:gsiv") from None
                if not sep or line < 0 or line > 0xffffffff:
                    raise ValueError(f"irq map entry {tok!r}: bad AIC line")
                if not GIC_SPI_MIN <= gsiv <= GIC_SPI_MAX:
                    raise ValueError(f"irq map entry {tok!r}: gsiv must be an SPI {GIC_SPI_MIN}..{GIC_SPI_MAX}")
                pairs.append((line, gsiv))
    for i, what in ((0, "AIC line"), (1, "gsiv")):
        seen = set()
        for pr in pairs:
            if pr[i] in seen:
                raise ValueError(f"irq map: {what} {pr[i]} appears twice")
            seen.add(pr[i])
    return pairs


def check_frames(window, frames, ram=()):
    """EL2-only frames [(name, base, size)] must lie inside the synthetic window [w0, w1), be 64 KiB
    aligned, overlap neither each other nor any guest RAM range [(start, end)]. Raises PlanError."""
    w0, w1 = window
    for name, base, size in frames:
        if base % 0x10000 or size <= 0:
            raise PlanError(f"{name} {base:#x}+{size:#x} is not a 64 KiB aligned frame")
        if not (w0 <= base and base + size <= w1):
            raise PlanError(f"{name} {base:#x}..{base + size:#x} is outside the synthetic window {w0:#x}..{w1:#x}")
        for s, e in ram:
            if overlaps(base, base + size, s, e):
                raise PlanError(f"{name} {base:#x}..{base + size:#x} overlaps m5w,memory {s:#x}..{e:#x}")
    for i, (n0, b0, s0) in enumerate(frames):
        for n1, b1, s1 in frames[i + 1:]:
            if overlaps(b0, b0 + s0, b1, b1 + s1):
                raise PlanError(f"{n0} {b0:#x}..{b0 + s0:#x} overlaps {n1} {b1:#x}..{b1 + s1:#x}")


def window_frames(args):
    frames = []
    if args.route == "gic":
        frames += [("GICD", args.gicd, GICD_FRAME), ("GICR", args.gicr, args.cpus * GICR_STRIDE)]
    if args.pl011:
        frames.append(("PL011", args.pl011, PL011_FRAME))
    if getattr(args, "cpu_classes", None) == "cppc":
        frames.append(("CPPC", args.cppc_page, CPPC_PAGE_SIZE))
    return frames


def node_regs(node):
    out = []
    try:
        for idx in range(len(node.reg)):
            base, size = node.get_reg(idx)
            if size:
                out.append((idx, base, size))
    except Exception:
        pass
    return out


def usb_host_plan(adt, port, proxy_port):
    """--usb-host: the controller window the guest keeps, its AIC line, and the MMIO EL2 keeps for itself
    (every DART, every usb-drd/atc-phy/dart-usb block but this controller's reg[0], the SPMI buses that carry
    the USB-PD controllers). Raises PlanError."""
    if port == proxy_port:
        raise PlanError(f"usb-drd{port} carries the proxy; it never goes to the guest")
    try:
        drd = adt[f"/arm-io/usb-drd{port}"]
        adt[f"/arm-io/atc-phy{port}"]
        dart = adt[f"/arm-io/dart-usb{port}"]
    except KeyError:
        raise PlanError(f"no usb-drd{port}/atc-phy{port}/dart-usb{port} in the ADT") from None
    if "dart,t8110" not in list(getattr(dart, "compatible", [])):
        raise PlanError(f"dart-usb{port} is not a dart,t8110")
    _, xbase, xsize = node_regs(drd)[0]
    line = list(drd.interrupts)[0]
    if hasattr(adt["/arm-io/aic"], "aic-numdies") and getattr(adt["/arm-io/aic"], "aic-numdies", 1) != 1:
        raise PlanError("multi-die AIC: the irq-map line numbering (die * max_irq + num) is not handled here")

    keep = []
    usb_names = {f"{k}{i}" for k in ("usb-drd", "atc-phy", "dart-usb") for i in range(16)}
    spmi_with_hpm = set()

    def walk(node, path):
        for child in node:
            cpath = f"{path}/{child.name}"
            compat = getattr(child, "compatible", [])
            compat = [compat] if isinstance(compat, str) else list(compat or [])
            if any(str(c).startswith("usbc,sn2012") for c in compat):
                spmi_with_hpm.add(path)
            yield cpath, child, compat
            yield from walk(child, cpath)

    nodes = list(walk(adt["/arm-io"], "/arm-io"))
    for cpath, child, compat in nodes:
        name = child.name
        dart_node = any(str(c).startswith("dart,") for c in compat)
        if not (dart_node or name in usb_names or cpath in spmi_with_hpm):
            continue
        for idx, base, size in node_regs(child):
            if name == f"usb-drd{port}" and xbase <= base and base + size <= xbase + xsize:
                continue  # the controller window itself (reg[0] and the Apple blocks inside it)
            keep.append((base, size, f"{cpath} reg[{idx}]"))
    return dict(xhci=(xbase, xsize), line=line, el2=keep, spmi=sorted(spmi_with_hpm))


def epoch_with_counter(t0, t1, counter, cntfrq):
    """m5w,epoch 16-byte form: whole seconds and the counter value at that second, from a counter read
    taken between the wall-clock samples t0 and t1 (taken as their midpoint)."""
    t = (t0 + t1) / 2
    seconds = int(t)
    return seconds, max(0, counter - round((t - seconds) * cntfrq))


def uniform_cluster_types(adt):
    """sched1: give every /cpus/* node the boot CPU's cluster-type, so Mu (J716sCpuEfficiencyClasses, the only
    reader of the guest ADT's cluster-type) publishes one efficiency class (0) in the MADT GICC entries.

    Why: with the ADT classes (M = 0 for CPUs 0-11, P = 1 for CPUs 12-17) Windows applies its heterogeneous
    core-parking policy, where an efficiency-class-1 core is unparked only when class-0 *performance* crosses
    HeteroIncreaseThreshold. The firmware publishes no performance control (_CPC/_PSS), and on hardware
    (runs/20261110-213116-smpN c18a4) CPUs 12-17 stayed parked with 18 busy loops while CPUs 0-11 ran them.
    The hypervisor and Windows never read this property; the host ADT is unchanged.
    Returns [(node name, old, new)] for the nodes changed."""
    cpus = list(adt["/cpus"])
    if not cpus:
        return []
    boot = next((c for c in cpus if c._properties.get("state") == "running"), cpus[0])
    want = boot._properties.get("cluster-type")
    if not isinstance(want, str) or not want:
        return []
    changed = []
    for c in cpus:
        old = c._properties.get("cluster-type")
        if old is not None and old != want:
            c._properties["cluster-type"] = want
            changed.append((c.name, old, want))
    return changed


# cpuperf: T6050 operating points and capacities (perf/DESIGN.md, perf/WRITES.md).
# OPP tables: the owner's Linux DT, linux-cs wt-power 0fdf38de arch/arm64/boot/dts/apple/t6050-base.dtsi
# (opp-table-mcluster opp1-opp16, opp16 turbo-mode; opp-table-pcluster opp1-opp21); opp-level = hardware p-state.
T6050_OPP_MHZ = {
    "M": {1: 300, 2: 1344, 3: 1644, 4: 1992, 5: 2304, 6: 2652, 7: 2964, 8: 3240, 9: 3504, 10: 3696, 11: 3876,
          12: 4044, 13: 4176, 14: 4284, 15: 4308, 16: 4380},
    "P": {1: 300, 2: 1308, 3: 1620, 4: 1980, 5: 2292, 6: 2580, 7: 2880, 8: 3180, 9: 3432, 10: 3648, 11: 3828,
          12: 3984, 13: 4104, 14: 4188, 15: 4236, 16: 4284, 17: 4308, 18: 4332, 19: 4428, 20: 4512, 21: 4608},
}
# Capacity per MHz: the T6050 DT has none. The same Linux tree gives the same core compatibles
# (apple,sawtooth = the T6050 M tier, apple,everest = the P tier) capacity-dmips-mhz 851 / 1024 on T6030
# (t6030.dtsi). That is the closest Linux-derived figure; it is NOT a T6050 measurement (t604x.dtsi:33-37
# shows how to measure it: pinned single-core throughput at the lowest p-state).
T6050_CAPACITY = {"M": 851, "P": 1024}
T6050_PMIN = 1                       # soc_t6050_info.min_pstate
T6050_PMAX = {"M": 15, "P": 17}      # default clamp: M p16 is turbo-mode (refused); P stops at its nominal 4308 MHz
T6050_PERF_DIV = 4608                # P p21 (fastest OPP) at capacity 1024 = 1024


def t6050_perf(ctype, pstate):
    """CPPC perf of one operating point: round(MHz * capacity / 4608)."""
    return (T6050_OPP_MHZ[ctype][pstate] * T6050_CAPACITY[ctype] + T6050_PERF_DIV // 2) // T6050_PERF_DIV


def t6050_cpc(ctype, pmax=None, pmin=T6050_PMIN):
    """_CPC tuple (highest, nominal, lowest-nonlinear, lowest, lowest MHz, nominal MHz) for the clamp
    [pmin, pmax]: highest = nominal = pmax (no boost is ever granted), lowest-nonlinear = the next state
    above pmin (p2: p1 300 MHz is the idle floor), lowest = pmin."""
    pmax = T6050_PMAX[ctype] if pmax is None else pmax
    lnl = min(pmin + 1, pmax)
    top = t6050_perf(ctype, pmax)
    return (top, top, t6050_perf(ctype, lnl), t6050_perf(ctype, pmin), T6050_OPP_MHZ[ctype][pmin],
            T6050_OPP_MHZ[ctype][pmax])


# Tuple: (highest, nominal, lowest-nonlinear, lowest, lowest MHz, nominal MHz).
# Derived: M 796/796/248/55 MHz 300-4308, P 957/957/291/67 MHz 300-4308.
CPPC_PERF_T6050 = {t: t6050_cpc(t) for t in ("M", "P")}

# cpuperf-B: vCPU MPIDR cluster (Aff2:Aff1) -> (cluster DVFS block, expected cluster-type).
# linux-cs wt-power t6050-dieX.dtsi:17-36 (cpufreq_mc0/mc1/pc) and t6050-max.dtsi:30-47 (CPU -> domain).
T6050_DVFS = {0x040000: (0x210e20000, "M"), 0x040100: (0x211e20000, "M"), 0x010200: (0x212e20000, "P")}
DVFS_CFG_MAGIC = 0x53465644
DVFS_CFG_SIZE = 632


T6050_FIXED_DEFAULT = {"M": 6, "P": 6}   # --cpu-dvfs fixed: mc0's iBoot level (p6 2652 MHz) on every cluster


def parse_dvfs_mode(text):
    """--cpu-dvfs off | el2 | fixed[:M=n,P=n] -> (mode, {type: pstate} for fixed). Raises ValueError."""
    if text in ("off", "el2"):
        return text, {}
    head, _, rest = text.partition(":")
    if head != "fixed":
        raise ValueError(f"--cpu-dvfs {text!r}: off, el2 or fixed[:M=n,P=n]")
    fixed = dict(T6050_FIXED_DEFAULT)
    for part in filter(None, rest.split(",")):
        t, _, v = part.partition("=")
        if t not in T6050_PMAX or not v.isdigit() or int(v) not in T6050_OPP_MHZ[t] or int(v) < T6050_PMIN \
                or int(v) > T6050_PMAX[t]:
            raise ValueError(f"--cpu-dvfs {text!r}: {part!r} must be M=1..{T6050_PMAX['M']} or P=1..{T6050_PMAX['P']}")
        fixed[t] = int(v)
    return "fixed", fixed


def dvfs_plan(adt, vcpus, pmax=None, interval_ns=1000000, fixed=None):
    """cpuperf-B: the P_HV_WIN_CPPC_DVFS config (hv_win_dvfs_core.h struct dvfs_cfg) and a summary.
    fixed = {type: pstate}: those clusters get pmin = pmax = pstate (EL2 drives them there with no Desired).
    Raises PlanError for a vCPU outside the three T6050 clusters or with the wrong cluster-type."""
    pmax = dict(T6050_PMAX, **(pmax or {}))
    nodes = {c.name: c for c in adt["/cpus"]}
    order, cluster_of = [], []
    for _, name, mpidr, _ in vcpus:
        key = mpidr & 0xffff00
        if key not in T6050_DVFS:
            raise PlanError(f"{name}: MPIDR {mpidr:#x} is in no T6050 DVFS cluster")
        base, want = T6050_DVFS[key]
        node = nodes.get(name)
        got = node._properties.get("cluster-type") if node is not None else None
        if got != want:
            raise PlanError(f"{name}: cluster-type {got!r}, the {base:#x} cluster is {want!r}")
        if base not in order:
            order.append(base)
        cluster_of.append(order.index(base))
    if len(vcpus) > 32 or interval_ns < 1000000:
        raise PlanError("cpuperf-B: at most 32 vCPUs and a rate limit of at least 1 ms")
    out = struct.pack("<IIIIQ", DVFS_CFG_MAGIC, 1, len(order), len(vcpus), interval_ns)
    out += bytes(cluster_of) + bytes(32 - len(cluster_of))
    lines = []
    for i in range(4):
        if i >= len(order):
            out += bytes(144)
            continue
        base = order[i]
        t = next(v[1] for v in T6050_DVFS.values() if v[0] == base)
        hi = pmax[t]
        if not (T6050_PMIN <= hi <= max(T6050_OPP_MHZ[t])):
            raise PlanError(f"--dvfs-max {t}={hi}: outside p{T6050_PMIN}..p{max(T6050_OPP_MHZ[t])}")
        perf = [t6050_perf(t, p) if p in T6050_OPP_MHZ[t] else 0 for p in range(32)]
        lo = T6050_PMIN
        if fixed and t in fixed:
            lo = hi = fixed[t]
        out += struct.pack("<QII32I", base, lo, hi, *perf)
        lines.append((i, base, t, lo, hi))
    assert len(out) == DVFS_CFG_SIZE
    return out, lines


CPPC_PAGE_DEFAULT = 0xF_3C00_0000   # 16 KiB, clear of GICD/GICR/PL011/S1 ECAM+BAR and the ANSM REG/OP frames
CPPC_PAGE_SIZE = 0x4000
CPPC_SLOT = 0x40
CPPC_FLAG_PSD = 1
CPPC_FLAG_AUTONOMOUS = 2   # _CPC Autonomous Selection Enable = 1 (platform-managed); Desired still published


def parse_cppc_perf(text):
    """--cppc-perf TYPE=h,n,lnl,l,lowMHz,nomMHz -> (type, tuple). Raises ValueError."""
    t, _, vals = text.partition("=")
    v = tuple(int(x, 0) for x in vals.split(","))
    if t not in ("E", "M", "P") or len(v) != 6:
        raise ValueError(f"--cppc-perf {text!r}: want TYPE=highest,nominal,lowest-nonlinear,lowest,lowMHz,nomMHz")
    check_cppc_perf(t, v)
    return t, v


def check_cppc_perf(t, v):
    h, n, lnl, lo, lmhz, nmhz = v
    if not (0 < lo <= lnl <= n <= h <= 0xFFFF) or not (0 < lmhz <= nmhz <= 0xFFFF):
        raise ValueError(f"cpu type {t}: CPPC values {v} break lowest <= lowest-nonlinear <= nominal <= highest "
                         f"<= 0xffff or 0 < lowMHz <= nomMHz <= 0xffff (Mu J716sParseCppc refuses them)")


def cppc_plan(adt, vcpus, page, psd=True, table=None, autonomous=False):
    """cpuperf-A: the /chosen/m5w,cppc bytes for Mu (J716sAcpiGenLib.h layout) and one line per vCPU.
    vcpus = [(cpu_id, node name, mpidr, is_boot)] in m5w,cpus order. Domain = the first vCPU of the same
    MPIDR cluster (Aff3..Aff1). Raises PlanError when a CPU has no usable cluster-type."""
    table = dict(CPPC_PERF_T6050 if table is None else table)
    if page % CPPC_PAGE_SIZE or len(vcpus) * CPPC_SLOT > CPPC_PAGE_SIZE:
        raise PlanError(f"CPPC page {page:#x} not 16 KiB aligned or too small for {len(vcpus)} vCPUs")
    nodes = {c.name: c for c in adt["/cpus"]}
    first = {}
    flags = (CPPC_FLAG_PSD if psd else 0) | (CPPC_FLAG_AUTONOMOUS if autonomous else 0)
    out = struct.pack("<QIII", page, CPPC_SLOT, len(vcpus), flags)
    lines = []
    for i, (_, name, mpidr, _) in enumerate(vcpus):
        node = nodes.get(name)
        ctype = node._properties.get("cluster-type") if node is not None else None
        if ctype not in table:
            raise PlanError(f"{name}: cluster-type {ctype!r} has no CPPC values (have {sorted(table)})")
        v = table[ctype]
        check_cppc_perf(ctype, v)
        dom = first.setdefault(mpidr & ~0xff, i)
        out += struct.pack("<7I", *v, dom)
        lines.append((i, name, ctype, v, dom))
    return out, lines


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


def validate_fb_shadow(base, size, *, host_base, host_limit, native_base, native_size, ranges=()):
    """Validate a dedicated C-heap allocation, never ordinary guest/proxy RAM."""
    if not isinstance(base, int) or base <= 0 or base % PAGE or size <= 0 or size % PAGE:
        raise PlanError("invalid framebuffer shadow allocation/alignment")
    end = base + size
    if end > 1 << 64 or base < host_base or end > host_limit:
        raise PlanError("framebuffer shadow is outside the excluded C heap")
    if overlaps(base, end, native_base, native_base + native_size):
        raise PlanError("framebuffer shadow overlaps native scanout")
    if any(overlaps(base, end, a, b) for a, b in ranges):
        raise PlanError("framebuffer shadow overlaps guest RAM")
    return base, end


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


# win_smoke's vuart RX self-test: the guest drains stale input, prints this banner on uart0 and then polls
# UTRSTAT.RXD for up to 2 s; the capture answers with one byte, once per run
VUART_PROBE_MARK = b"M5W-SMOKE vuart hello"
VUART_PROBE_BYTE = b"K"
VUART_PROBE_DELAY = 0.2


def capture_serial(dev, path, stop, note=print):
    import serial
    tail = b""
    probe_at = None
    probed = False
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
                    timeout = 0.02 if probe_at is not None else 0.5
                    if s.timeout != timeout:  # pyserial reprograms the port on every assignment
                        s.timeout = timeout
                    d = s.read(max(1, s.in_waiting))
                except Exception:
                    break
                if d:
                    f.write(d)
                    f.flush()
                    if not probed and probe_at is None:
                        tail += d
                        if VUART_PROBE_MARK in tail:
                            probe_at = time.monotonic() + VUART_PROBE_DELAY
                        tail = tail[-(len(VUART_PROBE_MARK) - 1):]
                if probe_at is not None and time.monotonic() >= probe_at:
                    try:
                        s.write(VUART_PROBE_BYTE)  # no flush(): tcdrain() can block on a stalled CDC OUT pipe
                    except Exception:
                        break
                    probe_at, probed = None, True
                    note(f"vuart probe: sent {VUART_PROBE_BYTE!r} to {dev} {VUART_PROBE_DELAY * 1000:.0f} ms after "
                         "the smoke's vuart banner")


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
                        help="the M0-RUNTIME-PLAN invocation: --mu-profile m0 --route aic -C 1 -E 0x0 -l m0.log, pinned FD hash")
    parser.add_argument("--raw-guest", action="store_true", help="raw EL1 test image (win_smoke/smoke.bin)")
    parser.add_argument("--mu-profile", choices=sorted(MU_IMAGES), default="m0",
                        help="which known Mu image to expect (pins its sha256, picks the default route; default m0)")
    parser.add_argument("--sha256", help="expected sha256 of the image instead of the --mu-profile image's")
    parser.add_argument("--no-verify", action="store_true", help="skip the hash check (development only)")
    parser.add_argument("-E", "--entry-offset", type=hexint, default=None, help="entry = image base + this (default 0)")
    parser.add_argument("--route", choices=("aic", "gic"), default=None,
                        help="interrupt route: aic = native AIC passthrough (single CPU; m0, m1-aic); gic = emulated "
                             "GICv3 (m1-gic). Default: the --mu-profile's route")
    parser.add_argument("-C", "--cpus", type=int, default=None,
                        help="guest vCPU count (default 1; more than 1 needs --route gic and runs without "
                             "--fb-shadow; at most the usable cores, 18 on the J716s)")
    parser.add_argument("--counter-hz", type=int, default=None, help="counter frequency to advertise (default CNTFRQ_EL0)")
    parser.add_argument("--psci-smc", action="store_true", help="also accept PSCI over SMC (sets HCR_EL2.TSC)")
    parser.add_argument("--reserve-cpu", action="store_true", help="keep the top CPU in host context")
    parser.add_argument("--no-wfi-trap", action="store_true",
                        help="smp2d: do NOT trap/emulate the guest WFI at EL2 (reproduce native-WFI behaviour as a "
                             "control; default: trapping ON when >1 vCPU). 1-vCPU never traps WFI regardless.")
    parser.add_argument("--cpu-classes", choices=("uniform", "adt", "cppc"), default="uniform",
                        help="sched1: efficiency classes Mu publishes. uniform (default): the guest ADT gets the boot "
                             "CPU's cluster-type on every CPU, so the MADT has one class and Windows does not park the "
                             "P cores; adt: the ADT's M/P types (classes 0/1, the smpN behaviour); cppc (cpuperf-A): "
                             "the ADT's classes plus /chosen/m5w,cppc so Mu publishes a per-CPU _CPC (and _PSD) backed "
                             "by an EL2 CPPC page (needs a cpuperf-A FD and resident; no frequency changes)")
    parser.add_argument("--cppc-page", type=hexint, default=CPPC_PAGE_DEFAULT,
                        help="cpuperf-A: synthetic-window base of the 16 KiB EL2 CPPC page (--cpu-classes cppc)")
    parser.add_argument("--no-cppc-psd", action="store_true",
                        help="cpuperf-A: publish _CPC without _PSD (default: _PSD per cluster, HW_ALL)")
    parser.add_argument("--cppc-classes", choices=("adt", "uniform"), default="adt",
                        help="cpuperf: with --cpu-classes cppc, the MADT efficiency classes: adt (M = 0, P = 1, default) "
                             "or uniform (one class, as --cpu-classes uniform; the _CPC values still differ per type)")
    parser.add_argument("--cppc-autonomous", action="store_true",
                        help="cpuperf-A: _CPC Autonomous Selection Enable = 1 (variant; default 0 = OS-directed)")
    parser.add_argument("--cpu-dvfs", default="off", metavar="off|el2|fixed[:M=n,P=n]",
                        help="cpuperf-B: el2 = EL2 maps Windows' CPPC Desired Performance to the T6050 cluster "
                             "p-states (CMD +0x20 writes, the Linux driver's sequence, perf/WRITES.md); needs "
                             "--cpu-classes cppc. fixed = EL2 sets every cluster once to a fixed p-state at guest "
                             "start (default M=6,P=6), with any --cpu-classes. Default off: no DVFS register is touched")
    parser.add_argument("--dvfs-max", dest="dvfs_max_raw", action="append", default=[], metavar="TYPE=PSTATE",
                        help="cpuperf-B: highest p-state EL2 may request for M or P (default M=15, P=17); also sets "
                             "the _CPC highest/nominal")
    parser.add_argument("--dvfs-interval-us", type=int, default=1000,
                        help="cpuperf-B: minimum time between two writes to one cluster (>= 1000 us)")
    parser.add_argument("--cppc-perf", action="append", default=[], metavar="TYPE=h,n,lnl,l,lowMHz,nomMHz",
                        help="cpuperf-A: override the T6050 _CPC values for one ADT cluster-type (repeatable)")
    parser.add_argument("--synthetic-window", type=hexint, default=SYNTH_WINDOW_BASE)
    parser.add_argument("--synthetic-size", type=hexint, default=SYNTH_WINDOW_SIZE)
    parser.add_argument("--gicd", type=hexint, default=GICD_DEFAULT)
    parser.add_argument("--gicr", type=hexint, default=GICR_DEFAULT)
    parser.add_argument("--pl011", type=hexint, default=PL011_DEFAULT, help="emulated PL011 base (0 = none)")
    parser.add_argument("--pl011-gsiv", type=hexint, default=None,
                        help="PL011 interrupt SPI (route gic; published as m5w,pl011-gsiv; default: polled, no interrupt)")
    parser.add_argument("--irq-map", action="append", default=[], metavar="AIC_LINE:GSIV[,...]",
                        help="route gic: AIC line (die * max_irq + num) -> SPI 32..1019, published as m5w,irq-map and "
                             "loaded with P_HV_WIN_MAP_IRQ (repeatable; default empty: no device interrupts, M1a)")
    parser.add_argument("--irq-map-file", type=pathlib.Path, default=None,
                        help="the same pairs from a file (one or more per line, '#' comments)")
    parser.add_argument("--usb-host", type=int, default=None, metavar="N",
                        help="hand the Type-C port of /arm-io/usb-drdN to the guest as an xHCI controller (1 = "
                             "left-front on the J716s, 2 = right; never the proxy's port); docs/USB-HOST.md")
    parser.add_argument("--usb-gsiv", type=hexint, default=None,
                        help="SPI for the controller's interrupt (route gic; default 64 + N), added to m5w,irq-map")
    parser.add_argument("--usb-phy", choices=("native", "legacy"), default="native",
                        help="USB2 PHY recipe: native = Aurora's T6050 sequence with the ADT tunables (default), "
                             "legacy = m1n1's values plus the host bits, no controller tunables")
    parser.add_argument("--usb-phy-before-reset", action="store_true",
                        help="run the USB2 PHY sequence while the DWC3 is still held in reset (Linux's order)")
    parser.add_argument("--usb-lock-dart", action="store_true", help="lock the port's DART TTBR/TCR until reset")
    parser.add_argument("--usb-no-connect", action="store_true",
                        help="Mu registers the xHCI but leaves connecting it to BDS / the Shell (m5w,xhci flag bit 2)")
    parser.add_argument("--usb-dart-bypass-unsafe", action="store_true",
                        help="try DART bypass before the identity map: the controller (and so the guest) can then "
                             "DMA into m1n1 and every carveout. Debug only")
    parser.add_argument("--ramdisk", type=pathlib.Path,
                        help="appended WinPE RAM disk: the .ntasi file from tools/windows/make-ramdisk-image.sh "
                             "(required by --mu-profile m1a)")
    parser.add_argument("--ramdisk-offset", type=hexint, default=None,
                        help="RAM-disk offset from the FD base (default and only value Mu accepts: the FD header's "
                             "image_size, PcdFdSize)")
    parser.add_argument("--run-dir", type=pathlib.Path, default=None,
                        help="run directory for logs and hashes (default runs/<stamp>-<label> in the cwd)")
    parser.add_argument("--label", default=None, help="run label (default m0 / smoke / mu)")
    parser.add_argument("--fb-shadow-no-copy", action="store_true",
                        help="diagnostic: prepare BGRA shadow but do not copy it to native scanout (needs --fb-shadow)")
    parser.add_argument("--fb-shadow", action="store_true",
                        help="opt-in BGRA shadow display (single-vCPU m1a only; hardware unqualified)")
    parser.add_argument("--fb-dirty", action="store_true",
                        help="FB shadow: stage-2 write-protect dirty-page tracking; convert only changed 16 KiB pages "
                             "(needs --fb-shadow)")
    parser.add_argument("--fb-scale2", action="store_true",
                        help="FB shadow: guest GOP at half resolution (1728x1117), EL2 integer 2x upscale "
                             "(needs --fb-shadow)")
    parser.add_argument("--fb-bench", action="store_true",
                        help="FB shadow: time legacy vs wide conversion at PREPARE (stats 22..25; blanks native rows 0..7)")
    parser.add_argument("--fb-budget", default="0:0:0", metavar="BASE_US:MAX_US:DEFER_MS",
                        help="FB shadow per-tick budget (default 250 us), escalated budget after 16 ms backlog "
                             "(default 1000 us, max 4000) and blit-settle defer limit (default 8 ms); 0 = default")
    parser.add_argument("--pmu-rewrite", choices=("off", "el2"), default="off",
                        help="el2: EL2 rewrites PMUv3 accesses at the first instruction fetch of each guest RAM page "
                             "(stage-2 XN; docs/PMU-EL2-REWRITE.md)")
    parser.add_argument("--capture-vuart", action="store_true",
                        help="record the second CDC port (vuart + PL011) to <run>/vuart.log and answer win_smoke's vuart "
                             "RX self-test with one 'K'; do not combine with a terminal on that port")
    parser.add_argument("--vuart-dev", default=None, help="second CDC port (default derived from M1N1DEVICE)")
    parser.add_argument("-l", "--logfile", type=pathlib.Path, help="hv trace/TTY log (relative paths go into the run dir)")
    parser.add_argument("-S", "--shell", action="store_true", help="drop to the hypervisor shell before start")
    parser.add_argument("--after", choices=("shell", "reboot", "none"), default="shell",
                        help="what to do when the guest exits (default: hypervisor shell)")
    parser.add_argument("boot_args", default=[], nargs="*")
    args = parser.parse_args()

    if args.m0:
        if args.raw_guest or args.psci_smc or args.ramdisk or args.reserve_cpu or args.mu_profile != "m0" \
                or args.route not in (None, "aic") or args.cpus not in (None, 1) or args.entry_offset not in (None, 0) \
                or args.irq_map or args.irq_map_file or args.pmu_rewrite != "off" \
                or args.usb_host is not None:
            parser.error("--m0 is the fixed M0-RUNTIME-PLAN invocation (profile m0, aic route, 1 CPU, entry 0, "
                         "no SMC, no RAM disk, no IRQ map, no PMU rewrite, no USB host)")
        if args.logfile is None:
            args.logfile = pathlib.Path("m0.log")
    profile_route = MU_IMAGES[args.mu_profile][0]
    args.route = args.route or ("aic" if args.raw_guest else profile_route)
    if not args.raw_guest and args.route != profile_route:
        parser.error(f"Mu profile {args.mu_profile} runs on the {profile_route} route, not {args.route}"
                     + (" (m1-gic needs m5w,gic and ID_AA64PFR0_EL1.GIC = 1)" if profile_route == "gic" else
                        " (it drives the AIC itself, which the gic route keeps out of stage 2)"))
    args.cpus = args.cpus if args.cpus is not None else 1
    args.entry_offset = args.entry_offset or 0
    if args.route == "aic" and args.cpus != 1:
        parser.error("the aic route is single-CPU")
    # smpN (SMP-PLAN M5): up to MAX_CPUS (smp_asm.h) vCPUs; the vCPU table below still refuses more than the
    # usable cores (18 on the J716s: -C 6 = cluster 0, the boot cluster; -C 18 = all three clusters).
    if not 1 <= args.cpus <= MAX_VCPUS:
        parser.error(f"-C must be 1..{MAX_VCPUS} (MAX_CPUS in smp_asm.h)")
    if args.raw_guest and (args.ramdisk or args.entry_offset):
        parser.error("--raw-guest takes no RAM disk and enters at offset 0")
    if not args.raw_guest and args.mu_profile in MU_NEEDS_RAMDISK and not args.ramdisk:
        parser.error(f"--mu-profile {args.mu_profile} boots the appended WinPE RAM disk: add --ramdisk <image>.ntasi "
                     "(without it Mu powers off at J716sRamdiskDxe)")
    if args.label is None:
        args.label = "m0" if args.m0 else ("smoke" if args.raw_guest else args.mu_profile)

    specs = list(args.irq_map)
    if args.irq_map_file:
        specs.append(args.irq_map_file.read_text())
    try:
        args.irq_pairs = parse_irq_map(specs)
    except ValueError as e:
        parser.error(str(e))
    if args.irq_pairs and args.route != "gic":
        parser.error("--irq-map needs --route gic (in the aic route the guest owns the AIC)")
    if args.usb_host is not None:
        if args.usb_host < 0 or args.usb_host > 7:
            parser.error("--usb-host takes the usb-drd index 0..7")
        if args.raw_guest:
            parser.error("--usb-host needs a Mu guest (it publishes /chosen/m5w,xhci)")
        if args.usb_gsiv is None and args.route == "gic":
            args.usb_gsiv = USB_GSIV_BASE + args.usb_host
        if args.usb_gsiv is not None:
            if args.route != "gic":
                parser.error("--usb-gsiv needs --route gic (the aic route has no GIC; Mu polls the xHC anyway)")
            if not GIC_SPI_MIN <= args.usb_gsiv <= GIC_SPI_MAX:
                parser.error(f"--usb-gsiv must be an SPI {GIC_SPI_MIN}..{GIC_SPI_MAX}")
            if any(g == args.usb_gsiv for _, g in args.irq_pairs) or args.usb_gsiv == args.pl011_gsiv:
                parser.error(f"--usb-gsiv {args.usb_gsiv} is already used by --irq-map or --pl011-gsiv")
    elif args.usb_gsiv is not None or args.usb_lock_dart or args.usb_dart_bypass_unsafe or args.usb_no_connect:
        parser.error("--usb-* options need --usb-host")
    if args.usb_host is not None and args.mu_profile not in USB_PROFILES and not args.raw_guest:
        print(f"note: --usb-host with Mu profile {args.mu_profile}: EL2 prepares the port, but only "
              f"{'/'.join(USB_PROFILES)} register its xHCI (UEFI) and describe it (SSDT)", file=sys.stderr)
    if args.usb_host is None and args.mu_profile in USB_PROFILES:
        print(f"note: profile {args.mu_profile} without --usb-host: no /chosen/m5w,xhci, Mu runs without USB",
              file=sys.stderr)
    if args.pl011_gsiv is not None:
        if args.route != "gic":
            parser.error("--pl011-gsiv needs --route gic (there is no GIC to deliver it otherwise)")
        if not args.pl011:
            parser.error("--pl011-gsiv without a PL011 (--pl011 0)")
        if not GIC_SPI_MIN <= args.pl011_gsiv <= GIC_SPI_MAX:
            parser.error(f"--pl011-gsiv must be an SPI {GIC_SPI_MIN}..{GIC_SPI_MAX}")
        if any(g == args.pl011_gsiv for _, g in args.irq_pairs):
            parser.error(f"--pl011-gsiv {args.pl011_gsiv} is also in the IRQ map (Mu refuses the clash)")

    args.cppc_table = dict(CPPC_PERF_T6050)
    args.dvfs_max = {}
    for text in args.dvfs_max_raw:
        t, _, v = text.partition("=")
        try:
            n = int(v, 0)
        except ValueError:
            parser.error(f"--dvfs-max {text!r}: want TYPE=PSTATE")
        if t not in T6050_PMAX or n not in T6050_OPP_MHZ[t] or n < T6050_PMIN:
            parser.error(f"--dvfs-max {text!r}: TYPE is M or P and PSTATE an operating point of that cluster")
        args.dvfs_max[t] = n
        args.cppc_table[t] = t6050_cpc(t, n)
    try:
        args.cpu_dvfs, args.dvfs_fixed = parse_dvfs_mode(args.cpu_dvfs)
    except ValueError as e:
        parser.error(str(e))
    if args.cpu_dvfs == "el2" or args.dvfs_max:
        if args.cpu_classes != "cppc":
            parser.error("--cpu-dvfs el2 / --dvfs-max need --cpu-classes cppc")
    if args.dvfs_interval_us < 1000:
        parser.error("--dvfs-interval-us must be >= 1000")
    for text in args.cppc_perf:
        if args.cpu_classes != "cppc":
            parser.error("--cppc-perf needs --cpu-classes cppc")
        try:
            t, v = parse_cppc_perf(text)
        except ValueError as e:
            parser.error(str(e))
        args.cppc_table[t] = v
    if (args.no_cppc_psd or args.cppc_autonomous or args.cppc_classes != "adt") and args.cpu_classes != "cppc":
        parser.error("--no-cppc-psd / --cppc-autonomous / --cppc-classes need --cpu-classes cppc")
    if args.cpu_classes == "cppc":
        if args.route != "gic":
            parser.error("--cpu-classes cppc needs --route gic (Mu publishes _CPC on the GIC route only)")
        if args.raw_guest:
            parser.error("--cpu-classes cppc needs a Mu guest")
        if args.cppc_page % CPPC_PAGE_SIZE:
            parser.error("--cppc-page must be 16 KiB aligned")
    try:
        check_frames((args.synthetic_window, args.synthetic_window + args.synthetic_size), window_frames(args))
    except PlanError as e:
        parser.error(f"synthetic window: {e}")
    if args.fb_shadow_no_copy and not args.fb_shadow:
        parser.error("--fb-shadow-no-copy requires --fb-shadow")
    if (args.fb_dirty or args.fb_scale2 or args.fb_bench) and not args.fb_shadow:
        parser.error("--fb-dirty/--fb-scale2/--fb-bench require --fb-shadow")
    try:
        b0, b1, dl = (int(v) for v in args.fb_budget.split(":"))
        if not (0 <= b0 <= 0xffff and 0 <= b1 <= 4000 and 0 <= dl <= 0xff and (not b0 or not b1 or b0 <= b1)):
            raise ValueError
    except ValueError:
        parser.error("--fb-budget must be BASE_US:MAX_US:DEFER_MS with MAX_US <= 4000, DEFER_MS <= 255")
    args.fb_flags = (int(args.fb_shadow_no_copy) | (2 if args.fb_dirty else 0) | (4 if args.fb_scale2 else 0)
                     | (8 if args.fb_bench else 0))
    args.fb_tuning = b0 | (b1 << 16) | (dl << 32)
    # smp2: the shadow's dirty tracking and converter are single-vCPU designs (EL2 refuses FB PREPARE with
    # more than one vCPU too); with -C 2 the guest draws straight into the native framebuffer instead.
    if args.fb_shadow and (args.raw_guest or args.mu_profile != "m1a" or args.route != "gic" or args.cpus != 1):
        parser.error("--fb-shadow requires --mu-profile m1a --route gic -C 1 (smp2/smpN: -C 2+ runs without the shadow)")
    return parser, args


def report_pmu_rewrite(p, run_dir, record):
    """P_HV_WIN_PMU_STATS at guest exit: one RUN.txt line, the counters and the last rewrites in pmu-rewrite.txt."""
    st = {name: p.hv_win_pmu_stats(i) for i, name in enumerate(HV_WIN_PMU_STATS)}
    record(f"PMU rewrite (EL2): {st['pages']} pages tracked, {st['scans']} first-execute scans, "
           f"{st['pages_rewritten']} pages rewritten, {st['insns']} words (PMCCNTR {st['insns_pmccntr']}, "
           f"other mrs {st['insns_mrs']}, msr {st['insns_msr']}); flips write {st['write_flips']} walker "
           f"{st['walk_flips']}; pinned {st['pins']} (list full {st['pin_full']}), sweeps {st['sweeps']}; "
           f"spurious {st['spurious']}, AT failures {st['ipa_fail']}, not ours {st['not_ours']}, stuck {st['stuck']}")
    lines = [f"{k} {v}" for k, v in st.items() if k != "id"]
    first = max(0, st["insns"] - HV_WIN_PMU_LOG_SIZE)
    lines.append(f"last {st['insns'] - first} rewrite(s): seq IPA old new fetch-PC")
    for seq in range(first, st["insns"]):
        slot = seq % HV_WIN_PMU_LOG_SIZE
        ipa = p.hv_win_pmu_stats(HV_WIN_PMU_STAT_LOG_IPA + slot)
        op = p.hv_win_pmu_stats(HV_WIN_PMU_STAT_LOG_OP + slot)
        pc = p.hv_win_pmu_stats(HV_WIN_PMU_STAT_LOG_ELR + slot)
        lines.append(f"{seq} {ipa:#x} {op >> 32:08x} {op & 0xffffffff:08x} {pc:#x}")
    (run_dir / "pmu-rewrite.txt").write_text("\n".join(lines) + "\n")


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
    want = args.sha256.lower() if args.sha256 else (None if args.raw_guest else MU_IMAGES[args.mu_profile][1])
    if not args.raw_guest:
        known = [k for k, (_, h) in MU_IMAGES.items() if h == digest]
        record(f"Mu profile {args.mu_profile} (route {args.route}); image is "
               f"{'the known ' + known[0] + ' image' if known else 'not a known profile image'}")
    if not args.no_verify and want and digest != want:
        record(f"REFUSED: image hash mismatch, expected {want}"
               + (f" (this is the {known[0]} image: --mu-profile {known[0]})" if not args.raw_guest and known else ""))
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

    rd = None
    if args.ramdisk:
        rd = args.ramdisk.read_bytes()
        rd_off = args.ramdisk_offset if args.ramdisk_offset is not None else payload_size
        if rd_off < len(image):
            parser.error("ramdisk-offset overlaps the FD")
        if not args.raw_guest and rd_off != payload_size:
            parser.error(f"Mu probes the RAM disk at FD base + PcdFdSize = +{payload_size:#x}, not +{rd_off:#x}")
        try:
            rd_isize, rd_crc = check_ntasi(rd)
        except PlanError as e:
            record(f"REFUSED: --ramdisk {args.ramdisk}: {e}")
            raise SystemExit(2)
        rd_sha = hashlib.sha256(rd).hexdigest()
        record(f"RAM disk {args.ramdisk.resolve()} size {len(rd):#x} sha256 {rd_sha}: NTASI v2, image {rd_isize:#x} "
               f"bytes crc32 {rd_crc:#010x}, GPT partition 1 basic data with the NTASI GUID; appended at FD + {rd_off:#x}")
        payload_size = rd_off + len(rd)
        with open(run_dir / "SHA256SUMS", "a") as f:
            f.write(f"{rd_sha}  {args.ramdisk.resolve()}\n")

    from m1n1.proxy import M1N1Proxy, UartInterface, IODEV
    from m1n1.proxyutils import ProxyUtils, bootstrap_port
    from m1n1.utils import irange
    from m1n1.shell import run_shell
    from m1n1.sysreg import CNTFRQ_EL0, CNTPCT_EL0, HCR_EL2
    from m1n1.hv import HV
    from m1n1.hv.types import TraceMode
    from m1n1.adt import load_adt
    from m1n1.tgtypes import BootArgs_r1, BootArgs_r2, BootArgs_r3

    stop_capture = threading.Event()
    if args.capture_vuart:
        vdev = args.vuart_dev or secondary_device(os.environ.get("M1N1DEVICE"))
        if not vdev:
            parser.error("--capture-vuart needs --vuart-dev or M1N1DEVICE=/dev/cu.usbmodem<serial>1")
        threading.Thread(target=capture_serial, args=(vdev, run_dir / "vuart.log", stop_capture, record),
                         daemon=True).start()
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

    cntfrq = u.mrs(CNTFRQ_EL0)
    counter_hz = args.counter_hz if args.counter_hz is not None else cntfrq
    record(f"counter {counter_hz} Hz" + ("" if counter_hz == cntfrq else f" (CNTFRQ_EL0 reads {cntfrq})"))
    if counter_hz != cntfrq and not args.raw_guest and args.mu_profile != "m0":
        raise SystemExit(f"m5w,counter-hz {counter_hz} must equal CNTFRQ_EL0 {cntfrq} for {args.mu_profile} (MU-M1.md 8.7)")

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
    usb = None
    if args.usb_host is not None:
        try:
            usb = usb_host_plan(orig_adt, args.usb_host, usb_idx)
        except PlanError as e:
            raise SystemExit(f"--usb-host refused: {e}")
        xb, xs = usb["xhci"]
        guarded = [(xb, xs, f"usb-drd{args.usb_host} xHCI window")]
        guarded += [(b, s, "uart0") for _, b, s in node_regs(orig_adt["/arm-io/uart0"])]
        for base, size, what in usb["el2"]:
            for g0, gs, gw in guarded:
                if overlaps(align_down(base), align_up(base + size), g0, g0 + gs):
                    raise SystemExit(f"--usb-host refused: unmapping {what} would take a page of the {gw}")
            hv.add_tracer(irange(base, size), "WIN-USB-EL2", TraceMode.RESERVED)
        record(f"usb host: usb-drd{args.usb_host} xHCI {xb:#x}+{xs:#x} passthrough, AIC line {usb['line']}; "
               f"{len(usb['el2'])} MMIO ranges kept by EL2 (all DARTs, the USB/ATC blocks of every port, "
               f"{', '.join(usb['spmi']) or 'no SPMI bus'})")
        if args.route == "gic":
            if any(l == usb["line"] for l, _ in args.irq_pairs):
                raise SystemExit(f"--usb-host: AIC line {usb['line']} is already in --irq-map")
            args.irq_pairs.append((usb["line"], args.usb_gsiv))
    if args.route == "gic":
        # EL2 acknowledges and masks AIC events itself (hv_win_handle_irq); a guest access would steal them.
        # Mu's m1-gic never maps the AIC (MU-M1.md 8.13), so it stays out of stage 2 (a touch = EL2 fault).
        aic = orig_adt["/arm-io/aic"]
        for idx in range(len(aic.reg)):
            base, size = aic.get_reg(idx)
            if size:
                hv.add_tracer(irange(base, size), "WIN-AIC-EL2", TraceMode.RESERVED)
                record(f"route gic: AIC {base:#x}..{base + size:#x} owned by EL2, not mapped for the guest")

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
    if args.pmu_rewrite == "el2" and not (feats & HV_WIN_CAP_PMU_RW):
        raise SystemExit("--pmu-rewrite el2: this firmware has no stage-2 PMU rewrite (P_HV_WIN_CAPS bit 4)")

    if args.fb_shadow and not (feats & HV_WIN_CAP_FB_SHADOW):
        raise SystemExit("--fb-shadow needs P_HV_WIN_CAPS bit5")

    # CPUs the guest may bring up (contract s2/s9), fixed before CONFIGURE because EL2 builds its vCPU table
    # (vCPU i = GICR frame i = ACPI UID i) from the same list: real MPIDRs in ADT order, boot CPU first,
    # without the CPU kept for the host (EL2's rule: the highest-numbered present CPU).
    cpus = []
    for node in hv.adt["cpus"]:
        v = p.hv_win_cpu_mpidr(node.cpu_id)
        if not v:
            continue
        cpus.append((node.cpu_id, node.name, v & MPIDR_MASK, bool(v >> 63)))
    if not cpus or not any(c[3] for c in cpus):
        raise SystemExit("P_HV_WIN_CPU_MPIDR reported no boot CPU")
    host_cpu = max(c[0] for c in cpus) if args.reserve_cpu else None
    if host_cpu is not None and any(c[3] and c[0] == host_cpu for c in cpus):
        raise SystemExit("--reserve-cpu: the highest-numbered CPU is the boot CPU")
    cpus.sort(key=lambda c: not c[3])
    usable = [c for c in cpus if c[0] != host_cpu]
    for cid, name, mpidr, boot in cpus:
        print(f"  {name}: cpu-id {cid} MPIDR {mpidr:#x}{' boot' if boot else ''}"
              f"{' reserved-for-host' if cid == host_cpu else ''}")
    if len(usable) < args.cpus:
        raise SystemExit(f"only {len(usable)} usable CPUs for {args.cpus} vCPUs")
    vcpus = usable[:args.cpus]
    cpus_prop = b"".join(struct.pack("<Q", c[2]) for c in vcpus)

    irq_model = HV_WIN_IRQ_VGIC3 if args.route == "gic" else HV_WIN_IRQ_AIC_PASSTHROUGH
    gicd = args.gicd if args.route == "gic" else 0
    gicr = args.gicr if args.route == "gic" else 0
    flags = (HV_WIN_F_PSCI_SMC if args.psci_smc else 0) | (HV_WIN_F_RESERVE_CPU if args.reserve_cpu else 0) \
            | (HV_WIN_F_NO_WFI_TRAP if args.no_wfi_trap else 0)
    rc = p.hv_win_configure(irq_model, gicd, gicr, args.cpus, flags, cpus_prop)
    if rc != 0:
        raise SystemExit(f"P_HV_WIN_CONFIGURE failed ({rc}): see the m1n1 console for the reason")
    hcr = u.mrs(HCR_EL2)
    tsc = bool(hcr & HCR_TSC)
    record(f"EL2 configured: route {args.route}, {args.cpus} vCPU(s), flags {flags:#x}, HCR_EL2 {hcr:#x} "
           f"(TSC {'set' if tsc else 'clear'}{' - WRITE IGNORED, SMC will not trap' if args.psci_smc and not tsc else ''})")
    if args.route == "gic":
        need = HCR_IMO | HCR_FMO | HCR_TID3
        if (hcr & need) != need:
            raise SystemExit(f"route gic: HCR_EL2 {hcr:#x} lacks IMO/FMO/TID3 (needed for the GIC model and the "
                             "ID_AA64PFR0_EL1.GIC = 1 view Mu checks)")
        for name, base, size in window_frames(args)[:2]:
            hv.add_tracer(irange(base, size), f"WIN-{name}", TraceMode.RESERVED)
        record(f"vGIC: GICD {args.gicd:#x}, GICR {args.gicr:#x}..{args.gicr + args.cpus * GICR_STRIDE:#x} "
               f"({args.cpus} frame(s)); HCR_EL2.TID3 set: the guest reads ID_AA64PFR0_EL1.GIC = 1")
    # EL2-RUNTIME item 6: Windows touches PMUv3 registers without checking ID_AA64DFR0_EL1.PMUVer; EL2 sets
    # MDCR_EL2.TPM|TPMCR (+ the FGT PMU bits) in both routes. Whether the bits stick is recorded here, whether
    # an EL1 access then traps or UNDEFs is the smoke guest's pmu probe.
    dfr0, mmfr0, mdcr = u.mrs(ID_AA64DFR0), u.mrs(ID_AA64MMFR0), u.mrs(MDCR_EL2)
    fgt = (mmfr0 >> 56) & 0xf
    fgt_regs = f", HDFGRTR_EL2 {u.mrs(HDFGRTR_EL2):#x} HDFGWTR_EL2 {u.mrs(HDFGWTR_EL2):#x}" if fgt else ""
    record(f"PMU at EL2: ID_AA64DFR0_EL1 {dfr0:#x} (PMUVer {(dfr0 >> 8) & 0xf:#x}), FGT {fgt}, MDCR_EL2 {mdcr:#x} "
           f"(TPM {'set' if mdcr & MDCR_TPM else 'NOT SET'}, TPMCR {'set' if mdcr & MDCR_TPMCR else 'NOT SET'})"
           f"{fgt_regs}; HCR_EL2.TID3 {'set: the guest reads PMUVer 0' if hcr & HCR_TID3 else 'CLEAR'}")
    if host_cpu is not None:
        el2_host = [c[0] for c in cpus if (p.hv_win_cpu_mpidr(c[0]) >> 62) & 1]
        if el2_host != [host_cpu]:
            raise SystemExit(f"EL2 reserved CPU(s) {el2_host} for the host, the launcher assumed {host_cpu}")

    if args.pl011:
        rc = p.hv_win_pl011(args.pl011, args.pl011_gsiv or 0)
        if rc != 0:
            raise SystemExit(f"P_HV_WIN_PL011 failed ({rc})")
        hv.add_tracer(irange(args.pl011, PL011_FRAME), "WIN-PL011", TraceMode.RESERVED)
        record(f"PL011 emulated at {args.pl011:#x}, GSIV {args.pl011_gsiv or 0}")

    if args.cpu_classes == "cppc":
        if not hasattr(p, "hv_win_cppc_configure"):
            raise SystemExit("--cpu-classes cppc: this proxyclient has no P_HV_WIN_CPPC_CONFIGURE")
        rc = p.hv_win_cppc_configure(args.cppc_page, len(vcpus))
        if rc != 0:
            raise SystemExit(f"P_HV_WIN_CPPC_CONFIGURE({args.cppc_page:#x}, {len(vcpus)}) refused ({rc}): "
                             "resident without cpuperf-A, or a bad page")
        hv.add_tracer(irange(args.cppc_page, CPPC_PAGE_SIZE), "WIN-CPPC", TraceMode.RESERVED)
        record(f"cpuperf: EL2 CPPC page at {args.cppc_page:#x} for {len(vcpus)} vCPUs (desired stored only, "
               "counters = CNTPCT, no DVFS access)")

    if args.cpu_dvfs != "off":
        if not hasattr(p, "hv_win_cppc_dvfs"):
            raise SystemExit(f"--cpu-dvfs {args.cpu_dvfs}: this proxyclient has no P_HV_WIN_CPPC_DVFS")
        try:
            cfg, dlines = dvfs_plan(hv.adt, vcpus, args.dvfs_max, args.dvfs_interval_us * 1000,
                                    args.dvfs_fixed if args.cpu_dvfs == "fixed" else None)
        except PlanError as e:
            raise SystemExit(f"--cpu-dvfs {args.cpu_dvfs}: {e}")
        buf = p.malloc(len(cfg))
        iface.writemem(buf, cfg)
        rc = p.hv_win_cppc_dvfs(buf, len(cfg))  # EL2 copies it, probes CMD/STATUS read-only, then enables
        p.free(buf)
        if rc != 0:
            raise SystemExit(f"P_HV_WIN_CPPC_DVFS refused ({rc}): see the EL2 console (probe or config)")
        record(f"cpuperf-B: EL2 DVFS ON ({args.cpu_dvfs}): " + "; ".join(f"cluster {i} {b:#x} {t} p{lo}..p{hi}"
                                                                         for i, b, t, lo, hi in dlines)
               + f"; rate limit {args.dvfs_interval_us} us")

    # m5w,irq-map: one P_HV_WIN_MAP_IRQ per pair, after CONFIGURE and before start (CONTRACT s9)
    for line, gsiv in args.irq_pairs:
        rc = p.hv_win_map_irq(line, gsiv)
        if rc != 0:
            raise SystemExit(f"P_HV_WIN_MAP_IRQ({line}, {gsiv}) refused ({rc}): no such AIC line, or a duplicate")
    if args.route == "gic":
        record(f"m5w,irq-map: {', '.join(f'{l}->{g}' for l, g in args.irq_pairs) or 'empty (no device interrupts)'}")

    # --- 4. memory plan, stage 2, load -------------------------------------------------------------------
    # re-read boot_args: anything m1n1 took with top_of_memory_alloc() during hv_init shrank mem_size
    ba = iface.readstruct(u.ba_addr, {2: BootArgs_r2, 3: BootArgs_r3}.get(u.ba_rev, BootArgs_r1))
    shadow = None
    if args.fb_shadow:
        if len(vcpus) != 1 or (ba.video.depth & 0xff) != 30:
            raise SystemExit("framebuffer shadow requires one actual vCPU and native depth30")
        shadow_base = p.hv_win_fb_prepare(ba.video.base, args.fb_flags, args.fb_tuning)
        if p.hv_win_fb_stat(13) != int(args.fb_shadow_no_copy) or p.hv_win_fb_stat(14) != args.fb_flags:
            raise SystemExit("framebuffer flags mismatch (stat13/14); reload a resident with improve-20261109 FB")
        fb_guest = (p.hv_win_fb_stat(26), p.hv_win_fb_stat(27), p.hv_win_fb_stat(28))
        shadow_size = p.hv_win_fb_stat(3)
        if p.hv_win_fb_stat(0) != 1 or shadow_size != align_up(fb_guest[2] * fb_guest[1]):
            raise SystemExit("framebuffer shadow preparation failed or size mismatched; reload resident")
        shadow = validate_fb_shadow(shadow_base, shadow_size, host_base=ba.phys_base,
                                    host_limit=u.heap_base, native_base=ba.video.base,
                                    native_size=align_up(ba.video.stride * ba.video.height))
        record(f"FB heaps proxy_base {u.heap_base:#x} proxy_top {u.heap_top:#x}; C remaining before/after "
               f"{p.hv_win_fb_stat(10)}/{p.hv_win_fb_stat(11)} bytes")
        record(f"FB shadow prepared {shadow[0]:#x}..{shadow[1]:#x}, native {ba.video.base:#x}, "
               f"{ba.video.width}x{ba.video.height} stride {ba.video.stride}; no_copy={int(args.fb_shadow_no_copy)}; "
               "permanent C-heap reservation")
        record(f"FB guest mode {fb_guest[0]}x{fb_guest[1]} stride {fb_guest[2]}; flags {args.fb_flags:#x} "
               f"tuning {args.fb_tuning:#x}; bench legacy/wide/read/write ticks "
               f"{[p.hv_win_fb_stat(i) for i in range(22, 26)]} (8 rows)")
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
    try:
        check_frames((sw0, sw1), window_frames(args), plan["ranges"])
    except PlanError as e:
        raise SystemExit(f"memory plan refused: {e}")
    if shadow:
        validate_fb_shadow(shadow[0], shadow[1] - shadow[0], host_base=ba.phys_base,
                           host_limit=u.heap_base, native_base=ba.video.base,
                           native_size=plan["fb"][1] - plan["fb"][0], ranges=plan["ranges"])
        plan["holes"].append((shadow[0], shadow[1], "BGRA shadow (inside excluded C heap)"))
        check_frames((sw0, sw1), window_frames(args), [shadow])
    memory_prop = b"".join(struct.pack("<QQ", s, e - s) for s, e in plan["ranges"])

    if usb is not None:
        # after the plan (the DART maps exactly m5w,memory), before the image lands in guest RAM
        flags = ((USB_HOST_F_LEGACY_PHY if args.usb_phy == "legacy" else 0)
                 | (USB_HOST_F_PHY_BEFORE_RESET if args.usb_phy_before_reset else 0)
                 | (USB_HOST_F_LOCK_DART if args.usb_lock_dart else 0)
                 | (USB_HOST_F_DART_BYPASS if args.usb_dart_bypass_unsafe else 0))
        if args.usb_dart_bypass_unsafe:
            record("WARNING: --usb-dart-bypass-unsafe: if bypass sticks, the guest's controller can DMA anywhere")
        t0 = time.time()
        info = p.hv_win_usb_prep(args.usb_host, flags, memory_prop)
        if info < 0:
            raise SystemExit(f"P_HV_WIN_USB_PREP(usb-drd{args.usb_host}, {flags:#x}) failed ({info}): "
                             f"{USB_HOST_ERRORS.get(info, 'unknown')}; see the m1n1 console")
        dart_mode = "identity over m5w,memory" if info & 1 else ("BYPASS" if info & 2 else "?")
        hciver = (info >> 16) & 0xffff
        record(f"usb host prepared in {(time.time() - t0) * 1e3:.0f} ms: DART {dart_mode} "
               f"({(info >> 32) & 0xffff} tables), {'legacy' if info & 8 else 'native'} USB2 PHY, xHCI "
               f"{hciver >> 8:x}.{hciver & 0xff:02x} {(info >> 8) & 0xff} ports "
               f"{'64-bit DMA' if info & 4 else 'NO 64-BIT DMA (guest RAM is above 4 GiB: will fail)'}, "
               f"ports with a device {[i + 1 for i in range(8) if (info >> 48) & (1 << i)] or 'none seen'} "
               f"(info {info:#x})")
    text = format_plan(plan)
    for name, base, size in window_frames(args):
        text += f"\n{name} frame {base:#x}..{base + size:#x} (synthetic window, EL2 hook)"
    print(text)
    (run_dir / "memory-plan.txt").write_text(text + "\n")

    for s, e in plan["ranges"]:
        hv.add_tracer(irange(s, e - s), "WIN-RAM", TraceMode.OFF)
    if shadow:
        hv.add_tracer(irange(shadow[0], shadow[1] - shadow[0]), "WIN-FB-SHADOW", TraceMode.OFF)
    elif plan["fb"]:
        hv.add_tracer(irange(plan["fb"][0], plan["fb"][1] - plan["fb"][0]), "WIN-FB", TraceMode.OFF)

    rs, re_ = plan["ba_range"]
    hv.ram_base, hv.ram_size = dram_base, dram_size
    hv.phys_base, hv.mem_size = rs, re_ - rs
    hv.adt_base = plan["adt_base"]
    hv.guest_base = plan["fd_base"]
    hv.bootargs_off = plan["bootargs"] - plan["fd_base"]
    hv.entry = plan["fd_base"] + args.entry_offset
    hv._load_executable_raw(image, hv.guest_base, "smoke" if args.raw_guest else "Mu FD")
    if rd is not None:
        rd_base = hv.guest_base + rd_off
        if rd_off > len(image):
            u.compressed_writemem(hv.guest_base + len(image), b"\0" * (rd_off - len(image)))
        t_rd = time.time()
        for off in range(0, len(rd), RAMDISK_CHUNK):
            u.compressed_writemem(rd_base + off, rd[off:off + RAMDISK_CHUNK])
            if (off // RAMDISK_CHUNK) % 8 == 7:
                print(f"  RAM disk {off + RAMDISK_CHUNK >> 20}/{len(rd) >> 20} MiB, {time.time() - t_rd:.0f} s")
        p.dc_cvau(rd_base, len(rd))
        took = max(time.time() - t_rd, 1e-3)
        if iface.readmem(rd_base, 0x1000) != rd[:0x1000] or iface.readmem(rd_base + len(rd) - 0x1000, 0x1000) != rd[-0x1000:]:
            raise SystemExit("RAM disk read-back of the header page or the last page differs: upload failed")
        rd_end = rd_base + len(rd)
        record(f"RAM disk at {rd_base:#x}..{rd_end:#x} (header; image at {rd_base + NTASI_HEADER_SIZE:#x}), uploaded in "
               f"{took:.1f} s ({len(rd) / took / 1e6:.1f} MB/s), first/last page read back; ends "
               f"{(re_ - MU_TOP_WORKSPACE - rd_end) >> 20} MiB below Mu's top-128 MiB workspace "
               f"({re_ - MU_TOP_WORKSPACE:#x})")
    mm = hv.adt["chosen"]["memory-map"]
    mm.DeviceTree = (hv.adt_base, align_up(u.ba.devtree_size))
    mm.BootArgs = (plan["bootargs"], PAGE)
    if shadow:
        # HV.init shallow-copies boot_args; isolate nested metadata before editing.
        hv.tba.video = hv.tba.video.copy()
        hv.tba.video.base = shadow[0]
        hv.tba.video.depth = (ba.video.depth & ~0xff) | 32
        hv.tba.video.width, hv.tba.video.height = fb_guest[0], fb_guest[1]
        hv.tba.video.stride = fb_guest[2]
    hv._write_boot_args(plan["fd_end"])
    record(f"image at {hv.guest_base:#x} entry {hv.entry:#x}; boot_args {plan['bootargs']:#x} = "
           f"phys_base {rs:#x} mem_size {re_ - rs:#x} rev {hv.tba.revision} ver {hv.tba.version}")
    if not (1 <= hv.tba.revision <= 3 and 1 <= hv.tba.version <= 3):
        raise SystemExit("boot_args revision/version outside 1-3: Mu's SEC would not accept x0")

    # --- 5. /chosen/m5w,* ------------------------------------------------------------------------------
    def put(name, value):
        hv.adt["chosen"]._properties[name] = value

    put("m5w,memory", memory_prop)
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
            put("m5w,irq-map", b"".join(struct.pack("<II", l, g) for l, g in args.irq_pairs))

    if usb is not None:
        xb, xs = usb["xhci"]
        xflags = M5W_XHCI_F_COHERENT | M5W_XHCI_F_USB2_ONLY | (args.usb_host << 8) | \
            (M5W_XHCI_F_NO_CONNECT if args.usb_no_connect else 0)
        put("m5w,xhci", struct.pack("<QQII", xb, xs, args.usb_gsiv or 0, xflags))
        record(f"m5w,xhci: {xb:#x}+{xs:#x} gsiv {args.usb_gsiv or 0} flags {xflags:#x}")

    # the same bytes EL2 got as P_HV_WIN_CONFIGURE's sixth argument
    put("m5w,cpus", cpus_prop)
    record(f"m5w,cpus: {', '.join(hex(c[2]) for c in vcpus)}")
    if not args.raw_guest:
        if args.cpu_classes == "uniform":
            changed = uniform_cluster_types(hv.adt)
            record(f"cpu classes: uniform (guest ADT cluster-type rewritten on {len(changed)} CPU node(s): "
                   f"{', '.join(f'{n} {o}->{w}' for n, o, w in changed) or 'none'}); Mu publishes one efficiency class")
        elif args.cpu_classes == "cppc":
            try:
                cppc_prop, cppc_lines = cppc_plan(hv.adt, vcpus, args.cppc_page, not args.no_cppc_psd, args.cppc_table,
                                                 args.cppc_autonomous)
            except PlanError as e:
                raise SystemExit(f"--cpu-classes cppc: {e}")
            put("m5w,cppc", cppc_prop)
            if args.cppc_classes == "uniform":
                changed = uniform_cluster_types(hv.adt)   # after cppc_plan: the _CPC values keep the real types
                record(f"cpu classes: cppc with uniform MADT classes (guest ADT cluster-type rewritten on "
                       f"{len(changed)} CPU node(s)); Mu publishes one efficiency class")
            types = {}
            for i, name, ctype, v, dom in cppc_lines:
                types.setdefault((ctype, v), []).append(i)
            record(f"cpu classes: cppc ({'guest ADT cluster-type unchanged; Mu publishes M = 0, P = 1' if args.cppc_classes == 'adt' else 'uniform MADT classes'}) + m5w,cppc "
                   f"{len(cppc_prop)} bytes, page {args.cppc_page:#x}, _PSD {'no' if args.no_cppc_psd else 'yes'}, "
                   f"autonomous {'yes' if args.cppc_autonomous else 'no'}: "
                   + "; ".join(f"{t} CPUs {c[0]}-{c[-1]} h/n/lnl/l {v[0]}/{v[1]}/{v[2]}/{v[3]} MHz {v[4]}-{v[5]}"
                               for (t, v), c in types.items())
                   + "; domains " + ",".join(sorted({str(d) for *_, d in cppc_lines}, key=int)))
        else:
            record("cpu classes: adt (guest ADT cluster-type unchanged; Mu publishes M = 0, P = 1)")

    if args.raw_guest:
        vuart = orig_adt["/arm-io/uart0"].get_reg(0)[0]
        regs = (vuart, args.pl011 or 0, SMOKE_X3_MAGIC)
        hv_start = p.hv_start
        p.hv_start = lambda entry, x0, *rest: hv_start(entry, x0, *regs)
        record(f"raw guest registers: x0 boot_args, x1 vuart {regs[0]:#x}, x2 PL011 {regs[1]:#x}, x3 {regs[2]:#x}")

    if args.pmu_rewrite == "el2":
        # Stage 2 is only built inside HV.start() (pt_update), so arm EL2 between that and the guest's first
        # instruction: wrap P_HV_START (after the raw-guest register wrapper, so this runs first).
        start_next = p.hv_start
        ram_ranges = list(plan["ranges"])

        def start_with_pmu_rewrite(entry, x0, *rest):
            pages = 0
            for s, e in ram_ranges:
                n = p.hv_win_pmu_rewrite(s, e - s)
                if n < 0:
                    raise SystemExit(f"P_HV_WIN_PMU_REWRITE({s:#x}, {e - s:#x}) refused ({n}): see the m1n1 console")
                pages += n
            pid = p.hv_win_pmu_stats(0)
            if pid >> 32 != HV_WIN_PMU_MAGIC or not pid & 1:
                raise SystemExit(f"PMU rewrite not armed after P_HV_WIN_PMU_REWRITE (id {pid:#x})")
            tables = p.hv_win_pmu_stats(HV_WIN_PMU_STATS.index("tables"))
            record(f"PMU rewrite (EL2) armed: {len(ram_ranges)} range(s), {pages} pages of 16 KiB execute-tracked, "
                   f"{tables} L3 table(s) ({tables * 16} KiB of m1n1 heap)")
            return start_next(entry, x0, *rest)

        p.hv_start = start_with_pmu_rewrite
        record("PMU rewrite: el2 (stage-2 first-execute rewrite, armed at guest start)")

    if args.shell:
        run_shell(hv.shell_locals, "Entering hypervisor shell. Type ^D to start the guest.")

    t0 = time.time()
    cntpct = u.mrs(CNTPCT_EL0)
    t1 = time.time()
    epoch, epoch_cnt = epoch_with_counter(t0, t1, cntpct, cntfrq)
    put("m5w,epoch", struct.pack("<QQ", epoch, epoch_cnt))
    blob = hv.adt.build()
    if len(blob) > align_up(u.ba.devtree_size):
        raise SystemExit(f"guest ADT grew to {len(blob):#x} bytes, more than iBoot's {u.ba.devtree_size:#x}")
    if not args.raw_guest and len(blob) > MU_ADT_MAX:
        raise SystemExit(f"guest ADT is {len(blob):#x} bytes; Mu reserves at most {MU_ADT_MAX:#x}")
    adt_out = run_dir / "adt-guest.bin"
    adt_out.write_bytes(blob)
    os.chmod(adt_out, 0o600)
    record(f"hand-off: m5w,epoch {epoch} ({datetime.datetime.fromtimestamp(epoch, datetime.timezone.utc).isoformat()}) "
           f"at CNTPCT {epoch_cnt:#x} (read {cntpct:#x}, wall-clock window {(t1 - t0) * 1e3:.1f} ms), "
           f"guest ADT {len(blob):#x} bytes")

    # --- 6. run -------------------------------------------------------------------------------------------
    hv.start()

    if shadow:
        fb_stats = [p.hv_win_fb_stat(i) for i in range(35)]
        record(f"FB shadow final state/base/native/size/stride/height/calls/lines/max_ticks/cursor/heap_before/heap_after/heap_now/no_copy: {fb_stats[:14]}")
        record("FB shadow v2 flags/faults/passes/pages/max_lat/sum_lat/max_backlog/escalated/bench x4/"
               f"gw/gh/gs/ipa_fail/busy/start/spurious/deferred/budget: {fb_stats[14:]}")
    status = p.hv_win_status()
    stats = p.hv_win_pl011_stats() if args.pl011 else 0
    record(f"guest exit: {HV_WIN_EXIT.get(status >> 32, status >> 32)} ({status >> 32}), PSCI calls {status & 0xffffffff}, "
           f"PL011 tx {stats & 0xffffffff} rx {(stats >> 32) & 0xffff} dropped {stats >> 48}")
    if args.pmu_rewrite == "el2":
        report_pmu_rewrite(p, run_dir, record)
    if args.capture_vuart:
        time.sleep(1)  # let the capture thread drain the pipe
        try:
            tail = (run_dir / "vuart.log").read_bytes().decode("utf-8", "replace").replace("\r", "").splitlines()[-12:]
        except OSError:
            tail = []
        print("last vuart lines:" if tail else "vuart.log is empty")
        for line in tail:
            print(f"  | {line}")
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
