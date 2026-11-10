/* SPDX-License-Identifier: MIT */

#include "hv_nvme_queue.h"
#ifndef VNVME_HOST_TEST
#include "hv.h"
#else
u64 hv_ipa_to_pa(u64 ipa);
#endif
#include "string.h"

#define NVME_FID_ARBITRATION       0x01
#define NVME_FID_POWER_MANAGEMENT  0x02
#define NVME_FID_ERROR_RECOVERY    0x05
#define NVME_FID_VOLATILE_WC       0x06
#define NVME_FID_NUMBER_OF_QUEUES  0x07
#define NVME_FID_IRQ_COALESCING    0x08
#define NVME_FID_IRQ_VECTOR_CONFIG 0x09
#define NVME_FID_ASYNC_EVENT       0x0b

static void put_le16(u8 *p, u16 value)
{
    p[0] = value;
    p[1] = value >> 8;
}

static void put_le32(u8 *p, u32 value)
{
    p[0] = value;
    p[1] = value >> 8;
    p[2] = value >> 16;
    p[3] = value >> 24;
}

static void put_le64(u8 *p, u64 value)
{
    put_le32(p, value);
    put_le32(p + 4, value >> 32);
}

static void put_ascii(u8 *dest, size_t length, const char *text)
{
    memset(dest, ' ', length);
    for (size_t i = 0; i < length && text[i]; i++)
        dest[i] = text[i];
}

static void trace_event(struct vnvme_ctrl *ctrl, const struct vnvme_trace_event *event)
{
    if (ctrl->ops->trace)
        ctrl->ops->trace(ctrl->opaque, event);
}

bool vnvme_intx_can_inject(bool asserted, u32 intms, bool injected, bool gic_enabled, int free_lr)
{
    return asserted && !intms && !injected && gic_enabled && free_lr >= 0;
}

bool vnvme_intx_delivery_can_inject(const struct vnvme_intx_delivery *delivery, bool asserted,
                                    u32 intms, bool gic_enabled, int free_lr)
{
    return delivery && !delivery->assertion_notified &&
           vnvme_intx_can_inject(asserted, intms, delivery->outstanding, gic_enabled, free_lr);
}

void vnvme_intx_delivery_update_line(struct vnvme_intx_delivery *delivery, bool asserted)
{
    if (delivery && !asserted) {
        delivery->assertion_notified = false;
        delivery->owner_kick_pending = false;
    }
}

void vnvme_intx_delivery_mark_injected(struct vnvme_intx_delivery *delivery)
{
    if (delivery) {
        delivery->outstanding = true;
        delivery->assertion_notified = true;
        delivery->owner_kick_pending = false;
    }
}

void vnvme_intx_delivery_eoi(struct vnvme_intx_delivery *delivery)
{
    if (delivery)
        delivery->outstanding = false;
}

bool vnvme_intx_delivery_should_kick_owner(struct vnvme_intx_delivery *delivery, bool asserted,
                                           u32 intms, int current_cpu, int owner_cpu)
{
    if (!delivery || current_cpu == owner_cpu || !asserted || intms || delivery->outstanding ||
        delivery->assertion_notified || delivery->owner_kick_pending)
        return false;

    delivery->owner_kick_pending = true;
    return true;
}

void vnvme_intx_delivery_owner_polled(struct vnvme_intx_delivery *delivery)
{
    if (delivery)
        delivery->owner_kick_pending = false;
}

static void update_irq(struct vnvme_ctrl *ctrl)
{
    bool asserted = false;
    for (u32 i = 0; i < VNVME_MAX_QUEUES; i++) {
        struct vnvme_queue *q = &ctrl->queues[i];
        if (q->cq_valid && q->irq_enabled && q->cq_pending) {
            asserted = true;
            break;
        }
    }

    if (asserted == ctrl->irq_asserted)
        return;
    ctrl->irq_asserted = asserted;
    if (ctrl->ops->irq)
        ctrl->ops->irq(ctrl->opaque, asserted);
}

void vnvme_init(struct vnvme_ctrl *ctrl, u64 namespace_blocks, const struct vnvme_backend_ops *ops,
                void *opaque)
{
    memset(ctrl, 0, sizeof(*ctrl));
    ctrl->namespace_blocks = namespace_blocks;
    ctrl->ops = ops;
    ctrl->opaque = opaque;
}

