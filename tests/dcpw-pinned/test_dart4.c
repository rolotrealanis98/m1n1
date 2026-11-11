/* SPDX-License-Identifier: GPL-2.0-only */
/* test_dart4.c -- dart4 against a fake locked "four-level" (TTBR + 2 tables) T8110 stream:
 * firmware entries never modified, only vacant ROOT slots in the window claimed, table vs leaf PTE
 * format (SP_END), no TLB invalidate on map, RANGE invalidate on unmap, refusals. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../service/dart4.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define MEM_BASE 0x10000000000ull
#define MEM_SIZE (64ull << 20)
static uint8_t *mem;
static uint64_t next_table;
static uint32_t regs[0x2000 / 4];
static int busy_cycles, tlb_cmds, other_writes, bad_access, alloc_fail, barriers;
static uint32_t last_tlb, tlb_start, tlb_end;
static uint64_t fw_lo, fw_hi;
static int fw_writes;

static uint64_t rd64(void *c, uint64_t pa)
{
	(void)c;
	if (pa < MEM_BASE || pa + 8 > MEM_BASE + MEM_SIZE || (pa & 7)) { bad_access++; return 0; }
	uint64_t v; memcpy(&v, mem + (pa - MEM_BASE), 8); return v;
}
static void wr64(void *c, uint64_t pa, uint64_t v)
{
	(void)c;
	if (pa < MEM_BASE || pa + 8 > MEM_BASE + MEM_SIZE || (pa & 7)) { bad_access++; return; }
	if (pa >= fw_lo && pa < fw_hi) {
		uint64_t old; memcpy(&old, mem + (pa - MEM_BASE), 8);
		if (old) fw_writes++;                           /* overwrote a live firmware entry */
		if (pa >= fw_lo + 0x4000) fw_writes++;          /* any write into a firmware table below the root */
	}
	memcpy(mem + (pa - MEM_BASE), &v, 8);
}
#define DART 0x48a340000ull
static uint32_t mrd(void *c, uint64_t pa)
{
	(void)c;
	if (pa < DART || pa >= DART + sizeof(regs)) { bad_access++; return 0; }
	if (pa == DART + 0x80 && busy_cycles > 0) { busy_cycles--; return 1u << 31; }
	return regs[(pa - DART) / 4];
}
static void mwr(void *c, uint64_t pa, uint32_t v)
{
	(void)c;
	if (pa == DART + 0x80) { tlb_cmds++; last_tlb = v; busy_cycles = 3; return; }
	if (pa == DART + 0x98) { tlb_start = v; return; }
	if (pa == DART + 0xa0) { tlb_end = v; return; }
	other_writes++;
}
static uint64_t alloc_table(void *c)
{
	(void)c;
	if (alloc_fail) return 0;
	uint64_t t = next_table; next_table += 0x4000;
	memset(mem + (t - MEM_BASE), 0, 0x4000);
	return t;
}
static void barrier(void *c) { (void)c; barriers++; }
static struct dart4_io IO = { 0, rd64, wr64, mrd, mwr, alloc_table, barrier };

static uint64_t tblpte(uint64_t pa) { return ((pa >> 14) << 10) | 1; }
static uint64_t fwleaf(uint64_t pa) { return ((pa >> 14) << 10) | (0xfffull << 40) | 1; }

/* firmware: root at MEM_BASE; slot 16 -> mid (+0x4000) -> leaf (+0x8000) mapping a buffer at DVA
 * 0x1000ACA4000 (the crashlog-region DVA of the macOS capture) */
