/* SPDX-License-Identifier: MIT */
/*
 * nightly-5: the 4 KiB scan granule of src/hv/win/hv_win_pmu.c, against an independent reference
 * model, with ASan/UBSan. Mocks: hv_vm.c's execute tracking (state and flips per 16 KiB block), AT
 * (VA -> IPA, PA | VA[11:0]), HPFAR, cache maintenance, and the guest's stage-1 tables (real
 * descriptors in tracked mock RAM, walked by the production walker through the production
 * pmu_read_pte()). Replaces the codex proof-only suite (store emulation was dropped, see
 * REVIEW-GUIDE.md); its walker checks and hardware word replay are kept in adapted form.
 *
 * The guarantees tested:
 *   G1 a word is modified only in a frame whose own fetch faulted and whose VA the guest's stage-1
 *      tables map executable at the faulting EL to that frame (data frames next to code: never);
 *   G2 a frame is read (cleaned, scanned) only after that proof, inside a block that is read-only and
 *      execute-never on every CPU (SCANNING);
 *   G3 guest stores are never emulated or lost: a store to a read-only block flips it writable and
 *      lands natively, byte for byte, and every frame of the block is rescanned at its next proven
 *      fetch; a device fill lands byte for byte and the filled frames are rescanned;
 *   G4 every fault makes progress (an unproven fetch opens the block anyway; no STUCK);
 *   G5 a CODE block with an unscanned frame is re-protected every 10 ticks, so a later first
 *      fetch from that frame faults and is scanned (the residual is between those points);
 *   G6 the kernel debugger's software breakpoint (guest store of BRK into a code frame, later
 *      restored) and a debugger write of a PMU instruction behave as on bare metal.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pmu_host.h"
#include "hv_win.h"
#include "hv_win_pmu_4k.h"
#include "hv_win_pmu_rewrite.h"
#undef printf

#define BLOCKS 8
#define FRAMES (BLOCKS * 4)
#define WORDS  (FRAMES * 1024)
#define BASE   0x100000UL
#define VA     0x400000UL /* VA + 4096 * f maps frame f */
#define NOP    0xd503201fU
#define BRK    0xd4200000U
#define IESR   ((0x20UL << 26) | 15)          /* instruction abort, permission L3 */
#define DESR   ((0x24UL << 26) | 15 | BIT(6)) /* data abort, permission L3, WnR */
#define TABLE0 28                             /* frames 28..31: stage-1 tables */
#ifndef PMU_MAX_OPEN
#error "build with -DPMU_MAX_OPEN=<n> (Makefile: 4, so 8 blocks reach the list-full path)"
#endif

static u32 mem[WORDS] ALIGNED(16384), ref[WORDS];
static enum hv_xt_state states[BLOCKS];
static u32 flips_of[BLOCKS];
static bool tracked[BLOCKS], executable[FRAMES];
static struct pmu_s1 s1;
static unsigned long checks;
static bool at_ok;
static u64 hpfar;
int host_cpu;
u64 hv_cpus_in_guest;
struct hv_win_state hv_win;
struct boot_args cur_boot_args;
void hv_win_pmu_host_reset(void);
u8 hv_win_pmu_host_frame(u64 ipa);

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #x);                           \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

int host_printf(const char *fmt, ...)
{
    (void)fmt;
    return 0;
}
void host_ic_ialluis(void) {}
size_t heapblock_remaining(void)
{
    return 1UL << 30;
}
bool pmu_host_s1(struct pmu_s1 *s)
{
    *s = s1;
    return true;
}

/* ---- hv_vm.c execute tracking (same state semantics) ------------------------------------------- */

static int block_of(u64 ipa)
{
    return ipa >= BASE && ipa - BASE < sizeof(mem) ? (int)((ipa - BASE) / 16384) : -1;
}
int hv_xt_track(u64 ipa, u64 size, bool dry, u64 *tables)
{
    *tables = 1;
    if (ipa != BASE || size != sizeof(mem))
        return -1;
    if (!dry)
        for (unsigned i = 0; i < BLOCKS; i++) {
            tracked[i] = true;
            states[i] = HV_XT_DIRTY;
            flips_of[i] = 0;
        }
    return BLOCKS;
}
enum hv_xt_state hv_xt_get(u64 ipa, u64 *pa, u32 *flips)
{
    int b = block_of(ipa);
    if (b < 0 || !tracked[b])
        return HV_XT_UNTRACKED;
    if (pa)
        *pa = (u64)&mem[b * 4096];
    if (flips)
        *flips = flips_of[b];
    return states[b];
}
int hv_xt_set(u64 ipa, enum hv_xt_state state, u32 flips)
{
    int b = block_of(ipa);
    CHECK(b >= 0 && tracked[b] && !(ipa & 16383));
    states[b] = state;
    flips_of[b] = flips < HV_XT_FLIPS_MAX ? flips : HV_XT_FLIPS_MAX;
    return 0;
}
void hv_xt_tlbi(bool broadcast)
{
    (void)broadcast;
}
bool hv_xt_fault_ipa(struct exc_info *ctx, bool s1ptw, bool write, u64 *ipa)
{
    (void)write;
    if (s1ptw) {
        *ipa = hpfar;
        return true;
    }
    if (!at_ok || ctx->far < VA || ctx->far - VA >= sizeof(mem))
        return false;
    *ipa = BASE + (ctx->far - VA);
    return true;
}
/* G2: a scan starts only on a frame the stage-1 tables map executable, in a SCANNING block */
static bool legacy16k; /* replay(): the 16 KiB granule cleans whole blocks without a proof */
static void cache_check(void *addr, size_t size)
{
    if (legacy16k)
        return;
    size_t off = (u8 *)addr - (u8 *)mem;
    CHECK(off < sizeof(mem) && size == 4096 && !(off & 4095));
    CHECK(states[off / 16384] == HV_XT_SCANNING);
    CHECK(executable[off / 4096]);
}
void dc_civac_range(void *addr, size_t size)
{
    cache_check(addr, size);
}
void dc_cvac_range(void *addr, size_t size)
{
    cache_check(addr, size);
}

