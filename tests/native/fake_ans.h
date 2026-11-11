/* Fake T6050 ANS for host tests: ASC CPU, v4 mailbox + RTKit firmware, SARTv4, split NVMe/NVMMU
 * banks with Linux apple.c linear-SQ/TCB semantics, a fixed private arena, a fake ADT and a trace.
 * Never touches hardware. */
#ifndef FAKE_ANS_H
#define FAKE_ANS_H
#include "types.h"

#define F_ASC_CPU   0x419600000UL
#define F_ASC_SIZE  0x88000UL
#define F_MBOX      0x419608000UL
#define F_MBOX_SIZE 0x4000UL
#define F_NVME      0x45dcc0000UL
#define F_NVME_SIZE 0x10000UL
#define F_NVMMU     0x41dcc0000UL
#define F_NVMMU_SIZE 0x60000UL
#define F_SART      0x41dc50000UL
#define F_SART_SIZE 0xc000UL

#define ARENA_BASE 0x300000000UL
#define ARENA_SIZE (16UL << 20)

#define F_NSZE  244276265ULL
#define F_MODEL "APPLE SSD AP1024Z"
#define F_FW    "241.60.3"

enum fbank { B_NONE = 0, B_NVME, B_NVMMU, B_ASC, B_MBOX, B_SART };

struct fake_cfg {
    bool admin_error;
    bool duplicate_cqe, tamper_mailbox, corrupt_canary_read;
    bool cpu_running_at_entry;
    int protected_entries;      /* iBoot entries at indices 0..n-1 */
    bool nvmmu_mirror_on_reg9;  /* hypothesis: reg[9] decodes reg[3] layout beyond 0x10000 */
    u64 adt_reg0, adt_reg9, adt_sart;
    u32 sart_version;
    bool no_hello, ignore_sart_writes, boot_status_never_ok, rdy_never_set, rdy_never_clears;
    bool bad_model, bad_lbads, wrong_tag, error_status, tcb_stat_fail, corrupt_lba1;
    bool crash_on_read, no_quiesce_ack, no_power_on_ack;
    int cqe_delay;     /* MMIO accesses before a CQE becomes visible */
    bool cqe_never;
    /* r3 */
    int gpt_mutation;      /* enum gpt_mut */
    bool syslog_backpressure; /* refuse doorbells while > 32 syslog messages unacked */
    /* r3.1 */
    u64 hello_delay_us;       /* HELLO arrives this long after the INIT power message */
    u64 nsze_override;
    bool read_no_dma;         /* complete READs with success but without DMA */
    /* r4.4 (= r3.5): the r4.2/r3.3 `identify_bad` "poisoned admin queue" model is REMOVED: never
     * observed on hardware (r3.4 INDEPENDENT-REVIEW F3/M2). Any Identify reaching the fake is now
     * a forbidden opcode (test failure); its reply is unmodelled. */
    /* r4 */
    bool allow_rw;            /* WRITE/FLUSH are legitimate (rw contract); else forbidden */
    bool write_error;         /* fail WRITEs with a media error status */
    bool flush_error;
    bool shn_no_persist;      /* ANS SHN does not persist the cache (tests the driver's flush) */
    int flush_delay;          /* MMIO steps before a FLUSH completes (slow ANS flush) */
    /* r4.3: controller PRP/alignment model (fake_ans.c prp_check/data_dma). */
    int prp_model;            /* PRP_STRICT (default) | PRP_SPEC | PRP_MASK, as in r3.4 */
    /* r4.4 (= r3.5): RTKit handshake timing (ANS-T6050-CROSSCHECK D1; Linux J716s patch 0045 "about
     * 3 s", pcie_t6050.c:768 "~3 s"), in fake microseconds from the triggering host message. */
    u64 epmap_delay_us, power_on_delay_us;
    bool no_epmap;
    u32 tcb_alias_stat;       /* value reg9+0x29120 reads (default 0) */
};
enum prp_model { PRP_STRICT = 0, PRP_SPEC, PRP_MASK };
enum gpt_mut { GM_NONE = 0, GM_TYPE, GM_GUID, GM_START, GM_SIZE, GM_HDR_CRC, GM_ENT_CRC,
               GM_OVERLAP, GM_USABLE, GM_SIG, GM_NENTRIES, GM_CAPACITY, GM_DISK_GUID, GM_BACKUP, GM_BACKUP_ARRAY, GM_PRIMARY_HDR_CRC };
/* Live GPT geometry of the fake internal SSD (mirrors the J716s after the 2026-11-09 edit). */
#define W_START  174213120ULL
#define W_BLOCKS 24424454ULL
/* r4.6: guest writes of 1 + 1 + 256 + 256 blocks reach ANS as 1 + 1 + 8 + 8 commands of <= 32 blocks */
#define RW4_WRITES (2u + 2u * ((256u + 31u) / 32u))
extern const u8 W_TYPE[16], W_GUID[16];

struct ftrace { u8 bank; u8 write; u8 width; u32 off; u64 val; u64 seq; };

