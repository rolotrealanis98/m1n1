/* SPDX-License-Identifier: MIT */

#include "hv_nvme_queue.h"
#include "string.h"

#define NVME_FID_ARBITRATION       0x01
#define NVME_FID_POWER_MANAGEMENT  0x02
#define NVME_FID_ERROR_RECOVERY    0x05
#define NVME_FID_VOLATILE_WC       0x06
#define NVME_FID_NUMBER_OF_QUEUES  0x07
#define NVME_FID_IRQ_COALESCING    0x08
#define NVME_FID_IRQ_VECTOR_CONFIG 0x09
#define NVME_FID_ASYNC_EVENT       0x0b

/* S0 adaptation: all guest accesses require a full permission-checked span. */
static void *guest_span(struct vnvme_ctrl *ctrl, u64 addr, size_t length, bool write)
{
    if (!addr || !length || length > UINT64_MAX - addr || !ctrl->ops->guest_span)
        return NULL;
    return ctrl->ops->guest_span(ctrl->opaque, addr, length, write);
}

/* S0 requires disjoint guest-IPA intervals. A live mapper must additionally
 * prevent distinct IPAs aliasing the same backing bytes and pin permissions. */
static bool overlaps(u64 a, size_t an, u64 b, size_t bn)
{
    if (!an || !bn || an > UINT64_MAX - a || bn > UINT64_MAX - b)
        return true; /* invalid spans fail closed */
    return a <= b ? b - a < an : a - b < bn;
}

static bool overlaps_queue(struct vnvme_ctrl *ctrl, u64 addr, size_t length)
{
    for (size_t i = 0; i < VNVME_MAX_QUEUES; i++) {
        const struct vnvme_queue *q = &ctrl->queues[i];
        if ((q->sq_valid && overlaps(addr, length, q->sq_addr,
                                     (size_t)q->sq_size * sizeof(struct vnvme_command))) ||
            (q->cq_valid && overlaps(addr, length, q->cq_addr,
                                     (size_t)q->cq_size * sizeof(struct vnvme_completion))))
            return true;
    }
    return false;
}

static void trace_event(struct vnvme_ctrl *ctrl, const struct vnvme_trace_event *event);
/* r2.1: latch fatal with its first reason and trace it (no guest access). */
static void set_fatal(struct vnvme_ctrl *ctrl, u8 reason)
{
    if (!ctrl->fatal) {
        ctrl->fatal_reason = reason;
        struct vnvme_trace_event event = { .type = VNVME_TRACE_FATAL, .status = reason };
        trace_event(ctrl, &event);
    }
    ctrl->fatal = true;
}

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
    ctrl->temp_over = VNVME_WCTEMP;
    /* r4: defaults are the r2.2 fixture behaviour; a backend may widen them. */
    ctrl->caps.max_blocks = VNVME_MAX_BLOCKS;
    if (ops && ops->caps)
        ops->caps(opaque, &ctrl->caps);
    if (!ctrl->caps.max_blocks || ctrl->caps.max_blocks > VNVME_MAX_XFER_BLOCKS)
        ctrl->caps.max_blocks = VNVME_MAX_BLOCKS;
    if (!ctrl->caps.writable)
        ctrl->caps.vwc = false;
    ctrl->wce = ctrl->caps.vwc;
}

bool vnvme_set_admin_queue(struct vnvme_ctrl *ctrl, u64 sq_addr, u64 cq_addr, u16 sq_size,
                           u16 cq_size)
{
    if (!sq_addr || !cq_addr || (sq_addr & (VNVME_PAGE_SIZE - 1)) ||
        (cq_addr & (VNVME_PAGE_SIZE - 1)) || sq_size < 2 || cq_size < 2 || sq_size > VNVME_MAX_QSIZE ||
        cq_size > VNVME_MAX_QSIZE ||
        overlaps(sq_addr, (size_t)sq_size * sizeof(struct vnvme_command),
                 cq_addr, (size_t)cq_size * sizeof(struct vnvme_completion)) ||
        !guest_span(ctrl, sq_addr, (size_t)sq_size * sizeof(struct vnvme_command), false) ||
        !guest_span(ctrl, cq_addr, (size_t)cq_size * sizeof(struct vnvme_completion), true))
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
    /* Preserve the prior external line until the reset queues deassert it. */
    update_irq(ctrl);
    return true;
}

/* Snapshot every PRP and validate every output span before the first copy.
 * Max 128 KiB plus an unaligned first page = 33 segments.
 * r2.1 (NVMe 1.3 4.3): a PRP list pointer only needs qword alignment; entries
 * run to the end of that page and, if more are needed, the page's last entry
 * chains to the next list page (page aligned, like every non-first PRP entry).
 * 32 data entries therefore need at most 2 list fragments. Each fragment is
 * admitted (guest_span) for exactly the bytes read before it is read; each
 * data page is admitted individually before any copy. Output must not overlap
 * any active queue, another output, or any consumed list fragment.
 * The error completion itself is still posted normally after payload rejection.
 * This host-only experiment assumes mappings remain stable during a command. */
