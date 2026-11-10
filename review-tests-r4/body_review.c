/* Independent r4 write-safety review scenarios. Appended after the builder's prelude +
 * actual r4 sources + builder body.c (its main renamed). Reuses the builder's fake ANS,
 * whose fake disk rejects and counts any write outside [W_START, W_START+W_BLOCKS)
 * (F.writes_outside_window) and keeps a durable/volatile overlay only inside the window
 * (fake_overlay_inside_window). Independent oracle here: across heavy adversarial fuzzing,
 * NO physical write ever lands outside the window, the S1WC canary faithfully bounds the
 * absolute LBAs actually submitted to ANS, and no forbidden opcode ever writes. */

static u64 rvs = 0x12345678abcdef01ull;
static u64 rv(void) { rvs ^= rvs << 13; rvs ^= rvs >> 7; rvs ^= rvs << 17; return rvs; }

/* ans_ro's own record of the absolute LBAs it actually placed in ANS write SQEs (ground truth) */
static void canary(u64 *mn, u64 *mx, u64 *ans_w, u64 *refus) { ans_ro_canary(mn, mx, ans_w, refus); }

static void bringup_rw(u32 maxx)
{
    rw_up(rw_cfg(), maxx);   /* builder helper: rw window + queues + rw-identity checks */
}

/* every physical write ever addressed must be inside the window; canary must bound it */
static void assert_confined(void)
{
    CHECK(F.writes_outside_window == 0);
    CHECK(fake_overlay_inside_window());
    u64 mn, mx, aw, rf;
    canary(&mn, &mx, &aw, &rf);
    if (aw) { CHECK(mn >= W_START && mx < W_START + W_BLOCKS && mn <= mx); }
    /* the fake's own min/max absolute write LBA must agree with the canary */
    if (F.write_blocks) {
        CHECK(F.wmin >= W_START && F.wmax < W_START + W_BLOCKS);
        CHECK(mn == F.wmin && mx == F.wmax);     /* canary cannot be fooled */
    }
}

