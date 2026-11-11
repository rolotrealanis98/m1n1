/* SPDX-License-Identifier: MIT */
/* Independent review: GPT parser / window confinement fuzz for r3 native_window.c.
 * The actual native_window.c is #included (its one-shot statics are reset per case);
 * ans_ro is replaced by a stub disk that LOGS every physical LBA read. Oracles:
 *  - s1n_open succeeds  =>  an independent re-implementation of every documented check
 *    (header sig/rev/size/CRC/MyLBA, entry size/count, entries CRC, usable range < NSZE,
 *    entry type+GUID+start+size == launcher values, inside usable, no overlap) accepts too;
 *  - before verification the parser reads only LBA 1 and the declared entry sectors (< NSZE);
 *  - after verification every read (random, edge, overflow) stays in [start, start+blocks)
 *    and out-of-window requests issue no read at all;
 *  - after a failure nothing is exposed (blocks()==0, reads refused, zero disk reads).
 * Host only. Build with ASan+UBSan. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "native_window.c"

#define NSZE_DEF 244276265ULL
#define W_START 174213120ULL
#define W_BLOCKS 24424454ULL
static const u8 TYPE[16] = {0xa2,0xa0,0xd0,0xeb,0xe5,0xb9,0x33,0x44,0x87,0xc0,0x68,0xb6,0xb7,0x26,0x99,0xc7};
static const u8 GUID[16] = {0x59,0x72,0xac,0x89,0xa4,0xfa,0x10,0x41,0xb6,0x21,0xe9,0xa2,0x93,0x4b,0x45,0xc1};
static const u8 APFS[16] = {0xef,0x57,0x34,0x7c,0x00,0x00,0xaa,0x11,0xaa,0x11,0x00,0x30,0x65,0x43,0xec,0xac};

static unsigned checks, cases, accepted, oracle_rejects_but_ok, false_negatives;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL:%d %s (case %u)\n", __LINE__, #x, cases); exit(1); } } while (0)

/* ---- stub ANS disk ---- */
static u8 hdr[4096], ents[32 * 4096];
static u32 crc(const u8 *, u64);
static void p32(u8 *, unsigned, u32);
static void p64(u8 *, unsigned, u64);
static u32 g32(const u8 *, unsigned);
static u64 ents_lba, nsze = NSZE_DEF;
static u8 bhdr[4096], bents[4 * 4096];
static u64 bhdr_lba, bents_lba;   /* where the stub disk holds the backup (normally NSZE-1 / NSZE-5) */
static unsigned bk_cases, bk_accepted, bk_mutated_rejected, bk_directed;
static bool init_ok, verifying;
static u64 log_lba[4096]; static unsigned nlog;
bool ans_ro_init(void) { return init_ok; }
enum ans_ro_state ans_ro_state(void) { return ANS_RO_LIVE; }
void ans_ro_abort(enum ans_ro_reason r) { (void)r; }
u64 ans_ro_nsze(void) { return nsze; }
u32 ans_ro_lbads(void) { return 12; }
void ans_ro_poll(void) {}
void ans_ro_scrub(void) {} /* r3.1: native_window.c scrubs the bounce after verification */
bool ans_ro_shutdown(void) { return true; }
bool ans_ro_read_block(u64 lba, void *out)
{
    CHECK(lba < nsze);                         /* ans_ro itself refuses lba >= nsze */
    if (nlog < 4096) log_lba[nlog++] = lba;
    if (lba == 1) memcpy(out, hdr, 4096);
    else if (lba == bhdr_lba) memcpy(out, bhdr, 4096);
    else if (lba >= bents_lba && lba < bents_lba + 4) memcpy(out, bents + (lba - bents_lba) * 4096, 4096);
    else if (lba >= ents_lba && lba < ents_lba + 32) memcpy(out, ents + (lba - ents_lba) * 4096, 4096);
    else { memset(out, 0, 4096); memcpy(out, &lba, 8); }
    return true;
}

bool ans_ro_read_gpt(u64 lba, void *out) { return ans_ro_read_block(lba, out); }

