/*
 * Host harness for the EL2 framebuffer-shadow converter, second round (disp2-20261110).
 * Compiles the ACTUAL hv_win_fb.c of three trees against stubs (stub/hv/fbstub.h):
 *   -DORIG : r3.2/r4.1 legacy raster sweep (sha c5f57839...), hv_win_fb_prepare(native, flags)
 *   -DPREV : r3.2-disp (bin 053623c1) converter, the one the 2026-11-10 2x hardware run used
 *   -DPARENT: disp2 (bin ed252158), the parent of this build (same API as this tree, no ST_PRINT)
 *   (none) : this tree (disp3); run.sh also builds mutants of it with one disp3 fix reverted
 * with a simulated 1 GHz counter, a fake stage-2 write-protect table, a model of the guest's
 * combined stage-1+2 TLB (4 KiB guest pages over 16 KiB stage-2 pages, read-only entries cached,
 * optional speculative caching of the next 4 KiB page) and a memory cost model.
 *
 * Everything this program prints is a HOST-HARNESS ESTIMATE. The cost model (ns per shadow byte
 * read / scanout byte written) is calibrated so that the PREV converter costs what the hardware
 * run measured (136 us per 2x page); the read/write split is NOT known and is given as two
 * bracketing scenarios. Fault cost, TLB refill cost and prefetch probability are assumptions.
 *
 * disp3, mode "strict": an adversarial stage-2 model on top of the combined-entry TLB:
 *  - a stage-2 leaf store (hv_wp_set) is not visible to the table walker until the next DSB;
 *  - stage-2-only TLB entries per 16 KiB page: filled on every walk (permission-faulting entries
 *    included), removed by ipas2e1* / vmalls12e1*, NOT by vmalle1* or vaae1;
 *  - a stage-2 invalidation issued while that page's leaf store is still invisible is followed by
 *    a speculative walk that refills the entry from the OLD leaf (worst case the architecture allows);
 *  - an ipas2e1* operand with IPA[13:12] != 0 matches no 16 KiB entry (a core that honours those
 *    RES0 bits; the other legal behaviour, ignoring them, would hide the bug);
 *  - guest VA = IPA + va_off (a linear mapping whose offset is 4 KiB- but not 16 KiB-aligned);
 *  - at every return to the guest: no invisible leaf store, no TLBI without a later DSB, and an
 *    ISB after that DSB (sequence check).
 * It does NOT model cycles, real speculation, other CPUs, AT/HPFAR, non-linear guest mappings or
 * the EL2&0 regime; barriers are ordering events only.
 */
#include "stub/hv/fbstub.h"
#undef printf
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/wait.h>

#if !defined(ORIG) && !defined(PREV)
#define NEW 1
#endif

#ifdef ORIG
s64 hv_win_fb_prepare(u64 expected_native, u64 flags);
#else
s64 hv_win_fb_prepare(u64 expected_native, u64 flags, u64 tuning);
bool hv_win_fb_handle_dabort(struct exc_info *ctx, u64 esr);
#endif
bool hv_win_fb_start(const struct boot_args *gba);
void hv_win_fb_tick(void);
u64 hv_win_fb_stat(u64 idx);

/* ---------------- stub environment ---------------- */
struct hv_win_state hv_win = {1};
struct boot_args cur_boot_args;
u64 hv_cpus_in_guest;
bool hv_started_cpus[MAX_CPUS];
bool hv_win_vgic_enabled(void) { return true; }
u64 stub_mrs(int r) { (void)r; return 1000000000ULL; } /* 1 GHz, as recorded on T6050 */
static double sim_now;                                  /* ns */
static double rd_ns_per_b, wr_ns_per_b;                 /* cost model */
static double fault_ns = 3000;                          /* stage-2 WP fault round trip (assumed) */
static double legacy_row_ns;                            /* ORIG: cost of one legacy row */
static int in_tick __attribute__((unused));
static double tick_base __attribute__((unused));
static u64 lines_base __attribute__((unused));
static u64 n_tlbi, n_tlbi_all, n_sysop;
static int print_on;
static double print_ns;      /* disp3: modelled cost of one console line (0 = free, as before) */
static u64 n_print_lines;
static char last_line[2][512];
int stub_printf(const char *fmt, ...)
{
    va_list ap, aq;
    va_start(ap, fmt);
    va_copy(aq, ap);
    vsnprintf(last_line[n_print_lines & 1], sizeof(last_line[0]), fmt, aq);
    va_end(aq);
    n_print_lines++;
    sim_now += print_ns;
    int r = 0;
    if (print_on) {
        printf("    [EL2 console] ");
        r = vprintf(fmt, ap);
    }
    va_end(ap);
    return r;
}
void stub_cost(u64 rd, u64 wr) { sim_now += rd * rd_ns_per_b + wr * wr_ns_per_b; }
u64 get_ticks(void)
{
#ifdef ORIG
    if (in_tick)
        return (u64)(tick_base + legacy_row_ns * (double)(hv_win_fb_stat(7) - lines_base));
#endif
    return (u64)sim_now;
}
static void tlb_flush_all(void);
/* disp3 strict stage-2 model state (see header) */
static int strict;
static u64 va_off;
static unsigned char *wp_vis, *wp_pend, *s2tlb; /* walker-visible leaf RO?, store invisible?, cached S2 entry */
static u64 n_pend;
static int tlbi_open, isb_due;
static u64 order_viol, seq_viol, stale_refill, s2_op_unaligned, win_bad, win_checked;
static void s2_drain(void);
static void s2_inval_all(void);
void stub_sysop(const char *s)
{
    n_sysop++;
    if (strict && strstr(s, "dsb")) {
        s2_drain();
        if (tlbi_open) { tlbi_open = 0; isb_due = 1; }
    } else if (strict && strstr(s, "isb")) {
        isb_due = 0;
    }
    if (strstr(s, "tlbi vmalls12e1")) {
        if (strict) s2_inval_all();
        tlb_flush_all();
    } else if (strstr(s, "tlbi vmalle1")) {
        tlb_flush_all(); /* stage-1 and combined entries only: stage-2-only entries survive */
    }
    if (strict && strstr(s, "tlbi") && !strstr(s, "alle2"))
        tlbi_open = 1;
}
void dc_civac_range(void *p, size_t n) { (void)p; (void)n; }
static size_t heap_left = 256UL << 20;
size_t heapblock_remaining(void) { return heap_left; }
/* n5c extension pool as the resident reports it; nightly-6c test "pool-overlap" points it at the shadow */
static int pool_overlap;
static u8 *shadow_for_pool;
void heapblock_pool_info(u64 *base, u64 *size, u64 *used)
{
    *base = pool_overlap ? (u64)shadow_for_pool + 0x4000 : 0;
    *size = pool_overlap ? 0x4000 : 0;
    *used = 0;
}
static void *allocs[4];
static int nallocs;
void *heapblock_alloc_aligned(size_t sz, size_t al)
{
    void *p = NULL;
    if (sz > heap_left || posix_memalign(&p, al, sz))
        return NULL;
    memset(p, 0xa5, sz);
    heap_left -= sz;
    allocs[nallocs++] = p;
    return p;
}
void mmu_rm_mapping(u64 va, u64 sz) { (void)va; (void)sz; }
int mmu_add_mapping(u64 va, u64 pa, u64 sz, int a, int p) { (void)va; (void)pa; (void)sz; (void)a; (void)p; return 0; }
int hv_map_hw_nc(u64 f, u64 t, u64 s) { (void)f; (void)t; (void)s; return 0; }
/* fake stage-2 for the shadow */
static u64 wp_base, wp_pages;
static unsigned char *wp_ro;
int hv_wp_prepare(u64 ipa, u64 size)
{
    wp_base = ipa;
    wp_pages = size >> 14;
    free(wp_ro); free(wp_vis); free(wp_pend); free(s2tlb);
    wp_ro = malloc(wp_pages);
    wp_vis = malloc(wp_pages);
    wp_pend = calloc(wp_pages, 1);
    s2tlb = calloc(wp_pages, 1);
    memset(wp_ro, 1, wp_pages);
    memset(wp_vis, 1, wp_pages); /* the real hv_wp_prepare ends with dsb ishst */
    n_pend = 0;
    return (int)wp_pages;
}
int hv_wp_set(u64 ipa, bool w)
{
    if (ipa < wp_base || ((ipa - wp_base) >> 14) >= wp_pages || (ipa & 0x3fff))
        abort();
    u64 pg = (ipa - wp_base) >> 14;
    wp_ro[pg] = !w;
    if (!strict)
        wp_vis[pg] = !w;
    else if (!wp_pend[pg] && wp_vis[pg] != wp_ro[pg]) {
        wp_pend[pg] = 1; /* plain store: the walker sees it only after a DSB */
        n_pend++;
    }
    return 0;
}
static void s2_drain(void)
{
    if (!n_pend) return;
    for (u64 i = 0; i < wp_pages; i++)
        if (wp_pend[i]) { wp_pend[i] = 0; wp_vis[i] = wp_ro[i]; }
    n_pend = 0;
}
static void s2_inval_page(u64 pg)
{
    if (wp_pend[pg]) { /* TLBI overtook the leaf store: a walk refills from the old leaf */
        order_viol++;
        stale_refill++;
        s2tlb[pg] = wp_vis[pg] ? 1 : 2;
    } else {
        s2tlb[pg] = 0;
    }
}
static void s2_inval_all(void) { for (u64 i = 0; i < wp_pages; i++) s2_inval_page(i); }
static unsigned char s2_walk(u64 pg) /* 1 = RO, 2 = RW */
{
    if (!strict) return wp_ro[pg] ? 1 : 2;
    if (!s2tlb[pg]) s2tlb[pg] = wp_vis[pg] ? 1 : 2;
    return s2tlb[pg];
}
static void seq_check(void) /* at every return to the guest */
{
    if (strict && (n_pend || tlbi_open || isb_due)) seq_viol++;
}
bool hv_xt_fault_ipa(struct exc_info *c, bool s1, bool w, u64 *ipa) { (void)s1; (void)w; *ipa = c->far - va_off; return true; }

