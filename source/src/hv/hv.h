/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef HV_H
#define HV_H

#include "exception.h"
#include "iodev.h"
#include "types.h"
#include "uartproxy.h"

enum hv_sptm_mode {
    SPTM_NONE = 0,
    SPTM_EMULATION = 1,
    SPTM_NESTED_VIRT = 2,
};

extern enum hv_sptm_mode hv_sptm_mode;
void hv_sptm_set_mode(enum hv_sptm_mode mode);

typedef bool(hv_hook_t)(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width);

#define MMIO_EVT_ATTR  GENMASK(31, 24)
#define MMIO_EVT_CPU   GENMASK(23, 16)
#define MMIO_EVT_SH    GENMASK(15, 14)
#define MMIO_EVT_MULTI BIT(6)
#define MMIO_EVT_WRITE BIT(5)
#define MMIO_EVT_WIDTH GENMASK(4, 0)

struct hv_evt_mmiotrace {
    u32 flags;
    u32 reserved;
    u64 pc;
    u64 addr;
    u64 data;
};

struct hv_evt_irqtrace {
    u32 flags;
    u16 type;
    u16 num;
};

#define HV_MAX_RW_SIZE  64
#define HV_MAX_RW_WORDS (HV_MAX_RW_SIZE >> 3)

struct hv_vm_proxy_hook_data {
    u32 flags;
    u32 id;
    u64 addr;
    u64 data[HV_MAX_RW_WORDS];
};

typedef enum _hv_entry_type {
    HV_HOOK_VM = 1,
    HV_VTIMER,
    HV_USER_INTERRUPT,
    HV_WDT_BARK,
    HV_CPU_SWITCH,
    HV_VIRTIO,
    HV_PANIC,
} hv_entry_type;

/* VM */
void hv_pt_init(void);
int hv_map(u64 from, u64 to, u64 size, u64 incr);
int hv_unmap(u64 from, u64 size);
int hv_map_hw(u64 from, u64 to, u64 size);
int hv_map_hw_nc(u64 from, u64 to, u64 size);
int hv_map_hw_ro(u64 from, u64 to, u64 size);
int hv_map_sw(u64 from, u64 to, u64 size);
int hv_map_hook(u64 from, hv_hook_t *hook, u64 size);
bool hv_pt_is_ram(u64 ipa);
int hv_pt_set_writable(u64 ipa, bool writable);
int hv_wp_prepare(u64 ipa, u64 size);
int hv_wp_set(u64 ipa, bool writable);
u64 hv_translate(u64 addr, bool s1only, bool w, u64 *par_out);
u64 hv_pt_walk(u64 addr);
/* Raw IPA-only S2 query. No AT instruction, allocation, AF update or permission change.
 * Caller holds the big HV lock (or no guest has started), with tables immutable.
 * Only valid hardware L2 blocks/L3 pages; software hooks/subpage tables fail.
 * Table pointers are trusted resident-owned page tables, not guest pointers. */
struct hv_s2_leaf {
    u64 ipa_base, pa_base, size, descriptor;
};
bool hv_pt_resolve_leaf(u64 ipa, struct hv_s2_leaf *out);

bool hv_emulate_pt_store(struct exc_info *ctx, u64 far, u64 ipa, u64 *bytes);
bool hv_handle_dabort(struct exc_info *ctx);
bool hv_pa_write(struct exc_info *ctx, u64 addr, u64 *val, int width);
bool hv_pa_read(struct exc_info *ctx, u64 addr, u64 *val, int width);
bool hv_pa_rw(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width);

/* Execute tracking of HW-mapped guest RAM, 16 KiB pages (hv_vm.c; policy in win/hv_win_pmu.c) */
#define HV_XT_PAGE_SIZE 0x4000UL
#define HV_XT_FLIPS_MAX 7U
enum hv_xt_state {
    HV_XT_UNTRACKED = 0,
    HV_XT_DIRTY,    /* RW, XN: an instruction fetch faults */
    HV_XT_SCANNING, /* RO, XN: transient, under the big HV lock */
    HV_XT_CODE,     /* RO, X: a write faults */
    HV_XT_PINNED,   /* RW, X: not tracked until re-armed */
};
int hv_xt_track(u64 ipa, u64 size, bool dry_run, u64 *new_tables);
enum hv_xt_state hv_xt_get(u64 ipa, u64 *pa, u32 *flips);
int hv_xt_set(u64 ipa, enum hv_xt_state state, u32 flips);
void hv_xt_tlbi(bool broadcast);
bool hv_xt_fault_ipa(struct exc_info *ctx, bool s1ptw, bool write, u64 *ipa);

/* AIC events through tracing the MMIO event address */
bool hv_trace_irq(u32 type, u32 num, u32 count, u32 flags);
void hv_aic_init(void);
bool hv_aic_hook(void);

/* AMCC interrupt filtering */
bool hv_amcc_init(void);
bool hv_amcc_handle_irq(u32 irq);

/* Virtual peripherals */
void hv_vuart_poll(void);
void hv_map_vuart(u64 base, int irq, iodev_id_t iodev);
struct virtio_conf;
void hv_map_virtio(u64 base, struct virtio_conf *conf);
void virtio_put_buffer(u64 base, int qu, u32 id, u32 len);

/* Exceptions */
void hv_exc_proxy(struct exc_info *ctx, uartproxy_boot_reason_t reason, u32 type, void *extra);
void hv_set_time_stealing(bool enabled, bool reset);
void hv_add_time(s64 time);
u64 hv_pmcr0_read(void);
void hv_pmcr0_write(u64 val);

/* WDT */
void hv_wdt_pet(void);
void hv_wdt_suspend(void);
void hv_wdt_resume(void);
void hv_wdt_init(void);
void hv_wdt_start(int cpu);
void hv_wdt_stop(void);
void hv_wdt_breadcrumb(char c);
void hv_do_panic(void);

#define hv_panic(fmt, ...)                                                                         \
    do {                                                                                           \
        debug_printf("HV panic:" fmt, ##__VA_ARGS__);                                              \
        hv_do_panic();                                                                             \
        flush_and_reboot();                                                                        \
    } while (0)

/* Utilities */
void hv_update_fiq(void);
void hv_write_hcr(u64 val);
void hv_write_hacr(u64 val);
void hv_write_avncr(u64 val);
u64 hv_get_spsr(void);
void hv_set_spsr(u64 val);
u64 hv_get_esr(void);
u64 hv_get_far(void);
u64 hv_get_elr(void);
u64 hv_get_afsr1(void);
void hv_set_elr(u64 val);

bool hv_is_nested(void);
extern bool hv_genter_trap_enabled;
void hv_set_genter_trap(bool enabled);
void hv_enable_genter_trap(void);
void hv_disable_genter_trap(void);

/* HV main */
void hv_init(void);
bool hv_cpu_power_init(void);
extern bool hv_vm_tmr_fiq_locked;
void hv_start(void *entry, u64 regs[4]);
void hv_start_secondary(int cpu, void *entry, u64 regs[4]);
void hv_exit_cpu(int cpu);
void hv_rendezvous(void);
bool hv_switch_cpu(int cpu);
void hv_pin_cpu(int cpu);
void hv_arm_tick(bool secondary);
void hv_rearm(void);
void hv_maybe_exit(void);
void hv_tick(struct exc_info *ctx);

#endif
