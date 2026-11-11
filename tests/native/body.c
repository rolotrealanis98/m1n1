/* r3 end-to-end scenarios: guest NVMe (S1 frontend) -> native window -> ans_ro -> fake ANS.
 * One scenario per process (every layer is one-shot by design). */

#define ASQ (GUEST + 0x0000)
#define ACQ (GUEST + 0x1000)
#define IOSQ (GUEST + 0x2000)
#define IOCQ (GUEST + 0x3000)
#define PRPL (GUEST + 0x4000)
#define IDBUF (GUEST + 0x5000)
#define DATA (GUEST + 0x10000)
#define G(a) (guest + ((a) - GUEST))

static struct { struct hv_s1_registration head; struct hv_sm_range ranges[2]; } packet;
static u16 next_cid = 1;

static void fresh(struct fake_cfg g)
{
    fake_reset(&g);
    u64 b, sz;
    ans_ro_dma_region(&b, &sz);
    fake_set_dma_region2(b, sz);
}
static s64 select_native_w(struct s1n_window w) { return hv_win_storage_native_select((u64)&w, sizeof(w)); }
static struct s1n_window good_window(void)
{
    struct s1n_window w = {W_START, W_BLOCKS, 7, 0, {0}, {0}};
    memcpy(w.type_guid, W_TYPE, 16);
    memcpy(w.unique_guid, W_GUID, 16);
    return w;
}
static s64 select_native(void) { return select_native_w(good_window()); }
static s64 prep(u64 blocks)
{
    packet = (typeof(packet)){
        .head = {.device = {HV_S1_MAGIC, 1, HV_S1_ECAM, HV_S1_BAR, S1_BAR_SIZE, 66, 7, 4096, 0, blocks, 0},
                 .epoch = 17, .resident_base = 0x1000, .resident_size = UINT64_MAX - 0x1001,
                 .nr_ram = 1, .nr_protected = 1},
        .ranges = {{GUEST, sizeof(guest)}, {GUEST + sizeof(guest), 4096}}};
    return hv_win_storage_prepare((u64)&packet, sizeof(packet));
}
static bool wr(u64 off, u64 v, int width) { return hooks[1](NULL, HV_S1_BAR + off, &v, true, width); }
static u16 submit(unsigned qid, struct vnvme_command c)
{
    struct vnvme_queue *q = &controller.queue.queues[qid];
    u16 slot = q->cq_tail;
    c.cid = next_cid++;
    memcpy(G(q->sq_addr) + (u64)q->sq_tail * sizeof(c), &c, sizeof(c));
    CHECK(wr(0x1000 + qid * 8, (q->sq_tail + 1) % q->sq_size, 2));
    CHECK(q->cq_pending == 1);
    struct vnvme_completion done;
    memcpy(&done, G(q->cq_addr) + (u64)slot * sizeof(done), sizeof(done));
    CHECK(done.cid == c.cid && done.sq_id == qid);
    CHECK(wr(0x1004 + qid * 8, q->cq_tail, 2));
    return done.status >> 1;
}
static void guest_bringup(void)
{
    CHECK(wr(0x24, 0x00070007, 2));
    CHECK(wr(0x28, ASQ, 3));
    CHECK(wr(0x30, ACQ, 3));
    CHECK(wr(0x14, S1_CC_VALID, 2));
    CHECK(controller.regs.csts == 1);
    struct vnvme_command c = {.opcode = VNVME_ADMIN_IDENTIFY, .nsid = 1, .prp1 = IDBUF, .cdw10 = 0};
    CHECK(submit(0, c) == 0);
    u64 nsze;
    memcpy(&nsze, G(IDBUF), 8);
    CHECK(nsze == W_BLOCKS);
    CHECK(G(IDBUF)[99] & 1); /* NSATTR write protected */
    c = (struct vnvme_command){.opcode = VNVME_ADMIN_IDENTIFY, .prp1 = IDBUF, .cdw10 = 1};
    CHECK(submit(0, c) == 0);
    CHECK(G(IDBUF)[525] == 0); /* VWC = 0 */
    c = (struct vnvme_command){.opcode = VNVME_ADMIN_CREATE_CQ, .prp1 = IOCQ, .cdw10 = (15 << 16) | 1, .cdw11 = 3};
    CHECK(submit(0, c) == 0);
    c = (struct vnvme_command){.opcode = VNVME_ADMIN_CREATE_SQ, .prp1 = IOSQ, .cdw10 = (15 << 16) | 1, .cdw11 = (1 << 16) | 1};
    CHECK(submit(0, c) == 0);
}
static u16 guest_read(u64 lba, u32 n, u64 dest)
{
    struct vnvme_command c = {.opcode = VNVME_IO_READ, .nsid = 1, .prp1 = dest,
                              .cdw10 = (u32)lba, .cdw11 = (u32)(lba >> 32), .cdw12 = n - 1};
    if (n == 2) c.prp2 = dest + 4096;
    if (n > 2) {
        for (u32 i = 1; i < n; i++) { u64 e = dest + 4096ULL * i; memcpy(G(PRPL) + 8 * (i - 1), &e, 8); }
        c.prp2 = PRPL;
    }
    return submit(1, c);
}
static void check_blocks(u64 lba, u32 n, u64 dest)
{
    for (u32 i = 0; i < n; i++) {
        const u8 *b = G(dest) + 4096ULL * i;
        CHECK(fake_phys_of_pattern(b) == W_START + lba + i);
        CHECK(b[100] == (u8)((W_START + lba + i) * 31 + 100 * 7));
    }
}
static void live_policy(void)
{
    CHECK(F.dma_violations == 0);
    for (unsigned i = 0; i < F.ndma; i++) CHECK(fake_in_arena(F.dma_addrs[i], 1));
    CHECK(F.forbidden_ops == 0 && F.mutating_ops == 0);
    CHECK(F.tcb_mismatch == 0 && F.tag_fault == 0 && F.cfs_events == 0);
    CHECK(F.unknown_nvme == 0 && F.unknown_nvmmu == 0 && F.unmapped == 0);
    CHECK(F.prot_writes == 0 && fake_sart_protected_unchanged());
    CHECK(F.dart_calls == 0);
    /* r4.3: alignment / PRP policy (strict fake). */
    CHECK(F.prp_rejects == 0 && F.misaligned_base == 0);
    CHECK(F.dma_outside == 0 && F.dma_into_queue == 0);
    CHECK(F.not16k == 0);                      /* every queue/TCB base 16 KiB aligned */
    /* r4.4 (= r3.5): only T6050-proven CC fields / Create CQ flags ever reach the controller. */
    CHECK(F.cc_unproven == 0 && F.cq_flags_unproven == 0);
#ifdef ANS_RO_HAVE_R44
    /* r4.4: I/O on NVMMU tag 2 only (Mu core), admin on 0; ANS never sees FUA (flush instead). */
    CHECK(F.tag_policy == 0 && F.fua_writes == 0);
    for (unsigned i = 0; i < 64; i++) CHECK(i == ANS_RO_IO_TAG || i == ANS_RO_IO_TAG_ALT || F.io_tag_seen[i] == 0);
#endif
}
static void exposed_nothing(void)
{
    CHECK(s1_backend_blocks() == 0);           /* poisoned */
    CHECK(prep(W_BLOCKS) == -1);
    CHECK(hook_calls == 0);
}
static void stopped_like_stage1(void)
{
    CHECK((F.cpu_control & 0x10) == 0 && F.cpu_stops == 1);
    CHECK(F.stopped_after_sleep_ack && !F.stop_while_cc_en);
    CHECK(F.pmgr_resets == 2);                 /* stage-1 parity: "ANS" and "ANS2" */
    /* stage-1 parity: SHN normal request, then clear32(CC.EN) which keeps the SHN field. */
    CHECK(F.shn_writes == 2);
    for (int i = F.cfg.protected_entries; i < 16; i++) CHECK(F.s_cfg[i] == 0);
    CHECK(F.sart_clear_while_live == 0 && F.frees_while_live == 0);
    CHECK(ans_ro_state() == ANS_RO_STOPPED);
#ifdef ANS_RO_HAVE_R44
    /* r4.4: +0x1200 then +0x1208 zeroed after Delete SQ/CQ (aura nvme_shutdown), never under a live queue */
    CHECK(F.iosq_reg == 0 && F.iocq_reg == 0 && ans_ro_stats()->ioq_regs_cleared == 1);
    CHECK(F.ioq_reg_zero_while_queues == 0 && F.ioq_reg_zero_order_bad == 0);
    CHECK(hv_win_storage_native_stat(25) == 1);
#endif
}
static unsigned mmio_writes(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < F.ntr; i++) n += F.tr[i].write;
    return n;
}
static void frozen_untouched(void)
{
    CHECK(ans_ro_state() == ANS_RO_FROZEN);
    CHECK(F.frees == 0 && F.pmgr_resets == 0 && F.cpu_stops == 0 && F.sart_clear_while_live == 0);
}
static struct fake_cfg base_cfg(void)
{
    struct fake_cfg g = {0};
    g.protected_entries = 7;          /* as measured on hardware: [0..5,7] -> 7 protected */
    g.sart_version = 4;
    g.nvmmu_mirror_on_reg9 = true;    /* as measured on hardware */
    return g;
}

