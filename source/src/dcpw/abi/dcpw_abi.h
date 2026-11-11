/* SPDX-License-Identifier: MIT */
/*
 * dcpw_abi.h -- guest <-> resident ABI of the DCPW display service (v1).
 *
 * The header wins over prose. Plain C99, no includes, little-endian, every
 * access an aligned 32-bit load/store (house pattern: input/hid-20261110/abi,
 * storage/native-miniport-20261110/abi).
 *
 * One 16 KiB register page, trapped by the resident. The guest fills the
 * request block, then stores the sequence number into DCPW_REG_DOORBELL; the
 * store does not retire until the response block is complete (synchronous,
 * like HIDM/ANSM). VSync and mode-change events raise one level-high virtual
 * SPI; the line is a pure function of IRQ_STATUS & IRQ_ENABLE.
 *
 * No guest pointers are dereferenced except the PRESENT surface GPA, which the
 * resident validates against guest RAM before use and never writes.
 */
#ifndef DCPW_ABI_H
#define DCPW_ABI_H

typedef unsigned char      dcpw_u8;
typedef unsigned short     dcpw_u16;
typedef unsigned int       dcpw_u32;
typedef unsigned long long dcpw_u64;

#define DCPW_STATIC_ASSERT(c, n) typedef char dcpw_sa_##n[(c) ? 1 : -1]

/* Platform placement (proposed; GSIV 68 is contested by HIDM/AGXW, 69 is HIDM's
 * hardware number, so DCPW takes 70). ACPI _HID "M5WD0001". */
#define DCPW_REG_BASE        0xF38200000ull
#define DCPW_REG_SIZE        0x4000u
#define DCPW_GSIV            70u
#define DCPW_ACPI_HID        "M5WD0001"

#define DCPW_MAGIC           0x57504344u /* "DCPW" */
#define DCPW_VERSION         1u

/* Register offsets (32-bit each). RO unless noted. Reserved: RAZ/WI. */
#define DCPW_REG_MAGIC       0x000
#define DCPW_REG_VERSION     0x004
#define DCPW_REG_STATUS      0x008 /* DCPW_ST_* */
#define DCPW_REG_CAPS        0x00C /* DCPW_CAP_* */
#define DCPW_REG_DOORBELL    0x010 /* WO: request seq (nonzero) */
#define DCPW_REG_IRQ_STATUS  0x014 /* RW1C: DCPW_IRQ_* */
#define DCPW_REG_IRQ_ENABLE  0x018 /* RW */
#define DCPW_REG_VBL_LO      0x020 /* vblank counter, latched on LO read */
#define DCPW_REG_VBL_HI      0x024
#define DCPW_REG_VBL_NS_LO   0x028 /* time of last vblank, resident ns clock */
#define DCPW_REG_VBL_NS_HI   0x02C
#define DCPW_REG_CUR_MODE    0x030 /* index into the mode table */
#define DCPW_REG_CUR_MHZ     0x034 /* current refresh, millihertz */
#define DCPW_REG_BL_MNITS    0x038 /* current brightness, millinits (0 = unknown) */
#define DCPW_REG_VRR         0x03C /* 1 = adaptive sync on */
#define DCPW_REG_BL_MAX_MNITS 0x040
#define DCPW_REG_PRESENTS_LO 0x044 /* completed presents */
#define DCPW_REG_MODE_COUNT  0x048

#define DCPW_REQ_OFF         0x100 /* struct dcpw_req, guest-written */
#define DCPW_RSP_OFF         0x180 /* struct dcpw_rsp, resident-written */
#define DCPW_MODES_OFF       0x200 /* struct dcpw_mode[DCPW_MAX_MODES] */
#define DCPW_MAX_MODES       16u

#define DCPW_ST_READY        (1u << 0) /* service configured */
#define DCPW_ST_LIVE         (1u << 1) /* backend owns the DCP session (M3+) */
#define DCPW_ST_FAILED       (1u << 2) /* backend failed; static fallback in force */

#define DCPW_CAP_MODESET     (1u << 0)
#define DCPW_CAP_VRR         (1u << 1)
#define DCPW_CAP_BRIGHTNESS  (1u << 2)
#define DCPW_CAP_FLIP        (1u << 3) /* PRESENT is a real DCP swap */
#define DCPW_CAP_HW_VBLANK   (1u << 4) /* vblank from swap_complete, not a timer */

#define DCPW_IRQ_VSYNC       (1u << 0)
#define DCPW_IRQ_MODE        (1u << 1)
#define DCPW_IRQ_PRESENT     (1u << 2) /* a PRESENT completed */
#define DCPW_IRQ_ERROR       (1u << 3)
#define DCPW_IRQ_ALL         0xFu

enum dcpw_op {
	DCPW_OP_NOP            = 0,
	DCPW_OP_SET_MODE       = 1, /* arg[0] = mode index */
	DCPW_OP_SET_VRR        = 2, /* arg[0] = 0/1 */
	DCPW_OP_SET_BRIGHTNESS = 3, /* arg[0] = millinits */
	DCPW_OP_PRESENT        = 4, /* arg[0..1] GPA, [2] stride, [3] w, [4] h, [5] fmt, [6..9] dirty x,y,w,h */
	DCPW_OP_REFRESH_MODES  = 5,
	DCPW_OP_MAX
};

enum dcpw_status {
	DCPW_OK          = 0,
	DCPW_E_BADSEQ    = 1,
	DCPW_E_BADOP     = 2,
	DCPW_E_RANGE     = 3,
	DCPW_E_UNSUP     = 4,
	DCPW_E_BUSY      = 5,
	DCPW_E_BACKEND   = 6,
	DCPW_E_NOTREADY  = 7,
};

#define DCPW_FMT_B8G8R8A8 1u /* the only v1 format: Windows D3DDDIFMT_A8R8G8B8 / X8R8G8B8 */

#define DCPW_REQ_ARGS 12u
struct dcpw_req {
	dcpw_u32 seq;
	dcpw_u32 op;
	dcpw_u32 arg[DCPW_REQ_ARGS];
	dcpw_u32 rsvd[18];
};
DCPW_STATIC_ASSERT(sizeof(struct dcpw_req) == 0x80, req);

struct dcpw_rsp {
	dcpw_u32 seq;
	dcpw_u32 status;
	dcpw_u32 ret[6];
	dcpw_u32 rsvd[24];
};
DCPW_STATIC_ASSERT(sizeof(struct dcpw_rsp) == 0x80, rsp);

#define DCPW_MODE_PREFERRED (1u << 0)
#define DCPW_MODE_VRR       (1u << 1) /* adaptive sync can be enabled in this mode */
#define DCPW_MODE_VALID     (1u << 31)
struct dcpw_mode {
	dcpw_u32 flags;
	dcpw_u32 timing_id;   /* DCP TimingElements ID */
	dcpw_u32 width, height;
	dcpw_u32 htotal, vtotal;
	dcpw_u32 refresh_mhz; /* millihertz */
	dcpw_u32 vrr_min_mhz; /* 0 when not VRR */
	dcpw_u32 vrr_max_mhz;
	dcpw_u32 rsvd[3];
};
DCPW_STATIC_ASSERT(sizeof(struct dcpw_mode) == 48, mode);
DCPW_STATIC_ASSERT(DCPW_MODES_OFF + DCPW_MAX_MODES * 48 <= DCPW_REG_SIZE, modes_fit);

#endif
