/* SPDX-License-Identifier: MIT */
/* N1 (Apple Centauri Wi-Fi/BT) resident core: hardware abstraction. Everything the probe and the
 * bring-up core do to the machine goes through this table, so the host tests can run the same
 * code against a mock chip (tests/mock_n1.c) and every access can be logged/allow-listed.
 * Written from wireless/n1-20261111/LINUX-MAP.md (behaviour description); no Linux code. */
#ifndef N1_HAL_H
#define N1_HAL_H
#include <stdint.h>
#include <stddef.h>

typedef uint8_t n1_u8;
typedef uint16_t n1_u16;
typedef uint32_t n1_u32;
typedef uint64_t n1_u64;

/* Physical addresses, all resolved from the ADT by the resident (pcie_t6050_n1_addrs()). */
struct n1_addrs {
    n1_u64 ecam, rc, phy, phy_ctl;
    n1_u64 port[4];          /* GP port control blocks (port 0 = N1) */
    n1_u64 lane, i2axi;      /* port 0 lane PHY, intr2axi */
    n1_u64 perst, gpio_base; /* PERST# pad (pin 80) and the gpio0 block */
    n1_u32 perst_pin;
    n1_u64 dart;             /* /arm-io/dart-apcie0 reg[0] */
    n1_u32 reset_key, reset_template; /* SMC pKW4 of pci-bridge0 function-reset (gP13, 0x00800000) */
    n1_u64 storage[8];       /* ANS ctrl, ANS status, NVMe BOOT_STATUS, CC, CSTS, A2I, I2A, (0) */
    n1_u32 board_id, chip_id;/* /chosen board-id, chip-id: platform id = board << 16 | chip */
    n1_u8 mac[6];            /* /chosen mac-address-wifi0 */
    n1_u8 bdaddr[6];         /* /chosen mac-address-bluetooth0 (network order) */
};

/* Power-state names read through ps_read (PMGR PS registers, read only). */
enum n1_ps {
    N1_PS_GPIO, N1_PS_APCIE_GP, N1_PS_APCIE_SYS_GP, N1_PS_APCIE_PHY_SW, N1_PS_APHY_GP_AUS5_A,
    N1_PS_APHY_GP_AUS5, N1_PS_FAB6_SOC, N1_PS_ANS, N1_PS_APCIE_ST0, N1_PS_APCIE_SYS_ST0, N1_PS_COUNT
};

struct n1_hal {
    void *ctx;
    n1_u32 (*rd32)(void *ctx, n1_u64 pa);
    void (*wr32)(void *ctx, n1_u64 pa, n1_u32 v);
    n1_u32 (*ps_read)(void *ctx, enum n1_ps ps);         /* 0 if the device is unknown */
    int (*smc_read)(void *ctx, n1_u32 key, n1_u32 *val);  /* 0 = ok (one RTKit session) */
    int (*smc_write)(void *ctx, n1_u32 key, n1_u32 val);
    void (*udelay)(void *ctx, n1_u32 us);
    n1_u64 (*ticks_us)(void *ctx);
    /* DMA: allocate zeroed, physically contiguous, cache-coherent memory owned by the resident;
     * returns the CPU pointer and the device address (IOVA after the DART mapping for `sid`). */
    void *(*dma_alloc)(void *ctx, int sid, size_t size, size_t align, n1_u64 *iova);
    void (*dma_sync)(void *ctx);                          /* barrier: CPU writes visible to the device */
    /* PCIe port 0 operations the resident already implements (pcie_t6050.c). */
    int (*train)(void *ctx);                              /* cold training (m1n1 mode 4 path) */
    int (*port_cycle)(void *ctx);                         /* PME_Turn_Off, PERST#, retrain (post-ROM) */
    int (*route_sid)(void *ctx, n1_u32 rid, n1_u32 sid);  /* RID2SID slot = sid */
    void (*log)(void *ctx, const char *fmt, ...);
};

#define N1_RD(h, a) ((h)->rd32((h)->ctx, (a)))
#define N1_WR(h, a, v) ((h)->wr32((h)->ctx, (a), (v)))
#endif
