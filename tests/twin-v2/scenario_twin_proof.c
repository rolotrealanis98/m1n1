/* SPDX-License-Identifier: MIT */
/* write-safety-20261110 twin v2: the confinement-proof workload. Appended to a COPY of the resident's
 * tests/native/body.c (it reuses rw_up / guest_write / io_cmd / submit / gen_block). The fake ANS is the
 * v2 fake (patch_fake_v2.py): it accepts and stores every write wherever it points, logs every command,
 * and its durable media is the sparse backing file $S1TWIN_DISK. This workload never CHECK-exits on a
 * write outcome: it records what happened and lets the external audit decide.
 *
 *  phase A  legitimate in-window guest writes: first block, last block, 1 MiB at both ends, 2/3-block
 *           shapes at the end, a mid-window block, 64 pseudo-random writes (1..256 blocks); FLUSH;
 *           guest read-back of every write.
 *  phase B  adversarial guest commands a correct resident must refuse: one past the end, straddling the
 *           end (n = 2, 256), far past the end, SLBA near 2^64 (wrap to just below the window and to
 *           LBA 0), 64-bit SLBA truncation probe, NLB field 256, WRITE ZEROES / DSM / WRITE UNCORRECTABLE.
 *  phase C  frontend bypass: backend calls (s1n_write, ans_ro_xfer) below / straddling / past the
 *           window, at LBA 0 and at the last LBA of the disk.
 *  end      FLUSH, guest CC.SHN normal, hv retire (stage-1 quiesce), then the fake's volatile cache is
 *           drained to the file so even an unflushed stray write lands in the image. */
