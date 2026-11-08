#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# run_windows.py -- load a Project Mu UEFI FD as an EL1 guest under the m1n1 hypervisor's Windows
# personality (the open EL2 runtime, agent C). Derived from tools/run_guest.py.
#
# SCAFFOLD: this wires the handoff but nothing has booted. Where agent B's Mu load requirements are not
# yet pinned (entry offset, FD size, RAM-disk probe offset, the exact memory holes), the value is a CLI
# parameter and defaulted conservatively; see m5pro-windows docs/CONTRACT.md and docs/M0-RUNTIME-PLAN.md.
#
# Pipeline:
#   1. bring up the proxy, HV.init() (stock m1n1 hypervisor bring-up)
#   2. prune ANS/NVMe and the proxy USB from the guest ADT (contract s1); EL2 keeps the DARTs
#   3. load the raw Mu FD into guest RAM (HV.load_raw); verify its sha256 first
#   4. publish the /chosen/m5w,* handoff properties
#   5. P_HV_WIN_CAPS handshake, then P_HV_WIN_CONFIGURE (interrupt route, GICD/GICR, vCPUs, PSCI conduit)
#   6. HV.start()
#
# Route R1 (gic): emulated GICv3 distributor/redistributor in EL2 -- NOT YET usable (the EL2 vGIC is a
#   scaffold; the firmware refuses it). Route aic/R2: single CPU, guest drives the physical AIC (M0).

import sys, pathlib, struct, hashlib, argparse

sys.path.append(str(pathlib.Path(__file__).resolve().parents[1]))

from m1n1.proxy import *
from m1n1.proxyutils import *
from m1n1.utils import *
from m1n1.shell import run_shell
from m1n1.sysreg import *
from m1n1.hv import HV
from m1n1.hv.types import TraceMode

# must match src/hv/win/hv_win.h
HV_WIN_MAGIC = 0x57454c32
HV_WIN_IRQ_AIC_PASSTHROUGH = 1
HV_WIN_IRQ_VGIC3 = 2
HV_WIN_F_PSCI_SMC = 1 << 0
HV_WIN_F_RESERVE_CPU = 1 << 1
HV_WIN_F_KEEP_STEAL = 1 << 2
HV_WIN_CAP_VGIC3 = 1 << 2

# Contract defaults (docs/CONTRACT.md s1)
SYNTH_WINDOW_BASE = 0xF_0000_0000
SYNTH_WINDOW_SIZE = 0x4000_0000
GICD_DEFAULT = 0xF_0000_0000
GICR_DEFAULT = 0xF_1000_0000
PL011_DEFAULT = 0xF_2000_0000


def hexint(s):
    return int(s, 0)


parser = argparse.ArgumentParser(description="Boot a Mu UEFI FD as a Windows EL2-runtime guest")
parser.add_argument("mu", type=pathlib.Path, help="raw Mu FD image")
parser.add_argument("--sha256", help="expected lowercase hex sha256 of the Mu FD (required unless --no-verify)")
parser.add_argument("--no-verify", action="store_true", help="skip the FD hash check (development only)")
parser.add_argument("-E", "--entry-offset", type=hexint, default=0x0,
                    help="guest entry = fd_base + this (Mu raw FD: 0x0)")
parser.add_argument("--route", choices=("aic", "gic"), default="aic",
                    help="interrupt route: aic = native AIC passthrough (M0, single CPU); gic = emulated GICv3")
parser.add_argument("-C", "--cpus", type=int, default=None, help="guest vCPU count (default 1 for aic)")
parser.add_argument("--counter-hz", type=int, default=None,
                    help="counter frequency to advertise (default: CNTFRQ_EL0, 1 GHz on T6050)")
