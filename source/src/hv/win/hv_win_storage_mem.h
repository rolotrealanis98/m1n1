/* SPDX-License-Identifier: MIT */
#ifndef HV_WIN_STORAGE_MEM_H
#define HV_WIN_STORAGE_MEM_H
#include "types.h"

#define HV_SM_MAX_RAM 64
#define HV_SM_MAX_PROTECTED 128
#define HV_SM_MAX_SPAN (128UL * 1024)
#define HV_SM_PAGE 0x4000UL

struct hv_sm_range { u64 base, size; };
enum hv_sm_state { HV_SM_EMPTY, HV_SM_READY, HV_SM_LEASED, HV_SM_DEAD };
/* Private resident-owned object; never guest writable. Input arrays are copied.
 * The controller supplies authoritative final launcher ranges, not guest tables.
 * protected MUST include immutable FD/media, ADT, bootargs, handoff, framebuffers,
 * carveouts and synthetic/physical MMIO. resident bounds cover EL2 and all heaps,
 * including future allocations. RAM must not intersect resident. Guest-owned
 * Mu boot-services workspace is permitted, except exact protected subranges.
 * This function validates a declaration; it cannot prove omitted ownership. */
struct hv_sm_config {
    u64 epoch;
    struct hv_sm_range resident;
    const struct hv_sm_range *ram, *protected;
    size_t nr_ram, nr_protected;
};
struct hv_storage_mem {
    enum hv_sm_state state;
    u64 epoch;
    int owner_cpu;
    struct hv_sm_range resident;
    size_t nr_ram, nr_protected;
    struct hv_sm_range ram[HV_SM_MAX_RAM], protected[HV_SM_MAX_PROTECTED];
};
/* Zero-initialized object, one registration only, before guest start. No alloc. */
bool hv_storage_mem_register(struct hv_storage_mem *m, const struct hv_sm_config *cfg);
/* Whole synchronous dispatch under bhl, boot vCPU stopped and IRQ/FIQ masked.
 * No callbacks may release bhl, service proxy requests, remap S2, run PMU scans,
 * or resume the guest until end. Returned pointers expire at end. No async DMA.
 * Controller must retire BEFORE guest reset/reconfigure or ownership/map change;
 * epoch belongs to that immutable launch contract, not a page-table attestation. */
bool hv_storage_mem_begin(struct hv_storage_mem *m, u64 epoch);
bool hv_storage_mem_end(struct hv_storage_mem *m);
void hv_storage_mem_retire(struct hv_storage_mem *m);
/* Exact S0/S1 guest_span ABI. Reads and writes both require actual RW pages;
 * an execute-tracked page counts as RW in the PMU rewrite's DIRTY, CODE and
 * PINNED states (never SCANNING). pmu-reads: a write admission resets the
 * admitted pages' rewrite state (CODE/PINNED -> DIRTY, permissions only, same
 * output address, broadcast TLBI; hv_win_pmu_dma_write) so code EL2 copies in is
 * rescanned before it runs; nothing else in S2, no cache attributes, no scans.
 * No guest VA translation. Identity IPA == PA prevents distinct admitted IPAs
 * aliasing the same physical bytes. */
void *hv_storage_guest_span(void *opaque, u64 ipa, size_t length, bool write);

/* r4.13 diagnostics: why hv_storage_guest_span last refused (reason, address, length, the offending range
 * or leaf) and per-reason counts. Pure bookkeeping, no behaviour change. */
enum hv_sm_why {
    HV_SM_WHY_NONE = 0, HV_SM_WHY_STATE, HV_SM_WHY_CONTEXT, HV_SM_WHY_ARGS, HV_SM_WHY_NOT_RAM,
    HV_SM_WHY_RESIDENT, HV_SM_WHY_PROTECTED, HV_SM_WHY_LEAF_WALK, HV_SM_WHY_LEAF_ATTR, HV_SM_WHY_SCANNING,
    HV_SM_WHY_NR,
};
struct hv_sm_refusal {
    u64 why;
    u64 ipa, length, detail; /* detail: protected range base, or the leaf descriptor */
    u64 count[HV_SM_WHY_NR];
};
extern struct hv_sm_refusal hv_sm_last_refusal;

#endif
