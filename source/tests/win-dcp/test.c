/* SPDX-License-Identifier: MIT */
/* dcp-pt: host test of the DCP passthrough guard decisions (src/hv/win/hv_win_dcp_logic.h). */
#include <stdio.h>
#include <string.h>
#include "../../src/hv/win/hv_win_dcp_logic.h"

static int fails;
static unsigned long checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; if (fails < 30) { printf("FAIL: " __VA_ARGS__); printf("\n"); } } } while (0)

static int passes(int d, unsigned long off, int width, unsigned long long v)
{
    return dcp_dart_write(d, off, width, v).act == DCP_PASS;
}

int main(void)
{
    /* ---- DART: the SIDs each DART admits ---- */
    for (unsigned int s = 0; s < 256; s++) {
        CHECK(dcp_sid_allowed(DCP_DART_DCP, s) == (s == 23), "dart-dcp SID %u", s);
        CHECK(dcp_sid_allowed(DCP_DART_DISP, s) == (s == 0 || s == 16), "dart-disp0 SID %u", s);
    }
    CHECK(dcp_sid_mask(DCP_DART_DCP) == (1u << 23), "dart-dcp SID mask");
    CHECK(dcp_sid_mask(DCP_DART_DISP) == ((1u << 0) | (1u << 16)), "dart-disp0 SID mask");

    /* ---- TLB_CMD (0x80): every 32-bit value, both DARTs ---- */
    for (int d = 0; d < 2; d++) {
        unsigned long n_pass = 0;
        for (unsigned long long v = 0; v < (1ull << 17); v++) {
            unsigned int val = (unsigned int)v | (unsigned int)((v >> 16) << 30); /* bits 16 -> 30: cover BUSY region too */
            struct dcp_write w = dcp_dart_write(d, DCP_DART_TLB_CMD, 2, val);
            unsigned int op = (val >> 8) & 7, sid = val & 0xff;
            int want = op == 1 && dcp_sid_allowed(d, sid) && !(val & ~(0xffu | 0x700u | 0xe000u));
            CHECK((w.act == DCP_PASS) == want, "TLB_CMD dart %d val 0x%x: act %d want %d", d, val, w.act, want);
            if (w.act == DCP_PASS) {
                n_pass++;
                CHECK(w.val == val, "TLB_CMD value altered 0x%x -> 0x%x", val, w.val);
            }
        }
        /* flush by SID x {none, V2, RANGE, STT, V2+RANGE+STT} for each own SID: 5 x (1 or 2) passes */
        CHECK(n_pass == (d == DCP_DART_DCP ? 1u : 2u) * 8u, "TLB_CMD passing values dart %d: %lu", d, n_pass);
    }
    /* the exact commands our engine and Linux issue */
    CHECK(passes(DCP_DART_DCP, 0x80, 2, 0x117), "flush SID 23 on dart-dcp (whole SID)");
    CHECK(passes(DCP_DART_DISP, 0x80, 2, 0x100), "flush SID 0 on dart-disp0 (whole SID)");
    CHECK(passes(DCP_DART_DISP, 0x80, 2, 0x110), "flush SID 16 on dart-disp0");
    CHECK(passes(DCP_DART_DISP, 0x80, 2, 0x110 | (1u << 15) | (1u << 14) | (1u << 13)), "range flush SID 16 v2");
    CHECK(!passes(DCP_DART_DISP, 0x80, 2, 0x000), "flush all refused");
    CHECK(!passes(DCP_DART_DISP, 0x80, 2, 0x400), "unlock refused");
    CHECK(!passes(DCP_DART_DISP, 0x80, 2, 0x103), "SID 3 (exclave) flush refused");
    CHECK(!passes(DCP_DART_DISP, 0x80, 2, 0x104), "SID 4 (exclave) flush refused");
    CHECK(!passes(DCP_DART_DCP, 0x80, 2, 0x118), "SID 24 (exclave) flush refused");
    CHECK(!passes(DCP_DART_DCP, 0x80, 2, 0x100), "SID 0 on dart-dcp refused");
    CHECK(!passes(DCP_DART_DCP, 0x80, 2, 0x110), "SID 16 on dart-dcp refused");
    CHECK(!passes(DCP_DART_DISP, 0x80, 2, 0x117), "SID 23 on dart-disp0 refused");
    CHECK(!passes(DCP_DART_DISP, 0x80, 2, 0x80000100u), "BUSY bit set refused");
    CHECK(!passes(DCP_DART_DISP, 0x80, 2, 0x100 | (1u << 12)), "reserved bit 12");
    CHECK(!passes(DCP_DART_DISP, 0x80, 2, 0x100 | (1u << 11)), "reserved bit 11");
    CHECK(!passes(DCP_DART_DISP, 0x80, 2, 0x100 | (1u << 16)), "reserved bit 16");

    /* ---- widths and alignment: only aligned 32-bit stores ---- */
    for (int d = 0; d < 2; d++) {
        for (int width = 0; width < 4; width++) {
            if (width == 2)
                continue;
            CHECK(!passes(d, 0x80, width, d ? 0x100 : 0x117), "dart %d TLB_CMD width %d", d, width);
            CHECK(!passes(d, 0x98, width, 0), "TLB_START width %d", width);
            CHECK(!passes(d, 0x100, width, ~0ull), "ERROR width %d", width);
        }
        CHECK(!passes(d, 0x81, 2, 0), "unaligned 0x81");
        CHECK(!passes(d, 0x82, 2, 0), "unaligned 0x82");
        CHECK(!passes(d, 0x99, 2, 0), "unaligned 0x99");
    }
    CHECK(!passes(5, 0x80, 2, 0x117), "unknown dart");

    /* ---- range registers ---- */
    for (int d = 0; d < 2; d++) {
        CHECK(passes(d, 0x98, 2, 0x1234), "TLB_START lo");
        CHECK(passes(d, 0x9c, 2, 0x1), "TLB_START hi");
        CHECK(passes(d, 0xa0, 2, 0x5678), "TLB_END lo");
        CHECK(passes(d, 0xa4, 2, 0x0), "TLB_END hi");
        CHECK(!passes(d, 0x94, 2, 0), "0x94 not a range register");
        CHECK(!passes(d, 0xa8, 2, 0), "0xa8 not a range register");
        CHECK(!passes(d, 0x84, 2, 0), "0x84 refused");
    }

    /* ---- error status ---- */
    for (int d = 0; d < 2; d++) {
        struct dcp_write w = dcp_dart_write(d, 0x100, 2, 0x8000a123u);
        CHECK(w.act == DCP_PASS && w.val == 0x8000a123u, "ERROR clear value");
        w = dcp_dart_write(d, 0x1c0, 2, 0xffffffffu);
        CHECK(w.act == DCP_PASS && w.val == dcp_sid_mask(d), "ERROR_STREAMS masked to own SIDs (0x%x)", w.val);
        CHECK(!passes(d, 0x1c0, 2, ~dcp_sid_mask(d)), "ERROR_STREAMS bits of other SIDs");
        CHECK(!passes(d, 0x1c4, 2, 1), "ERROR_STREAMS word 1");
        w = dcp_dart_write(d, 0x704, 2, 0xffffffffu);
        CHECK(w.act == DCP_PASS && w.val == 0x777, "BLOCK_ERROR masked");
        CHECK(!passes(d, 0x704, 2, 0x888), "BLOCK_ERROR outside the mask");
        CHECK(!passes(d, 0x104, 2, 0), "ERROR_MASK refused");
        for (unsigned int sid = 0; sid < 32; sid++) {
            CHECK(passes(d, 0x8000 + 0x40 * sid, 2, 0x100) == dcp_sid_allowed(d, sid), "ERR_SID %u status", sid);
            CHECK(!passes(d, 0x8000 + 0x40 * sid + 8, 2, 0), "ERR_SID %u addr_lo", sid);
            CHECK(!passes(d, 0x8000 + 0x40 * sid + 4, 2, 0), "ERR_SID %u +4", sid);
        }
        CHECK(!passes(d, 0x8000 + 0x40 * 32, 2, 0), "ERR_SID 32");
    }

    /* ---- everything the guard exists to stop: TTBR/TCR/PROTECT/streams, every SID, edge offsets ---- */
    for (int d = 0; d < 2; d++) {
        for (unsigned int sid = 0; sid < 32; sid++) {
            CHECK(!passes(d, 0x1000 + 4 * sid, 2, 0x9), "TCR[%u]", sid);
            CHECK(!passes(d, 0x1000 + 4 * sid, 2, 0), "TCR[%u] clear", sid);
            CHECK(!passes(d, 0x1400 + 4 * sid, 2, 0), "TTBR[%u] clear", sid);
            CHECK(!passes(d, 0x1400 + 4 * sid, 2, 0x12345679), "TTBR[%u]", sid);
        }
        CHECK(!passes(d, 0x200, 2, 0), "PROTECT");
        CHECK(!passes(d, 0x200, 2, 1), "PROTECT set");
        CHECK(!passes(d, 0x204, 2, 1), "UNPROTECT");
        CHECK(!passes(d, 0x208, 2, 1), "PROTECT_LOCK");
        CHECK(!passes(d, 0x210, 2, 2), "DIAG_LOCK");
        CHECK(!passes(d, 0xc00, 2, 0xffffffffu), "ENABLE_STREAMS");
        CHECK(!passes(d, 0xc20, 2, 0xffffffffu), "DISABLE_STREAMS");
        CHECK(!passes(d, 0x604, 2, 0), "TLB_BUSY2");
        CHECK(!passes(d, 0x4000, 2, 0), "EXCEPTION(0)");
        CHECK(!passes(d, 0x0, 2, 0), "offset 0");
        CHECK(!passes(d, 0x1ffffc, 2, 0), "far offset");
    }
    /* sweep: of every 4-byte-aligned offset of a 0x20000 window and every value class, exactly the documented
     * registers can pass (anything else would be a hole) */
    for (int d = 0; d < 2; d++) {
        unsigned long open = 0;
        for (unsigned long off = 0; off < 0x20000; off += 4) {
            int any = 0;
            static const unsigned int probes[] = {0, 1, 0x100, 0x110, 0x117, 0x777, 0xffffffffu, 0x80000000u};
            for (unsigned int k = 0; k < sizeof(probes) / sizeof(probes[0]); k++)
                any |= passes(d, off, 2, probes[k]);
            if (any)
                open++;
        }
        /* TLB_CMD, 4 range words, ERROR, ERROR_STREAMS, BLOCK_ERROR, per-SID status (1 or 2) */
        unsigned long want = 1 + 4 + 1 + 1 + 1 + (d == DCP_DART_DCP ? 1 : 2);
        CHECK(open == want, "dart %d: %lu registers writable, expected %lu", d, open, want);
    }

    /* ---- root tables ---- */
    unsigned char zero[DCP_ROOT_BITMAP_BYTES];
    memset(zero, 0, sizeof(zero));
    /* handoff state: entries 0..15 used by iBoot, 16..31 zero except 20, everything above zero except 100 */
    for (unsigned int e = 0; e < DCP_ROOT_ENTRIES; e++)
        if (e >= 16 && e != 20 && e != 100 && !(e < 16))
            zero[e >> 3] |= (unsigned char)(1u << (e & 7));
    const unsigned long long tbl = ((0x8abcd0000ull >> 14) << 10) | 1;
    for (unsigned int e = 0; e < DCP_ROOT_ENTRIES; e++) {
        int want = e >= 16 && e < 32 && e != 20;
        CHECK(dcp_root_write_ok(zero, 8ul * e, 3, tbl) == want, "root entry %u table value", e);
        CHECK(dcp_root_write_ok(zero, 8ul * e, 3, 0) == want, "root entry %u back to zero", e);
    }
    CHECK(!dcp_root_write_ok(zero, 8ul * 31 + 8, 3, tbl), "entry 32");
    CHECK(!dcp_root_write_ok(zero, 8ul * 15, 3, tbl), "entry 15");
    CHECK(!dcp_root_write_ok(zero, 0x4000, 3, tbl), "past the page");
    CHECK(!dcp_root_write_ok(zero, 0x3ff8, 3, tbl), "last entry (2047)");
    CHECK(dcp_root_write_ok(zero, 8ul * 16, 3, tbl), "first window entry");
    CHECK(dcp_root_write_ok(zero, 8ul * 31, 3, tbl), "last window entry");
    CHECK(!dcp_root_write_ok(zero, 8ul * 20, 3, 0), "firmware-held window entry cleared");
    CHECK(!dcp_root_write_ok(zero, 8ul * 20, 3, tbl), "firmware-held window entry replaced");
    /* widths and alignment */
    for (int width = 0; width < 3; width++)
        CHECK(!dcp_root_write_ok(zero, 8ul * 17, width, tbl), "root width %d", width);
    CHECK(!dcp_root_write_ok(zero, 8ul * 17 + 4, 3, tbl), "root unaligned +4");
    CHECK(!dcp_root_write_ok(zero, 8ul * 17 + 1, 3, tbl), "root unaligned +1");
    CHECK(!dcp_root_write_ok(zero, 8ul * 17 + 4, 2, tbl >> 32), "root upper half as 32-bit");
    /* value shape: dart4 tbl_ok */
    CHECK(!dcp_root_write_ok(zero, 8ul * 17, 3, 2), "valid bit clear");
    CHECK(!dcp_root_write_ok(zero, 8ul * 17, 3, tbl | 2), "bit 1 set");
    CHECK(!dcp_root_write_ok(zero, 8ul * 17, 3, tbl | (1ull << 40)), "bit 40 set (leaf SP_END style)");
    CHECK(!dcp_root_write_ok(zero, 8ul * 17, 3, tbl | (1ull << 63)), "bit 63 set");
    CHECK(!dcp_root_write_ok(zero, 8ul * 17, 3, 0x3ff), "low bits");
    CHECK(dcp_root_write_ok(zero, 8ul * 17, 3, 1 | DCP_PTE_PA_MASK), "max PA field");
    /* a bitmap with every entry zero admits exactly the window */
    memset(zero, 0xff, sizeof(zero));
    unsigned long open = 0;
    for (unsigned int e = 0; e < DCP_ROOT_ENTRIES; e++)
        open += dcp_root_write_ok(zero, 8ul * e, 3, tbl);
    CHECK(open == DCP_ROOT_WIN_COUNT, "%lu root entries writable with an all-zero handoff", open);
    /* an all-nonzero handoff admits nothing */
    memset(zero, 0, sizeof(zero));
    open = 0;
    for (unsigned int e = 0; e < DCP_ROOT_ENTRIES; e++)
        open += dcp_root_write_ok(zero, 8ul * e, 3, tbl) + dcp_root_write_ok(zero, 8ul * e, 3, 0);
    CHECK(open == 0, "%lu root entries writable with an all-nonzero handoff", open);

    /* ---- owner register page ---- */
    struct dcpo_state s;
    memset(&s, 0, sizeof(s));
    s.fb_exists = 1;
    s.armed = 1;
    s.passes = 0x123456789ull;
    s.dart_drops = 3;
    s.root_drops = 7;
    CHECK(dcpo_read(0x00, 2, &s) == 0, "owner default 0");
    CHECK(dcpo_read(0x04, 2, &s) == ((1u << 2) | (1u << 3)), "state 0x%llx", dcpo_read(0x04, 2, &s));
    CHECK(dcpo_read(0x08, 2, &s) == 0x23456789u && dcpo_read(0x0c, 2, &s) == 1, "pass count words");
    CHECK(dcpo_read(0x08, 3, &s) == 0x123456789ull, "pass count 64-bit read");
    CHECK(dcpo_read(0x10, 2, &s) == 0x4f57354du, "magic");
    CHECK(dcpo_read(0x10, 3, &s) == (0x4f57354dull | (1ull << 32)), "magic+version 64-bit read");
    CHECK(dcpo_read(0x18, 2, &s) == 3 && dcpo_read(0x1c, 2, &s) == 7, "drop counters");
    CHECK(dcpo_read(0x20, 2, &s) == 0 && dcpo_read(0x3ff8, 3, &s) == 0, "unused registers read 0");
    CHECK(dcpo_read(0x00, 1, &s) == 0 && dcpo_read(0x01, 2, &s) == 0 && dcpo_read(0x04, 3, &s) == 0, "odd reads");
    s.owner = 1;
    s.fb_paused = 1;
    s.changes = 300;
    CHECK(dcpo_read(0x00, 2, &s) == 1, "owner 1");
    CHECK(dcpo_read(0x04, 2, &s) == (1u | 2u | 4u | 8u | (255u << 8)), "state paused, changes saturate");
    unsigned int o = 9;
    CHECK(dcpo_write(0x00, 2, 1, &o) == DCPO_SET_OWNER && o == 1, "write 1");
    CHECK(dcpo_write(0x00, 2, 0, &o) == DCPO_SET_OWNER && o == 0, "write 0");
    o = 9;
    CHECK(dcpo_write(0x00, 2, 2, &o) == DCPO_IGNORE && o == 9, "write 2 ignored");
    CHECK(dcpo_write(0x00, 2, 0x100000001ull, &o) == DCPO_IGNORE, "write 0x100000001 ignored");
    CHECK(dcpo_write(0x00, 2, 0xffffffffu, &o) == DCPO_IGNORE, "write all ones ignored");
    CHECK(dcpo_write(0x00, 3, 1, &o) == DCPO_IGNORE, "64-bit write ignored");
    CHECK(dcpo_write(0x00, 0, 1, &o) == DCPO_IGNORE && dcpo_write(0x00, 1, 1, &o) == DCPO_IGNORE, "narrow writes");
    for (unsigned long off = 4; off < 0x4000; off += 4)
        CHECK(dcpo_write(off, 2, 1, &o) == DCPO_IGNORE, "write at +0x%lx", off);
    CHECK(o == 9, "owner untouched by ignored writes");

    if (fails) {
        printf("win-dcp FAIL (%d of %lu checks)\n", fails, checks);
        return 1;
    }
    printf("win-dcp PASS (%lu checks)\n", checks);
    return 0;
}