extern int fake_disk_open(const char *path, bool writable);
extern void tw_drain_cache_to_file(void);
extern unsigned long long tw_file_errors, tw_outside_blocks, tw_beyond_capacity, tw_logged;
static unsigned tp_adv_accepted, tp_adv_total, tp_legit_failed, tp_readback_bad, tp_bypass_accepted;
static u64 tp_rng = 0x9e3779b97f4a7c15ull;
static u64 tp_rnd(void) { tp_rng ^= tp_rng << 13; tp_rng ^= tp_rng >> 7; tp_rng ^= tp_rng << 17; return tp_rng; }
static void tp_legit(u64 lba, u32 n, u32 gen)
{
    u16 sc = guest_write(lba, n, gen, 0);
    if (sc) { tp_legit_failed++; fprintf(stderr, "legit write %llu+%u -> %u\n", (unsigned long long)lba, n, sc); }
}
static void tp_check(u64 lba, u32 n, u32 gen)
{
    if (io_cmd(VNVME_IO_READ, lba, n, RDATA, 0)) { tp_readback_bad++; return; }
    u8 b[4096];
    for (u32 i = 0; i < n; i++) { gen_block(lba + i, gen, b); if (memcmp(G(RDATA) + 4096ULL * i, b, 4096)) { tp_readback_bad++; return; } }
}
static void tp_adv(const char *what, u64 lba, u32 n, u32 extra12)
{
    tp_adv_total++;
    u16 sc = guest_write(lba, n, 0xadadu, extra12);
    if (!sc) { tp_adv_accepted++; fprintf(stderr, "ADVERSARIAL ACCEPTED: %s lba=%llu n=%u\n", what, (unsigned long long)lba, n); }
}
static void tp_adv_raw(const char *what, u8 op, u64 lba, u32 nlb0, u32 cdw11x)
{
    tp_adv_total++;
    struct vnvme_command c = {.opcode = op, .nsid = 1, .prp1 = WDATA, .cdw10 = (u32)lba, .cdw11 = (u32)(lba >> 32) | cdw11x, .cdw12 = nlb0};
    if (op == 0x09) { /* DSM: one range {attr 0, nlb, slba} at WDATA, deallocate */
        u8 *r = G(WDATA); memset(r, 0, 16); u32 nn = nlb0 + 1; memcpy(r + 4, &nn, 4); memcpy(r + 8, &lba, 8);
        c.cdw10 = 0; c.cdw11 = 4;
    }
    u16 sc = submit(1, c);
    if (!sc) { tp_adv_accepted++; fprintf(stderr, "ADVERSARIAL ACCEPTED: %s\n", what); }
}
static void tp_bypass(const char *what, bool ok)
{
    tp_adv_total++;
    if (ok) { tp_bypass_accepted++; fprintf(stderr, "BYPASS ACCEPTED: %s\n", what); }
}
static void s_twin_stage(void)
{
    const char *disk = getenv("S1TWIN_DISK");
    CHECK(disk && fake_disk_open(disk, true) >= 0);
    fresh(rw_cfg());
    CHECK(select_native_w(rw_window(0)) == 0);
    int which = getenv("S1TWIN_CASE") ? atoi(getenv("S1TWIN_CASE")) : -1;
    bool ok;
    if (which == -1) {
#if S1N_STAGE == 0
        ok = ans_ro_flush();
#else
        ok = ans_ro_run_canary();
#endif
    } else {
#if S1N_STAGE_CANARY
        ok = ans_ro_test_stage_write(which == 0 ? S1N_SUB_START - 1 : S1N_SUB_START + S1N_SUB_BLOCKS, 1);
#else
        ok = ans_ro_test_stage_write(S1N_ZONE_START, 1);
#endif
    }
    tw_drain_cache_to_file();
    printf("{\"stage\":%u,\"case\":%d,\"operation_ok\":%d,\"writes\":%u,\"blocks\":%llu,\"outside\":%u,\"file_errors\":%llu}\n",
           S1N_STAGE, which, ok, F.io_writes, (unsigned long long)F.write_blocks, F.writes_outside_window, tw_file_errors);
}
static void s_twin_proof(void)
{
#if S1N_STAGE != 3
    s_twin_stage();
    return;
#endif
    const char *disk = getenv("S1TWIN_DISK"), *mx = getenv("S1N_MAXXFER");
    CHECK(disk && fake_disk_open(disk, true) >= 0);
    u32 maxxfer = mx ? (u32)atoi(mx) : 256;
    rw_up(rw_cfg(), maxxfer);
    int only = getenv("S1TWIN_CASE") ? atoi(getenv("S1TWIN_CASE")) : -1;
    if (only == -1) {
    /* phase A */
    tp_legit(0, 1, 1); tp_legit(W_BLOCKS - 1, 1, 1);
    tp_legit(256, 256, 2); tp_legit(W_BLOCKS - 256 - 8, 256, 2);
    tp_legit(W_BLOCKS - 3, 2, 3); tp_legit(W_BLOCKS - 6, 3, 3);
    tp_legit(12212227, 1, 4);
    static struct { u64 l; u32 n; } rw[64];
    for (unsigned i = 0; i < 64; i++) {
        rw[i].n = 1 + (u32)(tp_rnd() % 256);
        rw[i].l = 1024 + (tp_rnd() % (W_BLOCKS / 64 - 2048)) + (u64)i * (W_BLOCKS / 64);
        tp_legit(rw[i].l, rw[i].n, 10 + i);
    }
    if (io_cmd(VNVME_IO_FLUSH, 0, 1, 0, 0)) tp_legit_failed++;
    tp_check(0, 1, 1); tp_check(W_BLOCKS - 1, 1, 1); tp_check(256, 256, 2); tp_check(W_BLOCKS - 264, 256, 2);
    tp_check(W_BLOCKS - 3, 2, 3); tp_check(W_BLOCKS - 6, 3, 3); tp_check(12212227, 1, 4);
    for (unsigned i = 0; i < 64; i++) tp_check(rw[i].l, rw[i].n, 10 + i);
    }
    /* phase B */
    if (only == 0) tp_adv("one past the end", W_BLOCKS, 1, 0);
    if (only == 1) tp_adv("straddle end n=2", W_BLOCKS - 1, 2, 0);
    if (only == 2) tp_adv("straddle end n=256", W_BLOCKS - 255, 256, 0);
    if (only == 3) tp_adv("far past the end", W_BLOCKS + 100000, 8, 0);
    if (only == 4) tp_adv("SLBA 2^64-1 n=1", UINT64_MAX, 1, 0);
    if (only == 5) tp_adv("SLBA 2^64-1 n=2 (wraps to W_START-1)", UINT64_MAX, 2, 0);
    if (only == 6) tp_adv("SLBA 2^64-200 n=256 (wraps below window)", UINT64_MAX - 199, 256, 0);
    if (only == 7) tp_adv("SLBA 2^64-W_START (translates to LBA 0)", (u64)0 - W_START, 1, 0);
    if (only == 8) tp_adv("64-bit SLBA truncation probe", (1ull << 32) | 5, 1, 0);
    if (only == 9) tp_adv("NLB field 256 at end-256", W_BLOCKS - 256, 1, 256);
    if (only == 10) tp_adv_raw("WRITE ZEROES straddling end", 0x08, W_BLOCKS - 1, 3, 0);
    if (only == 11) tp_adv_raw("WRITE ZEROES in window", 0x08, 100, 0, 0);
    if (only == 12) tp_adv_raw("WRITE UNCORRECTABLE at end", 0x04, W_BLOCKS - 1, 1, 0);
    if (only == 13) tp_adv_raw("DSM deallocate straddling end", 0x09, W_BLOCKS - 2, 3, 0);
    /* phase C */
    static u8 buf[256 * 4096];
    memset(buf, 0xbb, sizeof buf);
    if (only == 14) tp_bypass("s1n_write straddle end", s1n_write(W_BLOCKS - 1, 2, buf));
    if (only == 15) tp_bypass("s1n_write past end", s1n_write(W_BLOCKS, 1, buf));
    if (only == 16) tp_bypass("s1n_write 2^64-1 n=2", s1n_write(UINT64_MAX, 2, buf));
    if (only == 17) tp_bypass("s1n_write 2^64-200 n=256", s1n_write(UINT64_MAX - 199, 256, buf));
    if (only == 18) tp_bypass("ans_ro_xfer W_START-1", ans_ro_xfer(true, W_START - 1, 1, 0));
    if (only == 19) tp_bypass("ans_ro_xfer straddle end", ans_ro_xfer(true, W_START + W_BLOCKS - 1, 2, 0));
    if (only == 20) tp_bypass("ans_ro_xfer W_END", ans_ro_xfer(true, W_START + W_BLOCKS, 1, 0));
    if (only == 21) tp_bypass("ans_ro_xfer LBA 0", ans_ro_xfer(true, 0, 1, 0));
    if (only == 22) tp_bypass("ans_ro_xfer last disk LBA", ans_ro_xfer(true, F_NSZE - 1, 1, 0));
    /* end */
    io_cmd(VNVME_IO_FLUSH, 0, 1, 0, 0);
    wr(0x14, S1_CC_VALID | 0x4000, 2);               /* guest normal shutdown */
    hv_win_storage_retire();                         /* hv exit: stage-1 quiesce */
    tw_drain_cache_to_file();
    const struct s1_wcanary *w = &controller.wcanary;
    printf("{\"proof\":\"twin_v2\",\"maxxfer\":%u,\"fake_writes_outside_window\":%u,\"fake_outside_blocks\":%llu,"
           "\"fake_io_writes\":%u,\"fake_write_blocks\":%llu,\"fake_wmin\":%llu,\"fake_wmax\":%llu,\"fake_beyond_capacity\":%llu,"
           "\"fake_forbidden_ops\":%u,\"fake_mutating_ops\":%u,\"fake_dma_violations\":%u,\"file_errors\":%llu,\"logged\":%llu,"
           "\"s1wc_abs_min\":%llu,\"s1wc_abs_max\":%llu,\"s1wc_window_refusals\":%llu,\"s1wc_write_cmds\":%llu,"
           "\"legit_failed\":%u,\"readback_bad\":%u,\"adversarial_total\":%u,\"adversarial_accepted\":%u,\"bypass_accepted\":%u,"
           "\"ans_state\":%d}\n",
           maxxfer, F.writes_outside_window, tw_outside_blocks, F.io_writes, (unsigned long long)F.write_blocks,
           (unsigned long long)F.wmin, (unsigned long long)F.wmax, tw_beyond_capacity, F.forbidden_ops, F.mutating_ops,
           F.dma_violations, tw_file_errors, tw_logged,
           (unsigned long long)w->abs_min_first, (unsigned long long)w->abs_max_last, (unsigned long long)w->window_refusals,
           (unsigned long long)w->write_cmds, tp_legit_failed, tp_readback_bad, tp_adv_total, tp_adv_accepted,
           tp_bypass_accepted, (int)ans_ro_state());
}