/* ---- guest TLB model: combined entries per 4 KiB guest page of the shadow (VA == IPA here) ----
 * 0 = none, 1 = read-only (a walk while stage 2 was RO: such entries may be cached), 2 = RW.
 * Stage-1-only invalidation by VA or a VMID-wide flush removes combined entries; invalidation by
 * IPA alone does not. p_pf: on a walk, probability that the core also caches the next 4 KiB page
 * (TLB prefetch / younger speculative stores), with that page's stage-2 permission at that time. */
static unsigned char *tlb;
static u64 tlb_n;
static double p_pf;
static u64 stale_rw; /* store that went through on a cached RW entry while stage 2 was RO: lost update */
static void tlb_flush_all(void) { if (tlb) memset(tlb, 0, tlb_n); }
void hv_xt_tlbi(bool b) /* dsb ishst; tlbi vmalls12e1(is); dsb; isb */
{
    (void)b;
    n_tlbi_all++;
    if (strict) { s2_drain(); s2_inval_all(); }
    tlb_flush_all();
}
static u64 log_vaae1[8], log_ipas2;
static int n_log_vaae1, n_log_ipas2;
void stub_tlbi(const char *op, u64 v)
{
    n_tlbi++;
    if (strict) tlbi_open = 1;
    if (tlb && !strcmp(op, "vaae1")) {
        if (n_log_vaae1 < 8) log_vaae1[n_log_vaae1] = v;
        n_log_vaae1++;
        u64 base = (wp_base + va_off) >> 12; /* stage-1 entries are per guest VA page */
        if (v >= base && v - base < tlb_n)
            tlb[v - base] = 0;
    }
    if (strict && !strncmp(op, "ipas2e1", 7)) {
        log_ipas2 = v;
        n_log_ipas2++;
        u64 base = wp_base >> 12;
        if (v & 3)
            s2_op_unaligned++; /* IPA[13:12] set: matches no 16 KiB stage-2 entry (see header) */
        else if (v >= base && ((v - base) >> 2) < wp_pages)
            s2_inval_page((v - base) >> 2);
    }
}
static u64 rng = 0x9e3779b97f4a7c15ULL;
static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (u32)rng; }
static double rnd01(void) { return (rnd() >> 8) / 16777216.0; }

/* ---------------- reference model ---------------- */
static inline u32 ref_px(u32 p) /* the legacy loop's formula, verbatim semantics */
{
    u32 b = p & 0xff, g = (p >> 8) & 0xff, r = (p >> 16) & 0xff;
    return ((b << 2) | (b >> 6)) | (((g << 2) | (g >> 6)) << 10) | (((r << 2) | (r >> 6)) << 20);
}

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------------- device model ---------------- */
static u32 W, H, S, GW, GH, GS, SCALE = 1;
static u8 *native, *shadow;
static u64 nsize;
static u64 flags_used;
static double next_tick;
static u64 faults_seen, guest_faults;
static u64 tlbi_mode_used;

static int setup(u32 w, u32 h, u32 stride, u64 flags, u64 tuning)
{
    W = w; H = h; S = stride;
    nsize = ALIGN_UP((u64)S * H, 0x4000);
    void *p = NULL;
    if (posix_memalign(&p, 0x4000, nsize))
        abort();
    native = p;
    memset(native, 0x5a, nsize); /* sentinel: padding must survive */
    memset(&cur_boot_args, 0, sizeof(cur_boot_args));
    cur_boot_args.video.base = (u64)native;
    cur_boot_args.video.stride = S;
    cur_boot_args.video.width = W;
    cur_boot_args.video.height = H;
    cur_boot_args.video.depth = 30 | 0x10000; /* retina flag above the low byte */
    sim_now = 0;
    flags_used = flags;
    tlbi_mode_used = (tuning >> 40) & 3;
#ifdef ORIG
    (void)tuning;
    s64 r = hv_win_fb_prepare((u64)native, flags);
    GW = W; GH = H; GS = S; SCALE = 1;
#else
    s64 r = hv_win_fb_prepare((u64)native, flags, tuning);
    GW = (u32)hv_win_fb_stat(26); GH = (u32)hv_win_fb_stat(27); GS = (u32)hv_win_fb_stat(28);
    SCALE = (flags & 4) ? 2 : 1;
#endif
    if (r <= 0)
        return (int)r;
    shadow = (u8 *)r;
    shadow_for_pool = shadow;
    struct boot_args g = cur_boot_args;
    g.video.base = (u64)shadow;
    g.video.width = GW; g.video.height = GH; g.video.stride = GS;
    g.video.depth = (cur_boot_args.video.depth & ~0xffULL) | 32;
    if (!hv_win_fb_start(&g))
        return -100;
    tlb_n = ALIGN_UP((u64)GS * GH, 0x4000) >> 12;
    tlb = calloc(tlb_n, 1);
    next_tick = sim_now + 1e6;
    return 1;
}

static void teardown(void)
{
    free(native);
    for (int i = 0; i < nallocs; i++) free(allocs[i]);
    nallocs = 0;
    free(tlb); tlb = NULL;
    native = shadow = NULL;
}

