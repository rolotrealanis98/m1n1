/* SPDX-License-Identifier: MIT */
/*
 * nightly-4: PRP planner overlap shapes on the real model (hv_nvme_queue.c), host only.
 *
 * Every command goes through the real controller (SQ doorbell, CQE). Its effect is compared byte for byte with a
 * simple reference model of an NVMe controller that performs the transfer in command order:
 *   - segments: PRP1 (to the end of its page), then one entry per page (the last one short), entries from PRP2
 *     (a data pointer for two pages, else a PRP list of at most 2 fragments, chained by the last qword of a page);
 *   - every list fragment must be readable guest RAM for exactly the bytes read; every data segment must be guest
 *     RAM with write permission (READ) or read permission (WRITE) on its page, may not touch an active queue or a
 *     consumed list fragment;
 *   - WRITE: source segments must be disjoint, except that a whole page-aligned page may exactly repeat an earlier
 *     whole page-aligned segment (nightly-4b). READ: destination segments may overlap in any way;
 *   - accepted READ: segment k receives the next len(k) bytes of the media stream, in order (later bytes overwrite
 *     earlier ones). Accepted WRITE: the media receives the source segments' bytes in order;
 *   - refused: status 0x04, no backend call, guest memory and media unchanged; the logged rule matches.
 * The whole guest RAM (except the live queue bytes) and the touched media range are compared after every command.
 * The media is a flat array: GUARD blocks, the NS_BLOCKS namespace ("window"), GUARD blocks; the backend refuses any
 * LBA outside the window and both guard ranges are compared with their initial bytes after every command.
 *
 * usage: test_prp_overlap [iterations] [seed]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hv_nvme_queue.h"

#define PAGE 4096u
#define NPAGES 512u
#define RAM_SIZE ((size_t)NPAGES * PAGE)
#define RAM_BASE UINT64_C(0x80000000)
#define NS_BLOCKS 2048u
#define GUARD 64u
#define XFER_CAP (256u * PAGE)
#define PR 1u
#define PW 2u
#define PGA(n) (RAM_BASE + (u64)(n) * PAGE)
#define ASQ PGA(0)
#define ACQ PGA(1)
#define IOSQ PGA(2)
#define IOCQ PGA(3)
#define AQS 8u
#define IOQS 16u
#define LPG(n) PGA(4 + (n))   /* list area: pages 4..15 */
#define DPG(n) PGA(16 + (n))  /* data area: pages 16..511 */
#define NDATA (NPAGES - 16u)