/* r4: new ans_ro entry points used by native_window.c. ans_ro_xfer logs every block exactly
 * like ans_ro_read_block, and refuses (like the driver) anything outside the window. */
static u8 xfer[1u << 20];
static bool win_set; static u64 ws, wb;
bool ans_ro_set_window(bool rw, u32 max_blocks)
{ (void)rw; (void)max_blocks; if (win_set) return false; win_set = true; ws = S1N_PIN_START; wb = S1N_PIN_BLOCKS; return true; }
bool ans_ro_xfer(bool write, u64 lba, u32 blocks, u64 offset)
{
    CHECK(!write);
    if (!win_set || !blocks || lba < ws || lba - ws >= wb || blocks > wb - (lba - ws)) return false;
    for (u32 i = 0; i < blocks; i++) {
        CHECK(lba + i < nsze);
        if (nlog < 4096) log_lba[nlog++] = lba + i;
        memset(xfer + offset + 4096ull * i, 0, 4096); u64 v = lba + i; memcpy(xfer + offset + 4096ull * i, &v, 8);
    }
    return true;
}
bool ans_ro_flush(void) { return false; }
void *ans_ro_xfer_buffer(size_t *capacity) { *capacity = sizeof(xfer); return xfer; }
void ans_ro_canary(u64 *a, u64 *b, u64 *c, u64 *d) { *a = UINT64_MAX; *b = 0; *c = 0; *d = 0; }

static u64 rs;
static u64 rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static void p32(u8 *b, unsigned o, u32 v) { memcpy(b + o, &v, 4); }
static void p64(u8 *b, unsigned o, u64 v) { memcpy(b + o, &v, 8); }
static u32 g32(const u8 *b, unsigned o) { u32 v; memcpy(&v, b + o, 4); return v; }
static u64 g64(const u8 *b, unsigned o) { u64 v; memcpy(&v, b + o, 8); return v; }
static u32 crc(const u8 *p, u64 n)  /* independent bytewise CRC32 (table) */
{
    static u32 t[256]; static int init;
    if (!init) { for (u32 i = 0; i < 256; i++) { u32 c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1; t[i] = c; } init = 1; }
    u32 c = ~0u; for (u64 i = 0; i < n; i++) c = t[(c ^ p[i]) & 0xff] ^ (c >> 8); return ~c;
}
static void entry(unsigned idx, const u8 *type, const u8 *guid, u64 first, u64 last)
{
    u8 *e = ents + (u64)idx * 128;
    memcpy(e, type, 16); memcpy(e + 16, guid, 16); p64(e, 32, first); p64(e, 40, last);
}
static void fix_crcs(void)
{
    u32 n = g32(hdr, 80), es = g32(hdr, 84);
    u64 eb = (u64)n * es; if (eb > sizeof ents) eb = sizeof ents;
    p32(hdr, 88, crc(ents, eb));
    u32 hs = g32(hdr, 12); if (hs > 4096) hs = 4096;
    p32(hdr, 16, 0); p32(hdr, 16, crc(hdr, hs));
}
/* A correct backup for the CURRENT primary (UEFI 5.3.2 mirror). Called explicitly, then possibly
 * mutated independently of the primary. */