bool vnvme_set_admin_queue(struct vnvme_ctrl *ctrl, u64 sq_addr, u64 cq_addr, u16 sq_size,
                           u16 cq_size)
{
    if (!sq_addr || !cq_addr || (sq_addr & (VNVME_PAGE_SIZE - 1)) ||
        (cq_addr & (VNVME_PAGE_SIZE - 1)) || !sq_size || !cq_size || sq_size > VNVME_MAX_QSIZE ||
        cq_size > VNVME_MAX_QSIZE)
        return false;

    memset(ctrl->queues, 0, sizeof(ctrl->queues));
    struct vnvme_queue *q = &ctrl->queues[0];
    q->sq_addr = sq_addr;
    q->cq_addr = cq_addr;
    q->sq_size = sq_size;
    q->cq_size = cq_size;
    q->cq_phase = 1;
    q->sq_valid = true;
    q->cq_valid = true;
    q->irq_enabled = true;
    ctrl->irq_asserted = false;
    return true;
}

static u8 *prp_pointer(const struct vnvme_command *cmd, size_t total, size_t offset,
                       size_t *available)
{
    if (!cmd->prp1 || offset >= total)
        return NULL;

    size_t first = VNVME_PAGE_SIZE - (cmd->prp1 & (VNVME_PAGE_SIZE - 1));
    if (first > total)
        first = total;
    if (offset < first) {
        *available = first - offset;
        return (u8 *)(uintptr_t)hv_ipa_to_pa(cmd->prp1 + offset);
    }

    offset -= first;
    size_t remaining = total - first;
    if (!cmd->prp2)
        return NULL;
    if (remaining <= VNVME_PAGE_SIZE) {
        if (cmd->prp2 & (VNVME_PAGE_SIZE - 1))
            return NULL;
        *available = remaining - offset;
        return (u8 *)(uintptr_t)hv_ipa_to_pa(cmd->prp2 + offset);
    }

    if (cmd->prp2 & (VNVME_PAGE_SIZE - 1))
        return NULL;
    u64 *list = (u64 *)(uintptr_t)hv_ipa_to_pa(cmd->prp2);
    if (!list)
        return NULL;
    size_t page = offset / VNVME_PAGE_SIZE;
    size_t in_page = offset & (VNVME_PAGE_SIZE - 1);
    if (page >= VNVME_PAGE_SIZE / sizeof(*list) || !list[page] ||
        (list[page] & (VNVME_PAGE_SIZE - 1)))
        return NULL;
    *available = VNVME_PAGE_SIZE - in_page;
    if (*available > total - first - offset)
        *available = total - first - offset;
    return (u8 *)(uintptr_t)hv_ipa_to_pa(list[page] + in_page);
}

static bool prp_copy(const struct vnvme_command *cmd, size_t total, size_t offset, void *buffer,
                     size_t length, bool to_guest)
{
    u8 *local = buffer;
    while (length) {
        size_t available;
        u8 *guest = prp_pointer(cmd, total, offset, &available);
        if (!guest)
            return false;
        size_t chunk = available < length ? available : length;
        if (to_guest)
            memcpy(guest, local, chunk);
        else
            memcpy(local, guest, chunk);
        local += chunk;
        offset += chunk;
        length -= chunk;
    }
    return true;
}

static void identify_controller(u8 *data)
{
    memset(data, 0, VNVME_PAGE_SIZE);
    put_le16(data + 0, 0x1b36);
    put_le16(data + 2, 0x1b36);
    put_ascii(data + 4, 20, "M1N1NVME000000000001");
    put_ascii(data + 24, 40, "m1n1 Apple ANS NVMe bridge");
    put_ascii(data + 64, 8, "0.1");
    data[72] = 4; /* Recommended arbitration burst. */
    data[77] = 5; /* MDTS = 2^5 4 KiB pages = 128 KiB. */
    put_le32(data + 80, 0x00010300);
    put_le16(data + 256, 0);      /* No optional admin command sets. */
    data[258] = 0;                /* One outstanding Abort command. */
    data[259] = 0;                /* One outstanding Async Event Request. */
    data[261] = 1;                /* SMART/health log is supported. */
    data[262] = 0;                /* One error log entry. */
    data[512] = 0x66;             /* Required/max SQE size = 64 bytes. */
    data[513] = 0x44;             /* Required/max CQE size = 16 bytes. */
    put_le32(data + 516, 1);      /* One namespace. */
    put_le16(data + 520, BIT(2)); /* Dataset Management supported. */
    data[525] = BIT(0);           /* Flush applies to all namespaces. */
    data[526] = 0;                /* No volatile write cache. */
}