/* guest store of one 32-bit pixel through the TLB model; takes stage-2 faults as needed */
static inline void guest_store(u32 x, u32 y, u32 v)
{
    u64 a = (u64)shadow + (u64)y * GS + (u64)x * 4;
#ifndef ORIG
    if (flags_used & 2) {
        u64 vp = (a - wp_base) >> 12, pg = vp >> 2;
        for (int tries = 0;; tries++) {
            unsigned char e = tlb[vp];
            if (!e) {
                e = tlb[vp] = s2_walk(pg);
                if (p_pf > 0 && vp + 1 < tlb_n && !tlb[vp + 1] && rnd01() < p_pf)
                    tlb[vp + 1] = s2_walk((vp + 1) >> 2);
            }
            if (e == 2) {
                if (wp_ro[pg])
                    stale_rw++; /* the converter would miss this store: lost update */
                break;
            }
            /* read-only entry: permission fault (real if stage 2 is RO, spurious otherwise) */
            struct exc_info c = {a + va_off, 0x0c | BIT(6), 0};
            u64 f_before = strict ? hv_win_fb_stat(15) : 0;
            n_log_vaae1 = n_log_ipas2 = 0;
            bool ok = hv_win_fb_handle_dabort(&c, c.esr);
            seq_check();
            if (strict && ok && hv_win_fb_stat(15) != f_before) {
                /* a real fault: the fault-path TLBIs must name exactly the faulting 16 KiB IPA
                 * page (aligned) and, in sub mode, exactly its four 4 KiB VAs */
                u64 pipa = wp_base + (pg << 14), bad = n_log_ipas2 != 1 || log_ipas2 != pipa >> 12;
                if (tlbi_mode_used == 0) {
                    u64 seen = 0;
                    bad |= n_log_vaae1 != 4;
                    for (int k = 0; k < n_log_vaae1 && k < 8; k++) {
                        u64 d = log_vaae1[k] - ((pipa + va_off) >> 12);
                        if (d < 4) seen |= 1u << d; else bad = 1;
                    }
                    bad |= seen != 0xf;
                }
                win_checked++;
                win_bad += bad != 0;
            }
            if (!ok || tries > 6) {
                printf("FATAL: fault not handled (ok=%d tries=%d)\n", ok, tries);
                exit(2);
            }
            sim_now += fault_ns;
            guest_faults++;
            if (!tries)
                faults_seen++;
        }
    }
#endif
    *(u32 *)a = v;
}

static void guest_rect(u32 x, u32 y, u32 w, u32 h, u32 v)
{
    for (u32 j = y; j < y + h && j < GH; j++)
        for (u32 i = x; i < x + w && i < GW; i++)
            guest_store(i, j, v);
    /* the guest's own drawing time: BDD blit ~ memcpy at ~4 GB/s into NC */
    sim_now += (double)w * h * 4 / 4.0;
}

static double busy_ns;
static void do_tick(void)
{
    double t0 = sim_now;
#ifdef ORIG
    in_tick = 1;
    tick_base = sim_now;
    lines_base = hv_win_fb_stat(7);
    hv_win_fb_tick();
    sim_now = tick_base + legacy_row_ns * (double)(hv_win_fb_stat(7) - lines_base);
    in_tick = 0;
#else
    hv_win_fb_tick();
#endif
    seq_check();
    busy_ns += sim_now - t0;
    next_tick = sim_now + 1e6; /* hv_arm_tick re-arms 1 ms after the tick ends */
}

static int native_ok(u32 gx, u32 gy)
{
    u32 e = ref_px(*(u32 *)(shadow + (u64)gy * GS + gx * 4));
    for (u32 dy = 0; dy < SCALE; dy++)
        for (u32 dx = 0; dx < SCALE; dx++)
            if (*(u32 *)(native + (u64)(gy * SCALE + dy) * S + (gx * SCALE + dx) * 4) != e)
                return 0;
    return 1;
}

static u64 verify_full(void)
{
    u64 bad = 0;
    for (u32 y = 0; y < GH; y++)
        for (u32 x = 0; x < GW; x++)
            bad += !native_ok(x, y);
    for (u32 y = 0; y < H; y++) /* padding untouched */
        for (u32 b = W * 4; b < S; b++)
            bad += native[(u64)y * S + b] != 0x5a;
    return bad;
}

static void run_until_idle(double max_ns)
{
    double end = sim_now + max_ns;
    u64 last __attribute__((unused)) = ~0ULL;
    int still __attribute__((unused)) = 0;
    while (sim_now < end) {
        sim_now = next_tick;
        do_tick();
#ifndef ORIG
        u64 pd = hv_win_fb_stat(17);
        still = pd == last ? still + 1 : 0;
        last = pd;
        if ((flags_used & 2) && hv_win_fb_stat(16) && still >= 30)
            return;
#endif
    }
}

/* PREPARE latches "attempted" (one per boot), so every scenario runs in a forked child. */
#define CHILD(stmt) do { fflush(stdout); pid_t _p = fork(); if (!_p) { stmt; fflush(stdout); _exit(failures != 0); } \
    int _st = 0; waitpid(_p, &_st, 0); if (!WIFEXITED(_st) || WEXITSTATUS(_st)) { failures++; printf("child failed (status %d)\n", _st); } } while (0)

/* ---------------- tests ---------------- */
#ifndef ORIG
#include "hv_win_fb_conv.h"
static void test_colour(void)
{
    u64 bad = 0;
    for (u32 c = 0; c < (1u << 24); c++) {
        u32 p = c | (rnd() & 0xff000000u);
        if (fb_px1(p) != ref_px(p))
            bad++;
    }
    for (int i = 0; i < 1000000; i++) {
        u64 q = ((u64)rnd() << 32) | rnd();
        u64 o = fb_px2(q);
        if ((u32)o != ref_px((u32)q) || (u32)(o >> 32) != ref_px((u32)(q >> 32)))
            bad++;
    }
    CHECK(bad == 0, "colour expansion mismatches: %llu", (unsigned long long)bad);
    static u32 src[5000], dst[5000], dst2a[10000], dst2b[10000];
    for (int t = 0; t < 2000; t++) {
        u32 off = rnd() % 8, n = rnd() % 4000;
        for (u32 i = 0; i < n + off; i++) src[i] = rnd();
        memset(dst, 0, sizeof(dst));
        fb_conv_span(dst + off, src + off, n);
        for (u32 i = 0; i < n; i++) if (dst[off + i] != ref_px(src[off + i])) { bad++; break; }
        u32 n2 = n / 2 ? n / 2 : 1;
        fb_conv_span_2x(dst2a + 2 * off, dst2b + 2 * off, src + off, n2);
        for (u32 i = 0; i < n2; i++) {
            u32 e = ref_px(src[off + i]);
            if (dst2a[2 * off + 2 * i] != e || dst2a[2 * off + 2 * i + 1] != e || dst2b[2 * off + 2 * i] != e ||
                dst2b[2 * off + 2 * i + 1] != e) { bad++; break; }
        }
    }
    CHECK(bad == 0, "span mismatches: %llu", (unsigned long long)bad);
    printf("colour: 2^24 values + 1M pairs + 2000 spans/2x spans bit-exact vs legacy formula\n");
#ifdef NEW
    /* diff spans: after any sequence of partial source changes, dst == conversion of src, and
     * chunks without a change are not written (sentinel survives) */
    static u32 copy[5000], d1[5000], e0[10000], e1[10000], cp2[5000];
    u64 badd = 0, ex = 0, wr = 0, unchanged_written = 0;
    for (int t = 0; t < 3000; t++) {
        u32 off = (rnd() % 2) * 2, n = 1 + rnd() % 3000;
        for (u32 i = 0; i < n + off; i++) src[i] = rnd();
        fb_conv_span_diff(d1 + off, src + off, copy + off, n, 1, &ex);
        fb_conv_span_2x_diff(e0 + 2 * off, e1 + 2 * off, src + off, cp2 + off, n, 1, &ex);
        for (int round = 0; round < 4; round++) {
            u32 a = rnd() % n, b = a + rnd() % (n - a + 1);
            for (u32 i = a; i < b; i++) src[off + i] = rnd();
            /* poison one untouched chunk's dst; it must stay poisoned (proves no rewrite) */
            u32 pc = ((off & 7) == 0 && n >= 32) ? (rnd() % (n / 16)) * 16 : 0xffffffff;
            int poison = pc != 0xffffffff && (pc + 16 <= a || pc >= b);
            if (poison) d1[off + pc] = 0xdeadbeef;
            wr += fb_conv_span_diff(d1 + off, src + off, copy + off, n, 0, &ex);
            fb_conv_span_2x_diff(e0 + 2 * off, e1 + 2 * off, src + off, cp2 + off, n, 0, &ex);
            for (u32 i = 0; i < n; i++) {
                if (poison && i == pc) { unchanged_written += d1[off + i] != 0xdeadbeef; d1[off + i] = ref_px(src[off + i]); continue; }
                u32 e = ref_px(src[off + i]);
                if (d1[off + i] != e) { badd++; break; }
                if (e0[2 * (off + i)] != e || e0[2 * (off + i) + 1] != e || e1[2 * (off + i)] != e || e1[2 * (off + i) + 1] != e) { badd++; break; }
            }
        }
    }
    CHECK(badd == 0 && unchanged_written == 0, "diff span mismatches %llu, unchanged chunks rewritten %llu",
          (unsigned long long)badd, (unsigned long long)unchanged_written);
    printf("diff spans (1x and 2x): 3000 x 4 random partial updates bit-exact, unchanged chunks never rewritten "
           "(%llu chunks examined, %llu written)\n", (unsigned long long)ex, (unsigned long long)wr);
#endif
}
#endif

