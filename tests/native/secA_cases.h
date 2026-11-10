/* Section A review: r4.4 assertions restored against r4.5 source.
 * Included at the end of the harness copy of tests/native/body.c (after r45_cases.h).
 * Adaptation rule: one refusal per process; after the refusal, either the session is FROZEN
 * (then nothing more may reach ANS: r45_frozen) or it is LIVE (then service must continue
 * exactly as r4.4 asserted). Which of the two happened is printed, never assumed. */
#define access posix_access /* unistd.h access() vs the harness MMIO hook named access */
#include <unistd.h>
#undef access
#include <sys/wait.h>

static void secA_after_refusal_read(u64 dest)
{
    unsigned n = F.nio;
    if (ans_ro_state() == ANS_RO_FROZEN) {
        printf("{\"secA_after\":\"frozen\",\"reason\":%u}\n", (unsigned)ans_ro_stats()->reason);
        CHECK(guest_read(0, 1, dest) != 0);
        CHECK(F.nio == n);
        r45_frozen();
    } else {
        printf("{\"secA_after\":\"live\"}\n");
        CHECK(ans_ro_state() == ANS_RO_LIVE);
        CHECK(guest_read(0, 1, dest) == 0); check_blocks(0, 1, dest);
        CHECK(F.nio == n + 1);
    }
}

/* (c) r4.4 e2e_reads boundary refusals, READ-ONLY config (base_cfg, the stage-0 shape of r4.4).
 *     r4.4 body.c:191-195. r4.5 only runs these with rw_cfg and (MAX,2) instead of (MAX,1). */
static void secA_ro_guest_read(int which, bool stage_cfg)
{
    if (stage_cfg) {               /* the stage-0/1/2 launcher shape: RW contract flag, RO guest */
        fresh(rw_cfg());
        CHECK(select_native_w(rw_window(0)) == 0);
    } else {
        fresh(base_cfg());
        CHECK(select_native() == 0);
    }
    CHECK(prep(W_BLOCKS) == 0);
    guest_bringup();
    CHECK(guest_read(5, 1, DATA) == 0); check_blocks(5, 1, DATA);
    static const u64 lba[] = {W_BLOCKS - 1, W_BLOCKS, UINT64_MAX, UINT64_MAX - 1, 0, W_BLOCKS - 31};
    static const u32 cnt[] = {2, 1, 1, 32, 33, 32};
    unsigned before = F.io_reads, n = F.nio, a = F.nadmin;
    CHECK(guest_read(lba[which], cnt[which], DATA) == VNVME_SC_INVALID_FIELD);
    CHECK(F.io_reads == before && F.nio == n && F.nadmin == a);  /* no ANS command */
    live_policy();
    secA_after_refusal_read(DATA);
}

/* (c) r4.4 rw_basic refusals, r4.4 body.c:586-592, one per process, after the same 4 writes. */
static void secA_rw_write(int which)
{
    rw_up(rw_cfg(), 0);
    CHECK(guest_write(0, 1, 1, 0) == 0); expect_guest(0, 1, 1);
    CHECK(guest_write(W_BLOCKS - 1, 1, 1, 0) == 0); expect_guest(W_BLOCKS - 1, 1, 1);
    CHECK(guest_write(5000, 256, 2, 0) == 0); expect_guest(5000, 256, 2);
    CHECK(guest_write(W_BLOCKS - 256, 256, 3, 0) == 0); expect_guest(W_BLOCKS - 256, 256, 3);
    unsigned w = F.io_writes, r = F.io_reads, n = F.nio;
    u16 s = 0xffff;
    switch (which) {
    case 0: s = guest_write(W_BLOCKS - 255, 256, 9, 0); break;     /* max length straddling the end */
    case 1: s = guest_write(W_BLOCKS - 1, 2, 9, 0); break;
    case 2: s = guest_write(W_BLOCKS, 1, 9, 0); break;
    case 3: s = guest_write(UINT64_MAX, 1, 9, 0); break;            /* overflow */
    case 4: s = guest_write(UINT64_MAX - 100, 256, 9, 0); break;
    case 5: s = io_cmd(VNVME_IO_WRITE, 0, 1, WDATA, 256); break;    /* NLB field 256 = 257 blocks */
    default: CHECK(0);
    }
    CHECK(s == VNVME_SC_INVALID_FIELD);
    CHECK(F.io_writes == w && F.io_reads == r && F.nio == n);      /* nothing reached ANS */
    CHECK(F.io_writes == RW4_WRITES);                              /* r4.6: the 4 guest writes split to 18 */
    outside_untouched();
    if (ans_ro_state() == ANS_RO_FROZEN) {
        printf("{\"secA_after\":\"frozen\",\"reason\":%u}\n", (unsigned)ans_ro_stats()->reason);
        CHECK(guest_write(800, 1, 11, 0) != 0);
        CHECK(F.nio == n && F.io_writes == RW4_WRITES);
        r45_frozen();
    } else {
        printf("{\"secA_after\":\"live\"}\n");
        CHECK(guest_write(800, 1, 11, 0) == 0); expect_guest(800, 1, 11);
        CHECK(F.io_writes == RW4_WRITES + 1);
        CHECK(io_cmd(VNVME_IO_FLUSH, 0, 1, 0, 0) == 0);
        expect_media(800, 1, 11);
        outside_untouched();
    }
}

