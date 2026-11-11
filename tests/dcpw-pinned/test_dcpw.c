/* SPDX-License-Identifier: GPL-2.0-only */
/* test_dcpw.c -- host tests for the DCPW service, the IOMFB codec and session (our code). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fake_dcp.h"
#include "../service/dcpw_static.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define GROUP(n) printf("== %s\n", n)

static uint32_t g32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t g64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }

/* ---------------- env ---------------- */
static int irq_level, irq_calls;
static void env_irq(void *c, int l) { (void)c; irq_level = l; irq_calls++; }
static int env_gpa_ok(void *c, uint64_t gpa, uint64_t len) { (void)c; return gpa >= 0x80000000ull && len <= 0x40000000ull && gpa - 0x80000000ull <= 0x40000000ull - len; }
static struct dcpw_env ENV = { 0, env_gpa_ok, env_irq };

static uint32_t seqno = 1;
static uint32_t req(struct dcpw_dev *d, uint32_t op, const uint32_t *args, int n, uint32_t *ret0)
{
	uint32_t s = seqno++;
	dcpw_write32(d, DCPW_REQ_OFF + 0, s, 0);
	dcpw_write32(d, DCPW_REQ_OFF + 4, op, 0);
	for (int i = 0; i < (int)DCPW_REQ_ARGS; i++)
		dcpw_write32(d, DCPW_REQ_OFF + 8 + 4 * i, i < n ? args[i] : 0, 0);
	dcpw_write32(d, DCPW_REG_DOORBELL, s, 0);
	if (dcpw_read32(d, DCPW_RSP_OFF) != s) return 0xdead;
	if (ret0) *ret0 = dcpw_read32(d, DCPW_RSP_OFF + 8);
	return dcpw_read32(d, DCPW_RSP_OFF + 4);
}

static void present_args(uint32_t *a, uint64_t gpa, uint32_t stride, uint32_t w, uint32_t h)
{
	memset(a, 0, 4 * DCPW_REQ_ARGS);
	a[0] = (uint32_t)gpa; a[1] = (uint32_t)(gpa >> 32); a[2] = stride; a[3] = w; a[4] = h; a[5] = DCPW_FMT_B8G8R8A8;
	a[6] = 0; a[7] = 0; a[8] = w; a[9] = h;
}