static void identify_namespace(struct vnvme_ctrl *ctrl, u8 *data)
{
    memset(data, 0, VNVME_PAGE_SIZE);
    put_le64(data + 0, ctrl->namespace_blocks);
    put_le64(data + 8, ctrl->namespace_blocks);
    put_le64(data + 16, ctrl->namespace_blocks);
    data[25] = 0;       /* One LBA format. */
    data[26] = 0;       /* LBA format zero is active. */
    data[128 + 2] = 12; /* 2^12 = 4096-byte logical blocks. */
}

static u16 identify(struct vnvme_ctrl *ctrl, const struct vnvme_command *cmd)
{
    u8 cns = cmd->cdw10 & 0xff;
    switch (cns) {
        case VNVME_IDENTIFY_CONTROLLER:
            identify_controller(ctrl->bounce);
            break;
        case VNVME_IDENTIFY_NAMESPACE:
            if (cmd->nsid != 1)
                return VNVME_SC_INVALID_NAMESPACE;
            identify_namespace(ctrl, ctrl->bounce);
            break;
        case VNVME_IDENTIFY_ACTIVE_NS_LIST:
            memset(ctrl->bounce, 0, VNVME_PAGE_SIZE);
            if (cmd->nsid < 1)
                put_le32(ctrl->bounce, 1);
            break;
        case VNVME_IDENTIFY_NS_ID_DESCRIPTOR_LIST:
            if (cmd->nsid != 1)
                return VNVME_SC_INVALID_NAMESPACE;
            memset(ctrl->bounce, 0, VNVME_PAGE_SIZE);
            break;
        default:
            return VNVME_SC_INVALID_FIELD;
    }

    if (!prp_copy(cmd, VNVME_PAGE_SIZE, 0, ctrl->bounce, VNVME_PAGE_SIZE, true))
        return VNVME_SC_DATA_TRANSFER_ERROR;
    return VNVME_SC_SUCCESS;
}

static u16 get_log_page(struct vnvme_ctrl *ctrl, const struct vnvme_command *cmd)
{
    u8 lid = cmd->cdw10 & 0xff;
    u32 numd = ((cmd->cdw11 & 0xffff) << 16) | (cmd->cdw10 >> 16);
    size_t length = ((size_t)numd + 1) * 4;
    if (length > VNVME_PAGE_SIZE || (lid != 1 && lid != 2))
        return VNVME_SC_INVALID_FIELD;
    memset(ctrl->bounce, 0, length);
    if (!prp_copy(cmd, length, 0, ctrl->bounce, length, true))
        return VNVME_SC_DATA_TRANSFER_ERROR;
    return VNVME_SC_SUCCESS;
}

static u16 create_cq(struct vnvme_ctrl *ctrl, const struct vnvme_command *cmd)
{
    u16 qid = cmd->cdw10 & 0xffff;
    u16 size = (cmd->cdw10 >> 16) + 1;
    if (qid != 1 || size < 2 || size > VNVME_MAX_QSIZE || !(cmd->cdw11 & BIT(0)) || !cmd->prp1 ||
        (cmd->prp1 & (VNVME_PAGE_SIZE - 1)))
        return VNVME_SC_INVALID_FIELD;
    struct vnvme_queue *q = &ctrl->queues[qid];
    if (q->cq_valid)
        return VNVME_SC_INVALID_FIELD;
    q->cq_addr = cmd->prp1;
    q->cq_size = size;
    q->cq_phase = 1;
    q->irq_enabled = cmd->cdw11 & BIT(1);
    q->cq_valid = true;
    return VNVME_SC_SUCCESS;
}

static u16 create_sq(struct vnvme_ctrl *ctrl, const struct vnvme_command *cmd)
{
    u16 qid = cmd->cdw10 & 0xffff;
    u16 size = (cmd->cdw10 >> 16) + 1;
    u16 cqid = cmd->cdw11 >> 16;
    if (qid != 1 || cqid != 1 || size < 2 || size > VNVME_MAX_QSIZE || !(cmd->cdw11 & BIT(0)) ||
        !cmd->prp1 || (cmd->prp1 & (VNVME_PAGE_SIZE - 1)) || !ctrl->queues[cqid].cq_valid)
        return VNVME_SC_INVALID_FIELD;
    struct vnvme_queue *q = &ctrl->queues[qid];
    if (q->sq_valid)
        return VNVME_SC_INVALID_FIELD;
    q->sq_addr = cmd->prp1;
    q->sq_size = size;
    q->sq_head = 0;
    q->cq_id = cqid;
    q->sq_valid = true;
    return VNVME_SC_SUCCESS;
}

