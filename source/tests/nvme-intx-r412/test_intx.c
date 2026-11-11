/* SPDX-License-Identifier: MIT */
/*
 * r4.12: hardware run 20261110-200708 sequence on the real model (hv_nvme_queue.c): the firmware runs
 * I/O on q1, the guest disables the controller (vnvme_init, as CC.EN=0 does), re-creates the admin
 * queue and I/O queues, issues Windows' admin bring-up (identify, set/get features, create CQ/SQ, get
 * log page, an AER that stays held, get features VWC) and acknowledges each completion. The INTx
 * condition must follow "some CQ with IEN has an unacknowledged entry": asserted on each post, 0 once
 * everything posted is acknowledged - including with the AER held and after a mask/unmask cycle.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hv_nvme_queue.h"

#define RAM_SIZE (1u << 20)
static u8 *ram;
static u64 RAM_BASE = 0x40000000;
static int fails, checks, line, edges;
#define CHECK(n, c) do { checks++; if (!(c)) { fails++; printf("FAIL: %s (line %d)\n", n, __LINE__); } } while (0)

static void *span(void *o, u64 ipa, size_t n, bool w)
{
    (void)o; (void)w;
    if (ipa < RAM_BASE || ipa + n > RAM_BASE + RAM_SIZE) return NULL;
    return ram + (ipa - RAM_BASE);
}
static bool rb(void *o, u64 lba, u32 n, void *buf) { (void)o; memset(buf, (int)(lba & 0xff), (size_t)n * 4096); return true; }
static bool wb(void *o, u64 lba, u32 n, const void *buf) { (void)o; (void)lba; (void)n; (void)buf; return true; }
static bool fl(void *o) { (void)o; return true; }
static void irq(void *o, bool a) { (void)o; if (a != line) edges++; line = a; }
static void caps(void *o, struct vnvme_caps *c) { (void)o; c->writable = true; c->vwc = true; c->max_blocks = 256; }
static const struct vnvme_backend_ops ops = {.guest_span = span, .read_blocks = rb, .write_blocks = wb,
                                             .flush = fl, .irq = irq, .caps = caps};
static struct vnvme_ctrl ctrl;

struct q { u64 sq, cq; u16 size, tail, head; u8 phase; };
static void sqe(struct q *q, u8 op, u16 cid, u32 nsid, u64 prp1, u32 c10, u32 c11, u32 c12)
{
    struct vnvme_command *c = (struct vnvme_command *)(ram + (q->sq - RAM_BASE)) + q->tail;
    memset(c, 0, sizeof(*c));
    c->opcode = op; c->cid = cid; c->nsid = nsid; c->prp1 = prp1; c->cdw10 = c10; c->cdw11 = c11; c->cdw12 = c12;
    q->tail = (u16)((q->tail + 1) % q->size);
}
/* guest consumes every visible CQE (phase match) and rings the CQ head doorbell once */
static int reap(u16 qid, struct q *q, u16 *last_cid, u16 *last_status)
{
    struct vnvme_completion *c = (struct vnvme_completion *)(ram + (q->cq - RAM_BASE));
    int n = 0;
    while ((c[q->head].status & 1) == q->phase) {
        *last_cid = c[q->head].cid; *last_status = c[q->head].status >> 1;
        q->head = (u16)((q->head + 1) % q->size);
        if (!q->head) q->phase ^= 1;
        n++;
    }
    if (n) vnvme_cq_doorbell(&ctrl, qid, q->head);
    return n;
}
static u16 cmd(struct q *a, u8 op, u16 cid, u32 nsid, u64 prp1, u32 c10, u32 c11, bool expect_cqe)
{
    sqe(a, op, cid, nsid, prp1, c10, c11, 0);
    vnvme_sq_doorbell(&ctrl, 0, a->tail);
    u16 lc = 0xffff, ls = 0xffff;
    if (expect_cqe) {
        CHECK("line asserted on the admin completion", line == 1);
        CHECK("exactly one admin CQE visible", reap(0, a, &lc, &ls) == 1 && lc == cid);
        CHECK("line drops when the guest acks the admin CQE", line == 0 && !ctrl.irq_asserted);
    } else {
        CHECK("held command: no CQE, line stays low", line == 0 && reap(0, a, &lc, &ls) == 0);
    }
    return ls;
}

