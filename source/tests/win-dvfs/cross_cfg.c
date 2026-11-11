/* SPDX-License-Identifier: MIT */
/* cpuperf-B cross test: the launcher's real dvfs_plan() bytes (test_dvfs_plan.py --write) read as
   struct dvfs_cfg and accepted by dvfs_check_cfg, with the expected clusters and the T6050 boot probe. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#include "hv_win_dvfs_core.h"
static u64 cmd[3] = {0x400106, 0x400102, 0x400102};
static int idx(u64 a) { return (int)((a >> 24) & 0xff) - 0x10; }
static u64 rd64(void *c, u64 a) { (void)c; return (a & 0xfff) == 0x20 ? cmd[idx(a & ~0xfffUL) & 3] : ~0UL; }
static u32 rd32(void *c, u64 a) { (void)c; return (a & 0xfff) == 0x50 ? (u32)((cmd[idx(a & ~0xfffUL) & 3] & 0x1f) * 0x21) : ~0U; }
static void wr64(void *c, u64 a, u64 v) { (void)c; (void)a; (void)v; }
int main(int argc, char **argv)
{
    struct dvfs_cfg cfg;
    struct dvfs_state st;
    FILE *f = fopen(argv[1], "rb");
    size_t n = fread(&cfg, 1, sizeof(cfg) + 1, f);
    fclose(f);
    (void)argc;
    int ok = n == sizeof(cfg) && dvfs_check_cfg(&cfg, 18) == 0 && cfg.nclusters == 3 &&
             cfg.c[0].base == 0x210e20000UL && cfg.c[2].base == 0x212e20000UL && cfg.c[2].pmax == 17 &&
             cfg.c[0].perf[15] == 796 && cfg.c[2].perf[17] == 957 && cfg.cluster_of[12] == 2;
    struct dvfs_io io = {rd64, rd32, wr64, NULL};
    ok = ok && dvfs_probe(&st, &cfg, &io) == 0 && st.c[0].expected == 6 && st.c[2].expected == 2;
    printf("%s: launcher config (%zu bytes) accepted by dvfs_check_cfg and probed (p6/p2/p2)\n", ok ? "PASS" : "FAIL", n);
    return !ok;
}