static void test_full_frame(const char *name, u32 w, u32 h, u32 s, u64 flags)
{
    int r = setup(w, h, s, flags, 0);
    CHECK(r == 1, "%s: setup %d", name, r);
    if (r != 1) return;
    for (u32 y = 0; y < GH; y++)
        for (u32 x = 0; x < GW; x++)
            *(u32 *)(shadow + (u64)y * GS + x * 4) = rnd(); /* pre-start content, no faults */
    double t0 = sim_now;
    u64 bad = ~0ULL;
    while (sim_now - t0 < 20e9) {
        sim_now = next_tick;
        do_tick();
#ifdef ORIG
        if (hv_win_fb_stat(9) == 0 && hv_win_fb_stat(7) >= GH) { bad = verify_full(); break; }
#else
        if (hv_win_fb_stat(16) >= 1) { bad = verify_full(); break; }
#endif
    }
    CHECK(bad == 0, "%s: %llu bad pixels/padding bytes after first full pass", name, (unsigned long long)bad);
    printf("full-frame %-28s %ux%u stride %u: first complete frame at %.1f ms, bad=%llu\n", name, GW, GH, GS,
           (sim_now - t0) / 1e6, (unsigned long long)bad);
    teardown();
}

#ifndef ORIG
static void test_random_dirty(u64 flags, u64 tuning, double pf)
{
    p_pf = pf;
    int r = setup(1000, 600, 4160, flags, tuning);
    CHECK(r == 1, "setup %d", r);
    run_until_idle(5e9);
    for (int ev = 0; ev < 3000; ev++) {
        u32 x = rnd() % GW, y = rnd() % GH, w = 1 + rnd() % 200, h = 1 + rnd() % 100;
        guest_rect(x, y, w, h, rnd());
        if (rnd() % 3 == 0) { sim_now = sim_now > next_tick ? sim_now : next_tick; do_tick(); }
    }
    run_until_idle(10e9);
    u64 bad = verify_full();
    CHECK(bad == 0 && stale_rw == 0, "random dirty (flags %llx tuning %llx pf %.2f): %llu stale pixels, %llu stores "
          "through a stale RW entry", (unsigned long long)flags, (unsigned long long)tuning, pf,
          (unsigned long long)bad, (unsigned long long)stale_rw);
    printf("random-dirty flags=%llx tuning=%#llx pf=%.2f: 3000 rects, partial passes; faults %llu spurious %llu "
           "passes %llu", (unsigned long long)flags, (unsigned long long)tuning, pf,
           (unsigned long long)hv_win_fb_stat(15), (unsigned long long)hv_win_fb_stat(32),
           (unsigned long long)hv_win_fb_stat(16));
#ifdef NEW
    printf(" merged %llu", (unsigned long long)hv_win_fb_stat(69));
#endif
    printf(", stale after idle = %llu, lost-update stores = %llu\n", (unsigned long long)bad,
           (unsigned long long)stale_rw);
    teardown();
}

#ifdef NEW
/*
 * nightly-6-perf background refresh (tuning bits 47:44). Stores that reach the shadow WITHOUT a
 * stage-2 fault (written straight into shadow memory, bypassing guest_store's TLB/fault model) are
 * a missed update: with the refresh they must reach the scanout within one period plus one round
 * and be counted in stat 79; without it (negative control) they must stay stale. A corrupted
 * scanout (written behind the converter's back) must be repaired by the forced round (every 4th).
 */
static void run_for(double ns)
{
    double end = sim_now + ns;
    while (sim_now < end) {
        sim_now = next_tick;
        do_tick();
    }
}

static void test_refresh(u64 tuning, int refresh_on)
{
    int r = setup(1000, 600, 4160, 2, tuning);
    CHECK(r == 1, "refresh setup %d", r);
    if (r != 1) return;
    run_until_idle(5e9);
    for (int ev = 0; ev < 200; ev++) /* ordinary tracked drawing first */
        guest_rect(rnd() % GW, rnd() % GH, 1 + rnd() % 100, 1 + rnd() % 50, rnd());
    run_until_idle(10e9);
    u64 fixes0 = hv_win_fb_stat(79);
    for (u32 y = 0; y < 300; y++) /* the top half changes without any fault */
        for (u32 x = 0; x < GW; x++)
            *(u32 *)(shadow + (u64)y * GS + x * 4) = 0x00101010u ^ rnd();
    run_for(3e9); /* period 0.5..1 s + a round */
    u64 bad = verify_full(), fixes = hv_win_fb_stat(79) - fixes0;
    if (refresh_on) {
        CHECK(bad == 0, "refresh %#llx: %llu stale pixels after missed stores", (unsigned long long)tuning,
              (unsigned long long)bad);
        CHECK(fixes > 0 && hv_win_fb_stat(82) > 0, "refresh: missed updates counted (%llu chunks)",
              (unsigned long long)fixes);
        /* the scanout corrupted behind the converter's back: only a forced round repairs it */
        for (u32 y = 400; y < 410; y++)
            for (u32 x = 0; x < W * 4; x += 4)
                *(u32 *)(native + (u64)y * S + x) = 0xdeadbeefu;
        run_for(12e9); /* > 4 rounds */
        u64 bad2 = verify_full();
        CHECK(bad2 == 0, "refresh: %llu bad pixels after a corrupted scanout and 4+ rounds", (unsigned long long)bad2);
        CHECK(hv_win_fb_stat(78) >= 4, "refresh rounds %llu", (unsigned long long)hv_win_fb_stat(78));
        printf("refresh tuning=%#llx: missed-store pixels healed (fixes %llu chunks, %llu pages), rounds %llu, "
               "scanout corruption repaired, stale %llu/%llu\n", (unsigned long long)tuning,
               (unsigned long long)fixes, (unsigned long long)hv_win_fb_stat(82),
               (unsigned long long)hv_win_fb_stat(78), (unsigned long long)bad, (unsigned long long)bad2);
    } else {
        CHECK(bad > 0 && fixes == 0 && hv_win_fb_stat(78) == 0,
              "negative control: without refresh the missed stores must stay stale (%llu)", (unsigned long long)bad);
        printf("refresh off (control): %llu stale pixels stay stale, as the old converter\n", (unsigned long long)bad);
    }
    teardown();
}
#endif

static void test_scale2_checker(void)
{
    int r = setup(3456, 2234, 13824, 4 | 2, 0);
    CHECK(r == 1 && GW == 1728 && GH == 1117, "scale2 geometry %ux%u", GW, GH);
    for (u32 y = 0; y < GH; y++)
        for (u32 x = 0; x < GW; x++)
            guest_store(x, y, ((x ^ y) & 1) ? 0x00ffffff : 0);
    run_until_idle(20e9);
    u64 bad = 0;
    for (u32 y = 0; y < H; y++)
        for (u32 x = 0; x < W; x++) {
            u32 e = (((x / 2) ^ (y / 2)) & 1) ? 0x3fffffff : 0;
            bad += *(u32 *)(native + (u64)y * S + x * 4) != e;
        }
    CHECK(bad == 0, "scale2 checker: %llu bad native pixels", (unsigned long long)bad);
    printf("scale2: 1-px guest checkerboard -> exact 2x2 native blocks, bad=%llu\n", (unsigned long long)bad);
    teardown();
}
#endif

