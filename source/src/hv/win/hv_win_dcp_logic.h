/* SPDX-License-Identifier: MIT */
/*
 * dcp-pt: pure decision logic of the display-coprocessor passthrough guards (hv_win_dcp.c), shared with the host
 * test (tests/win-dcp). No hardware access here.
 *
 * The Windows guest owns the DCP (display/dcpn-20261111/DESIGN.md decisions 3-5). EL2 never talks to the DCP. It
 * maps hardware and guards three things:
 *   (a) dart-dcp and dart-disp0 MMIO: reads pass; writes only to the TLB flush registers for the stream the
 *       Windows driver owns on that DART, and to the error-status clear registers;
 *   (b) the root (TTBR) table page of SIDs 23 (dart-dcp), 16 and 0 (dart-disp0): writes only to entries 16..31 that
 *       were zero at handoff (the slots the owner's Linux and our engine claim), with a table-entry shaped value
 *       or zero;
 *   (c) the "display owner" register page (a 16 KiB EL2 frame in the synthetic window).
 *
 * T8110 DART register layout: owner's Linux drivers/iommu/apple-dart.c (DART_T8110_*) and
 * display/dcp-20261111/service/dart4.h.
 */
#ifndef HV_WIN_DCP_LOGIC_H
#define HV_WIN_DCP_LOGIC_H

/* ---- DART MMIO ---- */
#define DCP_DART_DCP  0 /* dart-dcp   0x48a340000: SID 23 */
#define DCP_DART_DISP 1 /* dart-disp0 0x48a300000: SIDs 0 and 16 */

#define DCP_SID_DCP    23u
#define DCP_SID_PIODMA 16u
#define DCP_SID_DISP   0u

#define DCP_DART_TLB_CMD       0x80u
#define DCP_DART_TLB_START     0x98u /* + 4: high word */
#define DCP_DART_TLB_END       0xa0u /* + 4: high word */
#define DCP_DART_ERROR         0x100u
#define DCP_DART_ERROR_STREAMS 0x1c0u
#define DCP_DART_BLOCK_ERROR   0x704u
#define DCP_DART_ERR_SID(s)    (0x8000u + 0x40u * (s)) /* status word at +0 */

#define DCP_TLB_STREAM_MASK 0xffu
#define DCP_TLB_OP_SHIFT    8
#define DCP_TLB_OP_MASK     7u
#define DCP_TLB_OP_FLUSH_SID 1u
#define DCP_TLB_STT         (1u << 13)
#define DCP_TLB_RANGE       (1u << 14)
#define DCP_TLB_V2          (1u << 15)
/* every bit a flush-by-SID command may carry; BUSY (31) and the reserved bits 12:11, 30:16 are not */
#define DCP_TLB_ALLOWED     (DCP_TLB_STREAM_MASK | (DCP_TLB_OP_MASK << DCP_TLB_OP_SHIFT) | DCP_TLB_STT | \
                             DCP_TLB_RANGE | DCP_TLB_V2)
#define DCP_BLOCK_ERROR_MASK 0x777u

enum dcp_act { DCP_DROP = 0, DCP_PASS = 1 };

struct dcp_write {
    enum dcp_act act;
    unsigned int val; /* the value to store when act == DCP_PASS */
};

/* streams the Windows driver may flush / clear on this DART */
static inline int dcp_sid_allowed(int dart, unsigned int sid)
{
    if (dart == DCP_DART_DCP)
        return sid == DCP_SID_DCP;
    if (dart == DCP_DART_DISP)
        return sid == DCP_SID_DISP || sid == DCP_SID_PIODMA;
    return 0;
}

/* bit mask over SIDs 0..31 of the streams allowed on `dart` (ERROR_STREAMS word) */
static inline unsigned int dcp_sid_mask(int dart)
{
    unsigned int m = 0;
    for (unsigned int s = 0; s < 32; s++)
        if (dcp_sid_allowed(dart, s))
            m |= 1u << s;
    return m;
}

/*
 * A guest write of `val` (width 1 << width bytes) at byte offset `off` of the DART's register window. Only 32-bit
 * aligned writes are ever legitimate (Linux and our engine use writel); every other width is dropped.
 */
