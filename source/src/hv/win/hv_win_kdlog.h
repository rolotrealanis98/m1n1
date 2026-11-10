/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * smp2g: passive recogniser for the two kernel-debugger packets that bracket a break, in the byte
 * stream of the emulated PL011 (hv_win_pl011.c): the target's state change (packet type 7) and the
 * host's continue (type 2, DbgKdContinueApi / DbgKdContinueApi2). Diagnostic only: it never changes,
 * delays or drops a byte, and a byte sequence it does not recognise is ignored. Pure logic, also built
 * by tests/win-wfi.
 * Wire format (KD serial): leader "0000" (0x30303030), u16 type, u16 byte count, u32 id, u32 checksum,
 * then the payload. Type 7 payload: u32 NewState (0x3030 exception, 0x3031 load symbols, 0x3032
 * command string), u16 level, u16 processor, u32 number of processors, u64 thread @16, u64 PC @24.
 * Type 2 payload: u32 ApiNumber (0x3136, 0x313c), u16 level, u16 processor, u32 status, u32 continue
 * status @16.
 */
#ifndef HV_WIN_KDLOG_H
#define HV_WIN_KDLOG_H

#define KDLOG_HDR  12
#define KDLOG_DATA 32

struct kdlog_parser {
    u32 win;  /* last four bytes while hunting for the leader */
    u8 in;    /* inside a packet header/payload prefix */
    u8 n, need;
    u8 buf[KDLOG_HDR + KDLOG_DATA];
};

struct kdlog_event {
    u64 t;     /* CNTPCT (filled in by the caller) */
    u64 pc;    /* state change: program counter; continue: continue status */
    u32 code;  /* NewState or ApiNumber */
    u16 proc;  /* the processor field of the packet */
    u8 type;   /* 7 state change, 2 continue; 0 = empty slot */
    u8 cpu;    /* the CPU that wrote/read the byte completing it (filled in by the caller) */
};

static inline u32 kdlog_le(const u8 *p, int n)
{
    u32 v = 0;
    for (int i = n - 1; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

/* Feed one byte; true when it completes a state change or a continue (described in *ev). */
static inline bool kdlog_feed(struct kdlog_parser *p, u8 b, struct kdlog_event *ev)
{
    if (!p->in) {
        p->win = (p->win >> 8) | ((u32)b << 24);
        if (p->win == 0x30303030) {
            p->in = 1;
            p->n = 0;
            p->need = KDLOG_HDR;
            p->win = 0;
        }
        return false;
    }
    if (p->n >= sizeof(p->buf)) { /* cannot happen; never write past the buffer */
        p->in = 0;
        return false;
    }
    p->buf[p->n++] = b;
    if (p->n < p->need)
        return false;
    if (p->need == KDLOG_HDR) {
        u32 type = kdlog_le(p->buf, 2), count = kdlog_le(p->buf + 2, 2);
        if ((type != 7 && type != 2) || count < 20) {
            p->in = 0;
            return false;
        }
        p->need = KDLOG_HDR + (count < KDLOG_DATA ? count : KDLOG_DATA);
        return false;
    }
    const u8 *d = p->buf + KDLOG_HDR;
    u32 type = kdlog_le(p->buf, 2), code = kdlog_le(d, 4);
    u32 dlen = p->need - KDLOG_HDR;
    p->in = 0;
    if (type == 7 && code >= 0x3030 && code <= 0x3032) {
        ev->pc = dlen >= 32 ? ((u64)kdlog_le(d + 28, 4) << 32) | kdlog_le(d + 24, 4) : 0;
    } else if (type == 2 && (code == 0x3136 || code == 0x313c)) {
        ev->pc = kdlog_le(d + 16, 4);
    } else {
        return false;
    }
    ev->type = type;
    ev->code = code;
    ev->proc = kdlog_le(d + 6, 2);
    return true;
}

#endif