#ifdef NEW
static void test_stat_reset(void)
{
    p_pf = 0;
    int r = setup(1000, 600, 4160, 2, 0);
    CHECK(r == 1, "setup %d", r);
    run_until_idle(2e9);
    guest_rect(0, 0, 100, 100, 1);
    sim_now += 300e6; /* a 300 ms hold of the only CPU at EL2 (e.g. a host pause): no tick */
    sim_now = next_tick > sim_now ? next_tick : sim_now;
    do_tick();
    run_until_idle(2e9);
    u64 gap = hv_win_fb_stat(51), gross = hv_win_fb_stat(38), net = hv_win_fb_stat(37), stop = hv_win_fb_stat(56);
    u64 g2 = hv_win_fb_stat(51 | 0x100), g3 = hv_win_fb_stat(51), n2 = hv_win_fb_stat(37 | 0x100), n3 = hv_win_fb_stat(37);
    CHECK(gap >= 300000000ULL && gross >= 300000000ULL && net < 20000000ULL && stop >= 290000000ULL,
          "pause accounting: gap %llu gross %llu net %llu stopped %llu", (unsigned long long)gap,
          (unsigned long long)gross, (unsigned long long)net, (unsigned long long)stop);
    CHECK(g2 == gap && g3 == 0 && n2 == net && n3 == 0, "read-reset: %llu %llu %llu %llu", (unsigned long long)g2,
          (unsigned long long)g3, (unsigned long long)n2, (unsigned long long)n3);
    printf("pause accounting: 300 ms EL2 hold -> gap %.1f ms, pass latency gross %.1f ms / net %.1f ms, stopped %.1f ms; "
           "read-reset clears gap and net max\n", gap / 1e6, gross / 1e6, net / 1e6, stop / 1e6);
    teardown();
}
#endif

#ifdef NEW
/* ---- disp3 tests ---- */
static void strict_report(const char *name, u64 bad, int want_no_spurious)
{
    u64 sp = hv_win_fb_stat(32);
    CHECK(bad == 0 && stale_rw == 0 && order_viol == 0 && seq_viol == 0 && s2_op_unaligned == 0 && win_bad == 0 &&
          (!want_no_spurious || sp == 0),
          "%s: stale pixels %llu, untracked stores %llu, TLBI-before-leaf-store %llu (stale S2 refills %llu), "
          "sequence violations %llu, unaligned ipas2e1 %llu, fault-path TLBI operand mismatches %llu/%llu, spurious %llu",
          name, (unsigned long long)bad, (unsigned long long)stale_rw, (unsigned long long)order_viol,
          (unsigned long long)stale_refill, (unsigned long long)seq_viol, (unsigned long long)s2_op_unaligned,
          (unsigned long long)win_bad, (unsigned long long)win_checked, (unsigned long long)sp);
    printf("strict %-34s faults %llu spurious %llu passes %llu | stale %llu untracked %llu order %llu seq %llu "
           "unaligned %llu operand-bad %llu/%llu\n", name, (unsigned long long)hv_win_fb_stat(15),
           (unsigned long long)sp, (unsigned long long)hv_win_fb_stat(16), (unsigned long long)bad,
           (unsigned long long)stale_rw, (unsigned long long)order_viol, (unsigned long long)seq_viol,
           (unsigned long long)s2_op_unaligned, (unsigned long long)win_bad, (unsigned long long)win_checked);
}

/* M1: small-pass re-protect. Dirty a few pages, let the pass re-protect them (<= 32 pages: the
 * per-IPA path), then store to the same pages again: every second store must fault and be shown. */
static void test_strict_reprotect(const char *name, u64 flags, u64 tuning)
{
    strict = 1; va_off = 0x5000; p_pf = 0;
    int r = setup(1000, 600, 4160, flags, tuning);
    CHECK(r == 1, "setup %d", r);
    run_until_idle(5e9);
    for (int it = 0; it < 300; it++) {
        u32 x = rnd() % (GW - 8), y = rnd() % (GH - 12), v = rnd();
        guest_rect(x, y, 8, 12, v);           /* 12 rows x 4160 B: 3-5 x 16 KiB pages, 4 KiB sub-pages vary */
        run_until_idle(1e9);                  /* pass converts and re-protects them (small pass) */
        guest_rect(x, y, 8, 12, v ^ 0x00ffffff); /* must fault again */
        if (it % 4 == 0) run_until_idle(1e9);
    }
    run_until_idle(5e9);
    strict_report(name, verify_full(), tlbi_mode_used != 2);
    teardown();
}

static void test_strict_random(const char *name, u64 flags, u64 tuning, double pf)
{
    strict = 1; va_off = 0x5000; p_pf = pf;
    int r = setup(1000, 600, 4160, flags, tuning);
    CHECK(r == 1, "setup %d", r);
    run_until_idle(5e9);
    for (int ev = 0; ev < 3000; ev++) {
        u32 x = rnd() % GW, y = rnd() % GH, w = 1 + rnd() % 200, h = 1 + rnd() % 100;
        guest_rect(x, y, w, h, rnd());
        if (rnd() % 3 == 0) { sim_now = sim_now > next_tick ? sim_now : next_tick; do_tick(); }
    }
    run_until_idle(10e9);
    strict_report(name, verify_full(), tlbi_mode_used != 2);
    teardown();
}

/* L2: the time spent printing FBS/FBS2 lines is busy time, its own counter (stat 77, print_us) and
 * not a gap. Model: 40 us per console line, stats every 1 s, idle guest (ticks do no other work). */
static void test_print_accounting(void)
{
    p_pf = 0;
    int r = setup(1000, 600, 4160, 2, 1ULL << 48);
    CHECK(r == 1, "setup %d", r);
    run_until_idle(3e9);
    print_ns = 40000;
    hv_win_fb_stat(51 | 0x100);
    hv_win_fb_stat(76 | 0x100);
    u64 b0 = hv_win_fb_stat(30), p0 = hv_win_fb_stat(77), l0 = n_print_lines;
    double t_end = sim_now + 5.5e9;
    while (sim_now < t_end) { sim_now = next_tick; do_tick(); }
    u64 lines = n_print_lines - l0, db = hv_win_fb_stat(30) - b0, dp = hv_win_fb_stat(77) - p0;
    u64 tick = hv_win_fb_stat(76), gap = hv_win_fb_stat(51), want = lines / 2 * 80000;
    /* the latest FBS line reports print_us cumulative up to the previous print */
    const char *fbs = strstr(last_line[0], "FBS ") ? last_line[0] : last_line[1];
    const char *pu = strstr(fbs, "print_us=");
    u64 shown = pu ? strtoull(pu + 9, NULL, 10) : ~0ULL;
    u64 now77 = hv_win_fb_stat(77);
    CHECK(lines >= 10 && dp == want && db == want && tick >= 80000 && gap <= 1000000 + 1000 &&
          shown == (now77 - 80000) / 1000,
          "print accounting: %llu lines, d_print %llu d_busy %llu (want %llu), tick_max %llu, gap_max %llu, "
          "FBS print_us %llu (want %llu)", (unsigned long long)lines, (unsigned long long)dp,
          (unsigned long long)db, (unsigned long long)want, (unsigned long long)tick, (unsigned long long)gap,
          (unsigned long long)shown, (unsigned long long)((now77 - 80000) / 1000));
    printf("print accounting: %llu console lines at 40 us -> print %.0f us, busy +%.0f us, tick_max %.0f us, "
           "gap_max %.3f ms, FBS print_us=%llu\n", (unsigned long long)lines, dp / 1e3, db / 1e3, tick / 1e3,
           gap / 1e6, (unsigned long long)shown);
    teardown();
}
#endif

/* ---------------- workloads ---------------- */
struct ev { double t; u32 gx, gy; u32 v; int live; };
static struct ev evs[400000];
static int nev;

static u32 uniq;
static void mark(u32 gx, u32 gy)
{
    if (nev < (int)(sizeof(evs) / sizeof(evs[0]))) {
        u32 v = *(u32 *)(shadow + (u64)gy * GS + gx * 4);
        evs[nev++] = (struct ev){sim_now, gx, gy, v, 1};
    }
}
static u32 next_colour(void) { uniq = (uniq + 0x010203) & 0x00ffffff; if (!uniq) uniq = 1; return uniq; }

