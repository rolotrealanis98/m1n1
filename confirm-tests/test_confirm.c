/* SPDX-License-Identifier: MIT */
/* Confirmation-review tests for resident r2.1 (host only). Reuses the ASan
 * poisoned-arena harness of ../review-tests/test_review.c unchanged (its main is
 * renamed) and adds:
 *   - B3 PRP list walk attacks (last-qword list, chain to unadmitted page, chain
 *     "loops", exact page-end fits, prp1-vs-list overlap, 2-page offset case),
 *   - B3-focused fuzz: random qword-offset lists and random chain entries,
 *   - B2 abuse: CQ acks after every CFS source must not fetch or touch memory,
 *   - S1DG: bounded indices under flooding, and no host pointer ever recorded. */
#define main review_main
#include "../review-tests/test_review.c"
#undef main

static unsigned confirm_checks;
#define C(x) do { confirm_checks++; CHECK(x); } while (0)

static void fill_list(u64 at, unsigned n, unsigned first_page)
{ u64 *l = (u64 *)ptr(at); for (unsigned i = 0; i < n; i++) l[i] = DATA + (u64)(first_page + i) * PAGE; }
static struct vnvme_command rd_cmd(u64 prp1, u64 prp2, unsigned blocks)
{ struct vnvme_command r = cmd(2, 1, prp1, 0, 0); r.prp2 = prp2; r.cdw12 = blocks - 1; return r; }
static bool data_ok(unsigned pages)
{
    for (unsigned b = 0; b < pages; b++) if (ptr(DATA + b * PAGE)[0] != (u8)(b * 17)) return false;
    return true;
}

