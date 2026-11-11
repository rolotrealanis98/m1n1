/* Fake T6050 ANS model. Semantics follow Linux drivers/nvme/host/apple.c (pinned b36b78eb...) and
 * the live J716s ADT/DT; RTKit messages follow m1n1 rtkit.c. Host-only. */
#include "fake_ans.h"
extern unsigned r46_flip_read;
unsigned chk_fail_write_n, chk_max_write_nlb;
extern unsigned chk_fail_write_n;  /* CHECK.md: fail the n-th ANS WRITE (1-based) with a media error */
extern unsigned chk_max_write_nlb; /* CHECK.md: largest NLB seen in any ANS WRITE */
#include "s1_stage.h"
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

struct fake_state F;
static jmp_buf hang_env;
static bool hang_armed;
static u64 ARENA; /* real host base of the arena, fixed for the process */
void *adt = (void *)1;

#define BOOT_OK 0xde71ce55u
#define PWR_SLEEP 0x1
#define PWR_QUIESCED 0x10
#define PWR_ON 0x20
#define PWR_INIT 0x220

int fake_log(const char *fmt, ...)
{
    if (getenv("FAKE_VERBOSE")) {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
    }
    return 0;
}

u32 fake_crc32(const u8 *p, size_t n)
{
    u32 c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}

static void put32(u8 *b, unsigned o, u32 v) { memcpy(b + o, &v, 4); }
static void put64(u8 *b, unsigned o, u64 v) { memcpy(b + o, &v, 8); }
static u32 get32(const u8 *b, unsigned o) { u32 v; memcpy(&v, b + o, 4); return v; }
static u64 get64(const u8 *b, unsigned o) { u64 v; memcpy(&v, b + o, 8); return v; }

const u8 W_TYPE[16] = {0xa2,0xa0,0xd0,0xeb,0xe5,0xb9,0x33,0x44,0x87,0xc0,0x68,0xb6,0xb7,0x26,0x99,0xc7};
const u8 W_GUID[16] = {0x59,0x72,0xac,0x89,0xa4,0xfa,0x10,0x41,0xb6,0x21,0xe9,0xa2,0x93,0x4b,0x45,0xc1};
static void entry(u8 *e, const u8 *type, const u8 *guid, u64 first, u64 last)
{
    memcpy(e, type, 16); memcpy(e + 16, guid, 16); put64(e, 32, first); put64(e, 40, last);
}
static void gpt_entries(u8 *arr) /* 128 x 128 bytes = 4 sectors */
{
    static const u8 apfs[16] = {0xef,0x57,0x34,0x7c,0,0,0xaa,0x11,0xaa,0x11,0,0x30,0x65,0x43,0xec,0xac};
    static const u8 lnx[16] = {0xaf,0x3d,0xc6,0x0f,0x83,0x84,0x72,0x47,0x8e,0x79,0x3d,0x69,0xd8,0x47,0x7d,0xe4};
    u8 g[16];
    memset(arr, 0, 16384);
    for (int i = 0; i < 6; i++) { memset(g, 0x10 + i, 16); (void)g; }
    memset(g, 0x11, 16); entry(arr + 0 * 128, apfs, g, 6, 140805);
    memset(g, 0x12, 16); entry(arr + 1 * 128, apfs, g, 140806, 174213070);   /* real p2, then 49-block gap */
    memset(g, 0x13, 16); entry(arr + 2 * 128, apfs, g, 198637574, 200102374);
    memset(g, 0x14, 16); entry(arr + 3 * 128, W_TYPE, g, 200102375, 200224443); /* ESP-like */
    memset(g, 0x15, 16); entry(arr + 4 * 128, lnx, g, 200224444, 242949119);
    memset(g, 0x16, 16); entry(arr + 5 * 128, apfs, g, 242965551, 244276258);
    u64 first = W_START, last = W_START + W_BLOCKS - 1;
    const u8 *type = W_TYPE, *guid = W_GUID;
    u8 badtype[16], badguid[16];
    memcpy(badtype, W_TYPE, 16); badtype[0] ^= 1;
    memcpy(badguid, W_GUID, 16); badguid[15] ^= 1;
    if (F.cfg.gpt_mutation == GM_TYPE) type = badtype;
    if (F.cfg.gpt_mutation == GM_GUID) guid = badguid;
    if (F.cfg.gpt_mutation == GM_START) { first += 1; }
    if (F.cfg.gpt_mutation == GM_SIZE) { last -= 1; }
    entry(arr + 6 * 128, type, guid, first, last);
    if (F.cfg.gpt_mutation == GM_OVERLAP) put64(arr + 1 * 128, 40, W_START + 5); /* p2 runs into p7 */
}
#define M_ (F.cfg.gpt_mutation)
static void bk_entries(u8 *arr)
{
    gpt_entries(arr);
    if (M_ == 20) arr[6 * 128 + 16] ^= 1;                              /* stale backup: entry 7 GUID */
    if (M_ == 21) put64(arr + 6 * 128, 40, W_START + W_BLOCKS - 2);    /* stale backup: entry 7 range */
    if (M_ == 26) arr[0 * 128 + 16] ^= 1;                              /* backup differs in entry 1 only */
}
static void gpt_header(u8 *out, bool backup)
{
    u8 arr[16384];
    if (backup) bk_entries(arr); else gpt_entries(arr);
    memcpy(out, F.cfg.gpt_mutation == GM_SIG ? "EFI PARX" : "EFI PART", 8);
    put32(out, 8, 0x00010000);
    put32(out, 12, 92);
    put64(out, 24, backup ? F_NSZE - 1 : 1);
    put64(out, 32, F.cfg.gpt_mutation == GM_CAPACITY ? F_NSZE : (backup ? 1 : F_NSZE - 1)); /* AlternateLBA */
    put64(out, 40, 6);
    put64(out, 48, F.cfg.gpt_mutation == GM_USABLE ? W_START + 100 : F_NSZE - 6);
    const u8 disk_guid[16] = S1N_PIN_DISK_BYTES;
    memcpy(out + 56, disk_guid, 16);
    if (F.cfg.gpt_mutation == GM_DISK_GUID) out[56] ^= 1;
    put64(out, 72, backup ? F_NSZE - 5 : 2);
    put32(out, 80, F.cfg.gpt_mutation == GM_NENTRIES ? 6 : 128);
    put32(out, 84, 128);
    put32(out, 88, fake_crc32(arr, 128 * 128) ^ (F.cfg.gpt_mutation == GM_ENT_CRC ? 1u : 0u));
    if (backup && M_ == 23) put64(out, 24, F_NSZE - 2);   /* backup MyLBA wrong */
    if (backup && M_ == 24) put64(out, 72, F_NSZE - 33);  /* backup entries LBA wrong */
    if (backup && M_ == 27) put64(out, 40, 7);            /* backup FirstUsable differs */
    if (backup && M_ == 28) out[57] ^= 1;                 /* backup disk GUID differs */
    put32(out, 16, fake_crc32(out, 92) ^ ((F.cfg.gpt_mutation == GM_HDR_CRC || (!backup && F.cfg.gpt_mutation == GM_PRIMARY_HDR_CRC)) ? 1u : 0u));
}
/* r4: writable disk = base pattern < durable media overlay < volatile cache overlay. */
#define OV_SLOTS (1u << 15)
struct ov { u64 lba; u8 used; u8 *data; };
static struct ov ov_cache[OV_SLOTS], ov_media[OV_SLOTS];
static struct ov *ov_find(struct ov *t, u64 lba, bool create)
{
    u64 h = (lba * 0x9e3779b97f4a7c15ull) >> 49;
    for (unsigned i = 0; i < OV_SLOTS; i++) {
        struct ov *e = &t[(h + i) & (OV_SLOTS - 1)];
        if (e->used == 1 && e->lba == lba) return e;
        if (!e->used) {
            if (!create) return NULL;
            e->used = 1; e->lba = lba; e->data = calloc(1, 4096);
            return e;
        }
    }
    fprintf(stderr, "FAIL: fake overlay full\n");
    abort();
}
static void ov_clear(struct ov *t)
{
    for (unsigned i = 0; i < OV_SLOTS; i++) { if (t[i].used) free(t[i].data); t[i].used = 0; t[i].data = NULL; }
}
static unsigned ov_count(const struct ov *t)
{
    unsigned n = 0;
    for (unsigned i = 0; i < OV_SLOTS; i++) n += t[i].used == 1;
    return n;
}
void fake_power_loss(void) { ov_clear(ov_cache); }
unsigned fake_cached_blocks(void) { return ov_count(ov_cache); }
unsigned fake_durable_blocks(void) { return ov_count(ov_media); }
static void persist_cache(void)
{
    for (unsigned i = 0; i < OV_SLOTS; i++)
        if (ov_cache[i].used == 1) memcpy(ov_find(ov_media, ov_cache[i].lba, true)->data, ov_cache[i].data, 4096);
    ov_clear(ov_cache);
}
bool fake_overlay_inside_window(void)
{
    for (unsigned k = 0; k < 2; k++) {
        const struct ov *t = k ? ov_media : ov_cache;
        for (unsigned i = 0; i < OV_SLOTS; i++)
            if (t[i].used == 1 && (t[i].lba < W_START || t[i].lba >= W_START + W_BLOCKS)) return false;
    }
    return true;
}
static void base_block(u64 lba, u8 *out);
void fake_media_block(u64 lba, u8 *out)
{
    struct ov *e = ov_find(ov_media, lba, false);
    if (e) { memcpy(out, e->data, 4096); if (F.cfg.corrupt_canary_read) out[0] ^= 1; }
    else base_block(lba, out);
}
/* Data sectors carry their own physical LBA so translation errors are detectable. */
u64 fake_phys_of_pattern(const u8 *block) { u64 v; memcpy(&v, block, 8); return v; }
void fake_disk_block(u64 lba, u8 *out)
{
    struct ov *e = ov_find(ov_cache, lba, false);
    if (e) { memcpy(out, e->data, 4096);
        if (F.cfg.corrupt_canary_read) out[0] ^= 1;
        return; }
    fake_media_block(lba, out);
}
static void base_block(u64 lba, u8 *out)
{
    memset(out, 0, 4096);
    if (lba == 1 || lba == F_NSZE - 1) {
        if (lba != 1 && M_ == 22) return;                    /* no backup header at NSZE-1 */
        if (lba != 1 && M_ == 25) { gpt_header(out, false); return; } /* primary copy at NSZE-1 */
        gpt_header(out, lba != 1);
        if (lba != 1 && F.cfg.gpt_mutation == GM_BACKUP) out[16] ^= 1;
        return;
    }
    if (lba >= F_NSZE - 5 && lba < F_NSZE - 1) {
        u8 arr[16384];
        bk_entries(arr);
        memcpy(out, arr + (lba - (F_NSZE - 5)) * 4096, 4096);
        if (F.cfg.gpt_mutation == GM_BACKUP_ARRAY) out[0] ^= 1;
        if (M_ == 29 && lba == F_NSZE - 4) out[8 * 128 + 16] ^= 1; /* T1: backup array differs in sector 2 only (entry 41), header CRC field unchanged */
        return;
    }
    if (lba >= 2 && lba < 6) {
        u8 arr[16384];
        gpt_entries(arr);
        memcpy(out, arr + (lba - 2) * 4096, 4096);
        return;
    }
    put64(out, 0, lba);
    for (int i = 8; i < 4096; i++) out[i] = (u8)(lba * 31 + i * 7);
    return;
    if (lba == 1) {
        memcpy(out, "EFI PART", 8);
        put32(out, 8, 0x00010000);
        put32(out, 12, 92);
        put64(out, 24, 1);
        put64(out, 32, F_NSZE - 1);
        put64(out, 40, 6);
        put64(out, 48, F_NSZE - 6);
        for (int i = 0; i < 16; i++)
            out[56 + i] = (u8)(0xa0 + i);
        put64(out, 72, 2);
        put32(out, 80, 128);
        put32(out, 84, 128);
        put32(out, 88, 0x12345678);
        put32(out, 16, fake_crc32(out, 92));
        if (F.cfg.corrupt_lba1)
            out[50] ^= 0x40;
        return;
    }
    for (int i = 0; i < 4096; i++)
        out[i] = (u8)(lba * 31 + i * 7);
}