#define VNVME_MAX_LIST_FRAGS 2
struct prp_plan {
    u8 *ptr[VNVME_MAX_XFER_BLOCKS + 1];
    u64 addr[VNVME_MAX_XFER_BLOCKS + 1];
    size_t len[VNVME_MAX_XFER_BLOCKS + 1];
    size_t count;
};
/* r4: data buffer for READ/WRITE: the backend's private transfer buffer when it has one
 * (single copy), else the 128 KiB frontend bounce (fixture, r2.2 behaviour). */
static u8 *data_buffer(struct vnvme_ctrl *ctrl, size_t *capacity)
{
    size_t cap = 0;
    u8 *b = ctrl->ops->xfer_buffer ? ctrl->ops->xfer_buffer(ctrl->opaque, &cap) : NULL;
    if (!b || !cap) {
        *capacity = sizeof(ctrl->bounce);
        return ctrl->bounce;
    }
    *capacity = cap;
    return b;
}
/* r4: `to_guest` is true when data is written into guest memory (READ, Identify, logs) and
 * false when it is read from guest memory (WRITE); `capacity` bounds the transfer. */
static bool plan_prps_dir(struct vnvme_ctrl *ctrl, const struct vnvme_command *cmd,
                          size_t total, struct prp_plan *plan, size_t capacity, bool to_guest);
static bool plan_prps(struct vnvme_ctrl *ctrl, const struct vnvme_command *cmd,
                      size_t total, struct prp_plan *plan)
{
    return plan_prps_dir(ctrl, cmd, total, plan, sizeof(ctrl->bounce), true);
}
static bool plan_prps_dir(struct vnvme_ctrl *ctrl, const struct vnvme_command *cmd,
                          size_t total, struct prp_plan *plan, size_t capacity, bool to_guest)
{
    memset(plan, 0, sizeof(*plan));
    if (!total || total > capacity || !cmd->prp1)
        return false;
    size_t first = VNVME_PAGE_SIZE - (cmd->prp1 & (VNVME_PAGE_SIZE - 1));
    if (first > total) first = total;
    u64 addr = cmd->prp1;
    size_t remaining = total;
    static u64 entries[VNVME_MAX_XFER_BLOCKS]; /* r4: static (single-threaded under bhl) */
    memset(entries, 0, sizeof(entries));
    size_t rest = total - first;
    u64 frag_addr[VNVME_MAX_LIST_FRAGS] = {0};
    size_t frag_len[VNVME_MAX_LIST_FRAGS] = {0};
    size_t frags = 0;
    if (rest) {
        size_t count = (rest + VNVME_PAGE_SIZE - 1) / VNVME_PAGE_SIZE; /* <= 256 */
        if (!cmd->prp2 || count > VNVME_MAX_XFER_BLOCKS) return false;
        if (count == 1) {
            if (cmd->prp2 & (VNVME_PAGE_SIZE - 1)) return false; /* data pointer */
            entries[0] = cmd->prp2;
        } else {
            u64 list = cmd->prp2;
            size_t got = 0;
            while (got < count) {
                if (!list || (list & 7) || frags == VNVME_MAX_LIST_FRAGS) return false;
                if (frags && (list & (VNVME_PAGE_SIZE - 1))) return false; /* chained: page aligned */
                size_t in_page = (VNVME_PAGE_SIZE - (size_t)(list & (VNVME_PAGE_SIZE - 1))) / sizeof(u64);
                size_t need = count - got;
                bool last = need <= in_page;
                size_t n = last ? need : in_page;          /* 1..512 entries, <= 4096 bytes */
                const u8 *src = guest_span(ctrl, list, n * sizeof(u64), false);
                if (!src) return false;
                frag_addr[frags] = list;
                frag_len[frags++] = n * sizeof(u64);
                if (last) {
                    memcpy(entries + got, src, n * sizeof(u64));
                    got += n;
                } else {
                    memcpy(entries + got, src, (n - 1) * sizeof(u64));
                    got += n - 1;
                    memcpy(&list, src + (n - 1) * sizeof(u64), sizeof(u64)); /* chain entry */
                }
            }
        }
    }
    while (remaining) {
        size_t n = plan->count ? (remaining < VNVME_PAGE_SIZE ? remaining : VNVME_PAGE_SIZE) : first;
        if (plan->count) {
            addr = entries[plan->count - 1];
            if (!addr || (addr & (VNVME_PAGE_SIZE - 1))) return false;
        }
        if (plan->count >= VNVME_MAX_XFER_BLOCKS + 1) return false;
        u8 *ptr = guest_span(ctrl, addr, n, to_guest);
        if (!ptr || overlaps_queue(ctrl, addr, n)) return false;
        for (size_t i = 0; i < frags; i++)
            if (overlaps(addr, n, frag_addr[i], frag_len[i])) return false;
        for (size_t i = 0; i < plan->count; i++)
            if (overlaps(addr, n, plan->addr[i], plan->len[i])) return false;
        plan->addr[plan->count] = addr;
        plan->ptr[plan->count] = ptr;
        plan->len[plan->count++] = n;
        remaining -= n;
    }
    return true;
}
static void copy_to_plan(const struct prp_plan *plan, const u8 *buffer)
{
    for (size_t i = 0; i < plan->count; i++) {
        memcpy(plan->ptr[i], buffer, plan->len[i]);
        buffer += plan->len[i];
    }
}
static void copy_from_plan(const struct prp_plan *plan, u8 *buffer)
{
    for (size_t i = 0; i < plan->count; i++) {
        memcpy(buffer, plan->ptr[i], plan->len[i]);
        buffer += plan->len[i];
    }
}
static bool prp_copy(struct vnvme_ctrl *ctrl, const struct vnvme_command *cmd,
                     size_t total, const void *buffer)
{
    static struct prp_plan plan; /* r4: static (single-threaded under bhl) */
    if (!plan_prps(ctrl, cmd, total, &plan)) return false;
    copy_to_plan(&plan, buffer);
    return true;
}

