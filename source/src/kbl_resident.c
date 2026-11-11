/* SPDX-License-Identifier: MIT */
/* Resident side of the keyboard backlight (hidm/tp/kbl.h): ADT geometry check, FPWM0 power via
 * the ADT clock-gates (as Linux's power-domains = <&ps_fpwm0>), then the default level.
 * Preguest only. Writes: the FPWM0 power state (pmgr) and PWM CTRL/ON/OFF; nothing else. */
#include "kbl_resident.h"
#include "adt.h"
#include "pmgr.h"
#include "utils.h"

static void pwm_write32(void *ctx, hm_u64 addr, hm_u32 v)
{
    (void)ctx;
    /* only the three registers of /arm-io/pwm0 channel 0 */
    if (addr != KBL_PWM_BASE + KBL_PWM_CTRL && addr != KBL_PWM_BASE + KBL_PWM_ON &&
        addr != KBL_PWM_BASE + KBL_PWM_OFF)
        return;
    write32(addr, v);
}

int kbl_resident_start(struct kbl *b, hm_u64 now_us)
{
    int path[8], node;
    u64 base = 0, size = 0;
    const struct kbl_io io = {NULL, pwm_write32};

    hm_zero(b, sizeof(*b));
    node = adt_path_offset_trace(adt, "/arm-io/pwm0", path);
    if (node >= 0 && !adt_is_compatible(adt, node, "fpwm,s5l8920x"))
        printf("KBL: note: /arm-io/pwm0 is not compatible fpwm,s5l8920x (continuing on reg check)\n");
    if (node < 0 || adt_get_reg(adt, path, "reg", 0, &base, &size) || base != KBL_PWM_BASE || size < 0x20) {
        printf("KBL: /arm-io/pwm0 not as expected (node %d base %#lx size %#lx) -> backlight off\n",
               node, (unsigned long)base, (unsigned long)size);
        return -1;
    }
    if (adt_path_offset(adt, "/arm-io/pwm0/kbd-backlight") < 0) {
        printf("KBL: no /arm-io/pwm0/kbd-backlight -> backlight off\n");
        return -1;
    }
    int was_on = pmgr_power_is_on(0, "FPWM0");
    if (pmgr_adt_power_enable("/arm-io/pwm0") < 0) {
        printf("KBL: FPWM0 power enable failed -> backlight off\n");
        return -1;
    }
    printf("KBL: FPWM0 power %d -> %d; PWM before: CTRL %#x ON %u OFF %u\n", was_on,
           pmgr_power_is_on(0, "FPWM0"), read32(KBL_PWM_BASE + KBL_PWM_CTRL),
           read32(KBL_PWM_BASE + KBL_PWM_ON), read32(KBL_PWM_BASE + KBL_PWM_OFF));
    kbl_start(b, &io, now_us);
    printf("KBL: keyboard backlight level %d/%d (ON %u of %u cycles); after: CTRL %#x ON %u OFF %u\n",
           b->level, KBL_LEVELS, kbl_on_cycles(b->level), KBL_PERIOD_CYCLES,
           read32(KBL_PWM_BASE + KBL_PWM_CTRL), read32(KBL_PWM_BASE + KBL_PWM_ON),
           read32(KBL_PWM_BASE + KBL_PWM_OFF));
    return 0;
}