/* (c) guest READ (UINT64_MAX, 1) in the rw config (r4.5 uses (MAX,2)). */
static void secA_rw_read_max1(void)
{
    rw_up(rw_cfg(), 0);
    unsigned n = F.nio;
    CHECK(guest_read(UINT64_MAX, 1, RDATA) == VNVME_SC_INVALID_FIELD);
    CHECK(F.nio == n);
    secA_after_refusal_read(RDATA);
}

/* (d) r4.4 rw_random generator restored verbatim (r4.4 body.c:752-796). Every invalid shape is
 *     executed in a forked child (refusal ends the child's session); the parent skips it, exactly
 *     as r4.4's parent state was unchanged by a refusal. */
static unsigned secA_children, secA_child_writes, secA_child_reads;
static void secA_child_wait(pid_t c)
{
    int status;
    CHECK(c > 0);
    CHECK(waitpid(c, &status, 0) == c);
    if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0))
        fprintf(stderr, "FAIL: child refusal case status 0x%x\n", status);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
static void s_secA_rw_random(void)
{
    rw_up(rw_cfg(), 0);
    u32 gen = 1;
    for (unsigned op = 0; op < 600; op++) {
        u64 r = rnd();
        u32 n = 1 + (u32)(r % 4 == 0 ? rnd() % 256 : rnd() % 16);
        u64 lba;
        switch (rnd() % 4) {
        case 0: lba = rnd() % 4096; break;
        case 1: lba = W_BLOCKS - 1 - rnd() % 4096; break;
        case 2: lba = rnd() % W_BLOCKS; n = 1 + (u32)(rnd() % 4); break;
        default: lba = W_BLOCKS - n + (rnd() % 3); break;
        }
        bool fits = lba < W_BLOCKS && n <= W_BLOCKS - lba;
        unsigned k = (unsigned)(rnd() % 10);
        if (k < 5) {
            u32 g = ++gen;
            u32 fua = (rnd() % 8 == 0) ? (1u << 30) : 0;
            if (!fits) {
                fflush(stdout); fflush(stderr);
                pid_t c = fork();
                if (c == 0) {
                    unsigned w = F.io_writes, nn = F.nio;
                    u16 st = guest_write(lba, n, g, fua);
                    CHECK(st == VNVME_SC_INVALID_FIELD);
                    CHECK(F.io_writes == w && F.nio == nn && F.writes_outside_window == 0);
                    outside_untouched();
                    CHECK(ans_ro_state() == ANS_RO_FROZEN);
                    r45_frozen();
                    _exit(0);
                }
                secA_child_wait(c); secA_children++; secA_child_writes++;
                continue;
            }
            u16 st = guest_write(lba, n, g, fua);
            CHECK(st == 0);
            for (u32 i = 0; i < n; i++) *sh_find(lba + i, true) = g;
        } else if (k < 9) {
            if (!fits) {
                fflush(stdout); fflush(stderr);
                pid_t c = fork();
                if (c == 0) {
                    unsigned rr = F.io_reads, nn = F.nio;
                    CHECK(io_cmd(VNVME_IO_READ, lba, n, RDATA, 0) == VNVME_SC_INVALID_FIELD);
                    CHECK(F.io_reads == rr && F.nio == nn);
                    outside_untouched();
                    CHECK(ans_ro_state() == ANS_RO_LIVE);
                    CHECK(controller.queue.refused_reads == 1);
                    CHECK(guest_read(0, 1, RDATA) == 0);
                    _exit(0);
                }
                secA_child_wait(c); secA_children++; secA_child_reads++;
                continue;
            }
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
    for (unsigned k = 0; k < SH; k++)
        if (shadow[k].used) expect_media(shadow[k].l, 1, shadow[k].gen);
    CHECK(fake_cached_blocks() == 0);
    outside_untouched();
    CHECK(ans_ro_state() == ANS_RO_LIVE && controller.wcanary.write_cmds > 0);
    CHECK(secA_children > 0);
    printf("{\"random_ops\":600,\"invalid_children\":%u,\"invalid_writes\":%u,\"invalid_reads\":%u,\"durable_blocks\":%u,\"ans_writes\":%u}\n",
           secA_children, secA_child_writes, secA_child_reads, fake_durable_blocks(), F.io_writes);
}

/* (a/b) GPT reject: r4.4 asserted a clean stage-1 stop; r4.5 asserts FROZEN only. Restore the
 *       "nothing more reaches ANS" half of it: after the reject no xfer/flush/shutdown/poll MMIO. */
static void secA_gpt_frozen(int mut)
{
    s_gpt(mut);
    printf("{\"reason\":%u,\"s1n_reason\":%u}\n", (unsigned)ans_ro_stats()->reason, (unsigned)s1n_status()->reason);
    r45_frozen();
    CHECK(prep(W_BLOCKS) == -1);
}

/* (a) mismatched / stale backup GPT, harness fake mutations 20..29 (see secA_fake_patch.py; 29 added by the
 *     T1 overlay: only a later backup-array sector differs, so a first-sector-only compare is caught). */
static void secA_backup(int mut)
{
    struct fake_cfg g = base_cfg();
    g.gpt_mutation = mut;
    fresh(g);
    CHECK(select_native() == -1);
    CHECK(!s1n_verified());
    printf("{\"s1n_reason\":%u,\"ans_reason\":%u}\n", (unsigned)s1n_status()->reason, (unsigned)ans_ro_stats()->reason);
    CHECK(s1n_status()->reason == S1N_E_BACKUP || s1n_status()->reason == S1N_E_READ);
    exposed_nothing();
    frozen_untouched();
    live_policy();
    r45_frozen();
}
/* positive control for the patched fake: unmutated backup still accepted, rw too */
static void secA_backup_control(void)
{
    fresh(rw_cfg());
    CHECK(select_native_w(rw_window(0)) == 0);
    CHECK(s1n_verified());
}

/* (e) no_stray_writes slot allowance: which SQE/TCB slots does stray_allowed() accept? */
static void secA_stray_slots(void)
{
    rw_up(rw_cfg(), 0);
    CHECK(guest_write(10, 1, 1, 0) == 0);
    CHECK(guest_write(11, 1, 1, 0) == 0);
    rd_extent_lo = rd_extent_hi = 0; plist_lo = 0;
    CHECK(F.iosq_reg && F.iosq_tcb);
    for (unsigned s = 0; s < 8; s++) {
        bool sq = stray_allowed(F.iosq_reg + s * 64 + 8);
        bool tcb = stray_allowed(F.iosq_tcb + s * 128 + 8);
        printf("{\"slot\":%u,\"sqe_allowed\":%d,\"tcb_allowed\":%d}\n", s, sq, tcb);
        bool want = s == ANS_RO_IO_TAG || s == ANS_RO_IO_TAG_ALT;
        CHECK(sq == want && tcb == want);
    }
}

/* r4.4 defense-probe evidence (rw_requires_flag) restored one opcode per process: RO session
 * (no S1N_RW), raw I/O WRITE / FLUSH must not reach ANS whichever layer refuses it. */
static void secA_raw_ro_op(u8 op)
{
    fresh(rw_cfg());
    CHECK(select_native() == 0);
    CHECK(!s1n_writable());
    unsigned n = F.nio;
    int rc = ans_ro_test_submit(false, op);
    CHECK(rc < 0 && F.nio == n && F.io_writes == 0 && F.io_flushes == 0);
    printf("{\"rc\":%d,\"state\":%u,\"window_refusals\":%lu}\n", rc, (unsigned)ans_ro_state(), (unsigned long)ans_ro_stats()->window_refusals);
    if (ans_ro_state() == ANS_RO_FROZEN) r45_frozen();
    else { CHECK(ans_ro_test_submit(false, 0x02) == 0); CHECK(F.nio == n + 1); }
}
static bool secA_dispatch(const char *s)
{
    if (!strncmp(s, "secA_raw_ro_op_", 15)) { secA_raw_ro_op((u8)atoi(s + 15)); return true; }
    if (!strncmp(s, "secA_ro_guest_read_", 19)) secA_ro_guest_read(atoi(s + 19), false);
    else if (!strncmp(s, "secA_stage_guest_read_", 22)) secA_ro_guest_read(atoi(s + 22), true);
    else if (!strncmp(s, "secA_rw_write_", 14)) secA_rw_write(atoi(s + 14));
    else if (!strcmp(s, "secA_rw_read_max1")) secA_rw_read_max1();
    else if (!strcmp(s, "secA_rw_random")) s_secA_rw_random();
    else if (!strncmp(s, "secA_gpt_frozen_", 16)) secA_gpt_frozen(atoi(s + 16));
    else if (!strncmp(s, "secA_backup_", 12) && strcmp(s, "secA_backup_control")) secA_backup(atoi(s + 12));
    else if (!strcmp(s, "secA_backup_control")) secA_backup_control();
    else if (!strcmp(s, "secA_stray_slots")) secA_stray_slots();
    else return false;
    return true;
}
