#define access posix_access
#include <unistd.h>
#undef access
#include <sys/wait.h>
/* Independent review scenarios (appended after the builder's prelude + actual r3 sources +
 * builder body.c with its main renamed). The fake ANS copy is patched by run.py to log every
 * physical READ LBA (review_log_read) and to optionally complete a READ without DMA. */

static u64 rv_lba[1 << 16]; static unsigned rv_n;
static bool rv_verified_mark; static unsigned rv_mark;
bool review_skip_dma;
void review_log_read(u64 slba, unsigned nlb)
{
    for (unsigned i = 0; i < nlb; i++) if (rv_n < (1u << 16)) rv_lba[rv_n++] = slba + i;
}
static void rv_window_only(unsigned from)
{
    for (unsigned i = from; i < rv_n; i++) CHECK(rv_lba[i] >= W_START && rv_lba[i] - W_START < W_BLOCKS);
}
static unsigned rv_frozen_raw, rv_live_raw;
static u64 rv_rs = 0x9e3779b97f4a7c15ULL;
static u64 rv_rnd(void) { rv_rs ^= rv_rs << 13; rv_rs ^= rv_rs >> 7; rv_rs ^= rv_rs << 17; return rv_rs; }
static int rv_raw(unsigned qid, struct vnvme_command c)   /* status, or -1 when no CQE */
{
    struct vnvme_queue *q = &controller.queue.queues[qid];
    u16 slot = q->cq_tail;
    c.cid = next_cid++;
    memcpy(G(q->sq_addr) + (u64)q->sq_tail * sizeof(c), &c, sizeof(c));
    CHECK(wr(0x1000 + qid * 8, (q->sq_tail + 1) % q->sq_size, 2));
    if (!q->cq_pending) return -1;
    struct vnvme_completion d; memcpy(&d, G(q->cq_addr) + (u64)slot * sizeof(d), sizeof(d));
    CHECK(wr(0x1004 + qid * 8, q->cq_tail, 2));
    return d.cid == c.cid ? d.status >> 1 : -1;
}