struct lat { double max, sum; int n, lost, superseded; double p50, p95, p99; };
static double lats[400000];
static int cmpd(const void *a, const void *b) { double x = *(double *)a, y = *(double *)b; return x < y ? -1 : x > y; }
static void check_events(struct lat *L)
{
    for (int i = 0; i < nev; i++) {
        if (!evs[i].live) continue;
        u32 cur = *(u32 *)(shadow + (u64)evs[i].gy * GS + evs[i].gx * 4);
        if (cur != evs[i].v) { evs[i].live = 0; L->superseded++; continue; } /* overwritten before it was ever shown */
        u32 e = ref_px(cur);
        if (*(u32 *)(native + (u64)evs[i].gy * SCALE * S + (u64)evs[i].gx * SCALE * 4) == e) {
            double l = sim_now - evs[i].t;
            lats[L->n++] = l;
            L->sum += l;
            if (l > L->max) L->max = l;
            evs[i].live = 0;
        }
    }
}
static void finish_lat(struct lat *L)
{
    for (int i = 0; i < nev; i++) L->lost += evs[i].live;
    if (L->n) {
        qsort(lats, L->n, sizeof(double), cmpd);
        L->p50 = lats[L->n / 2];
        L->p95 = lats[(int)(L->n * 0.95)];
        L->p99 = lats[(int)(L->n * 0.99)];
    }
}
static void drain(struct lat *L)
{
    double drain_end = sim_now + 10e9;
    while (sim_now < drain_end) {
        sim_now = next_tick;
        do_tick();
        check_events(L);
        int live = 0;
        for (int i = 0; i < nev; i++) live += evs[i].live;
        if (!live) break;
    }
}
/* advance simulated time to t, running every tick that falls due on the way */
static void run_to(double t, struct lat *L)
{
    while (next_tick <= t) {
        if (sim_now < next_tick) sim_now = next_tick;
        do_tick();
        check_events(L);
    }
    if (sim_now < t) sim_now = t;
}

/* scenario: content drawn in native-pixel units, scaled to guest units */
static void workload(const char *wl, double dur_ns, struct lat *L)
{
    memset(L, 0, sizeof(*L));
    nev = 0;
    double t_end = sim_now + dur_ns, last_ev = 0;
    u32 sc = SCALE;
    int step = 0;
    double period = 0;
    if (!strcmp(wl, "idle")) period = 1e12;
    else if (!strcmp(wl, "typing")) period = 50e6;      /* 20 glyphs/s */
    else if (!strcmp(wl, "cursor")) period = 8e6;       /* 125 Hz mouse */
    else if (!strcmp(wl, "drag")) period = 16.7e6;      /* window drag at 60 Hz */
    else if (!strcmp(wl, "scroll")) period = 16.7e6;    /* full-screen redraw at 60 Hz */
    while (sim_now < t_end) {
        if (sim_now >= last_ev + period) {
            last_ev = sim_now;
            u32 c = next_colour();
            if (!strcmp(wl, "typing")) {
                u32 gw = 16 / sc, gh = 32 / sc, cols = (W / 16), x = (step % cols) * gw, y = (200 + (step / cols) * 32) / sc;
                guest_rect(x, y, gw, gh, c);
                mark(x + gw - 1, y + gh - 1);
            } else if (!strcmp(wl, "cursor")) {
                u32 sz = 64 / sc, x = (100 + step * 80) % (W - 64) / sc, y = (300 + (step * 80) / (W - 64) * 80) % (H - 64) / sc;
                guest_rect(x, y, sz, sz, c);
                mark(x + sz - 1, y + sz - 1);
            } else if (!strcmp(wl, "drag")) {
                u32 ww = 1200 / sc, hh = 800 / sc, x = (100 + step * 6) % (W - 1200) / sc, y = (200 + step * 3) % (H - 800) / sc;
                guest_rect(x, y, ww, hh, c);
                mark(x + ww - 1, y + hh - 1);
                mark(x, y);
            } else if (!strcmp(wl, "scroll")) {
                guest_rect(0, 0, GW, GH, c);
                mark(GW - 1, GH - 1);
                mark(0, 0);
                mark(GW / 2, GH / 2);
            }
            step++;
        }
        if (sim_now < next_tick) {
            double nt = next_tick < last_ev + period ? next_tick : last_ev + period;
            if (nt > sim_now) sim_now = nt;
        }
        if (sim_now >= next_tick) {
            do_tick();
            check_events(L);
        }
    }
    drain(L);
    finish_lat(L);
}

/*
 * The m5w test card, mode 1, as testcard.c draws it (display/improve-20261109/testcard):
 * every frame (Sleep(15) + draw) SetDIBitsToDevice re-copies the full-width band of rows
 * H*3/4-40 .. +H/4-60 (only a 24-px bar moving W per 4 s actually changes), then six opaque
 * TextOut counters ("t=<ms>ms  f=<frame>") at rows 170, H/2+40, H-H/28-60, left and right.
 * The blit is drawn in 16-row slices so EL2 ticks interleave with it as on hardware.
 * pause_at/pause_ns: one host pause (guest and EL2 tick both stopped), as an FBSTAT read does.
 */
static u32 card_bg(u32 x, u32 bx) { return (x >= bx && x < bx + 24) ? 0x00ffffff : 0x00181818; }
static void glyph_cell(u32 x0, u32 y0, u32 cw, u32 ch, u32 digit, struct lat *L, int markit)
{
    (void)L;
    for (u32 y = y0; y < y0 + ch && y < GH; y++)
        for (u32 x = x0; x < x0 + cw && x < GW; x++) {
            /* a crude glyph: digit-dependent stroke pattern, white on black (opaque TextOut) */
            u32 on = ((x - x0) * 7 + (y - y0) * (3 + digit)) % 11 < 3 + (digit % 4);
            guest_store(x, y, on ? 0x00ffffff : 0);
        }
    sim_now += (double)cw * ch; /* GDI text: ~1 ns/px */
    if (markit)
        mark(x0 + cw / 2, y0 + ch / 2);
}
struct card_out { double cpu, faults_s, spur_pct, passes_s, pages_pass, fps; u64 chunks, chunks_wr; };
static void card(double dur_ns, double pause_at, double pause_ns, struct lat *L, struct card_out *O)
{
    memset(L, 0, sizeof(*L));
    nev = 0;
    u32 by = GH * 3 / 4 - 40, bh = GH / 4 - 60, lastx = 0;
    u32 fh = GH / 28, cw = fh * 11 / 20, ys[3] = {170, GH / 2 + 40, GH - GH / 28 - 60}, xs[2] = {60, GW - GW / 3};
    double t0 = sim_now, t_end = sim_now + dur_ns;
    int paused = pause_ns <= 0;
    u64 frame = 0;
    u64 f0 = hv_win_fb_stat(15), s0 = hv_win_fb_stat(32), p0 = hv_win_fb_stat(16), g0 = hv_win_fb_stat(17);
#ifdef NEW
    u64 c0 = hv_win_fb_stat(64), w0 = hv_win_fb_stat(65);
#endif
    double busy0 = busy_ns, gf0 = (double)guest_faults;
    while (sim_now < t_end) {
        if (!paused && sim_now - t0 >= pause_at) {
            paused = 1;
            sim_now += pause_ns; /* guest and the EL2 tick both stopped (proxy round trip) */
            next_tick = sim_now;
        }
        u64 ms = (u64)((sim_now - t0) / 1e6);
        u32 x = (u32)((ms * GW / 4000) % GW);
        /* band blit: whole rows, in slices, ticks interleaved */
        for (u32 y = by; y < by + bh; y += 16) {
            for (u32 j = y; j < y + 16 && j < by + bh; j++)
                for (u32 i = 0; i < GW; i++)
                    guest_store(i, j, card_bg(i, x));
            sim_now += 16.0 * GW; /* ~4 GB/s copy */
            run_to(sim_now, L);
        }
        if (frame)
            for (u32 j = by; j < by + bh; j += bh / 3)
                mark(x, j); /* the bar's new left edge, top/middle/bottom of the band */
        (void)lastx;
        lastx = x;
        /* six counters: "t=" ms "ms  f=" frame; 6 digit cells for ms, 5 for frame */
        for (int k = 0; k < 3; k++)
            for (int s = 0; s < 2; s++) {
                u32 x0 = xs[s], y0 = ys[k];
                u64 v = ms;
                for (int d = 0; d < 6; d++, v /= 10)
                    glyph_cell(x0 + (7 - d) * cw, y0, cw, fh, (u32)(v % 10), L, d == 0);
                glyph_cell(x0, y0, 2 * cw, fh, 10, L, 0);              /* "t=" */
                glyph_cell(x0 + 8 * cw, y0, 6 * cw, fh, 11, L, 0);     /* "ms  f=" */
                v = frame;
                for (int d = 0; d < 5; d++, v /= 10)
                    glyph_cell(x0 + (18 - d) * cw, y0, cw, fh, (u32)(v % 10), L, d == 0);
                run_to(sim_now, L);
            }
        frame++;
        run_to(sim_now + 15e6, L); /* Sleep(15) */
    }
    double wall = sim_now - t0;
    drain(L);
    finish_lat(L);
    u64 f = hv_win_fb_stat(15) - f0, s = hv_win_fb_stat(32) - s0, p = hv_win_fb_stat(16) - p0, g = hv_win_fb_stat(17) - g0;
    O->cpu = 100.0 * ((busy_ns - busy0) + ((double)guest_faults - gf0) * fault_ns) / wall;
    O->faults_s = f / (wall / 1e9);
    O->spur_pct = f + s ? 100.0 * s / (f + s) : 0;
    O->passes_s = p / (wall / 1e9);
    O->pages_pass = p ? (double)g / p : 0;
    O->fps = frame / (wall / 1e9);
#ifdef NEW
    O->chunks = hv_win_fb_stat(64) - c0;
    O->chunks_wr = hv_win_fb_stat(65) - w0;
#else
    O->chunks = O->chunks_wr = 0;
#endif
}