static bool in_region(u64 b, u64 sz, u64 a, u64 len)
{
    return b && len && a >= b && a + len <= b + sz && a + len > a;
}
bool fake_in_arena(u64 a, u64 len)
{
    return in_region(ARENA, ARENA_SIZE, a, len) || in_region(F.dma2_base, F.dma2_size, a, len);
}
void fake_set_dma_region2(u64 base, u64 size) { F.dma2_base = base; F.dma2_size = size; }

static void note_dma(u64 a) { if (F.ndma < 256) F.dma_addrs[F.ndma++] = a; }

static bool dma_ok(u64 a, u64 len)
{
    note_dma(a);
    if (!fake_in_arena(a, len)) {
        F.dma_violations++;
        return false;
    }
    return true;
}

static void dma_write(u64 a, const void *src, u64 len)
{
    if (dma_ok(a, len))
        memcpy((void *)a, src, len);
}

static bool dma_read(u64 a, void *dst, u64 len)
{
    if (!dma_ok(a, len))
        return false;
    memcpy(dst, (void *)a, len);
    return true;
}

/* ---------- r4.3: PRP / alignment model ----------
 * NVMe memory page = 4 KiB (CC.MPS = 0). PRP rules (NVMe base spec "Physical Region Page Entry
 * and List"): PRP1 may carry an offset; if the transfer runs past PRP1's page, PRP2 is the second
 * page (<= 2 pages) or a PRP-list pointer. Linux sets NVME_QUIRK_ADMIN_PAGE_ALIGN for this
 * controller (apple.c:2495, nvme.h:185 "admin queue DMA buffers must be page aligned").
 * PRP_STRICT rejects what such a controller plausibly rejects (status 0x13 PRP Offset Invalid, no
 * DMA); PRP_SPEC / PRP_MASK accept and DMA so the TEST can see where bytes would land. Separately
 * and in every model, a data DMA outside the supplied resident buffers (dma_outside) or into a
 * registered queue/TCB array (dma_into_queue) is a test failure. */