/* ---- stage-1 tables: Windows' configuration (TCR_EL1 0x615b5513511: T0SZ 17, HPD0/1) ---------- */

static u64 *table(unsigned frame)
{
    return (u64 *)&mem[frame * 1024];
}
static u64 leaf(unsigned f)
{
    /* EL1 executable read-only code; data frames PXN + UXN; EL0 no access */
    return (BASE + f * 4096UL) | 3 | BIT(10) | BIT(7) | (executable[f] ? 0 : BIT(53) | BIT(54));
}
static void build_tables(void)
{
    /* TTBR0 root (level 0, 2 KiB) at frame 28 + 0x800, as Windows' TTBR1 at +0x800 */
    memset(table(TABLE0), 0, 4 * 4096);
    table(TABLE0)[256] = (BASE + 29 * 4096UL) | 3; /* L0[0] */
    table(29)[0] = (BASE + 30 * 4096UL) | 3;       /* L1[0] */
    table(30)[2] = (BASE + 31 * 4096UL) | 3;       /* L2[2]: VA 0x400000 */
    for (unsigned f = 0; f < FRAMES; f++)
        table(31)[f] = leaf(f);
    s1 = (struct pmu_s1){.sctlr = 1,
                         .tcr = 0x615b5513511UL,
                         .ttbr = {0x0001000000000000UL | (BASE + TABLE0 * 4096UL + 0x800),
                                  0x0040000000000000UL | (BASE + TABLE0 * 4096UL)}};
}

/* ---- reference model ------------------------------------------------------------------------- */

static struct {
    enum hv_xt_state st[BLOCKS];
    u32 flips[BLOCKS];
    bool listed[BLOCKS], scanned[FRAMES];
    unsigned nlisted, close_ticks, npinned, pin_ticks;
    u64 frames_scanned, words, denied, write_flips, walk_flips, pins, reprotects, unpins, dma;
} m;

static void reset(void)
{
    hv_win_pmu_host_reset();
    memset(&hv_win, 0, sizeof(hv_win));
    hv_win.irq_model = HV_WIN_IRQ_VGIC3;
    hv_win.nr_vcpus = 1;
    memset(states, 0, sizeof(states));
    memset(tracked, 0, sizeof(tracked));
    memset(&m, 0, sizeof(m));
    for (unsigned f = 0; f < FRAMES; f++) {
        executable[f] = f < 24 && !(f & 1); /* code in even frames, data in odd ones and above */
        u32 *w = &mem[f * 1024];
        for (unsigned i = 0; i < 1024; i++)
            w[i] = NOP;
        w[32] = 0xd53b9d05; /* mrs x5, PMCCNTR_EL0 */
        w[64] = 0xd51b9c01; /* msr PMCR_EL0, x1 */
        w[96] = 0xd53be8c3; /* PMU-looking PTE low word, high word below: data guard */
        w[97] = 0x00200100;
        w[100] = 0xd53bedc3; /* PMU-looking, next word defeats the data guard */
        w[101] = 0xf0200100;
        w[1023] = 0xd53b9d00; /* last slot: guarded by the next frame's first word */
    }
    mem[1024 * 2] = 0x00000000; /* frame 2 word 0 is data: frame 1's last word is guarded */
    build_tables();
    hv_cpus_in_guest = 0;
    host_cpu = 0;
    at_ok = true;
    for (unsigned b = 0; b < BLOCKS; b++)
        m.st[b] = HV_XT_DIRTY;
    CHECK(hv_win_pmu_set_mode(0) == 0);
    CHECK(hv_win_pmu_arm(BASE, sizeof(mem)) == BLOCKS);
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_ID) & HV_WIN_PMU_ID_4K);
    memcpy(ref, mem, sizeof(mem));
}