/* ---------------- codec ---------------- */
static void test_codec(void)
{
	GROUP("codec: mailbox words (macOS 27.3 capture vectors, DCP-UNDER-HV.md:69-79)");
	struct iomfb_msg m;
	CHECK(iomfb_msg_set_shmem(0x10000044000ull) == 0x0100000440000040ull);
	CHECK(iomfb_msg_decode(0x0100000440000040ull, &m) == 0 && m.type == IOMFB_T_SET_SHMEM && m.dva == 0x10000044000ull);
	CHECK(iomfb_msg_decode(0x0040227b39590101ull, &m) == 0 && m.type == IOMFB_T_INITIALIZED);
	uint64_t w = iomfb_msg_rpc(IOMFB_CTX_CMD, 0x1150, 0x40);
	CHECK(iomfb_msg_decode(w, &m) == 0 && m.type == 2 && m.ctx == 2 && m.offset == 0x40 && m.length == 0x1150 && !m.ack);
	CHECK(iomfb_msg_decode(iomfb_msg_ack(IOMFB_CTX_CB), &m) == 0 && m.ack && m.ctx == 0);
	CHECK(iomfb_msg_decode(0x0000000000000003ull, &m) < 0);
	CHECK(iomfb_msg_decode(0x0000000000000f02ull, &m) < 0);  /* ctx 15 */
	CHECK(iomfb_msg_decode(0x0100000440000010ull, &m) < 0);  /* SET_SHMEM flag != 4 */
	CHECK(iomfb_channel_offset(IOMFB_CTX_CB) == 0x60000 && iomfb_tx_offset(IOMFB_CTX_CMD) == 0 &&
	      iomfb_channel_offset(IOMFB_CTX_ASYNC) == 0x40000 && iomfb_tx_offset(IOMFB_CTX_ASYNC) < 0);

	GROUP("codec: packets");
	static uint8_t chan[IOMFB_CHAN_SIZE];
	uint8_t in[8] = { 1, 0, 0, 0, 2, 0, 0, 0 };
	size_t used = iomfb_pkt_write(chan, sizeof(chan), 0, "A463", in, 8, 4);
	CHECK(used == 24 && !memcmp(chan, "364A", 4) && g32(chan + 4) == 8 && g32(chan + 8) == 4 && g32(chan + 20) == 0xffffffffu);
	struct iomfb_pkt p;
	CHECK(iomfb_pkt_read(chan, sizeof(chan), 0, 24, &p) == 0 && !strcmp(p.tag, "A463") && p.in_len == 8 && p.out == chan + 20);
	CHECK(iomfb_pkt_read(chan, sizeof(chan), 0, 25, &p) < 0);       /* length mismatch */
	CHECK(iomfb_pkt_write(chan, sizeof(chan), 0x10, "A463", in, 8, 4) == 0); /* misaligned */
	CHECK(iomfb_pkt_write(chan, sizeof(chan), IOMFB_CHAN_SIZE - 0x40, "A458", 0, 0x1144, 0xc) == 0);
	memcpy(chan, "36ZA", 4);
	CHECK(iomfb_pkt_read(chan, sizeof(chan), 0, 0, &p) < 0);        /* non-digit tag */
	memcpy(chan, "364A", 4); memcpy(chan + 4, "\xff\xff\xff\x7f", 4);
	CHECK(iomfb_pkt_read(chan, sizeof(chan), 0, 0, &p) < 0);        /* huge in_len */

	GROUP("codec: builders");
	uint8_t a[0x28];
	iomfb_build_adaptive_sync(a, 24u << 16);
	CHECK(g32(a) == 15 && g32(a + 4) == 0x180000 && g32(a + 8) == 0 && g32(a + 0x24) == 3); /* "minRR = 0x180000" (vrrflip-phases.txt:27-42) */
	uint8_t dm[8];
	iomfb_build_digital_out(dm, 1, 2);
	CHECK(g32(dm) == 1 && g32(dm + 4) == 2);
	CHECK(iomfb_u32_to_f64_bits(300) == 0x4072C00000000000ull);
	CHECK(iomfb_u32_to_f64_bits(1) == 0x3FF0000000000000ull && iomfb_u32_to_f64_bits(0) == 0);
	CHECK(iomfb_u32_to_f64_bits(600) == 0x4082C00000000000ull && iomfb_u32_to_f64_bits(0xffffffffu) == 0x41EFFFFFFFE00000ull);
	static uint8_t sw[IOMFB_SWAP_SUBMIT_LEN];
	struct iomfb_swap_desc d = { 7, 0xf00004000ull, 3456, 2234, 13824, 1, 450 };
	CHECK(iomfb_build_swap_submit(sw, &d) == 0);
	CHECK(IOMFB_SW_SURF0 + 4 * IOMFB_SURF_STRIDE_V27 == IOMFB_SW_SURF_IOVA);       /* 0x830 + 0x8b0 */
	CHECK(IOMFB_SW_SWAP_NULL - IOMFB_SW_SURF_IOVA == 0xfea - 0xf90);               /* tail unchanged from 27.2 */
	CHECK(IOMFB_SW_SWAP_ID - 0x98 == 0x40 && IOMFB_SW_BL_UPDATE - 0x354 == 0x60);
	CHECK(IOMFB_SF_BASE_SIZE + 47 == IOMFB_SURF_STRIDE_V27);
	CHECK(g64(sw) == 120 && g64(sw + 8) == 120 && g64(sw + 0x30) == 120);
	CHECK(g32(sw + IOMFB_SW_SWAP_ID) == 7 && g32(sw + IOMFB_SW_SWAP_ENABLED) == 1);
	CHECK(g32(sw + IOMFB_SW_DST_RECT + 8) == 3456 && g32(sw + IOMFB_SW_DST_RECT + 12) == 2234);
	CHECK(sw[IOMFB_SW_BL_UPDATE] == 1 && g64(sw + IOMFB_SW_BL_NITS) == iomfb_u32_to_f64_bits(450));
	CHECK(g32(sw + IOMFB_SW_SURF0 + IOMFB_SF_FORMAT) == IOMFB_FMT_BGRA && g32(sw + IOMFB_SW_SURF0 + IOMFB_SF_STRIDE) == 13824);
	CHECK(g32(sw + IOMFB_SW_SURF0 + IOMFB_SF_BUF_SIZE) == 13824u * 2234u);
	CHECK(g64(sw + IOMFB_SW_SURF_IOVA) == 0xf00004000ull && sw[IOMFB_SW_SURF_NULL] == 0 && sw[IOMFB_SW_SURF_NULL + 1] == 1);
	/* the three 27.3 gap ranges are zero */
	int gz = 1;
	for (int i = 0x98; i < 0xd8; i++) gz &= sw[i] == 0;
	for (int i = 0x298 + 0x40; i < 0x2cc + 0x60; i++) gz &= sw[i] == 0;
	CHECK(gz);
	d.stride = 13820; CHECK(iomfb_build_swap_submit(sw, &d) < 0);
	d.stride = 13824; d.iova = 0; CHECK(iomfb_build_swap_submit(sw, &d) < 0);
	d.iova = 1; d.width = 0; CHECK(iomfb_build_swap_submit(sw, &d) < 0);
}