static u16 feature(const struct vnvme_command *cmd, u32 *result)
{
    switch (cmd->cdw10 & 0xff) {
        case NVME_FID_ARBITRATION:
        case NVME_FID_POWER_MANAGEMENT:
        case NVME_FID_ERROR_RECOVERY:
        case NVME_FID_VOLATILE_WC:
        case NVME_FID_IRQ_COALESCING:
        case NVME_FID_ASYNC_EVENT:
            *result = 0;
            return VNVME_SC_SUCCESS;
        case NVME_FID_NUMBER_OF_QUEUES:
            *result = 0; /* Zero-based: one submission and one completion queue. */
            return VNVME_SC_SUCCESS;
        case NVME_FID_IRQ_VECTOR_CONFIG:
            *result = cmd->cdw11 & 0xffff;
            return VNVME_SC_SUCCESS;
        default:
            return VNVME_SC_INVALID_FIELD;
    }
}

static u16 process_admin(struct vnvme_ctrl *ctrl, const struct vnvme_command *cmd, u32 *result,
                         bool *complete)
{
    switch (cmd->opcode) {
        case VNVME_ADMIN_IDENTIFY:
            return identify(ctrl, cmd);
        case VNVME_ADMIN_GET_LOG_PAGE:
            return get_log_page(ctrl, cmd);
        case VNVME_ADMIN_CREATE_CQ:
            return create_cq(ctrl, cmd);
        case VNVME_ADMIN_CREATE_SQ:
            return create_sq(ctrl, cmd);
        case VNVME_ADMIN_DELETE_SQ:
            if ((cmd->cdw10 & 0xffff) != 1 || !ctrl->queues[1].sq_valid)
                return VNVME_SC_INVALID_FIELD;
            ctrl->queues[1].sq_valid = false;
            return VNVME_SC_SUCCESS;
        case VNVME_ADMIN_DELETE_CQ:
            if ((cmd->cdw10 & 0xffff) != 1 || ctrl->queues[1].sq_valid || !ctrl->queues[1].cq_valid)
                return VNVME_SC_INVALID_FIELD;
            memset(&ctrl->queues[1], 0, sizeof(ctrl->queues[1]));
            return VNVME_SC_SUCCESS;
        case VNVME_ADMIN_SET_FEATURES:
        case VNVME_ADMIN_GET_FEATURES:
            return feature(cmd, result);
        case VNVME_ADMIN_ABORT:
            *result = 1; /* No matching outstanding command was found. */
            return VNVME_SC_SUCCESS;
        case VNVME_ADMIN_ASYNC_EVENT:
            *complete = false;
            return VNVME_SC_SUCCESS;
        default:
            return VNVME_SC_INVALID_OPCODE;
    }
}