/* Image maker: F_NSZE x 4096 sparse file; LBA 0 protective MBR, LBA 1..5 the fake's own GPT, backup GPT
 * at NSZE-5..NSZE-1 (written by twin_proof.py), and LBA-stamped (fake base pattern: LBA in bytes 0..7)
 * regions: 64 MiB at p1 start, 64 MiB below the window, 64 MiB above it (gap block + p3 head, which
 * also pre-allocates the 16 KiB chunk the last window block shares with W_END), 64 MiB below the
 * backup GPT. Everything else outside the window is a hole: any write there shows up as an allocation
 * change even if it writes zeros. */
void fake_disk_block_synth(u64 lba, u8 *out);
extern int fake_disk_create(const char *path, unsigned long long bytes);
extern bool fake_file_write_block(u64 lba, const u8 *in);
extern bool fake_disk_sync(void);
static void s_twin_mkimg(void)
{
    const char *disk = getenv("S1TWIN_DISK");
    CHECK(disk);
    CHECK(fake_disk_create(disk, F_NSZE * 4096) >= 0);
    struct fake_cfg g = base_cfg(); fake_reset(&g);
    u8 b[4096];
    memset(b, 0, sizeof b);
    b[446 + 4] = 0xee; b[446 + 2] = 2; b[446 + 8] = 1;
    u32 n = 0xffffffffu; memcpy(b + 446 + 12, &n, 4); b[510] = 0x55; b[511] = 0xaa;
    CHECK(fake_file_write_block(0, b));
    for (u64 l = 1; l < 6; l++) { fake_disk_block_synth(l, b); CHECK(fake_file_write_block(l, b)); }
    const u64 R = 16384, W_END = W_START + W_BLOCKS;
    const u64 rs[4][2] = {{6, 6 + R}, {W_START - R, W_START}, {W_END, W_END + R}, {F_NSZE - 5 - R, F_NSZE - 5}};
    for (unsigned k = 0; k < 4; k++)
        for (u64 l = rs[k][0]; l < rs[k][1]; l++) { fake_disk_block_synth(l, b); CHECK(fake_file_write_block(l, b)); }
    CHECK(fake_disk_sync());
    printf("{\"image\":\"%s\",\"nsze\":%llu,\"w_start\":%llu,\"w_blocks\":%llu}\n", disk, (unsigned long long)F_NSZE,
           (unsigned long long)W_START, (unsigned long long)W_BLOCKS);
}