/* ---- B3 directed ---- */
static void c_prp_walk(void)
{
    fresh(); enable(8); io_pair(8, 8);
    /* a) list at the last qword of a page: that slot is only the chain */
    u64 *last = (u64 *)ptr(LIST + PAGE - 8); *last = LIST + 4 * PAGE; fill_list(LIST + 4 * PAGE, 31, 1);
    memset(ptr(DATA), 0, 32 * PAGE);
    C(submit(&ioq, rd_cmd(DATA, LIST + PAGE - 8, 32), true) == 0 && data_ok(32));
    /* b) chain entry -> unadmitted page: rejected, nothing copied, canary (oracle) */
    deny(LIST + 4 * PAGE, CANARY); memset(ptr(DATA), 0x44, PAGE);
    C(submit(&ioq, rd_cmd(DATA, LIST + PAGE - 8, 32), true) == VNVME_SC_DATA_TRANSFER_ERROR && ptr(DATA)[0] == 0x44);
    allow(LIST + 4 * PAGE); fill_list(LIST + 4 * PAGE, 31, 1);
    /* c) "loop": chain points back at the page holding fragment 1 (bytes 0..), legal read-only reuse */
    *last = LIST; fill_list(LIST, 31, 1); *last = LIST; memset(ptr(DATA), 0, 32 * PAGE);
    int st = submit(&ioq, rd_cmd(DATA, LIST + PAGE - 8, 32), true);
    C(st == 0 && data_ok(32));
    /* d) chain into a page whose remaining entries need a 3rd fragment: impossible
     *    (fragment 2 is page aligned, 512 slots >= 32) -> walk always ends at <=2 */
    /* e) exact fit: 31 entries ending exactly at the page end -> no chain read */
    u64 at = LIST + PAGE - 31 * 8; fill_list(at, 31, 1); deny(LIST + PAGE, CANARY); /* next page refused */
    memset(ptr(DATA), 0, 32 * PAGE);
    C(submit(&ioq, rd_cmd(DATA, at, 32), true) == 0 && data_ok(32));
    allow(LIST + PAGE);
    /* f) one entry short of fitting: 30 slots + chain -> 2nd fragment holds 1 entry */
    at = LIST + PAGE - 31 * 8 + 8; fill_list(at, 29, 1); ((u64 *)ptr(at))[29] = LIST + 5 * PAGE;
    ((u64 *)ptr(LIST + 5 * PAGE))[0] = DATA + 30 * PAGE; ((u64 *)ptr(LIST + 5 * PAGE))[1] = DATA + 31 * PAGE;
    memset(ptr(DATA), 0, 32 * PAGE);
    C(submit(&ioq, rd_cmd(DATA, at, 32), true) == 0 && data_ok(32));
    /* g) prp1 (first data segment) overlapping a list fragment */
    fill_list(LIST + 256, 31, 1);
    C(submit(&ioq, rd_cmd(LIST + 256, LIST + 256, 32), true) == VNVME_SC_DATA_TRANSFER_ERROR);
    /* h) 2-block read with prp1 offset: 3 data segments -> count 2 -> list */
    struct vnvme_command r = rd_cmd(DATA + 512, LIST + 1024, 2);
    fill_list(LIST + 1024, 2, 1); memset(ptr(DATA), 0, 3 * PAGE);
    C(submit(&ioq, r, true) == 0);
    C(ptr(DATA + 512)[0] == 0 && ptr(DATA + 512)[1] == 1);          /* LBA0 byte1 = 0*17+1 */
    /* i) list pointer offsets that are qword but not 8-entry aligned, and odd */
    C(submit(&ioq, rd_cmd(DATA, LIST + 4, 32), true) == VNVME_SC_DATA_TRANSFER_ERROR);
    C(submit(&ioq, rd_cmd(DATA, LIST + 1, 32), true) == VNVME_SC_DATA_TRANSFER_ERROR);
    /* j) chain entry 0 / unaligned / near 2^64 */
    *last = 0;               C(submit(&ioq, rd_cmd(DATA, LIST + PAGE - 8, 32), true) == VNVME_SC_DATA_TRANSFER_ERROR);
    *last = LIST + 2 * PAGE + 8; C(submit(&ioq, rd_cmd(DATA, LIST + PAGE - 8, 32), true) == VNVME_SC_DATA_TRANSFER_ERROR);
    *last = UINT64_MAX & ~(u64)(PAGE - 1); C(submit(&ioq, rd_cmd(DATA, LIST + PAGE - 8, 32), true) == VNVME_SC_DATA_TRANSFER_ERROR);
    /* k) data entry inside the arena but list pointer in the last page of the arena,
     *    chain beyond the arena end */
    u64 endp = RAM + RAM_SIZE - 8; *(u64 *)ptr(endp) = RAM + RAM_SIZE;
    C(submit(&ioq, rd_cmd(DATA, endp, 32), true) == VNVME_SC_DATA_TRANSFER_ERROR);
    /* l) discriminating variants (valid payload except for the one violated rule) */
    *last = LIST + 4 * PAGE + 8; fill_list(LIST + 4 * PAGE + 8, 31, 1);     /* chain not page aligned */
    C(submit(&ioq, rd_cmd(DATA, LIST + PAGE - 8, 32), true) == VNVME_SC_DATA_TRANSFER_ERROR);
    for (unsigned i = 0; i < 31; i++) { u64 v = DATA + (u64)(i + 1) * PAGE; memcpy(ptr(LIST + 6 * PAGE + 4 + 8 * i), &v, 8); }
    C(submit(&ioq, rd_cmd(DATA, LIST + 6 * PAGE + 4, 32), true) == VNVME_SC_DATA_TRANSFER_ERROR); /* list not qword aligned */
    fill_list(LIST + 7 * PAGE + 256, 31, 1); ((u64 *)ptr(LIST + 7 * PAGE + 256))[4] = LIST + 7 * PAGE;
    C(submit(&ioq, rd_cmd(DATA, LIST + 7 * PAGE + 256, 32), true) == VNVME_SC_DATA_TRANSFER_ERROR); /* data over fragment 1 */
    *last = LIST + 4 * PAGE; fill_list(LIST + 4 * PAGE, 31, 1); ((u64 *)ptr(LIST + 4 * PAGE))[9] = LIST;
    C(submit(&ioq, rd_cmd(DATA, LIST + PAGE - 8, 32), true) == VNVME_SC_DATA_TRANSFER_ERROR); /* data over fragment 1 (chained) */
    ((u64 *)ptr(LIST + 4 * PAGE))[9] = LIST + 4 * PAGE;
    C(submit(&ioq, rd_cmd(DATA, LIST + PAGE - 8, 32), true) == VNVME_SC_DATA_TRANSFER_ERROR); /* data over fragment 2 */
    fill_list(LIST + 4 * PAGE, 31, 1);
    C(submit(&ioq, rd_cmd(DATA, LIST + PAGE - 8, 32), true) == 0);           /* control: valid again */
    C(rd(0x1c, 4) == 1);
}

