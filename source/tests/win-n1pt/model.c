/* SPDX-License-Identifier: MIT */
/*
 * tests/win-n1pt: register model of apcie0 (RC, shared PHY, port 0-3, lane PHY, intr2axi, the root port's and
 * the N1's config space), the PERST# pad (gpio0 pin 80), dart-apcie0, the storage registers m1n1's snapshot
 * reads, the SMC gP13 key and a fake ADT, for host runs of the unchanged pcie_t6050.c, n1pt_plat.c and
 * n1pt_dart.c. Behaviour rules are the minimum that m1n1's training/cycle/park and the n1pt programming poll
 * for (LINUX-MAP.md 9.4); everything else is a plain register file. A simulated clock advances on every
 * access and delay, so command durations come out in model time.
 */
#include <stdarg.h>
#include <sys/mman.h>
#include "model.h"

struct model M;

/* ---- address map ---- */
#define AP(idx) (0x416000000UL + (u64)(idx) * 0x10000)
const u64 M_ECAM = 0x1cb0000000UL, M_RC = 0x414000000UL, M_PHY = 0x415000000UL, M_PHYCTL = 0x415800000UL;
const u64 M_AUS5_TOP = 0x415900000UL, M_AUS5_AON = 0x415a00000UL, M_AXI2AF = 0x415b00000UL;
const u64 M_GPIO = 0x29c000000UL, M_DART = 0x410000000UL;
const u64 M_ANS = 0x2ab000000UL, M_NVME = 0x2ab800000UL, M_SART = 0x2ab900000UL, M_PS = 0x28e080000UL;
u64 m_port(int p) { return 0x410028000UL + (u64)p * 0x100000; }
u64 m_lane(int p) { return 0x417010000UL + (u64)p * 0x1000; }
u64 m_i2axi(int p) { return 0x410024000UL + (u64)p * 0x100000; }
#define PERST (M_GPIO + 4 * 80)

/* ---- sparse register file ---- */
#define NSLOT (1 << 18)
static u64 keys[NSLOT];
static u32 vals[NSLOT];
static bool used[NSLOT];
static u32 *slot(u64 a, bool create)
{
    u64 h = (a * 0x9E3779B97F4A7C15ULL) >> 46;
    for (u64 i = 0; i < NSLOT; i++) {
        u64 j = (h + i) & (NSLOT - 1);
        if (used[j] && keys[j] == a)
            return &vals[j];
        if (!used[j]) {
            if (!create)
                return NULL;
            used[j] = true;
            keys[j] = a;
            vals[j] = 0;
            return &vals[j];
        }
    }
    fprintf(stderr, "model: register file full\n");
    exit(2);
}
u32 m_get(u64 a) { u32 *p = slot(a, false); return p ? *p : 0; }
void m_set(u64 a, u32 v) { *slot(a, true) = v; }

/* ---- the N1's config space (three functions) ---- */
static u64 fn_cfg(int fn) { return M_ECAM + (1UL << 20) + ((u64)fn << 12); }
static void fn_reset(int fn)
{
    static const u32 mmc[3] = {5, 5, 1}; /* lspci: fn0 4/32 */
    u64 c = fn_cfg(fn);
    for (u32 o = 0; o < 0x1000; o += 4)
        m_set(c + o, 0);
    m_set(c + 0x08, 0xff000001);
    m_set(c + 0x10, 0xc);
    m_set(c + 0x20, 0xc);
    m_set(c + 0x34, 0x40);
    m_set(c + 0x40, 0x48035001);           /* PM, next 0x50 */
    m_set(c + 0x50, ((BIT(7) | (mmc[fn] << 1)) << 16) | 0x7005); /* MSI 64-bit, next 0x70 */
    m_set(c + 0x70, 0x00020010);
    M.mmc[fn] = mmc[fn];
}

static bool fn_present(int fn) { return M.link && (fn == 0 || M.fw_running); }

static u32 fn_read(int fn, u32 off)
{
    u64 c = fn_cfg(fn);
    if (!fn_present(fn))
        return 0xffffffff;
    switch (off) {
    case 0x00: return fn == 0 ? (M.fw_running ? 0x1901106b : 0x1900106b) : (fn == 1 ? 0x1902106b : 0x1903106b);
    case 0x10: return (m_get(c + 0x10) & ~0xffffu) | 0xc;
    case 0x20: return (m_get(c + 0x20) & ~0x1ffffffu) | 0xc;
    case 0x50: { u32 v = m_get(c + 0x50); u32 ctrl = ((v >> 16) & 0x71) | BIT(7) | (M.mmc[fn] << 1);
                 return (ctrl << 16) | (v & 0xffff); }
    default: return m_get(c + off);
    }
}