/* 1. heavy adversarial fuzz: random op/lba/len/flags, forbidden opcodes, odd NSIDs */
static void r_rw_fuzz(void)
{
    bringup_rw(256);
    unsigned ok_w = 0, ok_r = 0;
    for (unsigned it = 0; it < 6000; it++) {
        unsigned kind = rv() % 10;
        if (kind < 5) {                                  /* WRITE */
            u64 lba; u32 n = 1 + (u32)(rv() % 256);
            /* In-window writes are bounded to a small LBA span so the fake's finite overlay
             * does not overflow; out-of-window LBAs (rejected before any store) range freely. */
            switch (rv() % 8) {
            case 0: case 1: lba = rv() % 2048; break;                 /* in-window, bounded */
            case 2: lba = W_BLOCKS - 1 - rv() % 300; break;           /* near / over the end */
            case 3: lba = W_BLOCKS + rv() % 300; break;               /* past end */
            case 4: lba = rv(); break;                                /* anywhere in 64-bit */
            case 5: lba = UINT64_MAX - rv() % 300; break;             /* overflow */
            case 6: lba = (u64)-(long long)W_START + (rv() % 200) - 100; break; /* alias of phys 0 */
            default: lba = W_BLOCKS - n; break;                        /* exact fit at the end */
            }
            u32 extra = (rv() % 5 == 0) ? (1u << 30) : 0;     /* FUA sometimes */
            u16 s = guest_write(lba, n, (u32)it, extra);
            bool in = lba < W_BLOCKS && n <= W_BLOCKS - lba;
            if (in) { CHECK(s == 0); ok_w++; } else CHECK(s == VNVME_SC_INVALID_FIELD);
        } else if (kind < 8) {                           /* READ */
            u64 lba = rv() % 4 ? rv() % (W_BLOCKS + 256) : rv();
            u32 n = 1 + (u32)(rv() % 256);
            u16 s = guest_read(lba, n, DATA);
            bool in = lba < W_BLOCKS && n <= W_BLOCKS - lba;
            if (in) { CHECK(s == 0); ok_r++; } else CHECK(s == VNVME_SC_INVALID_FIELD);
        } else if (kind == 8) {                          /* FLUSH */
            struct vnvme_command c = {.opcode = VNVME_IO_FLUSH, .nsid = 1};
            CHECK(submit(1, c) == 0);
        } else {                                         /* forbidden I/O opcodes */
            static const u8 bad[] = {0x08 /*write zeroes*/, 0x09 /*DSM*/, 0x0d, 0x11, 0x19, 0x80, 0x90, 0xc0};
            u64 before = F.io_writes;
            struct vnvme_command c = {.opcode = bad[rv() % sizeof bad], .nsid = (u32)(rv() % 2 ? 1 : 2),
                                      .prp1 = WDATA, .cdw10 = (u32)(rv() % W_BLOCKS), .cdw12 = (u32)(rv() % 32)};
            u16 s = submit(1, c);
            CHECK(s == VNVME_SC_INVALID_OPCODE || s == VNVME_SC_NS_WRITE_PROTECTED ||
                  s == VNVME_SC_INVALID_NAMESPACE || s == VNVME_SC_INVALID_FIELD);
            CHECK(F.io_writes == before);                /* no forbidden op ever writes */
        }
        if ((it & 0x3f) == 0) assert_confined();
    }
    assert_confined();
    CHECK(F.mutating_ops == 0 && F.forbidden_ops == 0); /* ANS never saw a non-allowed opcode */
    CHECK(F.uncleaned_writes == 0);                      /* cache always cleaned before the write */
    printf("info: rw_fuzz ok_writes=%u ok_reads=%u ans_writes=%llu canary[%llu..%llu] window[%llu..%llu]\n",
           ok_w, ok_r, (unsigned long long)controller.wcanary.ans_write_cmds,
           (unsigned long long)controller.wcanary.abs_min_first, (unsigned long long)controller.wcanary.abs_max_last,
           (unsigned long long)W_START, (unsigned long long)(W_START + W_BLOCKS - 1));
    unsigned fl0 = F.io_flushes;
    hv_win_storage_retire();                             /* quiesce: FLUSH then stage-1 */
    CHECK(F.io_flushes > fl0);                            /* the quiesce really flushed ANS */
    CHECK(ans_ro_state() == ANS_RO_STOPPED && F.writes_outside_window == 0);
}

/* 2. canary faithfulness at exact extremes */
static void r_canary_faithful(void)
{
    bringup_rw(256);
    CHECK(guest_write(0, 1, 1, 0) == 0);                 /* absolute W_START */
    CHECK(guest_write(W_BLOCKS - 1, 1, 2, 0) == 0);      /* absolute W_START+W_BLOCKS-1 */
    CHECK(guest_write(100000, 256, 3, 0) == 0);
    u64 mn, mx, aw, rf; canary(&mn, &mx, &aw, &rf);
    CHECK(mn == W_START && mx == W_START + W_BLOCKS - 1);
    CHECK(rf == 0);
    /* the exposed S1WC canary (what a hardware run reads) is synced from that ground truth */
    CHECK(controller.wcanary.abs_min_first == mn && controller.wcanary.abs_max_last == mx);
    CHECK(controller.wcanary.magic == S1WC_MAGIC && controller.wcanary.window_refusals == 0);
    assert_confined();
}

