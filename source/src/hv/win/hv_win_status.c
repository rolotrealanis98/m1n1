/* SPDX-License-Identifier: MIT */
/* r4.11 diagnostics: periodic NVMe/backend status lines and the per-tick storage event watch. Firmware
 * only (not linked into the host storage tests); read-only views of the S1 model and native backend. */
#include "hv_win_storage.h"
#include "s1/model.h"
#include "utils.h"
#include "s1/hv_nvme_queue.h"
#include "hv_win_storage_mem.h"

/* r4.13: completions with a non-zero status, by status (first 8 distinct), and the first 50 logged */
static u32 err_status[8];
static u64 err_count[8], err_other, err_logged;
static const char *const sm_why_name[HV_SM_WHY_NR] = {
    "none", "lease-state", "context", "args/span", "not-guest-RAM", "resident", "protected",
    "s2-walk", "s2-attr", "pmu-scanning"};

/* r4.14: names of the PRP planner rules (enum vnvme_prp_rule) */
static const char *const prp_rule_name[VNVME_PRP_RULES] = {
    "none", "size/prp1", "prp2", "prp2-align", "list-ptr", "list-admission", "entry-align", "count",
    "data-admission", "data-over-queue", "data-over-list", "data-overlap"};
static const char *prp_rule(u8 r)
{
    return r < VNVME_PRP_RULES ? prp_rule_name[r] : "?";
}

static void err_note(u32 status)
{
    for (int i = 0; i < 8; i++) {
        if (err_count[i] && err_status[i] == status) { err_count[i]++; return; }
        if (!err_count[i]) { err_status[i] = status; err_count[i] = 1; return; }
    }
    err_other++;
}

static const struct s1_model *model(void)
{
    bool a, r;
    return hv_win_storage_model(&a, &r);
}

static const struct s1dg_entry *diag_last(u8 type)
{
    const struct s1_diag *g = &model()->diag;
    u64 seq = g->write_seq;
    for (u32 k = 0; k < 64 && k < seq && k < S1DG_RING; k++) {
        const struct s1dg_entry *e = &g->ring[(seq - 1 - k) % S1DG_RING];
        if (e->type == type)
            return e;
    }
    return NULL;
}

