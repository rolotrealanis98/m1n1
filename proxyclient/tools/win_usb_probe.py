#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# win_usb_probe.py -- rung U0 of m5pro-windows docs/USB-HOST.md: prepare a Type-C port with
# P_HV_WIN_USB_PREP exactly as run_windows.py --usb-host does, then drive its xHC from the Studio over
# the proxy (polled, no interrupts, no guest): enable slot, address the device on the first connected
# port, read its device and configuration descriptors. All xHCI structures live in one 16 KiB-aligned
# buffer from the proxy heap, and that buffer is the only range the port's DART maps (DVA == PA), so a
# descriptor that arrives proves PHY, DWC3, xHC and the identity map together. --dart-negative points
# one transfer outside the range and expects the DART to refuse it (ERROR register set, the xHC reports
# a transaction or data-buffer error, nothing written).
#
#   M1N1DEVICE=/dev/cu.usbmodem<serial>1 python3 tools/win_usb_probe.py --port 1 [--phy legacy]
#       [--phy-before-reset] [--late-port-reset] [--dart-negative]
#
# The xHCI driver part (class XhciProbe) only needs read32/write32/readmem/writemem on physical
# addresses, so the same code runs against QEMU's qemu-xhci through the gdbstub in the offline test
# (~/Work/agents/win-el2/usb/qemu/xhci_gdb_test.py).

import argparse
import struct
import sys
import time

# src/usb.h
USB_HOST_F_LEGACY_PHY = 1 << 1
USB_HOST_F_PHY_BEFORE_RESET = 1 << 5

TRB_NORMAL, TRB_SETUP, TRB_DATA, TRB_STATUS, TRB_LINK = 1, 2, 3, 4, 6
TRB_ENABLE_SLOT, TRB_ADDRESS_DEV, TRB_NOOP_CMD = 9, 11, 23
EV_TRANSFER, EV_CMD_COMPLETION, EV_PORT_STATUS = 32, 33, 34
CC = {1: "Success", 3: "Data Buffer Error", 4: "Babble", 5: "USB Transaction Error", 6: "TRB Error",
      13: "Short Packet", 17: "Parameter Error", 19: "Context State Error"}
SPEED = {1: "full", 2: "low", 3: "high", 4: "super", 5: "super+"}
PORTSC_RW1C = (1 << 17) | (1 << 18) | (1 << 19) | (1 << 20) | (1 << 21) | (1 << 22) | (1 << 23) | (1 << 1)
PORTSC_PP, PORTSC_PR, PORTSC_CCS, PORTSC_PED = 1 << 9, 1 << 4, 1 << 0, 1 << 1


class ProbeError(Exception):
    pass


class Arena:
    """Bump allocator inside one DMA buffer (the only range the DART maps)."""

    def __init__(self, base, size):
        self.base, self.size, self.off = base, size, 0

    def alloc(self, size, align=64):
        self.off = (self.off + align - 1) & ~(align - 1)
        if self.off + size > self.size:
            raise ProbeError("DMA arena exhausted")
        a = self.base + self.off
        self.off += size
        return a


class Ring:
    def __init__(self, mem, arena, ntrbs=32):
        self.mem, self.n = mem, ntrbs
        self.base = arena.alloc(16 * ntrbs, 64)
        mem.writemem(self.base, bytes(16 * ntrbs))
        self.idx, self.cycle = 0, 1
        # link TRB back to the start, toggle cycle
        self._write(ntrbs - 1, self.base, 0, (TRB_LINK << 10) | (1 << 1))

    def _write(self, i, param, status, control):
        a = self.base + 16 * i
        self.mem.writemem(a, struct.pack("<QII", param, status, control & ~1))
        self.mem.write32(a + 12, (control & ~1) | self.cycle)  # cycle bit last

    def push(self, param, status, control):
        a = self.base + 16 * self.idx
        self._write(self.idx, param, status, control)
        self.idx += 1
        if self.idx == self.n - 1:
            self._write(self.n - 1, self.base, 0, (TRB_LINK << 10) | (1 << 1))
            self.idx, self.cycle = 0, self.cycle ^ 1
        return a


