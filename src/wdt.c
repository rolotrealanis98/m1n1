/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "wdt.h"
#include "adt.h"
#include "string.h"
#include "types.h"
#include "utils.h"
#include "xnuboot.h"

#define WDT_COUNT 0x10
#define WDT_ALARM 0x14
#define WDT_CTL   0x1c

static u64 wdt_base = 0;
static bool wdt_kept = false;

// Dead-man switch for unattended runs: the proxy loop and the HV tick reset the count of a
// watchdog left armed by m1n1.wdt=keep, so only a wedged m1n1 lets it bite.
void wdt_kick(void)
{
    if (wdt_kept)
        write32(wdt_base + WDT_COUNT, 0);
}

void wdt_disable(void)
{
    int path[8];
    int node = adt_path_offset_trace(adt, "/arm-io/wdt", path);

    if (node < 0) {
        printf("WDT node not found!\n");
        return;
    }

    if (adt_get_reg(adt, path, "reg", 0, &wdt_base, NULL)) {
        printf("Failed to get WDT reg property!\n");
        return;
    }

    printf("Primary WDT register @ 0x%lx\n", wdt_base);
    // Experimental images are chainloaded with "m1n1.wdt=keep" after the host
    // armed the watchdog (hv/wdt-arm.py): leave it armed, so a hang anywhere
    // past this point still resets the machine into the installed stage-1.
    // The host disarms it once the new proxy answers (hv/wdt-arm.py --off).
    if (strstr(cur_boot_args.rv1.cmdline, "m1n1.wdt=keep")) {
        printf("Primary WDT left armed (m1n1.wdt=keep)\n");
        wdt_kept = true;
        return;
    }
    write32(wdt_base + WDT_CTL, 0);
    printf("Primary WDT disabled\n");

    // disable secondary watchdog if wdt-version is 2 or 3
    u32 wdt_version;
    if (ADT_GETPROP(adt, node, "wdt-version", &wdt_version) < 0)
        return;

    if (wdt_version == 2 || wdt_version == 3) {
        u64 wdt_2nd = 0;
        if (adt_get_reg(adt, path, "reg", 2, &wdt_2nd, NULL)) {
            printf("Failed to get WDT reg[2] property!\n");
            return;
        }

        printf("Secondary WDT register @ 0x%lx\n", wdt_2nd);
        write32(wdt_2nd, 0);
        printf("Secondary WDT disabled\n");
    }
}

void wdt_reboot(void)
{
    if (!wdt_base)
        return;

    write32(wdt_base + WDT_ALARM, 0x100000);
    write32(wdt_base + WDT_COUNT, 0);
    write32(wdt_base + WDT_CTL, 4);
}