static void fn_write(int fn, u32 off, u32 v)
{
    if (!fn_present(fn))
        return;
    u64 c = fn_cfg(fn);
    if (off == 0x00 || off == 0x08 || off == 0x34 || off == 0x40 || off == 0x70)
        return; /* read-only */
    if (off == 0x50)
        v = (v & 0x00710000) | 0x7005; /* MSI: only MME/enable are writable */
    if (off == 0x04)
        v &= 0xffff;
    m_set(c + off, v);
    M.cfg_writes[fn][off / 4]++;
}

static u64 fn0_bar0(void)
{
    u64 c = fn_cfg(0);
    return ((u64)m_get(c + 0x14) << 32) | (m_get(c + 0x10) & ~0xffffu);
}

/* ---- MMIO entry points ---- */
static void tick(void) { M.now++; }

static bool in(u64 a, u64 base, u64 size) { return a >= base && a < base + size; }

u32 read32(u64 a)
{
    tick();
    M.reads++;
    u64 p0 = m_port(0);
    if (in(a, M_ECAM, 1UL << 20)) {
        u32 off = (u32)(a - M_ECAM);
        if (off >= 0x8000)
            return 0xffffffff; /* only the root port of port 0 is modelled */
        if (off == 0x24)
            return (m_get(a) & 0xfff0fff0) | 0x00010001;
        if (off == 0x728)
            return 0x11;
        return m_get(a);
    }
    if (in(a, M_ECAM + (1UL << 20), 0x100000)) {
        u32 off = (u32)(a - M_ECAM - (1UL << 20));
        if (off >= 0x3000)
            return 0xffffffff;
        return fn_read((int)(off >> 12), off & 0xffc);
    }
    if (a == p0 + 0x208)
        return (M.link ? 1 : 0) | (M.l2 ? BIT(6) : 0);
    for (int p = 1; p < 4; p++)
        if (a == m_port(p) + 0x208)
            return 0;
    if (a == p0 + 0x0a8)
        return (m_get(p0 + 0x800) & 1) | ((m_get(p0 + 0x82c) & BIT(24)) ? BIT(16) : 0);
    if (a == p0 + 0x804)
        return ((m_get(p0 + 0x800) & 1) && (m_get(p0 + 0x82c) & 1)) ? 1 : 0;
    if (a == p0 + 0x820)
        return 0x4;
    if (a == M_RC + 0x58)
        return m_get(M_RC + 0x50) & 1;
    if (a == M_PHYCTL + 0x08) {
        u32 c = m_get(M_PHYCTL + 0x04);
        return ((c & BIT(4)) ? BIT(4) : 0) | (((c & BIT(4)) && !(c & BIT(5))) ? BIT(0) : 0);
    }
    if (a == M_PHY + 0x4000)
        return m_get(a) | BIT(31);
    if (a == M_AUS5_TOP + 0x94)
        return 0;
    if (in(a, M_DART, 0x20000)) {
        u32 off = (u32)(a - M_DART);
        if (off == 0x08) return 36u << 16;
        if (off == 0x0c) return M.dart_nsid;
        if (off == 0x80) return 0;
        if (off == 0x100) return M.dart_error;
        if (off == 0x200) return M.dart_protect;
        if (off == 0xc00) return M.dart_enabled;
        return m_get(a);
    }
    u64 bar = fn0_bar0();
    if (bar && a == bar + 0x8000) {
        u32 cmd = m_get(fn_cfg(0) + 0x04);
        return (M.link && (cmd & 2)) ? M.rom_stage : 0xffffffff;
    }
    if (bar && in(a, bar, 0x10000))
        return M.link ? 0 : 0xffffffff;
    return m_get(a);
}

static void perst_assert(void)
{
    M.perst_asserts++;
    M.link = false;
    M.l2 = false;
    for (int fn = 0; fn < 3; fn++)
        fn_reset(fn);
    if (M.trip_storage_on_perst)
        m_set(M_PS + 8 * 3, m_get(M_PS + 8 * 3) ^ 0x5); /* ANS PS target moves: the storage guard must trip */
    M.log[M.nlog++ % 64] = 'P';
}