/* r2: deterministic namespace identifiers, no machine serials. EUI-64 uses a
 * locally administered prefix (0x02) followed by "M5WS1" and namespace 1. */
static const u8 ns1_eui64[8] = { 0x02, 0x4d, 0x35, 0x57, 0x53, 0x31, 0x00, 0x01 };
static const u8 ns1_nguid[16] = { 0x4d, 0x35, 0x57, 0x2d, 0x53, 0x31, 0x4e, 0x53,
                                  0x02, 0x4d, 0x35, 0x57, 0x53, 0x31, 0x00, 0x01 };

static void identify_controller(const struct vnvme_ctrl *ctrl, u8 *data)
{
    memset(data, 0, VNVME_PAGE_SIZE);
    put_le16(data + 0, 0x1b36);
    put_le16(data + 2, 0x1b36);
    put_ascii(data + 4, 20, "M1N1NVME000000000001");
    put_ascii(data + 24, 40, !ctrl->caps.native ? "S0 immutable RAM disk (NO ANS)"
                             : ctrl->caps.writable ? "M5W native window (read-write)"
                                                   : "M5W native window (read-only)");
    put_ascii(data + 64, 8, "0.1");
    data[72] = 4; /* Recommended arbitration burst. */
    /* MDTS = 2^n 4 KiB pages: 5 (128 KiB) for the fixture, 8 (1 MiB) for native rw. */
    u8 mdts = 0;
    while ((1u << (mdts + 1)) <= ctrl->caps.max_blocks) mdts++;
    data[77] = mdts;
    put_le32(data + 80, 0x00010300);
    put_le16(data + 256, 0);      /* No optional admin command sets. */
    data[258] = 0;                /* One outstanding Abort command. */
    data[259] = 0;                /* One outstanding Async Event Request. */
    data[260] = 0x03;             /* r2 FRMW: one firmware slot, slot 1 read-only. */
    data[261] = 1;                /* SMART/health log is supported. */
    data[262] = 0;                /* One error log entry. */
    put_le16(data + 266, VNVME_WCTEMP); /* r2 WCTEMP */
    put_le16(data + 268, VNVME_CCTEMP); /* r2 CCTEMP */
    data[512] = 0x66;             /* Required/max SQE size = 64 bytes. */
    data[513] = 0x44;             /* Required/max CQE size = 16 bytes. */
    put_le32(data + 516, 1);      /* One namespace. */
    put_le16(data + 520, 0);      /* No Dataset Management support. */
    data[525] = ctrl->caps.vwc ? 1 : 0; /* r4: honest VWC (device cache in rw mode) */
    data[526] = 0;               /* AWUN zero-based. */
}

static void identify_namespace(struct vnvme_ctrl *ctrl, u8 *data)
{
    memset(data, 0, VNVME_PAGE_SIZE);
    put_le64(data + 0, ctrl->namespace_blocks);
    put_le64(data + 8, ctrl->namespace_blocks);
    put_le64(data + 16, ctrl->namespace_blocks);
    data[99] = ctrl->caps.writable ? 0 : 1; /* r2 NSATTR: write protected unless rw (r4) */
    memcpy(data + 104, ns1_nguid, sizeof(ns1_nguid)); /* r2 NGUID */
    memcpy(data + 120, ns1_eui64, sizeof(ns1_eui64)); /* r2 EUI64 */
    data[25] = 0;       /* One LBA format. */
    data[26] = 0;       /* LBA format zero is active. */
    data[128 + 2] = 12; /* 2^12 = 4096-byte logical blocks. */
}