/* 1. adversarial guest: random LBAs/lengths/opcodes/NSIDs; ANS sees only in-window READs */
static void r_guest_fuzz(void)
{
    fresh(base_cfg());
    CHECK(select_native() == 0);
    unsigned gpt_reads = rv_n;                    /* LBA 1 + entry sectors during verification */
    for (unsigned i = 0; i < gpt_reads; i++) CHECK(rv_lba[i] == 1 || (rv_lba[i] >= 2 && rv_lba[i] < 6) || (rv_lba[i] >= F_NSZE - 5 && rv_lba[i] < F_NSZE));
    CHECK(prep(W_BLOCKS) == 0);
    guest_bringup();
    unsigned ok_reads = 0;
    for (unsigned it = 0; it < 3000; it++) {
        unsigned kind = rv_rnd() % 10;
        if (kind < 6) {
            u64 lba; switch (rv_rnd() % 6) {
            case 0: lba = rv_rnd() % W_BLOCKS; break;
            case 1: lba = W_BLOCKS - 1 - rv_rnd() % 40; break;
            case 2: lba = W_BLOCKS + rv_rnd() % 40; break;
            case 3: lba = rv_rnd(); break;
            case 4: lba = UINT64_MAX - rv_rnd() % 40; break;
            default: lba = (u64)-(long long)W_START + rv_rnd() % 64 - 32; break;
            }
            u32 n = 1 + rv_rnd() % 32;
            /* Refusals terminate a resident session. Exercise one in a forked process. */
            bool invalid = lba >= W_BLOCKS || n > W_BLOCKS - lba;
            pid_t child = invalid ? fork() : -1;
            CHECK(!invalid || child >= 0);
            if (invalid && child > 0) {
                int status; CHECK(waitpid(child, &status, 0) == child);
                CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
                continue;
            }
            unsigned before = rv_n;
            u16 s = guest_read(lba, n, DATA);
            bool in = lba < W_BLOCKS && n <= W_BLOCKS - lba;
            if (in) { CHECK(s == 0); check_blocks(lba, n, DATA); ok_reads++; CHECK(rv_n == before + n); }
            else { CHECK(s == VNVME_SC_LBA_OUT_OF_RANGE); CHECK(rv_n == before); CHECK(ans_ro_state() == ANS_RO_LIVE); CHECK(guest_read(0, 1, DATA) == 0); _exit(0); }
        } else if (kind < 8) {
            /* secA: command generated in the parent (r4.4 rng order), tried in a child first. */
            struct vnvme_command c = {.opcode = (u8)rv_rnd(), .nsid = (u32[]){0, 1, 2, 3, 0xffffffffu}[rv_rnd() % 5],
                                      .prp1 = DATA, .cdw10 = (u32)rv_rnd(), .cdw11 = (u32)rv_rnd(), .cdw12 = (u32)(rv_rnd() % 32)};
            if (c.opcode == VNVME_IO_READ && c.nsid == 1) c.opcode = VNVME_IO_WRITE;
            fflush(stdout); fflush(stderr);
            pid_t child = fork(); CHECK(child >= 0);
            if (child > 0) {
                int status; CHECK(waitpid(child, &status, 0) == child);
                CHECK(WIFEXITED(status) && (WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == 10));
                if (WEXITSTATUS(status) == 0) { rv_frozen_raw++; continue; }
                rv_live_raw++;
                unsigned before = rv_n, nio = F.nio;
                (void)rv_raw(1, c);                 /* replay in the live session, as r4.4 did */
                CHECK(rv_n == before && F.nio == nio && ans_ro_state() == ANS_RO_LIVE);
                continue;
            }
            unsigned before = rv_n, nio = F.nio;
            int s = rv_raw(1, c);
            (void)s;
            CHECK(rv_n == before && F.nio == nio); /* no non-read / non-NSID-1 command reaches the disk */
            if (ans_ro_state() == ANS_RO_FROZEN) {
                unsigned w = mmio_writes();
                CHECK(guest_read(0, 1, DATA) != 0);
                CHECK(rv_n == before && F.nio == nio && mmio_writes() == w);
                _exit(0);
            }
            CHECK(ans_ro_state() == ANS_RO_LIVE);
            _exit(10);
        } else {
            static const u8 adm[] = {0x02, 0x06, 0x08, 0x09, 0x0a, 0x0c, 0x10, 0x11, 0x14, 0x15, 0x19, 0x80, 0x84, 0xc0};
            struct vnvme_command c = {.opcode = adm[rv_rnd() % sizeof adm], .nsid = (u32[]){0, 1, 2, 0xffffffffu}[rv_rnd() % 4],
                                      .prp1 = IDBUF, .cdw10 = (u32)(rv_rnd() % 16), .cdw11 = (u32)(rv_rnd() % 8)};
            if (c.opcode == 0x0c) continue;       /* held AER: fine, but keeps the admin CQ busy */
            unsigned na = F.nadmin, before = rv_n;
            rv_raw(0, c);
            CHECK(F.nadmin == na && rv_n == before);   /* guest admin never reaches ANS */
        }
    }
    rv_window_only(gpt_reads);
    live_policy();
    CHECK(F.bad_nsid == 0 && F.multi_block == 0);
    for (unsigned i = 0; i < F.nio && i < 64; i++) CHECK(F.io_op_seq[i] == 0x02);
    CHECK(F.nadmin <= 8);                         /* Identify + Create CQ/SQ only, all during init */
    hv_win_storage_retire();
    stopped_like_stage1();
    printf("info: %u in-window guest reads served, %u ANS reads, all within [start, start+len); raw cmds: %u froze (child), %u non-terminal replayed in-session\n", ok_reads, rv_n - gpt_reads, rv_frozen_raw, rv_live_raw);
    CHECK(rv_live_raw > 0);
}