/* ---------------- parser ---------------- */
static void test_parser(void)
{
	GROUP("parser: the six J716s timings");
	static uint8_t blob[16384];
	uint32_t n = j716s_timing_blob(blob, sizeof(blob));
	CHECK(n > 0);
	struct iomfb_timing t[16];
	int k = iomfb_parse_timings(blob, n, t, 16);
	CHECK(k == 6);
	CHECK(t[0].id == 2 && t[0].hactive == 3456 && t[0].vactive == 2234 && t[0].htotal == 3536 && t[0].vtotal == 2456);
	CHECK(t[0].rate_q16 == (120u << 16) && t[0].preferred && !t[0].promoted);
	CHECK(t[5].id == 7 && t[5].vtotal == 2946 && t[5].rate_q16 == (50u << 16));
	CHECK(iomfb_parse_timings(blob, n, t, 2) == 2);
	struct dcpw_mode m[16];
	uint32_t cur = 99;
	int nm = iomfb_timings_to_modes(t, k, m, 16, 2, &cur);
	CHECK(nm == 6 && cur == 0 && m[0].refresh_mhz == 120000 && (m[0].flags & DCPW_MODE_VRR) &&
	      m[0].vrr_min_mhz == 24000 && m[0].vrr_max_mhz == 120000);
	CHECK(m[3].refresh_mhz == 59940 && !(m[3].flags & DCPW_MODE_VRR) && m[4].refresh_mhz == 47952);
	CHECK(iomfb_timings_to_modes(t, k, m, 16, 99, &cur) < 0);

	GROUP("parser: malformed input (truncation every byte, 20000 random mutations)");
	int bad_ok = 1;
	for (uint32_t cut = 0; cut < n; cut++) {
		int r = iomfb_parse_timings(blob, cut, t, 16);
		if (r >= 0 && cut < n - 8) bad_ok = 0; /* a truncated blob must not parse fully */
	}
	CHECK(bad_ok);
	static uint8_t mut[16384];
	int crashes_would_show_in_asan = 0;
	for (int i = 0; i < 20000; i++) {
		memcpy(mut, blob, n);
		int flips = 1 + rnd() % 8;
		for (int f = 0; f < flips; f++) {
			uint32_t pos = rnd() % n;
			mut[pos] = (uint8_t)((rnd() & 1) ? rnd() : (mut[pos] ^ (1u << (rnd() % 8))));
		}
		int r = iomfb_parse_timings(mut, n, t, 16);
		if (r > 16) crashes_would_show_in_asan++;
	}
	CHECK(crashes_would_show_in_asan == 0);
	uint8_t nested[4 + 4 * 40 + 12];
	uint32_t hdr = 0xd3; memcpy(nested, &hdr, 4);
	for (int i = 0; i < 40; i++) { uint32_t tg = 1u | (IOMFB_SER_ARRAY << 24); memcpy(nested + 4 + 4 * i, &tg, 4); }
	{ uint32_t it = 8u | (IOMFB_SER_INT << 24); memcpy(nested + 4 + 160, &it, 4); memset(nested + 168, 0, 8); }
	CHECK(iomfb_parse_timings(nested, sizeof(nested), t, 16) < 0); /* depth bound (well-formed otherwise) */
	uint8_t padded[16];
	{ uint32_t w4[4] = { 0xd3, 1u | (IOMFB_SER_ARRAY << 24), 1u | (1u << 29) | (IOMFB_SER_BOOL << 24), 0 }; memcpy(padded, w4, 16); }
	CHECK(iomfb_parse_timings(padded, 12, t, 16) < 0);           /* padding bits set */
	padded[11] = IOMFB_SER_BOOL;
	CHECK(iomfb_parse_timings(padded, 12, t, 16) == 0);          /* same without padding: empty, ok */
	uint32_t huge[3] = { 0xd3, 0xffffffu | (IOMFB_SER_ARRAY << 24), 0 };
	CHECK(iomfb_parse_timings((uint8_t *)huge, sizeof(huge), t, 16) < 0);
}

/* ---------------- core with the static backend ---------------- */
static int hook_calls;
static int hook(void *c, const struct dcpw_present *p) { (void)c; (void)p; hook_calls++; return 0; }