static void ns_descriptors(u8 *data)
{
    memset(data, 0, VNVME_PAGE_SIZE);
    data[0] = 1; data[1] = 8;   /* NIDT EUI-64 */
    memcpy(data + 4, ns1_eui64, 8);
    data[12] = 2; data[13] = 16; /* NIDT NGUID */
    memcpy(data + 16, ns1_nguid, 16);
}

static bool post_completion(struct vnvme_ctrl *ctrl, u16 cqid, u16 sqid, u16 sq_head, u16 cid,
                            u32 result, u16 status_code);

static u16 identify(struct vnvme_ctrl *ctrl, const struct vnvme_command *cmd)
{
    u8 cns = cmd->cdw10 & 0xff;
    switch (cns) {
        case VNVME_IDENTIFY_CONTROLLER:
            identify_controller(ctrl, ctrl->bounce);
            break;
        case VNVME_IDENTIFY_NAMESPACE:
            /* r2.1 ruling: OACS.NMS=0, so NSID FFFFFFFFh is Invalid Namespace (0Bh). */
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
            ns_descriptors(ctrl->bounce);
            break;
        default:
            return VNVME_SC_INVALID_FIELD;
    }

    if (!prp_copy(ctrl, cmd, VNVME_PAGE_SIZE, ctrl->bounce))
        return VNVME_SC_DATA_TRANSFER_ERROR;
    return VNVME_SC_SUCCESS;
}

static u16 get_log_page(struct vnvme_ctrl *ctrl, const struct vnvme_command *cmd)
{
    u8 lid = cmd->cdw10 & 0xff;
    u32 numd = ((cmd->cdw11 & 0xffff) << 16) | (cmd->cdw10 >> 16);
    size_t length = ((size_t)numd + 1) * 4;
    u64 offset = ((u64)cmd->cdw13 << 32) | cmd->cdw12;
    size_t log_size;
    u8 *image = ctrl->bounce + VNVME_PAGE_SIZE; /* scratch page after the output page */
    switch (lid) {
        case VNVME_LID_ERROR: log_size = 64; break;
        case VNVME_LID_SMART: log_size = 512; break;
        case VNVME_LID_FW_SLOT: log_size = 512; break;
        default: return VNVME_SC_INVALID_LOG_PAGE;
    }
    if (length > VNVME_PAGE_SIZE || (offset & 3) || offset >= log_size)
        return VNVME_SC_INVALID_FIELD;
    memset(image, 0, log_size);
    if (lid == VNVME_LID_SMART) {
        put_le16(image + 1, VNVME_COMPOSITE_TEMP); /* composite temperature */
        image[3] = 100;                            /* available spare */
        image[4] = 10;                             /* spare threshold */
    } else if (lid == VNVME_LID_FW_SLOT) {
        image[0] = 0x01;                           /* AFI: slot 1 active */
        put_ascii(image + 8, 8, "0.1");            /* FRS1 == Identify FR */
    }
    memset(ctrl->bounce, 0, length);
    size_t avail = log_size - (size_t)offset;
    memcpy(ctrl->bounce, image + offset, avail < length ? avail : length);
    if (!prp_copy(ctrl, cmd, length, ctrl->bounce))
        return VNVME_SC_DATA_TRANSFER_ERROR;
    return VNVME_SC_SUCCESS;
}

static u16 create_cq(struct vnvme_ctrl *ctrl, const struct vnvme_command *cmd)
{
    u16 qid = cmd->cdw10 & 0xffff;
    u16 size = (cmd->cdw10 >> 16) + 1;
    if (ctrl->iocqes != 4) /* r2.1: CC.IOCQES must be valid before CQ creation */
        return VNVME_SC_INVALID_FIELD;
    if (qid != 1 || size < 2 || size > VNVME_MAX_QSIZE || !(cmd->cdw11 & BIT(0)) || !cmd->prp1 ||
        (cmd->prp1 & (VNVME_PAGE_SIZE - 1)) ||
        overlaps_queue(ctrl, cmd->prp1, (size_t)size * sizeof(struct vnvme_completion)) ||
        !guest_span(ctrl, cmd->prp1, (size_t)size * sizeof(struct vnvme_completion), true))
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
    if (ctrl->iosqes != 6) /* r2.1: CC.IOSQES must be valid before SQ creation */
        return VNVME_SC_INVALID_FIELD;
    if (qid != 1 || cqid != 1 || size < 2 || size > VNVME_MAX_QSIZE || !(cmd->cdw11 & BIT(0)) ||
        !cmd->prp1 || (cmd->prp1 & (VNVME_PAGE_SIZE - 1)) || !ctrl->queues[cqid].cq_valid ||
        overlaps_queue(ctrl, cmd->prp1, (size_t)size * sizeof(struct vnvme_command)) ||
        !guest_span(ctrl, cmd->prp1, (size_t)size * sizeof(struct vnvme_command), false))
        return VNVME_SC_INVALID_FIELD;
    struct vnvme_queue *q = &ctrl->queues[qid];
    if (q->sq_valid)
        return VNVME_SC_INVALID_FIELD;
    q->sq_addr = cmd->prp1;
    q->sq_size = size;
    q->sq_head = 0;
    q->sq_tail = 0;
    q->cq_id = cqid;
    q->sq_valid = true;
    return VNVME_SC_SUCCESS;
}