parser.add_argument("--psci-smc", action="store_true", help="also accept PSCI over SMC (sets HCR_EL2.TSC)")
parser.add_argument("--reserve-cpu", action="store_true", help="keep the top CPU in host context (debug monitor)")
parser.add_argument("--synthetic-window", type=hexint, default=SYNTH_WINDOW_BASE)
parser.add_argument("--synthetic-size", type=hexint, default=SYNTH_WINDOW_SIZE)
parser.add_argument("--gicd", type=hexint, default=GICD_DEFAULT)
parser.add_argument("--gicr", type=hexint, default=GICR_DEFAULT)
parser.add_argument("--pl011", type=hexint, default=PL011_DEFAULT)
parser.add_argument("--ramdisk", type=pathlib.Path, help="optional appended WinPE RAM disk")
parser.add_argument("--ramdisk-offset", type=hexint, default=None,
                    help="RAM-disk offset from fd_base (default: directly after the FD; agent B's PcdFdSize)")
parser.add_argument("-l", "--logfile", type=pathlib.Path)
parser.add_argument("-S", "--shell", action="store_true", help="drop to the hypervisor shell before start")
parser.add_argument("boot_args", default=[], nargs="*")
args = parser.parse_args()

image = args.mu.read_bytes()
digest = hashlib.sha256(image).hexdigest()
print(f"Mu FD: {args.mu} ({len(image):#x} bytes) sha256 {digest}")
if not args.no_verify:
    if not args.sha256:
        parser.error("--sha256 is required (or pass --no-verify for development)")
    if digest != args.sha256.lower():
        parser.error(f"Mu FD hash mismatch: expected {args.sha256.lower()}")

nr_vcpus = args.cpus if args.cpus is not None else 1
if args.route == "aic" and nr_vcpus != 1:
    parser.error("the aic route is single-CPU at M0")

iface = UartInterface()
p = M1N1Proxy(iface, debug=False)
bootstrap_port(iface, p)
u = ProxyUtils(p, heap_size=128 * 1024 * 1024, m1n1_heap=128 * 1024 * 1024)

hv = HV(iface, p, u)
hv.init()

counter_hz = args.counter_hz if args.counter_hz is not None else u.mrs(CNTFRQ_EL0)
print(f"Counter frequency: {counter_hz} Hz")

# Contract s1 / BUILD.md: remove ANS/NVMe, iop-ans-nub, sart-ans and the proxy USB from the guest, and
# leave their MMIO unmapped so Mu's early DART enumeration cannot touch them. EL2 keeps the DARTs.
# ("ans" also matches iop-ans-nub and sart-ans; the proxy USB is the iodev m1n1 is tethered through.)
PRUNE = ["ans", "nvme", "sart-ans", "iop-ans", f"usb-drd{hv.iodev - IODEV.USB0}", "sep"]
pruned_mmio = []
def strip_nodes(node, path=""):
    for child in list(node):
        cpath = f"{path}/{child.name}"
        if any(pat in child.name.lower() for pat in PRUNE):
            try:
                for idx in range(len(child.reg)):
                    pruned_mmio.append(child.get_reg(idx))
            except Exception:
                pass
            print(f"Pruning ADT node {cpath}")
            del node[child.name]
        else:
            strip_nodes(child, cpath)
strip_nodes(hv.adt)
# Reserve (leave unmapped in stage-2) the pruned devices' MMIO, the way VUART/SPRR-pool regions are
# reserved: a RESERVED sub-tracer nested in the /arm-io passthrough range keeps pt_update from mapping it.
for base, size in pruned_mmio:
    hv.add_tracer(irange(base, size), "WIN-PRUNED", TraceMode.RESERVED)
    print(f"Reserving (unmapped) pruned MMIO {base:#x}..{base + size:#x}")

if args.logfile:
    hv.set_logfile(args.logfile.open("w"))
if args.boot_args:
    hv.set_bootargs(" ".join(args.boot_args))

