#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""patch_fake_v2.py IN_fake_ans.c OUT_fake_ans.c -- the twin's fake T6050 ANS, v2 (review H3 / C5).

Applied to a COPY of a resident's tests/native/fake_ans.c (r4.x). The v1 twin (offline-twin-20261109)
made the fake REFUSE out-of-window writes (status 0x80, nothing stored), so the backing file could never
change outside the window and the disk audit proved only the fake's own behaviour. v2:

  * durable media = the sparse backing file (same as v1: base_block() reads it, FLUSH/SHN/cache-full
    write the volatile cache THROUGH to it);
  * the fake ACCEPTS and STORES every write-like command wherever it points (any LBA below NSZE):
    WRITE 0x01 (also when rw was never enabled), WRITE ZEROES 0x08, WRITE UNCORRECTABLE 0x04 (stores a
    0xDE pattern), DSM 0x09 with AD (stores zeros); only an LBA past the end of the disk is refused (0x80);
  * it counts writes_outside_window for every one of them (independent of the resident);
  * it LOGS every command that reaches it, admin and I/O, to $S1TWIN_WLOG as JSON lines: queue, opcode,
    nsid, slba, nlb, status, and for data-carrying writes an FNV-1a-64 of the stored data;
  * tw_drain_cache_to_file() persists the volatile cache at the end of a run, so an unflushed stray
    write is visible in the image too (worst case: the cache lands eventually);
  * S1TWIN_LEGACY_REFUSE=1 restores the v1 refusal (only to demonstrate that the v1 proof was blind).