struct fake_state {
    struct fake_cfg cfg;
    u64 now, steps, step_cap;
    unsigned mailbox_tampers;
    u64 dsb_syncs;
    u64 clean_sqe, clean_tcb, clean_init_sqe, clean_init_tcb, clean_init_cqe, invalidate_cqe;
    /* trace */
    struct ftrace tr[200000];
    unsigned ntr;
    /* ASC / firmware */
    u32 cpu_control;
    int fw_state;       /* 0 stopped, 1 wait hello ack, 2 wait epmap reply, 3 started, 4 up, 9 dead */
    unsigned cpu_starts, cpu_stops, hellos;
    unsigned iop_power, ap_power;
    bool stopped_after_sleep_ack, stop_while_cc_en, stop_abrupt;
    unsigned bufs_needed, bufs_acked;
    u64 buf_iova[8], buf_size[8]; unsigned nbufs;
    unsigned syslog_unacked, syslog_sent;
    u64 out0[64]; u32 out1[64]; unsigned oh, ot;
    bool crashed;
    /* SART */
    u32 s_cfg[16], s_paddr[16], s_size[16];
    u32 s_init_cfg[16], s_init_paddr[16], s_init_size[16];
    unsigned prot_writes, sart_writes, sart_clear_while_live;
    /* NVMe */
    u32 cc, csts, aqa, ioqa, num_tcbs, boot_status;
    u64 asq, acq, asq_tcb, iosq_tcb, iosq_reg, iocq_reg;
    bool io_cq_valid, io_sq_valid;
    u64 io_cq_base; unsigned io_cq_size;
    unsigned acq_tail, acq_head, acq_phase, iocq_tail, iocq_head, iocq_phase;
    u8 tag_busy[64];    /* 0 free, 1 in flight, 2 completed awaiting inval */
    u32 tcb_stat;
    struct { bool admin; u8 tag; int left; u16 status; u64 result; } pend[8];
    unsigned npend;
    unsigned enables, base_write_while_en;
    /* policy counters */
    unsigned shn_writes, pmgr_resets, lsq_ctrl_access, unknown_nvme, unknown_nvmmu, unmapped;
    unsigned dma_violations, tcb_mismatch, tag_policy, tag_fault, forbidden_ops, mutating_ops, cfs_events;
    unsigned admin_ops[64], io_ops[64], nadmin, nio;
    u8 admin_op_seq[64], io_op_seq[64];
    unsigned io_reads, multi_block, bad_nsid;
    u64 dma_addrs[256]; unsigned ndma;
    unsigned dart_calls;
    unsigned tcb_alias_reads, refused_doorbells;
    u64 dma2_base, dma2_size;
    u64 hello_at; bool hello_pending;
    /* r4.4 (= r3.5) */
    struct { u64 m0, at; u32 ep; } late[4]; unsigned nlate;
    unsigned cq_flags_unproven, cc_unproven;
    /* r4.4: post-M4 I/O queue registers at teardown (aura nvme_shutdown order) */
    unsigned ioq_reg_zero_while_queues;  /* +0x1200/+0x1208 cleared while an I/O queue still exists */
    unsigned ioq_reg_zero_order_bad;     /* +0x1208 cleared before +0x1200 */
    unsigned fua_writes;                 /* ANS WRITE with FUA (cdw12 bit 30): never allowed */
    unsigned io_tag_seen[64];            /* I/O doorbells per tag */
    /* r4.3 alignment / PRP policy (see r3.4). */
    unsigned prp_rejects, misaligned_base, not16k, dma_outside, dma_into_queue, io_sq_size;
    u64 last_read_prp1, last_read_prp2, last_read_bytes; unsigned data_dmas;
    u64 rd_lo, rd_hi;         /* union of READ PRP1 extents since the test last cleared them */
    /* r4 writable disk */
    unsigned io_writes, io_flushes, writes_outside_window, uncleaned_writes, prp_errors, shn_persists;
    u64 write_blocks, wmin, wmax;
    u64 clean_lo, clean_hi, clean_seq, doorbell_seq, cache_ops;
    /* arena */
    u8 *arena; u64 arena_cursor; unsigned frees, allocs, frees_while_live;
};

extern struct fake_state F;
void fake_reset(const struct fake_cfg *cfg);
void fake_disk_block(u64 lba, u8 *out);
u32 fake_crc32(const u8 *p, size_t n);
bool fake_in_arena(u64 a, u64 len);
/* Runs fn; returns 1 if the step cap (an unbounded loop in the code under test) was hit. */
int fake_run_bounded(void (*fn)(void));
bool fake_sart_covers(u64 a, u64 len);
unsigned fake_sart_free_unprotected(void);
bool fake_sart_protected_unchanged(void);
void fake_set_dma_region2(u64 base, u64 size);
void fake_spontaneous_syslog(unsigned n);
void fake_power_loss(void);          /* drop the volatile write cache */
unsigned fake_cached_blocks(void);
unsigned fake_durable_blocks(void);
bool fake_overlay_inside_window(void); /* every written LBA lies in [W_START, W_START+W_BLOCKS) */
void fake_media_block(u64 lba, u8 *out); /* durable media only (what survives power loss) */
u64 fake_phys_of_pattern(const u8 *block);
bool fake_overlaps_queue(u64 a, u64 len);
#endif
