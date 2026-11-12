/* SPDX-License-Identifier: MIT */
/*
 * dcp-pt: display-coprocessor passthrough support in the resident (launcher --dcp passthrough, P_HV_WIN_CONFIGURE
 * flag HV_WIN_F_DCP_PT). display/dcpn-20261111/DESIGN.md decisions 3-5 and platform/BUILD-NOTES.md.
 *
 * EL2 never talks to the DCP and hosts no driver logic. It maps hardware (the DCP ASC window is a default
 * passthrough mapping, identity IPA == PA, made explicit by the launcher) and installs three guards, all software
 * MMIO/memory hooks whose decisions live in hv_win_dcp_logic.h (host-tested):
 *   1. dart-dcp (0x48a340000) and dart-disp0 (0x48a300000) register windows: reads pass through; writes only to the
 *      TLB flush registers of the guest's own streams (SID 23 on dart-dcp; SIDs 0 and 16 on dart-disp0) and to the
 *      error-status clear registers. TTBR/TCR/PROTECT/CONFIG/other-SID writes are dropped and counted.
 *   2. the root-table page of each of those three streams (the ADT l1-tt-N page): writes only to entries 16..31
 *      that were zero at handoff, with a table-entry shaped value or zero. Writes the guard lets through are
 *      cleaned to the point of coherency at EL2 (the DART walker reads them), so the guest needs no cache
 *      maintenance for them. Exclave SIDs (3, 4, 24) are never mapped to the guest: their root pages are not hooked
 *      and their registers fall under the "other SID" drop rule.
 *   3. a "display owner" register page, a 16 KiB EL2 frame in the synthetic window (DCP_OWNER_BASE): the guest
 *      writes 1 when Windows drives the panel (the shadow converter pauses) and 0 to hand it back (the converter
 *      resumes, reconverting the whole shadow). The converter also resumes by itself at guest exit.
 *
 * Refuses the guest (hv_start fails, like the GPU PS guard) when any ADT fact differs from what the launcher
 * checked: the DCP is not running, a DART is not locked/translating/four-level, a TTBR does not match its l1-tt
 * root. Nothing here writes a DCP, DART or display register.
 *
 * Known gap (same as the GPU, DESIGN.md 3.1 decision 4): the middle/leaf tables Windows builds live in guest RAM,
 * so the DCP can be pointed at any physical page.
 */

#include "hv_win.h"
#include "adt.h"
#include "memory.h"
#include "utils.h"
#include "../hv.h"
#include "hv_win_dcp_logic.h"

#define DCP_ASC_BASE    0x48ae00000ul
#define DCP_ASC_CPU_CONTROL 0x44ul
#define DCP_ASC_RUN     0x10u
#define DART_DCP_BASE   0x48a340000ul
#define DART_DISP0_BASE 0x48a300000ul
#define DART_MIN_SIZE   0x9000ul /* PROTECT 0x200 .. per-SID error words up to 0x87c0, 0x704 */
#define DART_PROTECT    0x200ul
#define DART_TCR(sid)   (0x1000ul + 4ul * (sid))
#define DART_TTBR(sid)  (0x1400ul + 4ul * (sid))
#define TCR_TRANSLATE   BIT(0)
#define TCR_FOUR_LEVEL  BIT(3)
#define TTBR_VALID      BIT(0)

#define DCP_OWNER_BASE  0xF38120000ul /* synthetic window, 64 KiB aligned; HIDM 0x..00000, M5WN 0x..08000, SMCP 0x..10000 */

struct dcp_stream {
    int dart;
    u32 sid;
    const char *name;
    u64 root;
    u8 zero[DCP_ROOT_BITMAP_BYTES];
    u32 nonzero_total, nonzero_window;
};

static struct dcp_stream streams[3] = {
    {DCP_DART_DCP, DCP_SID_DCP, "dart-dcp SID 23", 0, {0}, 0, 0},
    {DCP_DART_DISP, DCP_SID_PIODMA, "dart-disp0 SID 16", 0, {0}, 0, 0},
    {DCP_DART_DISP, DCP_SID_DISP, "dart-disp0 SID 0", 0, {0}, 0, 0},
};
static const char *const dart_names[2] = {"dart-dcp", "dart-disp0"};
static u64 dart_base[2], dart_size[2];
static u64 dcp_asc_dropped, dcp_asc_base_addr;
static u64 dcp_dart_dropped, dcp_dart_passed, dcp_dart_masked, dcp_root_dropped, dcp_root_passed;
static u64 dcp_owner_writes, dcp_owner_ignored;
static u32 dcp_owner, dcp_changes;
static bool dcp_armed;

