/*
 * edid.h -- a synthetic EDID 1.4 + DisplayID 1.3 extension for the J716s panel, built from the
 * DCPW mode table, so Windows' monitor stack sees the panel's real timings and range limits.
 * The 120 Hz pixel clock (about 1.04 GHz) does not fit a base-block detailed timing (max 655.35 MHz),
 * so the base block carries the 60 Hz timing and a range-limits descriptor (47-120 Hz, up to
 * 1.05 GHz), and the DisplayID extension carries Type I detailed timings for every mode (pixel clock
 * in 10 kHz units, 24 bits). Vendor ID "MWD" is a made-up test ID, not any real manufacturer.
 * Header-only, freestanding; host-tested by kmdod/test/test_edid.c.
 * Copyright 2026 the m5pro-windows project
 * SPDX-License-Identifier: MIT
 */
#ifndef M5W_EDID_H
#define M5W_EDID_H

#include "dcpw_abi.h"

#define EDID_LEN 256u

static void edid_csum(unsigned char *b, unsigned n)   /* b[n-1] makes b[0..n-1] sum to 0 mod 256 */
{
	unsigned s = 0, i;
	for (i = 0; i + 1 < n; i++)
		s += b[i];
	b[n - 1] = (unsigned char)(256u - (s & 0xff)) & 0xff;
}

/* porches are not in the DCPW table; fixed small values inside each blanking interval (INFERENCE) */
#define EDID_HFP 8u
#define EDID_HSW 32u
#define EDID_VFP 3u
#define EDID_VSW 6u

static int edid_dtd(unsigned char *d, const struct dcpw_mode *m, unsigned hmm, unsigned vmm)
{
	unsigned hb = m->htotal - m->width, vb = m->vtotal - m->height;
	unsigned long long pc = (unsigned long long)m->htotal * m->vtotal * m->refresh_mhz / 1000ull / 10000ull;
	if (pc == 0 || pc > 65535 || hb < EDID_HFP + EDID_HSW || vb < EDID_VFP + EDID_VSW || m->width > 4095 ||
	    m->height > 4095 || hb > 4095 || vb > 4095)
		return -1;
	d[0] = pc & 0xff; d[1] = (unsigned char)(pc >> 8);
	d[2] = m->width & 0xff; d[3] = hb & 0xff; d[4] = (unsigned char)(((m->width >> 8) << 4) | (hb >> 8));
	d[5] = m->height & 0xff; d[6] = vb & 0xff; d[7] = (unsigned char)(((m->height >> 8) << 4) | (vb >> 8));
	d[8] = EDID_HFP; d[9] = EDID_HSW; d[10] = (unsigned char)((EDID_VFP << 4) | EDID_VSW); d[11] = 0;
	d[12] = hmm & 0xff; d[13] = vmm & 0xff; d[14] = (unsigned char)(((hmm >> 8) << 4) | (vmm >> 8));
	d[15] = 0; d[16] = 0;
	d[17] = 0x1e;   /* digital separate sync, +h +v */
	return 0;
}

static void put24m1(unsigned char *p, unsigned v) { v -= 1; p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; }
static void put16m1(unsigned char *p, unsigned v) { v -= 1; p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }

