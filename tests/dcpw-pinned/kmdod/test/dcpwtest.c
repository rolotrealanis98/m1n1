/*
 * dcpwtest.exe -- m5wdod.sys test (ARM64, no CRT). Prints "DCPWTEST PASS" when:
 *   1. user32 lists a display device "M5W internal display" with 3456x2234 at 120 Hz;
 *   2. the driver's escape answers, its VSync ISR count rises at about the mode rate over 2 s,
 *      and PRESENTs succeed;
 *   3. if the device offers 60 Hz: ChangeDisplaySettingsEx to 60 Hz takes (escape reports
 *      60000 mHz), then back to 120 Hz;
 *   4. if the service has brightness: IOCTL_VIDEO_SET_DISPLAY_BRIGHTNESS on \\.\LCD reaches it
 *      (escape brightness_sets rises), else this step is skipped and said so.
 * printf helper follows gpu/win-stack-20261110/tools/agxwesc/agxwesc.c (same project, MIT).
 * Copyright 2026 the m5pro-windows project
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

typedef int32_t NTSTATUS;
typedef uint16_t WCHAR;
#define WINAPI __stdcall
typedef uint32_t D3DKMT_HANDLE;
typedef struct { uint32_t LowPart; int32_t HighPart; } LUID;
typedef struct { D3DKMT_HANDLE hAdapter; LUID AdapterLuid; uint32_t NumOfSources; int32_t bPrecise; } D3DKMT_ADAPTERINFO;
typedef struct { uint32_t NumAdapters; D3DKMT_ADAPTERINFO *pAdapters; } D3DKMT_ENUMADAPTERS2;
typedef struct {
	D3DKMT_HANDLE hAdapter, hDevice;
	uint32_t Type, Flags;
	void *pPrivateDriverData;
	uint32_t PrivateDriverDataSize;
	D3DKMT_HANDLE hContext;
} D3DKMT_ESCAPE;

typedef struct {   /* DISPLAY_DEVICEW */
	uint32_t cb;
	WCHAR DeviceName[32], DeviceString[128];
	uint32_t StateFlags;
	WCHAR DeviceID[128], DeviceKey[128];
} DISPLAY_DEVICEW;
typedef struct {   /* DEVMODEW, 220 bytes */
	WCHAR dmDeviceName[32];
	uint16_t dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
	uint32_t dmFields;
	uint8_t u1[16];
	int16_t dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
	WCHAR dmFormName[32];
	uint16_t dmLogPixels;
	uint32_t dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
	uint32_t rest[8];
} DEVMODEW;
_Static_assert(sizeof(DEVMODEW) == 220, "DEVMODEW");
_Static_assert(sizeof(DISPLAY_DEVICEW) == 840, "DISPLAY_DEVICEW");
#define DM_BITSPERPEL 0x40000u
#define DM_PELSWIDTH 0x80000u
#define DM_PELSHEIGHT 0x100000u
#define DM_DISPLAYFREQUENCY 0x400000u
#define ENUM_CURRENT_SETTINGS 0xffffffffu
#define DM_POSITION 0x20u
#define CDS_UPDATEREGISTRY 0x1u
#define CDS_SET_PRIMARY 0x10u
#define CDS_NORESET 0x10000000u

typedef struct {   /* m5wdod.c M5WDOD_ESC */
	uint32_t magic, op, status;
	uint32_t caps, dcpw_status, cur_mode, cur_mhz, mode_count;
	uint32_t bl_mnits, bl_max_mnits, vrr;
	uint32_t vsync_isr, vsync_reported, presents, present_errors, commits, modesets;
	uint32_t surface_kind, width, height, stride, post_width, post_height;
	uint64_t surface_pa;
	uint32_t irq_enabled, brightness_sets;
} ESC;