static int dcp_dart_of(u64 addr)
{
    for (int i = 0; i < 2; i++)
        if (dart_base[i] && addr >= dart_base[i] && addr < dart_base[i] + dart_size[i])
            return i;
    return -1;
}

static u64 dcp_read_raw(u64 addr, int width)
{
    switch (width) {
        case 0: return read8(addr);
        case 1: return read16(addr);
        case 2: return read32(addr);
        default: return read64(addr);
    }
}

static bool hv_win_dcp_dart_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    UNUSED(ctx);
    int d = dcp_dart_of(addr);

    if (d < 0)
        return false;
    if (!write) {
        *val = dcp_read_raw(addr, width);
        return true;
    }

    u64 off = addr - dart_base[d];
    struct dcp_write w = dcp_dart_write(d, off, width, *val);
    if (w.act == DCP_PASS) {
        write32(addr, w.val);
        __atomic_fetch_add(&dcp_dart_passed, 1, __ATOMIC_RELAXED);
        if (w.val != (u32)*val)
            __atomic_fetch_add(&dcp_dart_masked, 1, __ATOMIC_RELAXED);
        return true;
    }
    u64 n = __atomic_add_fetch(&dcp_dart_dropped, 1, __ATOMIC_RELAXED);
    if (n <= 8)
        printf("HV/win dcp: %s write +0x%lx width %d value 0x%lx dropped (#%ld)\n", dart_names[d], (long)off,
               1 << width, (long)*val, (long)n);
    return true;
}

static bool hv_win_dcp_asc_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    UNUSED(ctx);
    u64 off = addr - dcp_asc_base_addr;

    if (!write) {
        *val = dcp_read_raw(addr, width);
        return true;
    }
    if (dcp_asc_write_ok(off, width, *val)) {
        switch (width) {
            case 0: write8(addr, *val); break;
            case 1: write16(addr, *val); break;
            case 2: write32(addr, *val); break;
            default: write64(addr, *val); break;
        }
        return true;
    }
    u64 n = __atomic_add_fetch(&dcp_asc_dropped, 1, __ATOMIC_RELAXED);
    if (n <= 8)
        printf("HV/win dcp: ASC +0x%lx width %d value 0x%lx dropped: would stop the DCP CPU (#%ld)\n", (long)off,
               1 << width, (long)*val, (long)n);
    return true;
}

static int dcp_root_of(u64 addr)
{
    for (int i = 0; i < 3; i++)
        if (streams[i].root && addr >= streams[i].root && addr < streams[i].root + DCP_ROOT_PAGE)
            return i;
    return -1;
}

static bool hv_win_dcp_root_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    UNUSED(ctx);
    int i = dcp_root_of(addr);

    if (i < 0)
        return false;
    if (!write) {
        *val = dcp_read_raw(addr, width);
        return true;
    }

    u64 off = addr - streams[i].root;
    if (dcp_root_write_ok(streams[i].zero, off, width, *val)) {
        *(volatile u64 *)(uintptr_t)addr = *val;
        dc_civac_range((void *)(uintptr_t)addr, 8);
        sysop("dsb sy");
        __atomic_fetch_add(&dcp_root_passed, 1, __ATOMIC_RELAXED);
        return true;
    }
    u64 n = __atomic_add_fetch(&dcp_root_dropped, 1, __ATOMIC_RELAXED);
    if (n <= 8)
        printf("HV/win dcp: root table of %s write +0x%lx (entry %ld) width %d value 0x%lx dropped (#%ld)\n",
               streams[i].name, (long)off, (long)(off >> 3), 1 << width, (long)*val, (long)n);
    return true;
}

static struct dcpo_state dcp_owner_state(void)
{
    struct dcpo_state s = {
        .owner = dcp_owner,
        .fb_exists = hv_win_fb_active(),
        .fb_paused = hv_win_fb_paused(),
        .armed = dcp_armed,
        .changes = dcp_changes,
        .passes = hv_win_fb_passes(),
        .dart_drops = dcp_dart_dropped > 0xffffffffu ? 0xffffffffu : (u32)dcp_dart_dropped,
        .root_drops = dcp_root_dropped > 0xffffffffu ? 0xffffffffu : (u32)dcp_root_dropped,
        .asc_drops = dcp_asc_dropped > 0xffffffffu ? 0xffffffffu : (u32)dcp_asc_dropped,
    };
    return s;
}

static void dcp_set_owner(u32 owner)
{
    if (owner == dcp_owner)
        return;
    dcp_owner = owner;
    dcp_changes++;
    hv_win_fb_set_paused(owner != 0);
    printf("HV/win dcp: display owner -> %s (shadow converter %s, %ld pass(es) so far)\n",
           owner ? "Windows" : "EL2", owner ? "paused" : "resumed, whole shadow reconverted",
           (long)hv_win_fb_passes());
}