/* 2. after a freeze, NOTHING (tick, retire, re-select, re-prep, guest I/O) writes MMIO */
static void r_freeze_total(void)
{
    fresh(base_cfg());
    CHECK(select_native() == 0); CHECK(prep(W_BLOCKS) == 0); guest_bringup();
    F.cfg.cqe_never = true;
    CHECK(guest_read(3, 1, DATA) == VNVME_SC_INTERNAL);
    unsigned w = mmio_writes(), n = F.ntr;
    for (int i = 0; i < 200; i++) hv_win_storage_tick();
    for (int i = 0; i < 20; i++) CHECK(guest_read(i, 4, DATA) == VNVME_SC_INTERNAL);
    CHECK(select_native() == -1); CHECK(prep(W_BLOCKS) == -1);
    hv_win_storage_retire(); hv_win_storage_retire();
    CHECK(mmio_writes() == w);
    printf("info: freeze: %u MMIO reads, 0 writes after the freeze\n", F.ntr - n);
}

/* 3. native selected, PREP never attempted (launcher aborts between steps 2 and 3), then hv
 *    exit/hv_init-style retire: is ANS quiesced? */
static void r_select_no_prep(void)
{
    /* r3.1: R1 is fixed. Before PREP the tick drains the mailbox, and retire quiesces. */
    fresh(base_cfg());
    CHECK(select_native() == 0);
    for (int i = 0; i < 5; i++) hv_win_storage_tick();
    CHECK(ans_ro_stats()->polls >= 5);
    hv_win_storage_retire();
    CHECK(ans_ro_state() == ANS_RO_STOPPED);
    CHECK((F.cpu_control & 0x10) == 0 && F.stopped_after_sleep_ack && F.pmgr_resets == 2);
    printf("info: select without PREP, then retire: ans_ro state %u (quiesced), cpu running %u, polls %lu\n",
           ans_ro_state(), (F.cpu_control & 0x10) != 0, (unsigned long)ans_ro_stats()->polls);
}

/* r4.4 diagnostic restored, disambiguated: compare the guest's sector against every GPT sector the
 * resident reads (primary header 1, primary array 2..5, backup array NSZE-5..NSZE-2, backup header
 * NSZE-1). A match with a NON-ZERO GPT sector is an outside-window leak and FAILS. An all-zero answer
 * is reported as zeros (it also equals the all-zero padding sectors, which carry no disk data). */
static void rv_stale_verdict(u16 s, const char *when)
{
    const u64 gl[] = {1, 2, 3, 4, 5, F_NSZE - 5, F_NSZE - 4, F_NSZE - 3, F_NSZE - 2, F_NSZE - 1};
    bool zero = true; for (unsigned i = 0; i < 4096; i++) zero &= G(DATA)[i] == 0;
    int leak = -1;
    for (unsigned k = 0; k < sizeof gl / 8; k++) {
        u8 b[4096]; fake_disk_block(gl[k], b);
        bool bz = true; for (unsigned i = 0; i < 4096; i++) bz &= b[i] == 0;
        if (!bz && !memcmp(G(DATA), b, 4096)) leak = (int)k;
    }
    printf("info: READ completed without DMA (%s) -> guest status %u, guest sees %s\n", when, s,
           leak >= 0 ? (gl[leak] == 1 || gl[leak] == F_NSZE - 1 ? "STALE GPT HEADER (outside-window data leak)" : "STALE GPT ENTRY SECTOR (outside-window data leak)")
                     : zero ? "all zeros (no GPT or stale data)" : "NON-ZERO non-GPT data");
    CHECK(s == 0);
    CHECK(leak < 0);
    CHECK(zero);
    CHECK(s1n_test_buffers_clear());
}

/* 4. ANS reports success for a READ but performs no DMA: does the guest see stale bounce
 *    contents (e.g. the last GPT sector read during verification)? */