#define NV_PAGE 0x1000ULL
static unsigned admin_depth(void);
static unsigned admin_cq_size(void);
static u64 io_sq_base;
static bool ovl(u64 b, u64 n, u64 a, u64 len) { return b && n && len && a < b + n && b < a + len; }
static bool overlaps_queue(u64 a, u64 len)
{
    u64 ntcb = (u64)F.num_tcbs + 1;
    return ovl(F.asq, (u64)admin_depth() * 64, a, len) || ovl(F.acq, (u64)admin_cq_size() * 16, a, len) ||
           ovl(F.asq_tcb, ntcb * 128, a, len) || ovl(F.iosq_tcb, ntcb * 128, a, len) ||
           (F.io_cq_valid && ovl(F.io_cq_base, (u64)F.io_cq_size * 16, a, len)) ||
           (F.io_sq_valid && ovl(io_sq_base, (u64)F.io_sq_size * 64, a, len));
}
bool fake_overlaps_queue(u64 a, u64 len) { return overlaps_queue(a, len); }
static void chk16k(u64 a) { if (a & 0x3fff) F.not16k++; }
static bool dma_read(u64 a, void *dst, u64 len);
/* PRP_STRICT validation of a data transfer of `len` bytes. Returns 0 or NVMe status 0x13. */
static u16 prp_check(bool admin, u64 prp1, u64 prp2, u64 len)
{
    if (F.cfg.prp_model != PRP_STRICT) return 0;
    if (!prp1 || (prp1 & 3) || (admin && (prp1 & (NV_PAGE - 1)))) goto bad;
    u64 first = NV_PAGE - (prp1 & (NV_PAGE - 1));
    if (len <= first) return 0;
    u64 rem = len - first;
    if (rem <= NV_PAGE) { if (!prp2 || (prp2 & (NV_PAGE - 1))) goto bad; return 0; }
    if (!prp2 || (prp2 & 7)) goto bad;
    for (u64 i = 0; i * NV_PAGE < rem; i++) {
        u64 e;
        if (!dma_read(prp2 + 8 * i, &e, 8) || !e || (e & (NV_PAGE - 1))) goto bad;
    }
    return 0;
bad:
    F.prp_rejects++;
    return 0x13;
}
static void data_put(u64 a, const u8 *src, u64 n)
{
    note_dma(a);
    F.data_dmas++;
    if (overlaps_queue(a, n)) F.dma_into_queue++;
    if (!fake_in_arena(a, n)) { F.dma_outside++; F.dma_violations++; return; } /* never touch host memory */
    memcpy((void *)a, src, n);
}
/* Device -> host data DMA of `len` bytes per the configured model. */
static void data_dma(u64 prp1, u64 prp2, const u8 *src, u64 len)
{
    bool mask = F.cfg.prp_model == PRP_MASK;
    u64 off = prp1 & (NV_PAGE - 1);
    u64 first = mask ? (len < NV_PAGE ? len : NV_PAGE) : (len < NV_PAGE - off ? len : NV_PAGE - off);
    data_put(mask ? prp1 - off : prp1, src, first);
    if (r46_flip_read && F.io_reads + 1 == r46_flip_read && fake_in_arena(prp1, 1))
        *(u8 *)prp1 ^= 0xff;
    u64 done = first;
    if (done >= len) return;
    if (len - done <= NV_PAGE) { data_put(prp2, src + done, len - done); return; }
    for (u64 i = 0; done < len; i++) {
        u64 e = 0, n = len - done < NV_PAGE ? len - done : NV_PAGE;
        if (!dma_read(prp2 + 8 * i, &e, 8)) { F.dma_outside++; return; }
        data_put(e, src + done, n);
        done += n;
    }
}

/* ---------- arena ---------- */
void *arena_memalign(size_t align, size_t sz)
{
    if (!align)
        align = 16;
    u64 c = (F.arena_cursor + align - 1) & ~((u64)align - 1);
    if (c + sz > ARENA_SIZE)
        return NULL;
    F.arena_cursor = c + sz;
    F.allocs++;
    return F.arena + c;
}
void *arena_calloc(size_t n, size_t sz) { void *p = arena_memalign(16, n * sz); if (p) memset(p, 0, n * sz); return p; }
void *arena_malloc(size_t sz) { return arena_memalign(16, sz); }

static bool fw_live(void) { return (F.cpu_control & 0x10) && F.fw_state >= 1 && F.fw_state <= 4 && F.iop_power != PWR_SLEEP; }

void arena_free(void *p)
{
    if (!p)
        return;
    F.frees++;
    u64 a = (u64)p;
    for (unsigned i = 0; i < F.nbufs; i++)
        if (a >= F.buf_iova[i] && a < F.buf_iova[i] + F.buf_size[i] && fw_live())
            F.frees_while_live++;
}
bool fake_is_heap(void *p) { return fake_in_arena((u64)p, 1); }

/* ---------- misc platform shims ---------- */
int pmgr_reset(int die, const char *name) { (void)die; (void)name; F.pmgr_resets++; return 0; }
int dart_map(void *d, u64 i, void *b, size_t l) { (void)d; (void)i; (void)b; (void)l; F.dart_calls++; return -1; }
void dart_unmap(void *d, u64 i, size_t l) { (void)d; (void)i; (void)l; F.dart_calls++; }
void *dart_translate(void *d, u64 i) { (void)d; (void)i; F.dart_calls++; return NULL; }
u64 iova_alloc(void *d, size_t s) { (void)d; (void)s; F.dart_calls++; return 0; }
void iova_free(void *d, u64 i, size_t s) { (void)d; (void)i; (void)s; F.dart_calls++; }

/* ---------- fake ADT ---------- */
struct adt_property;
static struct { char name[32]; u32 size; u8 value[4]; } secure_bar = {"nvme-secure-bar", 4, {1, 0, 0, 0}};
int adt_path_offset_trace(const void *a, const char *path, int *offsets)
{
    (void)a;
    int n = !strcmp(path, "/arm-io/ans") ? 1 : !strcmp(path, "/arm-io/sart-ans") ? 2 : -1;
    if (n > 0 && offsets) {
        offsets[0] = n;
        offsets[1] = 0;
    }
    return n;
}
const struct adt_property *adt_get_property(const void *a, int node, const char *name)
{
    (void)a;
    return (node == 1 && !strcmp(name, "nvme-secure-bar")) ? (const struct adt_property *)&secure_bar : NULL;
}
const void *adt_getprop(const void *a, int node, const char *name, u32 *lenp)
{
    (void)a;
    static const u32 irqs[5] = {1348, 1347, 1350, 1349, 2338};
    if (node == 1 && !strcmp(name, "interrupts")) {
        if (lenp) *lenp = sizeof(irqs);
        return irqs;
    }
    if (node == 2 && !strcmp(name, "sart-version")) {
        if (lenp)
            *lenp = 4;
        return &F.cfg.sart_version;
    }
    return NULL;
}
int adt_getprop_copy(const void *a, int node, const char *name, void *out, size_t len)
{
    (void)a; (void)node; (void)name; (void)out; (void)len;
    return -1;
}
int adt_get_reg(const void *a, int *path, const char *prop, int idx, u64 *addr, u64 *size)
{
    (void)a; (void)prop;
    u64 ad = 0, sz = 0;
    if (path[0] == 1 && idx == 0) { ad = F.cfg.adt_reg0; sz = F_ASC_SIZE; }
    else if (path[0] == 1 && idx == 3) { ad = F_NVMMU; sz = 0x60010; }
    else if (path[0] == 1 && idx == 9) { ad = F.cfg.adt_reg9; sz = 0x10000; }
    else if (path[0] == 2 && idx == 0) { ad = F.cfg.adt_sart; sz = F_SART_SIZE; }
    else return -1;
    if (addr) *addr = ad;
    if (size) *size = sz;
    return 0;
}
bool adt_is_compatible(const void *a, int node, const char *c) { (void)a; (void)node; (void)c; return false; }
int adt_first_child_offset(const void *a, int off) { (void)a; (void)off; return 3; }