void write32(u64 a, u32 v)
{
    tick();
    M.writes++;
    u64 p0 = m_port(0);
    if (in(a, M_ECAM + (1UL << 20), 0x3000)) {
        u32 off = (u32)(a - M_ECAM - (1UL << 20));
        if ((off & 0xffc) == 0x04 && !(v & 4) && (fn_read((int)(off >> 12), 4) & 4))
            M.log[M.nlog++ % 64] = 'M'; /* MASTER off */
        fn_write((int)(off >> 12), off & 0xffc, v);
        return;
    }
    if (in(a, M_ECAM + (1UL << 20), 0x100000))
        return;
    if (a == PERST) {
        u32 old = m_get(a);
        m_set(a, v);
        if ((old & 1) && !(v & 1))
            perst_assert();
        return;
    }
    if (a == p0 + 0x080) {
        m_set(a, v);
        if ((v & 1) && (m_get(PERST) & 1) && (m_get(p0 + 0x800) & 1) && M.released && !M.never_link) {
            M.link = true;
            M.l2 = false;
            M.link_ups++;
        } else if (!(v & 1)) {
            M.link = false;
        }
        return;
    }
    if (a == p0 + 0x088) {
        if (v == 0x11 && M.link) {
            M.l2 = true;
            m_set(a, 0x10);
        } else {
            m_set(a, v);
        }
        return;
    }
    if (a == p0 + 0x800 && !(v & 1))
        M.link = false;
    for (int p = 0; p < 4; p++)
        if (a == m_lane(p))
            v = (v & ~0xcu) | ((v & 3) << 2);
    if (in(a, M_DART, 0x20000)) {
        u32 off = (u32)(a - M_DART);
        if (off == 0xc00) { M.dart_enabled |= v; return; }
        if (off == 0xc20) { M.dart_enabled &= ~v; M.log[M.nlog++ % 64] = 'D'; return; }
        if (off == 0x80) { M.dart_flushes++; return; }
        if (off >= 0x1000 && off < 0x1080 && M.dart_no_four)
            v &= ~BIT(3);
    }
    m_set(a, v);
}

u16 read16(u64 a) { u32 v = read32(a & ~3UL); return (u16)(v >> ((a & 2) * 8)); }
void write16(u64 a, u16 v)
{
    u64 d = a & ~3UL;
    u32 sh = (a & 2) * 8, cur = m_get(d);
    write32(d, (cur & ~(0xffffu << sh)) | ((u32)v << sh));
}

void udelay(u32 us) { M.now += us; }
u64 timeout_calculate(u32 usec) { return M.now + usec; }
bool timeout_expired(u64 t) { return M.now >= t; }

void *memalign(size_t align, size_t size)
{
    static uintptr_t hint = 0x300000000UL;
    size_t len = (size + align + 0xffff) & ~(size_t)0xffff;
    void *p = mmap((void *)hint, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED)
        return NULL;
    hint = (uintptr_t)p + len + 0x10000;
    uintptr_t al = ((uintptr_t)p + align - 1) & ~(uintptr_t)(align - 1);
    if (al + size > (1UL << 42)) {
        fprintf(stderr, "model: mmap gave 0x%lx, beyond 42 bits\n", (unsigned long)al);
        exit(2);
    }
    return (void *)al;
}
void free_hook(void *p) { (void)p; M.frees++; }

int model_printf(const char *fmt, ...)
{
    va_list ap;
    char buf[512];
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (M.verbose)
        fputs(buf, stdout);
    if (strstr(buf, "STOP") || strstr(buf, "GUARD") || strstr(buf, "guard:"))
        M.stops++;
    return n;
}