static void build_fw(void)
{
	uint64_t root = MEM_BASE, mid = root + 0x4000, leaf = root + 0x8000;
	memset(mem, 0, MEM_SIZE);
	memset(regs, 0, sizeof(regs));
	fw_lo = fw_hi = 0;
	next_table = MEM_BASE + (32ull << 20);
	uint64_t dva = 0x1000ACA4000ull;
	wr64(0, root + 8 * ((dva >> 36) & 2047), tblpte(mid));
	wr64(0, mid + 8 * ((dva >> 25) & 2047), tblpte(leaf));
	for (int i = 0; i < 8; i++) wr64(0, leaf + 8 * (((dva >> 14) + i) & 2047), fwleaf(0x10bd12d8000ull + 0x4000ull * i) | 0x2);
	fw_lo = root; fw_hi = root + 0xc000;
	regs[0x08 / 4] = 0x0202;                  /* PARAMS3 version */
	regs[0x200 / 4] = 1;                      /* PROTECT */
	regs[(0x1000 + 4 * 23) / 4] = 0x9;        /* TCR translate + 4-level */
	regs[(0x1400 + 4 * 23) / 4] = (uint32_t)(((root >> 14) << 2) | 1);
}

int main(void)
{
	mem = calloc(1, MEM_SIZE);
	struct dart4 d;
	uint64_t wb = 0x10000000000ull, ws = 1ull << 40;   /* slots 16..31, as Linux's 0x1ffffe00000 */
	build_fw();

	printf("== init refusals (nothing written)\n");
	CHECK(dart4_init(&d, &IO, DART, 23, MEM_BASE + 0x4000, wb, ws, 2) == -2);   /* wrong ADT root */
	regs[0x200 / 4] = 0;
	CHECK(dart4_init(&d, &IO, DART, 23, MEM_BASE, wb, ws, 2) == -2);            /* not locked */
	regs[0x200 / 4] = 1; regs[(0x1000 + 4 * 23) / 4] = 0x1;
	CHECK(dart4_init(&d, &IO, DART, 23, MEM_BASE, wb, ws, 2) == -2);            /* three-level */
	regs[(0x1000 + 4 * 23) / 4] = 0x9;
	CHECK(dart4_init(&d, &IO, DART, 23, MEM_BASE, 1ull << 47, ws, 2) == -1);    /* window beyond 2^47 */
	CHECK(dart4_init(&d, &IO, DART, 23, MEM_BASE, wb + 0x4000, ws, 2) == -1);   /* window not slot-aligned */
	CHECK(dart4_init(&d, &IO, DART, 40, MEM_BASE, wb, ws, 2) == -1);
	CHECK(other_writes == 0 && tlb_cmds == 0 && fw_writes == 0);

	printf("== init: top-down vacant root slots\n");
	CHECK(dart4_init(&d, &IO, DART, 23, MEM_BASE, wb, ws, 2) == 0);
	CHECK(d.nslots == 2 && d.slot[0] == 31 && d.slot[1] == 30 && d.version == 0x0202);
	CHECK(dart4_translate(&d, 0x1000ACA4000ull) == 0x10bd12d8000ull);           /* firmware leaf with prot bits */
	CHECK(dart4_translate(&d, 0x1000ACA4000ull + 0x4123) == 0x10bd12dc123ull);

	printf("== map 1 MiB shmem and a 30.9 MB surface: no TLB command, firmware untouched\n");
	uint64_t shm_pa = MEM_BASE + (8ull << 20);
	uint64_t dva = dart4_map(&d, shm_pa, 0x100000);
	CHECK(dva == (31ull << 36));                           /* 0x1F000000000: Linux's slot */
	CHECK(dart4_translate(&d, dva) == shm_pa && dart4_translate(&d, dva + 0xfc000) == shm_pa + 0xfc000);
	uint64_t root31 = rd64(0, MEM_BASE + 8 * 31), mid = (root31 & 0x3ffffffc00ull) << 4;
	uint64_t l = rd64(0, mid), leaf = (l & 0x3ffffffc00ull) << 4, e = rd64(0, leaf);
	CHECK((root31 >> 40) == 0 && (l >> 40) == 0);           /* table entries: PA | VALID only */
	CHECK((e >> 40) == 0xfff && (e & 0xe) == 0 && (e & 1)); /* leaf: SP_END, rw+cacheable */
	CHECK(tlb_cmds == 0 && barriers > 0);
	CHECK(fw_writes == 0 && other_writes == 0 && bad_access == 0);
	uint64_t surf_pa = 0x10a00000000ull, len = ((13824ull * 2234) + 0x3fff) & ~0x3fffull;
	uint64_t sdva = dart4_map(&d, surf_pa, len);
	CHECK(sdva == dva + 0x100000 + 0x4000);
	CHECK(dart4_translate(&d, sdva + len - 0x4000) == surf_pa + len - 0x4000);
	CHECK(dart4_translate(&d, sdva + len) == 0);                       /* guard page */
	CHECK(dart4_translate(&d, 0x1000ACA4000ull) == 0x10bd12d8000ull);  /* firmware intact */
	CHECK(fw_writes == 0 && tlb_cmds == 0);
	printf("   tables allocated %u\n", d.stat_tables);

	printf("== unmap: RANGE invalidate of our DVAs only\n");
	CHECK(dart4_unmap(&d, dva, 0x100000) == 0 && dart4_translate(&d, dva) == 0);
	CHECK(tlb_cmds == 1 && last_tlb == ((1u << 8) | 23 | (1u << 14) | (1u << 13) | (1u << 15)));
	CHECK(tlb_start == (uint32_t)((dva >> 14) << 2) && tlb_end == (uint32_t)(((dva + 0x100000 - 1) >> 14) << 2));
	CHECK(dart4_unmap(&d, 0x1000ACA4000ull, 0x4000) == -1 && dart4_translate(&d, 0x1000ACA4000ull));  /* not ours */

	printf("== refusals\n");
	CHECK(dart4_map(&d, shm_pa + 0x1000, 0x4000) == 0);
	CHECK(dart4_map(&d, shm_pa, 0) == 0);
	CHECK(dart4_map(&d, 1ull << 44, 0x4000) == 0);         /* PA beyond 38 bits */
	alloc_fail = 1;
	uint64_t r = dart4_map(&d, shm_pa, 64ull << 20);      /* may succeed in existing tables; must not write fw */
	(void)r;
	alloc_fail = 0;
	CHECK(fw_writes == 0);

	printf("== the slot fills up: next owned slot; old DART version: no V2 bit\n");
	struct dart4 e2;
	build_fw();
	regs[0x08 / 4] = 0x0101;
	CHECK(dart4_init(&e2, &IO, DART, 23, MEM_BASE, wb, ws, 2) == 0);
	e2.next_dva = (31ull << 36) + (1ull << 36) - 0x8000;
	uint64_t x = dart4_map(&e2, shm_pa, 0x10000);
	CHECK(x == (30ull << 36));
	CHECK(dart4_unmap(&e2, x, 0x10000) == 0 && last_tlb == ((1u << 8) | 23 | (1u << 14) | (1u << 13)));

	printf("== firmware claims our slot before first use\n");
	struct dart4 f;
	build_fw();
	CHECK(dart4_init(&f, &IO, DART, 23, MEM_BASE, wb, ws, 1) == 0);
	uint64_t t = alloc_table(0);
	memcpy(mem + 8 * 31, &(uint64_t){ tblpte(t) }, 8);    /* firmware takes slot 31 */
	fw_hi = MEM_BASE + 8 * 32;
	int fwb = fw_writes;
	CHECK(dart4_map(&f, shm_pa, 0x4000) == 0 && fw_writes == fwb);

	printf("== no vacant slot in the window\n");
	struct dart4 g;
	build_fw();
	for (int s = 16; s < 32; s++) wr64(0, MEM_BASE + 8 * s, tblpte(MEM_BASE + 0x4000));
	CHECK(dart4_init(&g, &IO, DART, 23, MEM_BASE, wb, ws, 1) == -3);

	printf("== TLB busy that never clears on unmap\n");
	struct dart4 h;
	build_fw();
	CHECK(dart4_init(&h, &IO, DART, 23, MEM_BASE, wb, ws, 1) == 0);
	uint64_t hd = dart4_map(&h, shm_pa, 0x4000);
	busy_cycles = 1 << 30;
	CHECK(hd && dart4_unmap(&h, hd, 0x4000) == -1 && h.failed);
	CHECK(dart4_map(&h, shm_pa, 0x4000) == 0);           /* a failed DART refuses further maps */
	busy_cycles = 0;

	printf("%s: %d checks, %d failures\n", fails ? "FAIL" : "PASS", checks, fails);
	free(mem);
	return fails != 0;
}