/* ---------- SART ---------- */
bool fake_sart_covers(u64 a, u64 len)
{
    for (int i = 0; i < 16; i++) {
        if (!F.s_cfg[i])
            continue;
        u64 b = (u64)F.s_paddr[i] << 12, s = (u64)F.s_size[i] << 12;
        if (a >= b && a + len <= b + s)
            return true;
    }
    return false;
}
unsigned fake_sart_free_unprotected(void)
{
    unsigned n = 0;
    for (int i = F.cfg.protected_entries; i < 16; i++)
        if (!F.s_cfg[i])
            n++;
    return n;
}
bool fake_sart_protected_unchanged(void)
{
    for (int i = 0; i < F.cfg.protected_entries; i++)
        if (F.s_cfg[i] != F.s_init_cfg[i] || F.s_paddr[i] != F.s_init_paddr[i] || F.s_size[i] != F.s_init_size[i])
            return false;
    return true;
}
static u32 sart_access(u32 off, bool w, u32 v)
{
    u32 *r = NULL;
    int i = -1;
    if (off < 0x40) { i = off / 4; r = &F.s_cfg[i]; }
    else if (off >= 0x60 && off < 0xa0) { i = (off - 0x60) / 4; r = &F.s_paddr[i]; }
    else if (off >= 0xc0 && off < 0x100) { i = (off - 0xc0) / 4; r = &F.s_size[i]; }
    if (!r) { if (w) F.unmapped++; return 0; }
    if (!w)
        return *r;
    F.sart_writes++;
    if (i < F.cfg.protected_entries)
        F.prot_writes++;
    if (off < 0x40 && v == 0 && F.s_cfg[i] && fw_live()) {
        u64 b = (u64)F.s_paddr[i] << 12, s = (u64)F.s_size[i] << 12;
        for (unsigned k = 0; k < F.nbufs; k++)
            if (F.buf_iova[k] < b + s && F.buf_iova[k] + F.buf_size[k] > b)
                F.sart_clear_while_live++;
    }
    if (!F.cfg.ignore_sart_writes)
        *r = v;
    return v;
}

/* ---------- mailbox + RTKit firmware ---------- */
static void fw_send(u64 m0, u32 ep)
{
    if (F.ot - F.oh >= 64)
        return;
    F.out0[F.ot % 64] = m0;
    F.out1[F.ot % 64] = ep;
    F.ot++;
}
#define T(x) ((u64)(x) << 52)

static int buf_index(u32 ep) { return ep == 1 ? 0 : ep == 2 ? 1 : ep == 4 ? 2 : -1; }
static u64 bufaddr[3], bufsize[3];

/* r4.4 (= r3.5): firmware messages that the real ANS can take seconds to send (cross-check D1). */
static void fw_send_late(u64 m0, u32 ep, u64 delay)
{
    if (!delay) { fw_send(m0, ep); return; }
    if (F.nlate >= 4) { fprintf(stderr, "FAIL: fake late-message queue full\n"); abort(); }
    F.late[F.nlate].m0 = m0; F.late[F.nlate].ep = ep; F.late[F.nlate].at = F.now + delay;
    F.nlate++;
}
static void late_tick(void)
{
    for (unsigned i = 0; i < F.nlate;) {
        if (F.now >= F.late[i].at) {
            fw_send(F.late[i].m0, F.late[i].ep);
            F.late[i] = F.late[--F.nlate];
        } else {
            i++;
        }
    }
}

static void fw_crash(void)
{
    F.crashed = true;
    if (bufaddr[0] && fake_in_arena(bufaddr[0], 64)) {
        u8 hdr[64] = {0};
        put32(hdr, 0, 'CLHE');
        put32(hdr, 32, 'CLHE');
        memcpy((void *)bufaddr[0], hdr, sizeof(hdr));
        fw_send(T(1) | ((u64)4 << 44), 1);
    }
    F.fw_state = 9;
}

void fake_spontaneous_syslog(unsigned n);
static void fw_syslog(void)
{
    if (F.fw_state != 4 || !bufaddr[1])
        return;
    u8 line[64] = {0};
    memcpy(line + 8, "ans", 3);
    memcpy(line + 32, "probe\n", 6);
    dma_write(bufaddr[1], line, sizeof(line));
    fw_send(T(5) | 0, 2);
    F.syslog_sent++;
    F.syslog_unacked++;
}

static void fw_rx(u64 m0, u32 ep)
{
    if (!(F.cpu_control & 0x10) || F.fw_state == 9)
        return;
    u32 type = (m0 >> 52) & 0xff;
    if (ep == 0) {
        u32 st = m0 & 0xffff;
        switch (type) {
        case 6:
            if (st == PWR_INIT && F.fw_state == 0) {
                if (!F.cfg.no_hello && F.cfg.hello_delay_us) {
                    F.hello_pending = true;
                    F.hello_at = F.now + F.cfg.hello_delay_us;
                } else if (!F.cfg.no_hello) {
                    fw_send(T(1) | (11u) | (12u << 16), 0);
                    F.hellos++;
                }
                F.fw_state = 1;
            } else if (st == PWR_SLEEP || st == PWR_QUIESCED) {
                if (!F.cfg.no_quiesce_ack) {
                    F.iop_power = st;
                    fw_send(T(7) | st, 0);
                }
            }
            break;
        case 2:
            if (F.fw_state == 1 && (m0 & 0xffff) == 12) {
                if (!F.cfg.no_epmap) /* r4.4: may be seconds late */
                    fw_send_late(T(8) | (1ull << 51) | ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 8)), 0,
                                 F.cfg.epmap_delay_us);
                F.fw_state = 2;
            }
            break;
        case 8:
            if (F.fw_state == 2 && (m0 & (1ull << 51))) {
                F.fw_state = 3;
                F.bufs_needed = 3;
            }
            break;
        case 5: {
            u32 idx = (m0 >> 32) & 0xff;
            if (idx == 1) fw_send(T(1) | ((u64)4 << 44), 1);
            if (idx == 2) { fw_send(T(1) | ((u64)2 << 44), 2); fw_send(T(8) | 16 | ((u64)0x80 << 24), 2); }
            if (idx == 4) fw_send(T(1) | ((u64)2 << 44), 4);
            if (idx == 8) fw_send(T(0x10), 8);
            break;
        }
        case 0xb:
            if (st == PWR_ON && F.fw_state == 3) {
                F.ap_power = PWR_ON;
                fw_send(T(0xb) | PWR_ON, 0);
                if (!F.cfg.boot_status_never_ok)
                    F.boot_status = BOOT_OK;
                F.fw_state = 4;
            } else if (st == PWR_QUIESCED) {
                if (!F.cfg.no_quiesce_ack) {
                    F.ap_power = PWR_QUIESCED;
                    fw_send(T(0xb) | PWR_QUIESCED, 0);
                }
            }
            break;
        }
        return;
    }
    int bi = buf_index(ep);
    if (type == 1 && bi >= 0 && F.fw_state == 3) {
        u64 iova = m0 & ((1ull << 42) - 1), sz = ((m0 >> 44) & 0xff) << 12;
        if (!fake_sart_covers(iova, sz) || !fake_in_arena(iova, sz)) {
            fw_crash();
            return;
        }
        bufaddr[bi] = iova;
        bufsize[bi] = sz;
        if (F.nbufs < 8) { F.buf_iova[F.nbufs] = iova; F.buf_size[F.nbufs] = sz; F.nbufs++; }
        note_dma(iova);
        if (++F.bufs_acked == F.bufs_needed && !F.cfg.no_power_on_ack) {
            F.iop_power = PWR_ON;
            fw_send_late(T(7) | PWR_ON, 0, F.cfg.power_on_delay_us); /* r4.4: may be seconds late */
        }
        return;
    }
    if (ep == 2 && type == 5 && F.syslog_unacked)
        F.syslog_unacked--;
}