static const char *tree_name(void)
{
#if defined(ORIG)
    return "orig";
#elif defined(PREV)
    return "r3.2-disp";
#elif defined(PARENT)
    return "disp2";
#else
    return "disp3";
#endif
}

static void run_card(const char *label, u64 flags, u64 tuning, double pause_ns, int print)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid) { int st = 0; waitpid(pid, &st, 0); if (!WIFEXITED(st) || WEXITSTATUS(st)) failures++; return; }
    int r = setup(3456, 2234, 13824, flags, tuning);
    if (r != 1) { printf("setup failed %d\n", r); _exit(1); }
    run_until_idle(4e9);
    for (int k = 0; k < 50 && sim_now < 60e9; k++) { sim_now = next_tick; do_tick(); }
    busy_ns = 0;
#ifdef NEW
    for (u64 i = 37; i <= 38; i++) hv_win_fb_stat(i | 0x100); /* reset the (R) maxima at card start */
    hv_win_fb_stat(51 | 0x100);
#endif
    print_on = print;
    struct lat L;
    struct card_out O;
    card(6e9, 2.5e9, pause_ns, &L, &O);
    print_on = 0;
#ifdef NEW
    double net_card = hv_win_fb_stat(37) / 1e6;
#endif
    run_until_idle(10e9);
    u64 bad = verify_full();
    printf("%-9s %-24s cpu %5.1f%% faults/s %5.0f spur %4.1f%% passes/s %4.0f pg/pass %5.1f fps %4.1f | lat p50 %6.1f "
           "p95 %6.1f p99 %6.1f max %6.1f ms | EL2 max_lat %6.1f ms",
           tree_name(), label, O.cpu, O.faults_s, O.spur_pct, O.passes_s, O.pages_pass, O.fps, L.p50 / 1e6, L.p95 / 1e6,
           L.p99 / 1e6, L.max / 1e6, hv_win_fb_stat(18) / 1e6);
#ifdef NEW
    printf(" card net %6.1f | wr %4.1f%% of chunks", net_card,
           O.chunks ? 100.0 * O.chunks_wr / O.chunks : 0);
#endif
    printf(" | stale %llu lost %llu\n", (unsigned long long)bad, (unsigned long long)stale_rw);
#ifdef NEW
    if (print) {
        printf("          hist(<1,<2,<4,..,>=1024 ms):");
        for (int i = 0; i < 12; i++) printf(" %llu", (unsigned long long)hv_win_fb_stat(39 + i));
        printf("  gap_max %.1f ms gaps>5ms %llu stopped %.1f ms sp same/sub/alias/rep %llu/%llu/%llu/%llu\n",
               hv_win_fb_stat(51) / 1e6, (unsigned long long)hv_win_fb_stat(54), hv_win_fb_stat(56) / 1e6,
               (unsigned long long)hv_win_fb_stat(59), (unsigned long long)hv_win_fb_stat(60),
               (unsigned long long)hv_win_fb_stat(61), (unsigned long long)hv_win_fb_stat(62));
    }
#endif
    if (bad || stale_rw) failures++;
    teardown();
    fflush(stdout);
    _exit(failures != 0);
}

static void run_workloads(const char *mode, u64 flags, u64 tuning, const char *cost)
{
    const char *wls[] = {"idle", "typing", "cursor", "drag", "scroll"};
    for (unsigned i = 0; i < 5; i++) {
        fflush(stdout);
        pid_t pid = fork();
        if (pid) { int st = 0; waitpid(pid, &st, 0); if (!WIFEXITED(st) || WEXITSTATUS(st)) failures++; continue; }
        int r = setup(3456, 2234, 13824, flags, tuning);
        if (r != 1) { printf("setup failed %d\n", r); _exit(1); }
        run_until_idle(4e9);
        for (int k = 0; k < 50 && sim_now < 60e9; k++) { sim_now = next_tick; do_tick(); }
        double t0 = sim_now;
        busy_ns = 0;
        faults_seen = guest_faults = 0;
        struct lat L;
        workload(wls[i], 3e9, &L);
        double wall = sim_now - t0;
        printf("%-9s %-5s %-22s %-7s cpu %5.1f%%  lat p50 %8.1f ms  p95 %8.1f ms  max %8.1f ms  shown=%d never-shown=%d faults=%llu\n",
               tree_name(), cost, mode, wls[i], 100.0 * (busy_ns + guest_faults * fault_ns) / wall, L.p50 / 1e6, L.p95 / 1e6,
               L.max / 1e6, L.n, L.lost + L.superseded, (unsigned long long)faults_seen);
        teardown();
        fflush(stdout);
        _exit(0);
    }
}