/* ---- ADT ---- */
const void *adt = (const void *)1;
enum { N_APCIE = 1, N_BRIDGE, N_GPIO, N_DART, N_ANS, N_SART, N_CHOSEN };
static const struct { const char *path; int id; } nodes[] = {
    {"/arm-io/apcie0", N_APCIE},    {"/arm-io/apcie0/pci-bridge0", N_BRIDGE}, {"/arm-io/gpio0", N_GPIO},
    {"/arm-io/dart-apcie0", N_DART}, {"/arm-io/ans", N_ANS}, {"/arm-io/sart-ans", N_SART}, {"/chosen", N_CHOSEN},
};
int adt_path_offset(const void *a, const char *path)
{
    (void)a;
    for (size_t i = 0; i < ARRAY_SIZE(nodes); i++)
        if (!strcmp(nodes[i].path, path))
            return nodes[i].id;
    return -1;
}
int adt_path_offset_trace(const void *a, const char *path, int *offsets)
{
    int n = adt_path_offset(a, path);
    if (n >= 0) {
        offsets[0] = n;
        offsets[1] = 0;
    }
    return n;
}
bool adt_is_compatible(const void *a, int node, const char *compat)
{
    (void)a;
    return node == N_APCIE && !strcmp(compat, "apcie,t6050");
}
static u8 regprop[16 * 40];
const void *adt_getprop(const void *a, int node, const char *name, u32 *lenp)
{
    (void)a;
    if (node == N_APCIE && !strcmp(name, "reg")) {
        if (lenp)
            *lenp = sizeof(regprop);
        return regprop;
    }
    if (node == N_BRIDGE && !strcmp(name, "centauri-builtin"))
        return regprop;
    return NULL;
}
int adt_getprop_copy(const void *a, int node, const char *name, void *out, size_t len)
{
    (void)a;
    u32 u = 0;
    u32 quad[4];
    u8 mac[6];
    const void *src = &u;
    size_t n = 4;
    if (node == N_APCIE && !strcmp(name, "lane-cfg")) u = 0;
    else if (node == N_BRIDGE && !strcmp(name, "apcie-port")) u = 0;
    else if (node == N_BRIDGE && !strcmp(name, "t-refclk-to-perst")) u = 100;
    else if (node == N_GPIO && !strcmp(name, "AAPL,phandle")) u = 0x55;
    else if (node == N_CHOSEN && !strcmp(name, "board-id")) u = 0x0c;
    else if (node == N_CHOSEN && !strcmp(name, "chip-id")) u = 0x6050;
    else if (node == N_BRIDGE && !strcmp(name, "function-perst")) {
        quad[0] = 0x55; quad[1] = 0x4750494f; quad[2] = 80; quad[3] = 0;
        src = quad; n = 16;
    } else if (node == N_BRIDGE && !strcmp(name, "function-reset")) {
        quad[0] = 0x66; quad[1] = 0x704b5734; quad[2] = 0x67503133; quad[3] = 0x00800000;
        src = quad; n = 16;
    } else if (node == N_CHOSEN && (!strcmp(name, "mac-address-wifi0") || !strcmp(name, "mac-address-bluetooth0"))) {
        for (int i = 0; i < 6; i++) mac[i] = (u8)(0x10 + i);
        src = mac; n = 6;
    } else
        return -1;
    if (len < n)
        return -1;
    memcpy(out, src, n);
    return (int)n;
}
int adt_get_reg(const void *a, int *path, const char *prop, int idx, u64 *addr, u64 *size)
{
    (void)a; (void)prop;
    int node = path[0];
    *addr = 0; *size = 0;
    if (node == N_APCIE) {
        if (idx == 0) { *addr = M_ECAM; *size = 0x10000000; }
        else if (idx == 1) { *addr = M_RC; *size = 0x4000; }
        else if (idx == 2) { *addr = M_PHY; *size = 0x8000; }
        else if (idx == 3) { *addr = M_PHYCTL; *size = 0x100; }
        else if (idx == 5) { *addr = M_AXI2AF; *size = 0x1000; }
        else if (idx == 10) { *addr = M_AUS5_TOP; *size = 0x100; }
        else if (idx == 11) { *addr = M_AUS5_AON; *size = 0x100; }
        else if (idx >= 16 && idx < 40) {
            int p = (idx - 16) / 6, k = (idx - 16) % 6;
            if (k == 0) { *addr = m_port(p); *size = 0x8000; }
            else if (k == 2) { *addr = m_lane(p); *size = 0x10; }
            else if (k == 3) { *addr = m_i2axi(p); *size = 0x4000; }
            else { *addr = AP(idx); *size = 0x100; }
        } else { *addr = AP(idx); *size = 0x100; }
        return 0;
    }
    if (node == N_GPIO && idx == 0) { *addr = M_GPIO; *size = 0x1000; return 0; }
    if (node == N_DART && idx == 0) { *addr = M_DART; *size = 0x20000; return 0; }
    if (node == N_ANS && idx == 0) { *addr = M_ANS; *size = 0x10000; return 0; }
    if (node == N_ANS && idx == 9) { *addr = M_NVME; *size = 0x2000; return 0; }
    if (node == N_SART && idx == 0) { *addr = M_SART; *size = 0x200; return 0; }
    return -1;
}