static u64 mbox_access(u32 off, bool w, u64 v, int width)
{
    static u64 pend0;
    if (w) {
        if (off == 0x800) pend0 = v;
        else if (off == 0x808) fw_rx(pend0, (u32)v);
        else F.unmapped++;
        return 0;
    }
    (void)width;
    if (F.cfg.tamper_mailbox && F.iosq_reg) {
        F.mailbox_tampers++;
        for (unsigned tag = 2; tag <= 3; tag++) {
            u8 *slot = (u8 *)(uintptr_t)(F.iosq_reg + 64 * tag);
            if (slot[0] == 1) put64(slot, 40, W_START - 1);
        }
        F.cfg.tamper_mailbox = false;
    }
    if (off == 0x110) return 1u << 17;                         /* A2I empty, never full */
    if (off == 0x114) return F.oh == F.ot ? (1u << 17) : 0;    /* I2A empty flag */
    if (off == 0x830) return F.oh == F.ot ? 0 : F.out0[F.oh % 64];
    if (off == 0x838) { if (F.oh == F.ot) return 0; u64 r = F.out1[F.oh % 64]; F.oh++; return r; }
    return 0;
}

/* ---------- NVMe / NVMMU ---------- */
static unsigned admin_depth(void) { return (F.aqa & 0xfff) + 1; }
static unsigned admin_cq_size(void) { return ((F.aqa >> 16) & 0xfff) + 1; }

static void post_cqe(bool admin, u8 tag, u16 status, u64 result)
{
    u64 base = admin ? F.acq : F.io_cq_base;
    unsigned size = admin ? admin_cq_size() : F.io_cq_size;
    unsigned *tail = admin ? &F.acq_tail : &F.iocq_tail, *head = admin ? &F.acq_head : &F.iocq_head;
    unsigned *phase = admin ? &F.acq_phase : &F.iocq_phase;
    if (!size || (*tail + 1) % size == *head) {
        F.tag_fault++;
        return;
    }
    u8 c[16] = {0};
    put64(c, 0, result);
    u16 t = F.cfg.wrong_tag ? (u16)(tag ^ 1) : tag;
    memcpy(c + 12, &t, 2);
    u16 s = (u16)((status << 1) | (*phase & 1));
    memcpy(c + 14, &s, 2);
    dma_write(base + (u64)*tail * 16, c, 16);
    if (++*tail == size) { *tail = 0; *phase ^= 1; }
    F.tag_busy[tag] = 2;
}

static void tick(void)
{
    for (unsigned i = 0; i < F.npend;) {
        if (F.cfg.cqe_never) { i++; continue; }
        if (F.pend[i].left-- <= 0) {
            post_cqe(F.pend[i].admin, F.pend[i].tag, F.pend[i].status, F.pend[i].result);
            if (!F.pend[i].admin && F.cfg.duplicate_cqe)
                post_cqe(false, F.pend[i].tag, F.pend[i].status, F.pend[i].result);
            F.pend[i] = F.pend[--F.npend];
        } else {
            i++;
        }
    }
}

/* r4.4: Identify (0x06) is no longer an allowed admin opcode: nothing hardware-proven models it here. */
static bool admin_allowed(u8 op) { return op == 0x05 || op == 0x01 || op == 0x00 || op == 0x04; }
static bool admin_mutating(u8 op) { return op == 0x09 || op == 0x80 || op == 0x84 || op == 0x0d || op == 0x15 || op == 0x10 || op == 0x11 || op >= 0xc0; }
static bool io_mutating(u8 op) { return op == 0x00 || op == 0x01 || op == 0x04 || op == 0x08 || op == 0x09 || op >= 0x80; }

static void fault(void) { F.csts |= 2; F.cfs_events++; }
/* r4: resolve the PRPs of an n-page transfer (PRP1, PRP2 data or PRP2 list). */
static bool prp_pages(u64 prp1, u64 prp2, unsigned n, u64 *pages)
{
    if (!n || n > 256 || (prp1 & 4095)) { F.prp_errors++; return false; }
    pages[0] = prp1;
    if (n == 2) pages[1] = prp2;
    if (n > 2) {
        if (prp2 & 4095 || !dma_read(prp2, pages + 1, (u64)(n - 1) * 8)) { F.prp_errors++; return false; }
    }
    for (unsigned i = 1; i < n; i++)
        if (!pages[i] || (pages[i] & 4095)) { F.prp_errors++; return false; }
    return true;
}
static bool read_rest(u64 slba, unsigned nlb, u64 prp1, u64 prp2)
{
    static u64 pages[256];
    u8 blk[4096];
    if (!prp_pages(prp1, prp2, nlb, pages)) return false;
    for (unsigned i = 1; i < nlb; i++) { fake_disk_block(slba + i, blk); dma_write(pages[i], blk, 4096); }
    return true;
}