class XhciProbe:
    def __init__(self, mem, base, arena, log=print):
        self.m, self.base, self.arena, self.log = mem, base, arena, log
        cap = mem.read32(base)
        self.caplen, self.hciver = cap & 0xff, cap >> 16
        self.hcs1, self.hcs2, self.hcc1 = mem.read32(base + 4), mem.read32(base + 8), mem.read32(base + 0x10)
        self.dboff, self.rtsoff = mem.read32(base + 0x14) & ~3, mem.read32(base + 0x18) & ~0x1f
        self.op = base + self.caplen
        self.max_slots, self.max_ports = self.hcs1 & 0xff, self.hcs1 >> 24
        self.ctx = 64 if self.hcc1 & (1 << 2) else 32
        self.ac64 = bool(self.hcc1 & 1)
        if self.caplen < 0x20 or self.hciver < 0x100 or not self.max_ports:
            raise ProbeError(f"no xHC at {base:#x} (CAPLENGTH/HCIVERSION {cap:#x})")
        self.log(f"xHCI {self.hciver >> 8:x}.{self.hciver & 0xff:02x}: {self.max_slots} slots, {self.max_ports} "
                 f"ports, {self.ctx}-byte contexts, {'64' if self.ac64 else '32'}-bit DMA, DBOFF {self.dboff:#x} "
                 f"RTSOFF {self.rtsoff:#x}")
        self.protocols = self._protocols()

    def _protocols(self):
        """Supported Protocol capabilities: port number -> (major, minor)."""
        out = {}
        xecp = (self.hcc1 >> 16) << 2
        a = self.base + xecp
        for _ in range(64):
            if not xecp:
                break
            v = self.m.read32(a)
            if v & 0xff == 2:
                major, minor = v >> 24, (v >> 16) & 0xff
                w2 = self.m.read32(a + 8)
                first, count = w2 & 0xff, (w2 >> 8) & 0xff
                for p in range(first, first + count):
                    out[p] = (major, minor)
            nxt = (v >> 8) & 0xff
            if not nxt:
                break
            a += nxt << 2
        return out

    def portsc(self, port):
        return self.op + 0x400 + 0x10 * (port - 1)

    def wait(self, what, cond, timeout=1.0):
        t = time.time()
        while not cond():
            if time.time() - t > timeout:
                raise ProbeError(f"timeout: {what}")
            time.sleep(0.001)

    def reset(self):
        self.m.write32(self.op, self.m.read32(self.op) & ~1)
        self.wait("HCHalted", lambda: self.m.read32(self.op + 4) & 1)
        self.m.write32(self.op, 1 << 1)
        self.wait("HCRST/CNR", lambda: not (self.m.read32(self.op) & 2) and not (self.m.read32(self.op + 4) & (1 << 11)))

    def start(self):
        m, a = self.m, self.arena
        self.reset()
        slots = min(self.max_slots, 8)
        m.write32(self.op + 0x38, slots)                                   # CONFIG.MaxSlotsEn
        self.dcbaa = a.alloc(8 * 256, 64)
        m.writemem(self.dcbaa, bytes(8 * 256))
        nsp = ((self.hcs2 >> 27) & 0x1f) | (((self.hcs2 >> 21) & 0x1f) << 5)
        if nsp:
            arr = a.alloc(8 * nsp, 64)
            pages = [a.alloc(4096, 4096) for _ in range(nsp)]
            m.writemem(arr, b"".join(struct.pack("<Q", p) for p in pages))
            m.writemem(self.dcbaa, struct.pack("<Q", arr))
        self.log(f"{nsp} scratchpad buffer(s)")
        self._w64(self.op + 0x30, self.dcbaa)                               # DCBAAP
        self.cmd = Ring(m, a)
        self._w64(self.op + 0x18, self.cmd.base | 1)                        # CRCR, RCS = 1
        self.ev_base, self.ev_n = a.alloc(16 * 64, 64), 64
        m.writemem(self.ev_base, bytes(16 * 64))
        erst = a.alloc(16, 64)
        m.writemem(erst, struct.pack("<QII", self.ev_base, self.ev_n, 0))
        ir0 = self.base + self.rtsoff + 0x20
        m.write32(ir0 + 0x08, 1)                                            # ERSTSZ
        self._w64(ir0 + 0x18, self.ev_base)                                 # ERDP
        self._w64(ir0 + 0x10, erst)                                         # ERSTBA last
        self.ev_idx, self.ev_cycle, self.ir0 = 0, 1, ir0
        m.write32(self.op, m.read32(self.op) | 1)                           # RS, no INTE
        self.wait("run", lambda: not (m.read32(self.op + 4) & 1))
        self.log(f"running: DCBAA {self.dcbaa:#x}, command ring {self.cmd.base:#x}, event ring {self.ev_base:#x}")

    def _w64(self, addr, val):
        self.m.write32(addr, val & 0xffffffff)
        self.m.write32(addr + 4, val >> 32)

    def next_event(self, timeout=1.0):
        t = time.time()
        while True:
            a = self.ev_base + 16 * self.ev_idx
            param, status, control = struct.unpack("<QII", self.m.readmem(a, 16))
            if (control & 1) == self.ev_cycle:
                self.ev_idx += 1
                if self.ev_idx == self.ev_n:
                    self.ev_idx, self.ev_cycle = 0, self.ev_cycle ^ 1
                self._w64(self.ir0 + 0x18, (self.ev_base + 16 * self.ev_idx) | (1 << 3))
                return (control >> 10) & 0x3f, param, status, control
            if time.time() - t > timeout:
                raise ProbeError("timeout waiting for an event")
            time.sleep(0.001)

    def wait_event(self, kind, timeout=2.0):
        while True:
            t, param, status, control = self.next_event(timeout)
            if t == kind:
                return param, status, control
            if t == EV_PORT_STATUS:
                self.log(f"  port status change: port {param >> 24}")

    def command(self, param, status, control):
        self.cmd.push(param, status, control)
        self.m.write32(self.base + self.dboff, 0)
        p, s, c = self.wait_event(EV_CMD_COMPLETION)
        return s >> 24, c >> 24

    def power_ports(self):
        for port in range(1, self.max_ports + 1):
            sc = self.m.read32(self.portsc(port))
            if not sc & PORTSC_PP:
                self.m.write32(self.portsc(port), (sc & ~PORTSC_RW1C & ~PORTSC_PED) | PORTSC_PP)

    def scan(self, settle=0.5):
        self.power_ports()
        time.sleep(settle)
        found = []
        for port in range(1, self.max_ports + 1):
            sc = self.m.read32(self.portsc(port))
            proto = self.protocols.get(port, (0, 0))
            self.log(f"port {port} (USB {proto[0]}.{proto[1]:x}): PORTSC {sc:#010x} "
                     f"{'connected' if sc & PORTSC_CCS else 'empty'}")
            if sc & PORTSC_CCS:
                found.append(port)
        return found

    def reset_port(self, port):
        sc = self.m.read32(self.portsc(port))
        self.m.write32(self.portsc(port), (sc & ~PORTSC_RW1C & ~PORTSC_PED) | PORTSC_PR)
        self.wait("port reset", lambda: self.m.read32(self.portsc(port)) & (1 << 21), 1.0)
        sc = self.m.read32(self.portsc(port))
        self.m.write32(self.portsc(port), (sc & ~PORTSC_RW1C & ~PORTSC_PED) | (1 << 21) | (1 << 17))
        speed = (sc >> 10) & 0xf
        self.log(f"port {port} reset: PORTSC {sc:#010x}, {SPEED.get(speed, speed)} speed, "
                 f"{'enabled' if sc & PORTSC_PED else 'NOT enabled'}")
        return speed

    def address(self, port, speed):
        cc, slot = self.command(0, 0, TRB_ENABLE_SLOT << 10)
        if cc != 1:
            raise ProbeError(f"Enable Slot: {CC.get(cc, cc)}")
        out_ctx = self.arena.alloc(32 * self.ctx, 64)
        self.m.writemem(out_ctx, bytes(32 * self.ctx))
        self.m.writemem(self.dcbaa + 8 * slot, struct.pack("<Q", out_ctx))
        self.ep0 = Ring(self.m, self.arena)
        mps = {3: 64, 4: 512, 5: 512}.get(speed, 8)
        inp = self.arena.alloc(33 * self.ctx, 64)
        ictx = bytearray(33 * self.ctx)
        struct.pack_into("<II", ictx, 0, 0, 0x3)                            # add A0 | A1
        s = self.ctx
        struct.pack_into("<IIII", ictx, s, (1 << 27) | (speed << 20), port << 16, 0, 0)
        e = 2 * self.ctx
        struct.pack_into("<IIQI", ictx, e, 0, (3 << 1) | (4 << 3) | (mps << 16), self.ep0.base | 1, 8)
        self.m.writemem(inp, bytes(ictx))
        cc, _ = self.command(inp, 0, (TRB_ADDRESS_DEV << 10) | (slot << 24))
        if cc != 1:
            raise ProbeError(f"Address Device: {CC.get(cc, cc)}")
        self.slot = slot
        st = self.m.read32(out_ctx + 12) >> 27
        self.log(f"slot {slot} addressed (slot state {st}, USB address {self.m.read32(out_ctx + 12) & 0xff})")
        return slot

    def control_in(self, req, value, index, length, buf=None):
        if buf is None:
            buf = self.arena.alloc(max(length, 64), 64)
            self.m.writemem(buf, bytes(max(length, 64)))
        setup = 0x80 | (req << 8) | (value << 16) | (index << 32) | (length << 48)
        self.ep0.push(setup, 8, (TRB_SETUP << 10) | (1 << 6) | (3 << 16))   # IDT, TRT = IN
        self.ep0.push(buf, length, (TRB_DATA << 10) | (1 << 16))            # DIR = IN
        self.ep0.push(0, 0, (TRB_STATUS << 10) | (1 << 5))                  # OUT status, IOC
        self.m.write32(self.base + self.dboff + 4 * self.slot, 1)
        p, s, c = self.wait_event(EV_TRANSFER)
        cc = s >> 24
        return cc, buf, self.m.readmem(buf, length)

    def halt(self):
        self.m.write32(self.op, self.m.read32(self.op) & ~1)
        try:
            self.wait("halt", lambda: self.m.read32(self.op + 4) & 1)
        finally:
            self.reset()