static void mirror_backup(void)
{
    memcpy(bhdr, hdr, 4096);
    p64(bhdr, 24, nsze - 1); p64(bhdr, 32, 1); p64(bhdr, 72, nsze - 5);
    u32 hs = g32(hdr, 12); if (hs > 4096) hs = 4096;
    p32(bhdr, 16, 0); p32(bhdr, 16, crc(bhdr, hs));
    memcpy(bents, ents, sizeof bents);
    bhdr_lba = nsze - 1; bents_lba = nsze - 5;
}
static void fix_bcrcs(bool array_too)
{
    if (array_too) { u32 n = g32(bhdr, 80); u64 eb = (u64)n * 128; if (eb > sizeof bents) eb = sizeof bents; p32(bhdr, 88, crc(bents, eb)); }
    u32 hs = g32(bhdr, 12); if (hs > 4096) hs = 4096;
    p32(bhdr, 16, 0); p32(bhdr, 16, crc(bhdr, hs));
}
/* Backup acceptance, independent re-implementation: the backup must be an exact mirror. */
static bool oracle_backup(void)
{
    unsigned sv = nlog; u8 b[4096]; ans_ro_read_block(nsze - 1, b); nlog = sv; /* what the disk holds at NSZE-1 */
    u32 hs = g32(hdr, 12);
    if (memcmp(b, "EFI PART", 8) || g32(b, 8) != 0x00010000 || g32(b, 12) != hs) return false;
    u8 t[4096]; memcpy(t, b, hs); p32(t, 16, 0); if (crc(t, hs) != g32(b, 16)) return false;
    const u8 disk[16] = S1N_PIN_DISK_BYTES;
    if (g64(b, 24) != nsze - 1 || g64(b, 32) != 1 || g64(b, 40) != g64(hdr, 40) || g64(b, 48) != g64(hdr, 48) ||
        memcmp(b + 56, disk, 16) || g64(b, 72) != nsze - 5 || g32(b, 80) != g32(hdr, 80) || g32(b, 84) != g32(hdr, 84) ||
        g32(b, 88) != g32(hdr, 88)) return false;
    u64 eb = (u64)g32(hdr, 80) * 128;
    for (u64 s = 0; s * 4096 < eb; s++) {
        u8 a[4096]; ans_ro_read_block(nsze - 5 + s, a); nlog = sv;
        u64 len = eb - s * 4096 < 4096 ? eb - s * 4096 : 4096;
        if (memcmp(a, ents + s * 4096, len)) return false;
    }
    return true;
}
static u8 valid_ents[sizeof ents], valid_hdr[4096];
static bool mutate_backup(void)   /* returns true if something was changed */
{
    unsigned k = 1 + rnd() % 3; bool any = false;
    for (unsigned j = 0; j < k; j++) {
        any = true;
        switch (rnd() % 16) {
        case 0: bhdr[rnd() % 92] ^= (u8)(1u << (rnd() % 8)); break;                       /* any header bit */
        case 1: p64(bhdr, 24, (u64[]){1, nsze - 2, nsze, nsze - 33}[rnd() % 4]); break;      /* MyLBA */
        case 2: p64(bhdr, 32, (u64[]){0, 2, nsze - 1}[rnd() % 3]); break;                    /* AlternateLBA */
        case 3: p64(bhdr, 40, g64(bhdr, 40) + 1); break;                                    /* FirstUsable */
        case 4: p64(bhdr, 48, g64(bhdr, 48) - 1); break;                                    /* LastUsable */
        case 5: bhdr[56 + rnd() % 16] ^= 1; break;                                          /* disk GUID */
        case 6: p64(bhdr, 72, (u64[]){2, nsze - 33, nsze - 4, nsze - 6}[rnd() % 4]); break;  /* entries LBA */
        case 7: p32(bhdr, 80, (u32[]){127, 129, 6, 7}[rnd() % 4]); break;                   /* count */
        case 8: bents[6 * 128 + 16 + rnd() % 16] ^= 1; break;                               /* entry 7 GUID */
        case 9: p64(bents + 6 * 128, 32 + 8 * (rnd() % 2), g64(bents + 6 * 128, 32 + 8 * (rnd() % 2)) + 1 - 2 * (rnd() % 2)); break; /* entry 7 range */
        case 10: bents[6 * 128 + rnd() % 16] ^= 1; break;                                   /* entry 7 type */
        case 11: { unsigned i = rnd() % 128; bents[i * 128 + rnd() % 128] ^= (u8)(1u << (rnd() % 8)); break; } /* any entry */
        case 12: memset(bhdr, 0, 4096); break;                                              /* no backup header */
        case 13: bhdr_lba = (u64[]){nsze - 2, nsze - 33, 2}[rnd() % 3]; if (bhdr_lba == 2) bhdr_lba = nsze - 6; break; /* wrong place */
        case 14: bents_lba = (u64[]){nsze - 6, nsze - 4, nsze - 33}[rnd() % 3]; break;      /* array wrong place */
        default:                                                                            /* stale: backup of the unmutated table */
            memcpy(bents, valid_ents, sizeof bents); memcpy(bhdr, valid_hdr, 4096);
            p64(bhdr, 24, nsze - 1); p64(bhdr, 32, 1); p64(bhdr, 72, nsze - 5); fix_bcrcs(false);
            break;
        }
    }
    unsigned f = rnd() % 10;
    if (f < 5) fix_bcrcs(true);                  /* backup self-consistent: only the cross-check can catch it */
    else if (f < 8) fix_bcrcs(false);
    return any;
}
static void make_valid(void)
{
    memset(hdr, 0, sizeof hdr); memset(ents, 0, sizeof ents);
    ents_lba = 2; nsze = NSZE_DEF;
    memcpy(hdr, "EFI PART", 8); p32(hdr, 8, 0x00010000); p32(hdr, 12, 92);
    p64(hdr, 24, 1); p64(hdr, 32, nsze - 1); p64(hdr, 40, 6); p64(hdr, 48, nsze - 6);
    const u8 disk[16] = S1N_PIN_DISK_BYTES; memcpy(hdr + 56, disk, 16);
    p64(hdr, 72, 2); p32(hdr, 80, 128); p32(hdr, 84, 128);
    u8 g[16]; for (int i = 1; i <= 6; i++) { memset(g, 0x10 * i, 16); entry(i - 1, APFS, g, 6 + (u64)(i - 1) * 1000000, 6 + (u64)i * 1000000 - 1); }
    entry(6, TYPE, GUID, W_START, W_START + W_BLOCKS - 1);
    fix_crcs();
    mirror_backup();
}
static struct s1n_window launcher(void)
{
    struct s1n_window w = {W_START, W_BLOCKS, 7, 0, {0}, {0}};
    memcpy(w.type_guid, TYPE, 16); memcpy(w.unique_guid, GUID, 16); return w;
}
static void reset_window(void)
{ have_snapshot = false; opened = false; verified = false; rw = false; max_xfer = 0; win_set = false; memset(&st, 0, sizeof st); memset(&win, 0, sizeof win); nlog = 0; } /* r4: rw/max_xfer/window reset */

