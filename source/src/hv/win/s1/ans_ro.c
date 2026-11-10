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
 *   - reads go only to NSID 1, one block per command, through the private bounce. */
#include "ans_ro.h"
#include "adt.h"
#include "asc.h"
#include "pmgr.h"
#include "rtkit.h"
#include "sart.h"
#include "string.h"
#include "utils.h"

#define T6050_ANS_REG0  0x419600000UL
#define T6050_ANS_REG9  0x45dcc0000UL
#define T6050_ANS_REG3  0x41dcc0000UL /* NVMMU bank; only the Linux TCB status register is read here */
#define T6050_SART_BASE 0x41dc50000UL
/* r3.1: pinned controller identity (J716s internal SSD, 2026-11-09 Linux + probe) */
#define T6050_NSZE  244276265ULL
#define T6050_MODEL "APPLE SSD AP1024Z"
#define T6050_FW    "241.60.3"
#define RTKIT_HELLO_TIMEOUT 5000000 /* pcie_t6050.c: ANS can take ~3 s to answer after a stop */

#define NVME_TIMEOUT          1000000
#define NVME_ENABLE_TIMEOUT   5000000
#define NVME_SHUTDOWN_TIMEOUT 5000000
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
#define NVMMU_TCB_STAT  0x29120 /* frozen nvme.c offset (via reg[9]) */
#define NVMMU_TCB_STAT_LINUX 0x28120 /* Linux apple.c offset (via reg[3]) */

#define OP_DELETE_SQ 0x00
#define OP_CREATE_SQ 0x01
#define OP_READ      0x02
#define OP_DELETE_CQ 0x04
#define OP_CREATE_CQ 0x05
#define OP_IDENTIFY  0x06
#define QUEUE_CONTIGUOUS BIT(0)

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

/* Private DMA memory: resident BSS, page aligned, never in guest RAM. */
static struct {
    struct ans_tcb atcb[QD], itcb[QD];
    struct ans_cmd acmd[QD], icmd[QD];
    struct ans_cpl acpl[QD], icpl[QD];
    u8 bounce[ANS_RO_BLOCK];
} dma __attribute__((aligned(16384)));

static enum ans_ro_state state;
static struct ans_ro_stats st;
static u64 base, nvmmu;
static asc_dev_t *asc;
static sart_dev_t *sart;
static rtkit_dev_t *rtk;
static struct ans_queue adminq, ioq;
static u8 die;

static void set_state(enum ans_ro_state s, enum ans_ro_reason r)
{
    state = s;
    st.state = s;
    if (r && !st.reason)
        st.reason = r;
}
static void freeze(enum ans_ro_reason r)
{
    /* No MMIO, no RTKit sleep, no CPU stop, no PMGR, no free: platform reset recovers. */
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
        return opcode == OP_IDENTIFY || opcode == OP_CREATE_CQ || opcode == OP_CREATE_SQ ||
               opcode == OP_DELETE_SQ || opcode == OP_DELETE_CQ;
    return opcode == OP_READ;
}

/* r3.1 R2: clear the bounce before any command that DMAs into it, so a completion that
 * reports success without DMA can only ever yield zeros, never stale (e.g. GPT) data. */
static void clear_bounce(void)
{
    memset(dma.bounce, 0, sizeof(dma.bounce));
    dma_wmb();
}