static u16 feature(struct vnvme_ctrl *ctrl, const struct vnvme_command *cmd, u32 *result)
{
    bool set = cmd->opcode == VNVME_ADMIN_SET_FEATURES;
    /* ONCS.Save/Select=0: SEL is ignored; a Set with SV=1 is not saveable. */
    if (set && (cmd->cdw10 & BIT(31)))
        return VNVME_SC_FEATURE_NOT_SAVEABLE;
    switch (cmd->cdw10 & 0xff) {
        case VNVME_FID_TEMP_THRESHOLD: {
            u32 tmpsel = (cmd->cdw11 >> 16) & 0xf, thsel = (cmd->cdw11 >> 20) & 3;
            if (tmpsel != 0 || thsel > 1)
                return VNVME_SC_INVALID_FIELD;
            u16 *t = thsel ? &ctrl->temp_under : &ctrl->temp_over;
            if (set)
                *t = cmd->cdw11 & 0xffff;
            *result = set ? 0 : *t;
            return VNVME_SC_SUCCESS;
        }
        case NVME_FID_VOLATILE_WC:
            if (!ctrl->caps.vwc)
                return VNVME_SC_INVALID_FIELD; /* r2: VWC not present (Identify VWC=0) */
            /* r4: WCE=0 is honoured as write-through: every WRITE is followed by FLUSH. */
            if (set)
                ctrl->wce = cmd->cdw11 & 1;
            *result = set ? 0 : ctrl->wce;
            return VNVME_SC_SUCCESS;
        case NVME_FID_POWER_MANAGEMENT:
            if (set && (cmd->cdw11 & 0x1f))
                return VNVME_SC_INVALID_FIELD; /* NPSS=0: only PS0 */
            *result = 0;
            return VNVME_SC_SUCCESS;
        case NVME_FID_ARBITRATION:
        case NVME_FID_ERROR_RECOVERY:
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
            return feature(ctrl, cmd, result);
        case VNVME_ADMIN_ABORT:
            /* r2: the only command that can be outstanding is the held AER. */
            *result = 1;
            if ((cmd->cdw10 & 0xffff) == 0 && ctrl->aer_outstanding &&
                (cmd->cdw10 >> 16) == ctrl->aer_cid && ctrl->queues[0].cq_valid &&
                ctrl->queues[0].cq_size - 1 - ctrl->queues[0].cq_pending >= 2) { /* r2.1: capacity size-1 */
                ctrl->aer_outstanding = false;
                if (!post_completion(ctrl, 0, 0, ctrl->queues[0].sq_head, ctrl->aer_cid, 0,
                                     VNVME_SC_ABORT_REQUESTED))
                    return VNVME_SC_INTERNAL; /* fatal already latched */
                ctrl->aborted_commands++;
                *result = 0;
            }
            return VNVME_SC_SUCCESS;
        case VNVME_ADMIN_ASYNC_EVENT:
            if (ctrl->aer_outstanding)
                return VNVME_SC_AER_LIMIT; /* AERL=0 */
            ctrl->aer_outstanding = true;
            ctrl->aer_cid = cmd->cid;
            *complete = false; /* never completed: no asynchronous events exist */
            return VNVME_SC_SUCCESS;
        default:
            return VNVME_SC_INVALID_OPCODE;
    }
}

/* r4: backend flush (rw only). */
static bool backend_flush(struct vnvme_ctrl *ctrl)
{
    if (!ctrl->ops->flush || !ctrl->ops->flush(ctrl->opaque))
        return false;
    ctrl->backend_flushes++;
    return true;
}
bool vnvme_backend_flush(struct vnvme_ctrl *ctrl)
{
    return !ctrl->caps.writable || backend_flush(ctrl);
}

/* r4: WRITE into a writable backend. Same bounds as READ, data gathered from the guest
 * PRPs into the backend's private transfer buffer (single copy), FUA = write then flush. */
