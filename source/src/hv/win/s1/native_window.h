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
    u32 flags;            /* r4: bit 0 = read-write (S1N_RW); bits 8..16 = max blocks per ANS
                           * command, 0 = default (rw: 256, ro: 1); all other bits must be 0 */
    u8 type_guid[16];
    u8 unique_guid[16];
};
_Static_assert(sizeof(struct s1n_window) == 56, "s1n window ABI");

#define S1N_RW 1u
#define S1N_MAXXFER_SHIFT 8
#define S1N_MAXXFER_MASK (0x1ffu << S1N_MAXXFER_SHIFT)

enum s1n_reason {
    S1N_OK = 0,
    S1N_E_ARGS, S1N_E_ANS, S1N_E_GEOMETRY, S1N_E_READ, S1N_E_HDR_SIG, S1N_E_HDR_CRC, S1N_E_HDR_FIELDS,
    S1N_E_ENTRIES_CRC, S1N_E_ENTRY_TYPE, S1N_E_ENTRY_GUID, S1N_E_ENTRY_RANGE, S1N_E_OVERLAP,
    S1N_E_USABLE, S1N_E_STATE, S1N_E_WINDOW_SET, S1N_E_CAPACITY, S1N_E_PIN, S1N_E_BACKUP,
};

struct s1n_status {
    u32 reason, verified;
    u64 start_lba, blocks, reads, refused_reads;
    u64 writes, refused_writes, flushes;
    u32 rw, max_xfer;
};

/* Bring up ANS and verify the window against the live GPT. One shot. On any failure nothing
 * is exposed (and ANS is frozen or refused as ans_ro decides). */
bool s1n_open(const struct s1n_window *w);
/* Window read for the S1 model backend: guest LBA, block count (<= 32), output buffer.
 * Rejects any range that is not entirely inside the window (including overflow). */
bool s1n_read(u64 lba, u32 blocks, void *out);
/* r4 (rw mode only): write/flush with exactly the read path's bounds and translation. */
bool s1n_write(u64 lba, u32 blocks, const void *data);
bool s1n_flush(void);
bool s1n_writable(void);
bool s1n_recheck_before_write(void);
void *s1n_xfer_buffer(size_t *capacity);
void s1n_canary(u64 *abs_min_first, u64 *abs_max_last, u64 *ans_writes, u64 *refusals);
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