/* independent decode (no production classifier or replacement) */
static u32 replacement(u32 w)
{
    unsigned r = (w >> 5) & 0xffff, op1 = (r >> 11) & 7, crn = (r >> 7) & 15, crm = (r >> 3) & 15;
    if ((w >> 22) != 0x354 || ((r >> 14) & 3) != 3)
        return w;
    if (!((crn == 9 && ((op1 == 3 && crm >= 12 && crm <= 14) || (op1 == 0 && crm == 14))) ||
          (crn == 14 && op1 == 3 && crm >= 8)))
        return w;
    if (!(w & (1U << 21)))
        return NOP;
    return (r == 0xdce8 ? 0xd53be040U : 0xd2800000U) | (w & 31);
}
static void ref_scan(unsigned f)
{
    unsigned n = 0;
    for (unsigned i = 0; i < 1024; i++) {
        u32 *w = &ref[f * 1024 + i], new = replacement(*w);
        if (new == *w)
            continue;
        bool has_next = i < 1023 || f + 1 < FRAMES; /* all blocks tracked; none after the last */
        u32 next = i < 1023 ? w[1] : (f + 1 < FRAMES ? ref[(f + 1) * 1024] : 0);
        if (!has_next || (next >> 27) == 0)
            continue;
        *w = new;
        n++;
    }
    m.scanned[f] = true;
    m.frames_scanned++;
    m.words += n;
}
static bool ref_all(unsigned b)
{
    for (unsigned f = 4 * b; f < 4 * b + 4; f++)
        if (!m.scanned[f])
            return false;
    return true;
}
static void ref_clear(unsigned b)
{
    for (unsigned f = 4 * b; f < 4 * b + 4; f++)
        m.scanned[f] = false;
}
static void ref_close(void)
{
    for (unsigned b = 0; b < BLOCKS; b++) {
        if (m.listed[b] && m.st[b] == HV_XT_CODE && !ref_all(b)) {
            m.st[b] = HV_XT_SCANNING;
            m.reprotects++;
        }
        m.listed[b] = false;
    }
    m.nlisted = 0;
    m.close_ticks = 0;
}

static void equivalent(void)
{
    CHECK(memcmp(mem, ref, sizeof(mem)) == 0);
    for (unsigned f = 0; f < FRAMES; f++)
        CHECK(!!(hv_win_pmu_host_frame(BASE + f * 4096UL) & PMU_FRAME_SCANNED) == m.scanned[f]);
    for (unsigned b = 0; b < BLOCKS; b++)
        CHECK(states[b] == m.st[b] && flips_of[b] == m.flips[b]);
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_FRAMES_SCANNED) == m.frames_scanned);
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_WORDS_MODIFIED) == m.words);
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_INSNS) == m.words);
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_EXEC_DENIED) == m.denied);
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_WRITE_FLIPS) == m.write_flips);
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_WALK_FLIPS) == m.walk_flips);
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_PINS) == m.pins);
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_UNPINS) == m.unpins);
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_REPROTECTS) == m.reprotects);
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_DMA_RESETS) == m.dma);
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_MODIFIED_NEVER_EXECUTED) == 0);
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_STUCK) == 0);
}

