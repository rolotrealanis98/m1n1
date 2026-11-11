/* SPDX-License-Identifier: MIT */
/* r3 native read-only ANS driver (T6050). See ans_ro.h for the contract.
 *
 * Derivation: the controller programming below is the frozen m1n1 src/nvme.c sequence
 * (sha256 a6c7d75b...), which stage 1 executes on every boot of this machine and which the
 * 2026-11-09 resident-proxy probe reproduced (nvme_init 0.30 s, LBA 0/1 matched Linux,
 * nvme_shutdown 0.59 s incl. "pmgr: resetting device 0.ANS", no SError). The same single
 * base is used (ADT reg[9] = 0x45dcc0000; the probe showed reg[9]+0x28100.. reads back the
 * NVMMU registers identically to reg[3]). Differences from nvme.c, all on the host side:
 *   - every wait is bounded (RTKit power waits via rtkit_set_power_timeout);
 *   - any uncertain-DMA failure freezes: no further MMIO, no sleep/stop/PMGR, nothing freed;
 *   - a single submission path enforces the allowlist below;
 *   - queues, TCBs and the DMA bounce are static (resident BSS, excluded from guest RAM);
 *   - reads go only to NSID 1, one block per command, through the private bounce;
 *   - r4.3: every buffer ANS DMAs to/from is its own 16 KiB-aligned region. The six queue/TCB
 *     arrays have exactly nvme.c's sizes (nvme.c:145,149,153 memalign(SZ_16K, 64 * entry)); the
 *     1 MiB transfer buffer, its PRP list page and the bounce each start a 16 KiB slot. Data
 *     PRPs are 4 KiB pages inside the transfer buffer (nvme.c:568-570 requires 4 KiB). A
 *     pre-doorbell guard (dma_guard) refuses - by freezing, before any doorbell - any command
 *     whose SQE/TCB does not describe exactly those pages. NOTE: nvme.c only ever issues
 *     one-block reads with PRP2 = 0; r4's 2-block (PRP2 = page 2) and >2-block (PRP2 = list)
 *     layouts have no nvme.c counterpart and follow NVMe PRP rules as Linux apple.c:719-800,
 *     819-829 builds them.
 *   - r4.4 = r4.3 + r3.5 (ANS-T6050-CROSSCHECK-20261110 items 1-3: 10 s RTKit HELLO/endpoint-map
 *     and IOP power-on waits, which-wait recorded, TCB status freeze on reg[3]+0x28120 only)
 *     + the cross-check's write-path items 5-7: I/O commands use tag 2 (admin tag 0), as the MIT Mu
 *     J714AnsDxe core does (AppleNvmeControllerCore.c, ntasi_ans_controller_execute: "Linear ANS
 *     uses one NVMMU tag space for both queues ... those slots must still be distinct"); FLUSH,
 *     never FUA (already so in r4.3; Mu AppleNvmeCore.c ntasi_ans_cmd_flush/ntasi_ans_tcb_fill);
 *     after a successful Delete SQ + Delete CQ, +0x1200 then +0x1208 are zeroed lo/hi before the
 *     shutdown (behaviour of aura nvme.c nvme_shutdown, T6050 path; own code). */
#include "ans_ro.h"
#include "native_window.h"
#include "native_link.h"
#include "adt.h"
#include "asc.h"
#include "pmgr.h"
#include "rtkit.h"
#include "sart.h"
#include "string.h"
#include "utils.h"
#include "memory.h" /* r4: dc_*_range cache maintenance (ANS has no dma-coherent) */

#define T6050_ANS_REG0  0x419600000UL
#define T6050_ANS_REG9  0x45dcc0000UL
#define T6050_ANS_REG3  0x41dcc0000UL /* NVMMU bank; only the TCB status register (0x28120) is read here */
#define T6050_SART_BASE 0x41dc50000UL
/* r3.1: pinned controller identity (J716s internal SSD, 2026-11-09 Linux + probe) */
#define T6050_NSZE  244276265ULL
#define T6050_MODEL "APPLE SSD AP1024Z"
#define T6050_FW    "241.60.3"
/* r4.4 (= r3.5): 10 s, as this tree's pcie_t6050.c:768 (rtkit_set_boot_timeout(10 * USEC_PER_SEC),
 * "ANS can take ~3 s to answer after a stop") and the J716s Linux series patch 0045 (RTKit handshake
 * wait 1 s -> 10 s: "sometimes needs about 3 s to answer the power-state and endpoint-map
 * handshake"). r4.3 had 5 s. Bounds the HELLO and every endpoint-map wait. */
#define RTKIT_HELLO_TIMEOUT 10000000

#define NVME_TIMEOUT          1000000
/* r4: bounded but longer waits for data commands (ANS FLUSH is known to be slow) */
#define NVME_WRITE_TIMEOUT   30000000
#define NVME_FLUSH_TIMEOUT   30000000
#define NVME_ENABLE_TIMEOUT   5000000
#define NVME_SHUTDOWN_TIMEOUT 5000000
/* r4.4 (= r3.5): the boot-time IOP power-ON wait is 10 s too (r4.3: 1 s). The quiesce's power
 * waits (rtkit_sleep) keep the 1 s bound. */
#define RTKIT_BOOT_POWER_TIMEOUT 10000000
#define RTKIT_POWER_TIMEOUT   1000000
#define QD                    64

#define NVME_CC            0x14
#define NVME_CC_SHN        GENMASK(15, 14)
#define NVME_CC_SHN_NORMAL 1
#define NVME_CC_EN         BIT(0)
#define NVME_CSTS             0x1c
#define NVME_CSTS_CFS         BIT(1)
#define NVME_CSTS_SHST        GENMASK(3, 2)
#define NVME_CSTS_SHST_DONE   2
#define NVME_CSTS_RDY         BIT(0)
#define NVME_AQA 0x24
#define NVME_ASQ 0x28
#define NVME_ACQ 0x30
#define NVME_DB_ACQ  0x1004
#define NVME_DB_IOCQ 0x100c
#define NVME_BOOT_STATUS    0x1300
#define NVME_BOOT_STATUS_OK 0xde71ce55
#define NVME_LINEAR_SQ_CTRL    0x24908
#define NVME_LINEAR_SQ_CTRL_EN BIT(0)
#define NVME_IOQ_CMDS           0x1200
#define NVME_IOQ_CQES           0x1208
#define NVME_MAX_PEND_CMDS_CTRL 0x1210
#define NVME_DB_LINEAR_ASQ      0x2490c
#define NVME_DB_LINEAR_IOSQ     0x24910
#define NVMMU_NUM       0x28100
#define NVMMU_ASQ_BASE  0x28108
#define NVMMU_IOSQ_BASE 0x28110
#define NVMMU_TCB_INVAL 0x28118
#define NVMMU_TCB_STAT  0x29120 /* frozen nvme.c offset (via reg[9]); r4.4: diagnostic only */
#define NVMMU_TCB_STAT_LINUX 0x28120 /* aura nvme.c / Linux apple.c offset (via reg[3]); freezes */

#define OP_DELETE_SQ 0x00
#define OP_CREATE_SQ 0x01
#define OP_READ      0x02
/* r4 I/O opcodes, allowed only after GPT verification with the explicit rw contract flag */
#define OP_FLUSH     0x00
#define OP_WRITE     0x01
#define OP_DELETE_CQ 0x04
#define OP_CREATE_CQ 0x05
#define OP_IDENTIFY  0x06 /* r4.3: NOT in the allowlist; kept only to document the opcode */
#define QUEUE_CONTIGUOUS BIT(0)
/* r4.4: NVMMU tags. Ported from the MIT Mu core (aurora-silicon/mu@8bac64d
 * Platform/MacBookProM5Pkg/Drivers/J714AnsDxe/Shared/AppleNvmeControllerCore.c,
 * ntasi_ans_controller_execute + ntasi_ans_controller_submit, SPDX MIT): admin commands use tag 0,
 * I/O commands use tag = the admin queue depth Linux/Mu reserve (2; Linux apple.c
 * APPLE_NVME_AQ_DEPTH / reserved_tags), so an I/O TCB never shares an NVMMU tag (and TCB_INVAL)
 * with an admin TCB. This driver keeps AQA 63/63 (aura, m1n1, probe); only tag 0 is ever used on
 * the admin queue, so tag 2 is distinct from every admin tag in use. SQE slot = TCB slot = tag. */