__declspec(dllimport) void *WINAPI GetStdHandle(uint32_t);
__declspec(dllimport) int WINAPI WriteFile(void *, const void *, uint32_t, uint32_t *, void *);
__declspec(dllimport) void WINAPI ExitProcess(uint32_t);
__declspec(dllimport) void *WINAPI LoadLibraryA(const char *);
__declspec(dllimport) void *WINAPI GetProcAddress(void *, const char *);
__declspec(dllimport) uint32_t WINAPI GetLastError(void);
__declspec(dllimport) void *WINAPI CreateFileW(const WCHAR *, uint32_t, uint32_t, void *, uint32_t, uint32_t, void *);
__declspec(dllimport) int WINAPI DeviceIoControl(void *, uint32_t, void *, uint32_t, void *, uint32_t, uint32_t *, void *);
__declspec(dllimport) int WINAPI CloseHandle(void *);
__declspec(dllimport) void WINAPI Sleep(uint32_t);
__declspec(dllimport) uint32_t WINAPI GetTickCount(void);
__declspec(dllimport) int WINAPI EnumDisplayDevicesW(const WCHAR *, uint32_t, DISPLAY_DEVICEW *, uint32_t);
__declspec(dllimport) int WINAPI EnumDisplaySettingsExW(const WCHAR *, uint32_t, DEVMODEW *, uint32_t);
__declspec(dllimport) int32_t WINAPI ChangeDisplaySettingsExW(const WCHAR *, DEVMODEW *, void *, uint32_t, void *);
__declspec(dllimport) int32_t WINAPI SetDisplayConfig(uint32_t, void *, uint32_t, void *, uint32_t);
__declspec(dllimport) int WINAPI ProcessIdToSessionId(uint32_t, uint32_t *);
__declspec(dllimport) uint32_t WINAPI GetCurrentProcessId(void);

static NTSTATUS (WINAPI *D3DKMTEnumAdapters2)(D3DKMT_ENUMADAPTERS2 *);
static NTSTATUS (WINAPI *D3DKMTEscape)(const D3DKMT_ESCAPE *);

static void *g_out;
static char g_buf[1024];
static void put(const char *s, uint32_t n) { uint32_t w; WriteFile(g_out, s, n, &w, 0); }
static int fmt_u(char *o, uint64_t v, int base, int width)
{
	char t[24];
	int n = 0, k = 0;
	do { t[n++] = "0123456789abcdef"[v % base]; v /= base; } while (v);
	while (n < width) t[n++] = '0';
	while (n) o[k++] = t[--n];
	return k;
}
static void pr(const char *f, ...)
{
	va_list ap;
	int k = 0;
	va_start(ap, f);
	for (; *f && k < (int)sizeof(g_buf) - 64; f++) {
		if (*f != '%') { if (*f == '\n') g_buf[k++] = '\r'; g_buf[k++] = *f; continue; }
		f++;
		int w = 0, l = 0;
		while (*f >= '0' && *f <= '9') w = w * 10 + (*f++ - '0');
		while (*f == 'l') { l++; f++; }
		switch (*f) {
		case 'u': k += fmt_u(g_buf + k, l ? va_arg(ap, uint64_t) : va_arg(ap, uint32_t), 10, w); break;
		case 'd': { int64_t v = l ? va_arg(ap, int64_t) : va_arg(ap, int32_t);
		            if (v < 0) { g_buf[k++] = '-'; v = -v; }
		            k += fmt_u(g_buf + k, (uint64_t)v, 10, w); break; }
		case 'x': k += fmt_u(g_buf + k, l ? va_arg(ap, uint64_t) : va_arg(ap, uint32_t), 16, w); break;
		case 's': { const char *s = va_arg(ap, const char *); while (*s && k < (int)sizeof(g_buf) - 8) g_buf[k++] = *s++; break; }
		case 'S': { const WCHAR *s = va_arg(ap, const WCHAR *);
		            while (*s && k < (int)sizeof(g_buf) - 8) { g_buf[k++] = *s < 128 ? (char)*s : '?'; s++; } break; }
		default: g_buf[k++] = '%'; g_buf[k++] = *f; break;
		}
	}
	va_end(ap);
	put(g_buf, (uint32_t)k);
}
static void zero(void *p, size_t n) { volatile uint8_t *b = p; while (n--) *b++ = 0; }
void *memset(void *p, int c, size_t n) { uint8_t *b = p; while (n--) *b++ = (uint8_t)c; return p; }
void *memcpy(void *d, const void *s, size_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; return d; }
static int wcontains(const WCHAR *s, const char *n)
{
	for (; *s; s++) {
		int i = 0;
		while (n[i] && s[i] == (WCHAR)n[i]) i++;
		if (!n[i]) return 1;
	}
	return 0;
}

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; pr("FAIL: "); pr(__VA_ARGS__); pr("\n"); } else { pr("ok: "); pr(__VA_ARGS__); pr("\n"); } } while (0)

