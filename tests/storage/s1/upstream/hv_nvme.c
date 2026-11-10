/* SPDX-License-Identifier: MIT */

// Standards-facing NVMe controller for the Windows/UEFI guest. The guest queue engine is
// kept in hv_nvme_queue.c so its command and PRP semantics can be tested on a host. This
// file owns the BAR register model, Apple ANS backend connection, and virtual INTx line.

#include "hv.h"
#include "hv_runtime_diag.h"
#include "hv_nvme_fast_path.h"
#include "hv_nvme_queue.h"
#include "hv_vgic.h"
#include "nvme.h"
#include "smp.h"
#include "string.h"
#include "types.h"
#include "utils.h"

#define NVME_CAP   0x00
#define NVME_VS    0x08
#define NVME_INTMS 0x0c
#define NVME_INTMC 0x10
#define NVME_CC    0x14
#define NVME_CSTS  0x1c
#define NVME_AQA   0x24
#define NVME_ASQ   0x28
#define NVME_ACQ   0x30
#define NVME_DBS   0x1000

#define CC_EN         BIT(0)
#define CC_CSS        GENMASK(6, 4)
#define CC_MPS        GENMASK(10, 7)
#define CC_SHN        GENMASK(15, 14)
#define CC_IOSQES     GENMASK(19, 16)
#define CC_IOCQES     GENMASK(23, 20)
#define CC_SHN_NORMAL FIELD_PREP(CC_SHN, 1)

#define CSTS_RDY           BIT(0)
#define CSTS_CFS           BIT(1)
#define CSTS_SHST          GENMASK(3, 2)
#define CSTS_SHST_COMPLETE FIELD_PREP(CSTS_SHST, 2)

// 256-entry queues, contiguous queues required, five-second ready timeout, 4 KiB pages,
// and the NVM command set. DSTRD is zero, so doorbells have a four-byte stride.
#define NVME_CAP_VALUE    ((VNVME_MAX_QSIZE - 1ULL) | BIT(16) | (10ULL << 24) | (1ULL << 37))
#define NVME_VS_VALUE     0x00010300
#define NVME_BAR_SIZE     0x1000000
#define NVME_TRACE_BUDGET 64

extern int hv_pci_intx_irq(void);

static u64 bar_base;
static DECLARE_SPINLOCK(nvme_lock);
static bool backend_ready;
static u64 backend_blocks;
static struct vnvme_intx_delivery irq_delivery;
static u32 nvme_trace_budget;
static struct vnvme_ctrl queue_ctrl;
static bool irq_eoi_pending;
static u64 irq_owner_kicks;
static u64 irq_owner_polls;

static struct {
    u32 intms;
    u32 cc;
    u32 csts;
    u32 aqa;
    u64 asq;
    u64 acq;
} regs;

static bool nvme_trace_take(void)
{
    if (!hv_runtime_diag_enabled())
        return false;
    if (!nvme_trace_budget)
        return false;
    nvme_trace_budget--;
    return true;
}

static bool backend_read(void *opaque, u64 lba, void *buffer)
{
    UNUSED(opaque);
    return nvme_read(1, lba, buffer);
}

static bool backend_write(void *opaque, u64 lba, const void *buffer)
{
    UNUSED(opaque);
    return nvme_write(1, lba, buffer);
}

static bool backend_read_blocks(void *opaque, u64 lba, u32 blocks, void *buffer)
{
    UNUSED(opaque);
    return nvme_read_blocks(1, lba, blocks, buffer);
}

static bool backend_write_blocks(void *opaque, u64 lba, u32 blocks, const void *buffer)
{
    UNUSED(opaque);
    return nvme_write_blocks(1, lba, blocks, buffer);
}

static bool backend_flush(void *opaque)
{
    UNUSED(opaque);
    return nvme_flush(1);
}

static void backend_publish(void *opaque)
{
    UNUSED(opaque);
    dma_wmb();
}