static u16 process_io(struct vnvme_ctrl *ctrl, const struct vnvme_command *cmd)
{
    if (cmd->nsid != 1)
        return VNVME_SC_INVALID_NAMESPACE;

    if (cmd->opcode == VNVME_IO_FLUSH)
        return ctrl->ops->flush(ctrl->opaque) ? VNVME_SC_SUCCESS : VNVME_SC_INTERNAL;
    if (cmd->opcode == VNVME_IO_DATASET_MANAGEMENT)
        return VNVME_SC_SUCCESS;
    if (cmd->opcode != VNVME_IO_READ && cmd->opcode != VNVME_IO_WRITE)
        return VNVME_SC_INVALID_OPCODE;

    u32 blocks = (cmd->cdw12 & 0xffff) + 1;
    if (blocks > VNVME_MAX_BLOCKS)
        return VNVME_SC_INVALID_FIELD;
    u64 lba = ((u64)cmd->cdw11 << 32) | cmd->cdw10;
    if (lba >= ctrl->namespace_blocks || blocks > ctrl->namespace_blocks - lba)
        return VNVME_SC_INVALID_FIELD;
    size_t total = (size_t)blocks * VNVME_LBA_SIZE;

    if (cmd->opcode == VNVME_IO_WRITE && ctrl->ops->write_blocks) {
        if (!prp_copy(cmd, total, 0, ctrl->bounce, total, false))
            return VNVME_SC_DATA_TRANSFER_ERROR;
        return ctrl->ops->write_blocks(ctrl->opaque, lba, blocks, ctrl->bounce)
                   ? VNVME_SC_SUCCESS
                   : VNVME_SC_INTERNAL;
    }
    if (cmd->opcode == VNVME_IO_READ && ctrl->ops->read_blocks) {
        if (!ctrl->ops->read_blocks(ctrl->opaque, lba, blocks, ctrl->bounce))
            return VNVME_SC_INTERNAL;
        return prp_copy(cmd, total, 0, ctrl->bounce, total, true) ? VNVME_SC_SUCCESS
                                                                 : VNVME_SC_DATA_TRANSFER_ERROR;
    }

    /* Compatibility path for simple test or non-batching backends. */
    for (u32 i = 0; i < blocks; i++) {
        size_t offset = (size_t)i * VNVME_LBA_SIZE;
        if (cmd->opcode == VNVME_IO_WRITE) {
            if (!prp_copy(cmd, total, offset, ctrl->bounce, VNVME_LBA_SIZE, false))
                return VNVME_SC_DATA_TRANSFER_ERROR;
            if (!ctrl->ops->write(ctrl->opaque, lba + i, ctrl->bounce))
                return VNVME_SC_INTERNAL;
        } else {
            if (!ctrl->ops->read(ctrl->opaque, lba + i, ctrl->bounce))
                return VNVME_SC_INTERNAL;
            if (!prp_copy(cmd, total, offset, ctrl->bounce, VNVME_LBA_SIZE, true))
                return VNVME_SC_DATA_TRANSFER_ERROR;
        }
    }
    return VNVME_SC_SUCCESS;
}

static bool post_completion(struct vnvme_ctrl *ctrl, u16 cqid, u16 sqid, u16 sq_head, u16 cid,
                            u32 result, u16 status_code)
{
    struct vnvme_queue *cq = &ctrl->queues[cqid];
    if (!cq->cq_valid || !cq->cq_size || cq->cq_pending == cq->cq_size)
        return false;
    u16 slot = cq->cq_tail;
    u8 phase = cq->cq_phase;
    u16 next = cq->cq_tail + 1;
    if (next == cq->cq_size)
        next = 0;

    struct vnvme_completion cqe = {
        .result = result,
        .sq_head = sq_head,
        .sq_id = sqid,
        .cid = cid,
        .status = (status_code << 1) | cq->cq_phase,
    };
    void *cqe_dst = (void *)(uintptr_t)hv_ipa_to_pa(cq->cq_addr + cq->cq_tail * sizeof(cqe));
    if (!cqe_dst)
        return false;
    memcpy(cqe_dst, &cqe, sizeof(cqe));
    cq->cq_tail = next;
    cq->cq_pending++;
    if (!next)
        cq->cq_phase ^= 1;
    if (ctrl->ops->publish)
        ctrl->ops->publish(ctrl->opaque);
    update_irq(ctrl);
    struct vnvme_trace_event event = {
        .type = VNVME_TRACE_COMPLETION,
        .qid = sqid,
        .cqid = cqid,
        .slot = slot,
        .head = sq_head,
        .tail = next,
        .cid = cid,
        .status = status_code,
        .phase = phase,
        .irq_asserted = ctrl->irq_asserted,
    };
    trace_event(ctrl, &event);
    ctrl->stats.completions++;
    return true;
}

/*
 * Execute pending submissions until exactly one completion has been published. A command
 * such as AER may deliberately remain outstanding without a CQE, so it does not consume the
 * completion budget and the following submission may still be considered.
 *
 * The old queue path drained every entry named by one SQ doorbell while still inside the MMIO
 * exception. A 256-entry Windows batch could therefore perform thousands of synchronous ANS
 * block operations before EL1 got a chance to acknowledge even the first CQE. Keeping one
 * unconsumed CQE per queue provides backpressure and returns to the guest after each command.
 */