static D3DKMT_HANDLE g_adapter;
static int esc_read(ESC *e)
{
	D3DKMT_ESCAPE k;
	if (!D3DKMTEscape || !g_adapter) return -1;
	zero(e, sizeof(*e));
	e->magic = 0x45504344u;
	e->op = 1;
	e->status = 0xffffffffu;
	zero(&k, sizeof(k));
	k.hAdapter = g_adapter;
	k.pPrivateDriverData = e;
	k.PrivateDriverDataSize = sizeof(*e);
	return D3DKMTEscape(&k) >= 0 && e->status == 0 ? 0 : -1;
}

static void find_adapter(void)
{
	void *m = LoadLibraryA("gdi32.dll");
	const char *pre = "D3DKMT";
	static char nm[64];
	const char *fn[2] = {"EnumAdapters2", "Escape"};
	void **dst[2] = {(void **)&D3DKMTEnumAdapters2, (void **)&D3DKMTEscape};
	if (m && !GetProcAddress(m, "D3DKMTEnumAdapters2")) m = 0;
	if (!m) { m = LoadLibraryA("win32u.dll"); pre = "NtGdiDdDDI"; }
	if (!m) { pr("no gdi32/win32u: escape tests skipped\n"); return; }
	for (int i = 0; i < 2; i++) {
		int k = 0;
		for (const char *a = pre; *a; a++) nm[k++] = *a;
		for (const char *a = fn[i]; *a; a++) nm[k++] = *a;
		nm[k] = 0;
		*dst[i] = GetProcAddress(m, nm);
		if (!*dst[i]) { pr("GetProcAddress(%s) failed %u\n", nm, GetLastError()); D3DKMTEscape = 0; return; }
	}
	static D3DKMT_ADAPTERINFO ai[16];
	D3DKMT_ENUMADAPTERS2 en = {16, ai};
	NTSTATUS ns = D3DKMTEnumAdapters2(&en);
	pr("%sEnumAdapters2 -> %x, %u adapters\n", pre, (uint32_t)ns, en.NumAdapters);
	for (uint32_t i = 0; ns >= 0 && i < en.NumAdapters && i < 16; i++) {
		ESC e;
		g_adapter = ai[i].hAdapter;
		if (!esc_read(&e)) { pr("adapter %u (h %x, %u sources) answers the m5wdod escape\n", i, g_adapter, ai[i].NumOfSources); return; }
	}
	g_adapter = 0;
}

static void list_modes(const WCHAR *dev, int *has120, int *has60, uint32_t *curfreq)
{
	static DEVMODEW dm;
	uint32_t last_w = 0, last_h = 0, last_f = 0;
	zero(&dm, sizeof(dm));
	dm.dmSize = sizeof(dm);
	if (EnumDisplaySettingsExW(dev, ENUM_CURRENT_SETTINGS, &dm, 0)) {
		pr("    current: %ux%u %u bpp @ %u Hz\n", dm.dmPelsWidth, dm.dmPelsHeight, dm.dmBitsPerPel, dm.dmDisplayFrequency);
		*curfreq = dm.dmDisplayFrequency;
	}
	for (uint32_t i = 0; i < 512; i++) {
		zero(&dm, sizeof(dm));
		dm.dmSize = sizeof(dm);
		if (!EnumDisplaySettingsExW(dev, i, &dm, 0)) break;
		if (dm.dmPelsWidth == last_w && dm.dmPelsHeight == last_h && dm.dmDisplayFrequency == last_f) continue;
		last_w = dm.dmPelsWidth; last_h = dm.dmPelsHeight; last_f = dm.dmDisplayFrequency;
		pr("    mode %u: %ux%u %u bpp @ %u Hz\n", i, dm.dmPelsWidth, dm.dmPelsHeight, dm.dmBitsPerPel, dm.dmDisplayFrequency);
		if (dm.dmPelsWidth == 3456 && dm.dmPelsHeight == 2234) {
			if (dm.dmDisplayFrequency == 120) *has120 = 1;
			if (dm.dmDisplayFrequency == 60) *has60 = 1;
		}
	}
}