static void try_raise_intx(void)
{
    /* Until vGIC affinity routing is implemented, keep this INTx and its LR on CPU0. */
    int current_cpu = smp_id();
    if (current_cpu != boot_cpu_idx) {
        /*
         * The CQ doorbell that publishes the next synchronous completion can trap on any
         * guest CPU.  The synthetic INTx LR, however, belongs to the boot vCPU.  Wake that
         * owner explicitly instead of waiting for an unrelated CPU0 exit from the guest;
         * otherwise a completed read can sit unnoticed until stornvme's ten-second reset.
         */
        if (vnvme_intx_delivery_should_kick_owner(&irq_delivery, queue_ctrl.irq_asserted,
                                                   regs.intms, current_cpu, boot_cpu_idx)) {
            irq_owner_kicks++;
            if (hv_runtime_diag_verbose_enabled() && nvme_trace_take())
                HV_RUNTIME_VERBOSE_TRACE(
                    "HV: NVMe INTx owner kick source=%d owner=%d count=%lu\n", current_cpu,
                    boot_cpu_idx, irq_owner_kicks);
            smp_send_ipi(boot_cpu_idx);
        }
        return;
    }

    vnvme_intx_delivery_owner_polled(&irq_delivery);
    irq_owner_polls++;

    int irq = hv_pci_intx_irq();
    bool enabled = hv_vgic3_irq_enabled(irq);
    int free_lr = hv_vgic3_get_free_lr();
    if (!vnvme_intx_delivery_can_inject(&irq_delivery, queue_ctrl.irq_asserted, regs.intms,
                                        enabled, free_lr)) {
        if (queue_ctrl.irq_asserted && !regs.intms && !irq_delivery.outstanding && !enabled) {
            static u32 disabled_logs;
            if (disabled_logs++ < 8)
                printf("HV: NVMe INTx %d pending: GICD disabled\n", irq);
        } else if (queue_ctrl.irq_asserted && !regs.intms && !irq_delivery.outstanding &&
                   free_lr < 0) {
            static u32 no_lr_logs;
            if (no_lr_logs++ < 8)
                printf("HV: NVMe INTx %d delayed: no free vGIC LR\n", irq);
        }
        return;
    }

    hv_vgic3_inject_irq(irq, hv_vgic3_get_priority(irq), false, true, false, 0);
    vnvme_intx_delivery_mark_injected(&irq_delivery);
}

static void drain_deferred_eoi(void)
{
    if (__atomic_exchange_n(&irq_eoi_pending, false, __ATOMIC_ACQ_REL))
        vnvme_intx_delivery_eoi(&irq_delivery);
}

bool hv_nvme_try_handle_dabort(struct exc_info *ctx, bool *matched)
{
    bool is_write = ctx->esr & ESR_ISS_DABORT_WnR;
    u64 ipa = hv_translate(ctx->far, true, is_write, NULL);
    u64 base = __atomic_load_n(&bar_base, __ATOMIC_ACQUIRE);

    if (matched)
        *matched = false;
    if (!ipa || !hv_nvme_bar_contains(base, NVME_BAR_SIZE, ipa))
        return false;

    /*
     * The queue engine performs synchronous ANS I/O.  Serializing that work
     * with the hypervisor-wide bhl stopped every vCPU during a busy doorbell,
     * eventually tripping Windows' clock watchdog.  NVMe state needs
     * serialization, but unrelated CPU timers and IPIs do not.
     */
    spin_lock(&nvme_lock);
    base = __atomic_load_n(&bar_base, __ATOMIC_ACQUIRE);
    if (!hv_nvme_bar_contains(base, NVME_BAR_SIZE, ipa)) {
        spin_unlock(&nvme_lock);
        return false;
    }

    if (matched)
        *matched = true;
    bool handled = hv_handle_dabort(ctx);
    drain_deferred_eoi();
    try_raise_intx();
    spin_unlock(&nvme_lock);
    return handled;
}

