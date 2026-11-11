/* SPDX-License-Identifier: MIT */
/* Resident trackpad adapter (mtp_tp_resident.c): dart-mtp decisions, the exact
 * register writes, SMC gp1c values, preguest-only SMC use. Host stubs only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned char arena[1 << 20] __attribute__((aligned(16384)));
#define ARENA_PA 0x800000000ull
#define TP_PA(p) ((unsigned long long)((unsigned char *)(p) - arena) + ARENA_PA)
#define TP_PTR(pa) ((void *)(arena + ((pa) - ARENA_PA)))
#include "mtp_tp_resident.c" /* copied by run.sh so stubs shadow the resident headers */
static unsigned checks, groups;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
u64 test_ticks; void *adt; void (*mtp_kbd_attach_hook)(struct mtp_kbd *); /* defined by mtp_kbd_resident.c */
static unsigned arena_used, nwrites, smc_inits, smc_shut, nsmc; static u64 last_addr; static u32 last_val, smc_keys[8], smc_vals[8];
static u32 tcr, ttbr, err, tlb_busy_reads; static int smc_fail, geometry_bad; static u64 vmb = 0x10000004000ull;
void *memalign(size_t a, size_t n) { arena_used = (arena_used + (unsigned)a - 1) & ~((unsigned)a - 1); void *p = arena + arena_used; arena_used += (unsigned)n; CHECK(arena_used <= sizeof(arena)); return p; }
bool is_heap(void *p) { (void)p; return true; }
u32 read32(u64 a)
{
    if (a == DART_MTP + DART_T8110_TCR0) return tcr;
    if (a == DART_MTP + DART_T8110_TTBR0) return ttbr;
    if (a == DART_MTP + DART_T8110_ERROR) return err;
    if (a == DART_MTP + DART_T8110_PROTECT) return 0;
    if (a == DART_MTP + DART_T8110_TLB_CMD) { if (tlb_busy_reads) { tlb_busy_reads--; return DART_T8110_TLB_CMD_BUSY; } return 0; }
    CHECK(!"unexpected read"); return 0;
}
void write32(u64 a, u32 v) { nwrites++; last_addr = a; last_val = v; }
u64 read64(u64 a) { (void)a; CHECK(0); return 0; }
void write64(u64 a, u64 v) { (void)a; (void)v; CHECK(0); }
void udelay(u32 us) { (void)us; }
void wdt_kick(void) {}
int adt_path_offset_trace(const void *a, const char *p, int *path) { (void)a; (void)path; CHECK(!strcmp(p, "/arm-io/dart-mtp")); return geometry_bad ? -1 : 1; }
bool adt_is_compatible(const void *a, int n, const char *c) { (void)a; (void)n; return !strcmp(c, "dart,t8110"); }
int adt_get_reg(const void *a, int *p, const char *n, int i, u64 *b, u64 *s) { (void)a; (void)p; (void)n; CHECK(i == 0); *b = DART_MTP; *s = 0xc000; return 0; }
const void *adt_getprop(const void *a, int n, const char *p, u32 *len) { (void)a; (void)n; if (strcmp(p, "vm-base")) return NULL; *len = 8; return &vmb; }
struct smc_dev { int x; } smcdev;
smc_dev_t *smc_init(void) { smc_inits++; return smc_fail ? NULL : &smcdev; }
int smc_write_u32(smc_dev_t *d, u32 k, u32 v) { CHECK(d == &smcdev && nsmc < 8); smc_keys[nsmc] = k; smc_vals[nsmc++] = v; return 0; }
void smc_shutdown(smc_dev_t *d) { CHECK(d == &smcdev); smc_shut++; }
void rtkit_set_boot_timeout(u64 us) { (void)us; }
static void reset(void)
{
    memset(arena, 0, sizeof(arena)); arena_used = 0; nwrites = smc_inits = smc_shut = nsmc = 0; last_addr = 0;
    tcr = ttbr = err = tlb_busy_reads = 0; smc_fail = geometry_bad = 0; smc = NULL; smc_tried = false; preguest = true;
    dma_buf = NULL; dma_iova = 0; groups++;
}
int main(void)
{
    /* bypass: IOVA = PA, not a single register write */
    reset(); tcr = 0x6; CHECK(dma_prepare(81388) == 0 && nwrites == 0 && dma_iova == TP_PA(dma_buf) && dma_cap == 81920);
    CHECK((TP_PA(dma_buf) & 0x3fff) == 0);
    /* translate with iBoot tables: one TLB command write, PTEs in DRAM, top free L1 slot */
    reset(); u64 *l1 = memalign(16384, 16384); l1[2047] = 1; l1[2046] = 0;
    tcr = 1; ttbr = (u32)(((TP_PA(l1) >> 14) << 2) | 1);
    CHECK(dma_prepare(81388) == 0 && nwrites == 1 && last_addr == DART_MTP + DART_T8110_TLB_CMD && last_val == 0);
    CHECK((l1[2046] & 1) && dma_iova == (0x10000000000ull | (2046ull << 25)));
    u64 *l2 = TP_PTR(FIELD_GET(PTE_OFFSET, l1[2046]) << 14);
    for (unsigned i = 0; i < 5; i++) CHECK((l2[i] & 1) && (FIELD_GET(PTE_OFFSET, l2[i]) << 14) == TP_PA(dma_buf) + i * 16384u);
    CHECK(!l2[5] && l1[2047] == 1);
    /* states not handled: four-level, error pending, TTBR invalid, translate off -> no write */
    reset(); tcr = 1 | 8; ttbr = 1; CHECK(dma_prepare(1000) < 0 && nwrites == 0);
    reset(); tcr = 1; ttbr = (u32)(((TP_PA(arena) >> 14) << 2) | 1); err = 1; CHECK(dma_prepare(1000) < 0 && nwrites == 0);
    reset(); tcr = 1; ttbr = 0; CHECK(dma_prepare(1000) < 0 && nwrites == 0);
    reset(); tcr = 0; CHECK(dma_prepare(1000) < 0 && nwrites == 0);
    reset(); geometry_bad = 1; tcr = 6; CHECK(dma_prepare(1000) < 0 && nwrites == 0);
    /* TLB never idle before the flush: L1 entry withdrawn, no command written */
    reset(); l1 = memalign(16384, 16384); tcr = 1; ttbr = (u32)(((TP_PA(l1) >> 14) << 2) | 1); tlb_busy_reads = 5000;
    CHECK(dma_prepare(1000) < 0 && nwrites == 0 && !l1[2047]);
    /* AFE: SMC gp1c, active low; SMC brought up once, preguest only */
    reset(); CHECK(afe_reset(NULL, 1) == 0 && afe_reset(NULL, 0) == 0 && smc_inits == 1 && nsmc == 2);
    CHECK(smc_keys[0] == 0x67703163u && smc_vals[0] == 0x01000000u && smc_vals[1] == 0x01000001u);
    preguest = false; CHECK(afe_reset(NULL, 1) == -1 && nsmc == 2);
    reset(); smc_fail = 1; CHECK(afe_reset(NULL, 1) == -1 && afe_reset(NULL, 0) == -1 && smc_inits == 1);
    /* finish: no layer -> NULL; failed layer -> NULL, k->tp cleared, SMC released once */
    reset(); { struct mtp_kbd k; memset(&k, 0, sizeof(k)); CHECK(mtp_tp_resident_finish(&k) == NULL); }
    reset(); { struct mtp_kbd k; memset(&k, 0, sizeof(k)); k.phase = MTP_FAILED; tp.phase = TP_FAILED; k.tp = &tp;
        CHECK(afe_reset(NULL, 1) == 0); CHECK(mtp_tp_resident_finish(&k) == NULL && !k.tp && smc_shut == 1 && !preguest); }
    /* arm: hook only with a firmware */
    mtp_tp_resident_arm(0, 0); CHECK(mtp_kbd_attach_hook == NULL);
    mtp_tp_resident_arm(0x1000, 81420); CHECK(mtp_kbd_attach_hook == attach);
    printf("tp-resident: %u groups, %u checks PASS\n", groups, checks);
    return 0;
}
