/* test_edid.c -- host test for edid.h: checksums, base DTD, range limits, DisplayID Type I timings.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../service/dcpw_static.h"
#include "../edid.h"
static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %d: %s\n", __LINE__, #c); } } while (0)
static unsigned g24(const unsigned char *p) { return (p[0] | p[1] << 8 | p[2] << 16) + 1; }
static unsigned g16(const unsigned char *p) { return ((p[0] | p[1] << 8) & 0x7fff) + 1; }
int main(void)
{
	unsigned n, i, s;
	const struct dcpw_mode *m = dcpw_j716s_modes(&n);
	unsigned char e[EDID_LEN];
	CHECK(edid_build(e, m, n, 0, 345, 223) == 0);
	for (s = 0, i = 0; i < 128; i++) s += e[i];
	CHECK((s & 0xff) == 0);
	for (s = 0, i = 128; i < 256; i++) s += e[i];
	CHECK((s & 0xff) == 0);
	CHECK(!memcmp(e, "\x00\xff\xff\xff\xff\xff\xff\x00", 8) && e[18] == 1 && e[19] == 4 && e[126] == 1);
	/* base DTD = 60 Hz timing (index 1) */
	unsigned pc = e[54] | e[55] << 8, ha = e[56] | (e[58] >> 4) << 8, hb = e[57] | (e[58] & 15) << 8;
	unsigned va = e[59] | (e[61] >> 4) << 8, vb = e[60] | (e[61] & 15) << 8;
	CHECK(pc == 52106 && ha == 3456 && hb == 80 && va == 2234 && vb == 222);
	/* range limits */
	CHECK(e[75] == 0xfd && e[77] == 48 && e[78] == 120 && (e[76] & 0x08) && e[80] + 255 >= 295 && e[81] * 10 >= 1043);
	/* name */
	CHECK(e[93] == 0xfc && !memcmp(e + 95, "M5W Panel\n", 10));
	/* DisplayID */
	unsigned char *x = e + 128;
	CHECK(x[0] == 0x70 && x[1] == 0x13 && x[5] == 0x03 && x[7] == 5 * 20);
	for (s = 0, i = 1; i <= 5u + x[2]; i++) s += x[i];
	CHECK((s & 0xff) == 0);
	unsigned char *t = x + 8;
	CHECK(g24(t) == 104212 && (t[3] & 0x80) && g16(t + 4) == 3456 && g16(t + 6) == 80 && g16(t + 12) == 2234 && g16(t + 14) == 222);
	CHECK((t[9] & 0x80) && g16(t + 8) == EDID_HFP && g16(t + 18) == EDID_VSW);
	t += 20;
	CHECK(g24(t) == 52106 && !(t[3] & 0x80));
	t = x + 8 + 4 * 20;   /* five Type I timings fit in one extension: 120, 60, 59.94, 50, 48 */
	CHECK(g24(t) == (unsigned)(3536ull * 2456 * 48000 / 1000 / 10000));
	printf("%s: %d checks, %d failures\n", fails ? "FAIL" : "PASS", checks, fails);
	FILE *f = fopen(getenv("EDID_OUT") ? getenv("EDID_OUT") : "/dev/null", "wb"); if (f) { fwrite(e, 1, 256, f); fclose(f); }
	return fails != 0;
}
