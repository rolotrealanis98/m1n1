/* SPDX-License-Identifier: MIT */
/* dart-mtp host DMA (mtp_dma.c): decisions per stream-0 state and the exact writes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned char arena[1 << 21] __attribute__((aligned(16384)));
#define ARENA_PA 0x800000000ull
#define TP_PA(p) ((unsigned long long)((unsigned char *)(p) - arena) + ARENA_PA)
#define TP_PTR(pa) ((void *)(arena + ((pa) - ARENA_PA)))
/* nightly-2: memory classes for the iBoot-tables check. Resident = what memalign handed out so far; OS RAM = a
 * window the test sets; DRAM mapping = the whole arena. */
#define MTP_DMA_MEM_PROVIDED
static unsigned used;
static unsigned long long os_lo, os_hi;
static int resident_off; /* test switch: report nothing as resident-owned */
static _Bool mtp_dma_resident(unsigned long long pa, unsigned long long len)
{ return !resident_off && len && pa >= ARENA_PA && pa + len <= ARENA_PA + used; }
static _Bool mtp_dma_os_ram(unsigned long long pa, unsigned long long len) { return pa < os_hi && os_lo < pa + len; }
static _Bool mtp_dma_dram(unsigned long long pa, unsigned long long len)
{ return len && pa >= ARENA_PA && pa + len <= ARENA_PA + (1ull << 21); }
#include "mtp_dma.c" /* copied by run.sh so stubs shadow the resident headers */
static unsigned checks, groups;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
u64 test_ticks; void *adt;
static unsigned nw; static u64 wa[16]; static u32 wv[16];
static u32 tcr, ttbr, err, prot, en, busy; static int geobad; static u64 vmb = 0x10000004000ull;
void *memalign(size_t a, size_t n) { used = (used + (unsigned)a - 1) & ~((unsigned)a - 1); void *p = arena + used; used += (unsigned)n; CHECK(used <= sizeof(arena)); return p; }
void free(void *p) { (void)p; }
bool is_heap(void *p) { (void)p; return true; }
u32 read32(u64 a)
{
    if (a == DART_MTP + T8110_TCR0) return tcr;
    if (a == DART_MTP + T8110_TTBR0) return ttbr;
    if (a == DART_MTP + T8110_ERROR) return err;
    if (a == DART_MTP + T8110_PROTECT) return prot;
    if (a == DART_MTP + T8110_ENABLE_STREAMS) return en;
    if (a == DART_MTP + T8110_PARAMS3) return 0x24240202;
    if (a == DART_MTP + T8110_TLB_CMD) { if (busy) { busy--; return T8110_TLB_BUSY; } return 0; }
    CHECK(!"unexpected read"); return 0;
}
void write32(u64 a, u32 v) { CHECK(nw < 16); wa[nw] = a; wv[nw++] = v; }
void udelay(u32 us) { (void)us; }
int adt_path_offset_trace(const void *a, const char *p, int *path) { (void)a; (void)path; CHECK(!strcmp(p, "/arm-io/dart-mtp")); return geobad ? -1 : 1; }
bool adt_is_compatible(const void *a, int n, const char *c) { (void)a; (void)n; return !strcmp(c, "dart,t8110"); }
int adt_get_reg(const void *a, int *p, const char *n, int i, u64 *b, u64 *s) { (void)a; (void)p; (void)n; CHECK(i == 0); *b = DART_MTP; *s = 0xc000; return 0; }
const void *adt_getprop(const void *a, int n, const char *p, u32 *len) { (void)a; (void)n; if (strcmp(p, "vm-base")) return NULL; *len = 8; return &vmb; }
void rtkit_mtp_host_buffers(rtkit_dev_t *r, bool (*f)(size_t, void **, u64 *)) { (void)r; (void)f; }
static void reset(void) { memset(arena, 0, sizeof(arena)); used = nw = 0; os_lo = os_hi = 0; resident_off = 0; refused_pa = 0; tcr = ttbr = err = prot = en = busy = 0; geobad = 0;
    mode = MTP_DMA_OFF; tried = false; window = slot_base = 0; l2 = NULL; next_page = 0; groups++; }
