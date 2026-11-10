/* SPDX-License-Identifier: MIT */
#ifndef S1_MODEL_H
#define S1_MODEL_H
#include "hv_nvme_queue.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define S1_ECAM_SIZE UINT64_C(0x100000)
#define S1_BAR_SIZE UINT64_C(0x1000000)
#define S1_MEM 2u
#define S1_BME 4u
#define S1_INTX_DISABLE 0x400u
#define S1_CC_EN 1u
#define S1_CC_SHN 0xC000u
#define S1_CC_VALID (1u | (6u << 16) | (4u << 20))

/* r2.1 S1DG diagnostics: recorded only from model-local values (never guest
 * memory), no printf. Lives inside struct s1_model (BSS, inside the resident
 * containment check) and is read from the paused hypervisor at
 * &controller + S1DG_MODEL_OFFSET. Layout is version 1; see
 * proxyclient/tools/s1dg_decode.py. All multi-byte fields little-endian. */
#define S1DG_MAGIC 0x47443153u /* "S1DG" */
#define S1DG_VERSION 1u
#define S1DG_RING 512u
#define S1DG_ADMIN_FIRST 64u
#define S1DG_MMIO_FIRST 256u
enum s1dg_type {
    S1DG_MMIO_R = 1, S1DG_MMIO_W, S1DG_CFG_R, S1DG_CFG_W, S1DG_SQE, S1DG_CQE,
    S1DG_REFUSAL, S1DG_FATAL, S1DG_SHUTDOWN, S1DG_RESET, S1DG_ENABLE,
    S1DG_IRQ_EDGE, S1DG_INVALID_DB, S1DG_SHN_CLEAR_NO_RESET,
};
/* REFUSAL reasons (model-side; the provider's internal reason is not visible). */
enum { S1DG_REF_NOT_MAPPED = 1, S1DG_REF_FAULTED, S1DG_REF_NO_BME, S1DG_REF_PROVIDER };
/* FATAL reasons 1..5 = enum vnvme_fatal_reason, then: */
enum { S1DG_FATAL_ENABLE_CC = 6, S1DG_FATAL_ENABLE_BME, S1DG_FATAL_ENABLE_ADMQ, S1DG_FATAL_SHN11 };
/* INVALID_DB reason bits */
enum { S1DG_DB_NOT_READY = 1, S1DG_DB_CFS = 2, S1DG_DB_NO_BME = 4, S1DG_DB_VALUE = 8,
       S1DG_DB_INDEX = 16, S1DG_DB_QUEUE = 32, S1DG_DB_SHUTDOWN = 64 };
struct s1dg_entry {      /* 40 bytes */
    u32 seq, t;          /* t = CNTVCT low 32 bits (0 on host) */
    u8 type, width, qid, opcode;
    u16 cid, status;     /* status: full 15-bit SCT/SC, or access result for MMIO/CFG */
    u64 a, b;
    u32 c, d;
} __attribute__((packed));
struct s1dg_admin {      /* 72 bytes: first 64 admin commands, never wraps */
    u8 sqe[64];
    u16 status;
    u16 completed;
    u32 dw0;
} __attribute__((packed));
struct s1dg_access {     /* 24 bytes: first 256 register/config accesses, never wraps */
    u32 seq;
    u32 offset;
    u8 kind;             /* 0 = BAR MMIO, 1 = config */
    u8 width;
    u8 write;
    u8 ok;
    u32 csts;
    u64 value;
} __attribute__((packed));
struct s1_diag {
    u32 magic, version, ring_entries, entry_size;
    u64 write_seq;                          /* ring index = seq % 512 */
    u32 admin_first_count, mmio_first_count;
    u32 struct_size, reserved0;
    u64 admin_opcode[32], admin_other, last_admin_other;
    u64 io_opcode[32], io_other, last_io_other;
    u64 nonzero_status, last_err_qid, last_err_opcode, last_err_cid, last_err_status;
    u64 writes, last_write_lba, last_write_nlb;
    u64 mmio_width[2][4];                   /* [write][1,2,4,8 bytes] */
    u64 cfg_access[2];
    u64 mmio_rejected;
    u64 reserved_access, last_reserved_offset;
    u64 fatal_count, first_fatal_reason;
    u64 refusals[8];
    u64 invalid_db[8];                      /* per reason bit */
    u64 max_cq_pending[2];
    u64 cc_writes, shn_transitions, en_transitions;
    u64 intms_writes, intmc_writes, irq_edges;
    u64 shn_clear_no_reset, shutdowns_normal, shutdowns_abrupt;
    u64 aborted, discarded;
    struct s1dg_entry ring[S1DG_RING];
    struct s1dg_admin admin_first[S1DG_ADMIN_FIRST];
    struct s1dg_access mmio_first[S1DG_MMIO_FIRST];
};
_Static_assert(sizeof(struct s1dg_entry) == 40, "S1DG entry layout");
_Static_assert(sizeof(struct s1dg_admin) == 72, "S1DG admin layout");
_Static_assert(sizeof(struct s1dg_access) == 24, "S1DG access layout");