#define ANS_ADMIN_TAG 0
#define ANS_IO_TAG    2
/* r4.3 DMA layout constants. nvme.c allocates every queue array with memalign(SZ_16K, ...)
 * (nvme.c:145, 149, 153); the NVMe memory page (CC.MPS = 0) is 4 KiB (nvme.c:568 comment). */
#define ANS_DMA_ALIGN  0x4000UL
#define ANS_NVME_PAGE  0x1000UL

struct ans_cmd {
    u8 opcode, flags, tag, rsvd;
    u32 nsid, cdw2, cdw3;
    u64 metadata, prp1, prp2;
    u32 cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
};
struct ans_cpl { u64 result; u32 rsvd; u16 tag; u16 status; };
struct ans_tcb {
    u8 opcode, dma_flags, slot_id, unk0;
    u32 len;
    u64 unk1[2];
    u64 prp1, prp2;
    u64 unk2[2];
    u8 aes_iv[8];
    u8 aes_unk[64];
};
_Static_assert(sizeof(struct ans_cmd) == 64, "ans cmd");
_Static_assert(sizeof(struct ans_cpl) == 16, "ans cpl");
_Static_assert(sizeof(struct ans_tcb) == 128, "ans tcb");

struct ans_queue {
    struct ans_tcb *tcbs;
    struct ans_cmd *cmds;
    struct ans_cpl *cqes;
    u8 head, phase;
    bool admin;
};

/* Private DMA memory: resident BSS, page aligned, never in guest RAM.
 * r4: plus a 1 MiB transfer buffer and the private PRP list that describes it; every ANS
 * data PRP points into xfer, nowhere else. */
/* r4.3: one 16 KiB-aligned region per buffer. r4.2 packed the queue arrays after the transfer
 * buffer, putting icpl at +0x107400 and the bounce at +0x107800 (not page aligned; same defect as
 * r3.3, INDEPENDENT-REVIEW F1). Queue arrays: nvme.c alloc_queue() sizes (nvme.c:145 tcbs 64x128,
 * :149 cmds 64x64, :153 cqes 64x16), each memalign(SZ_16K). */
#define ANS_DMA_BUF __attribute__((aligned(ANS_DMA_ALIGN)))
static struct ans_dma {
    u8 xfer[ANS_XFER_BYTES] ANS_DMA_BUF;                 /* data buffer (4 KiB data pages inside) */
    u64 prp_list[ANS_RO_BLOCK / sizeof(u64)] ANS_DMA_BUF; /* PRP list page for > 2-block commands */
    struct ans_tcb atcb[QD] ANS_DMA_BUF; /* admin NVMMU TCBs   (nvme.c:145, adminq) */
    struct ans_tcb itcb[QD] ANS_DMA_BUF; /* I/O NVMMU TCBs     (nvme.c:145, ioq) */
    struct ans_cmd acmd[QD] ANS_DMA_BUF; /* admin SQ           (nvme.c:149, adminq) */
    struct ans_cmd icmd[QD] ANS_DMA_BUF; /* I/O SQ             (nvme.c:149, ioq) */
    struct ans_cpl acpl[QD] ANS_DMA_BUF; /* admin CQ           (nvme.c:153, adminq) */
    struct ans_cpl icpl[QD] ANS_DMA_BUF; /* I/O CQ             (nvme.c:153, ioq) */
    u8 bounce[ANS_RO_BLOCK] ANS_DMA_BUF; /* legacy bounce (scrubbed; no ANS command targets it) */
} dma ANS_DMA_BUF;
#define ANS_DMA_OFF_OK(m) (__builtin_offsetof(struct ans_dma, m) % ANS_DMA_ALIGN == 0)
_Static_assert(ANS_DMA_OFF_OK(xfer) && ANS_DMA_OFF_OK(prp_list) && ANS_DMA_OFF_OK(atcb) && ANS_DMA_OFF_OK(itcb) &&
               ANS_DMA_OFF_OK(acmd) && ANS_DMA_OFF_OK(icmd) && ANS_DMA_OFF_OK(acpl) && ANS_DMA_OFF_OK(icpl) &&
               ANS_DMA_OFF_OK(bounce), "every ANS DMA buffer starts on its own 16 KiB boundary");
_Static_assert(sizeof(dma.atcb) == 8192 && sizeof(dma.acmd) == 4096 && sizeof(dma.acpl) == 1024 &&
               sizeof(dma.bounce) == ANS_NVME_PAGE && sizeof(dma.prp_list) == ANS_NVME_PAGE,
               "queue sizes are nvme.c's (QD = NVME_QUEUE_SIZE = 64)");
_Static_assert(sizeof(dma) == ANS_XFER_BYTES + 8 * ANS_DMA_ALIGN, "xfer + eight 16 KiB slots, nothing else");
_Static_assert(sizeof(dma.acpl) <= ANS_NVME_PAGE && sizeof(dma.acmd) <= ANS_NVME_PAGE, "one page per queue");
_Static_assert(ANS_XFER_BYTES / ANS_RO_BLOCK - 1 <= ANS_NVME_PAGE / sizeof(u64), "one PRP list page suffices");

static _Atomic enum ans_ro_state state;
static bool session_frozen;
static struct ans_ro_stats st;
static u64 base, nvmmu;
static asc_dev_t *asc;
static sart_dev_t *sart;
static rtkit_dev_t *rtk;
static struct ans_queue adminq, ioq;
static u8 die;
/* r4: window and mode, set once after GPT verification (ans_ro_set_window). */
static bool window_set, rw_enabled;

static u32 max_xfer = 1;
static u64 sart_regs;
static u8 next_io_tag = ANS_IO_TAG;
static bool gpt_read_active;
static bool exec_busy, recheck_active;
#if S1N_STAGE_WRITES
static bool first_write_checked;
#endif
#if S1N_STAGE_CANARY
static bool canary_running, canary_tried;
static u8 canary_saved[S1N_CANARY_CHUNK * ANS_RO_BLOCK];
static u8 canary_pattern[S1N_CANARY_CHUNK * ANS_RO_BLOCK];
#endif
#if S1N_STAGE_WRITES
static u8 recheck_saved[ANS_XFER_BYTES];
#endif
const char s1n_stage_marker[] = S1N_STAGE_MARKER;