static void test_core_static(void)
{
	GROUP("core: static backend (no DCP traffic)");
	struct dcpw_static st = { hook, 0, {0}, 0 };
	struct dcpw_backend be;
	dcpw_static_backend(&st, &be);
	static struct dcpw_dev d;
	CHECK(dcpw_configure(&d, &be, &ENV, 600000, 300000, 0) == 0);
	CHECK(dcpw_read32(&d, DCPW_REG_MAGIC) == DCPW_MAGIC && dcpw_read32(&d, DCPW_REG_VERSION) == 1);
	CHECK(dcpw_read32(&d, DCPW_REG_STATUS) == DCPW_ST_READY && dcpw_read32(&d, DCPW_REG_CAPS) == 0);
	CHECK(dcpw_read32(&d, DCPW_REG_MODE_COUNT) == 6 && dcpw_read32(&d, DCPW_REG_CUR_MHZ) == 120000);
	CHECK(dcpw_read32(&d, DCPW_MODES_OFF + 4) == 2 && dcpw_read32(&d, DCPW_MODES_OFF + 8) == 3456);
	CHECK((dcpw_read32(&d, DCPW_MODES_OFF) & DCPW_MODE_VRR) == 0);      /* stripped: no VRR cap */
	CHECK(dcpw_read32(&d, DCPW_MODES_OFF + 48 * 6) == 0);               /* past the table: zero */
	CHECK(dcpw_read32(&d, DCPW_REG_BL_MAX_MNITS) == 0 && dcpw_read32(&d, DCPW_REG_BL_MNITS) == 0);
	uint32_t a[DCPW_REQ_ARGS], r0 = 0;
	a[0] = 0;
	CHECK(req(&d, DCPW_OP_NOP, a, 0, 0) == DCPW_OK);
	a[0] = 0; CHECK(req(&d, DCPW_OP_SET_MODE, a, 1, 0) == DCPW_OK);    /* already current */
	a[0] = 1; CHECK(req(&d, DCPW_OP_SET_MODE, a, 1, 0) == DCPW_E_UNSUP);
	a[0] = 6; CHECK(req(&d, DCPW_OP_SET_MODE, a, 1, 0) == DCPW_E_RANGE);
	a[0] = 1; CHECK(req(&d, DCPW_OP_SET_VRR, a, 1, 0) == DCPW_E_UNSUP);
	a[0] = 2; CHECK(req(&d, DCPW_OP_SET_VRR, a, 1, 0) == DCPW_E_RANGE);
	a[0] = 100000; CHECK(req(&d, DCPW_OP_SET_BRIGHTNESS, a, 1, 0) == DCPW_E_UNSUP);
	CHECK(req(&d, 99, a, 0, 0) == DCPW_E_BADOP);

	GROUP("core: doorbell sequencing");
	dcpw_write32(&d, DCPW_REQ_OFF, 77, 0); dcpw_write32(&d, DCPW_REQ_OFF + 4, DCPW_OP_NOP, 0);
	dcpw_write32(&d, DCPW_REG_DOORBELL, 78, 0);
	CHECK(dcpw_read32(&d, DCPW_RSP_OFF + 4) == DCPW_E_BADSEQ);
	dcpw_write32(&d, DCPW_REQ_OFF, 0, 0); dcpw_write32(&d, DCPW_REG_DOORBELL, 0, 0);
	CHECK(dcpw_read32(&d, DCPW_RSP_OFF + 4) == DCPW_E_BADSEQ);

	GROUP("core: PRESENT validation");
	present_args(a, 0x80000000ull, 13824, 3456, 2234);
	CHECK(req(&d, DCPW_OP_PRESENT, a, 10, &r0) == DCPW_OK && r0 == 1 && hook_calls == 1);
	CHECK(dcpw_read32(&d, DCPW_REG_IRQ_STATUS) & DCPW_IRQ_PRESENT);
	present_args(a, 0x80001000ull, 13824, 3456, 2234); CHECK(req(&d, DCPW_OP_PRESENT, a, 10, 0) == DCPW_E_RANGE); /* not 16K */
	present_args(a, 0x80000000ull, 13800, 3456, 2234); CHECK(req(&d, DCPW_OP_PRESENT, a, 10, 0) == DCPW_E_RANGE); /* stride */
	present_args(a, 0x80000000ull, 13824, 1728, 1117); CHECK(req(&d, DCPW_OP_PRESENT, a, 10, 0) == DCPW_E_RANGE); /* not the mode */
	present_args(a, 0xbfffc000ull, 13824, 3456, 2234); CHECK(req(&d, DCPW_OP_PRESENT, a, 10, 0) == DCPW_E_RANGE); /* past RAM */
	present_args(a, 0xffffffffffffc000ull, 13824, 3456, 2234); CHECK(req(&d, DCPW_OP_PRESENT, a, 10, 0) == DCPW_E_RANGE); /* wraps */
	present_args(a, 0x80000000ull, 13824, 3456, 2234); a[5] = 2; CHECK(req(&d, DCPW_OP_PRESENT, a, 10, 0) == DCPW_E_UNSUP);
	present_args(a, 0x80000000ull, 13824, 3456, 2234); a[6] = 3000; a[8] = 457; CHECK(req(&d, DCPW_OP_PRESENT, a, 10, 0) == DCPW_E_RANGE);
	present_args(a, 0x80000000ull, 13824, 3456, 2234); a[8] = 0; CHECK(req(&d, DCPW_OP_PRESENT, a, 10, 0) == DCPW_E_RANGE);
	CHECK(hook_calls == 1);

	GROUP("core: IRQ line and timer vblank at 120 Hz");
	dcpw_write32(&d, DCPW_REG_IRQ_STATUS, DCPW_IRQ_ALL, 0);
	CHECK(dcpw_read32(&d, DCPW_REG_IRQ_STATUS) == 0 && irq_level == 0);
	dcpw_write32(&d, DCPW_REG_IRQ_ENABLE, 0xffffffff, 0);
	CHECK(dcpw_read32(&d, DCPW_REG_IRQ_ENABLE) == DCPW_IRQ_ALL && irq_calls > 0);
	CHECK(dcpw_period_ns(120000) == 8333333 && dcpw_period_ns(59940) == 16683350);
	dcpw_tick(&d, 8333332);
	CHECK(dcpw_read32(&d, DCPW_REG_VBL_LO) == 0 && irq_level == 0);
	dcpw_tick(&d, 8333333);
	CHECK(dcpw_read32(&d, DCPW_REG_VBL_LO) == 1 && irq_level == 1 && dcpw_read32(&d, DCPW_REG_VBL_NS_LO) == 8333333);
	dcpw_write32(&d, DCPW_REG_IRQ_STATUS, DCPW_IRQ_VSYNC, 0);
	CHECK(irq_level == 0);
	dcpw_tick(&d, 8333333ull * 10 + 5); /* 9 periods later: counts the missed ones once */
	CHECK(dcpw_read32(&d, DCPW_REG_VBL_LO) == 10 && dcpw_read32(&d, DCPW_REG_VBL_NS_LO) == 83333330u);
	dcpw_write32(&d, DCPW_REG_IRQ_ENABLE, 0, 0);
	CHECK(irq_level == 0);
	uint32_t ba = d.stat_bad_access;
	dcpw_read32(&d, 2); dcpw_write32(&d, 0x4000, 1, 0);
	CHECK(d.stat_bad_access == ba + 2);
	dcpw_write32(&d, DCPW_REG_MAGIC, 0, 0);
	CHECK(dcpw_read32(&d, DCPW_REG_MAGIC) == DCPW_MAGIC);

	GROUP("core: configure refuses bad backends");
	struct dcpw_backend bad = be; bad.present = 0;
	static struct dcpw_dev d2;
	CHECK(dcpw_configure(&d2, &bad, &ENV, 0, 0, 0) < 0 && dcpw_read32(&d2, DCPW_REG_STATUS) == 0);
	CHECK(req(&d2, DCPW_OP_NOP, a, 0, 0) == DCPW_E_NOTREADY);
}