static u16 process_write(struct vnvme_ctrl *ctrl, const struct vnvme_command *cmd)
{
    /* FUA (30) and LR (31) accepted; PRINFO, directives and reserved bits must be zero. */
    const u32 write_cdw12_ok = 0xffffu | BIT(31) | BIT(30);
    if ((cmd->cdw12 & ~write_cdw12_ok) || (cmd->cdw13 & 0xffffff00u))
        return VNVME_SC_INVALID_FIELD;
    u32 blocks = (cmd->cdw12 & 0xffff) + 1;
    if (blocks > ctrl->caps.max_blocks) {
        if (ctrl->ops->abort) ctrl->ops->abort(ctrl->opaque);
        return VNVME_SC_INVALID_FIELD;
    }
    u64 lba = ((u64)cmd->cdw11 << 32) | cmd->cdw10;
    if (lba >= ctrl->namespace_blocks || blocks > ctrl->namespace_blocks - lba) {
        if (ctrl->ops->abort) ctrl->ops->abort(ctrl->opaque);
        return VNVME_SC_INVALID_FIELD;
    }
    size_t total = (size_t)blocks * VNVME_LBA_SIZE, capacity;
    u8 *buffer = data_buffer(ctrl, &capacity);
    static struct prp_plan plan;
    if (!plan_prps_dir(ctrl, cmd, total, &plan, capacity, false)) return VNVME_SC_DATA_TRANSFER_ERROR;
    if (!ctrl->ops->write_blocks) return VNVME_SC_INTERNAL;
    copy_from_plan(&plan, buffer);
    for (u32 done = 0; done < blocks;) {
        u32 n = blocks - done;
        if (n > 32) n = 32;
        if (!ctrl->ops->write_blocks(ctrl->opaque, lba + done, n,
                                     buffer + (size_t)done * VNVME_LBA_SIZE))
            return VNVME_SC_INTERNAL;
        done += n;
    }
    ctrl->backend_writes++;
    if ((cmd->cdw12 & BIT(30)) || !ctrl->wce) {
        ctrl->fua_flushes++; /* FUA or write-through (WCE=0): write then flush */
        if (!backend_flush(ctrl)) return VNVME_SC_INTERNAL;
    }
    return VNVME_SC_SUCCESS;
}

static u16 process_io(struct vnvme_ctrl *ctrl, const struct vnvme_command *cmd)
{
    /* r2: FLUSH is mandatory; with an immutable backend and VWC=0 it is a no-op.
     * r4: with a writable backend it is passed to the device (post-M4: always). */
    if (cmd->opcode == VNVME_IO_FLUSH) {
        if (cmd->nsid != 1 && cmd->nsid != 0xffffffffu) return VNVME_SC_INVALID_NAMESPACE;
        if (!ctrl->caps.writable) return VNVME_SC_SUCCESS;
        return backend_flush(ctrl) ? VNVME_SC_SUCCESS : VNVME_SC_INTERNAL;
    }
    if (cmd->nsid != 1) return VNVME_SC_INVALID_NAMESPACE;
    /* r2: WRITE is mandatory; the namespace is write protected (NSATTR bit 0).
     * Write Zeroes/DSM/others are not advertised (ONCS=0): Invalid Opcode.
     * r4: only a writable (native rw) backend accepts WRITE; DSM stays rejected. */
    if (cmd->opcode == VNVME_IO_WRITE && !ctrl->caps.writable) {
        u64 lba = ((u64)cmd->cdw11 << 32) | cmd->cdw10;
        u32 blocks = (cmd->cdw12 & 0xffff) + 1;
        (void)lba;
        (void)blocks;
        if (ctrl->ops->abort) ctrl->ops->abort(ctrl->opaque);
    }
    if (cmd->opcode == VNVME_IO_WRITE)
        return ctrl->caps.writable ? process_write(ctrl, cmd) : VNVME_SC_NS_WRITE_PROTECTED;
    if (cmd->opcode != VNVME_IO_READ) return VNVME_SC_INVALID_OPCODE;
    /* r2: READ accepts FUA (30) and LR (31) and DSM hints (CDW13 7:0). PRINFO,
     * directives/STC and reserved bits must be zero; no PI so CDW14/15 ignored. */
    if ((cmd->cdw12 & ~(0xffffu | BIT(30) | BIT(31))) || (cmd->cdw13 & 0xffffff00u))
        return VNVME_SC_INVALID_FIELD;
    u32 blocks = (cmd->cdw12 & 0xffff) + 1;
    u64 lba = ((u64)cmd->cdw11 << 32) | cmd->cdw10;
    if (lba >= ctrl->namespace_blocks || blocks > ctrl->namespace_blocks - lba) {
        ctrl->refused_reads++;
        return VNVME_SC_INVALID_FIELD;
    }
    if (blocks > ctrl->caps.max_blocks) {
        if (ctrl->ops->abort) ctrl->ops->abort(ctrl->opaque);
        return VNVME_SC_INVALID_FIELD;
    }
    size_t total = (size_t)blocks * VNVME_LBA_SIZE, capacity;
    u8 *buffer = data_buffer(ctrl, &capacity);
    static struct prp_plan plan; /* r4: 257-entry plan, static (single-threaded under bhl) */
    if (!plan_prps_dir(ctrl, cmd, total, &plan, capacity, true)) return VNVME_SC_DATA_TRANSFER_ERROR;
    static u8 read_result[256 * VNVME_LBA_SIZE];
    if (!ctrl->ops->read_blocks) return VNVME_SC_INTERNAL;
    for (u32 done = 0; done < blocks;) {
        u32 n = blocks - done;
        if (n > 32) n = 32;
        if (!ctrl->ops->read_blocks(ctrl->opaque, lba + done, n, buffer))
            return VNVME_SC_INTERNAL;
        memcpy(read_result + (size_t)done * VNVME_LBA_SIZE, buffer,
               (size_t)n * VNVME_LBA_SIZE);
        done += n;
    }
    copy_to_plan(&plan, read_result);
    return VNVME_SC_SUCCESS;
}