/* ------------------------------------------------------------------ scenarios */
static void s_happy(void)
{
    fresh(base_cfg());
    CHECK(select_native() == 0);
    CHECK(ndenied == 5);
    u32 want[5] = {1347, 1348, 1349, 1350, 2338};
    for (int i = 0; i < 5; i++) CHECK(denied[i] == want[i]);
    CHECK(s1_backend_blocks() == W_BLOCKS && s1n_verified());
    /* nvme.c parity: set32(LSQ_CTRL) = one read + one write. */
    CHECK(F.hellos == 1 && F.cpu_starts == 1 && F.lsq_ctrl_access == 2);
}
static void s_e2e(void)
{
    fresh(base_cfg());
    CHECK(select_native() == 0);
    CHECK(prep(40) == -1);              /* wrong blocks: refused, and PREP is now spent */
}
static void s_e2e_reads(void)
{
    fresh(base_cfg());
    CHECK(select_native() == 0);
    CHECK(prep(W_BLOCKS) == 0);
    guest_bringup();
    unsigned base_reads = F.io_reads;
    CHECK(guest_read(0, 1, DATA) == 0); check_blocks(0, 1, DATA);
    CHECK(guest_read(W_BLOCKS - 1, 1, DATA) == 0); check_blocks(W_BLOCKS - 1, 1, DATA);
    CHECK(guest_read(1000, 2, DATA) == 0); check_blocks(1000, 2, DATA);
    CHECK(guest_read(123456, 32, DATA) == 0); check_blocks(123456, 32, DATA);  /* 128 KiB */
    CHECK(guest_read(W_BLOCKS - 32, 32, DATA) == 0); check_blocks(W_BLOCKS - 32, 32, DATA);
    CHECK(F.io_reads == base_reads + 1 + 1 + 2 + 32 + 32);
    /* Boundary refusals run in isolated r45_guest_read_* processes. */
    /* Writes and flushes never reach ANS. */
    unsigned nio = F.nio;
    /* r4.6 (C9/L-1): the in-range WRITE to the read-only namespace is terminal now; it runs in
     * its own process (e2e_reads_ro_write) so the clean-retire checks below still run here. */
    struct vnvme_command c = (struct vnvme_command){.opcode = VNVME_IO_FLUSH, .nsid = 1};
    CHECK(submit(1, c) == 0);
    c = (struct vnvme_command){.opcode = 0x08 /* write zeroes */, .nsid = 1, .prp1 = DATA};
    CHECK(submit(1, c) == VNVME_SC_INVALID_OPCODE);
    CHECK(F.nio == nio);
    live_policy();
    for (unsigned i = 0; i < F.nio && i < 64; i++) CHECK(F.io_op_seq[i] == 0x02);
    CHECK(F.syslog_unacked == 0);
    /* hv exit / storage retire: stage-1 quiesce exactly once. */
    hv_win_storage_retire();
    stopped_like_stage1();
    unsigned a = F.nadmin;
    CHECK(F.admin_op_seq[a - 2] == 0x00 && F.admin_op_seq[a - 1] == 0x04);  /* Delete SQ, Delete CQ */
    hv_win_storage_retire();                                                 /* idempotent */
    CHECK(F.cpu_stops == 1 && F.pmgr_resets == 2);
}
/* r4.6: the e2e_reads write-protect probe. Still NS_WRITE_PROTECTED with no ANS command; C9 now
 * freezes the session, and the retire after a freeze issues no teardown I/O. */
static void r45_frozen(void);
static void s_e2e_reads_ro_write(void)
{
    fresh(base_cfg());
    CHECK(select_native() == 0);
    CHECK(prep(W_BLOCKS) == 0);
    guest_bringup();
    CHECK(guest_read(0, 1, DATA) == 0); check_blocks(0, 1, DATA);
    unsigned nio = F.nio, na = F.nadmin;
    struct vnvme_command c = {.opcode = VNVME_IO_WRITE, .nsid = 1, .prp1 = DATA, .cdw10 = 0};
    CHECK(submit(1, c) == VNVME_SC_NS_WRITE_PROTECTED);
    CHECK(F.nio == nio && F.nadmin == na && F.io_writes == 0);
    CHECK(ans_ro_stats()->reason == ANS_RO_E_ABORT);
    r45_frozen();
    hv_win_storage_retire();
    CHECK(F.nio == nio && F.nadmin == na && F.cpu_stops == 0);
}
static void s_gpt(int mut)
{
    struct fake_cfg g = base_cfg();
    g.gpt_mutation = mut;
    fresh(g);
    CHECK(select_native() == -1);
    CHECK(!s1n_verified());
    exposed_nothing();
    frozen_untouched(); /* GPT mismatch is terminal; no teardown I/O. */
    live_policy();
}
static void s_window_args(void)
{
    fresh(base_cfg());
    struct s1n_window w = good_window();
    w.blocks += 1;
    CHECK(select_native_w(w) == -1);
    exposed_nothing();
}
static void s_window_args_type(void)
{
    fresh(base_cfg());
    struct s1n_window w = good_window();
    w.type_guid[3] ^= 0x80;  /* not Microsoft basic data: refused before ANS is touched */
    CHECK(select_native_w(w) == -1);
    CHECK(F.cpu_starts == 0 && F.ntr == 0);
    exposed_nothing();
}
static void s_refuse_running(void)
{
    struct fake_cfg g = base_cfg();
    g.cpu_running_at_entry = true;
    fresh(g);
    CHECK(select_native() == -1);
    CHECK(ans_ro_state() == ANS_RO_REFUSED && F.cpu_starts == 0);
    for (unsigned i = 0; i < F.ntr; i++) CHECK(!F.tr[i].write);  /* reads only */
    exposed_nothing();
}
static void s_refuse_sart(void)
{
    struct fake_cfg g = base_cfg();
    g.protected_entries = 13;
    fresh(g);
    CHECK(select_native() == -1);
    CHECK(ans_ro_state() == ANS_RO_REFUSED && F.cpu_starts == 0);
    for (unsigned i = 0; i < F.ntr; i++) CHECK(!F.tr[i].write);
    exposed_nothing();
}
static void s_aic_already_routed(void)
{
    fresh(base_cfg());
    deny_fail_line = 2338;
    CHECK(select_native() == -1);
    CHECK(F.ntr == 0 && F.cpu_starts == 0);
    exposed_nothing();
}
static void s_freeze_init(int which)
{
    struct fake_cfg g = base_cfg();
    if (which == 0) g.no_hello = true;
    if (which == 1) g.boot_status_never_ok = true;
    if (which == 2) g.rdy_never_set = true;
    if (which == 3) g.cqe_never = true;
    if (which == 4) g.no_power_on_ack = true;
    fresh(g);
    CHECK(select_native() == -1);
    frozen_untouched();
    unsigned w = mmio_writes();
    exposed_nothing();
    hv_win_storage_retire();          /* hv exit: still untouched */
    frozen_untouched();
    CHECK(mmio_writes() == w);        /* not a single MMIO write after the freeze */
}
static void s_freeze_runtime(int which)
{
    fresh(base_cfg());
    CHECK(select_native() == 0);
    CHECK(prep(W_BLOCKS) == 0);
    guest_bringup();
    CHECK(guest_read(5, 1, DATA) == 0);
    if (which == 0) F.cfg.cqe_never = true;
    if (which == 1) F.cfg.tcb_stat_fail = true;
    if (which == 2) F.cfg.crash_on_read = true;
    if (which == 3) F.cfg.wrong_tag = true;
    CHECK(guest_read(6, 1, DATA) == VNVME_SC_INTERNAL);     /* guest sees an error, not a hang */
    frozen_untouched();
    unsigned w = mmio_writes();
    unsigned nio = F.nio;
    CHECK(guest_read(7, 1, DATA) == VNVME_SC_INTERNAL);     /* no retry, no ANS access */
    CHECK(F.nio == nio);
    hv_win_storage_retire();
    frozen_untouched();
    CHECK(mmio_writes() == w);        /* no retry, no quiesce, no MMIO at all after the freeze */
}
static void s_liveness(void)
{
    struct fake_cfg g = base_cfg();
    g.syslog_backpressure = true;
    fresh(g);
    CHECK(select_native() == 0);
    CHECK(prep(W_BLOCKS) == 0);
    guest_bringup();
    /* Idle guest: the firmware keeps logging; only the EL2 tick drains it. */
    fake_spontaneous_syslog(40);
    CHECK(F.syslog_unacked == 40);
    for (int i = 0; i < 3; i++) hv_win_storage_tick();
    CHECK(F.syslog_unacked == 0);
    /* Guest load interleaved with firmware chatter and ticks. */
    for (unsigned k = 0; k < 64; k++) {
        fake_spontaneous_syslog(10);
        if (k % 4 == 0) hv_win_storage_tick();
        CHECK(guest_read(k * 977, 8, DATA) == 0);
        check_blocks(k * 977, 8, DATA);
        CHECK(F.syslog_unacked <= 32);
    }
    CHECK(F.refused_doorbells == 0 && ans_ro_state() == ANS_RO_LIVE);
    live_policy();
    const struct ans_ro_stats *st = ans_ro_stats();
    CHECK(st->polls >= 19 && st->mailbox_msgs > 0);
}
/* r4.6 (C9/L-1): a refused physical opcode is terminal (-2, FROZEN, ANS_RO_E_NOT_ALLOWED) instead
 * of a nonterminal -1, so each opcode of the original list runs in its own process. */