/* The guest fetches frame f at word offset off (EL1h). Returns 1 if the fetch faulted. */
static unsigned execute_at(unsigned f, unsigned off)
{
    unsigned b = f / 4;
    if (m.st[b] == HV_XT_CODE || m.st[b] == HV_XT_PINNED)
        return 0; /* the real granule: no fault inside an executable block */
    struct exc_info ctx = {.far = VA + f * 4096UL + off * 4, .elr = VA + f * 4096UL + off * 4,
                           .esr = IESR, .spsr = 5};
    CHECK(hv_win_handle_iabort(&ctx)); /* G4 */
    if (m.st[b] == HV_XT_DIRTY)
        ref_clear(b);
    if (!m.scanned[f]) {
        if (executable[f])
            ref_scan(f);
        else
            m.denied++;
    }
    m.st[b] = HV_XT_CODE;
    if (!ref_all(b) && !m.listed[b]) {
        if (m.nlisted >= PMU_MAX_OPEN) /* list full: everything listed is re-protected first */
            ref_close();
        m.listed[b] = true;
        m.nlisted++;
    }
    equivalent();
    return 1;
}
static unsigned execute(unsigned f)
{
    return execute_at(f, 128);
}
static void ref_flip(unsigned b, bool walker)
{
    ref_clear(b);
    m.flips[b] = m.flips[b] + 1 < HV_XT_FLIPS_MAX ? m.flips[b] + 1 : HV_XT_FLIPS_MAX;
    if (m.flips[b] >= 4 && m.npinned < 256) {
        if (!m.npinned)
            m.pin_ticks = 0;
        m.npinned++;
        m.st[b] = HV_XT_PINNED;
        m.pins++;
        return;
    }
    m.st[b] = HV_XT_DIRTY;
    if (walker)
        m.walk_flips++;
    else
        m.write_flips++;
}
/* The guest stores size bytes of value at byte offset off (one 16 KiB block). G3. */
static void store(unsigned off, unsigned size, u64 value)
{
    unsigned b = off / 16384;
    CHECK((off + size - 1) / 16384 == b);
    if (m.st[b] == HV_XT_CODE || m.st[b] == HV_XT_SCANNING) {
        static u32 before[WORDS / BLOCKS];
        memcpy(before, &mem[b * 4096], sizeof(before));
        struct exc_info ctx = {.far = VA + off, .elr = 0x1234, .esr = DESR, .spsr = 5};
        CHECK(hv_win_handle_dabort(&ctx, ctx.esr));
        CHECK(ctx.elr == 0x1234); /* re-executed by the caller, not stepped over or emulated */
        CHECK(memcmp(before, &mem[b * 4096], sizeof(before)) == 0);
        CHECK(states[b] == HV_XT_DIRTY || states[b] == HV_XT_PINNED); /* the store can land */
        ref_flip(b, false);
    }
    memcpy((u8 *)mem + off, &value, size); /* the native store, re-executed */
    memcpy((u8 *)ref + off, &value, size);
    equivalent();
}
static void walker_update(unsigned f)
{
    unsigned b = f / 4;
    hpfar = BASE + f * 4096UL;
    struct exc_info ctx = {.far = 0x5000, .elr = 0x1234, .esr = DESR | BIT(7), .spsr = 5};
    if (m.st[b] == HV_XT_CODE || m.st[b] == HV_XT_SCANNING) {
        CHECK(hv_win_handle_dabort(&ctx, ctx.esr));
        ref_flip(b, true);
    }
    equivalent();
}
static void fill(unsigned off, unsigned size, u32 value)
{
    unsigned n = 0;
    for (unsigned b = off / 16384; b <= (off + size - 1) / 16384; b++) {
        if (m.st[b] == HV_XT_CODE || m.st[b] == HV_XT_SCANNING) {
            for (unsigned f = off / 4096 > 4 * b ? off / 4096 : 4 * b;
                 f <= (off + size - 1) / 4096 && f < 4 * b + 4; f++)
                m.scanned[f] = false;
            if (m.st[b] == HV_XT_CODE) {
                m.st[b] = HV_XT_SCANNING;
                n++;
            }
        } else if (m.st[b] == HV_XT_PINNED) {
            m.st[b] = HV_XT_DIRTY;
            n++;
        }
    }
    CHECK(hv_win_pmu_dma_write(BASE + off, size) == n);
    for (unsigned b = off / 16384; b <= (off + size - 1) / 16384; b++)
        CHECK(states[b] != HV_XT_CODE && states[b] != HV_XT_PINNED); /* no X while EL2 copies */
    m.dma += n;
    for (unsigned i = 0; i < size; i++) {
        ((u8 *)mem)[off + i] = ((u8 *)&value)[i % 4];
        ((u8 *)ref)[off + i] = ((u8 *)&value)[i % 4];
    }
    equivalent();
}
static void tick(void)
{
    hv_win_pmu_tick();
    if (m.nlisted && ++m.close_ticks >= 10)
        ref_close();
    if (m.npinned && ++m.pin_ticks >= 100) {
        for (unsigned b = 0; b < BLOCKS; b++)
            if (m.st[b] == HV_XT_PINNED) {
                m.st[b] = HV_XT_DIRTY;
                m.flips[b] = 0;
                m.unpins++;
            }
        m.npinned = 0;
        m.pin_ticks = 0;
    }
    equivalent();
}
static void ticks(unsigned n)
{
    while (n--)
        tick();
}

/* ---- tests ----------------------------------------------------------------------------------- */

static void frame_scanner(void)
{
    static u32 words[1024];
    u64 counts[PMU_RW_NR_KINDS] = {0};
    for (unsigned i = 0; i < 1024; i++)
        words[i] = NOP;
    words[1023] = 0xd53b9d00;
    CHECK(pmu_rw_rewrite_frame(words, NULL, NULL, NULL, NULL, counts) == 0);
    CHECK(words[1023] == 0xd53b9d00 && counts[PMU_RW_SKIP_BOUNDARY] == 1 &&
          counts[PMU_RW_SKIP_OTHER] == 1023);
    u32 data = 0x00200100, code = 0xd65f03c0;
    memset(counts, 0, sizeof(counts));
    CHECK(pmu_rw_rewrite_frame(words, &data, NULL, NULL, NULL, counts) == 0);
    CHECK(words[1023] == 0xd53b9d00 && counts[PMU_RW_SKIP_DATA] == 1);
    memset(counts, 0, sizeof(counts));
    CHECK(pmu_rw_rewrite_frame(words, &code, NULL, NULL, NULL, counts) == 1);
    CHECK(words[1023] == 0xd53be040 && counts[PMU_RW_READ_PMCCNTR] == 1);
}

