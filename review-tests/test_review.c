/* SPDX-License-Identifier: MIT */
/* Independent adversarial review tests for resident r2 (host only, no hardware).
 *
 * Oracle: the whole guest-RAM arena is ASan-POISONED for the duration of every
 * model entry (s1_mmio / s1_config). The guest_span provider unpoisons exactly
 * the span it grants, and only when every 4 KiB page of that span is admitted
 * (RW, like the live provider which requires RW in both directions). Therefore
 * ANY model read or write of guest memory that was not granted during the same
 * synchronous call aborts under ASan, on every path (SQE fetch, CQE write, PRP
 * list walk, data copy, shutdown-time abort completions, AER completion, log and
 * identify data-in). Refused pages additionally carry a canary that is compared
 * after every call. Compiled against the r2.1 sources (adapted to r2.1 fixes: former info lines are now assertions). */
#include "model.h"
#include "ram_disk.h"
#include <sanitizer/asan_interface.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ECAM UINT64_C(0xf28000000)
#define BARB UINT64_C(0xf30000000)
#define RAM UINT64_C(0x100000000)
#define PAGE 4096u
#define NPAGES 256u
#define RAM_SIZE (NPAGES * PAGE)
#define ASQ RAM
#define ACQ (RAM + PAGE)
#define IOSQ (RAM + 2 * PAGE)
#define IOCQ (RAM + 3 * PAGE)
#define LIST (RAM + 8 * PAGE)
#define DATA (RAM + 16 * PAGE)
#define CANARY 0x5a

static struct s1_model m;
static u8 arena[RAM_SIZE] __attribute__((aligned(4096)));
static u8 perms[NPAGES];          /* 3 = admitted RW, anything else = refused */
static u8 shadow[RAM_SIZE];       /* refused-page snapshot */
static unsigned checks, grants, refusals, calls;
static bool line;
static unsigned edges;

#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)

static void *span(void *o, u64 a, size_t n, bool w)
{
    (void)o; (void)w;
    if (a < RAM || !n || a - RAM > RAM_SIZE || n > RAM_SIZE - (a - RAM)) { refusals++; return NULL; }
    for (u64 p = (a - RAM) / PAGE; p <= (a - RAM + n - 1) / PAGE; p++)
        if (perms[p] != 3) { refusals++; return NULL; }
    grants++;
    ASAN_UNPOISON_MEMORY_REGION(arena + (a - RAM), n);
    return arena + (a - RAM);
}
static bool map(void *o, u64 a, size_t n) { (void)o; return a == BARB && n == S1_BAR_SIZE; }
static bool unmap(void *o, u64 a, size_t n) { (void)o; return a == BARB && n == S1_BAR_SIZE; }
static void irq(void *o, bool l) { (void)o; CHECK(l != line); line = l; edges++; }
static const struct s1_ops ops = { span, map, unmap, irq, NULL };

static void snapshot_refused(void)
{
    for (unsigned p = 0; p < NPAGES; p++)
        if (perms[p] != 3) memcpy(shadow + p * PAGE, arena + p * PAGE, PAGE);
}
static void verify_refused(void)
{
    for (unsigned p = 0; p < NPAGES; p++)
        if (perms[p] != 3) CHECK(memcmp(shadow + p * PAGE, arena + p * PAGE, PAGE) == 0);
}
static void enter(void) { calls++; snapshot_refused(); ASAN_POISON_MEMORY_REGION(arena, RAM_SIZE); }
static void leave(void) { ASAN_UNPOISON_MEMORY_REGION(arena, RAM_SIZE); verify_refused(); }
static bool cfgw(u32 off, unsigned b, u64 v) { enter(); bool r = s1_config(&m, ECAM + off, b, true, &v); leave(); return r; }
static bool wr(u32 off, unsigned b, u64 v) { enter(); bool r = s1_mmio(&m, BARB + off, b, true, &v); leave(); return r; }
static u64 rd(u32 off, unsigned b) { u64 v = 0; enter(); bool r = s1_mmio(&m, BARB + off, b, false, &v); leave(); CHECK(r); return v; }
static u8 *ptr(u64 a) { CHECK(a >= RAM && a - RAM < RAM_SIZE); return arena + (a - RAM); }
static void deny(u64 a, u8 fill) { unsigned p = (unsigned)((a - RAM) / PAGE); memset(arena + p * PAGE, fill, PAGE); perms[p] = 0; }
static void allow(u64 a) { perms[(a - RAM) / PAGE] = 3; }