def enumerate_first(x, log, late_reset=None):
    x.start()
    ports = x.scan()
    if late_reset:
        late_reset()
        ports = x.scan(0.3)
    if not ports:
        raise ProbeError("no device on any port (plugged in before the probe? PD in S0? VBUS?)")
    port = ports[0]
    speed = x.reset_port(port)
    x.address(port, speed)
    cc, buf, d = x.control_in(6, 0x0100, 0, 18)
    if cc not in (1, 13):
        raise ProbeError(f"GET_DESCRIPTOR(device): {CC.get(cc, cc)}")
    bl, bt, bcd, cls, sub, proto, mps0, vid, pid, rel = struct.unpack("<BBHBBBBHHH", d[:14])
    log(f"DEVICE DESCRIPTOR: bLength {bl} type {bt} USB {bcd >> 8:x}.{bcd & 0xff:02x} class {cls:#x} "
        f"VID:PID {vid:04x}:{pid:04x} bcdDevice {rel:#x} configs {d[17]}")
    if bl != 18 or bt != 1:
        raise ProbeError("descriptor content is wrong (DMA landed elsewhere?)")
    cc, _, c = x.control_in(6, 0x0200, 0, 9)
    total = struct.unpack("<H", c[2:4])[0]
    cc, _, c = x.control_in(6, 0x0200, 0, min(total, 255))
    i = 0
    while i + 2 <= len(c) and c[i]:
        if c[i + 1] == 4 and i + 9 <= len(c):
            log(f"  interface {c[i + 2]}: class {c[i + 5]:#x} subclass {c[i + 6]:#x} protocol {c[i + 7]:#x}"
                + ("  (mass storage, bulk-only)" if c[i + 5] == 8 and c[i + 7] == 0x50 else "")
                + ("  (mass storage, UAS)" if c[i + 5] == 8 and c[i + 7] == 0x62 else "")
                + ("  (HID)" if c[i + 5] == 3 else ""))
        i += c[i]
    return port, vid, pid