/* G1/G2/G5: code and data frames share every block; data frames are never read or written */
static void mixed(void)
{
    reset();
    CHECK(execute(0) == 1);
    CHECK(mem[32] == 0xd53be045 && mem[64] == NOP && mem[96] == 0xd53be8c3 &&
          mem[100] == 0xd2800003);
    CHECK(mem[1023] == 0xd53be040); /* frame 1's word 0 is NOP: guarded word rewritten */
    CHECK(mem[1024 + 100] == 0xd53bedc3 && mem[1024 + 32] == 0xd53b9d05); /* frame 1: data */
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_DATA_SKIPS) == 1);
    CHECK(states[0] == HV_XT_CODE);
    CHECK(execute(2) == 0 && !m.scanned[2]); /* the residual: no fault inside an open block */
    ticks(10);
    CHECK(states[0] == HV_XT_SCANNING);
    CHECK(execute(0) == 1 && hv_win_pmu_stat(HV_WIN_PMU_FRAMES_SCANNED) == 1); /* reopen only */
    ticks(10);
    CHECK(execute(2) == 1 && m.scanned[2] && mem[2048 + 32] == 0xd53be045);
    CHECK(mem[2048 - 1] == 0xd53b9d00); /* frame 1's last word: data, never touched */
    /* frames 0 and 2 scanned, 1 and 3 are data: the block keeps being re-protected */
    ticks(10);
    CHECK(states[0] == HV_XT_SCANNING && hv_win_pmu_stat(HV_WIN_PMU_REPROTECTS) == 3);
    /* a block whose four frames are code and all scanned is never re-protected */
    reset();
    for (unsigned f = 0; f < 4; f++) {
        executable[f] = true;
        table(31)[f] = leaf(f);
    }
    memcpy(ref, mem, sizeof(mem));
    for (unsigned f = 0; f < 4; f++) {
        execute(f);
        ticks(10);
    }
    CHECK(states[0] == HV_XT_CODE && ref_all(0));
    ticks(100);
    CHECK(states[0] == HV_XT_CODE);
    /* a device fill re-protects a listed block; its last frame's fetch completes it while it is
     * still listed: the next close leaves it executable */
    reset();
    for (unsigned f = 0; f < 4; f++) {
        executable[f] = true;
        table(31)[f] = leaf(f);
    }
    memcpy(ref, mem, sizeof(mem));
    execute(0);
    ticks(10);
    execute(1);
    ticks(10);
    execute(2);
    fill(3 * 4096 + 64, 4, NOP);
    execute(3);
    CHECK(ref_all(0) && m.listed[0]);
    ticks(10);
    CHECK(states[0] == HV_XT_CODE);
}

/* G4/G1: unproven fetches open the block without reading it */
static void unproven(void)
{
    struct exc_info ctx;
    /* the frame's leaf is PXN: the walk refuses */
    reset();
    ctx = (struct exc_info){.far = VA + 4096 + 8, .elr = VA + 4096 + 8, .esr = IESR, .spsr = 5};
    CHECK(hv_win_handle_iabort(&ctx) && states[0] == HV_XT_CODE);
    m.st[0] = HV_XT_CODE;
    m.denied++;
    m.listed[0] = true;
    m.nlisted++;
    equivalent();
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_WORDS_SKIP_EXEC) == 1024);
    /* FAR != ELR, FnV, a data-abort class: never a proof */
    reset();
    ctx = (struct exc_info){.far = VA + 128, .elr = VA, .esr = IESR, .spsr = 5};
    CHECK(hv_win_handle_iabort(&ctx));
    CHECK(memcmp(mem, ref, sizeof(mem)) == 0 && hv_win_pmu_stat(HV_WIN_PMU_EXEC_DENIED) == 1);
    reset();
    ctx = (struct exc_info){.far = VA, .elr = VA, .esr = IESR | BIT(10), .spsr = 5};
    CHECK(hv_win_handle_iabort(&ctx));
    CHECK(memcmp(mem, ref, sizeof(mem)) == 0 && hv_win_pmu_stat(HV_WIN_PMU_EXEC_DENIED) == 1);
    reset();
    ctx = (struct exc_info){.far = VA, .elr = VA, .esr = (0x24UL << 26) | 15, .spsr = 5};
    CHECK(hv_win_handle_iabort(&ctx));
    CHECK(memcmp(mem, ref, sizeof(mem)) == 0 && hv_win_pmu_stat(HV_WIN_PMU_EXEC_DENIED) == 1);
    /* AT and the walk disagree on the frame (stale TLB / racing table update) */
    reset();
    table(31)[0] = leaf(2); /* VA of frame 0 now maps frame 2 (executable) */
    memcpy(ref, mem, sizeof(mem));
    ctx = (struct exc_info){.far = VA, .elr = VA, .esr = IESR, .spsr = 5};
    CHECK(hv_win_handle_iabort(&ctx));
    CHECK(memcmp(mem, ref, sizeof(mem)) == 0 && hv_win_pmu_stat(HV_WIN_PMU_FRAMES_SCANNED) == 0);
    /* an EL0 fetch from an EL1-only mapping: refused */
    reset();
    ctx = (struct exc_info){.far = VA, .elr = VA, .esr = IESR, .spsr = 0};
    CHECK(hv_win_handle_iabort(&ctx) && hv_win_pmu_stat(HV_WIN_PMU_FRAMES_SCANNED) == 0);
    CHECK(memcmp(mem, ref, sizeof(mem)) == 0);
    /* AT fails, HPFAR names a DIRTY block: opened, never scanned (a hint is no proof) */
    reset();
    at_ok = false;
    hpfar = BASE + 8192;
    ctx = (struct exc_info){.far = VA + 8192, .elr = VA + 8192, .esr = IESR, .spsr = 5};
    CHECK(hv_win_handle_iabort(&ctx) && states[0] == HV_XT_CODE);
    CHECK(memcmp(mem, ref, sizeof(mem)) == 0 && hv_win_pmu_stat(HV_WIN_PMU_EXEC_DENIED) == 1);
    /* another vCPU in the guest: still proven and scanned (no SMP refusal, no livelock) */
    reset();
    hv_cpus_in_guest = 2;
    CHECK(execute(0) == 1 && m.scanned[0]);
    /* an unsupported table format (52-bit LPA2) denies; the guest still progresses at once */
    reset();
    s1.tcr |= BIT(59);
    ctx = (struct exc_info){.far = VA, .elr = VA, .esr = IESR, .spsr = 5};
    CHECK(hv_win_handle_iabort(&ctx) && states[0] == HV_XT_CODE);
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_STUCK) == 0 && memcmp(mem, ref, sizeof(mem)) == 0);
}