static void process(bool admin, u32 tag)
{
    if (F.cfg.syslog_backpressure && F.syslog_unacked > 32) { F.refused_doorbells++; return; }
    if (!(F.csts & 1) || F.fw_state != 4) { fault(); return; }
    unsigned depth = admin ? admin_depth() : (F.ioqa & 0xffff) + 1;
    if (tag >= 64 || tag >= depth || tag > F.num_tcbs) { F.tag_fault++; fault(); return; }
    if (admin ? tag >= 2 : tag < 2) F.tag_policy++; /* Linux/Mu: admin tags 0-1, I/O tags >= 2 */
    if (!admin) F.io_tag_seen[tag & 63]++;
    if (F.tag_busy[tag]) { F.tag_fault++; fault(); return; }
    if (!admin && (!F.io_sq_valid || !F.iosq_reg || !F.iocq_reg)) { fault(); return; }
    if (!admin && F.iosq_reg != io_sq_base) F.tcb_mismatch++;
    u64 sqb = admin ? F.asq : io_sq_base, tcbb = admin ? F.asq_tcb : F.iosq_tcb;
    u8 sqe[64], tcb[128];
    if (!dma_read(sqb + (u64)tag * 64, sqe, 64) || !dma_read(tcbb + (u64)tag * 128, tcb, 128)) { fault(); return; }
    u8 op = sqe[0];
    u32 nsid = get32(sqe, 4), cdw10 = get32(sqe, 40), cdw11 = get32(sqe, 44), cdw12 = get32(sqe, 48);
    u64 prp1 = get64(sqe, 24), prp2 = get64(sqe, 32);
    u8 want_flags = prp1 ? ((op & 1) ? 2 : 1) : 0;
    u16 tlen;
    memcpy(&tlen, tcb + 4, 2);
    u16 status = 0;
    if (sqe[2] != tag || tcb[2] != tag || tcb[0] != 0 || get64(tcb, 24) != prp1 || get64(tcb, 32) != prp2 ||
        tcb[1] != want_flags || tlen != (cdw12 & 0xffff)) {
        F.tcb_mismatch++;
        status = 0x2;
    }
    if (admin) { if (F.nadmin < 64) F.admin_op_seq[F.nadmin] = op; F.nadmin++; F.admin_ops[op & 63]++; }
    else { if (F.nio < 64) F.io_op_seq[F.nio] = op; F.nio++; F.io_ops[op & 63]++; }
    bool rw_ok = !admin && F.cfg.allow_rw && (op == 0x01 || op == 0x00);
    if (!rw_ok && (admin ? !admin_allowed(op) : op != 0x02)) F.forbidden_ops++;
    if (!rw_ok && (admin ? admin_mutating(op) : io_mutating(op))) F.mutating_ops++;
    static u8 data[256 * 4096];
    if (!status && admin) {
        switch (op) {
        case 0x06: {
            /* r4.4: UNMODELLED. Reaching here is already a test failure (forbidden_ops); the reply is
             * an NVMe-spec placeholder, not hardware behaviour (r4.2 poison model removed). */
            memset(data, 0, 4096);
            u32 cns = cdw10 & 0xff;
            if (cns == 1) {
                put32(data, 0, 0x106b);
                memset(data + 24, ' ', 40);
                const char *m = F.cfg.bad_model ? "APPLE SSD AP0512Z" : F_MODEL;
                memcpy(data + 24, m, strlen(m));
                memset(data + 64, ' ', 8);
                memcpy(data + 64, F_FW, strlen(F_FW));
                put32(data, 516, 3);
                data[525] = 1;
            } else if (cns == 0 && nsid == 1) {
                u64 nz = F.cfg.nsze_override ? F.cfg.nsze_override : F_NSZE;
                put64(data, 0, nz); put64(data, 8, nz); put64(data, 16, nz);
                data[130] = F.cfg.bad_lbads ? 9 : 12;
            } else if (cns == 0) {
                status = 0x0b;
                break;
            }
            if ((status = prp_check(true, prp1, prp2, 4096))) break; /* r4.3 */
            data_dma(prp1, prp2, data, 4096);
            break;
        }
        case 0x05:
            /* r4.4: cdw11 = PC (aura/m1n1, IEN 0) or PC|IEN (Linux/Mu), vector 0; else unproven. */
            if (cdw11 != 0x1 && cdw11 != 0x3) { F.cq_flags_unproven++; status = 0x2; break; }
            if ((status = prp_check(true, prp1, 0, ((cdw10 >> 16) + 1) * 16))) break; /* r4.3 */
            if ((cdw10 & 0xffff) != 1 || (cdw10 >> 16) + 1 > 64 || !dma_ok(prp1, ((cdw10 >> 16) + 1) * 16)) { status = 0x2; break; }
            chk16k(prp1);
            F.io_cq_valid = true; F.io_cq_base = prp1; F.io_cq_size = (cdw10 >> 16) + 1;
            F.iocq_tail = F.iocq_head = 0; F.iocq_phase = 1;
            break;
        case 0x01:
            if ((status = prp_check(true, prp1, 0, ((cdw10 >> 16) + 1) * 64))) break; /* r4.3 */
            if ((cdw10 & 0xffff) != 1 || (cdw11 >> 16) != 1 || !F.io_cq_valid || (cdw10 >> 16) + 1 > 64 ||
                !dma_ok(prp1, ((cdw10 >> 16) + 1) * 64)) { status = 0x2; break; }
            chk16k(prp1);
            F.io_sq_valid = true; io_sq_base = prp1; F.io_sq_size = (cdw10 >> 16) + 1;
            break;
        case 0x00: F.io_sq_valid = false; break;
        case 0x04: if (F.io_sq_valid) status = 0x0c; else F.io_cq_valid = false; break;
        default: status = 0x1;
        }
    } else if (!status) {
        if (op == 0x02) {
            u64 slba = cdw10 | ((u64)cdw11 << 32);
            unsigned nlb = (cdw12 & 0xffff) + 1;
            if (nsid != 1) { F.bad_nsid++; status = 0x0b; }
            else {
                if (nlb > 1) F.multi_block++;
                if (F.cfg.crash_on_read) { fw_crash(); F.tag_busy[tag] = 1; return; }
                /* r4.3: validate PRPs (strict model) before any DMA; DMA every block per the model. */
                if (nlb > 256) { status = 0x2; goto io_done; }
                if ((status = prp_check(false, prp1, prp2, (u64)nlb * 4096))) goto io_done;
                /* r4.3: data PRPs are 4 KiB pages inside the 16 KiB-aligned xfer buffer (not chk16k). */
                F.last_read_prp1 = prp1; F.last_read_prp2 = prp2; F.last_read_bytes = (u64)nlb * 4096;
                if (!F.rd_lo || prp1 < F.rd_lo) F.rd_lo = prp1;
                if (prp1 + (u64)nlb * 4096 > F.rd_hi) F.rd_hi = prp1 + (u64)nlb * 4096;
                for (unsigned b = 0; b < nlb; b++) fake_disk_block(slba + b, data + (u64)b * 4096);
                if (!F.cfg.read_no_dma) data_dma(prp1, prp2, data, (u64)nlb * 4096);
                F.io_reads++;
                if (F.cfg.error_status) status = 0x281;
            }
        } else if (op == 0x01 && F.cfg.allow_rw) {
            if (cdw12 & (1u << 30)) F.fua_writes++; /* r4.4: ANS is BROKEN_FUA (Linux); never sent */
            /* r4 WRITE: gather from the PRPs into the volatile cache overlay. */
            u64 slba = cdw10 | ((u64)cdw11 << 32);
            unsigned nlb = (cdw12 & 0xffff) + 1;
            static u64 pages[256];
            if (chk_fail_write_n && F.io_writes + 1 == chk_fail_write_n) F.cfg.write_error = true; /* CHECK.md hook */
            if (nsid != 1) { F.bad_nsid++; status = 0x0b; }
            else if ((status = prp_check(false, prp1, prp2, (u64)nlb * 4096))) { } /* r4.3 strict PRP model */
            else if (!prp_pages(prp1, prp2, nlb, pages)) status = 0x4;
            else {
                u64 bytes = (u64)nlb * 4096;
                bool contiguous = true;
                for (unsigned i = 1; i < nlb; i++) contiguous &= pages[i] == prp1 + 4096ull * i;
                if (!contiguous || F.clean_seq <= F.doorbell_seq || F.clean_lo > prp1 || F.clean_hi < prp1 + bytes)
                    F.uncleaned_writes++;
                if (slba < W_START || slba >= W_START + W_BLOCKS || nlb > W_START + W_BLOCKS - slba) {
                    F.writes_outside_window++;
                    status = 0x80; /* LBA out of range: nothing stored */
                } else if (F.cfg.write_error) {
                    status = 0x281;
                } else {
                    for (unsigned i = 0; i < nlb; i++) {
                        u8 blk[4096];
                        if (!dma_read(pages[i], blk, 4096)) { status = 0x4; break; }
                        memcpy(ov_find(ov_cache, slba + i, true)->data, blk, 4096);
                    }
                    if (!F.write_blocks || slba < F.wmin) F.wmin = slba;
                    if (slba + nlb - 1 > F.wmax) F.wmax = slba + nlb - 1;
                    F.write_blocks += nlb;
                }
                F.io_writes++;
                if (nlb > chk_max_write_nlb) chk_max_write_nlb = nlb;
            }
        } else if (op == 0x00 && F.cfg.allow_rw) {
            /* r4 FLUSH: volatile cache becomes durable media. */
            if (nsid != 1 && nsid != 0xffffffffu) status = 0x0b;
            else if (F.cfg.flush_error) status = 0x6;
            else persist_cache();
            F.io_flushes++;
        } else {
            status = 0x1;
        }
    }
io_done:
    if (admin && F.cfg.admin_error) status = 2;
    F.doorbell_seq = F.steps;
    F.tag_busy[tag] = 1;
    if (F.npend < 8) {
        F.pend[F.npend].admin = admin; F.pend[F.npend].tag = (u8)tag;
        F.pend[F.npend].left = (!admin && op == 0x00 && F.cfg.allow_rw && F.cfg.flush_delay) ? F.cfg.flush_delay : F.cfg.cqe_delay;
        F.pend[F.npend].status = status; F.pend[F.npend].result = 0;
        F.npend++;
    }
    fw_syslog();
}

