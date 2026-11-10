/* SPDX-License-Identifier: MIT */
/* r3 native read-only window over ans_ro. See native_window.h. */
#include "native_window.h"
#include "ans_ro.h"
#include "string.h"

#define SECTOR 4096u
#define GPT_MAX_ENTRY_BYTES (128u * 128u) /* 128 entries x 128 bytes = 4 sectors */

static const u8 basic_data_type[16] = { /* EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, on-disk order */
    0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44, 0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7};

static struct s1n_status st;
static struct s1n_window win;
static bool opened, verified;
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
    ans_ro_scrub();
}

static bool fail(enum s1n_reason r)
{
    scrub();
    if (!st.reason)
        st.reason = r;
    verified = false;
    return false;
}

static bool zero16(const u8 *g)
{
    for (int i = 0; i < 16; i++)
        if (g[i])
            return false;
    return true;
}

bool s1n_open(const struct s1n_window *w)
{
    if (opened)
        return fail(S1N_E_STATE);
    opened = true;
    if (!w || w->flags || w->entry_index < 1 || w->entry_index > 128 || !w->blocks ||
        w->start_lba < 34 || w->start_lba > UINT64_MAX - w->blocks ||
        memcmp(w->type_guid, basic_data_type, 16) || zero16(w->unique_guid))
        return fail(S1N_E_ARGS);
    win = *w;
    st.start_lba = w->start_lba;
    st.blocks = w->blocks;

    if (!ans_ro_init())
        return fail(S1N_E_ANS);
    u64 nsze = ans_ro_nsze();
    if (ans_ro_lbads() != 12 || w->start_lba + w->blocks > nsze)
        return fail(S1N_E_GEOMETRY);

    /* Primary GPT header (LBA 1). */
    if (!ans_ro_read_block(1, header))
        return fail(S1N_E_READ);
    if (memcmp(header, "EFI PART", 8) || rd32(header, 8) != 0x00010000)
        return fail(S1N_E_HDR_SIG);
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
    /* r3.2: DMA-verified capacity cross-check. The primary header's AlternateLBA is the backup
     * header's location = the last addressable LBA = capacity-1. Its CRC is already validated, so
     * this ties the pinned capacity to bytes actually read from this disk. */
    if (alt_lba != nsze - 1)
        return fail(S1N_E_CAPACITY);
    if (my_lba != 1 || esize != 128 || !nentries || nentries > 128 || entries_lba < 2 ||
        entries_lba >= first_usable || first_usable > last_usable || last_usable >= nsze ||
        w->entry_index > nentries)
        return fail(S1N_E_HDR_FIELDS);
    u32 ebytes = nentries * esize;
    u32 esectors = (ebytes + SECTOR - 1) / SECTOR;
    for (u32 i = 0; i < esectors; i++)
        if (!ans_ro_read_block(entries_lba + i, scratch + (u64)i * SECTOR))
            return fail(S1N_E_READ);
    if (s1n_crc32(scratch, ebytes) != ecrc)
        return fail(S1N_E_ENTRIES_CRC);

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
    verified = true;
    st.verified = 1;
    scrub();
    return true;
}

bool s1n_read(u64 lba, u32 blocks, void *out)
{
    if (!verified || !out || !blocks || blocks > 32 || lba >= win.blocks ||
        blocks > win.blocks - lba) {
        st.refused_reads++;
        return false;
    }
    u8 *p = out;
    for (u32 i = 0; i < blocks; i++) {
        u64 phys = win.start_lba + lba + i;
        /* Re-check the translated address stays inside the verified partition. */
        if (phys < win.start_lba || phys - win.start_lba >= win.blocks) {
            st.refused_reads++;
            return false;
        }
        if (!ans_ro_read_block(phys, p + (u64)i * SECTOR))
            return false;
        st.reads++;
    }
    return true;
}

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