/* Returns 0 and fills out[256], or -1 if no mode fits. preferred = index of the preferred mode. */
static int edid_build(unsigned char out[EDID_LEN], const struct dcpw_mode *modes, unsigned n, unsigned preferred,
                      unsigned hmm, unsigned vmm)
{
	unsigned char *b = out, *x = out + 128;
	unsigned i, k, best60 = n, vmin = 1000, vmax = 0;
	unsigned long long pcmax = 0;
	for (i = 0; i < EDID_LEN; i++)
		out[i] = 0;
	if (!n || preferred >= n)
		return -1;
	/* base block */
	b[1] = b[2] = b[3] = b[4] = b[5] = b[6] = 0xff;
	b[8] = 0x36; b[9] = 0xe4;                  /* "MWD": (13<<10)|(23<<5)|4 = 0x36e4 */
	b[10] = 0x01; b[11] = 0x00;                /* product 1 */
	b[16] = 45; b[17] = 36;                    /* week 45, 2026 */
	b[18] = 1; b[19] = 4;                      /* EDID 1.4 */
	b[20] = 0xa5;                              /* digital, 8 bpc, DisplayPort */
	b[21] = (unsigned char)((hmm + 5) / 10); b[22] = (unsigned char)((vmm + 5) / 10);
	b[23] = 120;                               /* gamma 2.2 */
	b[24] = 0x06;                              /* sRGB default, preferred timing is native */
	{   /* sRGB chromaticity (10-bit) */
		static const unsigned char c[10] = { 0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26, 0x0f, 0x50, 0x54 };
		for (i = 0; i < 10; i++) b[25 + i] = c[i];
	}
	for (i = 38; i < 54; i++) b[i] = 0x01;     /* no standard timings */
	for (i = 0; i < n; i++) {
		unsigned hz = (modes[i].refresh_mhz + 500) / 1000;
		unsigned long long pc = (unsigned long long)modes[i].htotal * modes[i].vtotal * modes[i].refresh_mhz / 1000ull;
		if (hz < vmin) vmin = hz;
		if (hz > vmax) vmax = hz;
		if (pc > pcmax) pcmax = pc;
		if (modes[i].refresh_mhz <= 60000 && (best60 == n || modes[i].refresh_mhz > modes[best60].refresh_mhz))
			best60 = i;
	}
	if (best60 == n || edid_dtd(b + 54, &modes[best60], hmm, vmm))
		return -1;
	{   /* range limits: V vmin..vmax Hz, H 1..(max+255) kHz, pixel clock up to pcmax rounded up to 10 MHz */
		unsigned char *r = b + 72;
		unsigned long long hmax = 0;
		for (i = 0; i < n; i++) {
			unsigned long long h = (unsigned long long)modes[i].vtotal * modes[i].refresh_mhz / 1000000ull + 1;
			if (h > hmax) hmax = h;
		}
		r[3] = 0xfd;
		r[4] = hmax > 255 ? 0x08 : 0x00;       /* max horizontal rate +255 kHz offset */
		r[5] = (unsigned char)vmin; r[6] = (unsigned char)vmax;
		r[7] = 1; r[8] = (unsigned char)(hmax > 255 ? hmax - 255 : hmax);
		r[9] = (unsigned char)((pcmax + 9999999ull) / 10000000ull);
		r[10] = 0x01;                          /* range limits only */
		r[11] = 0x0a; for (i = 12; i < 18; i++) r[i] = 0x20;
	}
	{   /* product name */
		static const char nm[] = "M5W Panel";
		unsigned char *d = b + 90;
		d[3] = 0xfc;
		for (i = 0; i < 13; i++) d[5 + i] = i < sizeof(nm) - 1 ? (unsigned char)nm[i] : (i == sizeof(nm) - 1 ? 0x0a : 0x20);
	}
	b[108 + 3] = 0x10;                         /* dummy descriptor */
	b[126] = 1;                                /* one extension */
	edid_csum(b, 128);

	/* DisplayID 1.3 extension: Type I detailed timings */
	x[0] = 0x70;
	x[1] = 0x13;                               /* DisplayID 1.3 */
	x[3] = 0x00;                               /* product type: extension section */
	x[4] = 0x00;
	k = 5;
	x[k++] = 0x03; x[k++] = 0x00;              /* Type I detailed timing block, revision 0 */
	{
		unsigned cnt = 0, lenpos = k++;
		for (i = 0; i < n && k + 20 <= 126 - 1; i++) {
			const struct dcpw_mode *m = &modes[i];
			unsigned long long pc = (unsigned long long)m->htotal * m->vtotal * m->refresh_mhz / 1000ull / 10000ull;
			unsigned char *t = x + k;
			if (m->htotal - m->width < EDID_HFP + EDID_HSW || m->vtotal - m->height < EDID_VFP + EDID_VSW)
				continue;
			put24m1(t, (unsigned)pc);
			t[3] = (unsigned char)(i == preferred ? 0x80 : 0x00);   /* preferred, progressive, aspect n/a */
			put16m1(t + 4, m->width);
			put16m1(t + 6, m->htotal - m->width);
			put16m1(t + 8, EDID_HFP); t[9] |= 0x80;                 /* +hsync */
			put16m1(t + 10, EDID_HSW);
			put16m1(t + 12, m->height);
			put16m1(t + 14, m->vtotal - m->height);
			put16m1(t + 16, EDID_VFP); t[17] |= 0x80;               /* +vsync */
			put16m1(t + 18, EDID_VSW);
			k += 20;
			cnt++;
		}
		x[lenpos] = (unsigned char)(cnt * 20);
	}
	x[2] = (unsigned char)(k - 5);             /* section payload bytes */
	{   /* section checksum: x[1..k] sums to 0 */
		unsigned s = 0;
		for (i = 1; i < k; i++) s += x[i];
		x[k] = (unsigned char)(256u - (s & 0xff)) & 0xff;
	}
	edid_csum(x, 128);
	return 0;
}

#endif