/* The only submission path. Returns 0 ok, -1 NVMe status error (DMA settled), -2 frozen. */
static int exec(struct ans_queue *q, const struct ans_cmd *cmd)
{
    if (state != ANS_RO_LIVE && !(state == ANS_RO_OFF && q->admin))
        return -2;
    if (!allowed(q->admin, cmd->opcode)) {
        set_state(state, ANS_RO_E_NOT_ALLOWED);
        return -1;
    }
    const u8 tag = 0;
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
    dma_wmb();
    st.commands++;
    if (!drain())
        return -2;
    write32(base + (q->admin ? NVME_DB_LINEAR_ASQ : NVME_DB_LINEAR_IOSQ), tag);

    u64 timeout = timeout_calculate(NVME_TIMEOUT);
    struct ans_cpl cqe;
    for (;;) {
        if (timeout_expired(timeout)) {
            freeze(ANS_RO_E_CMD_TIMEOUT);
            return -2;
        }
        if (!drain())
            return -2;
        dma_rmb();
        memcpy(&cqe, &q->cqes[q->head], sizeof(cqe));
        if ((cqe.status & 1) == q->phase)
            break;
    }
    if (cqe.tag != tag) {
        freeze(ANS_RO_E_CMD_TAG);
        return -2;
    }
    write32(base + NVMMU_TCB_INVAL, cqe.tag);
    /* Both status reads are proven on this machine: nvme.c reads reg9+0x29120 on every
     * stage-1 command; Linux reads NVMMU+0x28120. Either nonzero => uncertain TCB => freeze. */
    u32 stat_frozen = read32(base + NVMMU_TCB_STAT);
    u32 stat_linux = read32(nvmmu + NVMMU_TCB_STAT_LINUX);
    if (stat_frozen || stat_linux) {
        freeze(ANS_RO_E_TCB);
        return -2;
    }
    if (++q->head == QD) {
        q->head = 0;
        q->phase ^= 1;
    }
    write32(base + (q->admin ? NVME_DB_ACQ : NVME_DB_IOCQ), q->head);
    return (cqe.status >> 1) ? -1 : 0;
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
    q->tcbs = t;
    q->cmds = c;
    q->cqes = p;
    q->head = 0;
    q->phase = 1;
    q->admin = admin;
}

/* NVMe identify strings are space padded ASCII. */
static bool field_eq(const u8 *field, unsigned len, const char *want)
{
    unsigned n = strlen(want);
    if (n > len || memcmp(field, want, n))
        return false;
    for (unsigned i = n; i < len; i++)
        if (field[i] != ' ' && field[i] != 0)
            return false;
    return true;
}