struct q { u16 n, tail, head; u8 phase; u64 sq, cq; unsigned id; };
static struct q aq, ioq;
static u16 next_cid = 1;
static struct vnvme_command cmd(u8 op, u32 nsid, u64 prp1, u32 c10, u32 c11)
{
    struct vnvme_command c; memset(&c, 0, sizeof c);
    c.opcode = op; c.nsid = nsid; c.prp1 = prp1; c.cdw10 = c10; c.cdw11 = c11; return c;
}
static struct vnvme_completion cqe(struct q *q, u16 slot) { struct vnvme_completion d; memcpy(&d, ptr(q->cq + (u64)slot * 16), 16); return d; }
static void place(struct q *q, struct vnvme_command c) { memcpy(ptr(q->sq + (u64)q->tail * 64), &c, 64); q->tail = (u16)((q->tail + 1) % q->n); }
static int submit(struct q *q, struct vnvme_command c, bool ack)
{
    c.cid = next_cid++; place(q, c);
    CHECK(wr(0x1000 + q->id * 8, 4, q->tail));
    struct vnvme_completion d = cqe(q, q->head);
    if ((d.status & 1) != q->phase || d.cid != c.cid) return -1;
    if (ack) { q->head = (u16)((q->head + 1) % q->n); if (!q->head) q->phase ^= 1; CHECK(wr(0x1004 + q->id * 8, 4, q->head)); }
    return d.status >> 1;
}
static void fresh(void)
{
    line = false; edges = 0; memset(&m, 0, sizeof m); memset(arena, 0, sizeof arena); memset(perms, 3, sizeof perms);
    enter(); CHECK(s1_init(&m, &ops, NULL, ECAM, BARB, S1_BAR_SIZE)); leave();
    CHECK(cfgw(0x10, 4, (u32)BARB)); CHECK(cfgw(0x14, 4, BARB >> 32)); CHECK(cfgw(4, 2, S1_MEM | S1_BME)); CHECK(m.mapped);
}
static void enable(u16 n)
{
    aq = (struct q){ n, 0, 0, 1, ASQ, ACQ, 0 };
    CHECK(wr(0x24, 4, ((u32)(n - 1) << 16) | (n - 1))); CHECK(wr(0x28, 8, ASQ)); CHECK(wr(0x30, 8, ACQ));
    CHECK(wr(0x14, 4, S1_CC_VALID)); CHECK(rd(0x1c, 4) == 1);
}
static void io_pair(u16 sqn, u16 cqn)
{
    ioq = (struct q){ sqn, 0, 0, 1, IOSQ, IOCQ, 1 };
    CHECK(submit(&aq, cmd(5, 0, IOCQ, ((u32)(cqn - 1) << 16) | 1, 3), true) == 0);
    CHECK(submit(&aq, cmd(1, 0, IOSQ, ((u32)(sqn - 1) << 16) | 1, (1u << 16) | 1), true) == 0);
}

