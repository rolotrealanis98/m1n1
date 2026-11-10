/* r4.5: every rejected operation is exercised in a fresh process. */
static void r45_frozen(void)
{
    CHECK(ans_ro_state() == ANS_RO_FROZEN);
    unsigned n = F.nio, a = F.nadmin, w = mmio_writes();
    CHECK(!ans_ro_xfer(false, W_START, 1, 0));
    CHECK(!ans_ro_flush());
    CHECK(!ans_ro_shutdown());
    ans_ro_poll();
    CHECK(F.nio == n && F.nadmin == a && mmio_writes() == w);
}
static void r45_boundary(int which, bool write, bool direct)
{
    static const u64 lbas[] = {W_BLOCKS - 1, W_BLOCKS, UINT64_MAX, UINT64_MAX - 1};
    static const u32 counts[] = {2, 1, 2, 256};
    static u8 data[ANS_XFER_BYTES];
    rw_up(rw_cfg(), 0);
    unsigned n = F.nio;
    if (direct) {
        if (write) CHECK(!s1n_write(lbas[which], counts[which], data));
        else CHECK(!s1n_read(lbas[which], counts[which], data));
    } else if (write) CHECK(guest_write(lbas[which], counts[which], 7, 0) == VNVME_SC_INVALID_FIELD);
    else CHECK(guest_read(lbas[which], counts[which], DATA) == VNVME_SC_INVALID_FIELD);
    CHECK(F.nio == n);
    if (!write && !direct) {
        CHECK(ans_ro_state() == ANS_RO_LIVE);
        CHECK(controller.queue.refused_reads == 1);
        CHECK(guest_read(0, 1, DATA) == 0);
    } else r45_frozen();
    printf("{\"window_refusals\":%lu}\n", (unsigned long)ans_ro_stats()->window_refusals);
}
static void r45_absolute(int which)
{
    rw_up(rw_cfg(), 0);
    const u64 lbas[] = {W_START - 1, W_START + W_BLOCKS, UINT64_MAX};
    unsigned n = F.nio;
    CHECK(!ans_ro_xfer(true, lbas[which], 1, 0));
    CHECK(F.nio == n);
    r45_frozen();
    printf("{\"window_refusals\":%lu}\n", (unsigned long)ans_ro_stats()->window_refusals);
}
static void r45_recheck(int mut)
{
    rw_up(rw_cfg(), 0);
    F.cfg.gpt_mutation = mut;
    CHECK(guest_write(4096, 1, 3, 0) == VNVME_SC_INTERNAL);
    CHECK(F.io_writes == 0);
    r45_frozen();
}
static void r45_counter(void)
{
    rw_up(rw_cfg(), 0);
    struct ans_ro_stats *a = (struct ans_ro_stats *)ans_ro_stats();
    a->io_completed++;
    unsigned n = F.nio;
    CHECK(!ans_ro_flush());
    CHECK(F.nio == n && a->reason == ANS_RO_E_COUNTER);
    r45_frozen();
}
static void r45_duplicate(void)
{
    rw_up(rw_cfg(), 0);
    F.cfg.duplicate_cqe = true;
    CHECK(guest_read(0, 1, DATA) == VNVME_SC_INTERNAL);
    CHECK(ans_ro_stats()->reason == ANS_RO_E_CQE && ans_ro_stats()->cqe_anomalies == 1);
    r45_frozen();
}
static void r45_mailbox(void)
{
    rw_up(rw_cfg(), 0);
    /* Hook fires in the mailbox drain and corrupts both SQ slots if they contain a WRITE. */
    CHECK(guest_write(4095, 1, 1, 0) == 0);
    F.cfg.tamper_mailbox = true;
    fake_spontaneous_syslog(1);
    CHECK(guest_write(4096, 1, 8, 0) == 0);
    CHECK(F.mailbox_tampers > 0 && F.writes_outside_window == 0);
    expect_guest(4096, 1, 8);
    outside_untouched();
}
static u64 discard_left, discard_seen;
static ssize_t r45_reader(void *buf, size_t n)
{
    if (!discard_left) return 0;
    if (n > 7) n = 7; /* partial reads must still drain everything */
    if (n > discard_left) n = discard_left;
    memset(buf, 0x55, n); discard_left -= n; discard_seen += n;
    return n;
}
static void r45_link(void)
{
    CHECK(s1n_link_raw());
    fresh(base_cfg());
    CHECK(select_native() == 0 && s1n_link_locked());
    CHECK(!s1n_link_raw()); CHECK(!s1n_link_raw());
    for (u64 op = 0; op < 0x1000; op++) {
        bool want = op == P_NOP || op == P_REBOOT || op == P_HV_WIN_STORAGE_NATIVE_STAT ||
            op == P_HV_WIN_STORAGE_STAT || op == P_HV_WIN_STATUS || op == P_HV_WIN_FB_STAT ||
            op == P_HV_WIN_PL011_STATS || op == P_HV_WIN_STORAGE_NATIVE_QUIESCE ||
            op == P_HV_WIN_STORAGE_PREP || op == P_HV_START ||
            /* r4.6 (M-A): CANARY only before HV_START. This loop consumes P_HV_START (0xc02)
             * before it reaches CANARY (0xc25), so CANARY must now be refused here; the
             * allowed-before-HV_START case is asserted in its own process by r46_proxy. */
            (S1N_STAGE_CANARY && op == P_HV_WIN_STORAGE_NATIVE_CANARY && op < P_HV_START);
        CHECK(s1n_link_proxy(op, false) == want);
    }
    CHECK(!s1n_link_proxy(P_HV_WIN_STORAGE_PREP, false));
    CHECK(!s1n_link_proxy(P_HV_START, false));
    CHECK(s1n_link_proxy(P_EXIT, true)); CHECK(!s1n_link_proxy(P_EXIT, false));
    CHECK(s1n_link_refusals() > 4000);
    for (unsigned sentinel = 0; sentinel < 2; sentinel++) {
        discard_seen = 0; discard_left = 513 + 4 * sentinel;
        CHECK(s1n_link_discard(513, sentinel, r45_reader));
        CHECK(!discard_left && discard_seen == 513 + 4 * sentinel);
    }
    discard_left = 2;
    CHECK(!s1n_link_discard(100, false, r45_reader));
    CHECK(hv_win_storage_native_stat(29) == S1N_STAGE);
    CHECK(hv_win_storage_native_stat(31) == 0x12340000);
    CHECK(hv_win_storage_native_stat(32) == 0x400000);
    for (unsigned i = 0; i < 16; i++) {
        CHECK(hv_win_storage_native_stat(64 + i) == F.s_cfg[i]);
        CHECK(hv_win_storage_native_stat(80 + i) == F.s_paddr[i]);
        CHECK(hv_win_storage_native_stat(96 + i) == F.s_size[i]);
    }
}
static void r45_pin_args(int which)
{
    fresh(base_cfg());
    struct s1n_window w = good_window();
    if (which == 0) w.start_lba++;
    if (which == 1) w.blocks--;
    if (which == 2) w.entry_index = 4;
    if (which == 3) w.unique_guid[0] ^= 1;
    if (which == 4) { w.start_lba++; w.blocks--; F.cfg.gpt_mutation = GM_START; }
    CHECK(select_native_w(w) == -1);
    CHECK(!F.nadmin && !F.nio && !F.ntr && s1n_link_locked());
    r45_frozen();
}
static void r45_stage(bool reject, bool corrupt)
{
    fresh(rw_cfg());
    CHECK(select_native_w(rw_window(0)) == 0);
    CHECK(s1n_writable() == (S1N_STAGE == 3));
    CHECK(prep(W_BLOCKS) == 0);
#if S1N_STAGE != 3
    guest_bringup();
    /* r4.6 (C9/L-1): an in-range guest WRITE to the read-only namespace is now terminal, so the
     * write-protect probe moved to its own process (r45_stage_ro_write) and no longer precedes
     * the stage reject/canary/flush checks below. */
#endif
    if (reject) {
        unsigned n = F.nio;
        CHECK(!ans_ro_xfer(true, S1N_ZONE_START, 1, 0));
        CHECK(F.nio == n);
        r45_frozen();
        return;
    }
#if S1N_STAGE == 0
    CHECK(!ans_ro_run_canary());
    for (unsigned i = 0; i < 4; i++) CHECK(ans_ro_flush());
    CHECK(!ans_ro_flush() && F.io_flushes == 4 && F.io_writes == 0);
    CHECK(ans_ro_stats()->reason == ANS_RO_E_CAP);
    r45_frozen();
#elif S1N_STAGE_CANARY
    if (corrupt) F.cfg.corrupt_canary_read = true;
    bool ok = ans_ro_run_canary();
    if (corrupt) { CHECK(!ok && ans_ro_stats()->reason == ANS_RO_E_CANARY); r45_frozen(); return; }
    CHECK(ok);
    CHECK(F.io_writes == S1N_WRITE_CMD_CAP && F.write_blocks == S1N_WRITE_BLOCK_CAP);
    CHECK(ans_ro_stats()->cmd_writes == F.io_writes && ans_ro_stats()->blocks_written == F.write_blocks);
    CHECK(ans_ro_stats()->lowest_written == S1N_SUB_START);
    CHECK(ans_ro_stats()->highest_written == S1N_SUB_START + S1N_SUB_BLOCKS - 1);
    CHECK(ans_ro_stats()->io_completed == ans_ro_stats()->io_submitted);
    CHECK(!ans_ro_run_canary());
    u8 got[4096];
    for (u64 lba = S1N_SUB_START; lba < S1N_SUB_START + S1N_SUB_BLOCKS; lba++) {
        fake_media_block(lba, got);
        CHECK(fake_phys_of_pattern(got) == lba);
        for (unsigned i = 8; i < 4096; i++) CHECK(got[i] == (u8)(lba * 31 + i * 7));
    }
    CHECK(hv_win_storage_native_quiesce() == 0);
    CHECK(F.writes_outside_window == 0 && F.uncleaned_writes == 0);
#else
    (void)corrupt;
#endif
}
static void r45_cache(void)
{
    fresh(rw_cfg());
    CHECK(select_native_w(rw_window(0)) == 0);
    CHECK(F.clean_init_tcb == 2 && F.clean_init_cqe == 2);
    CHECK(F.dsb_syncs == 2 + 4 * ans_ro_stats()->commands + F.io_reads);
    CHECK(F.clean_sqe == ans_ro_stats()->commands && F.clean_tcb == ans_ro_stats()->commands);
    CHECK(F.invalidate_cqe == 3 * ans_ro_stats()->commands);
    CHECK(F.clean_init_sqe == 2 + 2 * F.io_reads); /* queues plus pre/post READ clean */
}
static void r45_raw_ro(void)
{
    fresh(rw_cfg());
    CHECK(select_native() == 0);
    unsigned n = F.nio;
    int rc = ans_ro_test_submit(false, 1);
    CHECK(rc < 0 && F.nio == n);
    printf("{\"window_refusals\":%lu}\n", (unsigned long)ans_ro_stats()->window_refusals);
}
static void r45_stage_bound(int which)
{
#if S1N_STAGE_CANARY
    fresh(rw_cfg());
    CHECK(select_native_w(rw_window(0)) == 0);
    const u64 lba[] = {S1N_SUB_START - 1, S1N_SUB_START + S1N_SUB_BLOCKS, S1N_SUB_START};
    unsigned n = F.nio;
    u32 blocks = which == 2 ? S1N_WRITE_MAX_BLOCKS + 1 : 1;
    CHECK(!ans_ro_test_stage_write(lba[which], blocks));
    CHECK(F.nio == n && F.io_writes == 0);
    r45_frozen();
#else
    (void)which;
#endif
}
static void r45_ro_guest_outside(void)
{
    fresh(base_cfg());
    CHECK(select_native() == 0 && prep(W_BLOCKS) == 0);
    guest_bringup();
    unsigned n = F.nio;
    CHECK(guest_write(W_BLOCKS, 1, 1, 0) == VNVME_SC_NS_WRITE_PROTECTED);
    CHECK(F.nio == n);
    r45_frozen();
}
static void r45_write_cap(bool blocks)
{
#if S1N_STAGE_CANARY
    fresh(rw_cfg());
    CHECK(select_native_w(rw_window(0)) == 0);
    if (blocks) ((struct ans_ro_stats *)ans_ro_stats())->blocks_written = S1N_WRITE_BLOCK_CAP;
    else CHECK(ans_ro_run_canary());
    unsigned n = F.nio;
    /* Cap refusal precedes even the first-write GPT recheck. */
    CHECK(!ans_ro_test_stage_write(S1N_SUB_START, 1));
    CHECK(F.io_writes == (blocks ? 0 : S1N_WRITE_CMD_CAP));
    CHECK(F.nio == n);
    CHECK(ans_ro_stats()->reason == ANS_RO_E_CAP);
    r45_frozen();
#else
    (void)blocks;
#endif
}
static void r45_admin_status(void)
{
    fresh(base_cfg()); CHECK(select_native() == 0);
    unsigned admin = F.nadmin, io = F.nio;
    F.cfg.admin_error = true;
    CHECK(!ans_ro_shutdown());
    CHECK(F.nadmin == admin + 1 && F.nio == io);
    CHECK(ans_ro_stats()->reason == ANS_RO_E_STATUS);
    r45_frozen();
}
/* r4.6 replacement for the write-protect probe formerly inside r45_stage: still refused with
 * NS_WRITE_PROTECTED, still no ANS command, and now (C9) the session is frozen with no further I/O. */