static void base_half(u64 *reg, u32 off_lo_bit, u32 v)
{
    if (F.cc & 1) F.base_write_while_en++;
    if (off_lo_bit) *reg = (*reg & 0xffffffffull) | ((u64)v << 32);
    else *reg = (*reg & ~0xffffffffull) | v;
}

static u32 nvme_access(u32 off, bool w, u32 v)
{
    switch (off) {
    case 0x00: return w ? 0 : 0x2001003fu;
    case 0x04: return w ? 0 : 0x20;
    case 0x08: return w ? 0 : 0x10300;
    case 0x14:
        if (!w) return F.cc;
        /* r4.4 (= r3.5): a shutdown request = SHN != 0 written while enabled (the stopped handoff's
         * CC already holds SHN = 1 with EN = 0, probe 0x474000; clear32(EN) keeps it). */
        if ((v & 0xc000) && (F.cc & 1)) {
            F.shn_writes++;
            if (F.csts & 1) F.csts |= (2u << 2);
            if ((v & 0xc000) == 0x4000 && !F.cfg.shn_no_persist) { persist_cache(); F.shn_persists++; } /* r4: normal SHN persists */
        }
        if ((v & 1) && !(F.cc & 1)) {
            F.enables++;
            /* r4.4 (= r3.5): CC at enable: MPS 0, CSS 0, AMS 0, IOCQES 4, IOSQES 7 (iBoot, kept by
             * m1n1/aura mask32; probe) or 6 (Linux, Mu). Anything else is unproven -> CFS. */
            u32 mps = (v >> 7) & 0xf, css = (v >> 4) & 7, ams = (v >> 11) & 7;
            u32 iosqes = (v >> 16) & 0xf, iocqes = (v >> 20) & 0xf;
            if (mps || css || ams || iocqes != 4 || (iosqes != 6 && iosqes != 7)) {
                F.cc_unproven++;
                fault();
                F.cc = v;
                return 0;
            }
            /* r4.3: ASQ/ACQ bits 11:0 are reserved (page aligned); the NVMMU TCB arrays likewise. */
            u64 bases[4] = {F.asq, F.acq, F.asq_tcb, F.iosq_tcb};
            bool mis = false;
            for (int i = 0; i < 4; i++) { chk16k(bases[i]); if (bases[i] & (NV_PAGE - 1)) mis = true; }
            if (mis) { F.misaligned_base++; if (F.cfg.prp_model == PRP_STRICT) { fault(); F.cc = v; return 0; } }
            if (F.fw_state == 4 && F.boot_status == BOOT_OK && F.aqa && F.asq && F.acq && F.num_tcbs && F.asq_tcb && F.iosq_tcb) {
                if (!F.cfg.rdy_never_set) F.csts |= 1;
                F.acq_tail = F.acq_head = 0; F.acq_phase = 1;
            } else {
                fault();
            }
        } else if (!(v & 1) && (F.cc & 1)) {
            if (!F.cfg.rdy_never_clears) F.csts &= ~1u;
            F.io_cq_valid = F.io_sq_valid = false;
            memset(F.tag_busy, 0, sizeof(F.tag_busy));
            F.npend = 0;
        }
        F.cc = v;
        return 0;
    case 0x1c: return w ? 0 : F.csts;
    case 0x24: if (w) { if (F.cc & 1) F.base_write_while_en++; F.aqa = v; } return F.aqa;
    case 0x28: case 0x2c: if (w) base_half(&F.asq, off & 4, v); return (u32)(F.asq >> ((off & 4) * 8));
    case 0x30: case 0x34: if (w) base_half(&F.acq, off & 4, v); return (u32)(F.acq >> ((off & 4) * 8));
    case 0x1004:
        if (w) { if (v >= admin_cq_size()) F.tag_fault++; else F.acq_head = v; }
        return 0;
    case 0x100c:
        if (w) { if (v >= F.io_cq_size) F.tag_fault++; else F.iocq_head = v; }
        return 0;
    case 0x1200: case 0x1204: case 0x1208: case 0x120c: {
        u64 *r = off < 0x1208 ? &F.iosq_reg : &F.iocq_reg;
        if (w) {
            u64 old = *r;
            base_half(r, off & 4, v);
            /* r4.4: teardown clear of the post-M4 I/O queue registers (aura nvme_shutdown): only
             * after both I/O queues are deleted, +0x1200 (CMDS) before +0x1208 (CQES). */
            if (old && !*r) {
                if (F.io_sq_valid || F.io_cq_valid) F.ioq_reg_zero_while_queues++;
                if (r == &F.iocq_reg && F.iosq_reg) F.ioq_reg_zero_order_bad++;
            }
        }
        return (u32)(*r >> ((off & 4) * 8));
    }
    case 0x1210: if (w) F.ioqa = v; return F.ioqa;
    case 0x1300: return w ? 0 : F.boot_status;
    }
    F.unknown_nvme++;
    return 0;
}

static u32 nvmmu_access(u32 off, bool w, u32 v)
{
    switch (off) {
    case 0x24908: F.lsq_ctrl_access++; return 1;
    case 0x2490c: if (w) process(true, v); return 0;
    case 0x24910: if (w) process(false, v); return 0;
    case 0x28100: if (w) F.num_tcbs = v; return F.num_tcbs;
    case 0x28108: case 0x2810c: if (w) base_half(&F.asq_tcb, off & 4, v); return (u32)(F.asq_tcb >> ((off & 4) * 8));
    case 0x28110: case 0x28114: if (w) base_half(&F.iosq_tcb, off & 4, v); return (u32)(F.iosq_tcb >> ((off & 4) * 8));
    case 0x28118:
        if (w) {
            if (v >= 64 || F.tag_busy[v] != 2) F.tag_fault++;
            else F.tag_busy[v] = 0;
            F.tcb_stat = F.cfg.tcb_stat_fail ? 1 : 0;
        }
        return 0;
    case 0x28120: return w ? 0 : F.tcb_stat;
    case 0x29120: F.tcb_alias_reads++; return w ? 0 : F.cfg.tcb_alias_stat; /* r4.4: configurable */
    }
    F.unknown_nvmmu++;
    return 0;
}

/* ---------- MMIO dispatch ---------- */
static void step(void)
{
    F.steps++;
    F.now++;
    if (F.hello_pending && F.now >= F.hello_at) {
        F.hello_pending = false;
        fw_send(T(1) | (11u) | (12u << 16), 0);
        F.hellos++;
    }
    late_tick();
    tick();
    if (F.steps > F.step_cap) {
        if (hang_armed)
            longjmp(hang_env, 1);
        fprintf(stderr, "FAIL: step cap without trap\n");
        abort();
    }
}