int main(void)
{
    void *cpu; u64 iova;
    /* bypass: no write, DVA = PA */
    reset(); tcr = 6; CHECK(mtp_dma_init() == MTP_DMA_BYPASS && nw == 0);
    CHECK(mtp_dma_alloc(0x5000, &cpu, &iova) && iova == TP_PA(cpu) && !(iova & 0x3fff) && nw == 0);
    /* real J716s state (tp2-a2): TCR=0x1 TTBR=0 ERROR=0 PROTECT=0x1a ENABLE=0 -> Linux reset + attach:
     * TCR0=0, TTBR0, ENABLE_STREAMS |= 1, TCR0 = translate, FLUSH_ALL */
    reset(); tcr = 1; ttbr = 0; prot = 0x1a; en = 0; CHECK(mtp_dma_init() == MTP_DMA_OWN_TABLES);
    CHECK(nw == 5 && wa[0] == DART_MTP + T8110_TCR0 && wv[0] == 0 && wa[1] == DART_MTP + T8110_TTBR0 && (wv[1] & 1) &&
          wa[2] == DART_MTP + T8110_ENABLE_STREAMS && wv[2] == 1 && wa[3] == DART_MTP + T8110_TCR0 && wv[3] == 1 &&
          wa[4] == DART_MTP + T8110_TLB_CMD && wv[4] == 0);
    { u64 *rt = TP_PTR((u64)((wv[1] >> 2) & 0xfffffff) << 14); CHECK((rt[1] & 1) && slot_base == (1ull << 25)); }
    /* stream off with TCR 0 and another stream enabled: same, enable bit OR-ed in */
    reset(); en = 0x10; CHECK(mtp_dma_init() == MTP_DMA_OWN_TABLES);
    CHECK(nw == 5 && wa[0] == DART_MTP + T8110_TCR0 && wv[0] == 0 && wa[2] == DART_MTP + T8110_ENABLE_STREAMS && wv[2] == 0x11);
    nw = 4; wv[0] = wv[1]; /* keep the indices the checks below expect: wv[0] = TTBR */
    u64 *root = TP_PTR((u64)((wv[0] >> 2) & 0xfffffff) << 14);
    CHECK((root[1] & 1) && !root[0] && !root[2] && slot_base == (1ull << 25) && window == 0x10000000000ull);
    CHECK(mtp_dma_alloc(0x5000, &cpu, &iova) && iova == 0x10002000000ull && nw == 5 && wv[4] == 0);
    u64 *L2 = TP_PTR(FIELD_GET(PTE_OFFSET, root[1]) << 14);
    CHECK((L2[0] & 1) && (FIELD_GET(PTE_OFFSET, L2[0]) << 14) == TP_PA(cpu) && (FIELD_GET(PTE_OFFSET, L2[1]) << 14) == TP_PA(cpu) + 0x4000 && !L2[2]);
    CHECK(mtp_dma_alloc(81388, &cpu, &iova) && iova == 0x10002008000ull);   /* firmware after the crashlog */
    CHECK(mtp_dma_init() == MTP_DMA_OWN_TABLES && nw == 6);                  /* idempotent */
    /* iBoot tables: one root entry in place, no TTBR/TCR write, lowest free slot >= 1 */
    /* nightly-2: iBoot's slot-1 entry now points at a real (resident) L2 table, since the table check walks it */
    reset(); u64 *ib = memalign(16384, 16384); u64 *ibt = memalign(16384, 16384); const u64 e1 = FIELD_PREP(PTE_OFFSET, TP_PA(ibt) >> 14) | 1;
    ib[1] = e1; tcr = 1; ttbr = (u32)(((TP_PA(ib) >> 14) << 2) | 1);
    CHECK(mtp_dma_init() == MTP_DMA_IBOOT_TABLES && nw == 1 && wa[0] == DART_MTP + T8110_TLB_CMD && (ib[2] & 1) && ib[1] == e1 && slot_base == (2ull << 25));
    /* refused states: locked, error, four-level, translate without TTBR, geometry */
    reset(); prot = 1; CHECK(mtp_dma_init() == MTP_DMA_OFF && nw == 0 && !mtp_dma_alloc(16, &cpu, &iova));
    reset(); prot = 0x1b; tcr = 1; CHECK(mtp_dma_init() == MTP_DMA_OFF && nw == 0);   /* real bits + lock */
    reset(); err = 4; CHECK(mtp_dma_init() == MTP_DMA_OFF && nw == 0);
    reset(); tcr = 9; ttbr = 1; CHECK(mtp_dma_init() == MTP_DMA_OFF && nw == 0);
    reset(); geobad = 1; CHECK(mtp_dma_init() == MTP_DMA_OFF && nw == 0);
    /* TLB never idle: own tables were written, then off (no allocation possible) */
    reset(); busy = 100000; CHECK(mtp_dma_init() == MTP_DMA_OFF && nw == 4 && !mtp_dma_alloc(16, &cpu, &iova));
    /* iBoot slots 1..127 all taken -> off, nothing written */
    reset(); ib = memalign(16384, 16384); for (int k = 0; k < 128; k++) ib[k] = 1; tcr = 1; ttbr = (u32)(((TP_PA(ib) >> 14) << 2) | 1);
    CHECK(mtp_dma_init() == MTP_DMA_OFF && nw == 0);
    /* nightly-2 (review finding 1): iBoot tables only when every table page and mapped page is resident-owned or
     * outside the OS RAM. Pages above `used` are not resident; OS RAM = the upper half of the arena. */
    const unsigned long long hi_pa = ARENA_PA + (1u << 20);          /* not handed out by memalign */
#define IBOOT_AT(off) (u64 *)(arena + (off))
#define SET_TTBR(t) (ttbr = (u32)(((TP_PA(t) >> 14) << 2) | 1))
    /* root in OS RAM, not resident -> refused, nothing written */
    reset(); ib = IBOOT_AT(1u << 20); tcr = 1; SET_TTBR(ib); os_lo = hi_pa; os_hi = ARENA_PA + (1ull << 21);
    CHECK(mtp_dma_init() == MTP_DMA_OFF && nw == 0 && refused_pa == hi_pa && !mtp_dma_alloc(16, &cpu, &iova));
    /* same root outside OS RAM -> accepted (iBoot carveout) */
    reset(); ib = IBOOT_AT(1u << 20); tcr = 1; SET_TTBR(ib); os_lo = ARENA_PA; os_hi = ARENA_PA + 0x4000;
    CHECK(mtp_dma_init() == MTP_DMA_IBOOT_TABLES && (ib[1] & 1));
    /* root fine, an existing L2 table in OS RAM -> refused */
    reset(); ib = IBOOT_AT(1u << 20); u64 *t2 = IBOOT_AT((1u << 20) + 0x8000);
    ib[3] = FIELD_PREP(PTE_OFFSET, TP_PA(t2) >> 14) | 1; tcr = 1; SET_TTBR(ib);
    os_lo = TP_PA(t2); os_hi = TP_PA(t2) + 0x4000;
    CHECK(mtp_dma_init() == MTP_DMA_OFF && nw == 0 && refused_pa == TP_PA(t2) && !ib[1]);
    /* tables fine, a mapped leaf page in OS RAM -> refused */
    reset(); ib = IBOOT_AT(1u << 20); t2 = IBOOT_AT((1u << 20) + 0x8000);
    ib[3] = FIELD_PREP(PTE_OFFSET, TP_PA(t2) >> 14) | 1; t2[7] = FIELD_PREP(PTE_OFFSET, (hi_pa + 0x40000) >> 14) | 1;
    tcr = 1; SET_TTBR(ib); os_lo = hi_pa + 0x40000; os_hi = hi_pa + 0x44000;
    CHECK(mtp_dma_init() == MTP_DMA_OFF && nw == 0 && refused_pa == hi_pa + 0x40000);
    /* same with the leaf outside OS RAM -> accepted, our slot is the lowest free one >= 1 */
    reset(); ib = IBOOT_AT(1u << 20); t2 = IBOOT_AT((1u << 20) + 0x8000);
    ib[3] = FIELD_PREP(PTE_OFFSET, TP_PA(t2) >> 14) | 1; t2[7] = FIELD_PREP(PTE_OFFSET, (hi_pa + 0x40000) >> 14) | 1;
    tcr = 1; SET_TTBR(ib); os_lo = hi_pa + 0x80000; os_hi = hi_pa + 0x84000;
    CHECK(mtp_dma_init() == MTP_DMA_IBOOT_TABLES && (ib[1] & 1) && slot_base == (1ull << 25));
    /* root outside the DRAM mapping -> refused before it is read */
    reset(); tcr = 1; ttbr = (u32)((((ARENA_PA + (4ull << 20)) >> 14) << 2) | 1);
    CHECK(mtp_dma_init() == MTP_DMA_OFF && nw == 0);
    /* own tables / buffers must be resident: if the heap is not resident-owned, nothing is enabled or mapped */
    reset(); resident_off = 1; CHECK(mtp_dma_init() == MTP_DMA_OFF && nw == 0);
    reset(); tcr = 6; CHECK(mtp_dma_init() == MTP_DMA_BYPASS); resident_off = 1; CHECK(!mtp_dma_alloc(0x4000, &cpu, &iova));
    printf("mtp-dma: %u groups, %u checks PASS\n", groups, checks);
    return 0;
}