/* ---------------- session against the fake DCP ---------------- */
static void test_session(void)
{
	GROUP("session: DCPAV TimingElements through D126-D128");
	static uint8_t shmem[IOMFB_SHMEM_SIZE];
	static struct iomfb_sess s;
	static struct fake_dcp f;
	static uint8_t blob[16384];
	fake_init(&f, shmem, &s);
	struct iomfb_transport tp = fake_transport(&f);
	iomfb_sess_init(&s, shmem, &tp, 2);
	uint32_t n = j716s_timing_blob(blob, sizeof(blob));
	CHECK(fake_deliver_prop(&f, "ColorElements", blob, 64) == 0);   /* other keys: accepted, not parsed */
	CHECK(s.ntimings == 0);
	CHECK(fake_deliver_prop(&f, "TimingElements", blob, n) == 0);
	CHECK(s.ntimings == 6 && !s.failed && f.unexpected == 0);

	GROUP("session: as a DCPW backend");
	struct dcpw_backend be;
	iomfb_sess_backend(&s, &be);
	static struct dcpw_dev d;
	CHECK(dcpw_configure(&d, &be, &ENV, 600000, 300000, 0) == 0);
	s.dev = &d;
	CHECK(dcpw_read32(&d, DCPW_REG_CAPS) == (DCPW_CAP_MODESET | DCPW_CAP_VRR | DCPW_CAP_BRIGHTNESS | DCPW_CAP_FLIP | DCPW_CAP_HW_VBLANK));
	CHECK(dcpw_read32(&d, DCPW_REG_MODE_COUNT) == 6 && (dcpw_read32(&d, DCPW_MODES_OFF) & DCPW_MODE_VRR));
	CHECK(dcpw_read32(&d, DCPW_REG_BL_MNITS) == 300000);

	uint32_t a[DCPW_REQ_ARGS], r0;
	fake_reset_calls(&f);
	a[0] = 1;
	CHECK(req(&d, DCPW_OP_SET_VRR, a, 1, 0) == DCPW_OK);
	CHECK(f.ncalls == 2 && !strcmp(f.calls[0].tag, "A500") && !strcmp(f.calls[1].tag, "A463"));
	CHECK(f.adaptive_q16 == 0x180000 && g32(f.calls[1].in + 4) == 2 && g32(f.calls[1].in) == 1);
	CHECK(dcpw_read32(&d, DCPW_REG_VRR) == 1 && (dcpw_read32(&d, DCPW_REG_IRQ_STATUS) & DCPW_IRQ_MODE));

	fake_reset_calls(&f);
	a[0] = 1; /* index 1 = timing 3, 60 Hz, fixed: VRR drops, only A463 */
	CHECK(req(&d, DCPW_OP_SET_MODE, a, 1, 0) == DCPW_OK);
	CHECK(f.ncalls == 1 && !strcmp(f.calls[0].tag, "A463") && f.cur_timing == 3);
	CHECK(dcpw_read32(&d, DCPW_REG_VRR) == 0 && dcpw_read32(&d, DCPW_REG_CUR_MHZ) == 60000);
	a[0] = 1; CHECK(req(&d, DCPW_OP_SET_VRR, a, 1, 0) == DCPW_E_UNSUP);
	fake_reset_calls(&f);
	a[0] = 0; CHECK(req(&d, DCPW_OP_SET_MODE, a, 1, 0) == DCPW_OK);
	CHECK(f.ncalls == 2 && f.adaptive_q16 == 0 && f.cur_timing == 2); /* back to 120 Hz, adaptive sync 0 */

	GROUP("session: PRESENT as a DCP swap, completion as vblank");
	fake_reset_calls(&f);
	dcpw_write32(&d, DCPW_REG_IRQ_STATUS, DCPW_IRQ_ALL, 0);
	present_args(a, 0x80000000ull, 13824, 3456, 2234);
	CHECK(req(&d, DCPW_OP_PRESENT, a, 10, &r0) == DCPW_OK && r0 == 1);
	CHECK(f.ncalls == 2 && !strcmp(f.calls[0].tag, "A456") && !strcmp(f.calls[1].tag, "A458"));
	CHECK(f.last_submitted == 1 && g64(f.calls[1].in + IOMFB_SW_SURF_IOVA) == 0xf00000000ull);
	CHECK(f.calls[1].in[IOMFB_SW_BL_UPDATE] == 0);
	CHECK(!(dcpw_read32(&d, DCPW_REG_IRQ_STATUS) & DCPW_IRQ_PRESENT));  /* not before swap_complete */
	CHECK(req(&d, DCPW_OP_PRESENT, a, 10, 0) == DCPW_E_BACKEND);         /* one swap in flight */
	dcpw_tick(&d, 1000000000ull);                                        /* HW vblank: timer ignored */
	CHECK(dcpw_read32(&d, DCPW_REG_VBL_LO) == 0);
	f.now = 123456789;
	CHECK(fake_complete_swap(&f, 1) == 1);
	CHECK(dcpw_read32(&d, DCPW_REG_VBL_LO) == 1 && dcpw_read32(&d, DCPW_REG_VBL_NS_LO) == 123456789u);
	CHECK(dcpw_read32(&d, DCPW_REG_IRQ_STATUS) & DCPW_IRQ_PRESENT);
	CHECK(fake_complete_swap(&f, 1) == 1 && dcpw_read32(&d, DCPW_REG_VBL_LO) == 1); /* stale id ignored */

	GROUP("session: brightness rides in the next swap");
	fake_reset_calls(&f);
	a[0] = 450000;
	CHECK(req(&d, DCPW_OP_SET_BRIGHTNESS, a, 1, 0) == DCPW_OK);
	CHECK(f.ncalls == 2 && f.calls[1].in[IOMFB_SW_BL_UPDATE] == 1 && g64(f.calls[1].in + IOMFB_SW_BL_NITS) == iomfb_u32_to_f64_bits(450));
	CHECK(dcpw_read32(&d, DCPW_REG_BL_MNITS) == 450000);
	a[0] = 600001; CHECK(req(&d, DCPW_OP_SET_BRIGHTNESS, a, 1, 0) == DCPW_E_RANGE);
	CHECK(fake_complete_swap(&f, f.last_submitted) == 1);
	CHECK(f.unexpected == 0 && !s.failed);

	GROUP("session: faults");
	f.fault = FF_SWAP_RET;
	present_args(a, 0x80000000ull, 13824, 3456, 2234);
	CHECK(req(&d, DCPW_OP_PRESENT, a, 10, 0) == DCPW_E_BACKEND && s.swap_inflight == 0);
	f.fault = FF_SWAP_START_RET;
	CHECK(req(&d, DCPW_OP_PRESENT, a, 10, 0) == DCPW_E_BACKEND);
	f.fault = FF_DIGITAL_RET;
	a[0] = 1; CHECK(req(&d, DCPW_OP_SET_MODE, a, 1, 0) == DCPW_E_BACKEND && dcpw_read32(&d, DCPW_REG_CUR_MODE) == 0);
	f.fault = FF_NONE;
	present_args(a, 0x7fffc000ull, 13824, 3456, 2234); /* not mappable by the fake */
	CHECK(req(&d, DCPW_OP_PRESENT, a, 10, 0) == DCPW_E_RANGE);
	CHECK(!s.failed);
	/* unknown callback: session fails, does not ack, DCPW reports FAILED */
	uint8_t junk[16] = {0};
	CHECK(fake_callback(&f, "D999", junk, 16, 0, NULL) == 0);
	CHECK(s.failed && !strcmp(s.fail_tag, "D999") && (dcpw_read32(&d, DCPW_REG_STATUS) & DCPW_ST_FAILED));
	CHECK(req(&d, DCPW_OP_PRESENT, a, 10, 0) == DCPW_E_BACKEND && req(&d, DCPW_OP_NOP, a, 0, 0) == DCPW_OK);

	GROUP("session: dropped ack times out");
	static struct iomfb_sess s2;
	static struct fake_dcp f2;
	fake_init(&f2, shmem, &s2);
	struct iomfb_transport tp2 = fake_transport(&f2);
	iomfb_sess_init(&s2, shmem, &tp2, 2);
	s2.timeout_polls = 50;
	f2.fault = FF_DROP_ACK;
	uint8_t dmo[8], o[4];
	iomfb_build_digital_out(dmo, 1, 2);
	CHECK(iomfb_sess_call(&s2, "A463", dmo, 8, 4, o) < 0 && s2.failed);
	CHECK(iomfb_sess_call(&s2, "A463", dmo, 8, 4, o) < 0); /* stays failed */

	GROUP("session: prop stream abuse");
	static struct iomfb_sess s3;
	static struct fake_dcp f3;
	fake_init(&f3, shmem, &s3);
	struct iomfb_transport tp3 = fake_transport(&f3);
	iomfb_sess_init(&s3, shmem, &tp3, 2);
	uint8_t out[4], st[4] = { 0, 0, 1, 0 }; /* 0x10000 > IOMFB_PROP_MAX */
	CHECK(fake_callback(&f3, "D126", st, 4, 4, out) == 1 && out[0] == 0);
	uint8_t ch[0x1008] = {0};
	uint32_t off = 0, len = 0x1000;
	memcpy(ch + 0x1000, &off, 4); memcpy(ch + 0x1004, &len, 4);
	CHECK(fake_callback(&f3, "D127", ch, 0x1008, 4, out) == 1 && out[0] == 0); /* chunk without start */
	st[2] = 0; st[1] = 0x10; /* 0x1000 */
	CHECK(fake_callback(&f3, "D126", st, 4, 4, out) == 1 && out[0] == 1);
	off = 0x800; memcpy(ch + 0x1000, &off, 4);
	CHECK(fake_callback(&f3, "D127", ch, 0x1008, 4, out) == 1 && out[0] == 0); /* overflow */
	uint8_t key[0x40]; memset(key, 'A', sizeof(key));                       /* unterminated key */
	CHECK(fake_callback(&f3, "D128", key, 0x40, 4, out) == 1 && out[0] == 0);
	/* duplicated chunk: every byte arrives, but not as one contiguous stream -> refused */
	st[1] = 0x20; /* 0x2000 */
	CHECK(fake_callback(&f3, "D126", st, 4, 4, out) == 1 && out[0] == 1);
	off = 0; memcpy(ch + 0x1000, &off, 4);
	CHECK(fake_callback(&f3, "D127", ch, 0x1008, 4, out) == 1 && out[0] == 1);
	CHECK(fake_callback(&f3, "D127", ch, 0x1008, 4, out) == 1 && out[0] == 1);
	off = 0x1000; memcpy(ch + 0x1000, &off, 4);
	CHECK(fake_callback(&f3, "D127", ch, 0x1008, 4, out) == 1 && out[0] == 1);
	memset(key, 0, sizeof(key)); memcpy(key, "ColorElements", 13);
	CHECK(fake_callback(&f3, "D128", key, 0x40, 4, out) == 1 && out[0] == 0);
	CHECK(fake_callback(&f3, "D126", st, 3, 4, out) == 0 && s3.failed);      /* wrong size: unhandled */
}