static void trace(u8 bank, bool w, int width, u32 off, u64 v)
{
    if (F.ntr < sizeof(F.tr) / sizeof(F.tr[0]))
        F.tr[F.ntr++] = (struct ftrace){bank, w, (u8)width, off, v, F.steps};
}

static u64 access(u64 addr, bool w, u64 v, int width)
{
    step();
    if (addr >= F_ASC_CPU && addr < F_ASC_CPU + 0x8000) {
        u32 off = (u32)(addr - F_ASC_CPU);
        trace(B_ASC, w, width, off, v);
        if (off != 0x44) { if (w) F.unmapped++; return 0; }
        if (!w) return F.cpu_control;
        u32 old = F.cpu_control;
        F.cpu_control = (u32)v;
        if (!(old & 0x10) && (v & 0x10)) {
            F.cpu_starts++;
            if (F.fw_state == 0) F.boot_status = 0;
        } else if ((old & 0x10) && !(v & 0x10)) {
            F.cpu_stops++;
            F.stopped_after_sleep_ack = F.iop_power == PWR_SLEEP;
            if (!F.stopped_after_sleep_ack) F.stop_abrupt = true;
            if (F.cc & 1) F.stop_while_cc_en = true;
            F.fw_state = 0;
        }
        return 0;
    }
    if (addr >= F_MBOX && addr < F_MBOX + F_MBOX_SIZE) {
        u32 off = (u32)(addr - F_MBOX);
        u64 r = mbox_access(off, w, v, width);
        trace(B_MBOX, w, width, off, w ? v : r);
        return r;
    }
    if (addr >= F_SART && addr < F_SART + F_SART_SIZE) {
        u32 off = (u32)(addr - F_SART);
        u32 r = sart_access(off, w, (u32)v);
        trace(B_SART, w, width, off, w ? v : r);
        return r;
    }
    bool nv = addr >= F_NVME && addr < F_NVME + F_NVME_SIZE;
    bool mirror = F.cfg.nvmmu_mirror_on_reg9 && addr >= F_NVME + F_NVME_SIZE && addr < F_NVME + F_NVMMU_SIZE;
    bool mm = addr >= F_NVMMU && addr < F_NVMMU + F_NVMMU_SIZE;
    if (nv || mirror || mm) {
        u32 off = (u32)(addr - (nv || mirror ? F_NVME : F_NVMMU));
        u8 bank = nv ? B_NVME : B_NVMMU;
        if (width == 8) {
            trace(bank, w, 8, off, v);
            u64 lo, hi;
            if (nv) { lo = nvme_access(off, w, (u32)v); hi = nvme_access(off + 4, w, (u32)(v >> 32)); }
            else { lo = nvmmu_access(off, w, (u32)v); hi = nvmmu_access(off + 4, w, (u32)(v >> 32)); }
            return lo | (hi << 32);
        }
        u32 r = nv ? nvme_access(off, w, (u32)v) : nvmmu_access(off, w, (u32)v);
        trace(bank, w, width, off, w ? v : r);
        return r;
    }
    F.unmapped++;
    trace(B_NONE, w, width, (u32)addr, v);
    return 0;
}

u32 fake_read32(u64 a) { return (u32)access(a, false, 0, 4); }
void fake_write32(u64 a, u32 v) { access(a, true, v, 4); }
u64 fake_read64(u64 a) { return access(a, false, 0, 8); }
void fake_write64(u64 a, u64 v) { access(a, true, v, 8); }
void fake_udelay(u32 us) { F.now += us; }
void (*r46_barrier_hook)(void);
u64 r46_clock_step;
unsigned r46_flip_read;
void fake_barrier(int kind) {
    if (kind == 3) {
        F.dsb_syncs++;
        if (r46_barrier_hook) {
            void (*fn)(void) = r46_barrier_hook;
            r46_barrier_hook = NULL;
            fn();
        }
    }
}
/* The ANS/ARM generic timer runs regardless of MMIO; advance it on every read so a bounded wait
 * terminates even when the mailbox is dead (e.g. after a firmware crash). */
u64 fake_now(void) { F.now += r46_clock_step ? r46_clock_step : 1; return F.now; }

int fake_run_bounded(void (*fn)(void))
{
    hang_armed = true;
    if (setjmp(hang_env)) {
        hang_armed = false;
        return 1;
    }
    fn();
    hang_armed = false;
    return 0;
}

void fake_reset(const struct fake_cfg *cfg)
{
    u8 *arena = F.arena;
    memset(&F, 0, sizeof(F));
    memset(bufaddr, 0, sizeof(bufaddr));
    memset(bufsize, 0, sizeof(bufsize));
    io_sq_base = 0;
    F.cfg = *cfg;
    ov_clear(ov_cache);
    ov_clear(ov_media);
    if (!F.cfg.adt_reg0) F.cfg.adt_reg0 = F_ASC_CPU;
    if (!F.cfg.adt_reg9) F.cfg.adt_reg9 = F_NVME;
    if (!F.cfg.adt_sart) F.cfg.adt_sart = F_SART;
    if (!F.cfg.sart_version) F.cfg.sart_version = 4;
    F.step_cap = 20000000;
    if (!arena) {
        void *p = mmap(NULL, ARENA_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED) {
            fprintf(stderr, "FAIL: arena mmap failed\n");
            exit(2);
        }
        arena = p;
    }
    F.arena = arena;
    ARENA = (u64)arena;
    memset(F.arena, 0, ARENA_SIZE);
    for (int i = 0; i < cfg->protected_entries && i < 16; i++) {
        F.s_cfg[i] = 0xff;
        F.s_paddr[i] = (u32)((0x800000000ull + (u64)i * 0x100000) >> 12);
        F.s_size[i] = 0x100;
    }
    memcpy(F.s_init_cfg, F.s_cfg, sizeof(F.s_cfg));
    memcpy(F.s_init_paddr, F.s_paddr, sizeof(F.s_paddr));
    memcpy(F.s_init_size, F.s_size, sizeof(F.s_size));
    F.cpu_control = cfg->cpu_running_at_entry ? 0x10 : 0;
    F.fw_state = cfg->cpu_running_at_entry ? 4 : 0;
    F.boot_status = BOOT_OK; /* stale value left by the previous session */
    /* r4.4 (= r3.5): CC as the stopped stage-1 handoff leaves it (probe: 0x474000). Was 0. */
    F.cc = cfg->cpu_running_at_entry ? 0x470001 : 0x474000;
}

void fake_spontaneous_syslog(unsigned n)
{
    for (unsigned i = 0; i < n; i++) fw_syslog();
}

/* r4: shim for m1n1 memory.h; records the last clean range for the write-coherency check. */
void dc_civac_range(void *addr, size_t length)
{
    F.cache_ops++;
    if (length == 64) F.clean_sqe++;
    if (length == 128) F.clean_tcb++;
    if (length == 8192) F.clean_init_tcb++;
    if (length == 1024) F.clean_init_cqe++;
    if (length == 4096) F.clean_init_sqe++;
    /* Queue cleans must not erase the most recent data clean evidence. */
    if (length >= 4096 && !fake_overlaps_queue((u64)addr, length)) {
        F.clean_lo = (u64)addr; F.clean_hi = (u64)addr + length; F.clean_seq = F.steps;
    }
}
void dc_cvac_range(void *addr, size_t length) { dc_civac_range(addr, length); }
void dc_ivac_range(void *addr, size_t length) { F.cache_ops++; if (length == 16) F.invalidate_cqe++; (void)addr; }