/* Independent oracle: every documented acceptance condition. */
static bool oracle(const struct s1n_window *w)
{
    if (w->start_lba != S1N_PIN_START || w->blocks != S1N_PIN_BLOCKS || w->entry_index != 7 || memcmp(w->unique_guid, GUID, 16)) return false;
    const u8 disk[16] = S1N_PIN_DISK_BYTES;
    if (memcmp(hdr + 56, disk, 16) || nsze != S1N_PIN_NSZE) return false;
    if (!w || w->flags || w->entry_index < 1 || w->entry_index > 128 || !w->blocks || w->start_lba < 34 ||
        w->start_lba > UINT64_MAX - w->blocks || memcmp(w->type_guid, TYPE, 16)) return false;
    bool zg = true; for (int i = 0; i < 16; i++) zg &= !w->unique_guid[i]; if (zg) return false;
    if (w->start_lba + w->blocks > nsze) return false;
    if (memcmp(hdr, "EFI PART", 8) || g32(hdr, 8) != 0x00010000) return false;
    u32 hs = g32(hdr, 12); if (hs < 92 || hs > 4096) return false;
    u8 t[4096]; memcpy(t, hdr, hs); p32(t, 16, 0); if (crc(t, hs) != g32(hdr, 16)) return false;
    u64 my = g64(hdr, 24), alt = g64(hdr, 32), fu = g64(hdr, 40), lu = g64(hdr, 48), el = g64(hdr, 72);
    u32 n = g32(hdr, 80), es = g32(hdr, 84);
    if (alt != nsze - 1) return false; /* r4.1 (= r3.2) capacity cross-check */
    if (my != 1 || es != 128 || !n || n > 128 || el != 2 || el + ((n * 128 + 4095) / 4096) > fu || el >= fu || fu > lu || lu >= nsze - 5 || w->entry_index > n) return false;
    if (el != ents_lba) return false;   /* our disk only holds the array at ents_lba */
    if (crc(ents, (u64)n * 128) != g32(hdr, 88)) return false;
    const u8 *e = ents + (u64)(w->entry_index - 1) * 128;
    if (memcmp(e, w->type_guid, 16) || memcmp(e + 16, w->unique_guid, 16)) return false;
    u64 f = g64(e, 32), l = g64(e, 40);
    if (f != w->start_lba || l < f || l - f + 1 != w->blocks || f < fu || l > lu) return false;
    for (u32 i = 0; i < n; i++) {
        if (i == w->entry_index - 1) continue;
        const u8 *o = ents + (u64)i * 128; bool z = true; for (int k = 0; k < 16; k++) z &= !o[k];
        if (z) continue;
        u64 of = g64(o, 32), ol = g64(o, 40);
        if (of <= l && ol >= f) return false;
    }
    return true;
}

