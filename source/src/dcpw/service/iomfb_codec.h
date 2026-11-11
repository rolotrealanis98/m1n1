/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * iomfb_codec.h -- pure encoders/decoders for the DCP IOMFB protocol, firmware 27.3
 * (J716s, AppleDCP-1317.60.10). Freestanding, no allocation, no globals.
 *
 * Behaviour ported (not pasted) from the Asahi/Aurora drm/apple driver
 * (iomfb.h, iomfb.c, parser.c, iomfb_plane.h: GPL-2.0-only OR MIT; iomfb_v27.h:
 * GPL-2.0-only) and the owner's 27.3 patches (dcp-27.3/patches-kernel/0002,
 * vrr/0001, promotion/0001). Because the v27 swap layout comes from a
 * GPL-2.0-only file, this file is GPL-2.0-only and lives only in the resident.
 * It must never be compiled into a Windows driver. Citations: LINUX-MAP.md.
 */
#ifndef IOMFB_CODEC_H
#define IOMFB_CODEC_H

#include <stddef.h>
#include <stdint.h>

/* ---- RTKit endpoints and mailbox message words (REF/iomfb.h:43-70) ---- */
#define IOMFB_EP_REMOTE_ALLOC 0x2d
#define IOMFB_EP              0x37

enum iomfb_msg_type { IOMFB_T_SET_SHMEM = 0, IOMFB_T_INITIALIZED = 1, IOMFB_T_MSG = 2 };
enum iomfb_ctx {
	IOMFB_CTX_CB = 0, IOMFB_CTX_ASYNC_CB = 1, IOMFB_CTX_CMD = 2, IOMFB_CTX_ASYNC = 3,
	IOMFB_CTX_OOBCB = 4, IOMFB_CTX_OOBASYNC_CB = 5, IOMFB_CTX_OOBCMD = 6, IOMFB_CTX_OOBASYNC = 7,
	IOMFB_CTX_NUM
};

struct iomfb_msg {
	uint8_t type;
	uint8_t ctx;
	uint8_t ack;
	uint16_t offset;
	uint32_t length;
	uint64_t dva; /* SET_SHMEM only */
};

uint64_t iomfb_msg_set_shmem(uint64_t dva);
uint64_t iomfb_msg_rpc(unsigned ctx, uint32_t length, uint16_t offset);
uint64_t iomfb_msg_ack(unsigned ctx);
int iomfb_msg_decode(uint64_t w, struct iomfb_msg *out); /* 0 ok, -1 malformed */

/* ---- shared memory layout (REF/iomfb.c:44-70) ---- */
#define IOMFB_SHMEM_SIZE   0x100000u
#define IOMFB_PKT_ALIGN    0x40u
#define IOMFB_CHAN_SIZE    0x8000u
#define IOMFB_HDR_SIZE     12u
int iomfb_tx_offset(unsigned ctx);      /* where the AP writes commands / callback replies */
int iomfb_channel_offset(unsigned ctx); /* where packets of this context live */

/* Packet header: tag stored byte-reversed ("A463" -> "364A"), in_len, out_len. */
struct iomfb_pkt {
	char tag[5]; /* NUL-terminated, natural order */
	uint32_t in_len, out_len;
	const uint8_t *in;
	uint8_t *out;
};
size_t iomfb_pkt_write(uint8_t *chan, size_t chan_size, size_t off, const char tag[4],
                       const void *in, uint32_t in_len, uint32_t out_len); /* bytes used (unaligned) or 0 */
int iomfb_pkt_read(uint8_t *chan, size_t chan_size, size_t off, uint32_t msg_len, struct iomfb_pkt *p);

/* ---- fw 27.3 method tags actually used (dcp-27.3/patches-kernel/0002:136-178) ---- */
#define IOMFB_A_SWAP_START        "A456" /* 0x10 / 0x8 */
#define IOMFB_A_SWAP_SUBMIT       "A458" /* 0x1144 / 0xc */
#define IOMFB_A_SET_DIGITAL_OUT   "A463" /* 8 / 4 */
#define IOMFB_A_SET_PARAMETER     "A500" /* 0x28 / 4 */
#define IOMFB_A_SET_REFRESH_PROPS "A526" /* 0 / 4 */
#define IOMFB_D_PROP_START        "D126" /* 4 / 4 */
#define IOMFB_D_PROP_CHUNK        "D127" /* 0x1008 / 4 */
#define IOMFB_D_PROP_END          "D128" /* 0x40 / 4 */
#define IOMFB_D_SWAP_COMPLETE     "D594" /* 0x730 / 0 */

/* ---- request builders ---- */
#define IOMFB_DIGITAL_OUT_LEN   8u
#define IOMFB_SET_PARAM_LEN     0x28u
#define IOMFB_SWAP_START_LEN    0x10u
#define IOMFB_SWAP_START_OUT    8u
#define IOMFB_SWAP_SUBMIT_LEN   0x1144u
#define IOMFB_SWAP_SUBMIT_OUT   0xcu
#define IOMFB_PARAM_ADAPTIVE_SYNC_FW27 15u