void hv_nvme_poll_irq(void)
{
    /* Never make a bhl owner wait behind synchronous storage I/O. */
    if (!spin_try_lock(&nvme_lock))
        return;
    drain_deferred_eoi();
    try_raise_intx();
    spin_unlock(&nvme_lock);
}

void hv_nvme_irq_eoi(u32 intid)
{
    if (intid != (u32)hv_pci_intx_irq())
        return;
    if (!spin_try_lock(&nvme_lock)) {
        __atomic_store_n(&irq_eoi_pending, true, __ATOMIC_RELEASE);
        return;
    }
    vnvme_intx_delivery_eoi(&irq_delivery);
    try_raise_intx();
    spin_unlock(&nvme_lock);
}

void hv_nvme_get_diag_snapshot(struct vnvme_snapshot *out, bool *ready)
{
    if (!spin_try_lock(&nvme_lock)) {
        if (ready)
            *ready = false;
        return;
    }
    vnvme_get_snapshot(&queue_ctrl, out);
    if (ready)
        *ready = backend_ready && (regs.csts & CSTS_RDY);
    spin_unlock(&nvme_lock);
}

static void backend_irq(void *opaque, bool asserted)
{
    UNUSED(opaque);
    /* Track the effective wire level, including the controller's INTM mask. */
    vnvme_intx_delivery_update_line(&irq_delivery, asserted && !regs.intms);
    if (hv_runtime_diag_verbose_enabled() && nvme_trace_take())
        HV_RUNTIME_VERBOSE_TRACE("HV: NVMe INTx logical=%d masked=0x%x injected=%d\n", asserted,
                                 regs.intms, irq_delivery.outstanding);
    try_raise_intx();
}

static void backend_trace(void *opaque, const struct vnvme_trace_event *event)
{
    UNUSED(opaque);
    if (!hv_runtime_diag_verbose_enabled() || !nvme_trace_take())
        return;

    if (event->type == VNVME_TRACE_SUBMISSION) {
        printf("HV: NVMe SQ q=%u cq=%u slot=%u head=%u tail=%u opc=0x%x cid=0x%x status=0x%x "
               "irq=%d\n",
               event->qid, event->cqid, event->slot, event->head, event->tail, event->opcode,
               event->cid, event->status, event->irq_asserted);
    } else {
        printf("HV: NVMe CQE cq=%u sq=%u slot=%u head=%u tail=%u phase=%u cid=0x%x "
               "status=0x%x irq=%d\n",
               event->cqid, event->qid, event->slot, event->head, event->tail, event->phase,
               event->cid, event->status, event->irq_asserted);
    }
}

static const struct vnvme_backend_ops backend_ops = {
    .read = backend_read,
    .write = backend_write,
    .read_blocks = backend_read_blocks,
    .write_blocks = backend_write_blocks,
    .flush = backend_flush,
    .publish = backend_publish,
    .irq = backend_irq,
    .trace = backend_trace,
};

bool hv_nvme_init_backend(void)
{
    if (backend_ready)
        return true;
    if (!nvme_init()) {
        printf("HV: NVMe backend: Apple ANS initialization failed\n");
        return false;
    }

    u32 lba_size = 0;
    if (!nvme_get_namespace_info(1, &backend_blocks, &lba_size)) {
        printf("HV: NVMe backend: Identify Namespace 1 failed\n");
        return false;
    }
    if (lba_size != VNVME_LBA_SIZE) {
        printf("HV: NVMe backend: unsupported physical LBA size %u (expected %u)\n", lba_size,
               VNVME_LBA_SIZE);
        return false;
    }

    backend_ready = true;
    printf("HV: NVMe backend: namespace 1, %lu x %u-byte blocks (%lu MiB)\n", backend_blocks,
           lba_size, backend_blocks / 256);
    return true;
}