static u8 *ram, *exp_ram, *disk_all, *disk, *ref_disk, *guard_lo, *guard_hi;
static u8 perm[NPAGES];
static u8 xfer[XFER_CAP];
static unsigned long read_calls, write_calls, checks;
static int fails;
static const char *ctx = "";
#define CHECK(c)                                                                                                   \
    do {                                                                                                           \
        checks++;                                                                                                  \
        if (!(c)) {                                                                                                \
            if (fails++ < 25) printf("FAIL [%s] line %d: %s\n", ctx, __LINE__, #c);                                \
        }                                                                                                          \
    } while (0)

/* ---- guest memory and backend ---- */
static bool span_ok(u64 ipa, size_t n, bool w)
{
    if (!n || ipa < RAM_BASE || ipa - RAM_BASE >= RAM_SIZE || n > RAM_SIZE - (ipa - RAM_BASE)) return false;
    u64 o = ipa - RAM_BASE;
    for (u64 p = o / PAGE; p <= (o + n - 1) / PAGE; p++)
        if (!(perm[p] & (w ? PW : PR))) return false;
    return true;
}
static void *span(void *o, u64 ipa, size_t n, bool w) { (void)o; return span_ok(ipa, n, w) ? ram + (ipa - RAM_BASE) : NULL; }
static bool rb(void *o, u64 lba, u32 n, void *buf)
{
    (void)o; read_calls++;
    if (lba >= NS_BLOCKS || n > NS_BLOCKS - lba || n > 32) return false; /* window */
    memcpy(buf, disk + lba * PAGE, (size_t)n * PAGE);
    return true;
}
static bool wb(void *o, u64 lba, u32 n, const void *buf)
{
    (void)o; write_calls++;
    if (lba >= NS_BLOCKS || n > NS_BLOCKS - lba || n > 32) return false; /* window */
    memcpy(disk + lba * PAGE, buf, (size_t)n * PAGE);
    return true;
}
static bool fl(void *o) { (void)o; return true; }
static void *xb(void *o, size_t *cap) { (void)o; *cap = sizeof(xfer); return xfer; }
static void caps(void *o, struct vnvme_caps *c) { (void)o; c->writable = true; c->vwc = true; c->max_blocks = 256; c->native = true; }
static const struct vnvme_backend_ops ops = {.guest_span = span, .read_blocks = rb, .write_blocks = wb, .flush = fl,
                                             .xfer_buffer = xb, .caps = caps};
static struct vnvme_ctrl ctrl;

/* ---- queues ---- */
struct q { u64 sq, cq; u16 size, tail, head, qid; u8 phase; };
static struct q aq, ioq;
static u16 next_cid;
static u16 submit(struct q *q, struct vnvme_command c)
{
    c.cid = next_cid++;
    memcpy(ram + (q->sq - RAM_BASE) + (size_t)q->tail * 64, &c, 64);
    q->tail = (u16)((q->tail + 1) % q->size);
    vnvme_sq_doorbell(&ctrl, q->qid, q->tail);
    struct vnvme_completion e;
    memcpy(&e, ram + (q->cq - RAM_BASE) + (size_t)q->head * 16, 16);
    CHECK((e.status & 1) == q->phase && e.cid == c.cid);
    if ((e.status & 1) != q->phase) return 0xffff;
    q->head = (u16)((q->head + 1) % q->size);
    if (!q->head) q->phase ^= 1;
    vnvme_cq_doorbell(&ctrl, q->qid, q->head);
    return e.status >> 1;
}
static void setup(void)
{
    vnvme_init(&ctrl, NS_BLOCKS, &ops, NULL);
    memset(ram, 0, 4 * PAGE);
    for (unsigned i = 0; i < NPAGES; i++) perm[i] = PR | PW;
    aq = (struct q){ASQ, ACQ, AQS, 0, 0, 0, 1};
    CHECK(vnvme_set_admin_queue(&ctrl, ASQ, ACQ, AQS, AQS));
    ctrl.iosqes = 6; ctrl.iocqes = 4;
    struct vnvme_command c = {0};
    c.opcode = VNVME_ADMIN_CREATE_CQ; c.prp1 = IOCQ; c.cdw10 = ((IOQS - 1) << 16) | 1; c.cdw11 = 0x3;
    CHECK(submit(&aq, c) == 0);
    memset(&c, 0, sizeof(c));
    c.opcode = VNVME_ADMIN_CREATE_SQ; c.prp1 = IOSQ; c.cdw10 = ((IOQS - 1) << 16) | 1; c.cdw11 = (1u << 16) | 1;
    CHECK(submit(&aq, c) == 0);
    ioq = (struct q){IOSQ, IOCQ, IOQS, 0, 0, 1, 1};
}

/* ---- reference model ---- */
struct seg { u64 addr; size_t len; };
struct ref { int rule; size_t nseg; struct seg seg[VNVME_MAX_XFER_BLOCKS + 1]; bool dup, part; };
static bool ovl(u64 a, size_t an, u64 b, size_t bn)
{
    return (unsigned __int128)a < (unsigned __int128)b + bn && (unsigned __int128)b < (unsigned __int128)a + an;
}
static bool over_queue(u64 a, size_t n)
{
    return ovl(a, n, ASQ, AQS * 64) || ovl(a, n, ACQ, AQS * 16) || ovl(a, n, IOSQ, IOQS * 64) || ovl(a, n, IOCQ, IOQS * 16);
}
static u64 rd64(u64 a) { u64 v; memcpy(&v, ram + (a - RAM_BASE), 8); return v; }
static void ref_plan(u64 prp1, u64 prp2, size_t total, bool to_guest, struct ref *r)
{
    memset(r, 0, sizeof(*r));
    if (!total || total > XFER_CAP || !prp1) { r->rule = VNVME_PRP_SIZE; return; }
    size_t first = PAGE - (size_t)(prp1 % PAGE);
    if (first > total) first = total;
    size_t rest = total - first, count = (rest + PAGE - 1) / PAGE;
    u64 ent[VNVME_MAX_XFER_BLOCKS];
    u64 fa[2] = {0};
    size_t fn[2] = {0}, nf = 0;
    if (rest) {
        if (!prp2) { r->rule = VNVME_PRP_PRP2; return; }
        if (count == 1) {
            if (prp2 % PAGE) { r->rule = VNVME_PRP_PRP2_ALIGN; return; }
            ent[0] = prp2;
        } else {
            u64 list = prp2;
            size_t got = 0;
            while (got < count) {
                if (!list || list % 8 || nf == 2 || (nf && list % PAGE)) { r->rule = VNVME_PRP_LIST_PTR; return; }
                size_t room = (PAGE - (size_t)(list % PAGE)) / 8, need = count - got;
                size_t n = need <= room ? need : room;
                if (!span_ok(list, n * 8, false)) { r->rule = VNVME_PRP_LIST_ADMIT; return; }
                fa[nf] = list; fn[nf++] = n * 8;
                if (need <= room) {
                    for (size_t i = 0; i < n; i++) ent[got++] = rd64(list + 8 * i);
                } else {
                    for (size_t i = 0; i + 1 < n; i++) ent[got++] = rd64(list + 8 * i);
                    list = rd64(list + 8 * (n - 1));
                }
            }
        }
    }
    size_t rem = total;
    for (size_t k = 0; rem; k++) {
        u64 a = k ? ent[k - 1] : prp1;
        size_t n = k ? (rem < PAGE ? rem : PAGE) : first;
        if (k && (!a || a % PAGE)) { r->rule = VNVME_PRP_ENTRY_ALIGN; return; }
        if (!span_ok(a, n, to_guest)) { r->rule = VNVME_PRP_DATA_ADMIT; return; }
        if (over_queue(a, n)) { r->rule = VNVME_PRP_DATA_QUEUE; return; }
        for (size_t f = 0; f < nf; f++)
            if (ovl(a, n, fa[f], fn[f])) { r->rule = VNVME_PRP_DATA_LIST; return; }
        for (size_t i = 0; i < r->nseg; i++)
            if (ovl(a, n, r->seg[i].addr, r->seg[i].len)) {
                bool exact = n == PAGE && r->seg[i].len == PAGE && a == r->seg[i].addr;
                if (!to_guest && !exact) { r->rule = VNVME_PRP_DATA_DATA; return; }
                r->dup = true;
                if (!(n == PAGE && r->seg[i].len == PAGE && a == r->seg[i].addr)) r->part = true;
            }
        r->seg[r->nseg++] = (struct seg){a, n};
        rem -= n;
    }
}

/* ---- one command: real controller vs reference ---- */
static unsigned long st_rule[2][VNVME_PRP_RULES], st_ok[2], st_dup, st_part, st_big;
static struct ref R;
static unsigned long cmds, st_dupw;
static int run_one(u8 op, u64 lba, u32 blocks, u64 prp1, u64 prp2, int expect)
{
    bool to_guest = op == VNVME_IO_READ;
    size_t total = (size_t)blocks * PAGE;
    ref_plan(prp1, prp2, total, to_guest, &R);
    if (expect >= 0) CHECK(R.rule == expect); /* the shape is what the test says it is */
    memcpy(exp_ram, ram, RAM_SIZE);
    if (!R.rule) {
        size_t off = 0;
        for (size_t i = 0; i < R.nseg; i++, off += R.seg[i - 1].len) {
            if (to_guest) memcpy(exp_ram + (R.seg[i].addr - RAM_BASE), ref_disk + lba * PAGE + off, R.seg[i].len);
            else memcpy(ref_disk + lba * PAGE + off, ram + (R.seg[i].addr - RAM_BASE), R.seg[i].len);
        }
        CHECK(off == total);
    }
    unsigned long rc = read_calls, wc = write_calls;
    u64 dup = ctrl.prp_dup_dest_reads, part = ctrl.prp_part_dest_reads, rt = ctrl.prp_refusal_total;
    u64 dupw = ctrl.prp_dup_src_writes;
    struct vnvme_command c = {0};
    c.opcode = op; c.nsid = 1; c.prp1 = prp1; c.prp2 = prp2;
    c.cdw10 = (u32)lba; c.cdw11 = (u32)(lba >> 32); c.cdw12 = blocks - 1;
    u16 st = submit(&ioq, c);
    /* the live queue bytes are the harness's, not data */
    memcpy(exp_ram + (ASQ - RAM_BASE), ram + (ASQ - RAM_BASE), AQS * 64);
    memcpy(exp_ram + (ACQ - RAM_BASE), ram + (ACQ - RAM_BASE), AQS * 16);
    memcpy(exp_ram + (IOSQ - RAM_BASE), ram + (IOSQ - RAM_BASE), IOQS * 64);
    memcpy(exp_ram + (IOCQ - RAM_BASE), ram + (IOCQ - RAM_BASE), IOQS * 16);
    CHECK(st == (R.rule ? VNVME_SC_DATA_TRANSFER_ERROR : VNVME_SC_SUCCESS));
    if (memcmp(ram, exp_ram, RAM_SIZE)) {
        size_t d = 0;
        while (ram[d] == exp_ram[d]) d++;
        CHECK(!"guest memory equals the in-order model");
        if (fails < 25) printf("  first difference at page %zu +0x%zx (op %u rule %d nseg %zu)\n", d / PAGE, d % PAGE, op, R.rule, R.nseg);
    }
    CHECK(!memcmp(disk + lba * PAGE, ref_disk + lba * PAGE, total));
    CHECK(!memcmp(disk_all, guard_lo, (size_t)GUARD * PAGE));                       /* nothing outside the window */
    CHECK(!memcmp(disk + (size_t)NS_BLOCKS * PAGE, guard_hi, (size_t)GUARD * PAGE));
    if (!(++cmds % 4096)) CHECK(!memcmp(disk, ref_disk, (size_t)NS_BLOCKS * PAGE)); /* whole window, periodically */
    CHECK(!ctrl.fatal);
    if (R.rule) {
        CHECK(read_calls == rc && write_calls == wc); /* nothing reached the backend */
        CHECK(ctrl.prp_refusal_total == rt + 1);
        const struct vnvme_prp_refusal *x = &ctrl.prp_log[(ctrl.prp_refusal_total - 1) % VNVME_PRP_LOG];
        CHECK(x->rule == R.rule && x->opcode == op && x->to_guest == to_guest);
        CHECK(ctrl.prp_dup_dest_reads == dup && ctrl.prp_part_dest_reads == part && ctrl.prp_dup_src_writes == dupw);
        st_rule[to_guest][R.rule]++;
    } else {
        CHECK(ctrl.prp_refusal_total == rt);
        if (to_guest) CHECK(read_calls == rc + (blocks + 31) / 32 && write_calls == wc);
        else CHECK(write_calls == wc + (blocks + 31) / 32 && read_calls == rc);
        CHECK(ctrl.prp_dup_dest_reads == dup + (to_guest && R.dup));
        CHECK(ctrl.prp_part_dest_reads == part + (to_guest && R.part));
        CHECK(!to_guest || !R.dup || ctrl.prp_dup_dest_last == prp1);
        CHECK(!R.part || ctrl.prp_part_dest_last == prp1);
        CHECK(!to_guest || !R.part || R.dup);
        CHECK(to_guest || !R.part);                                  /* a WRITE overlap is never partial */
        CHECK(ctrl.prp_dup_src_writes == dupw + (!to_guest && R.dup));
        CHECK(to_guest || !R.dup || ctrl.prp_dup_src_last == prp1);
        st_dupw += !to_guest && R.dup;
        st_ok[to_guest]++;
        st_dup += to_guest && R.dup && !R.part;
        st_part += R.part;
        st_big += blocks > 32;
    }
    return R.rule;
}
static void put_list(u64 at, const u64 *e, size_t n) { memcpy(ram + (at - RAM_BASE), e, n * 8); }
static void fill(u64 a, size_t n, u8 v) { memset(ram + (a - RAM_BASE), v, n); }

/* ---- explicit shapes ---- */
static void shapes(void)
{
    const u64 DUMMY = DPG(100), ZP = DPG(101);
    /* A1: unaligned first segment (PRP1 = D0+512) and the same page again as a whole page; short last on D1 */
    ctx = "A1 partial PRP1 then whole page";
    fill(DPG(0), 3 * PAGE, 0xee);
    {u64 e[2] = {DPG(0), DPG(1)}; put_list(LPG(0), e, 2);}
    CHECK(run_one(VNVME_IO_READ, 10, 2, DPG(0) + 512, LPG(0), 0) == 0);
    CHECK(!memcmp(ram + (DPG(0) - RAM_BASE), ref_disk + 10 * PAGE + 3584, PAGE));        /* literal: later copy wins */
    CHECK(!memcmp(ram + (DPG(1) - RAM_BASE), ref_disk + 10 * PAGE + 7680, 512));
    for (unsigned i = 512; i < PAGE; i++) CHECK(ram[DPG(1) - RAM_BASE + i] == 0xee);    /* past the short last: untouched */
    CHECK(R.dup && R.part);
    /* A2: short last segment (head of a page) over an earlier whole page */
    ctx = "A2 short last over whole page";
    {u64 e[2] = {DPG(5), DPG(5)}; put_list(LPG(0), e, 2);}
    CHECK(run_one(VNVME_IO_READ, 20, 2, DPG(2) + 1024, LPG(0), 0) == 0);
    CHECK(!memcmp(ram + (DPG(5) - RAM_BASE), ref_disk + 20 * PAGE + 7168, 1024));
    CHECK(!memcmp(ram + (DPG(5) - RAM_BASE) + 1024, ref_disk + 20 * PAGE + 3072 + 1024, PAGE - 1024));
    CHECK(R.part);
    /* A3: Windows' dummy page as the first, partial segment and repeated (whole and short) later */
    ctx = "A3 dummy page first partial + repeats";
    {u64 e[9] = {DPG(6), DUMMY, DUMMY, DPG(7), DUMMY, DUMMY, DUMMY, DPG(8), DUMMY}; put_list(LPG(1), e, 9);}
    CHECK(run_one(VNVME_IO_READ, 30, 9, DUMMY + 0x200, LPG(1), 0) == 0);
    CHECK(!memcmp(ram + (DUMMY - RAM_BASE), ref_disk + 30 * PAGE + 36864 - 512, 512)); /* the short last wins its bytes */
    CHECK(!memcmp(ram + (DUMMY - RAM_BASE) + 512, ref_disk + 30 * PAGE + 3584 + 6 * PAGE + 512, PAGE - 512));
    CHECK(R.part);
    /* A4: two partial segments of one page (PRP1 tail, short last head): disjoint, accepted (r4.14 too) */
    ctx = "A4 two partials of one page";
    {u64 e[2] = {DPG(10), DPG(9)}; put_list(LPG(2), e, 2);}
    CHECK(run_one(VNVME_IO_READ, 40, 2, DPG(9) + 0x800, LPG(2), 0) == 0);
    CHECK(!R.dup);
    CHECK(run_one(VNVME_IO_READ, 41, 1, DPG(9) + 0x800, DPG(9), 0) == 0);               /* PRP2 = data pointer */
    CHECK(!R.dup);
    /* A5: exact whole-page repeats (the r4.14 shape) */
    ctx = "A5 exact whole-page repeats";
    {u64 e[3] = {DPG(11), DPG(12), DPG(11)}; put_list(LPG(2), e, 3);}
    CHECK(run_one(VNVME_IO_READ, 50, 4, DPG(11), LPG(2), 0) == 0);
    CHECK(R.dup && !R.part);
    /* A6: 64 blocks (two 32-block backend reads) with the dummy page across the split and a partial PRP1 */
    ctx = "A6 dummy page across the 32-block split";
    {u64 e[64]; for (unsigned i = 0; i < 64; i++) e[i] = (i >= 29 && i <= 34) || i == 63 ? DUMMY : DPG(20 + i);
     put_list(LPG(3), e, 64);}
    CHECK(run_one(VNVME_IO_READ, 100, 64, DUMMY + 0x100, LPG(3), 0) == 0);
    CHECK(!memcmp(ram + (DUMMY - RAM_BASE), ref_disk + 100 * PAGE + 64 * PAGE - 0x100, 0x100));
    CHECK(!memcmp(ram + (DUMMY - RAM_BASE) + 0x100, ref_disk + 100 * PAGE + (PAGE - 0x100) + 34 * PAGE + 0x100, PAGE - 0x100));
    /* A7: 256 blocks, every entry the dummy page (largest plan) */
    ctx = "A7 256 blocks on one page";
    {u64 e[256]; for (unsigned i = 0; i < 256; i++) e[i] = DUMMY; put_list(LPG(4), e, 256);}
    CHECK(run_one(VNVME_IO_READ, 1000, 256, DUMMY + 8, LPG(4), 0) == 0);
    CHECK(!memcmp(ram + (DUMMY - RAM_BASE), ref_disk + 1000 * PAGE + 256 * PAGE - 8, 8));
    CHECK(!memcmp(ram + (DUMMY - RAM_BASE) + 8, ref_disk + 1000 * PAGE + 255 * PAGE, PAGE - 8));

    /* ---- still refused ---- */
    /* ---- WRITE exact whole-page source repeats (nightly-4b: accepted) ---- */
    /* W1/W2: Windows' WRITE shapes from n2-a3: PRP1 page aligned, the same page again; nlb 2 (PRP2 = PRP1) and
     * nlb 3..13 (a list naming the page every time). Media verified literally: every block = the page. */
    for (u32 nlb = 2; nlb <= 13; nlb++) {
        ctx = nlb == 2 ? "W1 WRITE exact repeat (2 blocks, PRP2 = PRP1)" : "W2 WRITE exact repeat (list, every entry the page)";
        for (unsigned i = 0; i < PAGE; i++) ram[ZP - RAM_BASE + i] = (u8)(i * 7 + nlb);   /* distinct per nlb */
        u64 lba = 200 + 16 * nlb;
        if (nlb > 2) {u64 e[12]; for (unsigned i = 0; i < 12; i++) e[i] = ZP; put_list(LPG(5), e, nlb - 1);}
        u64 w = ctrl.prp_dup_src_writes;
        CHECK(run_one(VNVME_IO_WRITE, lba, nlb, ZP, nlb > 2 ? LPG(5) : ZP, 0) == 0);
        for (u32 b = 0; b < nlb; b++) CHECK(!memcmp(disk + (lba + b) * PAGE, ram + (ZP - RAM_BASE), PAGE));
        CHECK(R.dup && ctrl.prp_dup_src_writes == w + 1 && ctrl.prp_dup_src_last == ZP);
        /* read back through the controller: the same bytes */
        {u64 e[12]; for (unsigned i = 0; i < 12; i++) e[i] = DPG(201 + i); put_list(LPG(9), e, nlb - 1);}
        CHECK(run_one(VNVME_IO_READ, lba, nlb, DPG(200), nlb > 2 ? LPG(9) : DPG(201), 0) == 0);
        for (u32 b = 0; b < nlb; b++) CHECK(!memcmp(ram + (DPG(200 + b) - RAM_BASE), ram + (ZP - RAM_BASE), PAGE));
    }
    /* W2b: a mix: page A, B, A, C, A (A repeated between other pages) */
    ctx = "W2b WRITE repeat between other pages";
    {u64 e[4] = {DPG(30), ZP, DPG(31), ZP}; put_list(LPG(5), e, 4);}
    CHECK(run_one(VNVME_IO_WRITE, 500, 5, ZP, LPG(5), 0) == 0);
    CHECK(!memcmp(disk + 502 * PAGE, ram + (ZP - RAM_BASE), PAGE) && !memcmp(disk + 501 * PAGE, ram + (DPG(30) - RAM_BASE), PAGE));
    /* W6: an exact repeat of a page that is the PRP list's own page: data-list */
    ctx = "W6 WRITE repeat of the list page";
    {u64 e[2] = {LPG(8), LPG(8)}; put_list(LPG(8) + 0x800, e, 2);}
    CHECK(run_one(VNVME_IO_WRITE, 510, 3, LPG(8), LPG(8) + 0x800, VNVME_PRP_DATA_LIST) == VNVME_PRP_DATA_LIST);
    /* W7: an exact repeat of a live queue page: data-queue */
    ctx = "W7 WRITE repeat of a queue page";
    CHECK(run_one(VNVME_IO_WRITE, 520, 2, IOSQ, IOSQ, VNVME_PRP_DATA_QUEUE) == VNVME_PRP_DATA_QUEUE);
    /* W8: an unaligned "repeat" (entry = page + 8): entry-align */
    ctx = "W8 WRITE unaligned repeat";
    CHECK(run_one(VNVME_IO_WRITE, 530, 2, ZP, ZP + 8, VNVME_PRP_PRP2_ALIGN) == VNVME_PRP_PRP2_ALIGN);
    {u64 e[2] = {ZP, ZP + 8}; put_list(LPG(5), e, 2);}
    CHECK(run_one(VNVME_IO_WRITE, 530, 3, ZP, LPG(5), VNVME_PRP_ENTRY_ALIGN) == VNVME_PRP_ENTRY_ALIGN);
    /* W9: a repeated page that is not readable: data-admit (first occurrence already) */
    ctx = "W9 WRITE repeat of an unreadable page";
    perm[16 + 40] = PW;
    CHECK(run_one(VNVME_IO_WRITE, 540, 2, DPG(40), DPG(40), VNVME_PRP_DATA_ADMIT) == VNVME_PRP_DATA_ADMIT);
    perm[16 + 40] = PR | PW;
    /* W3: WRITE partial overlap (unaligned PRP1, then the page whole) */
    ctx = "W3 WRITE partial overlap";
    {u64 e[2] = {DPG(16), DPG(17)}; put_list(LPG(5), e, 2);}
    CHECK(run_one(VNVME_IO_WRITE, 220, 2, DPG(16) + 0x200, LPG(5), VNVME_PRP_DATA_DATA) == VNVME_PRP_DATA_DATA);
    /* W4: WRITE whose short last segment overlaps an earlier whole page */
    ctx = "W4 WRITE short last over whole page";
    {u64 e[2] = {DPG(18), DPG(18)}; put_list(LPG(5), e, 2);}
    CHECK(run_one(VNVME_IO_WRITE, 230, 2, DPG(19) + 0x400, LPG(5), VNVME_PRP_DATA_DATA) == VNVME_PRP_DATA_DATA);
    /* W5: disjoint WRITE (PRP1 tail, PRP2 head of the same page) is accepted, as before */
    ctx = "W5 WRITE two disjoint partials of one page";
    CHECK(run_one(VNVME_IO_WRITE, 240, 1, DPG(19) + 0x400, DPG(19), 0) == 0);
    /* R1: READ partial overlap, but one page is not writable: data admission */
    ctx = "R1 READ overlap with a read-only page";
    perm[16 + 14] = PR;
    {u64 e[2] = {DPG(13), DPG(14)}; put_list(LPG(6), e, 2);}
    CHECK(run_one(VNVME_IO_READ, 300, 2, DPG(13) + 0x400, LPG(6), VNVME_PRP_DATA_ADMIT) == VNVME_PRP_DATA_ADMIT);
    perm[16 + 14] = PR | PW;
    /* R2: the dummy page repeated, one repeat outside guest RAM: data admission */
    ctx = "R2 READ repeat outside RAM";
    {u64 e[4] = {DUMMY, RAM_BASE + RAM_SIZE, DUMMY, DUMMY}; put_list(LPG(6), e, 4);}
    CHECK(run_one(VNVME_IO_READ, 310, 4, DUMMY + 0x100, LPG(6), VNVME_PRP_DATA_ADMIT) == VNVME_PRP_DATA_ADMIT);
    /* R3: partial overlap whose whole page is a live queue page (the CQ): data-queue */
    ctx = "R3 READ overlap on a queue page";
    {u64 e[2] = {IOCQ, DPG(15)}; put_list(LPG(6), e, 2);}
    CHECK(run_one(VNVME_IO_READ, 320, 2, IOCQ + 0x800, LPG(6), VNVME_PRP_DATA_QUEUE) == VNVME_PRP_DATA_QUEUE);
    /* R4: the dummy page is the PRP list's own page: data-list */
    ctx = "R4 READ overlap with the list page";
    {u64 e[3] = {DPG(15), LPG(7), LPG(7)}; put_list(LPG(7) + 0x100, e, 3);}
    CHECK(run_one(VNVME_IO_READ, 330, 3, LPG(7) + 0x800, LPG(7) + 0x100, VNVME_PRP_DATA_LIST) == VNVME_PRP_DATA_LIST);
    /* R5: partial overlap with an unaligned list entry: entry-align */
    ctx = "R5 READ unaligned entry";
    {u64 e[2] = {DPG(15) + 8, DPG(15)}; put_list(LPG(6), e, 2);}
    CHECK(run_one(VNVME_IO_READ, 340, 2, DPG(15) + 0x10, LPG(6), VNVME_PRP_ENTRY_ALIGN) == VNVME_PRP_ENTRY_ALIGN);
    /* R6: list fragment not readable */
    ctx = "R6 READ list not readable";
    perm[4 + 6] = PW;
    CHECK(run_one(VNVME_IO_READ, 350, 3, DUMMY, LPG(6), VNVME_PRP_LIST_ADMIT) == VNVME_PRP_LIST_ADMIT);
    perm[4 + 6] = PR | PW;
    ctx = "";
}

/* ---- randomised differential test ---- */
static u64 rs = 1;
static u64 rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static u64 hot[6];
static bool clean; /* a command built only from hot/data pages (so large plans get accepted too) */
static u64 pick_page(void)
{
    unsigned r = (unsigned)(rnd() % 64);
    if (clean) return r < 32 ? hot[rnd() % 6] : DPG(rnd() % NDATA);
    if (r < 30) return hot[rnd() % 6];
    if (r < 56) return DPG(rnd() % NDATA);
    if (r < 59) return PGA(rnd() % 4);                         /* a queue page */
    if (r < 61) return LPG(rnd() % 12);                        /* the list area */
    if (r == 61) return RAM_BASE + RAM_SIZE + (rnd() % 4) * PAGE; /* outside RAM */
    if (r == 62) return UINT64_C(0xfffffffffffff000);
    return 0;
}
static u64 pick_entry(void)
{
    u64 a = pick_page();
    if (!clean && rnd() % 40 == 0) a += (rnd() % 512) * 8; /* unaligned entry */
    return a;
}
static void random_cmd(void)
{
    if (rnd() % 8 == 0)
        for (unsigned k = 0; k < 6; k++) hot[k] = rnd() % 4 ? DPG(rnd() % NDATA) : LPG(rnd() % 12);
    if (rnd() % 8 == 0) {                                      /* permission changes outside the queue pages */
        unsigned p = 4 + (unsigned)(rnd() % (NPAGES - 4));
        perm[p] = (u8[]){PR | PW, PR | PW, PR, PW, 0}[rnd() % 5];
    }
    if (rnd() % 64 == 0)
        for (unsigned p = 4; p < NPAGES; p++) perm[p] = PR | PW;
    if (rnd() % 16 == 0) {                                     /* fresh guest bytes on a hot page */
        u64 h = hot[rnd() % 6];
        for (unsigned i = 0; i < PAGE; i++) ram[h - RAM_BASE + i] = (u8)rnd();
    }
    clean = rnd() % 4 == 0;
    u8 op = rnd() % 10 < 7 ? VNVME_IO_READ : VNVME_IO_WRITE;
    unsigned r = (unsigned)(rnd() % 20);
    u32 blocks = r < 14 ? 1 + (u32)(rnd() % 8) : r < 18 ? 9 + (u32)(rnd() % 32) : 41 + (u32)(rnd() % 216);
    u64 lba = rnd() % (NS_BLOCKS - blocks + 1);
    u64 prp1 = pick_page();
    unsigned o = (unsigned)(rnd() % 4);
    if (o == 1) prp1 += rnd() % PAGE;
    else if (o == 2) prp1 += (rnd() % 8) * 512;
    size_t total = (size_t)blocks * PAGE, first = PAGE - (size_t)(prp1 % PAGE);
    if (first > total) first = total;
    size_t count = (total - first + PAGE - 1) / PAGE;
    u64 prp2 = 0;
    if (count == 1) {
        prp2 = rnd() % 32 ? pick_entry() : 0;
    } else if (count > 1) {
        /* the list: mostly in the list area at a qword offset (so it may chain), sometimes on a data page */
        unsigned w = (unsigned)(rnd() % 10);
        u64 list = w < 7 ? LPG(rnd() % 12) + (rnd() % 512) * 8 : w < 9 ? pick_page() + (rnd() % 512) * 8 : pick_page() + rnd() % PAGE;
        prp2 = rnd() % 64 ? list : 0;
        u64 at = list;
        for (size_t got = 0, frag = 0; got < count && frag < 3; frag++) {
            if (at < LPG(0) || at % 8 || at - RAM_BASE >= RAM_SIZE) break;   /* the planner refuses or reads no RAM */
            size_t room = (PAGE - (size_t)(at % PAGE)) / 8, need = count - got;
            size_t n = need <= room ? need : room;
            for (size_t i = 0; i < n; i++) {
                u64 v = pick_entry();
                if (need > room && i + 1 == n) {                 /* chain pointer */
                    unsigned cw = (unsigned)(rnd() % 16);
                    v = clean || cw < 12 ? LPG(rnd() % 12) : cw < 14 ? pick_page() : pick_page() + (rnd() % 512) * 8;
                }
                memcpy(ram + (at - RAM_BASE) + 8 * i, &v, 8);
                if (need > room && i + 1 == n) at = v;
            }
            got += need <= room ? n : n - 1;
        }
    }
    run_one(op, lba, blocks, prp1, prp2, -1);
}

int main(int argc, char **argv)
{
    unsigned long iters = argc > 1 ? strtoul(argv[1], NULL, 0) : 200000;
    rs = argc > 2 ? strtoull(argv[2], NULL, 0) : 20261111;
    if (!rs) rs = 1;
    ram = calloc(1, RAM_SIZE); exp_ram = calloc(1, RAM_SIZE);
    size_t all = (size_t)(NS_BLOCKS + 2 * GUARD) * PAGE;
    disk_all = malloc(all); disk = disk_all + (size_t)GUARD * PAGE; ref_disk = malloc((size_t)NS_BLOCKS * PAGE);
    for (size_t i = 0; i < all; i++) disk_all[i] = (u8)rnd();
    guard_lo = malloc((size_t)GUARD * PAGE); guard_hi = malloc((size_t)GUARD * PAGE);
    memcpy(guard_lo, disk_all, (size_t)GUARD * PAGE); memcpy(guard_hi, disk + (size_t)NS_BLOCKS * PAGE, (size_t)GUARD * PAGE);
    memcpy(ref_disk, disk, (size_t)NS_BLOCKS * PAGE);
    for (size_t i = 4 * PAGE; i < RAM_SIZE; i++) ram[i] = (u8)rnd();
    setup();
    shapes();
    unsigned long shape_fails = (unsigned long)fails;
    for (unsigned k = 0; k < 6; k++) hot[k] = DPG(rnd() % NDATA);
    for (unsigned long i = 0; i < iters; i++) {
        ctx = "random";
        random_cmd();
        if (ctrl.fatal) { CHECK(!"controller fatal"); break; }
    }
    CHECK(!memcmp(disk, ref_disk, (size_t)NS_BLOCKS * PAGE));     /* whole media at the end */
    if (iters >= 20000) {                                           /* the random mix reached every class */
        CHECK(st_part > 1000 && st_dup > 100 && st_big > 100 && st_ok[0] > 1000 && st_ok[1] > 1000);
        CHECK(st_rule[0][VNVME_PRP_DATA_DATA] > 1000 && st_dupw > 1000);
        int need[] = {VNVME_PRP_PRP2, VNVME_PRP_PRP2_ALIGN, VNVME_PRP_LIST_PTR, VNVME_PRP_LIST_ADMIT, VNVME_PRP_ENTRY_ALIGN,
                      VNVME_PRP_DATA_ADMIT, VNVME_PRP_DATA_QUEUE, VNVME_PRP_DATA_LIST};
        for (unsigned i = 0; i < sizeof(need) / sizeof(need[0]); i++) CHECK(st_rule[1][need[i]] > 10);
    }
    printf("{\"checks\":%lu,\"failed\":%d,\"shape_failures\":%lu,\"iterations\":%lu,\"reads_ok\":%lu,\"writes_ok\":%lu,"
           "\"writes_exact_repeat\":%lu,\"reads_exact_repeat\":%lu,\"reads_partial_overlap\":%lu,\"over_32_blocks\":%lu,\"read_refused\":{",
           checks, fails, shape_fails, iters, st_ok[1], st_ok[0], st_dupw, st_dup, st_part, st_big);
    for (int i = 1; i < VNVME_PRP_RULES; i++) printf("%s\"%d\":%lu", i > 1 ? "," : "", i, st_rule[1][i]);
    printf("},\"write_refused\":{");
    for (int i = 1; i < VNVME_PRP_RULES; i++) printf("%s\"%d\":%lu", i > 1 ? "," : "", i, st_rule[0][i]);
    printf("}}\n");
    printf("prp-overlap-n4: %lu checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