/* ---------------- request fuzz on the core ---------------- */
static void test_fuzz_core(void)
{
	GROUP("fuzz: 20000 random register sequences on both backends");
	static uint8_t shmem[IOMFB_SHMEM_SIZE];
	static struct iomfb_sess s;
	static struct fake_dcp f;
	static uint8_t blob[16384];
	fake_init(&f, shmem, &s);
	struct iomfb_transport tp = fake_transport(&f);
	iomfb_sess_init(&s, shmem, &tp, 2);
	CHECK(fake_deliver_prop(&f, "TimingElements", blob, j716s_timing_blob(blob, sizeof(blob))) == 0);
	struct dcpw_backend b1, b2;
	struct dcpw_static st = { 0, 0, {0}, 0 };
	dcpw_static_backend(&st, &b1);
	iomfb_sess_backend(&s, &b2);
	static struct dcpw_dev d1, d2;
	CHECK(dcpw_configure(&d1, &b1, &ENV, 600000, 0, 0) == 0);
	CHECK(dcpw_configure(&d2, &b2, &ENV, 600000, 300000, 0) == 0);
	s.dev = &d2;
	int inv_ok = 1;
	for (int i = 0; i < 20000; i++) {
		struct dcpw_dev *d = (i & 1) ? &d2 : &d1;
		uint32_t op = rnd() % 8;
		if (op < 5) {
			uint32_t off = DCPW_REQ_OFF + 4 * (rnd() % 14);
			uint32_t v = (rnd() & 3) ? rnd() % 8 : rnd();
			dcpw_write32(d, off, v, 0);
		} else if (op == 5) {
			uint32_t sq = dcpw_read32(d, DCPW_REQ_OFF);
			dcpw_write32(d, DCPW_REG_DOORBELL, (rnd() & 7) ? sq : rnd(), 0);
		} else if (op == 6) {
			dcpw_write32(d, (rnd() % 0x4000) & ~3u, rnd(), 0);
			dcpw_tick(d, (uint64_t)i * 1000000);
		} else {
			if (s.swap_inflight && (rnd() & 1)) fake_complete_swap(&f, s.swap_inflight);
			dcpw_read32(d, rnd() % 0x4000);
			/* a well-formed request with one field perturbed half of the time */
			uint32_t fa[DCPW_REQ_ARGS], fop = 1 + rnd() % 5;
			const struct dcpw_mode *cm = &d->modes[d->cur];
			memset(fa, 0, sizeof(fa));
			if (fop == DCPW_OP_PRESENT) present_args(fa, 0x80000000ull + 0x4000ull * (rnd() % 64), 13824, cm->width, cm->height);
			else fa[0] = fop == DCPW_OP_SET_BRIGHTNESS ? rnd() % 700000 : rnd() % 7;
			if (rnd() & 1) fa[rnd() % 10] ^= 1u << (rnd() % 32);
			req(d, fop, fa, 10, 0);
		}
		uint32_t cur = dcpw_read32(d, DCPW_REG_CUR_MODE), nm = dcpw_read32(d, DCPW_REG_MODE_COUNT);
		if (cur >= nm || nm != 6) inv_ok = 0;
		if (dcpw_read32(d, DCPW_REG_BL_MNITS) > dcpw_read32(d, DCPW_REG_BL_MAX_MNITS) && d == &d2) inv_ok = 0;
		if (d->vrr && !(d->modes[d->cur].flags & DCPW_MODE_VRR)) inv_ok = 0;
		if (d->status != DCPW_ST_READY && !(d->status & DCPW_ST_FAILED)) inv_ok = 0;
	}
	CHECK(inv_ok);
	CHECK(f.unexpected == 0);
	printf("   fuzz: d1 requests %u rejects %u; d2 requests %u rejects %u; swaps %u; session failed=%d\n",
	       d1.stat_requests, d1.stat_rejects, d2.stat_requests, d2.stat_rejects, s.swaps_done, s.failed);
}

int main(void)
{
	test_codec();
	test_parser();
	test_core_static();
	test_session();
	test_fuzz_core();
	printf("%s: %d checks, %d failures\n", fails ? "FAIL" : "PASS", checks, fails);
	return fails ? 1 : 0;
}