static const u8 allow_io_bad[] = {0x00, 0x01, 0x04, 0x05, 0x08, 0x09, 0x0c, 0x0d, 0x11, 0x19, 0x81};
static const u8 allow_adm_bad[] = {0x06 /* r4.3: Identify not allowed */, 0x02, 0x08, 0x09, 0x0a, 0x0c, 0x0d, 0x10, 0x11, 0x15, 0x19, 0x80, 0x84, 0xc0, 0xd8};
static void s_allowlist_one(bool admin, unsigned i)
{
    fresh(base_cfg());
    CHECK(select_native() == 0);
    CHECK(i < (admin ? sizeof(allow_adm_bad) : sizeof(allow_io_bad)));
    unsigned na = F.nadmin, ni = F.nio, w = mmio_writes();
    CHECK(ans_ro_test_submit(admin, admin ? allow_adm_bad[i] : allow_io_bad[i]) == -2);
    CHECK(F.nadmin == na && F.nio == ni && F.mutating_ops == 0 && F.forbidden_ops == 0);
    CHECK(mmio_writes() == w);
    CHECK(ans_ro_state() == ANS_RO_FROZEN && ans_ro_stats()->reason == ANS_RO_E_NOT_ALLOWED);
    CHECK(ans_ro_test_submit(false, 0x02) == -2 && F.nio == ni); /* nothing after the freeze */
}
static void s_allowlist(void)
{
    fresh(base_cfg());
    CHECK(select_native() == 0);
    unsigned na = F.nadmin, ni = F.nio;
    const u8 io_bad[] = {0x00, 0x01, 0x04, 0x05, 0x08, 0x09, 0x0c, 0x0d, 0x11, 0x19, 0x81};
    const u8 adm_bad[] = {0x06 /* r4.3: Identify not allowed */, 0x02, 0x08, 0x09, 0x0a, 0x0c, 0x0d, 0x10, 0x11, 0x15, 0x19, 0x80, 0x84, 0xc0, 0xd8};
    (void)io_bad; (void)adm_bad; /* r4.6: refused opcodes are checked one per process (allowlist_io_N/adm_N) */
    CHECK(F.nadmin == na && F.nio == ni && F.mutating_ops == 0 && F.forbidden_ops == 0);
    CHECK(ans_ro_state() == ANS_RO_LIVE);
    CHECK(ans_ro_test_submit(false, 0x02) == 0);     /* the one allowed I/O opcode still works */
}
static void s_window_direct(int which)
{
    fresh(base_cfg());
    CHECK(select_native() == 0);
    static u8 buf[256 * 4096]; /* r4: direct reads up to 256 blocks */
    CHECK(s1n_read(0, 1, buf) && fake_phys_of_pattern(buf) == W_START);
    CHECK(s1n_read(W_BLOCKS - 1, 1, buf) && fake_phys_of_pattern(buf) == W_START + W_BLOCKS - 1);
    unsigned r = F.io_reads;
    if (which == 0) CHECK(!s1n_read(W_BLOCKS - 1, 2, buf));
    if (which == 1) CHECK(!s1n_read(W_BLOCKS, 1, buf));
    if (which == 2) CHECK(!s1n_read(UINT64_MAX, 2, buf));
    if (which == 3) CHECK(!s1n_read(UINT64_MAX - 1, 32, buf));
    if (which == 4) CHECK(!s1n_read(0, 0, buf));
    if (which == 5) CHECK(!s1n_read(0, 257, buf)); /* r4: limit is 256 (r3.1: 32) */
    if (which == 6) CHECK(!s1n_read(0, 1, NULL));
    /* r4: which layer refuses may differ (four window checks); the property is: no ANS read. */
    CHECK(F.io_reads == r && s1n_status()->refused_reads + ans_ro_stats()->window_refusals >= 1);
}
static void s_fixture_default(void)
{
    /* No native select: r2.2 behaviour, fixture of 40 blocks, ANS never touched. */
    fresh(base_cfg());
    CHECK(prep(40) == 0);
    CHECK(F.ntr == 0 && F.cpu_starts == 0);
    hv_win_storage_tick();
    hv_win_storage_retire();
    CHECK(F.ntr == 0);
}
static void s_late_select(void)
{
    fresh(base_cfg());
    CHECK(prep(40) == 0);
    CHECK(select_native() == -1);        /* after PREP: refused, fixture untouched */
    CHECK(s1_backend_blocks() == 40 && F.ntr == 0);
}

/* ---------------------------------------------------------- r3.1 fixes */
static void s_select_no_prep(void)            /* R1: drained before PREP, quiesced by retire */
{
    fresh(base_cfg());
    CHECK(select_native() == 0);
    fake_spontaneous_syslog(20);
    for (int i = 0; i < 5; i++) hv_win_storage_tick();
    CHECK(ans_ro_stats()->polls >= 5 && F.syslog_unacked == 0);
    hv_win_storage_retire();                  /* e.g. hv exit after an aborted launch */
    stopped_like_stage1();
}
static void s_quiesce_op(void)                /* R1: launcher finally path / hardware run 0 */
{
    fresh(base_cfg());
    CHECK(hv_win_storage_native_quiesce() == -1);   /* nothing selected */
    CHECK(select_native() == 0);
    hv_started_cpus[0] = true;
    CHECK(hv_win_storage_native_quiesce() == -1);   /* refused while a guest CPU runs */
    CHECK(ans_ro_state() == ANS_RO_LIVE && F.cpu_stops == 0);
    hv_started_cpus[0] = false;
    CHECK(hv_win_storage_native_quiesce() == 0);
    stopped_like_stage1();
    CHECK(hv_win_storage_native_quiesce() == 0);    /* idempotent, no second quiesce */
    CHECK(F.cpu_stops == 1 && F.pmgr_resets == 2);
    CHECK(prep(W_BLOCKS) == -1);                   /* a quiesced window is never exposed */
}
static void s_quiesce_after_prep(void)
{
    fresh(base_cfg());
    CHECK(select_native() == 0);
    CHECK(prep(W_BLOCKS) == 0);
    CHECK(hv_win_storage_native_quiesce() == 0);    /* PREP done, guest never started */
    stopped_like_stage1();
    CHECK(hv_win_storage_stat(0) & 4);
    CHECK(!(hv_win_storage_stat(0) & 1));            /* frontend retired: not ready */
}
static void s_stale_bounce(void)              /* R2 */
{
    fresh(base_cfg());
    CHECK(select_native() == 0);
    CHECK(s1n_test_buffers_clear());               /* no GPT bytes left behind */
    CHECK(prep(W_BLOCKS) == 0);
    guest_bringup();
    CHECK(guest_read(4242, 1, DATA) == 0);         /* bounce now holds a real block */
    check_blocks(4242, 1, DATA);
    F.cfg.read_no_dma = true;                      /* firmware "success" without DMA */
    memset(G(DATA), 0x77, 4096);
    CHECK(guest_read(0, 1, DATA) == 0);
    for (unsigned i = 0; i < 4096; i++) CHECK(G(DATA)[i] == 0);  /* zeros, never stale data */
    /* (all-zero output cannot equal the non-zero GPT header/entry sectors that were read last) */
    { u8 b[4096]; fake_disk_block(1, b); CHECK(memcmp(G(DATA), b, 4096)); fake_disk_block(2, b); CHECK(memcmp(G(DATA), b, 4096)); }
}
static void s_gpt_reject_scrub(void)
{
    struct fake_cfg g = base_cfg();
    g.gpt_mutation = GM_GUID;
    fresh(g);
    CHECK(select_native() == -1);
    CHECK(s1n_test_buffers_clear());
}
static void s_hello_slow(void)               /* HELLO 3 s after INIT (inside the 5 s / 10 s bounds) */
{
    struct fake_cfg g = base_cfg();
    g.hello_delay_us = 3000000;
    fresh(g);
    CHECK(select_native() == 0);
    CHECK(F.hellos == 1 && ans_ro_state() == ANS_RO_LIVE);
}
/* r4.4: the r4.2 `identify_bad` scenarios are gone with the unproven "poisoned admin queue" model
 * (r3.4 INDEPENDENT-REVIEW F3/M2). The fake counts any Identify as forbidden (live_policy). */
static void s_capacity_mismatch(void)
{
    struct fake_cfg g = base_cfg();
    g.gpt_mutation = GM_CAPACITY;
    fresh(g);
    CHECK(select_native() == -1);
    CHECK(s1n_status()->reason == S1N_E_CAPACITY);
    exposed_nothing();
    frozen_untouched();
}
static void s_no_identify_issued(void)     /* r4.2: no standalone Identify ever reaches ANS */
{
    fresh(base_cfg());
    CHECK(select_native() == 0);
    CHECK(F.admin_ops[0x06] == 0);
    CHECK(ans_ro_stats()->identify_nsze == UINT64_MAX && ans_ro_stats()->identity_confirmed == 0);
}