void ans_ro_scrub(void)
{
    clear_bounce();
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
    if (state != ANS_RO_OFF)
        return false;

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

    /* From here on the coprocessor is touched: any failure freezes. */
    /* Bound the RTKit power waits only for our own calls; other RTKit users (DCP, SMC)
     * keep their original behaviour. */
    rtkit_set_power_timeout(RTKIT_POWER_TIMEOUT);
    rtkit_set_boot_timeout(RTKIT_HELLO_TIMEOUT); /* r3.1: HELLO/EPMAP waits, restored below */
    bool booted = rtkit_boot(rtk);
    rtkit_set_boot_timeout(0);
    rtkit_set_power_timeout(0);
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
    /* r3.2: capacity is the pinned whole-disk value, NOT Identify Namespace NSZE.
     *
     * Root cause (hardware RUN-0, 2026-11-09): the proven path, m1n1 src/nvme.c, issues NO
     * Identify command at all (it reads with externally-known geometry); the earlier successful
     * proxy probe used exactly that path. r3.1 added an Identify Namespace (CNS 0) step the proven
     * path never exercised, and on this Apple ANS it completes but returns NSZE=0, so the geometry
     * pin froze. Linux (drivers/nvme/host/apple.c, NVME_QUIRK_IDENTIFY_CNS) obtains capacity only
     * through its fully initialized core; our minimal admin path cannot rely on it. The real
     * DMA-verified proof of "right disk, reads work" is the GPT entry-7 gate in native_window
     * (unique GUID + start + size + header/entries CRC, all via the proven I/O READ path), plus a
     * cross-check that the GPT backup-header LBA == capacity-1. So we pin capacity to the known
     * constant and let native_window do the strict identity/geometry check over real reads. */
    st.nsze = T6050_NSZE;
    st.lbads = 12; /* Apple ANS: 4096-byte logical blocks (pinned; confirmed by probe + Linux) */

    /* Advisory diagnostics, never a gate. These exercise admin-Identify on this minimal path so a
     * hardware run can record what the controller actually returns, without blocking selection. A
     * genuine fault (timeout / wrong tag / TCB) still freezes via exec; a benign zero or NVMe
     * status error does not. Identify Namespace is expected to report NSZE=0 on this ANS (the r3.1
     * regression); Identify Controller is expected to carry model/firmware if admin-Identify DMA
     * works at all. */
    st.identify_nsze = UINT64_MAX; /* sentinel: "not observed" */
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = OP_IDENTIFY;
    cmd.nsid = 1;
    cmd.prp1 = (u64)dma.bounce;
    cmd.cdw10 = 0; /* CNS 0: namespace */
    clear_bounce();
    int rc = exec(&adminq, &cmd);
    if (rc == -2)
        return false;
    if (rc == 0) {
        dma_rmb();
        memcpy(&st.identify_nsze, dma.bounce, sizeof(st.identify_nsze));
    }
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = OP_IDENTIFY;
    cmd.prp1 = (u64)dma.bounce;
    cmd.cdw10 = 1; /* CNS 1: controller */
    clear_bounce();
    rc = exec(&adminq, &cmd);
    if (rc == -2)
        return false;
    if (rc == 0) {
        dma_rmb();
        st.identity_confirmed =
            field_eq(dma.bounce + 24, 40, T6050_MODEL) && field_eq(dma.bounce + 64, 8, T6050_FW);
    }
    clear_bounce();

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
    printf("ans_ro: live, NSID 1 %lu x %u-byte blocks, SART %u protected/%u free\n",
           (unsigned long)st.nsze, 1u << st.lbads, st.sart_protected, st.sart_free);
    return true;
}

bool ans_ro_read_block(u64 lba, void *out)
{
    if (state != ANS_RO_LIVE || !out || lba >= st.nsze)
        return false;
    struct ans_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = OP_READ;
    cmd.nsid = 1;
    cmd.prp1 = (u64)dma.bounce;
    cmd.cdw10 = (u32)lba;
    cmd.cdw11 = (u32)(lba >> 32);
    cmd.cdw12 = 0; /* one block (0-based) */
    clear_bounce();
    int rc = exec(&ioq, &cmd);
    if (rc) {
        st.read_errors++;
        return false;
    }
    dma_rmb();
    memcpy(out, dma.bounce, ANS_RO_BLOCK);
    st.reads++;
    return true;
}

void ans_ro_poll(void)
{
    if (state != ANS_RO_LIVE)
        return;
    st.polls++;
    drain();
}

bool ans_ro_shutdown(void)
{
    if (state != ANS_RO_LIVE)
        return false;
    /* Exactly the stage-1 nvme_shutdown() order. Command failures are recorded but, as in
     * stage 1, the quiesce continues; a frozen driver never reaches this point. */
    struct ans_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = OP_DELETE_SQ;
    cmd.cdw10 = 1;
    if (exec(&adminq, &cmd) == -2)
        return false;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = OP_DELETE_CQ;
    cmd.cdw10 = 1;
    if (exec(&adminq, &cmd) == -2)
        return false;
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
/* Host-only: push an arbitrary command through the single submission path. */
int ans_ro_test_submit(bool admin, u8 opcode)
{
    struct ans_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = opcode;
    cmd.nsid = 1;
    cmd.prp1 = (u64)dma.bounce;
    return exec(admin ? &adminq : &ioq, &cmd);
}
#endif

u64 ans_ro_nsze(void) { return st.nsze; }
u32 ans_ro_lbads(void) { return st.lbads; }
enum ans_ro_state ans_ro_state(void) { return state; }
const struct ans_ro_stats *ans_ro_stats(void) { return &st; }