"""
import sys

t = open(sys.argv[1]).read()


def sub(o, n, c=1):
    global t
    assert t.count(o) == c, (o[:70], t.count(o))
    t = t.replace(o, n)


assert "persist_cache" in t, "needs an r4 (read-write) fake"
# ---- v1 twin patches (file-backed durable media), verbatim from offline-twin-20261109/native/build-native-model.sh
sub('    F.step_cap = 20000000;', '    F.step_cap = ~0ULL; /* twin: unbounded */')
sub('#define OV_SLOTS (1u << 15)', '#define OV_SLOTS (1u << 17) /* twin: volatile cache (128K blocks); auto-flush to file when full */')
sub('static struct ov *ov_find(struct ov *t, u64 lba, bool create)',
    'static void persist_cache(void);\nunsigned long long tw_file_errors;\nstatic struct ov *ov_find(struct ov *t, u64 lba, bool create)')
sub('    fprintf(stderr, "FAIL: fake overlay full\\n");\n    abort();',
    '    if (create && t == ov_cache) { persist_cache(); return ov_find(t, lba, create); }\n    fprintf(stderr, "FAIL: fake overlay full\\n");\n    abort();')
sub('        if (ov_cache[i].used == 1) memcpy(ov_find(ov_media, ov_cache[i].lba, true)->data, ov_cache[i].data, 4096);\n    ov_clear(ov_cache);',
    '        if (ov_cache[i].used == 1) { extern bool fake_file_write_block(u64, const u8 *); if (!fake_file_write_block(ov_cache[i].lba, ov_cache[i].data)) tw_file_errors++; }\n    ov_clear(ov_cache);\n    { extern bool fake_disk_sync(void); fake_disk_sync(); }')
sub('static void base_block(u64 lba, u8 *out)\n{\n    memset(out, 0, 4096);\n    if (lba == 1 || lba == F_NSZE - 1) {',
    'void fake_disk_block_synth(u64 lba, u8 *out);\nstatic void base_block(u64 lba, u8 *out) { extern void fake_file_read_block(u64, u8 *); fake_file_read_block(lba, out); }\nvoid fake_disk_block_synth(u64 lba, u8 *out)\n{\n    memset(out, 0, 4096);\n    if (lba == 1 || lba == F_NSZE - 1) {')

# ---- v2: logging + accept-everything helpers (inserted before process())
helpers = r'''
/* ---- twin v2 (write-safety-20261110): record every command, accept every write-like command ---- */
#include <stdio.h>
unsigned long long tw_outside_blocks, tw_logged, tw_beyond_capacity;
static FILE *tw_log;
static int tw_legacy = -1;
static u64 tw_fnv(const u8 *p, u64 n, u64 h) { for (u64 i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; } return h; }
static void tw_emit(bool admin, u8 op, u32 nsid, u64 slba, u64 nlb, u16 status, bool has_hash, u64 hash, const char *what)
{
    if (!tw_log) { const char *p = getenv("S1TWIN_WLOG"); if (!p) return; tw_log = fopen(p, "a"); if (!tw_log) return; }
    fprintf(tw_log, "{\"q\":\"%s\",\"op\":%u,\"nsid\":%u,\"slba\":%llu,\"nlb\":%llu,\"status\":%u,\"what\":\"%s\"",
            admin ? "admin" : "io", op, nsid, (unsigned long long)slba, (unsigned long long)nlb, status, what);
    if (has_hash) fprintf(tw_log, ",\"fnv1a64\":\"%016llx\"", (unsigned long long)hash);
    fprintf(tw_log, "}\n");
    fflush(tw_log);
    tw_logged++;
}
static bool tw_outside(u64 slba, u64 nlb) { return slba < W_START || slba >= W_START + W_BLOCKS || nlb > W_START + W_BLOCKS - slba; }
/* Store blocks [slba, slba+nlb) from src (NULL = fill byte) into the volatile cache. */
static u16 tw_store(u64 slba, u64 nlb, const u8 *src, u8 fill, u64 *hash)
{
    if (tw_legacy < 0) { const char *e = getenv("S1TWIN_LEGACY_REFUSE"); tw_legacy = e && *e == '1'; }
    if (tw_outside(slba, nlb)) {   /* counted even when it points past the end of the disk */
        F.writes_outside_window++;
        tw_outside_blocks += nlb;
        if (tw_legacy) return 0x80; /* v1 behaviour: refuse, store nothing */
    }
    if (!nlb || slba >= F_NSZE || nlb > F_NSZE - slba) { tw_beyond_capacity++; return 0x80; }
    u64 h = 0xcbf29ce484222325ull;
    u8 blk[4096];
    for (u64 i = 0; i < nlb; i++) {
        if (src) memcpy(blk, src + i * 4096, 4096); else memset(blk, fill, 4096);
        memcpy(ov_find(ov_cache, slba + i, true)->data, blk, 4096);
        h = tw_fnv(blk, 4096, h);
    }
    if (!F.write_blocks || slba < F.wmin) F.wmin = slba;
    if (slba + nlb - 1 > F.wmax) F.wmax = slba + nlb - 1;
    F.write_blocks += nlb;
    *hash = h;
    return 0;
}
void tw_drain_cache_to_file(void) { persist_cache(); }

static void process(bool admin, u32 tag)
{'''
sub('\nstatic void process(bool admin, u32 tag)\n{', helpers)

# ---- WRITE: accept wherever it points; log with data hash
sub('''                if (slba < W_START || slba >= W_START + W_BLOCKS || nlb > W_START + W_BLOCKS - slba) {
                    F.writes_outside_window++;
                    status = 0x80; /* LBA out of range: nothing stored */
                } else if (F.cfg.write_error) {
                    status = 0x281;
                } else {
                    for (unsigned i = 0; i < nlb; i++) {
                        u8 blk[4096];
                        if (!dma_read(pages[i], blk, 4096)) { status = 0x4; break; }
                        memcpy(ov_find(ov_cache, slba + i, true)->data, blk, 4096);
                    }
                    if (!F.write_blocks || slba < F.wmin) F.wmin = slba;
                    if (slba + nlb - 1 > F.wmax) F.wmax = slba + nlb - 1;
                    F.write_blocks += nlb;
                }''', '''                if (F.cfg.write_error && !tw_outside(slba, nlb)) {
                    status = 0x281;
                } else {
                    static u8 wbuf[256 * 4096];
                    for (unsigned i = 0; i < nlb && !status; i++)
                        if (!dma_read(pages[i], wbuf + (u64)i * 4096, 4096)) status = 0x4;
                    u64 h = 0;
                    if (!status) status = tw_store(slba, nlb, wbuf, 0, &h);
                    tw_emit(false, op, nsid, slba, nlb, status, !status, h, "write");
                    tw_logged_this = true;
                }''')
sub('''        } else if (op == 0x01 && F.cfg.allow_rw) {''', '''        } else if (op == 0x01 || op == 0x08 || op == 0x04 || op == 0x09) {
            /* v2: rw never enabled in the fake, or another write-like opcode: still accepted and stored. */
            if (op == 0x01 && F.cfg.allow_rw) goto tw_rw_write;
            u64 slba = cdw10 | ((u64)cdw11 << 32), h = 0;
            unsigned nlb = (cdw12 & 0xffff) + 1;
            if (op == 0x09) {
                /* DSM: cdw10[7:0] = ranges - 1; cdw11 bit 2 = deallocate. Ranges are 16 bytes at PRP1. */
                u8 rng[256 * 16]; unsigned nr = (cdw10 & 0xff) + 1;
                if (!dma_read(prp1, rng, (u64)nr * 16)) status = 0x4;
                for (unsigned i = 0; i < nr && !status; i++) {
                    u32 rn = get32(rng + i * 16, 4); u64 rs = get64(rng + i * 16, 8);
                    if (cdw11 & 4) status = tw_store(rs, rn, NULL, 0, &h);
                    tw_emit(false, op, nsid, rs, rn, status, false, 0, (cdw11 & 4) ? "dsm-deallocate" : "dsm-hint");
                }
            } else if (op == 0x01) {
                static u64 pg[256]; static u8 wb[256 * 4096];
                if (!prp_pages(prp1, prp2, nlb, pg)) status = 0x4;
                for (unsigned i = 0; i < nlb && !status; i++) if (!dma_read(pg[i], wb + (u64)i * 4096, 4096)) status = 0x4;
                if (!status) status = tw_store(slba, nlb, wb, 0, &h);
                tw_emit(false, op, nsid, slba, nlb, status, !status, h, "write-without-rw");
                F.io_writes++;
            } else {
                status = tw_store(slba, nlb, NULL, op == 0x08 ? 0x00 : 0xde, &h);
                tw_emit(false, op, nsid, slba, nlb, status, false, 0, op == 0x08 ? "write-zeroes" : "write-uncorrectable");
            }
            tw_logged_this = true;
        } else if (op == 0x01 && F.cfg.allow_rw) {
        tw_rw_write:;''')
# every command (that was not already logged above) is logged at io_done
sub('''io_done:
    if (admin && F.cfg.admin_error) status = 2;
    F.doorbell_seq = F.steps;''', '''io_done:
    if (admin && F.cfg.admin_error) status = 2;
    if (!tw_logged_this) {
        u64 sl = admin ? 0 : (cdw10 | ((u64)cdw11 << 32));
        tw_emit(admin, op, nsid, sl, admin ? 0 : (u64)(cdw12 & 0xffff) + 1, status, false, 0,
                admin ? "admin" : (op == 0x02 ? "read" : op == 0x00 ? "flush" : "other"));
    }
    F.doorbell_seq = F.steps;''')
sub('''    static u8 data[256 * 4096];''', '''    static u8 data[256 * 4096];
    bool tw_logged_this = false;''')
open(sys.argv[2], "w").write(t)