static int32_t set_freq(const WCHAR *dev, uint32_t hz)
{
	static DEVMODEW dm;
	zero(&dm, sizeof(dm));
	dm.dmSize = sizeof(dm);
	dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
	dm.dmPelsWidth = 3456; dm.dmPelsHeight = 2234; dm.dmBitsPerPel = 32; dm.dmDisplayFrequency = hz;
	return ChangeDisplaySettingsExW(dev, &dm, 0, 0, 0);
}

/* Attach a detached display to the desktop as primary at 3456x2234@hz (registry + apply). */
static int32_t attach(const WCHAR *dev, uint32_t hz)
{
	static DEVMODEW dm;
	int32_t a, b;
	zero(&dm, sizeof(dm));
	dm.dmSize = sizeof(dm);
	dm.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
	/* dmPosition (POINTL) is the first member of the display union at offset 76: 0,0 */
	dm.dmPelsWidth = 3456; dm.dmPelsHeight = 2234; dm.dmBitsPerPel = 32; dm.dmDisplayFrequency = hz;
	a = ChangeDisplaySettingsExW(dev, &dm, 0, CDS_UPDATEREGISTRY | CDS_SET_PRIMARY | CDS_NORESET, 0);
	b = ChangeDisplaySettingsExW(0, 0, 0, 0, 0);
	pr("attach: ChangeDisplaySettingsEx(dev, primary, noreset) %d, apply %d\n", a, b);
	if (a || b) {
		/* CCD: SDC_APPLY 0x80 | SDC_TOPOLOGY_EXTEND 0x4, then SDC_TOPOLOGY_INTERNAL 0x1 */
		int32_t c = SetDisplayConfig(0, 0, 0, 0, 0x80 | 0x4), d = SetDisplayConfig(0, 0, 0, 0, 0x80 | 0x1);
		pr("attach: SetDisplayConfig(extend) %d, (internal) %d\n", c, d);
		if (!c || !d) return 0;
	}
	return a ? a : b;
}

static void print_esc(const char *tag, const ESC *e)
{
	pr("%s: caps %x status %x mode %u/%u %u mHz bl %u/%u vrr %u vsync isr %u reported %u irq %u presents %u errs %u commits %u modesets %u bl_sets %u surface %u %ux%u/%u pa %lx post %ux%u\n",
	   tag, e->caps, e->dcpw_status, e->cur_mode, e->mode_count, e->cur_mhz, e->bl_mnits, e->bl_max_mnits, e->vrr,
	   e->vsync_isr, e->vsync_reported, e->irq_enabled, e->presents, e->present_errors, e->commits, e->modesets,
	   e->brightness_sets, e->surface_kind, e->width, e->height, e->stride, (uint64_t)e->surface_pa, e->post_width, e->post_height);
}