static void reset_frontend(void)
{
    memset(&regs, 0, sizeof(regs));
    irq_delivery = (struct vnvme_intx_delivery){0};
    irq_eoi_pending = false;
    nvme_trace_budget = NVME_TRACE_BUDGET;
    vnvme_init(&queue_ctrl, backend_blocks, &backend_ops, NULL);
    hv_vgic3_trace_intid(hv_pci_intx_irq(), NVME_TRACE_BUDGET);
}

static bool valid_cc(u32 cc)
{
    return !FIELD_GET(CC_CSS, cc) && !FIELD_GET(CC_MPS, cc) && FIELD_GET(CC_IOSQES, cc) == 6 &&
           FIELD_GET(CC_IOCQES, cc) == 4;
}

static void cc_write(u32 value)
{
    u32 old = regs.cc;
    regs.cc = value;

    if ((value & CC_EN) && !(old & CC_EN)) {
        u16 asq_size = (regs.aqa & 0xfff) + 1;
        u16 acq_size = ((regs.aqa >> 16) & 0xfff) + 1;
        if (nvme_trace_take())
            printf("HV: NVMe enable CC=0x%x ASQS=%u ACQS=%u max=%u backend=%d\n", value, asq_size,
                   acq_size, VNVME_MAX_QSIZE, backend_ready);
        if (!backend_ready || !valid_cc(value) ||
            !vnvme_set_admin_queue(&queue_ctrl, regs.asq, regs.acq, asq_size, acq_size)) {
            regs.csts = CSTS_CFS;
            printf("HV: NVMe enable rejected: backend=%d CC=0x%x AQA=0x%x ASQ=0x%lx ACQ=0x%lx\n",
                   backend_ready, value, regs.aqa, regs.asq, regs.acq);
            return;
        }
        regs.csts = CSTS_RDY;
        printf("HV: NVMe ready: AQA=0x%x ASQ=0x%lx ACQ=0x%lx\n", regs.aqa, regs.asq, regs.acq);
    } else if (!(value & CC_EN) && (old & CC_EN)) {
        vnvme_init(&queue_ctrl, backend_blocks, &backend_ops, NULL);
        irq_delivery = (struct vnvme_intx_delivery){0};
        regs.csts = 0;
    }

    if ((value & CC_SHN) == CC_SHN_NORMAL)
        regs.csts = (regs.csts & ~CSTS_SHST) | CSTS_SHST_COMPLETE;
    else if (!(value & CC_SHN))
        regs.csts &= ~CSTS_SHST;
}

static u64 register_value(u32 off)
{
    switch (off) {
        case NVME_CAP:
            return NVME_CAP_VALUE;
        case NVME_VS:
            return NVME_VS_VALUE;
        case NVME_INTMS:
        case NVME_INTMC:
            return regs.intms;
        case NVME_CC:
            return regs.cc;
        case NVME_CSTS:
            return regs.csts;
        case NVME_AQA:
            return regs.aqa;
        case NVME_ASQ:
            return regs.asq;
        case NVME_ACQ:
            return regs.acq;
        default:
            return 0;
    }
}

static u64 reg_read(u32 off, int width)
{
    u32 base = off & ~3u;
    if ((off & ~7u) == NVME_CAP || (off & ~7u) == NVME_ASQ || (off & ~7u) == NVME_ACQ)
        base = off & ~7u;
    u64 value = register_value(base);
    u32 byte_offset = off - base;
    u32 bytes = 1u << width;
    if (bytes >= 8)
        return value;
    u64 mask = (1ULL << (bytes * 8)) - 1;
    return (value >> (byte_offset * 8)) & mask;
}

static u64 merge_write(u64 old, u32 byte_offset, int width, u64 value)
{
    u32 bytes = 1u << width;
    if (bytes >= 8)
        return value;
    u64 mask = ((1ULL << (bytes * 8)) - 1) << (byte_offset * 8);
    return (old & ~mask) | ((value << (byte_offset * 8)) & mask);
}