static void r45_stage_ro_write(void)
{
#if S1N_STAGE != 3
    fresh(rw_cfg());
    CHECK(select_native_w(rw_window(0)) == 0);
    CHECK(!s1n_writable());
    CHECK(prep(W_BLOCKS) == 0);
    guest_bringup();
    unsigned n = F.nio, a = F.nadmin;
    CHECK(io_cmd(VNVME_IO_WRITE, 4096, 1, WDATA, 0) == VNVME_SC_NS_WRITE_PROTECTED);
    CHECK(F.io_writes == 0 && F.nio == n && F.nadmin == a);
    CHECK(ans_ro_stats()->reason == ANS_RO_E_ABORT);
    r45_frozen();
    CHECK(!ans_ro_run_canary() && F.io_writes == 0);
#endif
}
static bool r45_dispatch(const char *name)
{
    if (!strcmp(name, "r45_admin_status")) r45_admin_status();
    else if (!strcmp(name, "r45_write_cap")) r45_write_cap(false);
    else if (!strcmp(name, "r45_block_cap")) r45_write_cap(true);
    else if (!strcmp(name, "r45_ro_guest_outside")) r45_ro_guest_outside();
    else if (!strncmp(name, "r45_stage_bound_", 16)) r45_stage_bound(atoi(name + 16));
    else if (!strcmp(name, "r45_cache")) r45_cache();
    else if (!strcmp(name, "r45_raw_ro")) r45_raw_ro();
    else if (!strncmp(name, "r45_guest_read_", 15)) r45_boundary(atoi(name + 15), false, false);
    else if (!strncmp(name, "r45_guest_write_", 16)) r45_boundary(atoi(name + 16), true, false);
    else if (!strncmp(name, "r45_direct_read_", 16)) r45_boundary(atoi(name + 16), false, true);
    else if (!strncmp(name, "r45_direct_write_", 17)) r45_boundary(atoi(name + 17), true, true);
    else if (!strncmp(name, "r45_absolute_", 13)) r45_absolute(atoi(name + 13));
    else if (!strncmp(name, "r45_pin_args_", 13)) r45_pin_args(atoi(name + 13));
    else if (!strncmp(name, "r45_recheck_", 12)) r45_recheck(atoi(name + 12));
    else if (!strcmp(name, "r45_counter")) r45_counter();
    else if (!strcmp(name, "r45_duplicate")) r45_duplicate();
    else if (!strcmp(name, "r45_mailbox")) r45_mailbox();
    else if (!strcmp(name, "r45_link")) r45_link();
    else if (!strcmp(name, "r45_stage")) r45_stage(false, false);
    else if (!strcmp(name, "r45_stage_reject")) r45_stage(true, false);
    else if (!strcmp(name, "r45_stage_corrupt")) r45_stage(false, true);
    else if (!strcmp(name, "r45_stage_ro_write")) r45_stage_ro_write();
    else return false;
    return true;
}