/* Host-only adapter contract. map/unmap do not access hardware here. A failure
 * is ambiguous to a future platform adapter: it latches faulted, blocks every
 * MMIO request and suppresses IRQ; no automatic retry/physical fallback. */
struct s1_ops {
    void *(*guest_span)(void *opaque, u64 address, size_t length, bool write);
    bool (*map)(void *opaque, u64 address, size_t length);
    bool (*unmap)(void *opaque, u64 address, size_t length);
    void (*irq)(void *opaque, bool asserted);
    void (*publish)(void *opaque);
};
struct s1_model {
    struct vnvme_ctrl queue;
    const struct s1_ops *ops;
    void *opaque;
    u64 ecam, window, window_size, mapped_base;
    u16 command;
    u32 bar_lo, bar_hi;
    u8 intr_line;
    bool initialized, mapped, faulted, external_irq;
    struct { u32 intms, cc, csts, aqa; u64 asq, acq; } regs;
    u64 backend_reads, map_calls, unmap_calls, irq_changes;
    struct s1_diag diag; /* r2.1: keep last */
};
/* r2.1: published S1DG layout (checked on host and target builds; the decoder
 * relies on these numbers together with the ELF symbol `controller`). */
#define S1DG_MODEL_OFFSET 0x21088u
#define S1DG_SIZE 32232u
_Static_assert(offsetof(struct s1_model, diag) == S1DG_MODEL_OFFSET, "S1DG offset changed");
_Static_assert(sizeof(struct s1_diag) == S1DG_SIZE, "S1DG size changed");
_Static_assert(offsetof(struct s1_diag, ring) == 0x3e8, "S1DG ring offset");
_Static_assert(offsetof(struct s1_diag, admin_first) == 0x53e8, "S1DG admin offset");
_Static_assert(offsetof(struct s1_diag, mmio_first) == 0x65e8, "S1DG mmio offset");
/* r3: storage behind the S1 frontend. The default is the immutable RAM fixture; a
 * native read-only window may be selected once, before s1_init, by the native PREP
 * path. Kept outside struct s1_model so the published S1DG layout is unchanged. */
struct s1_backend {
    const char *name;
    u64 (*blocks)(void);
    bool (*read)(u64 lba, u32 blocks, void *buffer);
    void (*poll)(void);          /* EL2 tick: optional */
    void (*retire)(void);        /* storage retire/hv exit: optional */
};
bool s1_select_backend(const struct s1_backend *backend);
void s1_poison_backend(void);
const char *s1_backend_name(void);
u64 s1_backend_blocks(void);
void s1_backend_poll(void);
void s1_backend_retire(void);
/* Constructor requires a zero-initialized object; cannot reset/retry a live one. */
bool s1_init(struct s1_model *m, const struct s1_ops *ops, void *opaque,
             u64 ecam, u64 window, u64 window_size);
bool s1_config(struct s1_model *m, u64 address, unsigned bytes, bool write, u64 *value);
bool s1_mmio(struct s1_model *m, u64 address, unsigned bytes, bool write, u64 *value);
/* Internal protocol-model interface: no target functions or native ANS symbols. */
void s1_recompute_irq(struct s1_model *m);
void s1_reset_controller(struct s1_model *m);
bool s1_nvme_access(struct s1_model *m, u32 offset, unsigned bytes, bool write, u64 *value);
/* r2.1 diagnostics recorders (model-local values only). */
void s1dg_init(struct s1_model *m);
void s1dg_event(struct s1_model *m, u8 type, u8 width, u8 qid, u8 opcode, u16 cid, u16 status,
                u64 a, u64 b, u32 c, u32 d);
void s1dg_access(struct s1_model *m, u8 kind, u64 offset, unsigned bytes, bool write, bool ok, u64 value);
#endif