void hv_win_storage_status_lines(const char *prefix)
{
    bool attempted, ready;
    const struct s1_model *m = hv_win_storage_model(&attempted, &ready);
    if (!attempted) {
        printf("%s nvme: not prepared\n", prefix);
        return;
    }
    const struct vnvme_ctrl *q = &m->queue;
    const struct s1_diag *g = &m->diag;
    printf("%s nvme: ready %d cc 0x%x csts 0x%x intms 0x%x irq %d edges %lu chg %lu | sqdb %lu cqdb %lu cmds %lu "
           "cpl %lu pend %ld | admin", prefix, ready, m->regs.cc, m->regs.csts, m->regs.intms, q->irq_asserted,
           (unsigned long)g->irq_edges, (unsigned long)m->irq_changes, (unsigned long)q->stats.sq_doorbells,
           (unsigned long)q->stats.cq_doorbells, (unsigned long)q->stats.commands,
           (unsigned long)q->stats.completions, (long)(q->stats.commands - q->stats.completions));
    for (u32 i = 0; i < 32; i++)
        if (g->admin_opcode[i])
            printf(" %02x:%lu", i, (unsigned long)g->admin_opcode[i]);
    printf(" | intms-w %lu intmc-w %lu cc-w %lu | io flush %lu write %lu read %lu other %lu | err %lu last q%lu c%lu s0x%lx | fatal %lu(%lu) "
           "refused %lu invdb %lu abrt %lu disc %lu\n",
           (unsigned long)g->intms_writes, (unsigned long)g->intmc_writes, (unsigned long)g->cc_writes,
           (unsigned long)g->io_opcode[0], (unsigned long)g->io_opcode[1], (unsigned long)g->io_opcode[2],
           (unsigned long)g->io_other, (unsigned long)g->nonzero_status, (unsigned long)g->last_err_qid,
           (unsigned long)g->last_err_cid, (unsigned long)g->last_err_status, (unsigned long)g->fatal_count,
           (unsigned long)g->first_fatal_reason,
           (unsigned long)(g->refusals[0] + g->refusals[1] + g->refusals[2] + g->refusals[3] + g->refusals[4] +
                           g->refusals[5] + g->refusals[6] + g->refusals[7]),
           (unsigned long)q->invalid_doorbells, (unsigned long)q->aborted_commands,
           (unsigned long)q->discarded_commands);
    /* r4.13: error completions by status and guest-buffer admission refusals by reason */
    printf("%s nvme: errors by status", prefix);
    for (int i = 0; i < 8 && err_count[i]; i++)
        printf(" 0x%x:%lu", err_status[i], (unsigned long)err_count[i]);
    printf(" other %lu | admission refusals", (unsigned long)err_other);
    for (int i = 1; i < HV_SM_WHY_NR; i++)
        if (hv_sm_last_refusal.count[i])
            printf(" %s:%lu", sm_why_name[i], (unsigned long)hv_sm_last_refusal.count[i]);
    printf(" | out-of-range reads %lu | prp refusals", (unsigned long)q->refused_reads);
    for (int i = 0; i < VNVME_PRP_RULES; i++)
        if (q->prp_refusals[i])
            printf(" %s:%lu", prp_rule((u8)i), (unsigned long)q->prp_refusals[i]);
    printf(" | dup-dest reads %lu (last prp1 0x%lx) partial %lu (last prp1 0x%lx)\n",
           (unsigned long)q->prp_dup_dest_reads, (unsigned long)q->prp_dup_dest_last,
           (unsigned long)q->prp_part_dest_reads, (unsigned long)q->prp_part_dest_last);
    const struct s1dg_entry *sq = diag_last(S1DG_SQE), *cq = diag_last(S1DG_CQE);
    printf("%s nvme: q1 sq h%u t%u cq h%u t%u pend %u ien %d", prefix, q->queues[1].sq_head,
           q->queues[1].sq_tail, q->queues[1].cq_head, q->queues[1].cq_tail, q->queues[1].cq_pending,
           q->queues[1].irq_enabled);
    if (sq)
        printf(" | last SQE q%u op 0x%02x cid %u lba %lu nlb %u", sq->qid, sq->opcode, sq->cid,
               (unsigned long)sq->a, (sq->d & 0xffff) + 1);
    if (cq)
        printf(" | last CQE q%u cid %u st 0x%x", cq->qid, cq->cid, cq->status);
    /* r4.12: why the INTx line is asserted: each CQ with IEN holding unacknowledged entries, the
     * guest-written CQ head vs the model's tail/phase, and the CQE at the guest's head as the guest
     * sees it in its memory (cid, status, phase tag) */
    printf(" | line ext %d intms %u", m->external_irq, m->regs.intms & 1);
    for (u32 i = 0; i < VNVME_MAX_QUEUES; i++) {
        const struct vnvme_queue *cqq = &q->queues[i];
        if (!cqq->cq_valid || !cqq->irq_enabled || !cqq->cq_pending)
            continue;
        printf(" ASSERT-BY cq%u pend %u head %u tail %u phase %u", i, cqq->cq_pending, cqq->cq_head,
               cqq->cq_tail, cqq->cq_phase);
        u64 a = cqq->cq_addr + (u64)cqq->cq_head * sizeof(struct vnvme_completion);
        if (hv_win_storage_guest_ram_contains(a, sizeof(struct vnvme_completion))) {
            const volatile struct vnvme_completion *e = (const volatile struct vnvme_completion *)a;
            printf(" CQE@head cid %u st 0x%x p %u", e->cid, (e->status >> 1) & 0x7fff, e->status & 1);
        }
    }
    u64 st2 = hv_win_storage_native_stat(2);
    printf(" | ans state %lu reason %lu cmds %lu sub %lu done %lu rerr %lu werr %lu wref %lu tmo 0x%lx abrt %lu "
           "blkw %lu link_ref %lu\n",
           (unsigned long)(st2 >> 32), (unsigned long)(st2 & 0xffffffff),
           (unsigned long)hv_win_storage_native_stat(7), (unsigned long)hv_win_storage_native_stat(37),
           (unsigned long)hv_win_storage_native_stat(38), (unsigned long)hv_win_storage_native_stat(8),
           (unsigned long)hv_win_storage_native_stat(20), (unsigned long)hv_win_storage_native_stat(19),
           (unsigned long)hv_win_storage_native_stat(23), (unsigned long)hv_win_storage_native_stat(43),
           (unsigned long)hv_win_storage_native_stat(39), (unsigned long)hv_win_storage_native_stat(30));
}

/* Called every hypervisor tick: any new controller fatal, model refusal, backend error/refusal or
 * ANS state change is printed at once (not only in the 5 s report). Returns true if it printed. */