/* 1. normal shutdown: pending SQEs whose SQ page is refused are discarded unread */
static void t_shutdown_refused_sqe(void)
{
    fresh(); enable(8); io_pair(8, 8);
    struct vnvme_command r = cmd(2, 1, DATA, 0, 0);
    place(&ioq, r); place(&ioq, r); place(&ioq, r);
    CHECK(wr(0x1008, 4, ioq.tail));               /* first executes, two wait (backpressure) */
    u64 reads = m.backend_reads;
    deny(IOSQ, CANARY);                           /* revoke SQ page after fetch of #1 */
    CHECK(wr(0x14, 4, S1_CC_VALID | 0x4000));
    CHECK((rd(0x1c, 4) & 0xc) == 8);
    CHECK(m.queue.discarded_commands == 2 && m.queue.aborted_commands == 0 && m.backend_reads == reads);
}
/* 2. normal shutdown: IO CQ refused -> fatal; documents that shutdown keeps posting
 *    to the admin CQ after fatal was latched (BUILD-NOTES #11 says fatal stops all). */
static void t_shutdown_refused_cq_then_admin(void)
{
    fresh(); enable(8); io_pair(8, 8);
    struct vnvme_command r = cmd(2, 1, DATA, 0, 0);
    place(&ioq, r); place(&ioq, r); CHECK(wr(0x1008, 4, ioq.tail));
    /* admin: one executes (Get Features), one waits */
    place(&aq, cmd(10, 0, 0, 7, 0)); place(&aq, cmd(10, 0, 0, 7, 0)); CHECK(wr(0x1000, 4, aq.tail));
    deny(IOCQ, CANARY);
    u64 before_completions = m.queue.stats.completions;
    CHECK(wr(0x14, 4, S1_CC_VALID | 0x4000));
    u32 csts = (u32)rd(0x1c, 4);
    CHECK((csts & 2) && (csts & 0xc) == 8);       /* CFS + SHST complete */
    CHECK(m.queue.fatal);
    /* r2.1 N2: after fatal nothing more is read or posted: IO and admin entries discarded */
    CHECK(m.queue.discarded_commands == 2 && m.queue.aborted_commands == 0);
    CHECK(m.queue.stats.completions == before_completions);
}
/* 3. mass abort into a tiny CQ: discard counted, CQ never written past its span,
 *    and documents CQ occupancy reaching cq_size (NVMe full = size-1). */
static void t_mass_abort_cq_full(void)
{
    fresh(); enable(8);
    ioq = (struct q){ 256, 0, 0, 1, IOSQ, IOCQ, 1 };
    CHECK(submit(&aq, cmd(5, 0, IOCQ, (1u << 16) | 1, 3), true) == 0);                       /* CQ size 2 */
    /* IOSQ 256 entries = 16 KiB: pages 2..5 overlap IOCQ page 3 -> use a separate SQ base */
    ioq.sq = RAM + 32 * PAGE;
    CHECK(submit(&aq, cmd(1, 0, ioq.sq, (255u << 16) | 1, (1u << 16) | 1), true) == 0);
    memset(ptr(IOCQ + 32), CANARY, PAGE - 32);
    struct vnvme_command r = cmd(2, 1, DATA, 0, 0);
    for (unsigned i = 0; i < 255; i++) place(&ioq, r);
    CHECK(wr(0x1008, 4, ioq.tail));
    u64 reads = m.backend_reads;
    CHECK(wr(0x14, 4, S1_CC_VALID | 0x4000));
    CHECK(m.backend_reads == reads);
    for (unsigned i = 32; i < PAGE; i++) CHECK(ptr(IOCQ)[i] == CANARY);
    CHECK(m.queue.aborted_commands + m.queue.discarded_commands == 254);
    CHECK(m.queue.queues[1].cq_pending <= m.queue.queues[1].cq_size - 1); /* r2.1 N3 */
    CHECK(m.queue.aborted_commands == 0);
}
/* 4. Abort of AER when the admin CQ slot is refused: fatal, nothing written */
static void t_abort_aer_refused_cq(void)
{
    fresh(); enable(4);
    CHECK(submit(&aq, cmd(0x0c, 0, 0, 0, 0), true) == -1);
    u16 aer = (u16)(next_cid - 1);
    deny(ACQ, CANARY);
    struct vnvme_command a = cmd(8, 0, 0, ((u32)aer << 16), 0); a.cid = next_cid++; place(&aq, a);
    CHECK(wr(0x1000, 4, aq.tail));
    CHECK(m.queue.fatal && (rd(0x1c, 4) & 2));
    CHECK(wr(0x14, 4, 0)); allow(ACQ); enable(4);  /* recovery via CC.EN 1->0->1 */
}
/* 5. PRP list: refused list page, refused data page behind a list, offset list,
 *    list overlapping destination. All-or-nothing; nothing outside grants touched. */