static inline struct dcp_write dcp_dart_write(int dart, unsigned long off, int width, unsigned long long val)
{
    struct dcp_write w = {DCP_DROP, 0};
    unsigned int v = (unsigned int)val;

    if (width != 2 || (off & 3) || (dart != DCP_DART_DCP && dart != DCP_DART_DISP))
        return w;
    if (off == DCP_DART_TLB_CMD) {
        /* flush by SID only (op 1), no flush-all (op 0), no unlock (op 4), no BUSY or reserved bits */
        if ((v & ~DCP_TLB_ALLOWED) || ((v >> DCP_TLB_OP_SHIFT) & DCP_TLB_OP_MASK) != DCP_TLB_OP_FLUSH_SID ||
            !dcp_sid_allowed(dart, v & DCP_TLB_STREAM_MASK))
            return w;
        w.act = DCP_PASS;
        w.val = v;
        return w;
    }
    if (off == DCP_DART_TLB_START || off == DCP_DART_TLB_START + 4 || off == DCP_DART_TLB_END ||
        off == DCP_DART_TLB_END + 4) {
        /* the DVA range of the next range flush; it changes nothing until a (guarded) command uses it */
        w.act = DCP_PASS;
        w.val = v;
        return w;
    }
    if (off == DCP_DART_ERROR) {
        /* error status, write-to-clear (apple_dart_t8110_irq) */
        w.act = DCP_PASS;
        w.val = v;
        return w;
    }
    if (off == DCP_DART_ERROR_STREAMS) {
        /* per-stream error latches, write-1-to-clear: only the bits of our own streams */
        v &= dcp_sid_mask(dart);
        if (v) {
            w.act = DCP_PASS;
            w.val = v;
        }
        return w;
    }
    if (off == DCP_DART_BLOCK_ERROR) {
        v &= DCP_BLOCK_ERROR_MASK;
        if (v) {
            w.act = DCP_PASS;
            w.val = v;
        }
        return w;
    }
    if (off >= DCP_DART_ERR_SID(0) && off < DCP_DART_ERR_SID(32) && ((off - DCP_DART_ERR_SID(0)) % 0x40u) == 0) {
        /* per-SID exception status, clear */
        unsigned int sid = (unsigned int)((off - DCP_DART_ERR_SID(0)) / 0x40u);
        if (dcp_sid_allowed(dart, sid)) {
            w.act = DCP_PASS;
            w.val = v;
        }
        return w;
    }
    return w; /* TTBR, TCR, PROTECT, UNPROTECT, PROTECT_LOCK, ERROR_MASK, ENABLE/DISABLE_STREAMS, other SIDs, ... */
}

/* ---- root (TTBR) table pages ---- */
#define DCP_ROOT_PAGE        0x4000ul
#define DCP_ROOT_ENTRIES     2048u
#define DCP_ROOT_WIN_FIRST   16u /* dart4's window: DVA 0x10000000000.., root slots 16..31 */
#define DCP_ROOT_WIN_COUNT   16u
#define DCP_ROOT_BITMAP_BYTES (DCP_ROOT_ENTRIES / 8)
#define DCP_PTE_VALID        1ull
#define DCP_PTE_PA_MASK      0x0000003FFFFFFC00ull /* bits 37:10 (dart4.h DART4_PA_MASK) */

/* dart4 tbl_ok(): valid, nothing but the PA field and the valid bit */
static inline int dcp_root_value_ok(unsigned long long v)
{
    return v == 0 || ((v & DCP_PTE_VALID) && !(v & ~(DCP_PTE_PA_MASK | DCP_PTE_VALID)));
}

static inline int dcp_root_zero_at_handoff(const unsigned char *bitmap, unsigned int idx)
{
    return (bitmap[idx >> 3] >> (idx & 7)) & 1; /* bit set = the entry was zero at handoff */
}

