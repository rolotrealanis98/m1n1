/* SPDX-License-Identifier: MIT */

#ifndef HV_NVME_QUEUE_H
#define HV_NVME_QUEUE_H

#ifdef VNVME_HOST_TEST
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#define PACKED     __attribute__((packed))
#define ALIGNED(x) __attribute__((aligned(x)))
#define BIT(x)     (1UL << (x))
#else
#include "types.h"
#endif

#define VNVME_PAGE_SIZE  4096
#define VNVME_LBA_SIZE   4096
#define VNVME_MAX_QSIZE  256
#define VNVME_MAX_QUEUES 2
#define VNVME_MAX_BLOCKS 32

enum vnvme_admin_opcode {
    VNVME_ADMIN_DELETE_SQ = 0x00,
    VNVME_ADMIN_CREATE_SQ = 0x01,
    VNVME_ADMIN_GET_LOG_PAGE = 0x02,
    VNVME_ADMIN_DELETE_CQ = 0x04,
    VNVME_ADMIN_CREATE_CQ = 0x05,
    VNVME_ADMIN_IDENTIFY = 0x06,
    VNVME_ADMIN_ABORT = 0x08,
    VNVME_ADMIN_SET_FEATURES = 0x09,
    VNVME_ADMIN_GET_FEATURES = 0x0a,
    VNVME_ADMIN_ASYNC_EVENT = 0x0c,
};

enum vnvme_io_opcode {
    VNVME_IO_FLUSH = 0x00,
    VNVME_IO_WRITE = 0x01,
    VNVME_IO_READ = 0x02,
    VNVME_IO_WRITE_ZEROES = 0x08,
    VNVME_IO_DATASET_MANAGEMENT = 0x09,
};

enum vnvme_identify_cns {
    VNVME_IDENTIFY_NAMESPACE = 0x00,
    VNVME_IDENTIFY_CONTROLLER = 0x01,
    VNVME_IDENTIFY_ACTIVE_NS_LIST = 0x02,
    VNVME_IDENTIFY_NS_ID_DESCRIPTOR_LIST = 0x03,
};

enum vnvme_status_code {
    VNVME_SC_SUCCESS = 0x00,
    VNVME_SC_INVALID_OPCODE = 0x01,
    VNVME_SC_INVALID_FIELD = 0x02,
    VNVME_SC_DATA_TRANSFER_ERROR = 0x04,
    VNVME_SC_INTERNAL = 0x06,
    VNVME_SC_ABORT_REQUESTED = 0x07,
    VNVME_SC_INVALID_NAMESPACE = 0x0b,
    VNVME_SC_NS_WRITE_PROTECTED = 0x20,
    /* Command specific status: SCT 1 in bits 10:8 of the 15-bit status. */
    VNVME_SC_AER_LIMIT = 0x105,
    VNVME_SC_INVALID_LOG_PAGE = 0x109,
    VNVME_SC_FEATURE_NOT_SAVEABLE = 0x10d,
};
#define VNVME_LID_ERROR 0x01
#define VNVME_LID_SMART 0x02
#define VNVME_LID_FW_SLOT 0x03
#define VNVME_FID_TEMP_THRESHOLD 0x04
#define VNVME_WCTEMP 343u /* Kelvin, warning composite temperature */
#define VNVME_CCTEMP 358u /* Kelvin, critical composite temperature */
#define VNVME_COMPOSITE_TEMP 310u

struct vnvme_command {
    u8 opcode;
    u8 flags;
    u16 cid;
    u32 nsid;
    u32 cdw2;
    u32 cdw3;
    u64 metadata;
    u64 prp1;
    u64 prp2;
    u32 cdw10;
    u32 cdw11;
    u32 cdw12;
    u32 cdw13;
    u32 cdw14;
    u32 cdw15;
} PACKED;

struct vnvme_completion {
    u32 result;
    u32 reserved;
    u16 sq_head;
    u16 sq_id;
    u16 cid;
    u16 status;
} PACKED;

enum vnvme_trace_type {
    VNVME_TRACE_SUBMISSION,
    VNVME_TRACE_COMPLETION,
    VNVME_TRACE_FATAL,     /* r2.1: status carries the vnvme_fatal_reason */
};
/* r2.1: why the controller latched fatal (reported as CSTS.CFS). */
enum vnvme_fatal_reason {
    VNVME_FATAL_NONE = 0,
    VNVME_FATAL_QSPAN_SQ = 1,
    VNVME_FATAL_QSPAN_CQ = 2,
    VNVME_FATAL_SQE = 3,
    VNVME_FATAL_CQE = 4,
    VNVME_FATAL_WALK = 5,
    /* 6..9 are raised by the S1 model at enable / CC write */
};

struct vnvme_trace_event {
    enum vnvme_trace_type type;
    u16 qid;
    u16 cqid;
    u16 slot;
    u16 head;
    u16 tail;
    u16 cid;
    u16 status;
    u8 opcode;
    u8 phase;
    bool irq_asserted;
    /* r2.1 diagnostics: copies of the fetched SQE / posted CQE (never guest pointers). */
    const struct vnvme_command *command; /* SUBMISSION only: local copy */
    u32 result;                          /* COMPLETION: DW0 */
    u16 cq_pending;
    u16 sq_head;
};