/* ---- PMGR, tunables, SMC, RTKit, NVMe, WDT ---- */
static const char *psn[] = {"APCIE_ST0", "APCIE_ST1", "FAB6_SOC", "ANS", "APCIE_SYS_ST0", "APCIE_SYS_ST1",
                            "GPIO", "APCIE_GP", "APHY_GP_AUS5_A", "APCIE_SYS_GP", "APCIE_PHY_SW", "APHY_GP_AUS5"};
uintptr_t pmgr_psreg_by_name(int die, const char *name)
{
    (void)die;
    for (size_t i = 0; i < ARRAY_SIZE(psn); i++)
        if (!strcmp(psn[i], name))
            return M_PS + 8 * i;
    return 0;
}
int pmgr_power_is_on(int die, const char *name) { (void)die; (void)name; return 1; }
int pmgr_set_mode(uintptr_t addr, u8 mode) { m_set(addr, (m_get(addr) & ~0xffu & ~BIT(28)) | mode | 0xf0); return 0; }
int tunables_validate_local(const char *p, const char *q, u64 s) { (void)p; (void)q; (void)s; return 0; }
int tunables_apply_local_addr(const char *p, const char *q, uintptr_t b) { (void)p; (void)q; (void)b; M.tunables++; return 0; }
int tunables_check_local_addr(const char *p, const char *q, uintptr_t b, const struct tunable_excuse *e)
{ (void)p; (void)q; (void)b; (void)e; return 0; }
struct smc_dev { int x; };
static struct smc_dev smc_one;
smc_dev_t *smc_init(void) { M.smc_sessions++; return &smc_one; }
void smc_shutdown(smc_dev_t *s) { (void)s; }
int smc_read_u32(smc_dev_t *s, u32 key, u32 *v) { (void)s; *v = key == 0x67503133 ? M.gp13 : 0; return 0; }
int smc_write_u32(smc_dev_t *s, u32 key, u32 v)
{
    (void)s;
    if (key == 0x67503133) {
        M.gp13 = v;
        M.released = v & 1;
        if (M.ngp13 < 8)
            M.gp13_log[M.ngp13] = v;
        M.ngp13++;
    }
    return 0;
}
void rtkit_set_boot_timeout(u64 t) { (void)t; }
bool nvme_init(void) { m_set(M_ANS + 0x44, m_get(M_ANS + 0x44) | 0x10); return true; }
void nvme_shutdown(void) { m_set(M_ANS + 0x44, m_get(M_ANS + 0x44) & ~0x10u); }
bool nvme_read(u32 nsid, u64 lba, void *b) { (void)nsid; memset(b, (int)lba, SZ_4K); memcpy(b, "EFI PART", 8); return true; }
void wdt_reboot(void) { fprintf(stderr, "model: wdt_reboot (storage lost) called\n"); exit(3); }

void model_reset(void)
{
    memset(used, 0, sizeof(used));
    memset(&M, 0, sizeof(M));
    M.dart_nsid = 19;
    m_set(PERST, 0x202);               /* output low: PERST# asserted, as iBoot leaves it */
    m_set(M_ANS + 0x44, 0x10);         /* ANS running */
    m_set(M_NVME + 0x1300, 0xde71ce55);
    m_set(M_NVME + 0x14, 0x460001);
    m_set(M_NVME + 0x1c, 1);
    for (int i = 0; i < 6; i++)
        m_set(M_PS + 8 * i, 0x1000030f);
    for (int i = 0; i < 16; i++)
        m_set(M_SART + 4 * i, 0x100 + i);
    /* root port: Express capability at 0x40, Gen3 advertised in LNKCAP2, unnumbered buses */
    m_set(M_ECAM + 0x00, 0x100c106b);
    m_set(M_ECAM + 0x34, 0x40);
    m_set(M_ECAM + 0x40, 0x00420010);
    m_set(M_ECAM + 0x40 + 0x2c, 0x0e);
    m_set(M_ECAM + 0x40 + 0x10, 0x00130000);
    for (int fn = 0; fn < 3; fn++)
        fn_reset(fn);
}