/* ---- B3 fuzz: random list geometry, poisoned arena oracle ---- */
static void c_prp_fuzz(u64 seed, unsigned iters)
{
    rs = seed; fresh(); enable(8); io_pair(8, 8);
    for (unsigned i = 0; i < iters; i++) {
        for (unsigned p = 8; p < 16; p++) perms[p] = (rnd() % 6) ? 3 : 0;   /* list pages */
        for (unsigned p = 16; p < 56; p++) perms[p] = (rnd() % 40) ? 3 : 0; /* data pages */
        u64 lp = LIST + (rnd() % 8) * PAGE + (rnd() % 4 ? (rnd() % 512) * 8 : PAGE - 8 * (1 + rnd() % 4));
        if (rnd() % 16 == 0) lp += 1 + rnd() % 7;
        for (unsigned k = 0; k < 8; k++) {                 /* rewrite admitted list pages */
            if (perms[8 + k] != 3) continue;
            u64 *l = (u64 *)ptr(LIST + k * PAGE);
            for (unsigned e = 0; e < 512; e++) {
                u64 v;
                switch (rnd() % 10) {
                case 0: v = LIST + (rnd() % 8) * PAGE; break;          /* chain-like */
                case 1: v = rnd_addr(); break;
                case 2: v = LIST + (rnd() % 8) * PAGE + (rnd() % 512) * 8; break;
                default: v = DATA + (rnd() % 40) * PAGE; break;
                }
                l[e] = v;
            }
        }
        unsigned blocks = 1 + rnd() % 32;
        u64 prp1 = DATA + (rnd() % 40) * PAGE + (rnd() % 3 == 0 ? (rnd() % 512) * 8 : 0);
        struct vnvme_command r = rd_cmd(prp1, rnd() % 8 ? lp : DATA + (rnd() % 40) * PAGE, blocks);
        if (ioq.n == 0) break;
        int s = submit(&ioq, r, true);
        C(s == 0 || s == VNVME_SC_DATA_TRANSFER_ERROR || s == VNVME_SC_INVALID_FIELD || s == -1);
        if (s == -1 || rd(0x1c, 4) != 1) { memset(perms, 3, sizeof perms); wr(0x14, 4, 0); enable(8); io_pair(8, 8); }
    }
}

/* ---- B2 abuse: CQ acks after each CFS source ---- */
static void c_cfs_ack_sources(void)
{
    struct vnvme_command r = cmd(2, 1, DATA, 0, 0);
    /* (1) queue-level fatal (SQ page refused) */
    fresh(); enable(8); io_pair(8, 8);
    place(&ioq, r); place(&ioq, r); C(wr(0x1008, 4, ioq.tail));           /* #1 done, #2 waits */
    u64 reads = m.backend_reads;
    place(&aq, cmd(10, 0, 0, 7, 0)); deny(ACQ, CANARY);
    C(wr(0x1000, 4, aq.tail)); C(rd(0x1c, 4) & 2);                          /* admin CQ refused -> fatal */
    unsigned g = grants;
    C(wr(0x100c, 4, (u16)((ioq.head + 1) % ioq.n)));                        /* ack IO CQE: pending -> 0 */
    C(grants == g && m.backend_reads == reads);                              /* no fetch of #2 */
    /* (2) model-level CFS: SHN=11b while enabled */
    fresh(); enable(8); io_pair(8, 8);
    place(&ioq, r); place(&ioq, r); C(wr(0x1008, 4, ioq.tail));
    reads = m.backend_reads; u64 comp = m.queue.stats.completions;
    C(wr(0x14, 4, S1_CC_VALID | 0xc000)); C(rd(0x1c, 4) & 2);
    g = grants;
    C(wr(0x100c, 4, (u16)((ioq.head + 1) % ioq.n)));
    /* r2.2 D1 fixed: former info line is now an assertion */
    C(m.backend_reads == reads && m.queue.stats.completions == comp && grants == g);
    C(wr(0x14, 4, 0)); C(!line);
    /* (3) CFS at enable (no queues): CQ doorbells are refused as queue-invalid */
    fresh(); C(wr(0x24, 4, 0x00070007)); C(wr(0x28, 8, ASQ)); C(wr(0x30, 8, ACQ));
    C(wr(0x14, 4, S1_CC_VALID | (1u << 7))); C(rd(0x1c, 4) == 2);
    g = grants; C(wr(0x1004, 4, 1)); C(grants == g);
}