/* G3: stores, walker updates, pinning, device fills */
static void writes(void)
{
    reset();
    execute(0);
    store(4096 + 4, 4, 0x11223344); /* data frame 1 of block 0: native, exact */
    CHECK(states[0] == HV_XT_DIRTY && !m.scanned[0] && mem[1025] == 0x11223344);
    store(40 * 4, 4, 0xd53b9d02); /* guest writes a PMU read into code frame 0 (block DIRTY) */
    execute(0);
    CHECK(mem[40] == 0xd53be042);
    /* widths and alignments an emulator would have to decode land natively */
    store(4096 + 8, 8, 0x8877665544332211UL);
    execute(0);
    store(4096 + 15, 1, 0xab);
    /* a store into a re-protected (SCANNING) block flips it like CODE */
    execute(0);
    ticks(10);
    CHECK(states[0] == HV_XT_SCANNING);
    store(8192 + 12, 4, 0x55);
    CHECK(states[0] == HV_XT_PINNED || states[0] == HV_XT_DIRTY);
    /* pinning after four write-after-execute flips, RWX until the sweep, then rescanned */
    reset();
    for (int i = 0; i < 4; i++) {
        execute(4);
        store(5 * 4096 + 4, 4, (u64)i);
    }
    CHECK(states[1] == HV_XT_PINNED && hv_win_pmu_stat(HV_WIN_PMU_PINS) == 1);
    CHECK(execute(4) == 0); /* RWX: nothing scanned */
    store(4 * 4096 + 4, 4, 0xd51b9c00);
    ticks(100);
    CHECK(states[1] == HV_XT_DIRTY);
    execute(4);
    CHECK(mem[4 * 1024 + 1] == NOP);
    /* stage-1 walker update (AF/DBM) of a table in a CODE block */
    reset();
    execute(6);
    walker_update(7);
    CHECK(states[1] == HV_XT_DIRTY && hv_win_pmu_stat(HV_WIN_PMU_WALK_FLIPS) == 1);
    /* device fills: CODE -> re-protected, only touched frames reset; DIRTY untouched; PINNED */
    reset();
    execute(0);
    ticks(10);
    execute(2);
    fill(4096 * 2 + 100, 8, 0xd53b9d03); /* refill inside code frame 2 */
    CHECK(states[0] == HV_XT_SCANNING && m.scanned[0] && !m.scanned[2]);
    execute(2);
    CHECK(mem[2048 + 25] == 0xd53be043 && mem[2048 + 26] == 0xd53be043);
    fill(4096 * 3 + 4092, 8, NOP);           /* crosses into block 1 (DIRTY) */
    fill(16384 * 2, 3 * 16384, 0xd53b9d04); /* three DIRTY blocks: nothing to revoke */
    for (int i = 0; i < 4; i++) {
        execute(20);
        store(21 * 4096, 4, 1);
    }
    CHECK(states[5] == HV_XT_PINNED);
    fill(20 * 4096, 4, 0xd53b9d06);
    CHECK(states[5] == HV_XT_DIRTY);
    execute(20);
    CHECK(mem[20 * 1024] == 0xd53be046);
    CHECK(hv_win_pmu_dma_write(~0UL - 4, 100) == 0 && hv_win_pmu_dma_write(BASE, 0) == 0);
    equivalent();
}