static void t_prp_lists(void)
{
    fresh(); enable(8); io_pair(8, 8);
    u64 *l = (u64 *)ptr(LIST);
    for (unsigned i = 0; i < 31; i++) l[i] = DATA + (u64)(i + 1) * PAGE;
    struct vnvme_command r = cmd(2, 1, DATA, 0, 0); r.prp2 = LIST; r.cdw12 = 31; /* 32 blocks */
    CHECK(submit(&ioq, r, true) == 0);
    deny(LIST, CANARY);
    CHECK(submit(&ioq, r, true) == VNVME_SC_DATA_TRANSFER_ERROR);
    allow(LIST); for (unsigned i = 0; i < 31; i++) l[i] = DATA + (u64)(i + 1) * PAGE;
    memset(ptr(DATA), 0x11, PAGE);
    deny(DATA + 20 * PAGE, CANARY);
    CHECK(submit(&ioq, r, true) == VNVME_SC_DATA_TRANSFER_ERROR);
    CHECK(ptr(DATA)[0] == 0x11);                   /* no partial copy before validation */
    allow(DATA + 20 * PAGE);
    /* list pointer with qword-aligned non-zero offset (legal in NVMe 1.3 4.3) */
    u64 *l2 = (u64 *)ptr(LIST + 256);
    for (unsigned i = 0; i < 31; i++) l2[i] = DATA + (u64)(i + 1) * PAGE;
    r.prp2 = LIST + 256;
    memset(ptr(DATA), 0x22, 32 * PAGE);
    CHECK(submit(&ioq, r, true) == 0);              /* r2.1 B3: accepted */
    for (unsigned b = 0; b < 32; b++) for (unsigned i = 0; i < PAGE; i++)
        CHECK(ptr(DATA + b * PAGE)[i] == (i < 32 ? (u8)(b * 17 + i) : 0));
    /* list that runs off its page and chains through its last entry */
    u64 *tail = (u64 *)ptr(LIST + PAGE - 16);       /* 2 slots: 1 data entry + chain */
    u64 *next = (u64 *)ptr(LIST + 2 * PAGE);
    tail[0] = DATA + PAGE; tail[1] = LIST + 2 * PAGE;
    for (unsigned i = 0; i < 30; i++) next[i] = DATA + (u64)(i + 2) * PAGE;
    r.prp2 = LIST + PAGE - 16; memset(ptr(DATA), 0x22, 32 * PAGE);
    CHECK(submit(&ioq, r, true) == 0);
    for (unsigned b = 0; b < 32; b++) CHECK(ptr(DATA + b * PAGE)[0] == (u8)(b * 17));
    /* single slot at the end of the page: chain only */
    u64 *one = (u64 *)ptr(LIST + PAGE - 8); one[0] = LIST + 3 * PAGE;
    u64 *n3 = (u64 *)ptr(LIST + 3 * PAGE); for (unsigned i = 0; i < 31; i++) n3[i] = DATA + (u64)(i + 1) * PAGE;
    r.prp2 = LIST + PAGE - 8; CHECK(submit(&ioq, r, true) == 0);
    /* chained pointer not page aligned -> rejected, nothing copied */
    tail[1] = LIST + 2 * PAGE + 8; r.prp2 = LIST + PAGE - 16; memset(ptr(DATA), 0x33, PAGE);
    CHECK(submit(&ioq, r, true) == VNVME_SC_DATA_TRANSFER_ERROR); CHECK(ptr(DATA)[0] == 0x33);
    tail[1] = LIST + 2 * PAGE;
    /* chained fragment refused -> rejected, canary intact (oracle) */
    deny(LIST + 2 * PAGE, CANARY);
    CHECK(submit(&ioq, r, true) == VNVME_SC_DATA_TRANSFER_ERROR); CHECK(ptr(DATA)[0] == 0x33);
    allow(LIST + 2 * PAGE);
    for (unsigned i = 0; i < 30; i++) next[i] = DATA + (u64)(i + 2) * PAGE;
    /* destination overlapping the second (chained) fragment */
    next[5] = LIST + 2 * PAGE; CHECK(submit(&ioq, r, true) == VNVME_SC_DATA_TRANSFER_ERROR);
    next[5] = DATA + 7 * PAGE;
    /* destination overlapping the first fragment's page bytes */
    next[6] = LIST; CHECK(submit(&ioq, r, true) == VNVME_SC_DATA_TRANSFER_ERROR); /* page holds fragment 1 */
    next[6] = DATA + 8 * PAGE;
    /* misaligned (non-qword) list pointer and pointers near 2^64 */
    r.prp2 = LIST + 260; CHECK(submit(&ioq, r, true) == VNVME_SC_DATA_TRANSFER_ERROR);
    r.prp2 = UINT64_MAX - 7; CHECK(submit(&ioq, r, true) == VNVME_SC_DATA_TRANSFER_ERROR);
    tail[1] = UINT64_MAX - (PAGE - 1); r.prp2 = LIST + PAGE - 16;
    CHECK(submit(&ioq, r, true) == VNVME_SC_DATA_TRANSFER_ERROR);
    tail[1] = LIST + 2 * PAGE;
    /* 2-page transfer with PRP2 at an offset is a data pointer: must be page aligned */
    struct vnvme_command r2 = cmd(2, 1, DATA, 0, 0); r2.prp2 = DATA + PAGE + 8; r2.cdw12 = 1;
    CHECK(submit(&ioq, r2, true) == VNVME_SC_DATA_TRANSFER_ERROR);
    /* list overlapping a destination */
    for (unsigned i = 0; i < 31; i++) l[i] = DATA + (u64)(i + 1) * PAGE;
    l[3] = LIST; r.prp2 = LIST;
    CHECK(submit(&ioq, r, true) == VNVME_SC_DATA_TRANSFER_ERROR);
    /* destination overlapping a live queue */
    l[3] = IOSQ;
    CHECK(submit(&ioq, r, true) == VNVME_SC_DATA_TRANSFER_ERROR);
    /* entries near UINT64_MAX */
    l[3] = UINT64_MAX - (PAGE - 1);
    CHECK(submit(&ioq, r, true) == VNVME_SC_DATA_TRANSFER_ERROR);
    /* admin data-in (log page, identify) into refused pages */
    deny(DATA, CANARY);
    CHECK(submit(&aq, cmd(2, 0, DATA, (1023u << 16) | 3, 0), true) == VNVME_SC_DATA_TRANSFER_ERROR);
    CHECK(submit(&aq, cmd(6, 0, DATA, 1, 0), true) == VNVME_SC_DATA_TRANSFER_ERROR);
    struct vnvme_command id = cmd(6, 1, DATA - PAGE + 8, 0, 0); id.prp2 = DATA; /* 2nd half refused */
    CHECK(submit(&aq, id, true) == VNVME_SC_DATA_TRANSFER_ERROR);
    struct vnvme_command lg = cmd(2, 0, DATA - 8, (1023u << 16) | 2, 0); lg.prp2 = DATA; lg.cdw12 = 0;
    CHECK(submit(&aq, lg, true) == VNVME_SC_DATA_TRANSFER_ERROR);
    CHECK(rd(0x1c, 4) == 1);
}
/* 6. CFS with an un-acked CQE on the other queue: CQ doorbells are ignored, so the
 *    INTx level cannot be lowered by the guest ISR (interrupt-storm risk). */