bool hv_win_storage_watch(void)
{
    static u64 last_fatal, last_ref, last_werr, last_rerr, last_wref, last_st2, last_err;
    static bool primed;
    bool printed = false, attempted, ready;
    hv_win_storage_model(&attempted, &ready);
    if (!attempted)
        return false;
    const struct s1_diag *g = &model()->diag;
    u64 ref = 0;
    for (int i = 0; i < 8; i++)
        ref += g->refusals[i];
    u64 st2 = hv_win_storage_native_stat(2), werr = hv_win_storage_native_stat(20),
        rerr = hv_win_storage_native_stat(8), wref = hv_win_storage_native_stat(19);
    if (primed && (g->fatal_count != last_fatal || ref != last_ref || werr != last_werr ||
                   rerr != last_rerr || wref != last_wref || st2 != last_st2 || g->nonzero_status != last_err)) {
        printf("HV/win r4.14 STORAGE EVENT: ctrl fatal %lu (first reason %lu) refusals %lu nvme-err %lu "
               "(last q%lu c%lu st 0x%lx) | ans state %lu reason %lu rerr %lu werr %lu window-refusals %lu\n",
               (unsigned long)g->fatal_count, (unsigned long)g->first_fatal_reason, (unsigned long)ref,
               (unsigned long)g->nonzero_status, (unsigned long)g->last_err_qid, (unsigned long)g->last_err_cid,
               (unsigned long)g->last_err_status, (unsigned long)(st2 >> 32), (unsigned long)(st2 & 0xffffffff),
               (unsigned long)rerr, (unsigned long)werr, (unsigned long)wref);
        printed = true; /* the caller flushes the console */
    }
    primed = true;
    last_fatal = g->fatal_count; last_ref = ref; last_werr = werr; last_rerr = rerr; last_wref = wref;
    last_st2 = st2; last_err = g->nonzero_status;

    /* r4.12: one line per CC write, controller reset/enable, admin queue create/delete (and its
     * completion) and the first 32 INTMS / INTMC writes, with the event's CNTVCT stamp; read from the
     * S1DG ring since the last tick (counted when the ring wrapped past unread events). */
    static u64 seen;
    static u32 intms_logged, intmc_logged;
    static u16 qcid[8];
    static u64 last_ref_addr, last_ref_len, last_ref_reason, last_ref_seq = ~0ULL;
    static u8 nq;
    u64 seq = g->write_seq;
    if (seen < seq && seq - seen > S1DG_RING) {
        printf("HV/win r4.14 NVME-EV: %lu events not shown (ring wrapped)\n", (unsigned long)(seq - seen - S1DG_RING));
        seen = seq - S1DG_RING;
        printed = true;
    }
    for (; seen < seq; seen++) {
        const struct s1dg_entry *e = &g->ring[seen % S1DG_RING];
        const char *what = NULL;
        if (e->type == S1DG_MMIO_W && e->a == 0x14)
            what = "CC write";
        else if (e->type == S1DG_MMIO_W && e->a == 0x0c && intms_logged < 32)
            what = "INTMS write", intms_logged++;
        else if (e->type == S1DG_MMIO_W && e->a == 0x10 && intmc_logged < 32)
            what = "INTMC write", intmc_logged++;
        if (what) {
            printf("HV/win r4.14 NVME-EV t 0x%08x %s 0x%lx ok %u csts 0x%x\n", e->t, what, (unsigned long)e->b,
                   e->status, e->c);
            printed = true;
            continue;
        }
        if (e->type == S1DG_RESET || e->type == S1DG_ENABLE) {
            printf("HV/win r4.14 NVME-EV t 0x%08x %s cc 0x%lx -> 0x%lx\n", e->t,
                   e->type == S1DG_RESET ? "controller RESET (CC.EN=0)" : "controller ENABLE", (unsigned long)e->a,
                   (unsigned long)e->b);
            printed = true;
            continue;
        }
        if (e->type == S1DG_SQE && e->qid == 0 &&
            (e->opcode == 0x00 || e->opcode == 0x01 || e->opcode == 0x04 || e->opcode == 0x05)) {
            static const char *const qn[6] = {"DELETE SQ", "CREATE SQ", "?", "?", "DELETE CQ", "CREATE CQ"};
            printf("HV/win r4.14 NVME-EV t 0x%08x %s cid %u cdw10 0x%08x cdw11 0x%08x prp1 0x%lx\n", e->t,
                   qn[e->opcode], e->cid, (u32)e->a, (u32)(e->a >> 32), (unsigned long)e->b);
            qcid[nq++ & 7] = e->cid;
            printed = true;
            continue;
        }
        if (e->type == S1DG_REFUSAL) {
            last_ref_addr = e->a;
            last_ref_len = e->b;
            last_ref_reason = e->c;
            last_ref_seq = seen;
        }
        if (e->type == S1DG_CQE && e->status) {
            err_note(e->status);
            if (err_logged++ < 50) {
                /* the command: the latest SQE for this queue/cid still in the ring */
                const struct s1dg_entry *sq = NULL;
                for (u64 k = seen; k-- > 0 && seen - k < S1DG_RING;) {
                    const struct s1dg_entry *x = &g->ring[k % S1DG_RING];
                    if (x->type == S1DG_SQE && x->qid == e->qid && x->cid == e->cid) { sq = x; break; }
                }
                printf("HV/win r4.14 NVME-ERR #%lu t 0x%08x q%u cid %u status 0x%x", (unsigned long)err_logged, e->t,
                       e->qid, e->cid, e->status);
                if (sq)
                    printf(" op 0x%02x slba %lu nlb %u prp1 0x%lx nsid %u", sq->opcode, (unsigned long)sq->a,
                           (sq->d & 0xffff) + 1, (unsigned long)sq->b, sq->c);
                if (last_ref_seq != ~0ULL && seen - last_ref_seq < 64)
                    printf(" | model refused %s span 0x%lx+0x%lx (reason %lu)", (last_ref_len >> 63) ? "write-to-guest" : "read-from-guest",
                           (unsigned long)last_ref_addr, (unsigned long)(last_ref_len & ~(1ULL << 63)),
                           (unsigned long)last_ref_reason);
                const struct hv_sm_refusal *r = &hv_sm_last_refusal;
                if (r->why)
                    printf(" | admission (latest) %s ipa 0x%lx len 0x%lx detail 0x%lx",
                           r->why < HV_SM_WHY_NR ? sm_why_name[r->why] : "?", (unsigned long)r->ipa,
                           (unsigned long)r->length, (unsigned long)r->detail);
                /* r4.14: the PRP planner rule that refused this command, if it was one */
                if (sq && e->status == VNVME_SC_DATA_TRANSFER_ERROR) {
                    const struct vnvme_ctrl *qc = &model()->queue;
                    for (u32 k = 0; k < VNVME_PRP_LOG && k < qc->prp_refusal_total; k++) {
                        const struct vnvme_prp_refusal *r =
                            &qc->prp_log[(qc->prp_refusal_total - 1 - k) % VNVME_PRP_LOG];
                        if (r->cid == e->cid && r->opcode == sq->opcode) {
                            printf(" | prp rule %s seg %u addr 0x%lx len 0x%lx other 0x%lx", prp_rule(r->rule),
                                   r->index, (unsigned long)r->addr, (unsigned long)r->len,
                                   (unsigned long)r->other);
                            break;
                        }
                    }
                }
                printf("\n");
                printed = true;
                last_ref_seq = ~0ULL;
            }
        }
        if (e->type == S1DG_CQE && e->qid == 0) {
            for (u32 k = 0; k < 8 && k < nq; k++) {
                if (qcid[k] == e->cid) {
                    printf("HV/win r4.14 NVME-EV t 0x%08x   -> queue command cid %u status 0x%x\n", e->t, e->cid,
                           e->status);
                    qcid[k] = 0xffff;
                    printed = true;
                    break;
                }
            }
        }
    }
    /* r4.14: one line per PRP planner refusal (rule, segment, range), rate limited: the first 64,
     * then every 256th; refusals that left the 16-entry log before this tick are counted. */
    {
        static u64 prp_seen;
        const struct vnvme_ctrl *qc = &model()->queue;
        u64 total = qc->prp_refusal_total;
        if (total - prp_seen > VNVME_PRP_LOG) {
            printf("HV/win r4.14 PRP-REFUSE: %lu refusals left the log unshown\n",
                   (unsigned long)(total - prp_seen - VNVME_PRP_LOG));
            prp_seen = total - VNVME_PRP_LOG;
            printed = true;
        }
        for (; prp_seen < total; prp_seen++) {
            const struct vnvme_prp_refusal *r = &qc->prp_log[prp_seen % VNVME_PRP_LOG];
            if (r->ordinal > 64 && r->ordinal % 256)
                continue;
            printf("HV/win r4.14 PRP-REFUSE #%lu cmd %lu cid %u op 0x%02x %s rule %s seg %u addr 0x%lx len 0x%lx "
                   "other 0x%lx\n",
                   (unsigned long)r->ordinal, (unsigned long)r->command, r->cid, r->opcode,
                   r->to_guest ? "to-guest" : "from-guest", prp_rule(r->rule), r->index, (unsigned long)r->addr,
                   (unsigned long)r->len, (unsigned long)r->other);
            printed = true;
        }
    }
    return printed;
}