/* G6: Windows KD software breakpoint: store BRK, hit, restore, continue */
static void debugger(void)
{
    reset();
    execute(0);
    u32 saved = mem[200];
    store(200 * 4, 4, BRK); /* KD writes the breakpoint (through any VA: a store) */
    CHECK(mem[200] == BRK && states[0] == HV_XT_DIRTY);
    execute_at(0, 200); /* the CPU fetches it: frame rescanned, BRK untouched (not PMU) */
    CHECK(mem[200] == BRK && m.scanned[0]);
    store(200 * 4, 4, saved); /* restore */
    execute_at(0, 200);
    CHECK(mem[200] == saved);
    /* the debugger writes an unrewritten PMU read: rewritten before it runs, as before */
    store(300 * 4, 4, 0xd53b9d07);
    execute_at(0, 300);
    CHECK(mem[300] == 0xd53be047);
    /* single-step: a step into a non-executable block is one fetch fault and one retry */
    ticks(10);
    for (unsigned off = 128; off < 140; off++)
        execute_at(0, off);
    CHECK(hv_win_pmu_stat(HV_WIN_PMU_STUCK) == 0);
}

/* walker unit checks (production pmu_s1_exec over the mock tables) */
static bool read_table(void *unused, u64 ipa, u64 *v)
{
    (void)unused;
    if (ipa < BASE || ipa + 8 > BASE + sizeof(mem) || ipa % 8)
        return false;
    *v = *(u64 *)((u8 *)mem + ipa - BASE);
    return true;
}
static bool walk(unsigned f, unsigned mode)
{
    u64 ipa = 0;
    bool ok = pmu_s1_exec(&s1, VA + f * 4096UL, mode, read_table, NULL, &ipa);
    if (ok)
        CHECK(ipa == BASE + f * 4096UL);
    return ok;
}
static void walker(void)
{
    reset();
    CHECK(walk(0, 4) && walk(0, 5) && !walk(0, 0)); /* EL1 only (AP[1] = 0) */
    CHECK(!walk(0, 1) && !walk(0, 0x10) && !walk(1, 5));
    /* HPD0 set (Windows): table PXN/UXN/APTable bits ignored */
    table(29)[0] |= BIT(59) | BIT(62);
    CHECK(walk(0, 5));
    s1.tcr &= ~BIT(41); /* HPD0 clear: PXNTable honoured */
    CHECK(!walk(0, 5));
    table(29)[0] &= ~BIT(59);
    CHECK(walk(0, 5)); /* APTable RO only */
    s1.sctlr |= BIT(19);
    CHECK(walk(0, 5)); /* WXN, read-only through APTable */
    table(29)[0] &= ~BIT(62);
    CHECK(walk(0, 5)); /* leaf AP[2] read-only */
    table(31)[0] &= ~BIT(7);
    CHECK(!walk(0, 5)); /* writable + WXN */
    reset();
    table(31)[0] |= BIT(6);
    table(31)[0] &= ~BIT(7);
    CHECK(!walk(0, 5)); /* EL0-writable: implicit PXN */
    reset();
    table(31)[0] |= BIT(6);
    CHECK(walk(0, 0) && walk(0, 5)); /* EL0 read-only: both ELs may execute */
    table(31)[0] |= BIT(54);
    CHECK(!walk(0, 0) && walk(0, 5)); /* UXN */
    reset();
    table(31)[0] &= ~BIT(10);
    CHECK(!walk(0, 5)); /* AF = 0 */
    reset();
    table(31)[0] |= BIT(53);
    CHECK(!walk(0, 5)); /* PXN */
    /* root alignment: a 2 KiB level-0 root at +0x800 is fine; bits below the table size refuse */
    reset();
    s1.ttbr[0] |= 0x400;
    CHECK(!walk(0, 5));
    reset();
    s1.ttbr[0] |= 1; /* CnP ignored */
    CHECK(walk(0, 5));
    /* T0SZ 16: 4 KiB root, BADDR must be page aligned */
    reset();
    s1.tcr = (s1.tcr & ~63UL) | 16;
    table(TABLE0)[0] = table(TABLE0)[256];
    s1.ttbr[0] = BASE + TABLE0 * 4096UL;
    CHECK(walk(0, 5));
    s1.ttbr[0] |= 0x800;
    CHECK(!walk(0, 5));
    /* refused formats */
    reset();
    s1.sctlr |= BIT(25);
    CHECK(!walk(0, 5));
    reset();
    s1.tcr |= BIT(40);
    table(31)[0] |= BIT(51);
    CHECK(!walk(0, 5));
    reset();
    s1.tcr |= 6UL << 32;
    CHECK(!walk(0, 5));
    reset();
    s1.tcr |= BIT(14);
    CHECK(!walk(0, 5));
    reset();
    s1.tcr |= BIT(7);
    CHECK(!walk(0, 5));
    reset();
    s1.tcr |= BIT(59);
    CHECK(!walk(0, 5));
    reset();
    s1.tcr |= BIT(55);
    CHECK(!walk(0, 5));
    reset();
    table(TABLE0)[256] = 0;
    CHECK(!walk(0, 5));
    reset();
    table(29)[0] = (BASE + sizeof(mem)) | 3; /* next table outside guest RAM */
    CHECK(!walk(0, 5));
    /* HWU bits (only meaningful with HPD): accepted */
    reset();
    s1.tcr |= 0xffUL << 43;
    CHECK(walk(0, 5));
    /* MMU off: identity */
    reset();
    s1.sctlr = 0;
    u64 ipa;
    CHECK(pmu_s1_exec(&s1, BASE, 5, read_table, NULL, &ipa) && ipa == BASE);
    CHECK(!pmu_s1_exec(&s1, BASE + 1, 5, read_table, NULL, &ipa));
}