/* ============================================================ r4 read-write */
#define WDATA (GUEST + 0x100000)   /* 1 MiB guest write source */
#define RDATA (GUEST + 0x200000)   /* 1 MiB guest read destination */
static struct s1n_window rw_window(u32 maxxfer)
{
    struct s1n_window w = good_window();
    w.flags = S1N_RW | (maxxfer << S1N_MAXXFER_SHIFT);
    return w;
}
static struct fake_cfg rw_cfg(void)
{
    struct fake_cfg g = base_cfg();
    g.allow_rw = true;
    return g;
}
/* Guest data for window LBA `l` at generation `gen`: unique per (l, gen). */
static void gen_block(u64 l, u32 gen, u8 *out)
{
    u64 x = l * 0x9e3779b97f4a7c15ull ^ ((u64)gen << 32 | 0x5a5a);
    for (unsigned i = 0; i < 4096; i += 8) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; memcpy(out + i, &x, 8); }
    memcpy(out, &l, 8); memcpy(out + 8, &gen, 4);
}
static u16 io_cmd(u8 op, u64 lba, u32 n, u64 buf, u32 extra12)
{
    struct vnvme_command c = {.opcode = op, .nsid = 1, .prp1 = buf,
                              .cdw10 = (u32)lba, .cdw11 = (u32)(lba >> 32), .cdw12 = (n - 1) | extra12};
    if (n == 2) c.prp2 = buf + 4096;
    if (n > 2) {
        for (u32 i = 1; i < n; i++) { u64 e = buf + 4096ULL * i; memcpy(G(PRPL) + 8 * (i - 1), &e, 8); }
        c.prp2 = PRPL;
    }
    return submit(1, c);
}
static u16 guest_write(u64 lba, u32 n, u32 gen, u32 extra12)
{
    for (u32 i = 0; i < n; i++) gen_block(lba + i, gen, G(WDATA) + 4096ULL * i);
    return io_cmd(VNVME_IO_WRITE, lba, n, WDATA, extra12);
}
static void expect_guest(u64 lba, u32 n, u32 gen)
{
    CHECK(io_cmd(VNVME_IO_READ, lba, n, RDATA, 0) == 0);
    u8 b[4096];
    for (u32 i = 0; i < n; i++) { gen_block(lba + i, gen, b); CHECK(!memcmp(G(RDATA) + 4096ULL * i, b, 4096)); }
}
static void expect_media(u64 lba, u32 n, u32 gen) /* durable media only */
{
    u8 a[4096], b[4096];
    for (u32 i = 0; i < n; i++) { fake_media_block(W_START + lba + i, a); gen_block(lba + i, gen, b); CHECK(!memcmp(a, b, 4096)); }
}
static void expect_media_base(u64 lba, u32 n) /* never written */
{
    u8 a[4096];
    for (u32 i = 0; i < n; i++) { fake_media_block(W_START + lba + i, a); CHECK(fake_phys_of_pattern(a) == W_START + lba + i); }
}
static void outside_untouched(void)
{
    CHECK(F.writes_outside_window == 0 && fake_overlay_inside_window());
    u8 a[4096];
    const u64 sentinels[] = {W_START - 2, W_START - 1, W_START + W_BLOCKS, W_START + W_BLOCKS + 1, 6, 244276258};
    for (unsigned i = 0; i < sizeof(sentinels) / 8; i++) {
        fake_media_block(sentinels[i], a); CHECK(fake_phys_of_pattern(a) == sentinels[i]);
        fake_disk_block(sentinels[i], a); CHECK(fake_phys_of_pattern(a) == sentinels[i]);
    }
    CHECK(F.uncleaned_writes == 0 && F.prp_errors == 0 && F.dma_violations == 0);
    CHECK(F.forbidden_ops == 0 && F.mutating_ops == 0 && F.tcb_mismatch == 0 && F.tag_fault == 0);
    const struct s1_wcanary *w = &controller.wcanary;
    CHECK(w->magic == S1WC_MAGIC && w->window_refusals == 0);
    if (F.write_blocks) {
        CHECK(w->abs_min_first == F.wmin && w->abs_max_last == F.wmax);
        CHECK(w->abs_min_first >= W_START && w->abs_max_last < W_START + W_BLOCKS);
    } else {
        CHECK(w->abs_min_first == UINT64_MAX);
    }
}
static void rw_up(struct fake_cfg g, u32 maxxfer)
{
    fresh(g);
    CHECK(select_native_w(rw_window(maxxfer)) == 0);
    CHECK(prep(W_BLOCKS) == 0);
    CHECK(wr(0x24, 0x00070007, 2)); CHECK(wr(0x28, ASQ, 3)); CHECK(wr(0x30, ACQ, 3));
    CHECK(wr(0x14, S1_CC_VALID, 2)); CHECK(controller.regs.csts == 1);
    struct vnvme_command c = {.opcode = VNVME_ADMIN_IDENTIFY, .prp1 = IDBUF, .cdw10 = 1};
    CHECK(submit(0, c) == 0);
    CHECK(G(IDBUF)[77] == 8);           /* MDTS 1 MiB */
    CHECK(G(IDBUF)[525] == 1);          /* VWC honest */
    CHECK(!memcmp(G(IDBUF) + 24, "M5W native window (read-write)", 30));
    c = (struct vnvme_command){.opcode = VNVME_ADMIN_IDENTIFY, .nsid = 1, .prp1 = IDBUF, .cdw10 = 0};
    CHECK(submit(0, c) == 0);
    u64 nsze; memcpy(&nsze, G(IDBUF), 8); CHECK(nsze == W_BLOCKS);
    CHECK(G(IDBUF)[99] == 0);           /* NSATTR: not write protected */
    c = (struct vnvme_command){.opcode = VNVME_ADMIN_CREATE_CQ, .prp1 = IOCQ, .cdw10 = (15 << 16) | 1, .cdw11 = 3};
    CHECK(submit(0, c) == 0);
    c = (struct vnvme_command){.opcode = VNVME_ADMIN_CREATE_SQ, .prp1 = IOSQ, .cdw10 = (15 << 16) | 1, .cdw11 = (1 << 16) | 1};
    CHECK(submit(0, c) == 0);
}
static void s_no_identify_rw(void)          /* r4.4: rw mode never sends Identify either; writes work */
{
    struct fake_cfg g = rw_cfg();
    rw_up(g, 0);
    CHECK(ans_ro_nsze() == F_NSZE && s1n_writable() && F.admin_ops[0x06] == 0);
    CHECK(guest_write(100, 4, 1, 0) == 0);
    CHECK(io_cmd(VNVME_IO_FLUSH, 0, 1, 0, 0) == 0);
    expect_media(100, 4, 1);
    outside_untouched();
}
static void s_rw_basic(void)
{
    rw_up(rw_cfg(), 0);
    CHECK(guest_write(0, 1, 1, 0) == 0); expect_guest(0, 1, 1);                 /* first LBA */
    CHECK(guest_write(W_BLOCKS - 1, 1, 1, 0) == 0); expect_guest(W_BLOCKS - 1, 1, 1); /* last LBA */
    CHECK(guest_write(5000, 256, 2, 0) == 0); expect_guest(5000, 256, 2);       /* 1 MiB, r4.6: 8 x 32-block ANS cmds */
    CHECK(guest_write(W_BLOCKS - 256, 256, 3, 0) == 0); expect_guest(W_BLOCKS - 256, 256, 3);
    /* Boundary refusals run in isolated r45_guest_write_* processes. */
    CHECK(F.io_writes == RW4_WRITES);                                           /* r4.6: 1 + 1 + 8 + 8 split ANS writes */
    /* DSM / Write Zeroes stay rejected, never reach ANS. */
    struct vnvme_command c = {.opcode = 0x09, .nsid = 1, .prp1 = WDATA};
    CHECK(submit(1, c) == VNVME_SC_INVALID_OPCODE);
    c = (struct vnvme_command){.opcode = 0x08, .nsid = 1};
    CHECK(submit(1, c) == VNVME_SC_INVALID_OPCODE);
    c = (struct vnvme_command){.opcode = VNVME_IO_WRITE, .nsid = 2, .prp1 = WDATA};
    CHECK(submit(1, c) == VNVME_SC_INVALID_NAMESPACE);
    CHECK(F.io_writes == RW4_WRITES);
    outside_untouched();
    /* Nothing durable before FLUSH; all durable after. */
    expect_media_base(5000, 4);
    CHECK(io_cmd(VNVME_IO_FLUSH, 0, 1, 0, 0) == 0 && F.io_flushes == 1);
    expect_media(5000, 256, 2); expect_media(0, 1, 1); expect_media(W_BLOCKS - 256, 256, 3);
    printf("{\"window_refusals\":%lu}\n", (unsigned long)controller.wcanary.window_refusals);
}
static void s_rw_power_loss(void)
{
    rw_up(rw_cfg(), 0);
    CHECK(guest_write(100, 8, 1, 0) == 0);
    CHECK(io_cmd(VNVME_IO_FLUSH, 0, 1, 0, 0) == 0);
    CHECK(guest_write(100, 8, 2, 0) == 0);           /* overwrite, not flushed */
    CHECK(guest_write(200, 8, 2, 0) == 0);
    fake_power_loss();                               /* watchdog reset / power cut mid-run */
    expect_media(100, 8, 1);                         /* last flushed generation survives */
    expect_media_base(200, 8);                       /* unflushed new blocks are lost */
    outside_untouched();                             /* only window blocks can ever differ */
}
static void s_rw_fua(void)
{
    rw_up(rw_cfg(), 0);
    unsigned f = F.io_flushes;
    CHECK(guest_write(300, 4, 1, 1u << 30) == 0);    /* FUA */
    CHECK(F.io_flushes == f + 1);                    /* write then flush */
    fake_power_loss();
    expect_media(300, 4, 1);
    CHECK(controller.queue.fua_flushes == 1 && controller.wcanary.fua_flushes == 1);
    outside_untouched();
}
static void s_rw_write_through(void)
{
    rw_up(rw_cfg(), 0);
    struct vnvme_command c = {.opcode = VNVME_ADMIN_SET_FEATURES, .cdw10 = 0x06, .cdw11 = 0};
    CHECK(submit(0, c) == 0);                        /* WCE = 0 */
    c = (struct vnvme_command){.opcode = VNVME_ADMIN_GET_FEATURES, .cdw10 = 0x06};
    CHECK(submit(0, c) == 0);
    unsigned f = F.io_flushes;
    CHECK(guest_write(400, 2, 1, 0) == 0);
    CHECK(F.io_flushes == f + 1);
    fake_power_loss();
    expect_media(400, 2, 1);
}
static void s_rw_guest_shutdown(void)
{
    struct fake_cfg g = rw_cfg();
    g.shn_no_persist = true;
    rw_up(g, 0);
    CHECK(guest_write(500, 16, 1, 0) == 0);
    CHECK(wr(0x14, S1_CC_VALID | 0x4000, 2));        /* guest normal shutdown */
    CHECK((controller.regs.csts & 0xc) == 0x8 && !(controller.regs.csts & 2));
    CHECK(controller.wcanary.shutdown_flushes == 1);
    fake_power_loss();                               /* even if the platform dies right after */
    expect_media(500, 16, 1);
}
static void s_rw_quiesce_flush(void)
{
    struct fake_cfg g = rw_cfg();
    g.shn_no_persist = true;                         /* the driver's own FLUSH must persist */
    rw_up(g, 0);
    CHECK(guest_write(600, 3, 1, 0) == 0);
    hv_win_storage_retire();                         /* hv exit: quiesce */
    stopped_like_stage1();
    fake_power_loss();
    expect_media(600, 3, 1);
    outside_untouched();
}
static void s_rw_chunked(void)
{
    rw_up(rw_cfg(), 16);                              /* contract caps ANS commands at 16 blocks */
    CHECK(guest_write(7000, 256, 4, 0) == 0);
    CHECK(F.io_writes == 16);
    expect_guest(7000, 256, 4);
    outside_untouched();
}
static void s_rw_write_error(void)
{
    rw_up(rw_cfg(), 0);
    F.cfg.write_error = true;
    CHECK(guest_write(800, 2, 1, 0) == VNVME_SC_INTERNAL);
    CHECK(ans_ro_state() == ANS_RO_FROZEN && ans_ro_stats()->reason == ANS_RO_E_STATUS);
    unsigned nio = F.nio;
    F.cfg.write_error = false;
    CHECK(guest_write(800, 2, 2, 0) == VNVME_SC_INTERNAL);
    CHECK(F.nio == nio);

}
static void s_rw_slow_flush(void)
{
    struct fake_cfg g = rw_cfg();
    g.flush_delay = 1000000;          /* ~2-3 s of virtual time: above 1 s, below the 10 s bound */
    rw_up(g, 0);
    CHECK(guest_write(900, 4, 1, 0) == 0);
    u64 t0 = F.now;
    CHECK(io_cmd(VNVME_IO_FLUSH, 0, 1, 0, 0) == 0);
    CHECK(F.now - t0 > 1000000 && F.now - t0 < 10000000);
    CHECK(ans_ro_state() == ANS_RO_LIVE);
    fake_power_loss();
    expect_media(900, 4, 1);
}
static void s_rw_defense(int which)
{
    /* Direct backend calls bypass the frontend bounds; the window checks must still hold. */
    rw_up(rw_cfg(), 0);
    static u8 buf[256 * 4096];
    unsigned w = F.io_writes;
    if (which == 0) CHECK(!s1n_write(W_BLOCKS - 1, 2, buf));
    if (which == 1) CHECK(!s1n_write(W_BLOCKS, 1, buf));
    if (which == 2) CHECK(!s1n_write(UINT64_MAX, 2, buf));
    if (which == 3) CHECK(!s1n_write(UINT64_MAX - 1, 256, buf));
    if (which == 4) CHECK(!s1n_write(0, 0, buf));
    if (which == 5) CHECK(!s1n_write(0, 257, buf));
    if (which == 6) CHECK(!ans_ro_xfer(true, W_START - 1, 1, 0));                 /* driver: below window */
    if (which == 7) CHECK(!ans_ro_xfer(true, W_START + W_BLOCKS - 1, 2, 0));      /* driver: straddles end */
    if (which == 8) CHECK(!ans_ro_xfer(true, W_START, 257, 0));
    if (which == 9) CHECK(!ans_ro_xfer(true, W_START, 1, 4097));
    CHECK(F.io_writes == w && F.writes_outside_window == 0);
    printf("{\"window_refusals\":%lu}\n", (unsigned long)ans_ro_stats()->window_refusals);
}
static void s_rw_requires_flag(u8 flag_op)
{
    /* Without S1N_RW: read-only exactly as r3.1, even with a writable fake. */
    fresh(rw_cfg());
    CHECK(select_native() == 0);
    CHECK(!s1n_writable());
    /* r4.6 (C9/L-1): the refused opcode is terminal (-2, NOT_ALLOWED); FLUSH runs in rw_requires_flag_flush. */
    CHECK(ans_ro_test_submit(false, flag_op) == -2);
    CHECK(ans_ro_state() == ANS_RO_FROZEN && ans_ro_stats()->reason == ANS_RO_E_NOT_ALLOWED);
    CHECK(!s1n_write(0, 1, G(WDATA)) && !s1n_flush());
    CHECK(F.io_writes == 0 && F.io_flushes == 0);
    printf("{\"window_refusals\":%lu}\n", (unsigned long)ans_ro_stats()->window_refusals);
}
static void s_rw_requires_gpt(void)
{
    struct fake_cfg g = rw_cfg();
    g.gpt_mutation = GM_GUID;
    fresh(g);
    CHECK(select_native_w(rw_window(0)) == -1);
    exposed_nothing();
    CHECK(F.io_writes == 0);
}
/* Randomized read/write/flush workload with a shadow model; byte-exact verification. */
#define SH (1u << 15)
static struct { u64 l; u32 gen; u8 used; } shadow[SH];
static u32 *sh_find(u64 l, bool create)
{
    u64 h = (l * 0x9e3779b97f4a7c15ull) >> 49;
    for (unsigned i = 0; i < SH; i++) {
        unsigned k = (h + i) & (SH - 1);
        if (shadow[k].used && shadow[k].l == l) return &shadow[k].gen;
        if (!shadow[k].used) { if (!create) return NULL; shadow[k].used = 1; shadow[k].l = l; shadow[k].gen = 0; return &shadow[k].gen; }
    }
    abort();
}
static u64 rng = 0x1234567;
static u64 rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static void s_rw_random(void)
{
    rw_up(rw_cfg(), 0);
    u32 gen = 1;
    for (unsigned op = 0; op < 600; op++) {
        u64 r = rnd();
        u32 n = 1 + (u32)(r % 4 == 0 ? rnd() % 256 : rnd() % 16);
        u64 lba;
        switch (rnd() % 4) {
        case 0: lba = rnd() % 4096; break;                                 /* hot head */
        case 1: lba = W_BLOCKS - 1 - rnd() % 4096; break;                  /* hot tail (may straddle) */
        case 2: lba = rnd() % W_BLOCKS; n = 1 + (u32)(rnd() % 4); break;   /* anywhere, small */
        default: lba = W_BLOCKS - n + (rnd() % 3); break;                  /* edge: fits or straddles */
        }
        /* Invalid random shapes have their own fresh-process scenarios. */
        if (lba >= W_BLOCKS || n > W_BLOCKS - lba) lba = W_BLOCKS - n;
        bool fits = true;
        unsigned k = (unsigned)(rnd() % 10);
        if (k < 5) {
            u32 g = ++gen;
            u16 st = guest_write(lba, n, g, (rnd() % 8 == 0) ? (1u << 30) : 0);
            CHECK(st == (fits ? 0 : VNVME_SC_INVALID_FIELD));
            if (fits) for (u32 i = 0; i < n; i++) *sh_find(lba + i, true) = g;
        } else if (k < 9) {
            if (!fits) { CHECK(io_cmd(VNVME_IO_READ, lba, n, RDATA, 0) == VNVME_SC_LBA_OUT_OF_RANGE); continue; }
            CHECK(io_cmd(VNVME_IO_READ, lba, n, RDATA, 0) == 0);
            u8 b[4096];
            for (u32 i = 0; i < n; i++) {
                u32 *g = sh_find(lba + i, false);
                const u8 *got = G(RDATA) + 4096ULL * i;
                if (g) { gen_block(lba + i, *g, b); CHECK(!memcmp(got, b, 4096)); }
                else CHECK(fake_phys_of_pattern(got) == W_START + lba + i);
            }
        } else {
            CHECK(io_cmd(VNVME_IO_FLUSH, 0, 1, 0, 0) == 0);
        }
        if (op % 50 == 0) hv_win_storage_tick();
    }
    CHECK(io_cmd(VNVME_IO_FLUSH, 0, 1, 0, 0) == 0);
    /* Every shadowed block is durable and byte-exact; nothing outside the window changed. */
    for (unsigned k = 0; k < SH; k++)
        if (shadow[k].used) expect_media(shadow[k].l, 1, shadow[k].gen);
    CHECK(fake_cached_blocks() == 0);
    outside_untouched();
    CHECK(ans_ro_state() == ANS_RO_LIVE && controller.wcanary.write_cmds > 0);
    printf("{\"random_ops\":600,\"durable_blocks\":%u,\"ans_writes\":%u}\n", fake_durable_blocks(), F.io_writes);
}