static bool post_completion(struct vnvme_ctrl *ctrl, u16 cqid, u16 sqid, u16 sq_head, u16 cid,
                            u32 result, u16 status_code)
{
    struct vnvme_queue *cq = &ctrl->queues[cqid];
    /* r2.1: NVMe 1.3 4.1 - a CQ is full with size-1 entries. */
    if (!cq->cq_valid || cq->cq_size < 2 || cq->cq_pending >= cq->cq_size - 1)
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
    void *cqe_dst = guest_span(ctrl, cq->cq_addr + cq->cq_tail * sizeof(cqe), sizeof(cqe), true);
    if (!cqe_dst) {
        set_fatal(ctrl, VNVME_FATAL_CQE); /* refused CQE memory is a controller fatal status */
        return false;
    }
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
        .result = result,
        .cq_pending = cq->cq_pending,
        .sq_head = sq_head,
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
static void process_until_completion(struct vnvme_ctrl *ctrl, u16 qid)
{
    /* r2: never fetch after fatal/shutdown. Any refusal of queue memory latches
     * ctrl->fatal (reported as CSTS.CFS) instead of failing the guest access;
     * refused bytes are never read or written. */
    if (ctrl->fatal || ctrl->shutdown)
        return;
    struct vnvme_queue *sq = &ctrl->queues[qid];
    u16 cqid = qid ? sq->cq_id : 0;
    if (!sq->sq_valid || cqid >= VNVME_MAX_QUEUES)
        return;
    struct vnvme_queue *cq = &ctrl->queues[cqid];
    if (!cq->cq_valid)
        return;

    /* Wait until EL1 consumes the previous CQE before doing more synchronous backend work. */
    if (cq->cq_pending)
        return;

    /* Revalidate the entire queue spans each dispatch: no wrapped slot arithmetic. */
    if (!guest_span(ctrl, sq->sq_addr, (size_t)sq->sq_size * sizeof(struct vnvme_command), false)) {
        set_fatal(ctrl, VNVME_FATAL_QSPAN_SQ);
        return;
    }
    if (!guest_span(ctrl, cq->cq_addr, (size_t)cq->cq_size * sizeof(struct vnvme_completion), true)) {
        set_fatal(ctrl, VNVME_FATAL_QSPAN_CQ);
        return;
    }
    u16 walked = 0;
    while (sq->sq_head != sq->sq_tail) {
        u16 slot = sq->sq_head;
        struct vnvme_command cmd;
        void *sqe_src = guest_span(ctrl, sq->sq_addr + sq->sq_head * sizeof(cmd), sizeof(cmd), false);
        if (!sqe_src) {
            set_fatal(ctrl, VNVME_FATAL_SQE);
            return;
        }
        memcpy(&cmd, sqe_src, sizeof(cmd));
        sq->sq_head++;
        if (sq->sq_head == sq->sq_size)
            sq->sq_head = 0;
        if (++walked > sq->sq_size) {
            set_fatal(ctrl, VNVME_FATAL_WALK);
            return;
        }
        ctrl->stats.commands++;

        u32 result = 0;
        bool complete = true;
        u16 status = (cmd.flags || cmd.metadata || cmd.cdw2 || cmd.cdw3)
                         ? VNVME_SC_INVALID_FIELD
                         : qid ? process_io(ctrl, &cmd) : process_admin(ctrl, &cmd, &result, &complete);
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
            .command = &cmd,
        };
        trace_event(ctrl, &event);
        if (ctrl->fatal)
            return;
        if (complete) {
            post_completion(ctrl, cqid, qid, sq->sq_head, cmd.cid, result, status);
            return;
        }
    }
}

