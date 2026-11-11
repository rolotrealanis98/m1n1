/* CHECK.md (independent r4.6 review): stage-3 request splitting, differential against a flat
 * reference array, with scattered 4 KiB guest pages (and PRP1 page offsets), mid-request ANS
 * write failures, and FUA flush ordering. Stage 3 only. */
#if S1N_STAGE == 3
extern unsigned chk_fail_write_n, chk_max_write_nlb;
#define CHK_R 2048u                       /* head [0, CHK_R) and tail [W_BLOCKS - CHK_R, W_BLOCKS) */
#define CHK_POOL0 (GUEST + 0x100000)      /* 768 guest pages, clear of queues/PRPL/IDBUF/DATA */
#define CHK_POOL_PAGES 768u
static u8 chk_ref[2][CHK_R][4096];
static u8 chk_lin[257 * 4096];
static u64 chk_x = 0x9d2c5680a1b2c3d4ull;
static u64 chk_rnd(void) { chk_x ^= chk_x << 13; chk_x ^= chk_x >> 7; chk_x ^= chk_x << 17; return chk_x; }
static u8 *chk_ref_blk(u64 lba)
{
    if (lba < CHK_R) return chk_ref[0][lba];
    if (lba >= W_BLOCKS - CHK_R && lba < W_BLOCKS) return chk_ref[1][lba - (W_BLOCKS - CHK_R)];
    return NULL;
}
/* n + 1 distinct random guest pages (PRP1 page + the rest). */
static void chk_pages(u32 n, u64 *pages)
{
    static u16 idx[CHK_POOL_PAGES];
    for (unsigned i = 0; i < CHK_POOL_PAGES; i++) idx[i] = (u16)i;
    for (u32 i = 0; i <= n; i++) {
        unsigned j = i + (unsigned)(chk_rnd() % (CHK_POOL_PAGES - i));
        u16 t = idx[i]; idx[i] = idx[j]; idx[j] = t;
        pages[i] = CHK_POOL0 + 4096ULL * idx[i];
    }
}
/* Byte k of the transfer lives at: */
static u8 *chk_at(const u64 *pages, u32 off, size_t k)
{
    size_t first = 4096 - off;
    if (k < first) return G(pages[0] + off + k);
    k -= first;
    return G(pages[1 + k / 4096] + k % 4096);
}
static u16 chk_io(u8 op, u64 lba, u32 n, const u64 *pages, u32 off, u32 extra)
{
    struct vnvme_command c = {.opcode = op, .nsid = 1, .prp1 = pages[0] + off,
                              .cdw10 = (u32)lba, .cdw11 = (u32)(lba >> 32), .cdw12 = (n - 1) | extra};
    size_t total = (size_t)n * 4096, first = 4096 - off;
    if (first > total) first = total;
    size_t count = (total - first + 4095) / 4096;
    if (count == 1) c.prp2 = pages[1];
    else if (count > 1) {
        for (size_t i = 0; i < count; i++) memcpy(G(PRPL) + 8 * i, &pages[1 + i], 8);
        c.prp2 = PRPL;
    }
    return submit(1, c);
}
static u32 chk_len(void)
{
    static const u32 edge[] = {1, 2, 3, 31, 32, 33, 63, 64, 65, 96, 127, 128, 129, 224, 255, 256};
    u64 r = chk_rnd() % 4;
    if (r == 0) return edge[chk_rnd() % (sizeof(edge) / sizeof(edge[0]))];
    if (r == 1) return 1 + (u32)(chk_rnd() % 256);
    return 1 + (u32)(chk_rnd() % 40);
}
static u64 chk_lba(u32 n)
{
    switch (chk_rnd() % 4) {
    case 0: return 0;                                              /* first block of the window */
    case 1: return W_BLOCKS - n;                                   /* ends on the last block */
    case 2: return chk_rnd() % (CHK_R - n + 1);
    default: return W_BLOCKS - CHK_R + chk_rnd() % (CHK_R - n + 1);
    }
}
static void chk_split_diff(void)
{
    rw_up(rw_cfg(), 0);
    for (u64 l = 0; l < CHK_R; l++) {
        fake_media_block(W_START + l, chk_ref[0][l]);
        fake_media_block(W_START + W_BLOCKS - CHK_R + l, chk_ref[1][l]);
    }
    static u64 pages[258];
    unsigned writes = 0, reads = 0, flushes = 0, multi = 0;
    for (unsigned op = 0; op < 1500; op++) {
        u32 n = chk_len();
        u64 lba = chk_lba(n);
        u32 off = (chk_rnd() % 3 == 0) ? 4 * (u32)(1 + chk_rnd() % 1023) : 0;
        chk_pages(n, pages);
        unsigned k = (unsigned)(chk_rnd() % 20);
        if (k < 10) {
            for (size_t b = 0; b < (size_t)n * 4096; b += 8) { u64 v = chk_rnd(); memcpy(chk_lin + b, &v, 8); }
            for (size_t b = 0; b < (size_t)n * 4096; b++) *chk_at(pages, off, b) = chk_lin[b];
            unsigned w0 = F.io_writes; u64 b0 = F.write_blocks;
            u32 fua = (chk_rnd() % 8 == 0) ? (1u << 30) : 0;
            CHECK(chk_io(VNVME_IO_WRITE, lba, n, pages, off, fua) == 0);
            CHECK(F.io_writes - w0 == (n + 31) / 32);       /* pieces of <= 32 blocks: exactly ceil(n/32) */
            CHECK(F.write_blocks - b0 == n);                 /* no gap, no overlap: blocks sum to n */
            CHECK(chk_max_write_nlb <= 32);
            for (u32 i = 0; i < n; i++) memcpy(chk_ref_blk(lba + i), chk_lin + (size_t)i * 4096, 4096);
            writes++; multi += n > 32;
        } else if (k < 17) {
            memset(chk_lin, 0xa5, (size_t)n * 4096);
            for (size_t b = 0; b < (size_t)n * 4096; b++) *chk_at(pages, off, b) = 0xa5;
            CHECK(chk_io(VNVME_IO_READ, lba, n, pages, off, 0) == 0);
            for (u32 i = 0; i < n; i++)
                for (size_t b = 0; b < 4096; b++)
                    if (*chk_at(pages, off, (size_t)i * 4096 + b) != chk_ref_blk(lba + i)[b]) {
                        fprintf(stderr, "FAIL: chk read lba %llu n %u blk %u byte %zu\n",
                                (unsigned long long)lba, n, i, b);
                        exit(1);
                    }
            checks++;
            CHECK(F.last_read_bytes <= 32 * 4096);
            reads++;
        } else {
            CHECK(io_cmd(VNVME_IO_FLUSH, 0, 1, 0, 0) == 0);
            flushes++;
        }
        if (op % 64 == 0) hv_win_storage_tick();
    }
    CHECK(io_cmd(VNVME_IO_FLUSH, 0, 1, 0, 0) == 0);
    CHECK(fake_cached_blocks() == 0);
    u8 got[4096];
    for (u64 l = 0; l < CHK_R; l++) {
        fake_media_block(W_START + l, got); CHECK(!memcmp(got, chk_ref[0][l], 4096));
        fake_media_block(W_START + W_BLOCKS - CHK_R + l, got); CHECK(!memcmp(got, chk_ref[1][l], 4096));
    }
    outside_untouched();
    CHECK(F.wmin >= W_START && F.wmax == W_START + W_BLOCKS - 1);  /* the last window block was written */
    CHECK(chk_max_write_nlb == 32 && multi > 0);
    CHECK(ans_ro_state() == ANS_RO_LIVE);
    printf("{\"chk_ops\":1500,\"writes\":%u,\"multi_piece_writes\":%u,\"reads\":%u,\"flushes\":%u,\"ans_writes\":%u}\n",
           writes, multi, reads, flushes, F.io_writes);
}
/* A 256-block guest WRITE whose `piece`-th ANS write (1..8) fails with a media error. */
static void chk_split_error(unsigned piece)
{
    rw_up(rw_cfg(), 0);
    CHECK(guest_write(10, 1, 1, 0) == 0);                          /* first write (GPT recheck) done */
    CHECK(io_cmd(VNVME_IO_FLUSH, 0, 1, 0, 0) == 0);
    CHECK(fake_cached_blocks() == 0);
    unsigned w0 = F.io_writes;
    chk_fail_write_n = w0 + piece;
    u16 s = guest_write(1000, 256, 7, 0);
    CHECK(s == VNVME_SC_INTERNAL);                                 /* Windows sees a failed WRITE */
    CHECK(F.io_writes - w0 == piece);                              /* nothing after the failed piece */
    CHECK(ans_ro_state() == ANS_RO_FROZEN && ans_ro_stats()->reason == ANS_RO_E_STATUS);
    CHECK(fake_cached_blocks() == 32 * (piece - 1));               /* earlier pieces sit in the device cache */
    expect_media(10, 1, 1);
    expect_media_base(1000, 256);                                  /* nothing of the failed command is durable */
    unsigned n = F.nio, a = F.nadmin, w = mmio_writes();
    CHECK(io_cmd(VNVME_IO_FLUSH, 0, 1, 0, 0) == VNVME_SC_INTERNAL);
    CHECK(io_cmd(VNVME_IO_READ, 1000, 1, RDATA, 0) == VNVME_SC_INTERNAL);
    CHECK(guest_write(2000, 1, 9, 0) == VNVME_SC_INTERNAL);
    CHECK(F.nio == n && F.nadmin == a && mmio_writes() == w);
    /* outside_untouched() minus its wcanary == fake-success equality: the resident records the
     * failed piece's submitted LBAs (conservative), the fake records successful writes only. */
    CHECK(F.writes_outside_window == 0 && fake_overlay_inside_window() && F.uncleaned_writes == 0);
    const struct s1_wcanary *wc = &controller.wcanary;
    CHECK(wc->abs_min_first == W_START + 10 && wc->abs_max_last == W_START + 1000 + 32 * piece - 1);
    u8 blk[4096];
    const u64 sent[] = {W_START - 1, W_START + W_BLOCKS, 6, 244276258};
    for (unsigned i = 0; i < 4; i++) { fake_disk_block(sent[i], blk); CHECK(fake_phys_of_pattern(blk) == sent[i]); }
    r45_frozen();
}
/* FUA on a split WRITE: all pieces complete, then exactly one FLUSH, then durable. */
static void chk_split_fua(void)
{
    rw_up(rw_cfg(), 0);
    CHECK(guest_write(10, 1, 1, 0) == 0);
    CHECK(io_cmd(VNVME_IO_FLUSH, 0, 1, 0, 0) == 0);
    unsigned n0 = F.nio, f0 = F.io_flushes;
    CHECK(n0 + 9 <= 64);
    CHECK(guest_write(500, 256, 3, 1u << 30) == 0);
    CHECK(F.nio - n0 == 9 && F.io_flushes - f0 == 1);
    for (unsigned i = 0; i < 8; i++) CHECK(F.io_op_seq[n0 + i] == 0x01);
    CHECK(F.io_op_seq[n0 + 8] == 0x00);
    CHECK(fake_cached_blocks() == 0);
    expect_media(500, 256, 3);
    CHECK(F.fua_writes == 0);
    outside_untouched();
}
#endif
static bool chk_dispatch(const char *s)
{
#if S1N_STAGE == 3
    if (!strcmp(s, "chk_split_diff")) chk_split_diff();
    else if (!strncmp(s, "chk_split_error_", 16)) chk_split_error((unsigned)atoi(s + 16));
    else if (!strcmp(s, "chk_split_fua")) chk_split_fua();
    else return false;
    return true;
#else
    (void)s;
    return false;
#endif
}