static bool hv_win_dcp_owner_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    UNUSED(ctx);
    u64 off = addr - DCP_OWNER_BASE;

    if (!write) {
        struct dcpo_state s = dcp_owner_state();
        *val = dcpo_read(off, width, &s);
        return true;
    }
    __atomic_fetch_add(&dcp_owner_writes, 1, __ATOMIC_RELAXED);
    u32 owner = 0;
    if (dcpo_write(off, width, *val, &owner) == DCPO_SET_OWNER) {
        dcp_set_owner(owner);
    } else {
        u64 n = __atomic_add_fetch(&dcp_owner_ignored, 1, __ATOMIC_RELAXED);
        if (n <= 8)
            printf("HV/win dcp: owner page write +0x%lx width %d value 0x%lx ignored (#%ld)\n", (long)off,
                   1 << width, (long)*val, (long)n);
    }
    return true;
}

static int dcp_reg(const char *path, u64 *base, u64 *size)
{
    int p[8];
    if (adt_path_offset_trace(adt, path, p) < 0 || adt_get_reg(adt, p, "reg", 0, base, size) < 0)
        return -1;
    return 0;
}

static int dcp_l1tt(const char *path, u32 sid, u64 *root)
{
    char name[16];
    u64 v[2];
    int node = adt_path_offset(adt, path);

    snprintf(name, sizeof(name), "l1-tt-%u", sid);
    if (node < 0 || ADT_GETPROP_ARRAY(adt, node, name, v) < 0)
        return -1;
    *root = v[0];
    return 0;
}

static bool dcp_refuse(const char *why)
{
    printf("HV/win dcp: %s: guest refused\n", why);
    return false;
}