/* 3. direct ANS-level attacks (bypass the frontend) must be refused by layer 2 */
static void r_direct_ans(void)
{
    bringup_rw(256);
    size_t cap; void *xb = ans_ro_xfer_buffer(&cap);
    (void)xb;
    u64 ow0 = F.writes_outside_window, iw0 = F.io_writes;
    /* absolute LBA just past the window end, and at phys 0 */
    CHECK(!ans_ro_xfer(true, W_START + W_BLOCKS, 1, 0));
    CHECK(!ans_ro_xfer(true, W_START + W_BLOCKS - 1, 2, 0));   /* straddles end */
    CHECK(!ans_ro_xfer(true, 0, 1, 0));                       /* phys 0 (real GPT) */
    CHECK(!ans_ro_xfer(true, W_START - 1, 1, 0));
    CHECK(!ans_ro_xfer(true, UINT64_MAX, 1, 0));              /* overflow */
    CHECK(F.writes_outside_window == ow0 && F.io_writes == iw0); /* nothing submitted */
    /* forbidden opcodes straight through the single submission path */
    static const u8 bad[] = {0x08, 0x09, 0x04, 0x80, 0x0d, 0xc0, 0x90};
    for (unsigned i = 0; i < sizeof bad; i++) CHECK(ans_ro_test_submit(false, bad[i]) == -1);
    CHECK(F.io_writes == iw0 && F.mutating_ops == 0 && F.forbidden_ops == 0);
    /* a legitimate in-window write still works */
    CHECK(ans_ro_xfer(true, W_START + 42, 4, 0));
    assert_confined();
}

/* 4. ro regression: without S1N_RW, writes are refused even with a writable fake disk */
static void r_ro_regression(void)
{
    fresh(rw_cfg());                                    /* fake allows rw, but contract will not ask */
    CHECK(select_native_w(good_window()) == 0);          /* no S1N_RW flag */
    CHECK(prep(W_BLOCKS) == 0);
    guest_bringup();
    CHECK(!s1n_writable());
    struct vnvme_command c = {.opcode = VNVME_ADMIN_IDENTIFY, .prp1 = IDBUF, .cdw10 = 0, .nsid = 1};
    CHECK(submit(0, c) == 0);
    CHECK(G(IDBUF)[99] & 1);                              /* NSATTR write-protected */
    CHECK(guest_write(0, 1, 1, 0) == VNVME_SC_NS_WRITE_PROTECTED);
    CHECK(guest_write(W_BLOCKS - 1, 8, 1, 0) == VNVME_SC_NS_WRITE_PROTECTED);
    CHECK(F.io_writes == 0 && F.writes_outside_window == 0);
    CHECK(controller.wcanary.ans_write_cmds == 0);
}

/* 5. OOB defense: issue writes that address outside the window and prove NO physical write
 *    ever lands outside it. The guest status may differ by which layer rejects (frontend vs
 *    precheck), but the physical-disk invariant must hold. Used by the mutant runs. */
static void r_oob_probe(void)
{
    bringup_rw(256);
    const struct { u64 lba; u32 n; } probes[] = {
        {W_BLOCKS, 1}, {W_BLOCKS - 1, 2}, {W_BLOCKS - 255, 256}, {W_BLOCKS, 256},
        {UINT64_MAX, 1}, {UINT64_MAX - 100, 256}, {(u64)-(long long)W_START, 1},
        {(u64)-(long long)W_START - 1, 8}, {W_BLOCKS + 1000000, 64},
    };
    for (unsigned i = 0; i < sizeof probes / sizeof probes[0]; i++) {
        u16 s = guest_write(probes[i].lba, probes[i].n, 100 + i, 0);
        CHECK(s != 0);                                   /* never acknowledged */
        CHECK(F.writes_outside_window == 0);             /* and never physically written */
    }
    /* a few in-window writes still work, nothing leaked outside */
    CHECK(guest_write(0, 1, 1, 0) == 0);
    CHECK(guest_write(W_BLOCKS - 1, 1, 2, 0) == 0);
    CHECK(F.writes_outside_window == 0 && fake_overlay_inside_window());
    printf("info: oob_probe writes_outside_window=%u window_refusals=%llu\n",
           F.writes_outside_window, (unsigned long long)controller.wcanary.window_refusals);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    const char *s = argv[1];
    if (!strcmp(s, "rw_fuzz")) r_rw_fuzz();
    else if (!strcmp(s, "oob_probe")) r_oob_probe();
    else if (!strcmp(s, "canary_faithful")) r_canary_faithful();
    else if (!strcmp(s, "direct_ans")) r_direct_ans();
    else if (!strcmp(s, "ro_regression")) r_ro_regression();
    else return 2;
    printf("{\"scenario\":\"%s\",\"checks\":%u}\n", s, checks);
    return 0;
}