static void set_state(enum ans_ro_state s, enum ans_ro_reason r)
{
    if (__atomic_load_n(&session_frozen, __ATOMIC_ACQUIRE)) s = ANS_RO_FROZEN;
    state = s;
    st.state = s;
    u32 no_reason = 0;
    if (r) __atomic_compare_exchange_n(&st.reason, &no_reason, r, false,
                                       __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}
static void freeze(enum ans_ro_reason r)
{
    /* No MMIO, no RTKit sleep, no CPU stop, no PMGR, no free: platform reset recovers. */
    __atomic_store_n(&session_frozen, true, __ATOMIC_RELEASE);
    set_state(ANS_RO_FROZEN, r);
    printf("ans_ro: FROZEN (reason %u); no further ANS access until platform reset\n", r);
}

/* Drains the mailbox; returns false (and freezes) if RTKit reports a crash. */
static bool drain(void)
{
    struct rtkit_message msg;
    for (unsigned i = 0; i < 64; i++) {
        if (!rtkit_can_recv(rtk))
            return true;
        int ret = rtkit_recv(rtk, &msg);
        if (ret < 0) {
            freeze(ANS_RO_E_CRASH);
            return false;
        }
        st.mailbox_msgs++;
        if (ret == 0)
            return true;
    }
    return true;
}

static bool allowed(bool admin, u8 opcode)
{
    if (admin)
        return opcode == OP_CREATE_CQ || opcode == OP_CREATE_SQ || /* r4.3: Identify dropped */
               opcode == OP_DELETE_SQ || opcode == OP_DELETE_CQ;
    return opcode == OP_READ || (rw_enabled && (opcode == OP_FLUSH
#if S1N_STAGE_WRITES
           || opcode == OP_WRITE
#endif
           ));
}

/* r4: second, independent check of the exact SQE and TCB about to be submitted, built from
 * the queue memory itself immediately before the doorbell. A data command must address only
 * the verified window (before verification: reads only), carry NSID 1, and DMA only inside
 * the private transfer buffer with the expected PRP layout, length and direction. */
static bool in_window(u64 lba, u64 n)
{
    return window_set && n && lba >= S1N_PIN_START && lba - S1N_PIN_START < S1N_PIN_BLOCKS &&
           n <= S1N_PIN_BLOCKS - (lba - S1N_PIN_START);
}
/* Metadata reads after select are restricted to the pinned GPT locations. */
static bool gpt_lba(u64 lba, u64 n)
{
    return n == 1 && ((lba >= 1 && lba <= 5) ||
                     (lba >= S1N_PIN_NSZE - 5 && lba < S1N_PIN_NSZE));
}
static bool write_range(u64 lba, u64 n)
{
#if S1N_STAGE_WRITES
    return in_window(lba, n) && n <= S1N_WRITE_MAX_BLOCKS &&
           lba >= S1N_SUB_START && lba - S1N_SUB_START < S1N_SUB_BLOCKS &&
           n <= S1N_SUB_BLOCKS - (lba - S1N_SUB_START)
#if S1N_STAGE_CANARY
           && canary_running
#endif
           ;
#else
    (void)lba; (void)n;
    return false;
#endif
}
void ans_ro_abort(enum ans_ro_reason r)
{
    if (state == ANS_RO_FROZEN) return;
    st.aborts++;
    freeze(r);
}

static bool precheck(const struct ans_queue *q, const struct ans_cmd *c, const struct ans_tcb *t)
{
    if (q->admin)
        return allowed(true, c->opcode);
    if (!allowed(false, c->opcode) || c->nsid != 1 || c->flags || c->metadata || c->cdw13 ||
        c->cdw14 || c->cdw15 || t->opcode || t->slot_id != c->tag || t->prp1 != c->prp1 ||
        t->prp2 != c->prp2 || t->len != c->cdw12)
        return false;
    if (c->opcode == OP_FLUSH)
        return rw_enabled && window_set && !c->prp1 && !c->prp2 && !c->cdw10 && !c->cdw11 &&
               !c->cdw12 && t->dma_flags == 0;
    u64 n = (u64)(c->cdw12 & 0xffff) + 1, lba = c->cdw10 | ((u64)c->cdw11 << 32);
    u64 x = (u64)dma.xfer, bytes = n * ANS_RO_BLOCK;
    if ((c->cdw12 & ~0xffffu) || n > max_xfer || c->prp1 < x || (c->prp1 & (ANS_RO_BLOCK - 1)) ||
        c->prp1 - x > sizeof(dma.xfer) - bytes || bytes > sizeof(dma.xfer))
        return false;
    if (c->opcode == OP_WRITE) {
        if (!rw_enabled || !write_range(lba, n) || t->dma_flags != 2)
            return false;
    } else { /* READ */
        if (t->dma_flags != 1)
            return false;
        if (!(gpt_read_active && gpt_lba(lba, n)) &&
            (window_set ? !in_window(lba, n) : (lba >= st.nsze || n > st.nsze - lba)))
            return false;
    }
    if (n == 1) return c->prp2 == 0;
    if (n == 2) return c->prp2 == c->prp1 + ANS_RO_BLOCK;
    if (c->prp2 != (u64)dma.prp_list) return false;
    for (u64 i = 1; i < n; i++)
        if (dma.prp_list[i - 1] != c->prp1 + i * ANS_RO_BLOCK)
            return false;
    return true;
}

/* r4.3: the pre-doorbell DMA guard (runs before precheck). It re-reads the SQE and TCB as they
 * sit in queue memory and accepts only the command shapes this driver builds, each naming
 * exactly the expected private buffer pages. ANS has no IOMMU for this traffic (SART filters
 * only the RTKit shared buffers), so this is the last point at which a wrong DMA target can be
 * stopped. Any mismatch => the caller freezes; the doorbell is never written. Window/LBA policy
 * stays in precheck (refusal, r4 semantics). */
static bool buf16k(u64 a, const void *buf)
{
    return a == (u64)buf && a && !(a & (ANS_DMA_ALIGN - 1));
}
static bool one_page(u64 prp1, u64 bytes)
{
    return bytes && bytes <= ANS_NVME_PAGE - (prp1 & (ANS_NVME_PAGE - 1));
}
static bool dma_guard(const struct ans_queue *q, const struct ans_cmd *c, const struct ans_tcb *t)
{
    /* TCB must mirror the SQE exactly as nvme.c:229-240 builds it. */
    u8 want_flags = !c->prp1 ? 0 : (c->opcode & 1) ? 2 : 1;
    /* r4.4: the queue's own NVMMU tag, in its own slot (Mu core: admin 0, I/O = 2). */
    const struct ans_cmd *want_slot = &q->cmds[q->admin ? ANS_ADMIN_TAG : next_io_tag];
    if (c != want_slot || c->tag != (q->admin ? ANS_ADMIN_TAG : next_io_tag) ||
        t != &q->tcbs[c->tag])
        return false;
    if (t->opcode || t->slot_id != c->tag || t->unk0 || t->dma_flags != want_flags || t->len != c->cdw12 ||
        t->prp1 != c->prp1 || t->prp2 != c->prp2 || c->metadata)
        return false;
    if (q->admin) {
        if (c->prp2)
            return false;
        switch (c->opcode) {
        case OP_CREATE_CQ: /* nvme.c:398-403 */
            return q == &adminq && buf16k(c->prp1, dma.icpl) && c->cdw10 == (1u | ((QD - 1) << 16)) &&
                   c->cdw11 == QUEUE_CONTIGUOUS && !c->cdw12 && one_page(c->prp1, sizeof(dma.icpl));
        case OP_CREATE_SQ: /* nvme.c:409-415 */
            return q == &adminq && buf16k(c->prp1, dma.icmd) && c->cdw10 == (1u | ((QD - 1) << 16)) &&
                   c->cdw11 == (QUEUE_CONTIGUOUS | (1u << 16)) && !c->cdw12 && one_page(c->prp1, sizeof(dma.icmd));
        case OP_DELETE_SQ: /* nvme.c:515-517 */
        case OP_DELETE_CQ: /* nvme.c:521-523 */
            return !c->prp1 && c->cdw10 == 1 && !c->cdw11 && !c->cdw12;
        default:
            return false;
        }
    }
    if (q != &ioq)
        return false;
    if (c->opcode == OP_FLUSH) /* no data: no PRP at all (rw policy itself is precheck's job) */
        return !c->prp1 && !c->prp2 && !c->cdw12;
    if (c->opcode != OP_READ && c->opcode != OP_WRITE)
        return false;
    u64 x = (u64)dma.xfer, n = (u64)(c->cdw12 & 0xffff) + 1, bytes = n * ANS_RO_BLOCK;
    if (!buf16k(x, dma.xfer) || (c->cdw12 >> 16) || n > ANS_XFER_BYTES / ANS_RO_BLOCK || c->prp1 < x ||
        (c->prp1 & (ANS_NVME_PAGE - 1)) || c->prp1 - x > sizeof(dma.xfer) - bytes)
        return false;
    if (n == 1) /* nvme.c:569-578: one 4 KiB block, page-aligned PRP1, PRP2 = 0 */
        return !c->prp2 && one_page(c->prp1, bytes);
    if (n == 2) /* PRP2 = the second data page (NVMe PRP rules; apple.c:750-753) */
        return c->prp2 == c->prp1 + ANS_NVME_PAGE;
    /* PRP2 = the 16 KiB-aligned private list page; entries = the following data pages (apple.c:755-790) */
    if (!buf16k(c->prp2, dma.prp_list))
        return false;
    for (u64 i = 1; i < n; i++)
        if (dma.prp_list[i - 1] != c->prp1 + i * ANS_NVME_PAGE)
            return false;
    return true;
}

/* r3.1 R2: clear the bounce before any command that DMAs into it, so a completion that
 * reports success without DMA can only ever yield zeros, never stale (e.g. GPT) data. */
static void clear_bounce(void)
{
    memset(dma.bounce, 0, sizeof(dma.bounce));
    dma_wmb();
}

u32 s1n_smp_cmd_timeout_us(void); /* nightly-7: native_link.c */
/* The only submission path. Returns 0 ok, -1 NVMe status error (DMA settled), -2 frozen. */
static int exec_owned(struct ans_queue *q, const struct ans_cmd *cmd);
static int exec(struct ans_queue *q, const struct ans_cmd *cmd)
{
    if (__atomic_test_and_set(&exec_busy, __ATOMIC_ACQUIRE)) {
        freeze(ANS_RO_E_REENTRY);
        return -2;
    }
    int rc;
    if (!s1n_storage_context()) {
        freeze(ANS_RO_E_CONTEXT);
        rc = -2;
    } else {
        rc = exec_owned(q, cmd);
    }
    __atomic_clear(&exec_busy, __ATOMIC_RELEASE);
    return rc;
}

/* Private ownership path: only the owner may perform the first-write GPT reads and
 * rebuild the pending command. Public/nested submissions always enter exec(). */
static int exec_owned(struct ans_queue *q, const struct ans_cmd *cmd)
{
    if (state != ANS_RO_LIVE && !(state == ANS_RO_OFF && q->admin))
        return -2;
    if (!allowed(q->admin, cmd->opcode)) {
        freeze(ANS_RO_E_NOT_ALLOWED);
        return -2;
    }
    if (st.commands != st.cmd_admin + st.cmd_reads + st.cmd_writes + st.cmd_flushes ||
        st.io_submitted != st.io_completed) {
        freeze(ANS_RO_E_COUNTER);
        return -2;
    }

    /* M2: no mailbox or external callback after the slot is filled and checked. */
    if (!drain()) return -2;
    if (__atomic_load_n(&session_frozen, __ATOMIC_ACQUIRE)) return -2;
    dc_ivac_range(&q->cqes[q->head], sizeof(struct ans_cpl));
    sysop("dsb sy"); /* complete cache maintenance before DMA/CPU access */
    dma_rmb();
    if ((q->cqes[q->head].status & 1) == q->phase) {
        st.cqe_anomalies++;
        freeze(ANS_RO_E_CQE);
        return -2;
    }
    /* r4.4: admin tag 0, I/O tag 2 (Mu AppleNvmeControllerCore.c, see ANS_IO_TAG). */
    const u8 tag = q->admin ? ANS_ADMIN_TAG : next_io_tag;
    struct ans_cmd *slot = &q->cmds[tag];
    struct ans_tcb *tcb = &q->tcbs[tag];
    memcpy(slot, cmd, sizeof(*cmd));
    slot->tag = tag;
    memset(tcb, 0, sizeof(*tcb));
    if (!slot->prp1)
        tcb->dma_flags = 0;
    else if (slot->opcode & 1)
        tcb->dma_flags = 2; /* to device: only Create CQ/SQ (queue descriptors) */
    else
        tcb->dma_flags = 1; /* from device */
    tcb->slot_id = tag;
    tcb->len = slot->cdw12;
    tcb->prp1 = slot->prp1;
    tcb->prp2 = slot->prp2;
    dc_civac_range(slot, sizeof(*slot));
    dc_civac_range(tcb, sizeof(*tcb));
    sysop("dsb sy"); /* complete cache maintenance before DMA/CPU access */
    dma_wmb();
    /* r4.3: fail closed before the doorbell - nothing has been handed to ANS yet. */
    if (!dma_guard(q, slot, tcb)) {
        freeze(ANS_RO_E_DMA_GUARD);
        return -2;
    }
    if (!precheck(q, slot, tcb)) {
        st.window_refusals++;
        freeze(ANS_RO_E_WINDOW);
        return -2; /* nothing submitted */
    }
#if S1N_STAGE_WRITES
    if (!q->admin && slot->opcode == OP_WRITE) {
        u64 n = (slot->cdw12 & 0xffff) + 1;
        if (st.cmd_writes >= S1N_WRITE_CMD_CAP || st.blocks_written > S1N_WRITE_BLOCK_CAP ||
            n > S1N_WRITE_BLOCK_CAP - st.blocks_written) {
            freeze(ANS_RO_E_CAP);
            return -2;
        }
    }
#endif
#if S1N_STAGE_WRITES
    if (!q->admin && cmd->opcode == OP_WRITE && !first_write_checked) {
        memcpy(recheck_saved, dma.xfer, sizeof(recheck_saved));
        recheck_active = true;
        bool ok = s1n_recheck_before_write();
        recheck_active = false;
        if (state == ANS_RO_FROZEN) return -2;
        memcpy(dma.xfer, recheck_saved, sizeof(recheck_saved));
        dc_civac_range(dma.xfer, sizeof(dma.xfer));
        memset(recheck_saved, 0, sizeof(recheck_saved));
        if (!ok) {
            freeze(ANS_RO_E_CANARY);
            return -2;
        }
        first_write_checked = true;
        return exec_owned(q, cmd); /* refill, clean and revalidate under the same guard */
    }
#endif
    if (!q->admin && slot->opcode == OP_FLUSH && st.cmd_flushes >= S1N_FLUSH_CAP) {
        freeze(ANS_RO_E_CAP);
        return -2;
    }
#if S1N_STAGE_WRITES
    if (!q->admin && slot->opcode == OP_WRITE) {
        /* Write canary: the absolute LBAs actually placed in an ANS write SQE. */
        u64 first = slot->cdw10 | ((u64)slot->cdw11 << 32), last = first + (slot->cdw12 & 0xffff);
        if (first < st.w_abs_min_first) st.w_abs_min_first = first;
        if (last > st.w_abs_max_last) st.w_abs_max_last = last;
        st.ans_writes++;
    }
#endif
    if (__atomic_load_n(&session_frozen, __ATOMIC_ACQUIRE)) return -2;
    st.commands++;
    if (q->admin) st.cmd_admin++;
    else {
        st.io_submitted++;
        st.io_tags_used |= 1u << tag;
        if (slot->opcode == OP_READ) st.cmd_reads++;
        else if (slot->opcode == OP_FLUSH) st.cmd_flushes++;
#if S1N_STAGE_WRITES
        else st.cmd_writes++;
#endif
    }
    write32(base + (q->admin ? NVME_DB_LINEAR_ASQ : NVME_DB_LINEAR_IOSQ), tag);

    u32 limit = q->admin ? NVME_TIMEOUT
              : slot->opcode == OP_FLUSH ? NVME_FLUSH_TIMEOUT
              : slot->opcode == OP_WRITE ? NVME_WRITE_TIMEOUT : NVME_TIMEOUT;
    /* nightly-7: with more than one vCPU admitted, this wait holds bhl on its CPU, so other vCPUs that trap stall
     * behind it; bound every data command by the launcher's SMP limit (default 1 s; 0 at cap 1 = r4.14 bounds).
     * Expiry freezes the session exactly as the 30 s bound does (smp-storage review s5). */
    if (!q->admin) {
        u32 smp_limit = s1n_smp_cmd_timeout_us();
        if (smp_limit && smp_limit < limit)
            limit = smp_limit;
    }
    u64 timeout = timeout_calculate(limit);
    struct ans_cpl cqe;
    for (;;) {
        if (timeout_expired(timeout)) {
            freeze(ANS_RO_E_CMD_TIMEOUT);
            return -2;
        }
        if (!drain() || __atomic_load_n(&session_frozen, __ATOMIC_ACQUIRE))
            return -2;
        dc_ivac_range(&q->cqes[q->head], sizeof(cqe));
        sysop("dsb sy"); /* complete cache maintenance before DMA/CPU access */
        dma_rmb();
        memcpy(&cqe, &q->cqes[q->head], sizeof(cqe));
        if ((cqe.status & 1) == q->phase)
            break;
    }
    if (cqe.tag != tag) {
        st.cqe_anomalies++;
        freeze(ANS_RO_E_CQE);
        return -2;
    }
    write32(base + NVMMU_TCB_INVAL, cqe.tag);
    /* r4.4 (= r3.5): freeze only on the NVMMU TCB status register the T6050-proven code reads after
     * TCB_INVAL: reg[3]+0x28120 (aura nvme.c, Linux apple.c). reg[9]+0x29120 is the frozen m1n1
     * nvme.c offset: a harmless alias read here (stage 1 reads it after every command; the probe
     * read 0) but not the register either proven driver checks, so a nonzero value there is
     * recorded for diagnostics and no longer freezes. */
    u32 stat_alias = read32(base + NVMMU_TCB_STAT);
    u32 stat_linux = read32(nvmmu + NVMMU_TCB_STAT_LINUX);
    if (stat_alias) {
        st.tcb_alias_nonzero++;
        st.tcb_alias_last = stat_alias;
    }
    if (stat_linux) {
        freeze(ANS_RO_E_TCB);
        return -2;
    }
    if (++q->head == QD) {
        q->head = 0;
        q->phase ^= 1;
    }
    write32(base + (q->admin ? NVME_DB_ACQ : NVME_DB_IOCQ), q->head);
    dc_ivac_range(&q->cqes[q->head], sizeof(cqe));
    sysop("dsb sy"); /* complete cache maintenance before DMA/CPU access */
    dma_rmb();
    if ((q->cqes[q->head].status & 1) == q->phase) {
        st.cqe_anomalies++;
        freeze(ANS_RO_E_CQE);
        return -2;
    }
    if (!q->admin) {
        st.io_completed++;
        next_io_tag = tag == ANS_IO_TAG ? ANS_RO_IO_TAG_ALT : ANS_IO_TAG;
    }
    /* Every unexpected physical completion status is terminal, including Delete SQ/CQ. */
    if (cqe.status >> 1) {
        freeze(ANS_RO_E_STATUS);
        return -2;
    }
#if S1N_STAGE_WRITES
    if (!q->admin && slot->opcode == OP_WRITE) {
        u64 first = slot->cdw10 | ((u64)slot->cdw11 << 32);
        u64 n = (slot->cdw12 & 0xffff) + 1;
        st.blocks_written += n;
        if (first < st.lowest_written) st.lowest_written = first;
        if (first + n - 1 > st.highest_written) st.highest_written = first + n - 1;
    }
#endif
    return 0;
}

static bool wait_csts(u32 mask, u32 want, u32 usec)
{
    u64 timeout = timeout_calculate(usec);
    for (;;) {
        u32 csts = read32(base + NVME_CSTS);
        if (csts & NVME_CSTS_CFS)
            return false;
        if ((csts & mask) == want)
            return true;
        if (timeout_expired(timeout))
            return false;
        if (!drain())
            return false;
    }
}

static void init_queue(struct ans_queue *q, struct ans_tcb *t, struct ans_cmd *c, struct ans_cpl *p,
                       bool admin)
{
    memset(t, 0, QD * sizeof(*t));
    memset(c, 0, QD * sizeof(*c));
    memset(p, 0, QD * sizeof(*p));
    dc_civac_range(t, QD * sizeof(*t));
    dc_civac_range(c, QD * sizeof(*c));
    dc_civac_range(p, QD * sizeof(*p));
    sysop("dsb sy"); /* complete cache maintenance before DMA/CPU access */
    q->tcbs = t;
    q->cmds = c;
    q->cqes = p;
    q->head = 0;
    q->phase = 1;
    q->admin = admin;
}

void ans_ro_scrub(void)
{
    clear_bounce();
    memset(dma.xfer, 0, sizeof(dma.xfer)); /* r4: GPT reads land in the transfer buffer */
    dc_civac_range(dma.xfer, sizeof(dma.xfer));
}

static bool adt_ans_irqs_ok(int node)
{
    u32 len = 0;
    const u32 *irqs = adt_getprop(adt, node, "interrupts", &len);
    if (!irqs || len != 5 * sizeof(u32))
        return false;
    unsigned seen = 0;
    for (unsigned i = 0; i < 5; i++) {
        if (!ans_ro_owns_aic_line(irqs[i]))
            return false;
        seen |= 1u << (irqs[i] == 2338 ? 4 : irqs[i] - 1347);
    }
    return seen == 0x1f;
}

bool ans_ro_owns_aic_line(u32 line)
{
    return (line >= 1347 && line <= 1350) || line == 2338;
}

bool ans_ro_init(void)
{
    if (!s1n_storage_context()) { freeze(ANS_RO_E_CONTEXT); return false; }
    if (state != ANS_RO_OFF)
        return false;
    st.lowest_written = UINT64_MAX;
    st.w_abs_min_first = UINT64_MAX; /* r4 canary: "no write yet" */

    int path[8], spath[8];
    int node = adt_path_offset_trace(adt, "/arm-io/ans", path);
    int snode = adt_path_offset_trace(adt, "/arm-io/sart-ans", spath);
    u64 reg0 = 0, reg3 = 0, reg9 = 0, sreg = 0;
    const u32 *sver = snode >= 0 ? adt_getprop(adt, snode, "sart-version", NULL) : NULL;
    if (node < 0 || snode < 0 || !adt_get_property(adt, node, "nvme-secure-bar") ||
        adt_get_reg(adt, path, "reg", 0, &reg0, NULL) < 0 ||
        adt_get_reg(adt, path, "reg", 3, &reg3, NULL) < 0 ||
        adt_get_reg(adt, path, "reg", 9, &reg9, NULL) < 0 ||
        adt_get_reg(adt, spath, "reg", 0, &sreg, NULL) < 0 || !sver) {
        set_state(ANS_RO_REFUSED, ANS_RO_E_ADT);
        return false;
    }
    if (reg0 != T6050_ANS_REG0 || reg3 != T6050_ANS_REG3 || reg9 != T6050_ANS_REG9 || sreg != T6050_SART_BASE ||
        *sver != 4 || !adt_ans_irqs_ok(node)) {
        set_state(ANS_RO_REFUSED, ANS_RO_E_GEOMETRY);
        return false;
    }
    base = reg9;
    nvmmu = reg3;
    die = (base >> 37) & 3;

    asc = asc_init("/arm-io/ans");
    sart_regs = sreg;
    sart = sart_init("/arm-io/sart-ans");
    if (!asc || !sart) {
        set_state(ANS_RO_REFUSED, ANS_RO_E_ALLOC);
        return false;
    }
    /* Admission (reads only): stopped stage-1 handoff, at least 4 free unprotected SART slots. */
    if (asc_cpu_running(asc)) {
        set_state(ANS_RO_REFUSED, ANS_RO_E_RUNNING);
        return false;
    }
    for (unsigned i = 0; i < 16; i++) {
        if (read32(sreg + 4 * i))
            st.sart_protected++;
        else
            st.sart_free++;
    }
    if (st.sart_free < 4) {
        set_state(ANS_RO_REFUSED, ANS_RO_E_SART_SLOTS);
        return false;
    }
    rtk = rtkit_init("nvme", asc, NULL, NULL, sart, false);
    if (!rtk) {
        set_state(ANS_RO_REFUSED, ANS_RO_E_ALLOC);
        return false;
    }
    init_queue(&adminq, dma.atcb, dma.acmd, dma.acpl, true);
    init_queue(&ioq, dma.itcb, dma.icmd, dma.icpl, false);
    /* r4.3: the bases written to ASQ/ACQ/NVMMU below must each be a 16 KiB-aligned buffer of
     * their own (nvme.c:379-380, 387-388 write memalign(SZ_16K) pointers). Checked before the
     * coprocessor is touched, so a failure here is a clean refusal. r4.4: its own reason
     * (E_LAYOUT, 21, REFUSED) so 20 always means the pre-doorbell guard froze (review L2). */
    if (!buf16k((u64)adminq.tcbs, dma.atcb) || !buf16k((u64)ioq.tcbs, dma.itcb) ||
        !buf16k((u64)adminq.cmds, dma.acmd) || !buf16k((u64)adminq.cqes, dma.acpl) ||
        !buf16k((u64)ioq.cmds, dma.icmd) || !buf16k((u64)ioq.cqes, dma.icpl) ||
        !buf16k((u64)dma.xfer, dma.xfer) || !buf16k((u64)dma.prp_list, dma.prp_list)) {
        set_state(ANS_RO_REFUSED, ANS_RO_E_LAYOUT);
        return false;
    }

    /* From here on the coprocessor is touched: any failure freezes. */
    /* Bound the RTKit power waits only for our own calls; other RTKit users (DCP, SMC)
     * keep their original behaviour. */
    rtkit_set_power_timeout(RTKIT_BOOT_POWER_TIMEOUT); /* r4.4: 10 s (was 1 s) */
    rtkit_set_boot_timeout(RTKIT_HELLO_TIMEOUT); /* r3.1: HELLO/EPMAP waits, restored below; r4.4: 10 s */
    (void)rtkit_take_timeouts();
    bool booted = rtkit_boot(rtk);
    rtkit_set_boot_timeout(0);
    rtkit_set_power_timeout(0);
    /* r4.4: which wait timed out (RTKIT_TIMEOUT_* bits; 0 = the failure was not a timeout). */
    st.rtkit_timeouts |= rtkit_take_timeouts();
    if (!booted) {
        freeze(ANS_RO_E_RTKIT);
        return false;
    }
    if (poll32(base + NVME_BOOT_STATUS, 0xffffffff, NVME_BOOT_STATUS_OK, USEC_PER_SEC) < 0) {
        freeze(ANS_RO_E_BOOT_STATUS);
        return false;
    }
    set32(base + NVME_LINEAR_SQ_CTRL, NVME_LINEAR_SQ_CTRL_EN);
    write32(base + NVME_MAX_PEND_CMDS_CTRL, ((QD - 1) << 16) | (QD - 1));
    write32(base + NVMMU_NUM, QD - 1);
    write64_lo_hi(base + NVMMU_ASQ_BASE, (u64)adminq.tcbs);
    write64_lo_hi(base + NVMMU_IOSQ_BASE, (u64)ioq.tcbs);

    clear32(base + NVME_CC, NVME_CC_EN);
    if (!wait_csts(NVME_CSTS_RDY, 0, NVME_TIMEOUT)) {
        if (state != ANS_RO_FROZEN)
            freeze(ANS_RO_E_DISABLE);
        return false;
    }
    write64_lo_hi(base + NVME_ASQ, (u64)adminq.cmds);
    write64_lo_hi(base + NVME_ACQ, (u64)adminq.cqes);
    write32(base + NVME_AQA, ((QD - 1) << 16) | (QD - 1));
    mask32(base + NVME_CC, NVME_CC_SHN, NVME_CC_EN);
    if (!wait_csts(NVME_CSTS_RDY, NVME_CSTS_RDY, NVME_ENABLE_TIMEOUT)) {
        if (state != ANS_RO_FROZEN)
            freeze(ANS_RO_E_ENABLE);
        return false;
    }

    struct ans_cmd cmd;
    /* r4.1 (= r3.2 fix): capacity is the pinned whole-disk value, NOT Identify Namespace NSZE.
     * On this Apple ANS the minimal admin path's Identify Namespace completes but returns NSZE=0
     * (the proven m1n1 nvme.c read path issues no Identify at all). The strict DMA-verified
     * identity/geometry proof is native_window's GPT entry-7 gate plus the backup-header
     * (AlternateLBA == capacity-1) cross-check, all over the proven READ path. */
    st.nsze = T6050_NSZE;
    st.lbads = 12;

    /* r4.2/r4.3 (= r3.3/r3.4): issue NO standalone Identify (r4.3 also drops it from the
     * allowlist). The r3.3 claim "ANS rejects Identify and poisons the admin queue" is UNPROVEN
     * (independent review F3): the failing Identify and Create CQ both used buffers that were not
     * page aligned, and Linux flags this controller NVME_QUIRK_ADMIN_PAGE_ALIGN. Misalignment is
     * the leading hypothesis; r4.3 fixes only that. After enable: Create CQ, Create SQ, then I/O,
     * as nvme.c:395-427. Capacity is pinned; identity/geometry is proven over the controlled READ
     * path by native_window's GPT gate + AlternateLBA cross-check. */
    st.identify_nsze = UINT64_MAX;
    st.identity_confirmed = 0;

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = OP_CREATE_CQ;
    cmd.prp1 = (u64)ioq.cqes;
    cmd.cdw10 = 1 | ((QD - 1) << 16);
    cmd.cdw11 = QUEUE_CONTIGUOUS;
    if (exec(&adminq, &cmd)) {
        if (state != ANS_RO_FROZEN)
            freeze(ANS_RO_E_CMD_STATUS);
        return false;
    }
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = OP_CREATE_SQ;
    cmd.prp1 = (u64)ioq.cmds;
    cmd.cdw10 = 1 | ((QD - 1) << 16);
    cmd.cdw11 = QUEUE_CONTIGUOUS | (1 << 16);
    if (exec(&adminq, &cmd)) {
        if (state != ANS_RO_FROZEN)
            freeze(ANS_RO_E_CMD_STATUS);
        return false;
    }
    /* M4+ needs the I/O queue addresses in these registers too (nvme.c order: CQ, then SQ). */
    write64_lo_hi(base + NVME_IOQ_CQES, (u64)ioq.cqes);
    write64_lo_hi(base + NVME_IOQ_CMDS, (u64)ioq.cmds);
    set_state(ANS_RO_LIVE, ANS_RO_OK);
    printf("ans_ro: %s live, NSID 1 %lu x %u-byte blocks, SART %u protected/%u free\n",
           s1n_stage_marker, (unsigned long)st.nsze, 1u << st.lbads, st.sart_protected, st.sart_free);
    return true;
}

/* r4: one ANS READ or WRITE of `blocks` (<= max_xfer) at absolute `lba`, DMA to/from
 * dma.xfer + offset only. The SQE/TCB are authored here from validated values, and precheck()
 * re-validates them against the window right before the doorbell. */
static bool xfer_owned(bool write, u64 lba, u32 blocks, u64 offset, bool recheck);
bool ans_ro_access_ok(void)
{
    if (__atomic_load_n(&exec_busy, __ATOMIC_ACQUIRE)) {
        freeze(ANS_RO_E_REENTRY);
        return false;
    }
    if (!s1n_storage_context()) {
        freeze(ANS_RO_E_CONTEXT);
        return false;
    }
    return !__atomic_load_n(&session_frozen, __ATOMIC_ACQUIRE);
}
bool ans_ro_xfer(bool write, u64 lba, u32 blocks, u64 offset)
{
    if (!ans_ro_access_ok()) return false;
    return xfer_owned(write, lba, blocks, offset, false);
}
static bool xfer_owned(bool write, u64 lba, u32 blocks, u64 offset, bool recheck)
{
    if (state != ANS_RO_LIVE) return false;
    u64 bytes = (u64)blocks * ANS_RO_BLOCK;
    if ( !blocks || blocks > max_xfer || (offset & (ANS_RO_BLOCK - 1)) ||
        offset > sizeof(dma.xfer) || bytes > sizeof(dma.xfer) - offset ||
        (write && (!rw_enabled || !write_range(lba, blocks))) ||
        (!write && !(gpt_read_active && gpt_lba(lba, blocks)) &&
         (window_set ? !in_window(lba, blocks) : (lba >= st.nsze || blocks > st.nsze - lba)))) {
        st.window_refusals++;
        ans_ro_abort(ANS_RO_E_ABORT);
        return false;
    }
    u8 *buf = dma.xfer + offset;
    struct ans_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
#if S1N_STAGE_WRITES
    cmd.opcode = write ? OP_WRITE : OP_READ;
#else
    cmd.opcode = OP_READ;
#endif
    cmd.nsid = 1;
    cmd.prp1 = (u64)buf;
    if (blocks == 2)
        cmd.prp2 = (u64)buf + ANS_RO_BLOCK;
    else if (blocks > 2) {
        for (u32 i = 1; i < blocks; i++)
            dma.prp_list[i - 1] = (u64)buf + (u64)i * ANS_RO_BLOCK;
        cmd.prp2 = (u64)dma.prp_list;
        dc_civac_range(dma.prp_list, sizeof(dma.prp_list));
    }
    cmd.cdw10 = (u32)lba;
    cmd.cdw11 = (u32)(lba >> 32);
    cmd.cdw12 = blocks - 1;
    if (write) {
        /* Data must be at the point of coherency before ANS reads it. */
        dc_civac_range(buf, bytes);
    } else {
        /* r3.1 R2: no stale data can survive a success-without-DMA; and no dirty line may be
         * written back over DMA'd data later. */
        memset(buf, 0, bytes);
        dc_civac_range(buf, bytes);
    }
    dma_wmb();
    int rc = recheck ? exec_owned(&ioq, &cmd) : exec(&ioq, &cmd);
    if (rc) {
        if (write) st.write_errors++; else st.read_errors++;
        return false;
    }
    if (!write) {
        dc_civac_range(buf, bytes);
        sysop("dsb sy"); /* complete cache maintenance before DMA/CPU access */
        dma_rmb();
        st.reads += blocks;
    } else {
        st.writes += blocks;
    }
    return true;
}

bool ans_ro_flush(void)
{
    if (!ans_ro_access_ok()) return false;
    if (state != ANS_RO_LIVE || !rw_enabled)
        return false;
    struct ans_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = OP_FLUSH;
    cmd.nsid = 1;
    if (exec(&ioq, &cmd)) {
        st.flush_errors++;
        return false;
    }
    st.flushes++;
    return true;
}

bool ans_ro_set_window(bool rw, u32 max_blocks)
{
    if (!ans_ro_access_ok()) return false;
    if (state != ANS_RO_LIVE || window_set || st.nsze != S1N_PIN_NSZE ||
        !max_blocks || max_blocks > ANS_XFER_BYTES / ANS_RO_BLOCK)
        return false;
    max_xfer = max_blocks;
    rw_enabled = rw;
    window_set = true;
    return true;
}

bool ans_ro_read_gpt(u64 lba, void *out)
{
    if (!s1n_storage_context()) { freeze(ANS_RO_E_CONTEXT); return false; }
    if (gpt_read_active) { freeze(ANS_RO_E_REENTRY); return false; }
    if (!gpt_lba(lba, 1) || gpt_read_active) return false;
    gpt_read_active = true;
    bool ok;
    if (recheck_active && s1n_storage_context()) {
        ok = out && xfer_owned(false, lba, 1, 0, true);
        if (ok) memcpy(out, dma.xfer, ANS_RO_BLOCK);
    } else {
        ok = ans_ro_read_block(lba, out);
    }
    gpt_read_active = false;
    return ok;
}

/* Only this routine can open the write gate in stages 1/2. One shot even on failure. */
bool ans_ro_run_canary(void)
{
    if (!ans_ro_access_ok()) return false;
#if S1N_STAGE_CANARY
    if (!s1n_canary_context()) { freeze(ANS_RO_E_CONTEXT); return false; }
    if (canary_tried || !window_set || !rw_enabled || state != ANS_RO_LIVE) return false;
    canary_tried = true;
    canary_running = true;
    for (u64 off = 0; off < S1N_SUB_BLOCKS; off += S1N_CANARY_CHUNK) {
        const u32 n = S1N_CANARY_CHUNK;
        const u64 lba = S1N_SUB_START + off;
        /* max_xfer may be lower than the stage maximum; save/read in the same chunks. */
        for (u32 i = 0; i < n; i += max_xfer) {
            u32 k = n - i < max_xfer ? n - i : max_xfer;
            if (!ans_ro_xfer(false, lba + i, k, (u64)i * ANS_RO_BLOCK)) goto fail;
        }
        memcpy(canary_saved, dma.xfer, sizeof(canary_saved));
        /* Verify every saved byte before the first write to this chunk. */
        for (u32 i = 0; i < n; i += max_xfer) {
            u32 k = n - i < max_xfer ? n - i : max_xfer;
            if (!ans_ro_xfer(false, lba + i, k, (u64)i * ANS_RO_BLOCK)) goto fail;
        }
        if (memcmp(canary_saved, dma.xfer, sizeof(canary_saved))) {
            freeze(ANS_RO_E_CANARY);
            goto fail;
        }
        for (u32 i = 0; i < sizeof(canary_pattern); i++)
            canary_pattern[i] = (u8)((lba + i / ANS_RO_BLOCK) * 31 + i * 13 + 0x5a);
        for (unsigned pass = 0; pass < 2; pass++) {
            const u8 *expected = pass ? canary_saved : canary_pattern;
            memcpy(dma.xfer, expected, sizeof(canary_saved));
            for (u32 i = 0; i < n;) {
                u32 k = n - i < max_xfer ? n - i : max_xfer;
                if (k > S1N_WRITE_MAX_BLOCKS) k = S1N_WRITE_MAX_BLOCKS;
                if (!ans_ro_xfer(true, lba + i, k, (u64)i * ANS_RO_BLOCK)) goto fail;
                i += k;
            }
            if (!ans_ro_flush()) goto fail;
            for (u32 i = 0; i < n; i += max_xfer) {
                u32 k = n - i < max_xfer ? n - i : max_xfer;
                if (!ans_ro_xfer(false, lba + i, k, (u64)i * ANS_RO_BLOCK)) goto fail;
            }
            if (memcmp(dma.xfer, expected, sizeof(canary_saved))) {
                freeze(ANS_RO_E_CANARY);
                goto fail;
            }
        }
    }
    canary_running = false;
    if (st.cmd_writes != S1N_WRITE_CMD_CAP || st.blocks_written != S1N_WRITE_BLOCK_CAP ||
        st.io_submitted != st.io_completed) {
        freeze(ANS_RO_E_COUNTER);
        return false;
    }
    memset(canary_saved, 0, sizeof(canary_saved));
    memset(canary_pattern, 0, sizeof(canary_pattern));
    return true;
fail:
    canary_running = false;
    return false;
#else
    return false;
#endif
}

void *ans_ro_xfer_buffer(size_t *capacity)
{
    if (!ans_ro_access_ok()) { *capacity = 0; return NULL; }
    *capacity = sizeof(dma.xfer);
    return dma.xfer;
}

void ans_ro_canary(u64 *min_first, u64 *max_last, u64 *ans_writes, u64 *refusals)
{
    *min_first = st.w_abs_min_first;
    *max_last = st.w_abs_max_last;
    *ans_writes = st.ans_writes;
    *refusals = st.window_refusals;
}

bool ans_ro_read_block(u64 lba, void *out)
{
    if (!out || !ans_ro_xfer(false, lba, 1, 0))
        return false;
    memcpy(out, dma.xfer, ANS_RO_BLOCK);
    return true;
}

void ans_ro_poll(void)
{
    if (!ans_ro_access_ok()) return;
    if (state != ANS_RO_LIVE)
        return;
    st.polls++;
    drain();
}

bool ans_ro_shutdown(void)
{
    if (!ans_ro_access_ok()) return false;
    if (state != ANS_RO_LIVE)
        return false;
    /* Exactly the stage-1 nvme_shutdown() order. Command failures are recorded but, as in
     * stage 1, the quiesce continues; a frozen driver never reaches this point.
     * r4: in rw mode a FLUSH comes first, so written data is durable before the queues go
     * away (SHN below also asks the firmware to persist its cache). */
    if (rw_enabled && !ans_ro_flush() && state == ANS_RO_FROZEN)
        return false;
    struct ans_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = OP_DELETE_SQ;
    cmd.cdw10 = 1;
    int rc_sq = exec(&adminq, &cmd);
    if (rc_sq == -2)
        return false;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = OP_DELETE_CQ;
    cmd.cdw10 = 1;
    int rc_cq = exec(&adminq, &cmd);
    if (rc_cq == -2)
        return false;
    /* r4.4: with both I/O queues deleted, clear the post-M4 I/O queue registers: +0x1200 (IOQ_CMDS)
     * then +0x1208 (IOQ_CQES), each lo then hi, on reg[9]. Behaviour of aura nvme.c nvme_shutdown
     * (bedeef0d, T6050 = NVME_T8132 without nvme-secure-reg-layout: write64_lo_hi(qreg(IOQ_CMDS), 0);
     * write64_lo_hi(qreg(IOQ_CQES), 0) after Delete SQ/CQ); aura skips it when a Delete fails, and so
     * does this. Own code (aura's nvme.c is MIT AND GPL-2.0-only: behaviour reference only). */
    if (!rc_sq && !rc_cq) {
        write64_lo_hi(base + NVME_IOQ_CMDS, 0);
        write64_lo_hi(base + NVME_IOQ_CQES, 0);
        st.ioq_regs_cleared = 1;
    }
    mask32(base + NVME_CC, NVME_CC_SHN, FIELD_PREP(NVME_CC_SHN, NVME_CC_SHN_NORMAL));
    if (!wait_csts(NVME_CSTS_SHST, FIELD_PREP(NVME_CSTS_SHST, NVME_CSTS_SHST_DONE), NVME_SHUTDOWN_TIMEOUT))
        set_state(state, ANS_RO_E_SHUTDOWN);
    if (state == ANS_RO_FROZEN)
        return false;
    clear32(base + NVME_CC, NVME_CC_EN);
    if (!wait_csts(NVME_CSTS_RDY, 0, NVME_TIMEOUT)) {
        if (state != ANS_RO_FROZEN)
            freeze(ANS_RO_E_DISABLE);
        return false;
    }
    rtkit_set_power_timeout(RTKIT_POWER_TIMEOUT);
    rtkit_sleep(rtk);
    rtkit_set_power_timeout(0);
    st.rtkit_timeouts |= rtkit_take_timeouts(); /* r4.4: diagnostic only; control flow unchanged */
    /* Stage 1 issues these on every boot; the 2026-11-09 probe confirmed no SError. */
    pmgr_reset(die, "ANS");
    pmgr_reset(die, "ANS2");
    if (asc_cpu_running(asc)) {
        freeze(ANS_RO_E_SHUTDOWN);
        return false;
    }
    rtkit_free(rtk);
    sart_free(sart);
    asc_free(asc);
    rtk = NULL;
    sart = NULL;
    asc = NULL;
    set_state(ANS_RO_STOPPED, ANS_RO_OK);
    printf("ans_ro: stopped (stage-1 sequence)\n");
    return true;
}

void ans_ro_dma_region(u64 *start, u64 *size)
{
    *start = (u64)&dma;
    *size = sizeof(dma);
}

#ifdef ANS_RO_HOST_TEST
/* Test only: simulate the canary caller to independently exercise its lower driver gates. */
bool ans_ro_test_stage_write(u64 lba, u32 blocks)
{
#if S1N_STAGE_CANARY
    canary_running = true;
#endif
    bool ok = ans_ro_xfer(true, lba, blocks, 0);
#if S1N_STAGE_CANARY
    canary_running = false;
#endif
    return ok;
}
/* Host-only: push an arbitrary command through the single submission path. */
int ans_ro_test_submit(bool admin, u8 opcode)
{
    struct ans_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = opcode;
    cmd.nsid = 1;
    /* r4: data DMA is only ever allowed into the transfer buffer; r4.3: FLUSH carries no PRP. */
    cmd.prp1 = (!admin && opcode == OP_FLUSH) ? 0 : (u64)dma.xfer;
    if (!admin && opcode == OP_READ) {
        cmd.cdw10 = (u32)(window_set ? S1N_PIN_START : 1);
        cmd.cdw11 = (u32)((window_set ? S1N_PIN_START : 1) >> 32);
    }
    return exec(admin ? &adminq : &ioq, &cmd);
}
int ans_ro_test_submit_raw(bool admin, u8 opcode, u64 prp1, u64 prp2, u32 cdw10, u32 cdw11, u32 cdw12)
{
    struct ans_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = opcode;
    cmd.nsid = 1;
    cmd.prp1 = prp1;
    cmd.prp2 = prp2;
    cmd.cdw10 = cdw10;
    cmd.cdw11 = cdw11;
    cmd.cdw12 = cdw12;
    return exec(admin ? &adminq : &ioq, &cmd);
}
bool ans_ro_test_dma_buffer(unsigned i, u64 *addr, u64 *size, const char **name)
{
    static const char *const names[] = {"xfer", "prp_list", "atcb", "itcb", "acmd", "icmd", "acpl", "icpl", "bounce"};
    const void *b[] = {dma.xfer, dma.prp_list, dma.atcb, dma.itcb, dma.acmd, dma.icmd, dma.acpl, dma.icpl, dma.bounce};
    const u64 s[] = {sizeof(dma.xfer), sizeof(dma.prp_list), sizeof(dma.atcb), sizeof(dma.itcb), sizeof(dma.acmd),
                     sizeof(dma.icmd), sizeof(dma.acpl), sizeof(dma.icpl), sizeof(dma.bounce)};
    if (i >= ANS_RO_DMA_BUFFERS)
        return false;
    *addr = (u64)b[i];
    *size = s[i];
    *name = names[i];
    return true;
}
#endif

u64 ans_ro_nsze(void) { return st.nsze; }
u32 ans_ro_lbads(void) { return st.lbads; }
enum ans_ro_state ans_ro_state(void) { return state; }
const struct ans_ro_stats *ans_ro_stats(void) { return &st; }

/* Fixed SARTv4 register offsets, bounded read-only diagnostic API. */
u32 ans_ro_sart_reg(unsigned which, unsigned i)
{
    static const u32 offsets[] = {0, 0x60, 0xc0};
    if (!sart_regs || which >= 3 || i >= 16) return 0;
    return read32(sart_regs + offsets[which] + 4 * i);
}