void vnvme_shutdown(struct vnvme_ctrl *ctrl, bool abrupt)
{
    if (ctrl->shutdown)
        return;
    ctrl->shutdown = true;
    if (abrupt || ctrl->fatal) {
        /* Discard without completions and drop every queue. */
        for (u32 i = 0; i < VNVME_MAX_QUEUES; i++) {
            struct vnvme_queue *q = &ctrl->queues[i];
            if (q->sq_valid)
                ctrl->discarded_commands += (u16)((q->sq_tail + q->sq_size - q->sq_head) % q->sq_size);
        }
        ctrl->discarded_commands += ctrl->aer_outstanding;
        ctrl->aer_outstanding = false;
        memset(ctrl->queues, 0, sizeof(ctrl->queues));
        update_irq(ctrl);
        return;
    }
    /* Normal: every unfetched SQ entry (I/O first, then admin) and the held AER
     * complete with Command Abort Requested, without executing. An entry whose
     * SQE or CQE memory is refused, or that finds its CQ full, is discarded. */
    for (u32 pass = 0; pass < VNVME_MAX_QUEUES; pass++) {
        u16 qid = (u16)((pass + 1) % VNVME_MAX_QUEUES);
        struct vnvme_queue *sq = &ctrl->queues[qid];
        if (!sq->sq_valid)
            continue;
        u16 cqid = qid ? sq->cq_id : 0;
        while (sq->sq_head != sq->sq_tail) {
            if (ctrl->fatal) { /* r2.1: nothing more is read or posted after fatal */
                ctrl->discarded_commands += (u16)((sq->sq_tail + sq->sq_size - sq->sq_head) % sq->sq_size);
                sq->sq_head = sq->sq_tail;
                break;
            }
            struct vnvme_command cmd;
            void *src = (cqid < VNVME_MAX_QUEUES && ctrl->queues[cqid].cq_valid)
                            ? guest_span(ctrl, sq->sq_addr + sq->sq_head * sizeof(cmd), sizeof(cmd), false)
                            : NULL;
            sq->sq_head = (u16)((sq->sq_head + 1) % sq->sq_size);
            if (!src) { ctrl->discarded_commands++; continue; }
            memcpy(&cmd, src, sizeof(cmd));
            struct vnvme_queue *cq = &ctrl->queues[cqid];
            if (cq->cq_pending >= cq->cq_size - 1 ||
                !post_completion(ctrl, cqid, qid, sq->sq_head, cmd.cid, 0, VNVME_SC_ABORT_REQUESTED))
                ctrl->discarded_commands++;
            else
                ctrl->aborted_commands++;
        }
    }
    if (ctrl->aer_outstanding) {
        struct vnvme_queue *cq = &ctrl->queues[0];
        ctrl->aer_outstanding = false;
        if (!ctrl->fatal && cq->cq_valid && cq->cq_pending + 1 < cq->cq_size &&
            post_completion(ctrl, 0, 0, ctrl->queues[0].sq_head, ctrl->aer_cid, 0, VNVME_SC_ABORT_REQUESTED))
            ctrl->aborted_commands++;
        else
            ctrl->discarded_commands++;
    }
    /* A refused CQE above latched fatal; the caller reports CFS. */
}

bool vnvme_sq_doorbell(struct vnvme_ctrl *ctrl, u16 qid, u16 new_tail)
{
    if (qid >= VNVME_MAX_QUEUES)
        return false;
    struct vnvme_queue *sq = &ctrl->queues[qid];
    if (!sq->sq_valid || new_tail >= sq->sq_size)
        return false;
    if (ctrl->shutdown || ctrl->fatal)
        return false; /* r2: ignored by the caller, nothing fetched */
    ctrl->stats.sq_doorbells++;
    sq->sq_tail = new_tail;
    process_until_completion(ctrl, qid);
    return true;
}

bool vnvme_cq_doorbell(struct vnvme_ctrl *ctrl, u16 qid, u16 new_head)
{
    if (qid >= VNVME_MAX_QUEUES || !ctrl->queues[qid].cq_valid ||
        new_head >= ctrl->queues[qid].cq_size)
        return false;
    struct vnvme_queue *cq = &ctrl->queues[qid];
    u16 consumed = (new_head + cq->cq_size - cq->cq_head) % cq->cq_size;
    if (consumed > cq->cq_pending)
        return false;
    ctrl->stats.cq_doorbells++;
    cq->cq_head = new_head;
    cq->cq_pending -= consumed;
    update_irq(ctrl);

    if (cq->cq_pending)
        return true;

    /* CQ0 serves admin SQ0; each I/O SQ records the CQ it was created against. */
    if (qid == 0) {
        process_until_completion(ctrl, 0);
        return true;
    }
    for (u16 sqid = 1; sqid < VNVME_MAX_QUEUES; sqid++) {
        struct vnvme_queue *sq = &ctrl->queues[sqid];
        if (sq->sq_valid && sq->cq_id == qid && sq->sq_head != sq->sq_tail) {
            process_until_completion(ctrl, sqid);
            return true;
        }
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