static void t_cfs_intx_stuck(void)
{
    fresh(); enable(8); io_pair(8, 8);
    CHECK(submit(&aq, cmd(10, 0, 0, 7, 0), false) == 0); /* admin CQE pending, not acked */
    CHECK(line);
    struct vnvme_command r = cmd(2, 1, DATA, 0, 0); place(&ioq, r);
    deny(IOSQ, CANARY);
    CHECK(wr(0x1008, 4, ioq.tail));
    CHECK(m.queue.fatal && (rd(0x1c, 4) & 2));
    u64 inv = m.queue.invalid_doorbells;
    CHECK(wr(0x1004, 4, (u16)((aq.head + 1) % aq.n)));
    CHECK(m.queue.invalid_doorbells == inv && !line); /* r2.1 B2: ack honoured, INTx low */
    unsigned g = grants; place(&aq, cmd(10, 0, 0, 7, 0));
    CHECK(wr(0x1000, 4, aq.tail)); CHECK(grants == g && m.queue.invalid_doorbells == inv + 1); /* no new fetch */
    CHECK(wr(0x14, 4, 0)); CHECK(!line);
}
/* 7. INTMS across controller reset (NVMe: reset by CC.EN 1->0) */
static void t_intms_reset(void)
{
    fresh(); enable(8);
    CHECK(wr(0x0c, 4, 1)); CHECK(rd(0x0c, 4) == 1);
    CHECK(wr(0x14, 4, 0)); enable(8);
    CHECK(rd(0x0c, 4) == 0); /* r2.1 N1 */
}
/* 8. SHN cleared while enabled after shutdown: RDY=1, SHST=0, but controller is dead */
static void t_shn_clear_without_reset(void)
{
    fresh(); enable(8);
    CHECK(wr(0x14, 4, S1_CC_VALID | 0x4000)); CHECK(rd(0x1c, 4) == 9);
    CHECK(wr(0x14, 4, S1_CC_VALID)); CHECK(rd(0x1c, 4) == 1);
    int st = submit(&aq, cmd(6, 0, DATA, 1, 0), true);
    CHECK(st < 0 && m.diag.shn_clear_no_reset == 1); /* r2.1 N5: counted in S1DG */
}
/* 9. CC enable with IOSQES/IOCQES = 0 (legal at enable in NVMe 1.3) */
static void t_cc_iosqes_zero(void)
{
    fresh();
    CHECK(wr(0x24, 4, 0x00070007)); CHECK(wr(0x28, 8, ASQ)); CHECK(wr(0x30, 8, ACQ));
    CHECK(wr(0x14, 4, 1));
    CHECK(rd(0x1c, 4) == 1);                         /* r2.1 N4: ready */
    aq = (struct q){ 8, 0, 0, 1, ASQ, ACQ, 0 };
    CHECK(submit(&aq, cmd(5, 0, IOCQ, (7u << 16) | 1, 3), true) == VNVME_SC_INVALID_FIELD); /* IOCQES unset */
    CHECK(wr(0x14, 4, S1_CC_VALID));                 /* set sizes while no I/O queue exists */
    CHECK(rd(0x14, 4) == S1_CC_VALID && rd(0x1c, 4) == 1);
    CHECK(submit(&aq, cmd(5, 0, IOCQ, (7u << 16) | 1, 3), true) == 0);
    CHECK(wr(0x14, 4, 1)); CHECK(rd(0x14, 4) == S1_CC_VALID); /* frozen once an I/O queue exists */
    CHECK(wr(0x14, 4, 1 | (5u << 16))); CHECK(rd(0x14, 4) == S1_CC_VALID);
}
/* 10. abrupt shutdown / reset with everything outstanding touches no guest memory */
static void t_abrupt_untouched(void)
{
    fresh(); enable(8); io_pair(8, 8);
    CHECK(submit(&aq, cmd(0x0c, 0, 0, 0, 0), true) == -1);
    struct vnvme_command r = cmd(2, 1, DATA, 0, 0); place(&ioq, r); place(&ioq, r); CHECK(wr(0x1008, 4, ioq.tail));
    for (unsigned p = 0; p < NPAGES; p++) { memset(arena + p * PAGE, CANARY, PAGE); perms[p] = 0; }
    unsigned g = grants;
    CHECK(wr(0x14, 4, S1_CC_VALID | 0x8000)); CHECK(rd(0x1c, 4) == 9);
    CHECK(grants == g && !line);
    CHECK(wr(0x14, 4, 0)); CHECK(grants == g);
}

