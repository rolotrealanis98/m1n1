/* SPDX-License-Identifier: MIT */
/* r3: read-only window of the internal SSD (one GPT partition) exposed as a standalone disk.
 * Guest LBA 0 == the partition's first sector; nothing outside [start, start+blocks) is
 * addressable. Exposure requires the live primary GPT to describe exactly that partition. */
#ifndef S1_NATIVE_WINDOW_H
#define S1_NATIVE_WINDOW_H
#include "types.h"

/* Launcher-supplied window (on-disk GUID byte order, as stored in the GPT entry). */
struct s1n_window {
    u64 start_lba;        /* first physical LBA (4096-byte sectors) */
    u64 blocks;           /* partition length in sectors */
    u32 entry_index;      /* 1-based GPT entry number (7 for m5w-windows) */
    u32 flags;            /* must be 0 */
    u8 type_guid[16];
    u8 unique_guid[16];
};
_Static_assert(sizeof(struct s1n_window) == 56, "s1n window ABI");

enum s1n_reason {
    S1N_OK = 0,
    S1N_E_ARGS, S1N_E_ANS, S1N_E_GEOMETRY, S1N_E_READ, S1N_E_HDR_SIG, S1N_E_HDR_CRC, S1N_E_HDR_FIELDS,
    S1N_E_ENTRIES_CRC, S1N_E_ENTRY_TYPE, S1N_E_ENTRY_GUID, S1N_E_ENTRY_RANGE, S1N_E_OVERLAP,
    S1N_E_USABLE, S1N_E_STATE, S1N_E_CAPACITY,
};

struct s1n_status {
    u32 reason, verified;
    u64 start_lba, blocks, reads, refused_reads;
};

/* Bring up ANS and verify the window against the live GPT. One shot. On any failure nothing
 * is exposed (and ANS is frozen or refused as ans_ro decides). */
bool s1n_open(const struct s1n_window *w);
/* Window read for the S1 model backend: guest LBA, block count (<= 32), output buffer.
 * Rejects any range that is not entirely inside the window (including overflow). */
bool s1n_read(u64 lba, u32 blocks, void *out);
u64 s1n_blocks(void);
bool s1n_verified(void);
void s1n_poll(void);
void s1n_close(void);
const struct s1n_status *s1n_status(void);
u32 s1n_crc32(const void *data, u64 len);
#ifdef ANS_RO_HOST_TEST
bool s1n_test_buffers_clear(void);
#endif
#endif