def main():
    ap = argparse.ArgumentParser(description="U0: EL2 USB host preparation + a polled xHCI enumeration over the proxy")
    ap.add_argument("--port", type=int, default=1, help="usb-drdN to prepare (default 1, left-front on the J716s)")
    ap.add_argument("--phy", choices=("native", "legacy"), default="native")
    ap.add_argument("--phy-before-reset", action="store_true")
    ap.add_argument("--late-port-reset", action="store_true",
                    help="pulse the USB2 PHY port reset (atc-phyN reg[0] + 4 bit 1, 5 ms) after the xHC runs, as "
                         "Linux's T6050 driver and Aurora's J714 Mu do, and scan again")
    ap.add_argument("--dart-negative", action="store_true",
                    help="after the descriptor, read it again into a buffer outside the mapped range: the DART "
                         "must refuse (ERROR set, no data)")
    ap.add_argument("--no-prep", action="store_true", help="skip P_HV_WIN_USB_PREP (already prepared)")
    args = ap.parse_args()

    sys.path.append(str(__import__("pathlib").Path(__file__).resolve().parents[1]))
    from m1n1.setup import p, u, iface  # noqa: E402  (connects to M1N1DEVICE)
    from m1n1.adt import load_adt
    from m1n1.proxy import IODEV

    adt = load_adt(u.get_adt())
    if p.iodev_whoami() == IODEV.USB0 + args.port:
        raise SystemExit("that port carries the proxy")
    drd = adt[f"/arm-io/usb-drd{args.port}"]
    xbase, xsize = drd.get_reg(0)
    dart = adt[f"/arm-io/dart-usb{args.port}"]
    darts = [dart.get_reg(0)[0], dart.get_reg(1)[0]]
    usb2phy = adt[f"/arm-io/atc-phy{args.port}"].get_reg(0)[0]

    size = 1 << 20
    dma = u.memalign(0x4000, size)
    print(f"DMA arena {dma:#x}+{size:#x} (the only range the DART will map)")
    if not args.no_prep:
        flags = (USB_HOST_F_LEGACY_PHY if args.phy == "legacy" else 0) | \
                (USB_HOST_F_PHY_BEFORE_RESET if args.phy_before_reset else 0)
        info = p.hv_win_usb_prep(args.port, flags, struct.pack("<QQ", dma, size))
        print(f"P_HV_WIN_USB_PREP -> {info:#x}" if info >= 0 else f"P_HV_WIN_USB_PREP FAILED {info}")
        if info < 0:
            raise SystemExit(1)

    class ProxyMem:
        read32 = staticmethod(p.read32)
        write32 = staticmethod(p.write32)
        readmem = staticmethod(iface.readmem)
        writemem = staticmethod(iface.writemem)

    def late_reset():
        p.set32(usb2phy + 4, 2)
        time.sleep(0.005)
        p.clear32(usb2phy + 4, 2)
        print(f"late USB2 port reset done, PHY CTL {p.read32(usb2phy + 4):#x}")

    x = XhciProbe(ProxyMem, xbase, Arena(dma, size))
    err0 = [p.read32(d + 0x100) for d in darts]
    try:
        port, vid, pid = enumerate_first(x, print, late_reset if args.late_port_reset else None)
        if args.dart_negative:
            outside = u.memalign(0x4000, 0x4000)
            iface.writemem(outside, b"\x5a" * 64)
            try:
                cc, _, d = x.control_in(6, 0x0100, 0, 18, buf=outside)
                outcome = CC.get(cc, cc)
            except ProbeError as e:
                outcome = f"no completion ({e}), USBSTS {p.read32(x.op + 4):#x} (bit 2 = host system error)"
            d = iface.readmem(outside, 18)
            errs = [p.read32(d_ + 0x100) for d_ in darts]
            untouched = d == bytes([0x5a]) * 18
            print(f"negative: transfer to {outside:#x} (unmapped) -> {outcome}, buffer "
                  f"{'untouched' if untouched else 'WRITTEN'}, DART ERROR {[hex(e) for e in errs]}")
            if not untouched or not any(errs):
                raise SystemExit("U0 FAIL: the DART let a transfer outside the mapped range through")
    finally:
        x.halt()
    errs = [p.read32(d + 0x100) for d in darts]
    print(f"DART ERROR before {[hex(e) for e in err0]} after {[hex(e) for e in errs]}")
    print(f"U0 PASS: usb-drd{args.port} port {port} device {vid:04x}:{pid:04x} through the identity-mapped DART")


if __name__ == "__main__":
    main()