bool hv_win_dcp_start(void)
{
    if (!(hv_win.flags & HV_WIN_F_DCP_PT))
        return true;

    u64 asc_base = 0, asc_size = 0;
    if (dcp_reg("/arm-io/dcp", &asc_base, &asc_size) || asc_base != DCP_ASC_BASE || asc_size < 0x10000 ||
        (asc_size & 0x3fff))
        return dcp_refuse("ADT /arm-io/dcp reg[0] is not the expected ASC window");
    if (dcp_reg("/arm-io/dart-dcp", &dart_base[DCP_DART_DCP], &dart_size[DCP_DART_DCP]) ||
        dart_base[DCP_DART_DCP] != DART_DCP_BASE || dart_size[DCP_DART_DCP] < DART_MIN_SIZE ||
        (dart_size[DCP_DART_DCP] & 0x3fff))
        return dcp_refuse("ADT /arm-io/dart-dcp reg[0] is not the expected window");
    if (dcp_reg("/arm-io/dart-disp0", &dart_base[DCP_DART_DISP], &dart_size[DCP_DART_DISP]) ||
        dart_base[DCP_DART_DISP] != DART_DISP0_BASE || dart_size[DCP_DART_DISP] < DART_MIN_SIZE ||
        (dart_size[DCP_DART_DISP] & 0x3fff))
        return dcp_refuse("ADT /arm-io/dart-disp0 reg[0] is not the expected window");
    if (!(read32(asc_base + DCP_ASC_CPU_CONTROL) & DCP_ASC_RUN))
        return dcp_refuse("the DCP is not running (CPU_CONTROL RUN clear)");

    for (int d = 0; d < 2; d++)
        if (!(read32(dart_base[d] + DART_PROTECT) & 1))
            return dcp_refuse("a display DART is not locked (PROTECT bit 0 clear)");

    for (int i = 0; i < 3; i++) {
        struct dcp_stream *s = &streams[i];
        const char *path = s->dart == DCP_DART_DCP ? "/arm-io/dart-dcp" : "/arm-io/dart-disp0";
        u32 tcr = read32(dart_base[s->dart] + DART_TCR(s->sid));
        u32 ttbr = read32(dart_base[s->dart] + DART_TTBR(s->sid));
        u64 root = ((u64)((ttbr & 0x3ffffffcu) >> 2)) << 14;

        if (dcp_l1tt(path, s->sid, &s->root) || !s->root || (s->root & (DCP_ROOT_PAGE - 1)))
            return dcp_refuse("an ADT l1-tt root is missing or not 16 KiB aligned");
        if ((tcr & (TCR_TRANSLATE | TCR_FOUR_LEVEL)) != (TCR_TRANSLATE | TCR_FOUR_LEVEL) || !(ttbr & TTBR_VALID) ||
            root != s->root) {
            printf("HV/win dcp: %s TCR 0x%x TTBR 0x%x (root 0x%lx) vs ADT l1-tt 0x%lx\n", s->name, tcr, ttbr,
                   root, s->root);
            return dcp_refuse("a stream is not translating, four-level and rooted at its ADT l1-tt");
        }
        for (int j = 0; j < i; j++)
            if (streams[j].root == s->root)
                return dcp_refuse("two streams share a root table page");
    }

    /* snapshot of the root tables as iBoot left them: which entries were zero */
    for (int i = 0; i < 3; i++) {
        struct dcp_stream *s = &streams[i];
        for (u32 e = 0; e < DCP_ROOT_ENTRIES; e++) {
            u64 v = *(volatile u64 *)(uintptr_t)(s->root + 8ul * e);
            if (!v) {
                s->zero[e >> 3] |= (u8)(1u << (e & 7));
            } else {
                s->nonzero_total++;
                if (e >= DCP_ROOT_WIN_FIRST && e < DCP_ROOT_WIN_FIRST + DCP_ROOT_WIN_COUNT)
                    s->nonzero_window++;
            }
        }
        printf("HV/win dcp: root %s 0x%lx: %u of %u entries non-zero at handoff, %u of them in the window "
               "slots %u..%u\n",
               s->name, s->root, s->nonzero_total, DCP_ROOT_ENTRIES, s->nonzero_window, DCP_ROOT_WIN_FIRST,
               DCP_ROOT_WIN_FIRST + DCP_ROOT_WIN_COUNT - 1);
    }

    for (int d = 0; d < 2; d++) {
        if (hv_map_hook(dart_base[d], hv_win_dcp_dart_hook, dart_size[d]) < 0)
            return dcp_refuse("DART register hook failed");
        printf("HV/win dcp: %s 0x%lx+0x%lx trapped: reads pass, writes only TLB flush of own streams and "
               "error clears\n",
               dart_names[d], dart_base[d], dart_size[d]);
    }
    dcp_asc_base_addr = asc_base;
    if (hv_map_hook(asc_base, hv_win_dcp_asc_hook, DCP_ASC_PAGE) < 0)
        return dcp_refuse("ASC CPU_CONTROL page hook failed");
    printf("HV/win dcp: ASC 0x%lx+0x%lx trapped (CPU_CONTROL page): reads pass, writes that clear RUN dropped; mailbox page native\n",
           asc_base, DCP_ASC_PAGE);
    for (int i = 0; i < 3; i++) {
        if (hv_map_hook(streams[i].root, hv_win_dcp_root_hook, DCP_ROOT_PAGE) < 0)
            return dcp_refuse("root table hook failed");
    }
    if (hv_map_hook(DCP_OWNER_BASE, hv_win_dcp_owner_hook, 0x4000) < 0)
        return dcp_refuse("display owner page hook failed");

    sysop("dsb ishst");
    sysop("tlbi vmalls12e1is");
    sysop("dsb ish");
    sysop("isb");
    dcp_owner = 0;
    dcp_armed = true;
    printf("HV/win dcp: guard armed: ASC 0x%lx+0x%lx passthrough (mailbox +0x8000), roots 0x%lx 0x%lx 0x%lx "
           "(entries %u..%u writable when zero at handoff), owner register 0x%lx (default Windows-not-owner)\n",
           asc_base, asc_size, streams[0].root, streams[1].root, streams[2].root, DCP_ROOT_WIN_FIRST,
           DCP_ROOT_WIN_FIRST + DCP_ROOT_WIN_COUNT - 1, (unsigned long)DCP_OWNER_BASE);
    return true;
}

/* Guest exit (shutdown, reset, host stop, dead-man): the shadow converter always resumes. */
void hv_win_dcp_report(void)
{
    if (!dcp_armed)
        return;
    if (dcp_owner) {
        printf("HV/win dcp: guest exit with Windows as display owner: shadow converter resumes\n");
        dcp_set_owner(0);
    }
    printf("HV/win dcp: guards: DART writes %ld passed (%ld masked) %ld dropped; root writes %ld passed %ld dropped; ASC writes %ld dropped; "
           "owner page %ld write(s), %ld ignored, %ld owner change(s); converter %s, %ld pass(es)\n",
           (long)dcp_dart_passed, (long)dcp_dart_masked, (long)dcp_dart_dropped, (long)dcp_root_passed,
           (long)dcp_root_dropped, (long)dcp_asc_dropped, (long)dcp_owner_writes, (long)dcp_owner_ignored, (long)dcp_changes,
           hv_win_fb_paused() ? "PAUSED" : "running", (long)hv_win_fb_passes());
}
