/* SPDX-License-Identifier: MIT */
#include "hv_win_hidm.h"
#include "mtp_kbd.h"
#ifdef HIDM_HOST_TEST
#include "hidm_host.h"
#else
#include "hv/hv.h"
#include "hv/win/hv_win.h"
#include "hv/win/hv_win_storage.h"
#include "smp.h"
#include "utils.h"
#endif
/* Trap and tick already hold bhl (hv_exc_entry); configure is preguest only.
 * Never take another device lock while calling the vGIC, which uses bhl too. */
static hm_service service;
static struct mtp_kbd keyboard;
static bool attempted, exposed, retired, irq;
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
s64 hv_win_hidm_configure(u64 version)
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
    if (!mtp_kbd_resident_start(&keyboard, &service) &&
        hm_service_init(&service, &keyboard.keyboard, 1, mtp_kbd_report, &keyboard) != HM_OK)
        mtp_kbd_fail(&keyboard, MTP_E_DESCRIPTOR);
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
}
void hv_win_hidm_retire(void)
{
    if (!attempted) return;
    retired = true;
    update_irq();
    /* No MTP shutdown command; descriptors and state survive until full reset. */
}