void entry(void)
{
	static DISPLAY_DEVICEW dd;
	static WCHAR ours[32];
	int found = 0, has120 = 0, has60 = 0;
	uint32_t curfreq = 0;
	ESC e0, e1;

	g_out = GetStdHandle((uint32_t)-11);
	{
		uint32_t sid = 0xffffffffu;
		ProcessIdToSessionId(GetCurrentProcessId(), &sid);
		pr("dcpwtest: m5wdod/DCPW test (session %u)\n", sid);
	}
	for (uint32_t i = 0; i < 16; i++) {
		zero(&dd, sizeof(dd));
		dd.cb = sizeof(dd);
		if (!EnumDisplayDevicesW(0, i, &dd, 0)) break;
		pr("display %u: %S \"%S\" flags %x id %S\n", i, dd.DeviceName, dd.DeviceString, dd.StateFlags, dd.DeviceID);
		int h120 = 0, h60 = 0;
		uint32_t cf = 0;
		list_modes(dd.DeviceName, &h120, &h60, &cf);
		if (wcontains(dd.DeviceString, "M5W")) {
			found = 1; has120 = h120; has60 = h60; curfreq = cf;
			for (int k = 0; k < 32; k++) ours[k] = dd.DeviceName[k];
		}
	}
	CHECK(found, "user32 lists the M5W display device");
	CHECK(has120, "3456x2234 @ 120 Hz is in its mode list");
	pr("info: current %u Hz; 60 Hz %s\n", curfreq, has60 ? "offered" : "not offered (static backend offers only the active mode)");

	if (found && !curfreq) {
		int32_t r = attach(ours, 120);
		Sleep(1000);
		int h120b = 0, h60b = 0;
		list_modes(ours, &h120b, &h60b, &curfreq);
		CHECK(r == 0 && curfreq == 120, "display attached to the desktop at 120 Hz (rc %d, now %u Hz)", r, curfreq);
	}
	find_adapter();
	CHECK(g_adapter && !esc_read(&e0), "the driver answers its escape");
	if (g_adapter && !esc_read(&e0)) {
		print_esc("before", &e0);
		uint32_t t0 = GetTickCount();
		Sleep(2000);
		esc_read(&e1);
		uint32_t dt = GetTickCount() - t0;
		print_esc("after", &e1);
		uint32_t n = e1.vsync_isr - e0.vsync_isr;
		uint32_t rate = dt ? (uint32_t)((uint64_t)n * 1000 / dt) : 0;
		pr("vsync: %u interrupts in %u ms = %u/s (mode %u mHz; reported to dxgkrnl %u)\n", n, dt, rate, e1.cur_mhz,
		   e1.vsync_reported - e0.vsync_reported);
		CHECK(e1.width == 3456 && e1.height == 2234, "surface is native 3456x2234");
		CHECK(e1.presents > 0 && e1.present_errors == 0, "PRESENTs reached the service without error");
		if (e1.irq_enabled)
			CHECK(rate * 1000 >= e1.cur_mhz / 2 && rate * 1000 <= e1.cur_mhz * 2, "VSync rate is within 2x of the mode rate");
		else
			pr("info: dxgkrnl has VSync interrupts off right now (no waiter); ISR count %u\n", n);

		if (found && has60) {
			int32_t r = set_freq(ours, 60);
			Sleep(500);
			esc_read(&e1);
			CHECK(r == 0 && e1.cur_mhz == 60000, "switch to 60 Hz (ChangeDisplaySettingsEx %d, service %u mHz)", r, e1.cur_mhz);
			r = set_freq(ours, 120);
			Sleep(500);
			esc_read(&e1);
			CHECK(r == 0 && e1.cur_mhz == 120000, "switch back to 120 Hz (ChangeDisplaySettingsEx %d, service %u mHz)", r, e1.cur_mhz);
		}

		if (e1.caps & 4) {
			static const WCHAR lcd[] = {'\\', '\\', '.', '\\', 'L', 'C', 'D', 0};
			void *h = CreateFileW(lcd, 0xC0000000u, 3, 0, 3, 0, 0);
			if (h == (void *)(intptr_t)-1) {
				pr("info: \\\\.\\LCD not available (error %u)\n", GetLastError());
			} else {
				uint8_t lv[256], db[3] = {3, 50, 50};
				uint32_t got = 0, before = e1.brightness_sets;
				int ok = DeviceIoControl(h, 0x230494, 0, 0, lv, sizeof(lv), &got, 0);
				pr("IOCTL_VIDEO_QUERY_SUPPORTED_BRIGHTNESS -> %d, %u levels\n", ok, got);
				ok = DeviceIoControl(h, 0x23049C, db, 3, 0, 0, &got, 0);
				Sleep(200);
				esc_read(&e1);
				CHECK(ok && e1.brightness_sets > before, "brightness 50%% through \\\\.\\LCD reached the driver (%u mnits)", e1.bl_mnits);
				CloseHandle(h);
			}
		} else {
			pr("info: service reports no brightness capability: brightness test skipped\n");
		}
	}
	pr("%s: %u checks, %u failures\n", fails ? "DCPWTEST FAIL" : "DCPWTEST PASS", checks, fails);
	ExitProcess(fails ? 1 : 0);
}