/* ---------------------------------------------------------- r4.3: alignment / PRP / DMA confinement */
static void e2e_reads_policy(int model)
{
    struct fake_cfg g = base_cfg();
    g.prp_model = model;
    fresh(g);
    CHECK(select_native() == 0);
    CHECK(F.dma_outside == 0 && F.dma_into_queue == 0 && F.dma_violations == 0);
    CHECK(prep(W_BLOCKS) == 0);
    guest_bringup();
    CHECK(guest_read(0, 1, DATA) == 0); check_blocks(0, 1, DATA);
    CHECK(guest_read(W_BLOCKS - 1, 1, DATA) == 0); check_blocks(W_BLOCKS - 1, 1, DATA);
    CHECK(guest_read(123456, 32, DATA) == 0); check_blocks(123456, 32, DATA);
    live_policy();
    hv_win_storage_retire();
    stopped_like_stage1();
}
static void rw_policy(int model)           /* rw: 1/2/3/256-block commands (PRP1, PRP2 page, PRP list) */
{
    struct fake_cfg g = rw_cfg();
    g.prp_model = model;
    rw_up(g, 0);
    const u32 ns[] = {1, 2, 3, 17, 256};
    for (unsigned i = 0; i < 5; i++) {
        CHECK(guest_write(7000 + 300 * i, ns[i], 4 + i, 0) == 0);
        expect_guest(7000 + 300 * i, ns[i], 4 + i);
    }
    CHECK(io_cmd(VNVME_IO_FLUSH, 0, 1, 0, 0) == 0);
    for (unsigned i = 0; i < 5; i++) expect_media(7000 + 300 * i, ns[i], 4 + i);
    outside_untouched();
    live_policy();
}
static u64 rd_extent_lo, rd_extent_hi, plist_lo;
static bool stray_allowed(u64 a)
{
    if (a >= rd_extent_lo && a < rd_extent_hi) return true;               /* the READs' PRP extents */
    if (plist_lo && a >= plist_lo && a < plist_lo + 4096) return true;    /* PRP list page (CPU-written) */
#ifdef ANS_RO_IO_TAG
    const u64 sl = ANS_RO_IO_TAG;   /* r4.4: the I/O tag's SQE/TCB slot */
#else
    const u64 sl = 0;
#endif
    if (a >= F.iosq_reg + sl * 64 && a < F.iosq_reg + (sl + 2) * 64) return true;    /* I/O SQE slot (CPU-written) */
    if (a >= F.iosq_tcb + sl * 128 && a < F.iosq_tcb + (sl + 2) * 128) return true; /* I/O TCB slot (CPU-written) */
    if (a >= F.io_cq_base && a < F.io_cq_base + (u64)F.io_cq_size * 16) return true; /* I/O CQ ring */
    return false;
}
/* No byte outside the resident-owned read pages (plus PRP list, I/O SQE/TCB slot 0, I/O CQ ring)
 * changes in the resident DMA region during reads; no DMA outside resident memory; guest memory
 * beyond the destination untouched. A byte-for-byte diff of the whole region around each read. */
