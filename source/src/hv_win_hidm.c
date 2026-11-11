/* SPDX-License-Identifier: MIT */
#include "hv_win_hidm.h"
#include "mtp_kbd.h"
#include "mtp_tp_resident.h"
#include "hidm/tp/kbl.h"
#ifndef HIDM_HOST_TEST
#include "mtp_dma.h"
#else
void mtp_dma_rtkit_hook(struct rtkit_dev *rtk);
#endif
#ifdef HIDM_HOST_TEST
#include "hidm_host.h"
#else
#include "hv/hv.h"
#include "hv/win/hv_win.h"
#include "hv/win/hv_win_storage.h"
#include "kbl_resident.h"
#include "smp.h"
#include "utils.h"
#endif
/* Trap and tick already hold bhl (hv_exc_entry); configure is preguest only.
 * Never take another device lock while calling the vGIC, which uses bhl too. */
static hm_service service;
static struct mtp_kbd keyboard;
static bool attempted, exposed, retired, irq;
/* fnrow / keyboard backlight (2026-11-11): the backlight follows Ctrl+F1/F2 (media mode) and
 * input activity; display-brightness presses are counted (no DCP session while the guest runs,
 * so the panel level is not changed here; Windows receives consumer 0x6f/0x70). */
static struct kbl backlight;
static hm_u32 seen_kbl_up, seen_kbl_down, seen_input, seen_disp, disp_steps;
static hm_u64 kbl_log_next;
static void update_irq(void)
{
    bool level = exposed && !retired && hm_service_irq(&service);
    if (level != irq) { irq = level; hv_vgic3_set_spi_level(HM_GSIV, level); }
}
static void backend_changed(void *ctx) { (void)ctx; update_irq(); }
bool hv_win_hidm_claims_spi(u64 gsiv) { return attempted && gsiv == HM_GSIV; }
static bool access_page(struct exc_info *ctx, u64 address, u64 *value, bool write, int width)
{
    (void)ctx;
    unsigned page;
    u64 off;
    if (address >= HM_REG_BASE && address - HM_REG_BASE < 0x4000) {
        page = 0; off = address - HM_REG_BASE;
    } else if (address >= HM_DATA_BASE && address - HM_DATA_BASE < 0x4000) {
        page = 1; off = address - HM_DATA_BASE;
    } else return false;
    if (!value) return false;
    if (!write) {
        /* width is log2(bytes), including possible 16-byte accesses. */
        *value = 0;
        if (width == 4) value[1] = 0;
    }
    if (exposed && !retired && width == 2 && !(off & 3) && off <= HM_PAGE - 4) {
        if (write) hm_service_write(&service, page, off, (hm_u32)*value);
        else *value = hm_service_read(&service, page, off);
    }
    update_irq();
    return true; /* all padding/invalid-width accesses are RAZ/WI */
}
extern bool hv_started_cpus[];
s64 hv_win_hidm_configure(u64 version, u64 tp_fw, u64 tp_fw_len)
{
    if (attempted || version != HM_VERSION) return -1;
    for (unsigned i = 0; i < MAX_CPUS; i++) if (hv_started_cpus[i]) return -1;
    if (!hv_win_vgic_enabled() || hv_win_pl011_gsiv() == HM_GSIV ||
        hv_win_storage_claims_spi(HM_GSIV) || !hv_vgic3_spi_unassigned(HM_GSIV) ||
        hv_pt_walk(HM_REG_BASE) || hv_pt_walk(HM_DATA_BASE)) return -1;
    attempted = true;
    hm_zero(&service, sizeof(service)); /* valid zero-interface failure service */
    if (hv_map_hook(HM_REG_BASE, access_page, 0x4000) < 0 ||
        hv_map_hook(HM_DATA_BASE, access_page, 0x4000) < 0) return -1;
    mtp_tp_resident_arm(tp_fw, tp_fw_len); /* 0 = keyboard only, as before */
    mtp_kbd_dma_hook = mtp_dma_rtkit_hook;  /* host RTKit buffers via dart-mtp (tp1-a1 finding) */
    if (!mtp_kbd_resident_start(&keyboard, &service)) {
        hm_backend_interface ifs[2];
        hm_u32 n = 0;
        const hm_backend_interface *ptp = mtp_tp_resident_finish(&keyboard);
        ifs[n++] = keyboard.keyboard;
        if (ptp) ifs[n++] = *ptp;
        if (hm_service_init(&service, ifs, n, mtp_kbd_report_all, &keyboard) != HM_OK) {
            /* never lose the keyboard over the trackpad */
            if (n == 1 || hm_service_init(&service, ifs, 1, mtp_kbd_report_all, &keyboard) != HM_OK)
                mtp_kbd_fail(&keyboard, MTP_E_DESCRIPTOR);
            keyboard.tp = NULL;
        }
    }
#ifndef HIDM_HOST_TEST
    if (keyboard.phase == MTP_LIVE) {
        if (kbl_resident_start(&backlight, keyboard.io.now_us(keyboard.io.ctx)))
            hm_zero(&backlight, sizeof(backlight)); /* not ready: every kbl call is a no-op */
    }
#endif
    keyboard.io.input_changed = backend_changed;
    exposed = true; /* backend failure still supplies HELLO / COUNT=0 */
    update_irq();
    printf("HV/win HIDM: interfaces=%u backend=%u (unverified on hardware)\n",
           service.count, (unsigned)keyboard.error);
    return 0;
}
void hv_win_hidm_tick(void)
{
    if (!exposed || retired || smp_id() != boot_cpu_idx) return;
    mtp_kbd_poll(&keyboard);
    update_irq();
    if (keyboard.phase != MTP_LIVE) return;
    {
        hm_u64 now = keyboard.io.now_us(keyboard.io.ctx);
        /* counters only grow; a press and release between two ticks still counts once */
        while (seen_kbl_up != keyboard.kbl_up) { seen_kbl_up++; kbl_step(&backlight, 1, now); }
        while (seen_kbl_down != keyboard.kbl_down) { seen_kbl_down++; kbl_step(&backlight, -1, now); }
        if (seen_input != keyboard.input_seen) { seen_input = keyboard.input_seen; kbl_activity(&backlight, now); }
        if (seen_disp != keyboard.disp_up + keyboard.disp_down) {
            seen_disp = keyboard.disp_up + keyboard.disp_down;
            disp_steps++;
        }
        kbl_tick(&backlight, keyboard.fn.kbl_held, now);
        if (now >= kbl_log_next) {
            /* one summary line when something changed, at most every 2 s */
            static hm_u32 last[5];
            hm_u32 cur[5] = {backlight.steps, backlight.idle_offs, backlight.wakes, disp_steps,
                             keyboard.fnlock_toggles};
            unsigned i;
            for (i = 0; i < 5 && cur[i] == last[i]; i++) {}
            if (i < 5) {
                for (i = 0; i < 5; i++) last[i] = cur[i];
                printf("KBL: level %d lit %d steps %u idle-offs %u wakes %u writes %u | display keys up %u down %u"
                       " | fn-lock %s (toggles %u) | consumer reports %u\n",
                       backlight.level, backlight.lit, backlight.steps, backlight.idle_offs, backlight.wakes,
                       backlight.writes, keyboard.disp_up, keyboard.disp_down,
                       keyboard.fn.media ? "media" : "F-keys", keyboard.fnlock_toggles,
                       keyboard.fn.consumer_reports);
                kbl_log_next = now + 2000000;
            }
        }
    }
}
void hv_win_hidm_retire(void)
{
    if (!attempted) return;
    retired = true;
    kbl_stop(&backlight); /* light off when the hypervisor hands the machine back */
    update_irq();
    /* No MTP shutdown command; descriptors and state survive until full reset. */
}