void iomfb_build_digital_out(uint8_t out[8], uint32_t color_id, uint32_t timing_id);
/* param 15: {minRR 16.16 Hz, mediaTargetRate 0, fractional 0}, count 3 (vrr/0001; REF/iomfb_template.c:3230-3254) */
void iomfb_build_adaptive_sync(uint8_t out[0x28], uint32_t min_rr_q16);

/* fw 27.3 IOMFBSwap + surfaces + args (0x1144 bytes). Offsets: 27.2 v27 layout
 * (REF/iomfb_v27.h:10-79) shifted by the 27.3 gaps (dcp-27.3/0002:247-253). */
#define IOMFB_SW_TIMESTAMP0     0x000
#define IOMFB_SW_TIMESTAMP1     0x008
#define IOMFB_SW_TIMESTAMP6     0x030
#define IOMFB_SW_SWAP_ID        0x0d8 /* 27.2 0x098 + 0x40 */
#define IOMFB_SW_SURF_IDS       0x0dc
#define IOMFB_SW_SRC_RECT       0x0ec
#define IOMFB_SW_DST_RECT       0x14c
#define IOMFB_SW_SWAP_ENABLED   0x18c
#define IOMFB_SW_SWAP_COMPLETED 0x190
#define IOMFB_SW_BG_COLOR       0x194
#define IOMFB_SW_BL_UPDATE      0x3b4 /* 27.2 0x354 + 0x60: INFERRED from the gap rule, not traced */
#define IOMFB_SW_BL_FLAGS1      0x3b5
#define IOMFB_SW_BL_NITS        0x3be /* binary64 nits; 27.2 0x35e + 0x60: INFERRED */
#define IOMFB_SW_SURF0          0x830
#define IOMFB_SURF_STRIDE_V27   0x22c
#define IOMFB_SW_SURF_IOVA      0x10e0 /* 27.2 0xf90 + 0x150 */
#define IOMFB_SW_SWAP_NULL      0x113a /* 27.2 0xfea + 0x150 */
#define IOMFB_SW_SURF_NULL      0x113b
#define IOMFB_SW_UNK_PTR_NULL   0x1140
#define IOMFB_SW_UNK_OUT_NULL   0x1141
/* struct dcp_surface (packed, REF/iomfb_plane.h:137-162, 3 planes) */
#define IOMFB_SF_PREMULT        0x02
#define IOMFB_SF_PLANE_CNT      0x03
#define IOMFB_SF_PLANE_CNT2     0x07
#define IOMFB_SF_FORMAT         0x0b
#define IOMFB_SF_XFER           0x13
#define IOMFB_SF_COLORSPACE     0x14
#define IOMFB_SF_STRIDE         0x15
#define IOMFB_SF_PIX_SIZE       0x19
#define IOMFB_SF_PEL_W          0x1b
#define IOMFB_SF_PEL_H          0x1c
#define IOMFB_SF_WIDTH          0x21
#define IOMFB_SF_HEIGHT         0x25
#define IOMFB_SF_BUF_SIZE       0x29
#define IOMFB_SF_BASE_SIZE      0x1fd

#define IOMFB_FMT_BGRA   0x41524742u /* fourcc('A','R','G','B') = DCP_FORMAT_BGRA */
#define IOMFB_XFER_SDR   13u
#define IOMFB_CS_NATIVE  12u
#define IOMFB_SET_BG     (1u << 31)
#define IOMFB_FW27_DEADLINE 120u /* constant desktop deadline, promotion/0001 */

struct iomfb_swap_desc {
	uint32_t swap_id;
	uint64_t iova;         /* surface DVA in dart-disp0 SID 0 */
	uint32_t width, height, stride;
	int with_brightness;
	uint32_t bl_nits;      /* whole nits; encoded as binary64 with integer ops (no FP at EL2) */
};
int iomfb_build_swap_submit(uint8_t out[IOMFB_SWAP_SUBMIT_LEN], const struct iomfb_swap_desc *d);
uint64_t iomfb_u32_to_f64_bits(uint32_t v);

/* ---- DCP serialized dictionaries (REF/parser.c:20-80) ---- */
#define IOMFB_SER_HEADER 0xd3u
enum { IOMFB_SER_DICT = 1, IOMFB_SER_ARRAY = 2, IOMFB_SER_INT = 4, IOMFB_SER_STRING = 9,
       IOMFB_SER_BLOB = 10, IOMFB_SER_BOOL = 11 };

struct iomfb_timing {
	uint32_t id;
	uint32_t hactive, vactive, htotal, vtotal;
	uint32_t rate_q16;      /* VerticalAttributes.PreciseSyncRate (16.16 Hz) */
	uint32_t vrr_min_q16, vrr_max_q16;
	uint8_t preferred, promoted, is_virtual;
};
/* Parse a TimingElements blob. Returns the number of timings (<= max) or <0 on malformed input. */
int iomfb_parse_timings(const uint8_t *blob, size_t len, struct iomfb_timing *out, int max);

#endif