struct vnvme_backend_ops {
    /* Complete contiguous guest-RAM span, checked permissions; NULL rejects.
     * Host-only S0. No fallback to raw IPA translation. Mapping must remain stable
     * for this synchronous call; a concurrent guest needs pinning in S1. */
    void *(*guest_span)(void *opaque, u64 ipa, size_t length, bool write);
    bool (*read)(void *opaque, u64 lba, void *buffer);
    bool (*write)(void *opaque, u64 lba, const void *buffer);
    bool (*read_blocks)(void *opaque, u64 lba, u32 blocks, void *buffer);
    bool (*write_blocks)(void *opaque, u64 lba, u32 blocks, const void *buffer);
    bool (*flush)(void *opaque);
    void (*publish)(void *opaque);
    void (*irq)(void *opaque, bool asserted);
    void (*trace)(void *opaque, const struct vnvme_trace_event *event);
};

struct vnvme_queue {
    u64 sq_addr;
    u64 cq_addr;
    u16 sq_size;
    u16 cq_size;
    u16 sq_head;
    u16 cq_head;
    u16 cq_tail;
    u16 cq_pending;
    u16 cq_id;
    u16 sq_tail;
    u8 cq_phase;
    bool sq_valid;
    bool cq_valid;
    bool irq_enabled;
};

struct vnvme_stats {
    u64 sq_doorbells;
    u64 cq_doorbells;
    u64 commands;
    u64 completions;
};

struct vnvme_queue_state {
    u16 sq_head;
    u16 sq_tail;
    u16 cq_head;
    u16 cq_tail;
};

struct vnvme_snapshot {
    struct vnvme_stats stats;
    struct vnvme_queue_state queues[VNVME_MAX_QUEUES];
    bool irq_asserted;
};

struct vnvme_intx_delivery {
    bool outstanding;
    bool assertion_notified;
    bool owner_kick_pending;
};

struct vnvme_ctrl {
    u64 namespace_blocks;
    const struct vnvme_backend_ops *ops;
    void *opaque;
    struct vnvme_queue queues[VNVME_MAX_QUEUES];
    struct vnvme_stats stats;
    bool irq_asserted;
    /* r2: queue/CQE memory refused during dispatch => controller fatal (CSTS.CFS);
     * the refused bytes are never dereferenced. Cleared only by vnvme_init. */
    bool fatal;
    u8 fatal_reason; /* r2.1: first reason (enum vnvme_fatal_reason) */
    /* r2.1: CC.IOSQES/IOCQES as last accepted; checked at I/O queue creation. */
    u8 iosqes;
    u8 iocqes;
    /* r2: after normal/abrupt shutdown nothing more is fetched until reset. */
    bool shutdown;
    /* r2: AERL=0 => at most one held Asynchronous Event Request. */
    bool aer_outstanding;
    u16 aer_cid;
    /* r2: Temperature Threshold feature (composite sensor only). */
    u16 temp_over;
    u16 temp_under;
    u64 invalid_doorbells;
    u64 aborted_commands;
    u64 discarded_commands;
    u8 bounce[VNVME_MAX_BLOCKS * VNVME_LBA_SIZE] ALIGNED(VNVME_PAGE_SIZE);
};

_Static_assert(sizeof(struct vnvme_command) == 64, "invalid virtual NVMe command size");
_Static_assert(sizeof(struct vnvme_completion) == 16, "invalid virtual NVMe completion size");

void vnvme_init(struct vnvme_ctrl *ctrl, u64 namespace_blocks, const struct vnvme_backend_ops *ops,
                void *opaque);
bool vnvme_set_admin_queue(struct vnvme_ctrl *ctrl, u64 sq_addr, u64 cq_addr, u16 sq_size,
                           u16 cq_size);
bool vnvme_sq_doorbell(struct vnvme_ctrl *ctrl, u16 qid, u16 new_tail);
bool vnvme_cq_doorbell(struct vnvme_ctrl *ctrl, u16 qid, u16 new_head);
/* r2: normal shutdown completes every fetched-but-unexecuted SQ entry and a held
 * AER with Command Abort Requested (no backend work); abrupt discards them and
 * drops all queues. Both leave the controller unable to fetch until reset. */
void vnvme_shutdown(struct vnvme_ctrl *ctrl, bool abrupt);
bool vnvme_intx_can_inject(bool asserted, u32 intms, bool injected, bool gic_enabled, int free_lr);
bool vnvme_intx_delivery_can_inject(const struct vnvme_intx_delivery *delivery, bool asserted,
                                    u32 intms, bool gic_enabled, int free_lr);
void vnvme_intx_delivery_update_line(struct vnvme_intx_delivery *delivery, bool asserted);
void vnvme_intx_delivery_mark_injected(struct vnvme_intx_delivery *delivery);
void vnvme_intx_delivery_eoi(struct vnvme_intx_delivery *delivery);
bool vnvme_intx_delivery_should_kick_owner(struct vnvme_intx_delivery *delivery, bool asserted,
                                           u32 intms, int current_cpu, int owner_cpu);
void vnvme_intx_delivery_owner_polled(struct vnvme_intx_delivery *delivery);
void vnvme_get_snapshot(const struct vnvme_ctrl *ctrl, struct vnvme_snapshot *out);

#endif
