/* SPDX-License-Identifier: MIT */
#ifndef HV_WIN_STORAGE_H
#define HV_WIN_STORAGE_H
#include "types.h"
#define HV_S1_MAGIC 0x314e3553u
#define HV_S1_ECAM 0xf28000000UL
#define HV_S1_BAR 0xf30000000UL
#define HV_S1_GSIV 66u
struct hv_s1_descriptor {
    u32 magic, version;
    u64 ecam, bar, bar_size;
    u32 gsiv, flags, block_size, reserved0;
    u64 blocks, reserved1;
};
struct hv_s1_registration {
    struct hv_s1_descriptor device;
    u64 epoch, resident_base, resident_size;
    u32 nr_ram, nr_protected;
    /* Followed by nr_ram + nr_protected {u64 base,u64 size} pairs. */
};
_Static_assert(sizeof(struct hv_s1_descriptor)==64,"S1 handoff ABI");
_Static_assert(sizeof(struct hv_s1_registration)==96,"S1 registration ABI");
s64 hv_win_storage_prepare(u64 data, u64 size);
u64 hv_win_storage_stat(u64 index);
void hv_win_storage_retire(void);
/* r3 */
void hv_win_storage_tick(void);
s64 hv_win_storage_native_select(u64 data, u64 size);
/* r3.1 R1: stage-1 quiesce of a selected native backend; refused while a guest CPU runs */
s64 hv_win_storage_native_quiesce(void);
u64 hv_win_storage_native_stat(u64 index);
bool hv_win_storage_claims_spi(u32 gsiv);
bool hv_win_storage_guest_ram_contains(u64 addr, u64 size);
/* r4.11 diagnostics: periodic NVMe/backend status lines; per-tick change watch (prints at once).
 * The two printers live in hv_win_status.c (firmware only); the model view in hv_win_storage.c. */
struct s1_model;
const struct s1_model *hv_win_storage_model(bool *is_attempted, bool *is_ready);
void hv_win_storage_status_lines(const char *prefix);
bool hv_win_storage_watch(void); /* true: printed (caller flushes the console) */
/* Existing-device ownership queries, no mutation. */
bool hv_vgic3_spi_unassigned(u32 intid);
u32 hv_win_pl011_gsiv(void);
#endif