/* ---- S1DG: flooding and no host pointers ---- */
static bool host_ptr(u64 v)
{
    u64 lo[] = { (u64)(uintptr_t)arena, (u64)(uintptr_t)&m, (u64)(uintptr_t)shadow, (u64)(uintptr_t)&ops };
    u64 sz[] = { RAM_SIZE, sizeof m, RAM_SIZE, 4096 };
    /* On macOS the host image can sit just above 0x100000000 = the harness's guest
     * IPA base, so a genuine guest IPA may coincide with a host address. Values
     * inside the guest IPA window (legitimately logged) are not counted. */
    if (v >= RAM && v - RAM < RAM_SIZE) return false;
    if (v >= RAM + RAM_SIZE && v - RAM - RAM_SIZE < 64 * PAGE) return false; /* fuzzed IPAs just past the end */
    for (unsigned i = 0; i < 4; i++) if (v >= lo[i] && v - lo[i] < sz[i]) return true;
    return false;
}
static void c_s1dg(void)
{
    fresh(); enable(8); io_pair(8, 8);
    for (unsigned i = 0; i < 3000; i++) {
        submit(&aq, cmd((u8)(0x80 + i % 0x7f), 0, DATA, (u32)i, 0), true);   /* opcodes >= 32 */
        submit(&ioq, cmd((u8)(i % 256), (u32)i, DATA, (u32)i, 0), true);
        wr((u32)(0x40 + (i % 0xfc0)), 4, i); rd((u32)(0x40 + (i % 0xfc0)), 1);
        wr(0x1000 + 4 * (i % 64), 4, 0x10000 + i);                          /* invalid doorbells */
        cfgw((u32)(0x40 + i % 0x80), 1, i); s1_config(&m, ECAM + 0x100000 - 1, 4, false, &(u64){0}); /* out of ECAM */
    }
    struct s1_diag *g = &m.diag;
    C(g->magic == S1DG_MAGIC && g->struct_size == sizeof *g && g->write_seq > S1DG_RING);
    C(g->admin_first_count == S1DG_ADMIN_FIRST && g->mmio_first_count == S1DG_MMIO_FIRST);
    C(g->admin_other > 0 && g->io_other > 0 && g->reserved_access > 0 && g->invalid_db[3] > 0);
    for (unsigned i = 0; i < S1DG_RING; i++) {
        const struct s1dg_entry *e = &g->ring[i];
        C(!host_ptr(e->a) && !host_ptr(e->b) && e->type >= S1DG_MMIO_R && e->type <= S1DG_SHN_CLEAR_NO_RESET);
    }
    /* r2.2 builder adaptation: the raw qword scan of the packed ring/first-access
     * arrays produced ASLR-dependent false positives (e.g. a mmio_first record
     * {kind 0,width 4,write 1,ok 1,csts 1} reads as qword 0x101010400 when the host
     * image loads near 0x101000000). Header+counters keep the raw qword scan; the
     * packed arrays are scanned per field. */
    const u64 *w = (const u64 *)g; size_t nw = offsetof(struct s1_diag, ring) / 8;
    for (size_t i = 0; i < nw; i++) C(!host_ptr(w[i]));                      /* header + counters */
    for (unsigned i = 0; i < S1DG_MMIO_FIRST; i++)
        C(!host_ptr(g->mmio_first[i].value) && !host_ptr(g->mmio_first[i].offset));
    for (unsigned i = 0; i < S1DG_ADMIN_FIRST; i++) {
        u64 q[8]; memcpy(q, g->admin_first[i].sqe, 64); for (unsigned k = 0; k < 8; k++) C(!host_ptr(q[k]));
    }
    /* diag never changed by reset paths / survives CC.EN=0 and BAR remap */
    u64 seq = g->write_seq; C(wr(0x14, 4, 0)); C(g->write_seq > seq && g->magic == S1DG_MAGIC);
    C(cfgw(4, 2, 0)); C(cfgw(4, 2, S1_MEM | S1_BME)); C(g->magic == S1DG_MAGIC);
}

int main(int argc, char **argv)
{
    unsigned iters = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 20000;
    c_prp_walk(); c_cfs_ack_sources(); c_s1dg();
    for (u64 s = 1; s <= 8; s++) c_prp_fuzz(0xd1b54a32d192ed03ull * s, iters);
    C(s0_fixture_guards_valid());
    printf("{\"status\":\"pass\",\"confirm_checks\":%u,\"all_checks\":%u,\"model_calls\":%u,\"grants\":%u,\"refusals\":%u}\n",
           confirm_checks, checks, calls, grants, refusals);
    return 0;
}