static void r_stale_bounce(void)
{
    fresh(base_cfg());
    CHECK(select_native() == 0); CHECK(prep(W_BLOCKS) == 0); guest_bringup();
    review_skip_dma = true;
    memset(G(DATA), 0x77, 4096);
    u16 s = guest_read(0, 1, DATA);
    u8 gpt_last[4096]; fake_disk_block(5, gpt_last);
    u8 hdr[4096]; fake_disk_block(1, hdr);
    bool leak_entries = !memcmp(G(DATA), gpt_last, 4096), leak_hdr = !memcmp(G(DATA), hdr, 4096);
    bool any_gpt = false; for (u64 l = 1; l < 6; l++) { u8 b[4096]; fake_disk_block(l, b); any_gpt |= !memcmp(G(DATA), b, 4096); }
    /* r3.1: R2 is fixed. The guest must see zeros, never a GPT sector. */
    /* (GPT entry sector 5 is all zero, so an all-zero answer matches it: assert zeros + no header) */
    CHECK(s == 0 && !leak_hdr);
    for (unsigned i = 0; i < 4096; i++) CHECK(G(DATA)[i] == 0);
    rv_stale_verdict(s, "after GPT verification");
    (void)any_gpt; (void)leak_entries;
}
/* 4b. same, after an in-window data READ left real data in the transfer buffer */
static void r_stale_bounce_after_data(void)
{
    fresh(base_cfg());
    CHECK(select_native() == 0); CHECK(prep(W_BLOCKS) == 0); guest_bringup();
    CHECK(guest_read(4242, 1, DATA) == 0); check_blocks(4242, 1, DATA);
    review_skip_dma = true;
    memset(G(DATA), 0x77, 4096);
    u16 s = guest_read(0, 1, DATA);
    CHECK(fake_phys_of_pattern(G(DATA)) != W_START + 4242);
    rv_stale_verdict(s, "after an in-window read");
}
/* 4c. stage-3 rw: the first guest WRITE triggers the GPT recheck (GPT sectors re-read through
 *     the resident); a READ without DMA afterwards must still show zeros, never GPT bytes. */
static void r_stale_bounce_after_recheck(void)
{
    fresh(rw_cfg());
    CHECK(select_native_w(rw_window(0)) == 0); CHECK(prep(W_BLOCKS) == 0);
    CHECK(wr(0x24, 0x00070007, 2)); CHECK(wr(0x28, ASQ, 3)); CHECK(wr(0x30, ACQ, 3));
    CHECK(wr(0x14, S1_CC_VALID, 2)); CHECK(controller.regs.csts == 1);
    struct vnvme_command c = {.opcode = VNVME_ADMIN_CREATE_CQ, .prp1 = IOCQ, .cdw10 = (15 << 16) | 1, .cdw11 = 3};
    CHECK(submit(0, c) == 0);
    c = (struct vnvme_command){.opcode = VNVME_ADMIN_CREATE_SQ, .prp1 = IOSQ, .cdw10 = (15 << 16) | 1, .cdw11 = (1 << 16) | 1};
    CHECK(submit(0, c) == 0);
    unsigned gpt_before = 0; for (unsigned i = 0; i < rv_n; i++) gpt_before += rv_lba[i] < W_START || rv_lba[i] >= W_START + W_BLOCKS;
    CHECK(guest_write(100, 1, 5, 0) == 0);
    unsigned gpt_after = 0; for (unsigned i = 0; i < rv_n; i++) gpt_after += rv_lba[i] < W_START || rv_lba[i] >= W_START + W_BLOCKS;
    CHECK(gpt_after > gpt_before);          /* the recheck really re-read GPT sectors */
    review_skip_dma = true;
    memset(G(DATA), 0x77, 4096);
    u16 s = guest_read(0, 1, DATA);
    rv_stale_verdict(s, "after the first-write GPT recheck");
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    if (!strcmp(argv[1], "guest_fuzz")) r_guest_fuzz();
    else if (!strcmp(argv[1], "freeze_total")) r_freeze_total();
    else if (!strcmp(argv[1], "select_no_prep")) r_select_no_prep();
    else if (!strcmp(argv[1], "stale_bounce")) r_stale_bounce();
    else if (!strcmp(argv[1], "stale_bounce_after_data")) r_stale_bounce_after_data();
    else if (!strcmp(argv[1], "stale_bounce_after_recheck")) r_stale_bounce_after_recheck();
    else return 2;
    printf("{\"scenario\":\"%s\",\"checks\":%u}\n", argv[1], checks);
    return 0;
}