int main(int argc, char **argv)
{
    const char *what = argc > 1 ? argv[1] : "test";
    if (getenv("SIM_VCPUS")) /* nightly-7: the shadow display with N vCPUs */
        hv_win.nr_vcpus = strtoul(getenv("SIM_VCPUS"), NULL, 0);
    legacy_row_ns = 311718; /* measured max (attempt 26) */
    /*
     * Cost scenarios for the card (ns per byte). Both make the r3.2-disp 2x converter cost the
     * hardware-measured 136 us per 16 KiB shadow page (16 KiB NC read + 64 KiB NC write):
     *   wdom: reads 1.4 ns/B (23 us/page), writes 1.725 ns/B (113 us/page)
     *   rdom: reads 6.5 ns/B (106 us/page), writes 0.45 ns/B (29 us/page)
     * The split itself is unknown offline (the PREPARE bench ran with cached reads, see
     * BUILD-NOTES); disp2's rt counters (66..68) measure it on hardware.
     */
    struct { const char *n; double rd, wr; } costs[] = {{"wdom", 1.4, 1.725}, {"rdom", 6.5, 0.45}};
    if (!strcmp(what, "test")) {
        rd_ns_per_b = 0.0; wr_ns_per_b = 0.0;
#ifdef ORIG
        CHILD(test_full_frame("orig-sweep", 1000, 600, 4160, 0));
        CHILD(test_full_frame("orig-sweep-native-geom", 3456, 2234, 13824, 0));
#else
        test_colour();
        CHILD(test_full_frame("sweep", 1000, 600, 4160, 0));
        rd_ns_per_b = 1.4; wr_ns_per_b = 0.1;
        CHILD(test_full_frame("sweep-native-geom(mid)", 3456, 2234, 13824, 0));
        CHILD(test_full_frame("dirty-native-geom(mid)", 3456, 2234, 13824, 2));
        CHILD(test_full_frame("dirty+scale2(mid)", 3456, 2234, 13824, 6));
        CHILD(test_random_dirty(2, 0, 0));
        CHILD(test_random_dirty(6, 0, 0));
        CHILD(test_random_dirty(2, 0, 0.5));
        CHILD(test_random_dirty(6, 0, 0.5));
#ifdef NEW
        CHILD(test_random_dirty(2, 1ULL << 40, 0.5));            /* tlbi all */
        CHILD(test_random_dirty(2, 2ULL << 40, 0.5));            /* tlbi far (r3.2-disp) */
        CHILD(test_random_dirty(6, 1ULL << 42, 0.5));            /* no write elision */
        CHILD(test_random_dirty(6, 1ULL << 43, 0.5));            /* merge-ahead on */
        CHILD(test_random_dirty(2, 1ULL << 43, 0.5));
        CHILD(test_random_dirty(0, 0, 0));                       /* sweep + elision */
        CHILD(test_stat_reset());
        CHILD(test_refresh(1ULL << 44, 1));                       /* nightly-6-perf: refresh every 0.5 s */
        CHILD(test_refresh(2ULL << 44, 1));                       /* every 1 s */
        CHILD(test_refresh(0, 0));                                /* control: off = stale stays */
        CHILD(test_random_dirty(2, 1ULL << 44, 0.5));            /* refresh during random drawing */
        CHILD(test_random_dirty(6, (1ULL << 44) | (1ULL << 43), 0.5));
#endif
        CHILD(test_scale2_checker());
#endif
        printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
        return failures != 0;
    }
#ifdef NEW
    if (!strcmp(what, "strict")) { /* disp3: M1 barrier and L1 alignment under the strict stage-2 model */
        rd_ns_per_b = 1.4; wr_ns_per_b = 0.1;
        CHILD(test_strict_reprotect("reprotect small, tlbi sub", 2, 0));
        CHILD(test_strict_reprotect("reprotect small, tlbi all", 2, 1ULL << 40));
        CHILD(test_strict_reprotect("reprotect small, tlbi far", 2, 2ULL << 40));
        CHILD(test_strict_reprotect("reprotect small, 2x", 6, 0));
        CHILD(test_strict_reprotect("reprotect small, merge-ahead", 2, 1ULL << 43));
        CHILD(test_strict_random("random, sub, pf 0", 2, 0, 0));
        CHILD(test_strict_random("random, sub, pf 0.5", 2, 0, 0.5));
        CHILD(test_strict_random("random, 2x sub, pf 0.5", 6, 0, 0.5));
        CHILD(test_strict_random("random, all, pf 0.5", 2, 1ULL << 40, 0.5));
        CHILD(test_strict_random("random, merge-ahead, pf 0.5", 2, 1ULL << 43, 0.5));
        CHILD(test_strict_random("random, no elision, pf 0.5", 6, 1ULL << 42, 0.5));
        CHILD(test_strict_random("random, refresh 0.5 s, pf 0.5", 2, 1ULL << 44, 0.5)); /* nightly-6-perf */
        printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
        return failures != 0;
    }
    if (!strcmp(what, "refresh")) { /* nightly-6-perf: background refresh only (run.sh mutants) */
        rd_ns_per_b = 1.4; wr_ns_per_b = 0.1;
        CHILD(test_refresh(1ULL << 44, 1));
        CHILD(test_refresh(0, 0));
        printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
        return failures != 0;
    }
    if (!strcmp(what, "print")) { /* disp3: L2 */
        rd_ns_per_b = 1.4; wr_ns_per_b = 0.1;
        CHILD(test_print_accounting());
        printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
        return failures != 0;
    }
#endif
    if (!strcmp(what, "reject")) {
#ifndef ORIG
        int r = setup(3456, 2234, 13824, 0x10, 0);
        CHECK(r == -1, "unknown flag accepted (%d)", r);
        printf("reject unknown flag bit4: %d\n", r);
#endif
        return failures != 0;
    }
    if (!strcmp(what, "reject-tuning")) {
#ifndef ORIG
        int r = setup(3456, 2234, 13824, 2, 5000ULL << 16);
        CHECK(r == -4, "max budget 5000us accepted (%d)", r);
        printf("reject escalated budget 5000 us: %d\n", r);
#endif
        return failures != 0;
    }
#ifdef NEW
    if (!strcmp(what, "reject-pool-overlap")) { /* nightly-6c: a shadow inside the n5c pool is never mapped */
        pool_overlap = 1;
        int r = setup(1000, 600, 4160, 2, 0);
        CHECK(r == -100, "shadow overlapping the extension pool started (%d)", r);
        printf("reject shadow overlapping the n5c pool: %d\n", r);
        return failures != 0;
    }
    if (!strcmp(what, "reject-refresh-sweep")) { /* nightly-6-perf: refresh bits without DIRTY */
        int r = setup(3456, 2234, 13824, 0, 1ULL << 44);
        CHECK(r == -4, "refresh without dirty tracking accepted (%d)", r);
        printf("reject refresh in sweep mode: %d\n", r);
        return failures != 0;
    }
    if (!strcmp(what, "reject-tuning2")) { /* reserved tuning bits and tlbi mode 3 */
        int r = setup(3456, 2234, 13824, 2, argc > 2 ? strtoull(argv[2], NULL, 0) : (3ULL << 40));
        CHECK(r == -4, "bad tuning accepted (%d)", r);
        printf("reject tuning %s: %d\n", argc > 2 ? argv[2] : "tlbi=3", r);
        return failures != 0;
    }
#endif
    if (!strcmp(what, "work")) {
#ifdef ORIG
        rd_ns_per_b = wr_ns_per_b = 0;
        run_workloads("orig(1 row/tick)", 0, 0, "meas");
#else
        for (unsigned c = 0; c < 2; c++) {
            rd_ns_per_b = costs[c].rd; wr_ns_per_b = costs[c].wr;
            run_workloads("dirty (default)", 2, 0, costs[c].n);
            run_workloads("dirty+scale2 (default)", 6, 0, costs[c].n);
        }
#endif
        return failures != 0;
    }
    if (!strcmp(what, "card")) {
#ifndef ORIG
        /* p_pf calibrated so the r3.2-disp converter shows the hardware's ~27 % spurious share */
        p_pf = argc > 2 ? atof(argv[2]) : 0.45;
        for (unsigned c = 0; c < 2; c++) {
            rd_ns_per_b = costs[c].rd; wr_ns_per_b = costs[c].wr;
            printf("-- cost %s (rd %.2f ns/B, wr %.3f ns/B), TLB prefetch p=%.2f --\n", costs[c].n, costs[c].rd,
                   costs[c].wr, p_pf);
            run_card("2x, no pause", 6, 0, 0, 0);
            run_card("2x, 245 ms host pause", 6, 0, 245e6, 0);
#ifdef NEW
            run_card("2x tlbi=far", 6, 2ULL << 40, 0, 0);
            run_card("2x tlbi=all", 6, 1ULL << 40, 0, 0);
            run_card("2x no-diff", 6, 1ULL << 42, 0, 0);
            run_card("2x merge-ahead", 6, 1ULL << 43, 0, 0);
            run_card("1x (native)", 2, 0, 0, 0);
            run_card("2x pause, stats 1 s", 6, 1ULL << 48, 245e6, c == 0);
            if (getenv("SWEEP")) {
                run_card("2x base 500", 6, 500, 0, 0);
                run_card("2x base 1000", 6, 1000, 0, 0);
                run_card("2x defer 4", 6, 4ULL << 32, 0, 0);
                run_card("2x defer 2", 6, 2ULL << 32, 0, 0);
                run_card("2x defer 4 base 500", 6, 500 | (4ULL << 32), 0, 0);
            }
#else
            run_card("1x (native)", 2, 0, 0, 0);
#endif
        }
#endif
        return failures != 0;
    }
    return 2;
}