static bool process_until_completion(struct vnvme_ctrl *ctrl, u16 qid)
{
    struct vnvme_queue *sq = &ctrl->queues[qid];
    u16 cqid = qid ? sq->cq_id : 0;
    if (!sq->sq_valid || cqid >= VNVME_MAX_QUEUES)
        return false;
    struct vnvme_queue *cq = &ctrl->queues[cqid];
    if (!cq->cq_valid)
        return false;

    /* Wait until EL1 consumes the previous CQE before doing more synchronous backend work. */
    if (cq->cq_pending)
        return true;

    u16 walked = 0;
    while (sq->sq_head != sq->sq_tail) {
        u16 slot = sq->sq_head;
        struct vnvme_command cmd;
        void *sqe_src = (void *)(uintptr_t)hv_ipa_to_pa(sq->sq_addr + sq->sq_head * sizeof(cmd));
        if (!sqe_src)
            return false; // NOT break: breaking here returns true, so the doorbell would be
                          // swallowed with no completion and the guest would wait forever.
        memcpy(&cmd, sqe_src, sizeof(cmd));
        sq->sq_head++;
        if (sq->sq_head == sq->sq_size)
            sq->sq_head = 0;
        if (++walked > sq->sq_size)
            return false;
        ctrl->stats.commands++;

        u32 result = 0;
        bool complete = true;
        u16 status = qid ? process_io(ctrl, &cmd) : process_admin(ctrl, &cmd, &result, &complete);
        struct vnvme_trace_event event = {
            .type = VNVME_TRACE_SUBMISSION,
            .qid = qid,
            .cqid = cqid,
            .slot = slot,
            .head = sq->sq_head,
            .tail = sq->sq_tail,
            .cid = cmd.cid,
            .status = status,
            .opcode = cmd.opcode,
            .irq_asserted = ctrl->irq_asserted,
        };
        trace_event(ctrl, &event);
        if (complete) {
            if (!post_completion(ctrl, cqid, qid, sq->sq_head, cmd.cid, result, status))
                return false;
            return true;
        }
    }
    return true;
}

bool vnvme_sq_doorbell(struct vnvme_ctrl *ctrl, u16 qid, u16 new_tail)
{
    if (qid >= VNVME_MAX_QUEUES)
        return false;
    struct vnvme_queue *sq = &ctrl->queues[qid];
    if (!sq->sq_valid || new_tail >= sq->sq_size)
        return false;
    ctrl->stats.sq_doorbells++;
    sq->sq_tail = new_tail;
    return process_until_completion(ctrl, qid);
}

bool vnvme_cq_doorbell(struct vnvme_ctrl *ctrl, u16 qid, u16 new_head)
{
    if (qid >= VNVME_MAX_QUEUES || !ctrl->queues[qid].cq_valid ||
        new_head >= ctrl->queues[qid].cq_size)
        return false;
    struct vnvme_queue *cq = &ctrl->queues[qid];
    u16 consumed = (new_head + cq->cq_size - cq->cq_head) % cq->cq_size;
    if (!consumed && new_head == cq->cq_head && cq->cq_pending == cq->cq_size)
        consumed = cq->cq_size;
    if (consumed > cq->cq_pending)
        return false;
    ctrl->stats.cq_doorbells++;
    cq->cq_head = new_head;
    cq->cq_pending -= consumed;
    update_irq(ctrl);

    if (cq->cq_pending)
        return true;

    /* CQ0 serves admin SQ0; each I/O SQ records the CQ it was created against. */
    if (qid == 0)
        return process_until_completion(ctrl, 0);
    for (u16 sqid = 1; sqid < VNVME_MAX_QUEUES; sqid++) {
        struct vnvme_queue *sq = &ctrl->queues[sqid];
        if (sq->sq_valid && sq->cq_id == qid && sq->sq_head != sq->sq_tail)
            return process_until_completion(ctrl, sqid);
    }
    return true;
}

void vnvme_get_snapshot(const struct vnvme_ctrl *ctrl, struct vnvme_snapshot *out)
{
    if (!out)
        return;

    memset(out, 0, sizeof(*out));
    if (!ctrl)
        return;

    out->stats = ctrl->stats;
    out->irq_asserted = ctrl->irq_asserted;
    for (u32 i = 0; i < VNVME_MAX_QUEUES; i++) {
        out->queues[i] = (struct vnvme_queue_state){
            .sq_head = ctrl->queues[i].sq_head,
            .sq_tail = ctrl->queues[i].sq_tail,
            .cq_head = ctrl->queues[i].cq_head,
            .cq_tail = ctrl->queues[i].cq_tail,
        };
    }
}