static u64 rng;
static u32 random32(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (u32)(rng >> 16);
}
static unsigned long ops;
static void differential(void)
{
    static const u32 vals[] = {NOP,        0xd53b9d01, 0xd51befc3, 0x00200100,
                               0xd53bec03, 0x87654321, BRK};
    for (unsigned seed = 0; seed < 24; seed++) {
        reset();
        rng = 0x341a20261111UL + seed;
        for (unsigned n = 0; n < 4000; n++, ops++) {
            unsigned f = random32() % 24, off = f * 4096 + (random32() % 1020) * 4;
            u32 v = vals[random32() % 7];
            switch (random32() % 9) {
                case 0:
                case 1:
                    if (executable[f])
                        execute_at(f, random32() % 1024);
                    break;
                case 2:
                    store(off, 4, v);
                    break;
                case 3:
                    store(off & ~7U, 8, (u64)v << 32 | v);
                    break;
                case 4: {
                    unsigned size = 1 + random32() % 6000;
                    if (off + size > 24 * 4096)
                        size = 24 * 4096 - off;
                    fill(off, size, v);
                    break;
                }
                case 5:
                    ticks(1 + random32() % 12);
                    break;
                case 6:
                    walker_update(f);
                    break;
                case 7:
                    if (executable[f]) { /* KD breakpoint set and restored around a hit */
                        u32 saved = mem[off / 4];
                        store(off, 4, BRK);
                        execute_at(f, (off & 4095) / 4);
                        store(off, 4, saved);
                    }
                    break;
                case 8:
                    if (random32() % 8 == 0) { /* the guest remaps an even frame code <-> data */
                        unsigned g = (random32() % 12) * 2;
                        executable[g] = !executable[g];
                        store((TABLE0 + 3) * 4096 + g * 8, 8, leaf(g));
                    }
                    break;
            }
        }
    }
}

#include "replay.h"
/* Hardware-logged PMU-looking word pairs (bugcheck-1a sessions) placed in a data frame next to an
 * executed code frame: never modified by the 4 KiB granule; the 16 KiB granule rewrites data. */
static void replay(void)
{
    unsigned n = sizeof(replay_words) / sizeof(replay_words[0]), hazards16 = 0;
    for (unsigned i = 0; i < n; i++) {
        reset();
        unsigned w = 1024 + (replay_words[i].ipa & 4095) / 4;
        if (w >= 2047)
            w = 2040;
        mem[w] = replay_words[i].word;
        mem[w + 1] = replay_words[i].next;
        memcpy(ref, mem, sizeof(mem));
        execute(0);
        ticks(10);
        execute(2);
        CHECK(mem[w] == replay_words[i].word && mem[w + 1] == replay_words[i].next);
        /* the same block in the legacy 16 KiB granule */
        hv_win_pmu_host_reset();
        memcpy(mem, ref, sizeof(mem));
        legacy16k = true;
        CHECK(hv_win_pmu_set_mode(HV_WIN_PMU_MODE_16K) == 0 &&
              hv_win_pmu_arm(BASE, sizeof(mem)) == BLOCKS);
        struct exc_info c = {.far = VA + 512, .elr = VA + 512, .esr = IESR, .spsr = 5};
        CHECK(hv_win_handle_iabort(&c));
        hazards16 += mem[w] != replay_words[i].word;
        CHECK(hv_win_pmu_stat(HV_WIN_PMU_MODIFIED_NEVER_EXECUTED) >= 1); /* frame 1's own word */
        legacy16k = false;
    }
    printf("replay: %u logged word pairs in a data frame: 4 KiB granule modified 0, 16 KiB granule "
           "modified %u\n",
           n, hazards16);
}

int main(void)
{
    frame_scanner();
    mixed();
    unproven();
    writes();
    debugger();
    walker();
    differential();
    replay();
    hv_win_pmu_host_reset();
    printf("win-pmu-4k: %lu checks, %lu differential operations, 0 failed\n", checks, ops);
    return 0;
}