static void s_no_stray_writes(int model, bool rw)
{
    struct fake_cfg g = rw ? rw_cfg() : base_cfg();
    g.prp_model = model;
    u64 dest = rw ? RDATA : DATA;
    if (rw) rw_up(g, 0);
    else { fresh(g); CHECK(select_native() == 0); CHECK(prep(W_BLOCKS) == 0); guest_bringup(); }
    CHECK(F.dma_outside == 0 && F.dma_into_queue == 0 && F.dma_violations == 0);
    u64 rb, rs, sz;
    const char *nm;
    ans_ro_dma_region(&rb, &rs);
#ifdef ANS_RO_HAVE_DMA_GUARD
    CHECK(ans_ro_test_dma_buffer(1, &plist_lo, &sz, &nm));
#else
    (void)sz; (void)nm; plist_lo = 0;
#endif
    u8 *before = malloc(rs);
    CHECK(before != NULL);
    const struct { u64 lba; u32 n; } rd[] = {{0, 1}, {1, 1}, {5, 2}, {W_BLOCKS - 1, 1}, {777777, 3}, {12345, 32}, {4000, 256}};
    unsigned nrd = rw ? 7 : 6;
    for (unsigned k = 0; k < nrd; k++) {
        memset(G(dest), 0x5a, 257 * 4096);
        memcpy(before, (void *)rb, rs);
        F.rd_lo = F.rd_hi = 0;
        unsigned n0 = F.io_reads;
        CHECK(io_cmd(VNVME_IO_READ, rd[k].lba, rd[k].n, dest, 0) == 0);
        if (!rw) check_blocks(rd[k].lba, rd[k].n, dest);
        CHECK(F.io_reads > n0 && F.rd_lo && F.rd_hi > F.rd_lo);
        rd_extent_lo = F.rd_lo; rd_extent_hi = F.rd_hi;
        CHECK(rd_extent_lo >= rb && rd_extent_hi <= rb + rs);
        CHECK(!fake_overlaps_queue(rd_extent_lo, rd_extent_hi - rd_extent_lo));
        for (u64 i = 0; i < rs; i++)
            if (before[i] != ((const u8 *)rb)[i]) CHECK(stray_allowed(rb + i));
        for (u64 i = (u64)rd[k].n * 4096; i < 257 * 4096; i++) CHECK(G(dest)[i] == 0x5a);
    }
    free(before);
    CHECK(F.dma_outside == 0 && F.dma_into_queue == 0 && F.dma_violations == 0);
    for (unsigned i = 0; i < F.ndma; i++) CHECK(fake_in_arena(F.dma_addrs[i], 1));
}
/* ------------------------------------------- r4.4 (= r3.5): RTKit handshake timing (cross-check D1) */
#define MS 1000ULL
#ifdef ANS_RO_HAVE_R44
#define STAT_RTKIT 23
#define STAT_ALIAS 24
#else
#define STAT_RTKIT 15
#define STAT_ALIAS 16
#endif
static void s_rtkit_slow(u64 hello, u64 epmap, u64 power, bool rw)
{
    struct fake_cfg g = rw ? rw_cfg() : base_cfg();
    g.hello_delay_us = hello;
    g.epmap_delay_us = epmap;
    g.power_on_delay_us = power;
    if (rw) {
        rw_up(g, 0);                        /* asserts select == 0 */
        CHECK(guest_write(4242, 2, 1, 0) == 0);
        CHECK(io_cmd(VNVME_IO_FLUSH, 0, 1, 0, 0) == 0);
        expect_media(4242, 2, 1);
        outside_untouched();
    } else {
        fresh(g);
        CHECK(select_native() == 0);        /* r4.3 froze here (E_RTKIT) when a wait exceeded 1 s/5 s */
        CHECK(prep(W_BLOCKS) == 0);
        guest_bringup();
        CHECK(guest_read(4242, 1, DATA) == 0); check_blocks(4242, 1, DATA);
    }
    CHECK(ans_ro_state() == ANS_RO_LIVE && F.hellos == 1 && s1n_verified());
#ifdef ANS_RO_HAVE_R35
    CHECK(ans_ro_stats()->rtkit_timeouts == 0 && hv_win_storage_native_stat(STAT_RTKIT) == 0);
#endif
    live_policy();
    hv_win_storage_retire();
    stopped_like_stage1();
}
static void select_void(void) { CHECK(select_native() == -1); }
static void s_rtkit_timeout(int which)
{
    struct fake_cfg g = base_cfg();
    u32 want = 0;
    if (which == 0) { g.no_hello = true; want = 1u << 0; }
    if (which == 1) { g.no_epmap = true; want = 1u << 1; }
    if (which == 2) { g.no_power_on_ack = true; want = 1u << 2; }
    if (which == 3) { g.hello_delay_us = 11000 * MS; want = 1u << 0; }
    if (which == 4) { g.power_on_delay_us = 11000 * MS; want = 1u << 2; }
    fresh(g);
    CHECK(fake_run_bounded(select_void) == 0);   /* bounded: no hang */
    CHECK(ans_ro_state() == ANS_RO_FROZEN && ans_ro_stats()->reason == ANS_RO_E_RTKIT);
    CHECK(hv_win_storage_native_stat(2) == (((u64)ANS_RO_FROZEN << 32) | ANS_RO_E_RTKIT));
    CHECK(ans_ro_stats()->commands == 0);
    CHECK(F.lsq_ctrl_access == 0 && F.ioqa == 0 && F.num_tcbs == 0 && F.enables == 0 && !F.asq && !F.aqa);
    frozen_untouched();
#ifdef ANS_RO_HAVE_R35
    CHECK(ans_ro_stats()->rtkit_timeouts == want && hv_win_storage_native_stat(STAT_RTKIT) == want);
#else
    (void)want;
    fprintf(stderr, "FAIL: build records no RTKit which-wait marker\n"); exit(1);
#endif
}
static void s_tcb_alias_nonzero(void)
{
    struct fake_cfg g = rw_cfg();
    g.tcb_alias_stat = 0x5a;
    rw_up(g, 0);                            /* r4.3 froze (E_TCB) at the first completion */
    CHECK(guest_write(77, 1, 1, 0) == 0);
    CHECK(guest_read(77, 1, DATA) == 0);
    CHECK(ans_ro_state() == ANS_RO_LIVE);
#ifdef ANS_RO_HAVE_R35
    const struct ans_ro_stats *a = ans_ro_stats();
    CHECK(a->tcb_alias_last == 0x5a && a->tcb_alias_nonzero == a->commands);
    CHECK(hv_win_storage_native_stat(STAT_ALIAS) == (((u64)a->commands << 32) | 0x5a));
#endif
    live_policy();
    hv_win_storage_retire();
    stopped_like_stage1();
}
static void s_tcb_linux_nonzero(void)
{
    rw_up(rw_cfg(), 0);
    F.cfg.tcb_stat_fail = true;
    CHECK(guest_write(9, 1, 1, 0) == VNVME_SC_INTERNAL);
    CHECK(ans_ro_stats()->reason == ANS_RO_E_TCB);
    frozen_untouched();
}
static void s_cc_cq_shape(void)
{
    fresh(base_cfg());
    CHECK(F.cc == 0x474000);
    CHECK(select_native() == 0);
    CHECK(F.cc == 0x470001);
    CHECK(F.io_cq_valid);
    live_policy();
}
/* ------------------------------------------- r4.4: write-path items (cross-check §4 items 5-7) */
static void s_io_tag_2(void)          /* Mu core: admin tag 0, I/O tag = admin depth (2) */
{
    rw_up(rw_cfg(), 0);
    CHECK(guest_write(500, 3, 1, 0) == 0);
    CHECK(guest_read(500, 3, RDATA) == 0);
    CHECK(io_cmd(VNVME_IO_FLUSH, 0, 1, 0, 0) == 0);
    CHECK(F.io_writes > 0 && F.io_flushes > 0 && F.io_reads > 0);
#ifdef ANS_RO_HAVE_R44
    CHECK(F.io_tag_seen[ANS_RO_IO_TAG] > 0 && F.io_tag_seen[ANS_RO_IO_TAG_ALT] > 0 && F.io_tag_seen[ANS_RO_IO_TAG] + F.io_tag_seen[ANS_RO_IO_TAG_ALT] == F.nio && F.io_tag_seen[0] == 0 && F.tag_policy == 0);
#else
    fprintf(stderr, "FAIL: build issues I/O on NVMMU tag 0 (no r4.4 tag 2)\n"); exit(1);
#endif
    live_policy();
}
static void s_fua_is_flush(void)      /* Linux BROKEN_FUA: guest FUA -> ANS WRITE (no FUA) + FLUSH */
{
    rw_up(rw_cfg(), 0);
    unsigned f = F.io_flushes, w = F.io_writes;
    CHECK(guest_write(300, 4, 1, 1u << 30) == 0);
    CHECK(F.io_writes == w + 1 && F.io_flushes == f + 1 && F.fua_writes == 0);
    fake_power_loss();
    expect_media(300, 4, 1);
    live_policy();
}
static void s_teardown_ioq_regs(int which)  /* aura nvme_shutdown: zero +0x1200 then +0x1208 */
{
    if (which == 0) {                       /* rw session, clean quiesce */
        rw_up(rw_cfg(), 0);
        CHECK(guest_write(42, 1, 1, 0) == 0);
        CHECK(F.iosq_reg && F.iocq_reg);
        CHECK(hv_win_storage_native_quiesce() == 0);
    } else {                                /* GPT refused after LIVE: same quiesce */
        struct fake_cfg g = base_cfg();
        g.gpt_mutation = GM_GUID;
        fresh(g);
        CHECK(select_native() == -1);
    }
#ifdef ANS_RO_HAVE_R44
    if (which == 0) stopped_like_stage1();
    else { frozen_untouched(); CHECK(ans_ro_stats()->ioq_regs_cleared == 0); }
#else
    fprintf(stderr, "FAIL: build leaves +0x1200/+0x1208 programmed at teardown (no r4.4 clear)\n"); exit(1);
#endif
}