/* may the guest's 64-bit write of `val` at byte offset `off` of the root page reach the table? */
static inline int dcp_root_write_ok(const unsigned char *zero_bitmap, unsigned long off, int width,
                                    unsigned long long val)
{
    unsigned int idx;

    if (width != 3 || (off & 7) || off >= DCP_ROOT_PAGE)
        return 0;
    idx = (unsigned int)(off >> 3);
    if (idx < DCP_ROOT_WIN_FIRST || idx >= DCP_ROOT_WIN_FIRST + DCP_ROOT_WIN_COUNT)
        return 0;
    if (!dcp_root_zero_at_handoff(zero_bitmap, idx))
        return 0;
    return dcp_root_value_ok(val);
}

/* ---- display owner register page (EL2 frame, 16 KiB, in the synthetic window) ----
 *   +0x00 OWNER   R/W 32-bit  1 = Windows drives the panel (the shadow converter pauses); 0 = it resumes.
 *                             Any other value is ignored and counted. Reads return the current owner.
 *   +0x04 STATE   R   bit0 owner, bit1 converter paused (it exists and is paused), bit2 converter exists (shadow
 *                     scanout active), bit3 guard armed, bits 15:8 owner changes since start (saturating)
 *   +0x08 PASSES  R   converter pass count, low word;  +0x0c high word
 *   +0x10 MAGIC   R   'M5WO' 0x4f57354d
 *   +0x14 VERSION R   1
 *   +0x18 DART_DROPS R  guest writes dropped by the DART guards (saturating at 0xffffffff)
 *   +0x1c ROOT_DROPS R  guest writes dropped by the root-table guards
 * Everything else reads 0; writes to anything but OWNER are ignored.
 */
#define DCPO_OWNER      0x00ul
#define DCPO_STATE      0x04ul
#define DCPO_PASSES_LO  0x08ul
#define DCPO_PASSES_HI  0x0cul
#define DCPO_MAGIC      0x10ul
#define DCPO_VERSION    0x14ul
#define DCPO_DART_DROPS 0x18ul
#define DCPO_ROOT_DROPS 0x1cul
#define DCPO_MAGIC_VAL  0x4f57354du

struct dcpo_state {
    unsigned int owner;      /* 0 or 1 */
    unsigned int fb_exists;
    unsigned int fb_paused;
    unsigned int armed;
    unsigned int changes;
    unsigned long long passes;
    unsigned int dart_drops, root_drops;
};

static inline unsigned int dcpo_reg32(unsigned long off, const struct dcpo_state *s)
{
    unsigned int ch = s->changes > 255 ? 255 : s->changes;

    switch (off) {
        case DCPO_OWNER: return s->owner & 1;
        case DCPO_STATE:
            return (s->owner & 1) | ((s->fb_paused ? 1u : 0) << 1) | ((s->fb_exists ? 1u : 0) << 2) |
                   ((s->armed ? 1u : 0) << 3) | (ch << 8);
        case DCPO_PASSES_LO: return (unsigned int)s->passes;
        case DCPO_PASSES_HI: return (unsigned int)(s->passes >> 32);
        case DCPO_MAGIC: return DCPO_MAGIC_VAL;
        case DCPO_VERSION: return 1;
        case DCPO_DART_DROPS: return s->dart_drops;
        case DCPO_ROOT_DROPS: return s->root_drops;
        default: return 0;
    }
}

/* read of 4 or 8 bytes at an aligned offset; other widths and unaligned reads return 0 */
static inline unsigned long long dcpo_read(unsigned long off, int width, const struct dcpo_state *s)
{
    if (width == 2 && !(off & 3))
        return dcpo_reg32(off, s);
    if (width == 3 && !(off & 7))
        return (unsigned long long)dcpo_reg32(off, s) | ((unsigned long long)dcpo_reg32(off + 4, s) << 32);
    return 0;
}

enum dcpo_op { DCPO_IGNORE = 0, DCPO_SET_OWNER = 1 };

/* a guest write: 32-bit OWNER write of 0 or 1 only; *owner_out gets the new owner */
static inline enum dcpo_op dcpo_write(unsigned long off, int width, unsigned long long val, unsigned int *owner_out)
{
    if (width != 2 || off != DCPO_OWNER || val > 1)
        return DCPO_IGNORE;
    *owner_out = (unsigned int)val;
    return DCPO_SET_OWNER;
}

#endif
