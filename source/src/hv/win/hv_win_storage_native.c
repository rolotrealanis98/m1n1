/* SPDX-License-Identifier: MIT */
/* r3: select the native read-only window backend for the S1 frontend.
 *
 * Contract (launcher): after hv_init/vGIC setup and immediately before P_HV_WIN_STORAGE_PREP,
 * call P_HV_WIN_STORAGE_NATIVE once with a struct s1n_window (56 bytes). On success the
 * subsequent PREP descriptor must carry blocks == window blocks; on any failure nothing is
 * selected, so a PREP with those blocks is refused (expose nothing). One shot per resident. */
#include "hv_win_storage.h"
#include "hv_win.h"
#include "s1/model.h"
#include "s1/native_window.h"
#include "s1/ans_ro.h"
#include "string.h"
#include "utils.h"
#include "s1/native_link.h"
#include "heapblock.h"

extern bool hv_started_cpus[];
#define NATIVE_MAX_CPUS 24 /* smp_asm.h MAX_CPUS */

static bool tried, selected;
static s64 last_result = 1; /* 1 = never called */

static const u32 ans_lines[] = {1347, 1348, 1349, 1350, 2338};

static void native_abort(void) { ans_ro_abort(ANS_RO_E_ABORT); }

static const struct s1_backend native_backend = {
    .name = "native read-only window (internal SSD GPT entry)",
    .blocks = s1n_blocks,
    .read = s1n_read,
    .poll = s1n_poll,
    .retire = s1n_close,
    .abort = native_abort,
};

/* r4: read-write window. Selected only when the contract set S1N_RW and the GPT verified. */
static void rw_caps(struct vnvme_caps *caps)
{
    caps->max_blocks = 256;   /* MDTS 8 = 1 MiB = device MDTS */
    caps->writable = true;
    caps->vwc = true;         /* the device has a volatile write cache; FLUSH is real */
    caps->native = true;
}
static const struct s1_backend native_rw_backend = {
    .name = "native read-write window (internal SSD GPT entry)",
    .blocks = s1n_blocks,
    .read = s1n_read,
    .poll = s1n_poll,
    .retire = s1n_close,
    .abort = native_abort,
    .write = s1n_write,
    .flush = s1n_flush,
    .xfer_buffer = s1n_xfer_buffer,
    .caps = rw_caps,
    .canary = s1n_canary,
};

static s64 refuse(void)
{
    last_result = -1;
    s1_poison_backend(); /* PREP now refuses every descriptor: expose nothing */
    printf("HV/win S1 native: refused; nothing exposed\n");
    return -1;
}

s64 hv_win_storage_native_select(u64 data, u64 size)
{
    if (!s1n_storage_context()) { ans_ro_abort(ANS_RO_E_CONTEXT); return -1; }
    s1n_link_lock(); /* one-way even when select fails */
    if (tried)
        return -1;
    tried = true;
    /* Must precede PREP (stat 0 bit 2 = PREP attempted). A late call changes nothing:
     * the already-prepared frontend keeps its backend. */
    if (hv_win_storage_stat(0) & 4) {
        last_result = -1;
        return -1;
    }
    if (!hv_win_vgic_enabled())
        return refuse();
    if (!data || (data & 7) || size != sizeof(struct s1n_window))
        return refuse();
    struct s1n_window w;
    memcpy(&w, (const void *)data, sizeof(w));
    /* ANS interrupt lines are owned by EL2 for the rest of this resident's life. */
    for (unsigned i = 0; i < sizeof(ans_lines) / sizeof(ans_lines[0]); i++)
        if (hv_vgic3_deny_aic_line(ans_lines[i]) < 0)
            return refuse();
    if (!s1n_open(&w)) {
        s1n_close(); /* clean stage-1 quiesce if ANS came up; no-op if refused/frozen */
        return refuse();
    }
    if (!s1_select_backend(s1n_writable() ? &native_rw_backend : &native_backend)) {
        s1n_close();
        return refuse();
    }
    selected = true;
    last_result = 0;
    printf("HV/win S1 native: window LBA %lu + %lu verified against live GPT (%s)\n",
           (unsigned long)w.start_lba, (unsigned long)w.blocks, s1n_writable() ? "read-write" : "read-only");
    return 0;
}

/* r3.1 R1: launcher `finally` path and hardware run 0. Retires the frontend (if prepared)
 * and runs the stage-1 quiesce. Refused while any guest CPU is started; a frozen driver is
 * never touched (returns -1: recover by platform reset). */
s64 hv_win_storage_native_quiesce(void)
{
    if (!s1n_storage_context()) { ans_ro_abort(ANS_RO_E_CONTEXT); return -1; }
    if (!selected)
        return -1;
    for (unsigned i = 0; i < NATIVE_MAX_CPUS; i++)
        if (hv_started_cpus[i])
            return -1;
    hv_win_storage_retire();
    return ans_ro_state() == ANS_RO_STOPPED ? 0 : -1;
}

u64 hv_win_storage_native_stat(u64 index)
{
    const struct s1n_status *s = s1n_status();
    const struct ans_ro_stats *a = ans_ro_stats();
    if (index >= 64 && index < 112) return ans_ro_sart_reg((index - 64) / 16, (index - 64) % 16);
    switch (index) {
    case 0: return ((u64)tried << 2) | ((u64)selected << 1) | (last_result == 0);
    case 1: return s->reason;
    case 2: return ((u64)a->state << 32) | a->reason;
    case 3: return s->start_lba;
    case 4: return s->blocks;
    case 5: return s->reads;
    case 6: return s->refused_reads;
    case 7: return a->commands;
    case 8: return a->read_errors;
    case 9: return a->polls;
    case 10: return a->mailbox_msgs;
    case 11: return ((u64)a->sart_protected << 32) | a->sart_free;
    case 12: return a->nsze;
    /* r4 */
    case 13: return ((u64)s->rw << 32) | s->max_xfer;
    case 14: return s->writes;
    case 15: return s->flushes;
    case 16: return a->w_abs_min_first;
    case 17: return a->w_abs_max_last;
    case 18: return a->ans_writes;
    case 19: return a->window_refusals;
    case 20: return a->write_errors;
    /* r4.1 (= r3.2) diagnostics */
    case 21: return a->identify_nsze;      /* r4.2+: not attempted, always UINT64_MAX */
    case 22: return a->identity_confirmed; /* r4.2+: not attempted, always 0 */
    /* r4.4 (= r3.5 15/16) */
    case 23: return a->rtkit_timeouts;     /* RTKIT_TIMEOUT_* bits: which RTKit wait timed out */
    case 24: return ((u64)a->tcb_alias_nonzero << 32) | a->tcb_alias_last; /* reg9+0x29120 diag */
    case 25: return a->ioq_regs_cleared;   /* +0x1200/+0x1208 zeroed at teardown */
    case 26: return S1N_PIN_START;
    case 27: return S1N_PIN_BLOCKS;
    case 28: return S1N_PIN_ENTRY;
    case 29: return S1N_STAGE;
    case 30: return s1n_link_refusals();
    case 31: return (u64)heapblock_peek();
    case 32: return heapblock_remaining();
    case 33: return a->cmd_reads;
    case 34: return a->cmd_writes;
    case 35: return a->cmd_flushes;
    case 36: return a->cmd_admin;
    case 37: return a->io_submitted;
    case 38: return a->io_completed;
    case 39: return a->blocks_written;
    case 40: return a->lowest_written;
    case 41: return a->highest_written;
    case 42: return a->cqe_anomalies;
    case 43: return a->aborts;
    case 44: return a->io_tags_used;
    default: return UINT64_MAX;
    }
}