#ifdef ANS_RO_HAVE_DMA_GUARD
static void s_m1n1_layout(void)
{
    rw_up(rw_cfg(), 0);
    CHECK(guest_write(42, 3, 1, 0) == 0);
    expect_guest(42, 3, 1);
    const u64 want[9] = {1u << 20, 4096, 8192, 8192, 4096, 4096, 1024, 1024, 4096};
    u64 a[9], s[9], rb, rs;
    const char *nm;
    ans_ro_dma_region(&rb, &rs);
    for (unsigned i = 0; i < 9; i++) {
        CHECK(ans_ro_test_dma_buffer(i, &a[i], &s[i], &nm));
        CHECK(!(a[i] & 0x3fff) && s[i] == want[i]);
        CHECK(a[i] >= rb && a[i] + s[i] <= rb + rs);
        for (unsigned j = 0; j < i; j++) CHECK(a[i] + s[i] <= a[j] || a[j] + s[j] <= a[i]);   /* disjoint */
        if (i >= 1) for (unsigned j = 1; j < i; j++) CHECK((a[i] >> 14) != (a[j] >> 14));     /* own 16 KiB slot */
    }
    CHECK(!ans_ro_test_dma_buffer(9, &a[0], &s[0], &nm));
    CHECK(F.asq_tcb == a[2] && F.iosq_tcb == a[3] && F.asq == a[4] && F.iosq_reg == a[5]);
    CHECK(F.acq == a[6] && F.io_cq_base == a[7] && F.iocq_reg == a[7]);
    CHECK(F.last_read_prp1 >= a[0] && F.last_read_prp1 + F.last_read_bytes <= a[0] + s[0] && !(F.last_read_prp1 & 0xfff));
    CHECK(F.last_read_prp2 == a[1]);                 /* 3-block read: PRP2 = the private list page */
    live_policy();
}
static void guard_case(int which)
{
    rw_up(rw_cfg(), 0);
    u64 b[9], s, rb, rs;
    const char *nm;
    for (unsigned i = 0; i < 9; i++) CHECK(ans_ro_test_dma_buffer(i, &b[i], &s, &nm));
    ans_ro_dma_region(&rb, &rs);
    unsigned na = F.nadmin, ni = F.nio, w = mmio_writes();
    u32 lo = (u32)W_START, hi = (u32)(W_START >> 32);
    int rc = 0;
    switch (which) {
    case 0: rc = ans_ro_test_submit_raw(false, 0x02, b[0] + 0x800, 0, lo, hi, 0); break;            /* page offset */
    case 1: rc = ans_ro_test_submit_raw(false, 0x02, b[0], b[0] + 4096, lo, hi, 0); break;          /* PRP2 on 1 block */
    case 2: rc = ans_ro_test_submit_raw(false, 0x02, b[0], b[0] + 0x2000, lo, hi, 1); break;        /* 2 blocks, wrong PRP2 */
    case 3: rc = ans_ro_test_submit_raw(false, 0x02, b[0], 0, lo, hi, 1); break;                    /* 2 blocks, PRP2 = 0 */
    case 4: rc = ans_ro_test_submit_raw(false, 0x02, b[0], b[0] + 0x1000, lo, hi, 2); break;        /* 3 blocks, PRP2 not the list */
    case 5: rc = ans_ro_test_submit_raw(false, 0x02, b[7], 0, lo, hi, 0); break;                    /* I/O CQ as data */
    case 6: rc = ans_ro_test_submit_raw(false, 0x02, (u64)G(RDATA), 0, lo, hi, 0); break;           /* guest memory */
    case 7: rc = ans_ro_test_submit_raw(false, 0x02, b[0] + (1u << 20) - 4096, 0, lo, hi, 1); break; /* runs past xfer */
    case 8: rc = ans_ro_test_submit_raw(false, 0x01, b[0] + 0x800, 0, lo, hi, 0); break;            /* WRITE, page offset */
    case 9: rc = ans_ro_test_submit_raw(true, 0x05, b[7] + 0x400, 0, 1 | (63u << 16), 1, 0); break; /* r4.2 I/O CQ */
    case 10: rc = ans_ro_test_submit_raw(true, 0x01, b[5] + 0x1000, 0, 1 | (63u << 16), 1 | (1u << 16), 0); break;
    case 11: rc = ans_ro_test_submit_raw(true, 0x04, b[8], 0, 1, 0, 0); break;                      /* Delete CQ with a PRP */
    case 12: rc = ans_ro_test_submit_raw(false, 0x00, b[0], 0, 0, 0, 0); break;                     /* FLUSH with a PRP */
    case 13: /* the private list already holds the right entries, but PRP2 points elsewhere */
        CHECK(io_cmd(VNVME_IO_READ, 0, 3, RDATA, 0) == 0);
        CHECK(F.last_read_prp2 == b[1]);
        na = F.nadmin; ni = F.nio; w = mmio_writes();
        rc = ans_ro_test_submit_raw(false, 0x02, F.last_read_prp1, F.last_read_prp1 + 0x3000, lo, hi, 2);
        break;
    }
    CHECK(rc == -2);
    CHECK(ans_ro_state() == ANS_RO_FROZEN && ans_ro_stats()->reason == ANS_RO_E_DMA_GUARD);
    CHECK(F.nadmin == na && F.nio == ni);
    CHECK(mmio_writes() == w);
    hv_win_storage_retire();
    frozen_untouched();
    CHECK(mmio_writes() == w);
}
#else
static void s_m1n1_layout(void) { fprintf(stderr, "FAIL: build has no r4.3 DMA guard / per-buffer layout\n"); exit(1); }
static void guard_case(int which) { (void)which; fprintf(stderr, "FAIL: build has no r4.3 DMA guard\n"); exit(1); }
#endif

