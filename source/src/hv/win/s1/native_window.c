/* SPDX-License-Identifier: MIT */
/* r3 native read-only window over ans_ro. See native_window.h. */
#include "native_window.h"
#include "ans_ro.h"
#include "string.h"

#define SECTOR 4096u
#define GPT_MAX_ENTRY_BYTES (128u * 128u) /* 128 entries x 128 bytes = 4 sectors */

static const u8 basic_data_type[16] = { /* EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, on-disk order */
    0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44, 0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7};

static const u8 pinned_guid[16] = S1N_PIN_GUID_BYTES;
static const u8 pinned_disk[16] = S1N_PIN_DISK_BYTES;
static u32 selected_header_crc, selected_entries_crc, selected_backup_crc;
static bool have_snapshot;
static struct s1n_status st;
static struct s1n_window win;
static bool opened, verified, rw;
static u32 max_xfer;
static u8 scratch[GPT_MAX_ENTRY_BYTES] __attribute__((aligned(16)));
static u8 header[SECTOR] __attribute__((aligned(16)));
static u8 tmp[SECTOR];

u32 s1n_crc32(const void *data, u64 len)
{
    const u8 *p = data;
    u32 c = 0xffffffffu;
    for (u64 i = 0; i < len; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}

static u32 rd32(const u8 *b, unsigned o) { u32 v; memcpy(&v, b + o, 4); return v; }
static u64 rd64(const u8 *b, unsigned o) { u64 v; memcpy(&v, b + o, 8); return v; }

/* r3.1 R2: no GPT bytes survive verification in any buffer (success or failure). */
static void scrub(void)
{
    memset(header, 0, sizeof(header));
    memset(scratch, 0, sizeof(scratch));
    memset(tmp, 0, sizeof(tmp));
    if (ans_ro_state() != ANS_RO_FROZEN) ans_ro_scrub();
}

static bool fail(enum s1n_reason r)
{
    scrub();
    if (!st.reason)
        st.reason = r;
    verified = false;
    st.verified = 0;
    if (ans_ro_state() != ANS_RO_REFUSED) ans_ro_abort(ANS_RO_E_WINDOW);
    return false;
}

static bool zero16(const u8 *g)
{
    for (int i = 0; i < 16; i++)
        if (g[i])
            return false;
    return true;
}

static bool verify_gpt(const struct s1n_window *w)
{
    u64 nsze = ans_ro_nsze();
    /* Primary GPT header (LBA 1). */
    if (!ans_ro_read_gpt(1, header))
        return fail(S1N_E_READ);
    if (memcmp(header, "EFI PART", 8) || rd32(header, 8) != 0x00010000)
        return fail(S1N_E_HDR_SIG);
    if (memcmp(header + 56, pinned_disk, 16)) return fail(S1N_E_PIN);
    u32 header_digest = s1n_crc32(header, SECTOR);
    u32 hsize = rd32(header, 12);
    if (hsize < 92 || hsize > SECTOR)
        return fail(S1N_E_HDR_FIELDS);
    u32 hcrc = rd32(header, 16);
    memcpy(tmp, header, hsize);
    memset(tmp + 16, 0, 4);
    if (s1n_crc32(tmp, hsize) != hcrc)
        return fail(S1N_E_HDR_CRC);
    u64 my_lba = rd64(header, 24), alt_lba = rd64(header, 32);
    u64 first_usable = rd64(header, 40), last_usable = rd64(header, 48);
    u64 entries_lba = rd64(header, 72);
    u32 nentries = rd32(header, 80), esize = rd32(header, 84), ecrc = rd32(header, 88);
    if (alt_lba != nsze - 1)       /* r4.1 (= r3.2): DMA-verified capacity cross-check */
        return fail(S1N_E_CAPACITY);
    if (my_lba != 1 || esize != 128 || !nentries || nentries > 128 || entries_lba != 2 ||
        entries_lba >= first_usable || first_usable > last_usable || last_usable >= nsze ||
        w->entry_index > nentries)
        return fail(S1N_E_HDR_FIELDS);
    u32 ebytes = nentries * esize;
    u32 esectors = (ebytes + SECTOR - 1) / SECTOR;
    for (u32 i = 0; i < esectors; i++)
        if (!ans_ro_read_gpt(entries_lba + i, scratch + (u64)i * SECTOR))
            return fail(S1N_E_READ);
    if (s1n_crc32(scratch, ebytes) != ecrc)
        return fail(S1N_E_ENTRIES_CRC);

    if (entries_lba + esectors > first_usable || last_usable >= nsze - 5)
        return fail(S1N_E_HDR_FIELDS);
    const u8 *e = scratch + (u64)(w->entry_index - 1) * esize;
    if (memcmp(e, w->type_guid, 16))
        return fail(S1N_E_ENTRY_TYPE);
    if (memcmp(e + 16, w->unique_guid, 16))
        return fail(S1N_E_ENTRY_GUID);
    u64 first = rd64(e, 32), last = rd64(e, 40);
    if (first != w->start_lba || last < first || last - first + 1 != w->blocks)
        return fail(S1N_E_ENTRY_RANGE);
    if (first < first_usable || last > last_usable)
        return fail(S1N_E_USABLE);
    /* No other used entry may overlap the window. */
    for (u32 i = 0; i < nentries; i++) {
        if (i == w->entry_index - 1)
            continue;
        const u8 *o = scratch + (u64)i * esize;
        if (zero16(o))
            continue;
        u64 of = rd64(o, 32), ol = rd64(o, 40);
        if (of <= last && ol >= first)
            return fail(S1N_E_OVERLAP);
    }
    /* Backup header and array: exact mirror, checked independently of the primary CRC. */
    u64 primary_first = first_usable, primary_last = last_usable;
    if (!ans_ro_read_gpt(nsze - 1, header)) return fail(S1N_E_READ);
    u32 backup_digest = s1n_crc32(header, SECTOR);
    u32 bsize = rd32(header, 12), bcrc = rd32(header, 16);
    if (memcmp(header, "EFI PART", 8) || rd32(header, 8) != 0x10000 ||
        bsize != hsize || rd64(header, 24) != nsze - 1 || rd64(header, 32) != 1 ||
        rd64(header, 40) != primary_first || rd64(header, 48) != primary_last ||
        memcmp(header + 56, pinned_disk, 16) || rd64(header, 72) != nsze - 5 ||
        rd32(header, 80) != nentries || rd32(header, 84) != esize || rd32(header, 88) != ecrc)
        return fail(S1N_E_BACKUP);
    memcpy(tmp, header, bsize);
    memset(tmp + 16, 0, 4);
    if (s1n_crc32(tmp, bsize) != bcrc) return fail(S1N_E_BACKUP);
    for (u32 i = 0; i < esectors; i++) {
        if (!ans_ro_read_gpt(nsze - 5 + i, tmp)) return fail(S1N_E_READ);
        u32 bytes = ebytes - i * SECTOR < SECTOR ? ebytes - i * SECTOR : SECTOR;
        if (memcmp(tmp, scratch + i * SECTOR, bytes)) return fail(S1N_E_BACKUP);
    }
    if (have_snapshot && (header_digest != selected_header_crc || ecrc != selected_entries_crc ||
                          backup_digest != selected_backup_crc)) return fail(S1N_E_PIN);
    selected_header_crc = header_digest;
    selected_entries_crc = ecrc;
    selected_backup_crc = backup_digest;
    have_snapshot = true;
    scrub();
    return true;
}

bool s1n_recheck_before_write(void)
{
    if (!verified || !have_snapshot) return false;
    return verify_gpt(&win);
}

bool s1n_open(const struct s1n_window *w)
{
    if (opened)
        return fail(S1N_E_STATE);
    opened = true;
    if (!w || (w->flags & ~(S1N_RW | S1N_MAXXFER_MASK)) || w->entry_index < 1 || w->entry_index > 128 || !w->blocks ||
        w->start_lba < 34 || w->start_lba > UINT64_MAX - w->blocks ||
        memcmp(w->type_guid, basic_data_type, 16) || zero16(w->unique_guid))
        return fail(S1N_E_ARGS);
    if (w->entry_index != S1N_PIN_ENTRY || w->start_lba != S1N_PIN_START ||
        w->blocks != S1N_PIN_BLOCKS || memcmp(w->unique_guid, pinned_guid, 16))
        return fail(S1N_E_PIN);
    win = *w;
    st.start_lba = w->start_lba;
    st.blocks = w->blocks;
    bool want_rw = w->flags & S1N_RW;
    u32 mx = (w->flags & S1N_MAXXFER_MASK) >> S1N_MAXXFER_SHIFT;
    if (!mx) mx = want_rw ? 256 : 1;     /* ro default = r3.1 behaviour: 4 KiB ANS commands */
#if S1N_STAGE == 0 || S1N_STAGE == 1
    mx = 1; /* compile-time stage limit cannot be raised by contract flags */
#elif S1N_STAGE == 2
    /* Exact canary totals require fixed 128-block chunks. */
    if (want_rw && mx < S1N_CANARY_CHUNK) return fail(S1N_E_ARGS);
#endif
    if (mx > 256)
        return fail(S1N_E_ARGS);

    if (!ans_ro_init())
        return fail(S1N_E_ANS);
    u64 nsze = ans_ro_nsze();
    if (ans_ro_lbads() != 12 || w->start_lba + w->blocks > nsze)
        return fail(S1N_E_GEOMETRY);

    if (!verify_gpt(w)) return false;
    /* r4: confine the driver to the verified window; only now, and only with the explicit
     * contract flag, may WRITE/FLUSH ever be issued. */
    if (!ans_ro_set_window(want_rw, mx))
        return fail(S1N_E_WINDOW_SET);
    rw = want_rw;
    max_xfer = mx;
    st.rw = rw;
    st.max_xfer = mx;
    verified = true;
    st.verified = 1;
    scrub();
    return true;
}

/* Window range check shared by read and write (overflow-safe). */
static bool range_ok(u64 lba, u32 blocks)
{
    return verified && blocks && blocks <= 256 && lba < S1N_PIN_BLOCKS && blocks <= S1N_PIN_BLOCKS - lba;
}

/* One guest command = ceil(blocks / max_xfer) ANS commands, data contiguous in the private
 * transfer buffer (single copy when the frontend passes that buffer). */
static bool transfer(bool write, u64 lba, u32 blocks)
{
    for (u32 done = 0; done < blocks;) {
        u32 n = blocks - done < max_xfer ? blocks - done : max_xfer;
        u64 phys = S1N_PIN_START + lba + done;
        /* Re-check the translated range stays inside the verified partition. */
        if (phys < S1N_PIN_START || phys - S1N_PIN_START >= S1N_PIN_BLOCKS ||
            n > S1N_PIN_BLOCKS - (phys - S1N_PIN_START)) {
            ans_ro_abort(ANS_RO_E_ABORT);
            return false;
        }
        if (!ans_ro_xfer(write, phys, n, (u64)done * SECTOR))
            return false;
        done += n;
    }
    return true;
}

bool s1n_read(u64 lba, u32 blocks, void *out)
{
    if (!out || !range_ok(lba, blocks)) {
        st.refused_reads++;
        ans_ro_abort(ANS_RO_E_ABORT);
        return false;
    }
    if (!transfer(false, lba, blocks))
        return false;
    size_t cap;
    void *x = ans_ro_xfer_buffer(&cap);
    if (!x || cap < (u64)blocks * SECTOR) return false;
    if (out != x)
        memcpy(out, x, (u64)blocks * SECTOR);
    st.reads += blocks;
    return true;
}

bool s1n_write(u64 lba, u32 blocks, const void *data)
{
    if (ans_ro_state() != ANS_RO_LIVE) return false;
    if (!S1N_STAGE_GUEST_WRITES || !rw || !data || !range_ok(lba, blocks)) {
        st.refused_writes++;
        ans_ro_abort(ANS_RO_E_ABORT);
        return false;
    }
    size_t cap;
    void *x = ans_ro_xfer_buffer(&cap);
    if (!x || cap < (u64)blocks * SECTOR) return false;
    if (data != x)
        memcpy(x, data, (u64)blocks * SECTOR);
    if (!transfer(true, lba, blocks))
        return false;
    st.writes += blocks;
    return true;
}

bool s1n_flush(void)
{
    if (!rw || !verified)
        return false;
    if (!ans_ro_flush())
        return false;
    st.flushes++;
    return true;
}

bool s1n_writable(void) { return S1N_STAGE_GUEST_WRITES && verified && rw; }
void *s1n_xfer_buffer(size_t *capacity) { return ans_ro_xfer_buffer(capacity); }
void s1n_canary(u64 *a, u64 *b, u64 *c, u64 *d) { ans_ro_canary(a, b, c, d); }

u64 s1n_blocks(void) { return verified ? win.blocks : 0; }
bool s1n_verified(void) { return verified; }
void s1n_poll(void) { ans_ro_poll(); }
void s1n_close(void)
{
    verified = false;
    ans_ro_shutdown();
}
const struct s1n_status *s1n_status(void) { return &st; }
#ifdef ANS_RO_HOST_TEST
bool s1n_test_buffers_clear(void)
{
    for (unsigned i = 0; i < sizeof(scratch); i++) if (scratch[i]) return false;
    for (unsigned i = 0; i < SECTOR; i++) if (header[i] || tmp[i]) return false;
    return true;
}
#endif