static void mutate(struct s1n_window *w)
{
    unsigned k = 1 + rnd() % 4;
    for (unsigned j = 0; j < k; j++) {
        switch (rnd() % 27) {
        case 0: hdr[rnd() % 92] ^= (u8)(1u << (rnd() % 8)); break;
        case 1: p32(hdr, 12, (u32[]){0, 91, 92, 93, 512, 4096, 4097, 0xffffffffu}[rnd() % 8]); break;
        case 2: p64(hdr, 24, (u64[]){0, 1, 2, UINT64_MAX}[rnd() % 4]); break;
        case 25: p64(hdr, 32, (u64[]){0, nsze - 2, nsze - 1, nsze, UINT64_MAX}[rnd() % 5]); break;
        case 3: p64(hdr, 40, (u64[]){0, 1, 5, 6, 34, W_START, W_START + 1, nsze, UINT64_MAX}[rnd() % 9]); break;
        case 4: p64(hdr, 48, (u64[]){0, W_START + W_BLOCKS - 2, W_START + W_BLOCKS - 1, nsze - 1, nsze, UINT64_MAX}[rnd() % 6]); break;
        case 5: { u64 v = (u64[]){0, 1, 2, 3, 5, 6, 33, nsze - 1, UINT64_MAX, UINT64_MAX - 1}[rnd() % 10]; p64(hdr, 72, v); if (v >= 2 && v < 1000) ents_lba = v; break; }
        case 6: p32(hdr, 80, (u32[]){0, 1, 6, 7, 8, 32, 127, 128, 129, 0x80000000u, 0xffffffffu}[rnd() % 11]); break;
        case 7: p32(hdr, 84, (u32[]){0, 64, 127, 128, 129, 256, 0xffffffffu}[rnd() % 7]); break;
        case 8: ents[6 * 128 + rnd() % 128] ^= (u8)(1u << (rnd() % 8)); break;
        case 9: p64(ents + 6 * 128, 32, W_START + (rnd() % 3) - 1); break;
        case 10: p64(ents + 6 * 128, 40, (u64[]){W_START - 1, W_START, W_START + W_BLOCKS - 2, W_START + W_BLOCKS, UINT64_MAX}[rnd() % 5]); break;
        case 11: { unsigned i = rnd() % 128; if (i == 6) break; u8 g[16]; memset(g, (int)rnd(), 16);
                   u64 f = (u64[]){W_START - 10, W_START, W_START + 5, W_START + W_BLOCKS - 1, W_START + W_BLOCKS}[rnd() % 5];
                   entry(i, APFS, g, f, f + (rnd() % 20)); break; }                       /* overlapping/adjacent */
        case 12: { unsigned i = rnd() % 128; if (i == 6) break; u8 g[16]; memset(g, 3, 16);
                   entry(i, APFS, g, UINT64_MAX - 5, 2); break; }                       /* inverted range */
        case 13: memcpy(ents + 6 * 128, APFS, 16); break;
        case 14: ents[6 * 128 + 16 + rnd() % 16] ^= 1; break;
        case 15: memset(ents + 6 * 128, 0, 128); break;
        case 16: { unsigned i = rnd() % 128; memcpy(ents + (u64)i * 128, ents + 6 * 128, 128); break; } /* duplicate of entry 7 */
        case 17: w->entry_index = (u32[]){0, 1, 6, 7, 8, 128, 129, 0xffffffffu}[rnd() % 8]; break;
        case 18: w->start_lba = (u64[]){0, 33, 34, W_START - 1, W_START + 1, UINT64_MAX}[rnd() % 6]; break;
        case 19: w->blocks = (u64[]){0, 1, W_BLOCKS - 1, W_BLOCKS + 1, UINT64_MAX}[rnd() % 5]; break;
        case 20: w->flags = (u32)rnd(); break;
        case 21: w->unique_guid[rnd() % 16] ^= 0x40; break;
        case 22: memset(w->unique_guid, 0, 16); memset(ents + 6 * 128 + 16, 0, 16); break;
        case 23: nsze = (u64[]){W_START + W_BLOCKS - 1, W_START + W_BLOCKS, NSZE_DEF, 1000}[rnd() % 4]; break;
        case 24: memcpy(hdr, "EFI PARU", 8); break;
        case 26: hdr[92 + rnd() % 200] ^= 0xff; break;
        default: break;
        }
    }
    if (rnd() % 10 < 8) fix_crcs();   /* usually make the CRCs consistent so deeper checks run */
}