#include "r45_cases.h"
#include "secA_cases.h"
#include "r46_cases.h"
#include "chk_cases.h"

/* smp-storage: final state for the 1-vCPU trace comparison (TRACE_1CPU, trace_1cpu.py); never installed otherwise */
static u64 trace_bytes(const void *p, size_t n)
{
    u64 h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ ((const u8 *)p)[i]) * 0x100000001b3ull;
    return h;
}
static void trace_final(void)
{
    if (!trace_f) return;
    const struct vnvme_ctrl *q = &controller.queue;
    fprintf(trace_f, "END guest %llx diag %llx wcanary %llx stats %llx prp %llx/%llx/%llx dup %llu/%llu/%llu csts %x cc %x intms %x "
            "irq %d/%llu faulted %d ready %d disp %llu rej %llu lease %llu | ans io r%u w%u f%u admin %llx io %llx outside %u durable %u cached %u state %d\n",
            (unsigned long long)trace_guest_hash(), (unsigned long long)trace_bytes(&controller.diag, sizeof(controller.diag)),
            (unsigned long long)trace_bytes(&controller.wcanary, sizeof(controller.wcanary)),
            (unsigned long long)trace_bytes(&q->stats, sizeof(q->stats)),
            (unsigned long long)trace_bytes(q->prp_refusals, sizeof(q->prp_refusals)), (unsigned long long)q->prp_refusal_total,
            (unsigned long long)trace_bytes(q->prp_log, sizeof(q->prp_log)), (unsigned long long)q->prp_dup_dest_reads,
            (unsigned long long)q->prp_part_dest_reads, (unsigned long long)q->prp_dup_src_writes, controller.regs.csts,
            controller.regs.cc, controller.regs.intms, irq_level, (unsigned long long)controller.irq_changes, controller.faulted,
            ready, (unsigned long long)dispatches, (unsigned long long)rejections, (unsigned long long)lease_failures,
            F.io_reads, F.io_writes, F.io_flushes, (unsigned long long)trace_bytes(F.admin_ops, sizeof(F.admin_ops)),
            (unsigned long long)trace_bytes(F.io_ops, sizeof(F.io_ops)), F.writes_outside_window, fake_durable_blocks(),
            fake_cached_blocks(), (int)ans_ro_state());
    fclose(trace_f);
    trace_f = NULL;
}
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    if (getenv("TRACE_1CPU")) atexit(trace_final);
    const char *s = argv[1];
    if (!strcmp(s, "happy")) s_happy();
    else if (!strcmp(s, "prep_blocks_mismatch")) s_e2e();
    else if (!strcmp(s, "e2e_reads")) s_e2e_reads();
    else if (!strcmp(s, "e2e_reads_ro_write")) s_e2e_reads_ro_write();
    else if (!strncmp(s, "gpt_", 4)) s_gpt(atoi(s + 4));
    else if (!strcmp(s, "window_args")) s_window_args();
    else if (!strcmp(s, "window_args_type")) s_window_args_type();
    else if (!strcmp(s, "refuse_running")) s_refuse_running();
    else if (!strcmp(s, "refuse_sart")) s_refuse_sart();
    else if (!strcmp(s, "aic_already_routed")) s_aic_already_routed();
    else if (!strncmp(s, "freeze_init_", 12)) s_freeze_init(atoi(s + 12));
    else if (!strncmp(s, "freeze_runtime_", 15)) s_freeze_runtime(atoi(s + 15));
    else if (!strcmp(s, "liveness")) s_liveness();
    else if (!strcmp(s, "allowlist")) s_allowlist();
    else if (!strncmp(s, "allowlist_io_", 13)) s_allowlist_one(false, (unsigned)atoi(s + 13));
    else if (!strncmp(s, "allowlist_adm_", 14)) s_allowlist_one(true, (unsigned)atoi(s + 14));
    else if (!strcmp(s, "window_direct")) s_window_direct(0);
    else if (!strcmp(s, "fixture_default")) s_fixture_default();
    else if (!strcmp(s, "late_select")) s_late_select();
    else if (!strcmp(s, "rw_basic")) s_rw_basic();
    else if (!strcmp(s, "rw_power_loss")) s_rw_power_loss();
    else if (!strcmp(s, "rw_fua")) s_rw_fua();
    else if (!strcmp(s, "rw_write_through")) s_rw_write_through();
    else if (!strcmp(s, "rw_guest_shutdown")) s_rw_guest_shutdown();
    else if (!strcmp(s, "rw_quiesce_flush")) s_rw_quiesce_flush();
    else if (!strcmp(s, "rw_chunked")) s_rw_chunked();
    else if (!strcmp(s, "rw_write_error")) s_rw_write_error();
    else if (!strcmp(s, "rw_defense")) s_rw_defense(0);
    else if (!strcmp(s, "rw_slow_flush")) s_rw_slow_flush();
    else if (!strcmp(s, "rw_requires_flag")) s_rw_requires_flag(0x01);
    else if (!strcmp(s, "rw_requires_flag_flush")) s_rw_requires_flag(0x00);
    else if (!strcmp(s, "rw_requires_gpt")) s_rw_requires_gpt();
    else if (!strcmp(s, "rw_random")) s_rw_random();
    else if (!strcmp(s, "scrub_after_gpt_reject")) s_gpt_reject_scrub();
    else if (!strcmp(s, "select_no_prep")) s_select_no_prep();
    else if (!strcmp(s, "quiesce_op")) s_quiesce_op();
    else if (!strcmp(s, "quiesce_after_prep")) s_quiesce_after_prep();
    else if (!strcmp(s, "stale_bounce")) s_stale_bounce();
    else if (!strcmp(s, "hello_slow")) s_hello_slow();
    else if (!strcmp(s, "no_identify_rw")) s_no_identify_rw();
    else if (!strcmp(s, "rtkit_handshake_3s")) s_rtkit_slow(300 * MS, 3000 * MS, 3000 * MS, false);
    else if (!strcmp(s, "rtkit_handshake_7s")) s_rtkit_slow(7000 * MS, 7000 * MS, 7000 * MS, false);
    else if (!strcmp(s, "rtkit_handshake_9s_rw")) s_rtkit_slow(9500 * MS, 9500 * MS, 9500 * MS, true);
    else if (!strncmp(s, "rtkit_timeout_", 14)) s_rtkit_timeout(atoi(s + 14));
    else if (!strcmp(s, "tcb_alias_nonzero")) s_tcb_alias_nonzero();
    else if (!strcmp(s, "tcb_linux_nonzero")) s_tcb_linux_nonzero();
    else if (!strcmp(s, "cc_cq_shape")) s_cc_cq_shape();
    else if (!strcmp(s, "io_tag_2")) s_io_tag_2();
    else if (!strcmp(s, "fua_is_flush")) s_fua_is_flush();
    else if (!strncmp(s, "teardown_ioq_regs_", 18)) s_teardown_ioq_regs(atoi(s + 18));
    else if (!strcmp(s, "no_identify_issued")) s_no_identify_issued();
    else if (!strcmp(s, "capacity_mismatch")) s_capacity_mismatch();
    else if (!strcmp(s, "prp_spec_e2e")) e2e_reads_policy(PRP_SPEC);
    else if (!strcmp(s, "prp_mask_e2e")) e2e_reads_policy(PRP_MASK);
    else if (!strcmp(s, "prp_strict_rw")) rw_policy(PRP_STRICT);
    else if (!strcmp(s, "prp_spec_rw")) rw_policy(PRP_SPEC);
    else if (!strcmp(s, "prp_mask_rw")) rw_policy(PRP_MASK);
    else if (!strcmp(s, "no_stray_writes_strict")) s_no_stray_writes(PRP_STRICT, false);
    else if (!strcmp(s, "no_stray_writes_spec")) s_no_stray_writes(PRP_SPEC, false);
    else if (!strcmp(s, "no_stray_writes_mask")) s_no_stray_writes(PRP_MASK, false);
    else if (!strcmp(s, "no_stray_writes_rw")) s_no_stray_writes(PRP_STRICT, true);
    else if (!strcmp(s, "no_stray_writes_rw_spec")) s_no_stray_writes(PRP_SPEC, true);
    else if (!strcmp(s, "m1n1_layout")) s_m1n1_layout();
    else if (!strncmp(s, "dma_guard_", 10)) guard_case(atoi(s + 10));
    else if (!strncmp(s, "window_direct_", 14)) s_window_direct(atoi(s + 14));
    else if (!strncmp(s, "rw_defense_", 11)) s_rw_defense(atoi(s + 11));
    else if (r45_dispatch(s)) { }
    else if (secA_dispatch(s)) { }
    else if (r46_dispatch(s)) { }
    else if (chk_dispatch(s)) { }
    else { fprintf(stderr, "FAIL: unknown scenario %s\n", s); return 2; }
    printf("{\"scenario\":\"%s\",\"checks\":%u}\n", s, checks);
    return 0;
}