# Compose the FD (+ optional RAM disk) and load it as a raw image into guest RAM.
payload = image
if args.ramdisk:
    rd = args.ramdisk.read_bytes()
    off = args.ramdisk_offset if args.ramdisk_offset is not None else len(payload)
    if off < len(payload):
        parser.error("ramdisk-offset overlaps the FD")
    payload = payload + b"\0" * (off - len(payload)) + rd
    print(f"Appended RAM disk at fd_base+{off:#x} ({len(rd):#x} bytes)")

hv.load_raw(payload, entryoffset=args.entry_offset)
fd_base = hv.guest_base
print(f"Mu FD base (guest IPA = PA): {fd_base:#x}, entry {hv.entry:#x}")


def put(name, value):
    hv.adt["chosen"]._properties[name] = value


def ranges_blob(ranges):
    return b"".join(struct.pack("<QQ", b, s) for b, s in ranges)


# Guest RAM visible to Mu (contract s1: minus EL2 holes). The HV already unmapped the TZ carveouts; a
# precise hole list is agent-A/C follow-up (GAPS). For the scaffold, publish the usable span the HV sized.
usable_base = hv.guest_base
usable_size = hv.mem_top - hv.guest_base
put("m5w,memory", ranges_blob([(usable_base, usable_size)]))
put("m5w,fd-base", struct.pack("<Q", fd_base))
put("m5w,fd-size", struct.pack("<Q", len(image)))
if args.ramdisk:
    put("m5w,ramdisk-base", struct.pack("<Q", fd_base + off))
    put("m5w,ramdisk-size", struct.pack("<Q", len(rd)))
put("m5w,synthetic-window", struct.pack("<QQ", args.synthetic_window, args.synthetic_size))
put("m5w,pl011", struct.pack("<Q", args.pl011))
put("m5w,counter-hz", struct.pack("<Q", counter_hz))
put("m5w,epoch", struct.pack("<Q", 0))  # TODO: real wall-clock epoch for GetTime
if args.route == "gic":
    put("m5w,gic", struct.pack("<QQ", args.gicd, args.gicr))

# CPUs the guest may bring up (contract s2/s3): MPIDRs from the ADT, minus the reserved host CPU.
mpidrs = []
for cpu in hv.adt["cpus"]:
    mpidrs.append(getattr(cpu, "cpu-id", 0))
put("m5w,cpus", b"".join(struct.pack("<Q", u.mrs(MPIDR_EL1)) for _ in range(nr_vcpus)))

# Capability handshake, then configure the EL2 personality.
caps = p.hv_win_caps()
magic = caps >> 32
abi = (caps >> 16) & 0xffff
feats = caps & 0xffff
print(f"P_HV_WIN_CAPS: magic {magic:#x} abi {abi} feats {feats:#x}")
if magic != HV_WIN_MAGIC:
    raise RuntimeError("this m1n1 build has no Windows EL2 runtime (wrong caps magic)")
if args.route == "gic" and not (feats & HV_WIN_CAP_VGIC3):
    raise RuntimeError("GICv3 emulation (route gic) is not built/ready in this firmware; use --route aic")

irq_model = HV_WIN_IRQ_VGIC3 if args.route == "gic" else HV_WIN_IRQ_AIC_PASSTHROUGH
gicd = args.gicd if args.route == "gic" else 0
gicr = args.gicr if args.route == "gic" else 0
flags = 0
if args.psci_smc:
    flags |= HV_WIN_F_PSCI_SMC
if args.reserve_cpu:
    flags |= HV_WIN_F_RESERVE_CPU

rc = p.hv_win_configure(irq_model, gicd, gicr, nr_vcpus, flags)
if rc != 0:
    raise RuntimeError(f"P_HV_WIN_CONFIGURE failed ({rc})")
print(f"EL2 Windows runtime configured: route {args.route}, {nr_vcpus} vCPU(s), flags {flags:#x}")

if args.shell:
    run_shell(hv.shell_locals, "Entering hypervisor shell. Type ^D to start the guest.")

hv.start()

run_shell(hv.shell_locals, "Hypervisor exited. Entering shell.")
p.smp_stop_secondaries(True)
p.sleep(True)