/* 10b. plain reset (CC.EN 1->0, no SHN) with pending SQ entries: they never execute
 *      (property the rewritten handoff scenario no longer exercises directly). */
static void t_reset_pending_never_executes(void)
{
    fresh(); enable(8); io_pair(8, 8);
    struct vnvme_command r = cmd(2, 1, DATA, 0, 0); place(&ioq, r); place(&ioq, r); place(&ioq, r);
    CHECK(wr(0x1008, 4, ioq.tail));
    u64 reads = m.backend_reads; u64 comp = m.queue.stats.completions;
    CHECK(wr(0x14, 4, 0)); CHECK(rd(0x1c, 4) == 0 && !line);
    enable(8);
    CHECK(submit(&aq, cmd(6, 0, DATA + PAGE, 1, 0), true) == 0);
    CHECK(wr(0x100c, 4, 1));                       /* stale IO CQ ack: queue gone */
    CHECK(m.backend_reads == reads && m.queue.stats.completions == 1 && comp >= 1); /* stats reset by CC.EN=0 */
}

/* 11. randomized state-machine fuzz with page revocation between calls */
static u64 rs;
static u64 rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static u64 rnd_addr(void)
{
    switch (rnd() % 8) {
    case 0: return 0;
    case 1: return UINT64_MAX - (rnd() % 0x2000);
    case 2: return RAM + RAM_SIZE - (rnd() % 64) * 8;
    case 3: return RAM + (rnd() % NPAGES) * PAGE + (rnd() % 512) * 8;
    case 4: return rnd();
    default: return RAM + (rnd() % NPAGES) * PAGE;
    }
}
static void invariants(void)
{
    for (unsigned i = 0; i < VNVME_MAX_QUEUES; i++) {
        const struct vnvme_queue *q = &m.queue.queues[i];
        if (q->sq_valid) CHECK(q->sq_size && q->sq_head < q->sq_size && q->sq_tail < q->sq_size);
        if (q->cq_valid) CHECK(q->cq_size >= 2 && q->cq_tail < q->cq_size && q->cq_head < q->cq_size && q->cq_pending <= q->cq_size - 1);
    }
    if (m.queue.fatal && (m.regs.csts & 1)) CHECK(m.regs.csts & 2);
    CHECK(m.diag.magic == S1DG_MAGIC && m.diag.admin_first_count <= S1DG_ADMIN_FIRST && m.diag.mmio_first_count <= S1DG_MMIO_FIRST);
}
static void fuzz(u64 seed, unsigned ops)
{
    rs = seed; fresh(); enable(8); io_pair(8, 8);
    u16 sqt[2] = { aq.tail, ioq.tail };
    for (unsigned i = 0; i < ops; i++) {
        u64 r = rnd();
        switch (r % 16) {
        case 0: perms[rnd() % 24] ^= 3; break;                         /* revoke/restore low pages */
        case 1: { static const u32 v[] = { 0, S1_CC_VALID, S1_CC_VALID | 0x4000, S1_CC_VALID | 0x8000,
                                           S1_CC_VALID | 0xc000, 1, S1_CC_VALID | 0x80 };
                  wr(0x14, 4, v[rnd() % 7]); break; }
        case 2: wr(0x24, 4, (u32)rnd()); wr(0x28, 8, rnd() % 4 ? ASQ : rnd_addr()); wr(0x30, 8, rnd() % 4 ? ACQ : rnd_addr()); break;
        case 3: wr(0x1000 + (rnd() % 6) * 4, 4, rnd() % 4 ? rnd() % 300 : (u32)rnd()); break;
        case 4: wr((u32)(rnd() % 0x1100), (unsigned[]){ 1, 2, 4, 8 }[rnd() % 4], rnd()); break;
        case 5: cfgw(4, 2, (u32[]){ S1_MEM | S1_BME, S1_MEM, S1_MEM | S1_BME | S1_INTX_DISABLE }[rnd() % 3]); break;
        default: {                                                   /* place a command and ring */
            unsigned qi = rnd() % 2;
            struct vnvme_queue *q = &m.queue.queues[qi];
            if (!q->sq_valid || (rnd() % 64) == 0) {                  /* harness re-init */
                memset(perms, 3, sizeof perms); CHECK(cfgw(4, 2, S1_MEM | S1_BME));
                wr(0x14, 4, 0); enable(8); io_pair(8, 8); sqt[0] = aq.tail; sqt[1] = ioq.tail; break;
            }
            struct vnvme_command c; memset(&c, 0, sizeof c);
            static const u8 adm[] = { 0, 1, 2, 4, 5, 6, 8, 9, 10, 0x0c, 0x80 };
            static const u8 io[] = { 0, 1, 2, 2, 2, 8, 9, 0x80 };
            c.opcode = qi ? io[rnd() % sizeof io] : adm[rnd() % sizeof adm];
            c.nsid = (u32[]){ 0, 1, 1, 0xffffffffu, (u32)rnd() }[rnd() % 5];
            c.cid = (u16)rnd(); c.prp1 = rnd_addr(); c.prp2 = rnd() % 2 ? LIST : rnd_addr();
            c.cdw10 = rnd() % 2 ? (u32)(rnd() % 64) : (u32)rnd(); c.cdw11 = rnd() % 2 ? (u32)(rnd() % 8) : (u32)rnd();
            c.cdw12 = rnd() % 2 ? (u32)(rnd() % 32) | (rnd() % 2 ? 1u << 30 : 0) : (u32)rnd(); c.cdw13 = rnd() % 4 ? 0 : (u32)rnd();
            if (rnd() % 8 == 0) c.metadata = rnd();
            if (rnd() % 3 == 0) { u64 *l = (u64 *)ptr(LIST); for (unsigned k = 0; k < 32; k++) l[k] = rnd_addr(); }
            u64 sq = q->sq_addr; u16 n = q->sq_size;
            if (sq >= RAM && sq - RAM + (u64)n * 64 <= RAM_SIZE) {
                u16 t = q->sq_tail; memcpy(ptr(sq + (u64)t * 64), &c, 64);
                wr(0x1000 + qi * 8, 4, (u16)((t + 1) % n));
                sqt[qi] = (u16)((t + 1) % n);
            }
            if (rnd() % 2) { struct vnvme_queue *cq = &m.queue.queues[qi ? q->cq_id % 2 : 0];
                             if (cq->cq_valid) wr(0x1004 + (qi ? 8 : 0), 4, cq->cq_tail); }
        }
        }
        invariants();
    }
}

int main(int argc, char **argv)
{
    unsigned ops = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 200000;
    t_shutdown_refused_sqe(); t_shutdown_refused_cq_then_admin(); t_mass_abort_cq_full();
    t_abort_aer_refused_cq(); t_prp_lists(); t_cfs_intx_stuck(); t_intms_reset();
    t_shn_clear_without_reset(); t_cc_iosqes_zero(); t_abrupt_untouched(); t_reset_pending_never_executes();
    for (u64 s = 1; s <= 8; s++) fuzz(0x9e3779b97f4a7c15ull * s, ops);
    CHECK(s0_fixture_guards_valid());
    printf("{\"status\":\"pass\",\"checks\":%u,\"model_calls\":%u,\"grants\":%u,\"refusals\":%u}\n",
           checks, calls, grants, refusals);
    return 0;
}