static void probe_window(void)
{
    static u8 buf[256 * 4096]; /* r4: 256-block limit (r3: 32) */
    const u64 L = W_BLOCKS;
    struct { u64 lba; u32 n; bool ok; } pr[] = {
        {0, 1, 1}, {L - 1, 1, 1}, {L - 32, 32, 1}, {1000, 32, 1}, {L - 1, 2, 0}, {L, 1, 0}, {L - 31, 32, 0},
        {UINT64_MAX, 1, 0}, {UINT64_MAX - 31, 32, 0}, {UINT64_MAX - L, 32, 0}, {0, 0, 0}, {0, 257, 0}, {0, 256, 1}, {L - 256, 256, 1}, {L - 255, 256, 0},
        {(u64)-(long long)W_START, 1, 0}, {(u64)-(long long)W_START - 1, 32, 0},
    };
    for (unsigned i = 0; i < sizeof pr / sizeof pr[0]; i++) {
        nlog = 0;
        bool r = s1n_read(pr[i].lba, pr[i].n, buf);
        CHECK(r == pr[i].ok);
        if (!r) CHECK(nlog == 0);
        else { CHECK(nlog == pr[i].n);
               for (unsigned k = 0; k < nlog; k++) CHECK(log_lba[k] == W_START + pr[i].lba + k); }
    }
    for (unsigned j = 0; j < 200; j++) {
        u64 lba = rnd() % 4 ? rnd() % (L + 300) : rnd(); u32 n = (u32)(rnd() % 258);
        nlog = 0; bool r = s1n_read(lba, n, buf);
        bool want = n && n <= 256 && lba < L && n <= L - lba;
        CHECK(r == want);
        for (unsigned k = 0; k < nlog; k++) CHECK(log_lba[k] >= W_START && log_lba[k] - W_START < W_BLOCKS);
        if (!want) CHECK(nlog == 0);
    }
}