int main(void)
{
    ram = calloc(1, RAM_SIZE);
    u64 buf = RAM_BASE + 0x80000;
    vnvme_init(&ctrl, 24424454, &ops, NULL);

    /* ---- firmware session: admin queue, I/O queue pair, 957 reads + 14 writes, every CQE acked
     * except that the firmware leaves its last I/O completion un-acked (worst case) ---- */
    struct q fa = {RAM_BASE + 0x1000, RAM_BASE + 0x2000, 32, 0, 0, 1};
    CHECK("fw admin queue", vnvme_set_admin_queue(&ctrl, fa.sq, fa.cq, fa.size, fa.size));
    ctrl.iosqes = 6; ctrl.iocqes = 4;
    cmd(&fa, VNVME_ADMIN_CREATE_CQ, 1, 0, RAM_BASE + 0x4000, (63u << 16) | 1, 0x3, true);
    cmd(&fa, VNVME_ADMIN_CREATE_SQ, 2, 0, RAM_BASE + 0x3000, (63u << 16) | 1, (1u << 16) | 1, true);
    struct q fio = {RAM_BASE + 0x3000, RAM_BASE + 0x4000, 64, 0, 0, 1};
    for (int i = 0; i < 971; i++) {
        sqe(&fio, i < 957 ? VNVME_IO_READ : 0x01, (u16)i, 1, buf, (u32)i, 0, 0);
        vnvme_sq_doorbell(&ctrl, 1, fio.tail);
        CHECK("fw I/O completion asserts", line == 1);
        u16 lc, ls;
        if (i < 970) { reap(1, &fio, &lc, &ls); CHECK("fw I/O ack drops", line == 0); }
    }
    CHECK("fw left one I/O CQE un-acked: line high", line == 1);

    /* ---- guest takes over: CC.EN=0 (the model's disable path runs vnvme_init) ---- */
    vnvme_init(&ctrl, 24424454, &ops, NULL);
    line = ctrl.irq_asserted; /* hv_nvme.c cc_write: s1_recompute_irq after the disable */
    struct q wa = {RAM_BASE + 0x10000, RAM_BASE + 0x11000, 32, 0, 0, 1};
    memset(ram + 0x10000, 0, 0x2000);
    CHECK("guest admin queue", vnvme_set_admin_queue(&ctrl, wa.sq, wa.cq, wa.size, wa.size));
    CHECK("stale firmware CQE does not hold the line after reset", line == 0 && !ctrl.irq_asserted);
    ctrl.iosqes = 6; ctrl.iocqes = 4;
    u16 c = 0, st;
    st = cmd(&wa, VNVME_ADMIN_IDENTIFY, c++, 0, buf, 1, 0, true);            CHECK("identify ctrl ok", st == 0);
    st = cmd(&wa, VNVME_ADMIN_IDENTIFY, c++, 0xffffffff, buf, 6, 0, true);   CHECK("identify CNS 6 -> 0x2", st == 0x2);
    st = cmd(&wa, VNVME_ADMIN_IDENTIFY, c++, 0, buf, 2, 0, true);            CHECK("identify NS list ok", st == 0);
    st = cmd(&wa, VNVME_ADMIN_IDENTIFY, c++, 1, buf, 0, 0, true);            CHECK("identify NS ok", st == 0);
    st = cmd(&wa, VNVME_ADMIN_IDENTIFY, c++, 1, buf, 3, 0, true);            CHECK("identify NS desc ok", st == 0);
    st = cmd(&wa, VNVME_ADMIN_GET_FEATURES, c++, 0, 0, 0xd0, 0, true);       CHECK("get features 0xd0 -> 0x2", st == 0x2);
    st = cmd(&wa, VNVME_ADMIN_SET_FEATURES, c++, 0, 0, 7, 1, true);          CHECK("set features #queues ok", st == 0);
    st = cmd(&wa, VNVME_ADMIN_SET_FEATURES, c++, 0, 0, 8, 0, true);          CHECK("set features coalescing ok", st == 0);
    st = cmd(&wa, VNVME_ADMIN_SET_FEATURES, c++, 0, 0, 1, 4, true);          CHECK("set features arbitration ok", st == 0);
    st = cmd(&wa, VNVME_ADMIN_CREATE_CQ, c++, 0, RAM_BASE + 0x13000, (255u << 16) | 1, 0x3, true); CHECK("create CQ1 ok", st == 0);
    st = cmd(&wa, VNVME_ADMIN_CREATE_SQ, c++, 0, RAM_BASE + 0x20000, (255u << 16) | 1, (1u << 16) | 5, true); CHECK("create SQ1 ok", st == 0);
    st = cmd(&wa, VNVME_ADMIN_GET_LOG_PAGE, c++, 0xffffffff, buf, 0x007f00c0, 0, true); CHECK("log 0xc0 -> invalid log page", st == 0x109);
    st = cmd(&wa, VNVME_ADMIN_GET_FEATURES, c++, 0, 0, 4, 0, true);          CHECK("get features temp ok", st == 0);
    st = cmd(&wa, VNVME_ADMIN_SET_FEATURES, c++, 0, 0, 0xb, 0x1f, true);     CHECK("set features AEC ok", st == 0);
    cmd(&wa, VNVME_ADMIN_ASYNC_EVENT, c++, 0, 0, 0, 0, false);
    st = cmd(&wa, VNVME_ADMIN_GET_FEATURES, c++, 0, 0, 0xc, 0, true);        CHECK("get features APST -> 0x2", st == 0x2);
    st = cmd(&wa, VNVME_ADMIN_GET_FEATURES, c++, 1, 0, 0x7f, 1, true);       CHECK("get features 0x7f -> 0x2", st == 0x2);
    st = cmd(&wa, VNVME_ADMIN_GET_LOG_PAGE, c++, 0xffffffff, buf, 0x007f0002, 0, true); CHECK("SMART log ok", st == 0);
    st = cmd(&wa, VNVME_ADMIN_GET_FEATURES, c++, 0, 0, 6, 0, true);          CHECK("get features VWC ok (cid 23 on hw)", st == 0);

    /* the hardware stall point: VWC completion posted while the previous one is still un-acked (the
     * guest masked with INTMS and acks in its DPC) - the line must drop once both are acked */
    sqe(&wa, VNVME_ADMIN_GET_LOG_PAGE, c++, 0xffffffff, buf, 0x007f0002, 0, 0);
    vnvme_sq_doorbell(&ctrl, 0, wa.tail);
    sqe(&wa, VNVME_ADMIN_GET_FEATURES, c++, 0, 0, 6, 0, 0);
    vnvme_sq_doorbell(&ctrl, 0, wa.tail);
    CHECK("first CQE pending (the model serialises admin: one CQE outstanding at a time)", line == 1);
    u16 lc, ls;
    int got = reap(0, &wa, &lc, &ls);  /* the ack fetches and completes the next SQE: line high again */
    CHECK("second CQE posted inside the CQ-head doorbell", got == 1 && line == 1 && lc == c - 2);
    got = reap(0, &wa, &lc, &ls);
    CHECK("guest reaps the second", got == 1 && lc == c - 1);
    CHECK("all posted completions acked, AER held: line 0", line == 0 && !ctrl.irq_asserted);
    /* r4.13: a READ past the end of the namespace completes LBA Out of Range (0x80), not 0x02, without
     * backend I/O; the last block itself is in range */
    {
        struct q io = {RAM_BASE + 0x20000, RAM_BASE + 0x13000, 256, 0, 0, 1};
        u16 lc, ls;
        u64 nsze = 24424454;
        sqe(&io, VNVME_IO_READ, 500, 1, buf, (u32)(nsze - 1), 0, 0);
        vnvme_sq_doorbell(&ctrl, 1, io.tail);
        CHECK("last block READ ok", reap(1, &io, &lc, &ls) == 1 && ls == 0);
        sqe(&io, VNVME_IO_READ, 501, 1, buf, (u32)nsze, 0, 0);
        vnvme_sq_doorbell(&ctrl, 1, io.tail);
        CHECK("READ at NSZE -> 0x80 LBA Out of Range", reap(1, &io, &lc, &ls) == 1 && ls == 0x80);
        sqe(&io, VNVME_IO_READ, 502, 1, buf, (u32)(nsze - 1), 0, 1);  /* 2 blocks straddling the end */
        vnvme_sq_doorbell(&ctrl, 1, io.tail);
        CHECK("READ straddling the end -> 0x80", reap(1, &io, &lc, &ls) == 1 && ls == 0x80 && ctrl.refused_reads == 2);
        CHECK("line low after the I/O acks", line == 0);
    }
    printf("nvme-intx-r412: %d checks, %d failed (edges %d)\n", checks, fails, edges);
    free(ram);
    return fails ? 1 : 0;
}