static void reg_write(u32 off, int width, u64 value)
{
    if ((off & ~7u) == NVME_ASQ) {
        regs.asq = merge_write(regs.asq, off - NVME_ASQ, width, value);
        return;
    }
    if ((off & ~7u) == NVME_ACQ) {
        regs.acq = merge_write(regs.acq, off - NVME_ACQ, width, value);
        return;
    }

    u32 base = off & ~3u;
    u32 byte_offset = off - base;
    u32 merged = merge_write(register_value(base), byte_offset, width, value);
    switch (base) {
        case NVME_INTMS:
            if (hv_runtime_diag_verbose_enabled() && nvme_trace_take())
                HV_RUNTIME_VERBOSE_TRACE("HV: NVMe INTMS old=0x%x set=0x%x new=0x%x\n",
                                         regs.intms, merged, regs.intms | merged);
            regs.intms |= merged;
            vnvme_intx_delivery_update_line(&irq_delivery,
                                             queue_ctrl.irq_asserted && !regs.intms);
            break;
        case NVME_INTMC:
            if (hv_runtime_diag_verbose_enabled() && nvme_trace_take())
                HV_RUNTIME_VERBOSE_TRACE("HV: NVMe INTMC old=0x%x clear=0x%x new=0x%x\n",
                                         regs.intms, merged, regs.intms & ~merged);
            regs.intms &= ~merged;
            vnvme_intx_delivery_update_line(&irq_delivery,
                                             queue_ctrl.irq_asserted && !regs.intms);
            try_raise_intx();
            break;
        case NVME_CC:
            cc_write(merged);
            break;
        case NVME_AQA:
            regs.aqa = merged;
            break;
        default:
            break;
    }
}

static bool doorbell_write(u32 index, u32 value)
{
    if (!(regs.csts & CSTS_RDY))
        return true;
    u16 qid = index / 2;
    if (hv_runtime_diag_verbose_enabled() && nvme_trace_take())
        HV_RUNTIME_VERBOSE_TRACE(
            "HV: NVMe doorbell q=%u %s=%u irq=%d injected=%d masked=0x%x\n", qid,
            (index & 1) ? "CQH" : "SQT", value, queue_ctrl.irq_asserted,
            irq_delivery.outstanding, regs.intms);
    bool ok;
    if (index & 1) {
        ok = vnvme_cq_doorbell(&queue_ctrl, qid, value);
    } else {
        dma_rmb();
        ok = vnvme_sq_doorbell(&queue_ctrl, qid, value);
        try_raise_intx();
    }
    if (!ok)
        printf("HV: NVMe invalid doorbell q=%u %s=%u\n", qid, (index & 1) ? "CQH" : "SQT", value);
    return true;
}

static bool handle_nvme_bar(struct exc_info *ctx, u64 addr, u64 *value, bool write, int width)
{
    UNUSED(ctx);
    u32 off = addr - bar_base;
    if (off >= NVME_DBS) {
        u32 index = (off - NVME_DBS) / 4;
        if (write)
            return doorbell_write(index, *value);
        *value = 0;
        return true;
    }

    if (write)
        reg_write(off, width, *value);
    else
        *value = reg_read(off, width);
    return true;
}

void hv_nvme_map_bar(u64 base)
{
    if (bar_base && bar_base != base)
        hv_unmap(bar_base, NVME_BAR_SIZE);
    __atomic_store_n(&bar_base, base, __ATOMIC_RELEASE);
    reset_frontend();
    hv_map_hook(base, handle_nvme_bar, NVME_BAR_SIZE);
    printf("HV: NVMe BAR0 MMIO live at 0x%lx (backend=%d)\n", base, backend_ready);
}

void hv_nvme_unmap_bar(void)
{
    if (!bar_base)
        return;
    hv_unmap(bar_base, NVME_BAR_SIZE);
    __atomic_store_n(&bar_base, 0, __ATOMIC_RELEASE);
    reset_frontend();
}