int main(int argc, char **argv)
{
    unsigned iters = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 200000;
    rs = 0x2545F4914F6CDD1DULL;
    /* baseline valid */
    make_valid(); reset_window(); init_ok = true;
    struct s1n_window w = launcher();
    CHECK(s1n_open(&w) && s1n_blocks() == W_BLOCKS);
    probe_window();
    /* ANS refused: nothing read, nothing exposed */
    make_valid(); reset_window(); init_ok = false; w = launcher();
    CHECK(!s1n_open(&w) && nlog == 0 && s1n_blocks() == 0);
    init_ok = true;
    /* secA directed: backup NOT built from the primary. Each must be refused. */
    for (unsigned d = 0; d < 10; d++) {
        make_valid(); reset_window(); w = launcher();
        switch (d) {
        case 0: bents[6 * 128 + 16] ^= 1; fix_bcrcs(true); break;                         /* entry 7 GUID differs, consistent */
        case 1: p64(bents + 6 * 128, 40, W_START + W_BLOCKS - 2); fix_bcrcs(true); break; /* entry 7 range differs, consistent */
        case 2: bhdr[16] ^= 1; break;                                                     /* bad backup header CRC */
        case 3: bents[6 * 128 + 16] ^= 1; break;                                          /* entry 7 differs, backup CRC not fixed */
        case 4: bhdr_lba = nsze - 2; break;                   /* backup at wrong LBA, NSZE-1 empty */
        case 5: bents_lba = nsze - 33; break;                                             /* backup array at wrong LBA */
        case 6: p64(bhdr, 24, nsze - 2); fix_bcrcs(false); break;                         /* backup claims wrong MyLBA */
        case 7: bents[1 * 128 + 40] ^= 1; fix_bcrcs(true); break;                         /* differs only in entry 2 */
        case 8: memcpy(bhdr, hdr, 4096); break;                                           /* primary copy at NSZE-1 */
        case 9: bents[41 * 128 + 16] ^= 1; break;  /* T1 overlay: array sector 2 only differs, header (incl. CRC field) untouched */
        }
        bool ok = s1n_open(&w);
        printf("{\"directed_backup\":%u,\"accepted\":%d,\"reason\":%u}\n", d, ok, (unsigned)st.reason);
        CHECK(!ok && st.reason == S1N_E_BACKUP && s1n_blocks() == 0);
        CHECK(!oracle_backup());
        bk_directed++;
    }
    make_valid(); reset_window(); w = launcher();
    CHECK(s1n_open(&w) && oracle_backup());              /* control: the mirror itself is accepted */
    for (unsigned i = 0; i < iters; i++) {
        cases++;
        make_valid(); memcpy(valid_ents, ents, sizeof ents); memcpy(valid_hdr, hdr, 4096);
        reset_window(); w = launcher(); mutate(&w);
        mirror_backup();
        bool bk = rnd() % 2 && mutate_backup();
        struct s1n_window wcopy = w;
        bool ok = s1n_open(&w);
        bool want = oracle(&wcopy);
        bool pwant = want;
        if (want) want = oracle_backup();
        if (bk && pwant) { bk_cases++; if (ok) bk_accepted++; else bk_mutated_rejected++; }
        /* pre-verification reads: LBA 1, then the declared entry sectors only */
        u64 el = g64(hdr, 72); u32 n = g32(hdr, 80);
        for (unsigned k = 0; k < nlog; k++) {
            CHECK(log_lba[k] == 1 || (log_lba[k] >= el && log_lba[k] < el + 4 && n <= 128) ||
                  (log_lba[k] >= nsze - 5 && log_lba[k] < nsze));   /* the resident never follows a moved backup */
            CHECK(log_lba[k] < nsze);
            CHECK(log_lba[k] < W_START || log_lba[k] >= W_START + W_BLOCKS || el >= W_START); /* GPT area only */
        }
        if (ok) {
            accepted++;
            if (!want) { oracle_rejects_but_ok++;
                fprintf(stderr, "DEFECT: accepted a table the oracle rejects (case %u)\n", cases); exit(1); }
            CHECK(s1n_blocks() == wcopy.blocks && win.start_lba == wcopy.start_lba);
            if (wcopy.start_lba == W_START && wcopy.blocks == W_BLOCKS) probe_window();
        } else {
            if (want) false_negatives++;
            static u8 b[4096]; nlog = 0;
            CHECK(s1n_blocks() == 0 && !s1n_verified() && !s1n_read(0, 1, b) && nlog == 0);
        }
    }
    CHECK(bk_cases > 0 && bk_mutated_rejected > 0);
    printf("{\"status\":\"pass\",\"cases\":%u,\"accepted\":%u,\"false_negatives\":%u,\"checks\":%u,"
           "\"backup_directed\":%u,\"backup_mutated_primary_valid\":%u,\"backup_mutated_accepted\":%u,\"backup_mutated_rejected\":%u}\n",
           cases, accepted, false_negatives, checks, bk_directed, bk_cases, bk_accepted, bk_mutated_rejected);
    return 0;
}
