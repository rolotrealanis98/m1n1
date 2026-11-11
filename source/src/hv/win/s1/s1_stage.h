/* SPDX-License-Identifier: MIT */
/* r4.5: compile-time write stage and the pinned Windows window (INDEPENDENT-REVIEW r4.4 M3, M8, C8).
 *
 * One resident binary per write stage. S1N_STAGE comes from the build (make S1N_STAGE=n); there is no
 * default, no runtime field and no proxy operation that changes it, so neither the guest nor the host
 * link can raise it. A stage-N binary contains no code path that can issue a stage-N+1 write:
 *
 *   W0  S1N_STAGE 0  READ (GPT, guest) + FLUSH only. The WRITE opcode is compiled out of the driver
 *                    allowlist, the pre-doorbell check and the transfer path. FLUSH <= 4 per session.
 *   W1  S1N_STAGE 1  + WRITE, issued only by the resident's own canary procedure (no guest writes:
 *                    the guest gets the read-only backend). Zone C = p7 blocks 4096..4607
 *                    (absolute 174217216..174217727), one block per WRITE command, at most 1024
 *                    WRITE commands / 1024 blocks per session (512 pattern + 512 restore).
 *   W2  S1N_STAGE 2  + WRITE by the canary procedure only. Zone S = p7 blocks 4096..20479
 *                    (absolute 174217216..174233599), 128-block (512 KiB, PRP list) WRITE commands,
 *                    at most 256 WRITE commands / 32768 blocks per session.
 *   W3  S1N_STAGE 3  guest-driven writes on the whole pinned window (r4.4 behaviour), no caps.
 *
 * The window itself is pinned: only GPT entry 7 of the J716s internal SSD, exactly as measured
 * 2026-11-09 (pre-linux-ssd.txt), can ever be selected, and the last two window checks compare
 * against these constants, never against mutable memory. */
#ifndef S1_STAGE_H
#define S1_STAGE_H

#ifndef S1N_STAGE
#error "S1N_STAGE (0..3) must be defined by the build: one resident binary per write stage"
#endif
#if S1N_STAGE < 0 || S1N_STAGE > 3
#error "S1N_STAGE must be 0, 1, 2 or 3"
#endif

/* ---- M8: the pinned window (GPT entry 7, Microsoft basic data, "m5w-windows") ---- */
#define S1N_PIN_ENTRY  7u
#define S1N_PIN_START  174213120ULL
#define S1N_PIN_BLOCKS 24424454ULL
#define S1N_PIN_END    (S1N_PIN_START + S1N_PIN_BLOCKS) /* 198637574, first block after the window */
#define S1N_PIN_NSZE   244276265ULL                     /* whole disk, 4096-byte blocks */
/* Partition unique GUID 89AC7259-FAA4-4110-B621-E9A2934B45C1, on-disk (mixed-endian) order. */
#define S1N_PIN_GUID_BYTES {0x59, 0x72, 0xac, 0x89, 0xa4, 0xfa, 0x10, 0x41, \
                            0xb6, 0x21, 0xe9, 0xa2, 0x93, 0x4b, 0x45, 0xc1}
/* Disk GUID 6BC41126-DD32-4AE4-8526-C2A4E40657E7 (GPT header bytes 56..71), on-disk order. */
#define S1N_PIN_DISK_BYTES {0x26, 0x11, 0xc4, 0x6b, 0x32, 0xdd, 0xe4, 0x4a, \
                            0x85, 0x26, 0xc2, 0xa4, 0xe4, 0x06, 0x57, 0xe7}

/* ---- M3: per-stage write capability ---- */
#define S1N_ZONE_START (S1N_PIN_START + 4096ULL) /* p7 block 4096 = absolute 174217216 */
#if S1N_STAGE == 0
#define S1N_STAGE_WRITES    0             /* WRITE does not exist in this binary */
#define S1N_FLUSH_CAP       4ULL
#elif S1N_STAGE == 1
#define S1N_STAGE_WRITES    1
#define S1N_SUB_START       S1N_ZONE_START
#define S1N_SUB_BLOCKS      512ULL        /* zone C */
#define S1N_WRITE_MAX_BLOCKS 1u           /* blocks per WRITE command */
#define S1N_WRITE_CMD_CAP   1024ULL
#define S1N_WRITE_BLOCK_CAP 1024ULL
#define S1N_CANARY_CHUNK    128u
#define S1N_FLUSH_CAP       16ULL
#elif S1N_STAGE == 2
#define S1N_STAGE_WRITES    1
#define S1N_SUB_START       S1N_ZONE_START
#define S1N_SUB_BLOCKS      16384ULL      /* zone S */
#define S1N_WRITE_MAX_BLOCKS 256u
#define S1N_WRITE_CMD_CAP   256ULL
#define S1N_WRITE_BLOCK_CAP 32768ULL
#define S1N_CANARY_CHUNK    128u
#define S1N_FLUSH_CAP       257ULL
#else /* 3 */
#define S1N_STAGE_WRITES    1
#define S1N_SUB_START       S1N_PIN_START
#define S1N_SUB_BLOCKS      S1N_PIN_BLOCKS /* whole window */
#define S1N_WRITE_MAX_BLOCKS 256u
#define S1N_WRITE_CMD_CAP   (~0ULL)
#define S1N_WRITE_BLOCK_CAP (~0ULL)
#define S1N_FLUSH_CAP       (~0ULL)
#endif
/* Stages 1 and 2: the resident's canary procedure is the only writer; the guest is read-only. */
#define S1N_STAGE_CANARY (S1N_STAGE == 1 || S1N_STAGE == 2)
#define S1N_STAGE_GUEST_WRITES (S1N_STAGE == 3)

#define S1N_STR2(x) #x
#define S1N_STR(x) S1N_STR2(x)
/* Marker embedded once in every binary (the static gate checks the built image for exactly one). */
#define S1N_STAGE_MARKER "M5W-S1N-R4.5-STAGE-" S1N_STR(S1N_STAGE)
#endif
