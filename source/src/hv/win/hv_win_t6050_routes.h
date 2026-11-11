/* SPDX-License-Identifier: MIT */
/*
 * gpu-pt2: the T6050 AIC lines P_HV_WIN_MAP_IRQ may route to the guest. The boot firmware leaves the AIC
 * globally disabled on T6050 and EL2 enables it for the guest (hv_win.c hv_win_aic_enable_on_return), so
 * only lines measured on this machine are admitted. Pure function, shared with tests/win-gpu.
 *
 * Before gpu-pt2 the only admitted route was the Windows USB one (usb-drd1, AIC 1451 -> SPI 65), which made
 * the GPU's P_HV_WIN_MAP_IRQ(2797, 96) fail on hardware (run 20261111-102915-gpupt-gpu1-g1a).
 */
#ifndef HV_WIN_T6050_ROUTES_H
#define HV_WIN_T6050_ROUTES_H

#define HV_WIN_T6050_LINE_USB_DRD1  1451 /* docs/USB-HOST.md: SPI 65 only */
/* GPU mailboxes (t6050-j716s.dts; ADT gfx-asc/gfx1-asc interrupts[1], [2]): send-empty, recv-not-empty */
#define HV_WIN_T6050_LINE_GFX_SE    2797
#define HV_WIN_T6050_LINE_GFX_RNE   2800
#define HV_WIN_T6050_LINE_GFX1_SE   2801
#define HV_WIN_T6050_LINE_GFX1_RNE  2804

static inline int hv_win_t6050_route_allowed(unsigned long line, unsigned long gsiv)
{
    switch (line) {
        case HV_WIN_T6050_LINE_USB_DRD1:
            return gsiv == 65;
        case HV_WIN_T6050_LINE_GFX_SE:
        case HV_WIN_T6050_LINE_GFX_RNE:
        case HV_WIN_T6050_LINE_GFX1_SE:
        case HV_WIN_T6050_LINE_GFX1_RNE:
            return gsiv >= 32 && gsiv <= 1019; /* any SPI; duplicates are refused by hv_vgic3_map_irq */
        default:
            return 0;
    }
}

#endif
