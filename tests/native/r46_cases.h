/* r4.6 additions: existing assertions are not relaxed. */
#include <pthread.h>
extern void (*r46_barrier_hook)(void);
extern u64 r46_clock_step;
extern unsigned r46_flip_read;
extern int s1n_test_cpu;
extern bool s1n_test_exception;
static int nested_result;
static void r46_nested_hook(void) { nested_result = ans_ro_test_submit(false, 2); }
static void *r46_thread(void *unused) { (void)unused; r46_nested_hook(); return NULL; }
static void r46_concurrent_hook(void)
{
    pthread_t t;
    CHECK(pthread_create(&t, NULL, r46_thread, NULL) == 0);
    CHECK(pthread_join(t, NULL) == 0);
}
static void r46_reentry(bool concurrent)
{
    fresh(rw_cfg()); CHECK(select_native_w(rw_window(0)) == 0);
    unsigned n = F.nio, a = F.nadmin;
    r46_barrier_hook = concurrent ? r46_concurrent_hook : r46_nested_hook;
    CHECK(ans_ro_test_submit(false, 2) == -2);
    CHECK(nested_result == -2);
    CHECK(F.nio == n && F.nadmin == a);
    CHECK(ans_ro_stats()->reason == ANS_RO_E_REENTRY);
    r45_frozen();
}
static u8 *r46_shared;
static void r46_canary_hook(void)
{
    CHECK(!ans_ro_run_canary());
    for(unsigned i=0;i<4096;i++) CHECK(r46_shared[i]==0xa5);
}
static void r46_nested_canary(void)
{
#if S1N_STAGE_CANARY
    fresh(rw_cfg());CHECK(select_native_w(rw_window(0))==0);
    size_t cap;r46_shared=ans_ro_xfer_buffer(&cap);CHECK(r46_shared && cap>=4096);
    memset(r46_shared,0xa5,4096);unsigned n=F.nio;
    r46_barrier_hook=r46_canary_hook;
    CHECK(ans_ro_test_submit(false,2)==-2);
    CHECK(F.nio==n && !F.io_writes && ans_ro_stats()->reason==ANS_RO_E_REENTRY);r45_frozen();
#endif
}
static void r46_canary_pattern_corrupt(void)
{
#if S1N_STAGE_CANARY
    fresh(rw_cfg());CHECK(select_native_w(rw_window(0))==0);
    F.cfg.corrupt_canary_read=true;
    CHECK(!ans_ro_run_canary() && ans_ro_stats()->reason==ANS_RO_E_CANARY);r45_frozen();
#endif
}
static void r46_context(int mode)
{
    fresh(rw_cfg()); CHECK(select_native_w(rw_window(0)) == 0);
    unsigned n = F.nio, a = F.nadmin, w = mmio_writes();
    if (mode & 1) s1n_test_cpu = 1; else s1n_test_exception = true;
    switch (mode / 2) {
    case 0: CHECK(!ans_ro_xfer(false, W_START, 1, 0)); break;
    case 1: CHECK(!ans_ro_flush()); break;
    case 2: CHECK(!ans_ro_run_canary()); break;
    case 3: CHECK(!ans_ro_shutdown()); break;
    case 4: ans_ro_poll(); break;
    case 5: CHECK(ans_ro_test_submit(false, 2) == -2); break;
    }
    CHECK(F.nio == n && F.nadmin == a && mmio_writes() == w);
    CHECK(ans_ro_stats()->reason == ANS_RO_E_CONTEXT);
    s1n_test_cpu = 0; s1n_test_exception = false;
    r45_frozen();
}
static void r46_proxy(void)
{
    const u64 ops[] = {P_HV_WIN_STORAGE_NATIVE, P_HV_WIN_STORAGE_PREP,
        P_HV_WIN_STORAGE_NATIVE_QUIESCE, P_HV_WIN_STORAGE_NATIVE_CANARY,
        P_HV_WIN_STORAGE_NATIVE_STAT, P_HV_WIN_STORAGE_STAT};
    s1n_link_lock();
    for (unsigned i = 0; i < sizeof(ops)/sizeof(ops[0]); i++) {
        CHECK(!s1n_link_proxy(ops[i], true));
        s1n_test_cpu = 1; CHECK(!s1n_link_proxy(ops[i], false)); s1n_test_cpu = 0;
        s1n_test_exception = true; CHECK(!s1n_link_proxy(ops[i], false)); s1n_test_exception = false;
    }
#if S1N_STAGE_CANARY
    CHECK(s1n_link_proxy(P_HV_WIN_STORAGE_NATIVE_CANARY, false));
    CHECK(s1n_link_proxy(P_HV_START, false));
    CHECK(!s1n_link_proxy(P_HV_WIN_STORAGE_NATIVE_CANARY, false));
    fresh(rw_cfg()); CHECK(select_native_w(rw_window(0)) == 0);
    CHECK(!ans_ro_run_canary() && F.io_writes == 0);
    CHECK(ans_ro_stats()->reason == ANS_RO_E_CONTEXT);
#endif
}
static void r46_canary(bool corrupt)
{
#if S1N_STAGE_CANARY
    fresh(rw_cfg()); CHECK(select_native_w(rw_window(0)) == 0);
    u8 *original = NULL;
    if (!corrupt) {
        original = malloc((size_t)S1N_SUB_BLOCKS * 4096);
        CHECK(original != NULL);
        for (u64 off = 0; off < S1N_SUB_BLOCKS; off++)
            fake_disk_block(S1N_SUB_START + off, original + off * 4096);
    }
    unsigned n = F.io_reads;
    unsigned save_commands = S1N_CANARY_CHUNK / s1n_status()->max_xfer;
    if (!save_commands) save_commands = 1;
    if (corrupt) r46_flip_read = n + save_commands + 1;
    CHECK(ans_ro_run_canary() == !corrupt);
    if (corrupt) {
        CHECK(!F.io_writes && ans_ro_stats()->reason == ANS_RO_E_CANARY);
        r45_frozen();
    } else {
        CHECK(F.io_writes == S1N_WRITE_CMD_CAP && F.write_blocks == S1N_WRITE_BLOCK_CAP);
        for (u64 off = 0; off < S1N_SUB_BLOCKS; off++) {
            u8 actual[4096];
            fake_media_block(S1N_SUB_START + off, actual);
            CHECK(!memcmp(original + off * 4096, actual, sizeof(actual)));
        }
        CHECK(F.io_reads >= n + 4 * save_commands * (S1N_SUB_BLOCKS / S1N_CANARY_CHUNK));
        free(original);
    }
#else
    (void)corrupt;
#endif
}
static void r46_timeout(int op)
{
    rw_up(rw_cfg(), 0);
    if (op == 1) CHECK(guest_write(0, 1, 1, 0) == 0); /* finish GPT recheck */
    F.cfg.cqe_never = true;
    r46_clock_step = 1000000;
    u64 start = F.now;
    if (op == 0) CHECK(!ans_ro_flush());
    else if (op == 1) CHECK(guest_write(1, 1, 2, 0) == VNVME_SC_INTERNAL);
    else CHECK(!ans_ro_xfer(false, W_START, 1, 0));
    u64 elapsed = F.now - start;
    CHECK(elapsed >= (op == 2 ? 1000000u : 30000000u));
    CHECK(elapsed < (op == 2 ? 5000000u : 35000000u));
    CHECK(ans_ro_stats()->reason == ANS_RO_E_CMD_TIMEOUT);
    r46_clock_step = 0;
    r45_frozen();
}
static void r46_split(void)
{
    rw_up(rw_cfg(), 0);
    const u32 counts[] = {31, 32, 33, 64, 255, 256};
    for (unsigned i = 0; i < sizeof(counts)/sizeof(counts[0]); i++) {
        unsigned before = F.io_writes;
        CHECK(guest_write(W_BLOCKS - counts[i], counts[i], i + 1, 0) == 0);
        CHECK(F.io_writes - before == (counts[i] + 31) / 32);
        expect_guest(W_BLOCKS - counts[i], counts[i], i + 1);
        CHECK(F.last_read_bytes <= 32 * 4096);
    }
    CHECK(!F.writes_outside_window);
}
static void r46_read_probe(void)
{
    rw_up(rw_cfg(), 0);
    const u64 l[] = {W_BLOCKS-1, W_BLOCKS, UINT64_MAX, UINT64_MAX-1};
    const u32 n[] = {2, 1, 1, 256};
    for (unsigned i=0;i<4;i++) {
        unsigned before=F.nio;
        CHECK(guest_read(l[i],n[i],DATA)==VNVME_SC_LBA_OUT_OF_RANGE); /* r4.13 */
        CHECK(F.nio==before && ans_ro_state()==ANS_RO_LIVE);
        CHECK(controller.queue.refused_reads==i+1);
        CHECK(guest_write(0,1,i+1,0)==0);
        expect_guest(0,1,i+1);
    }
    unsigned before=F.nio;
    CHECK(io_cmd(VNVME_IO_READ,UINT64_MAX,1,DATA,65535)==VNVME_SC_LBA_OUT_OF_RANGE); /* r4.13 */
    CHECK(F.nio==before && ans_ro_state()==ANS_RO_LIVE && controller.queue.refused_reads==5);
    CHECK(hv_win_storage_stat(12)==5);

}
static void r46_raw_range(int which)
{
    rw_up(rw_cfg(),0);
    CHECK(ans_ro_xfer(false,W_START,128,0));
    unsigned n=F.nio;
    const u64 l[]={W_START-1,W_START+W_BLOCKS,UINT64_MAX,W_START+W_BLOCKS-1};
    u64 lba=l[which];
    CHECK(ans_ro_test_submit_raw(false,1,F.last_read_prp1,F.last_read_prp2,
          (u32)lba,(u32)(lba>>32),127)==-2);
    CHECK(F.nio==n && !F.io_writes && ans_ro_stats()->reason==ANS_RO_E_WINDOW);
    r45_frozen();
}
static void r46_admin_bad(int op)
{
    fresh(base_cfg());CHECK(select_native()==0);unsigned n=F.nadmin;
    CHECK(ans_ro_test_submit(true,op)==-2);
    CHECK(F.nadmin==n && ans_ro_stats()->reason==ANS_RO_E_NOT_ALLOWED);r45_frozen();
}
static void r46_ro_cap(void)
{
    fresh(base_cfg());CHECK(select_native()==0 && prep(W_BLOCKS)==0);guest_bringup();
    CHECK(s1n_status()->max_xfer==1);
    unsigned n=F.io_reads;CHECK(guest_read(0,32,DATA)==0);CHECK(F.io_reads==n+32);
    n=F.nio;CHECK(guest_read(0,33,DATA)==VNVME_SC_INVALID_FIELD);CHECK(F.nio==n);r45_frozen();
}
static void r46_write_refusal(int which)
{
    rw_up(rw_cfg(), 0);
    const u64 l[] = {W_BLOCKS-255,W_BLOCKS-1,W_BLOCKS,UINT64_MAX,UINT64_MAX-100,0};
    const u32 n[] = {256,2,1,1,256,257};
    unsigned before=F.nio;
    CHECK(io_cmd(VNVME_IO_WRITE,l[which],1,WDATA,n[which]-1)==VNVME_SC_INVALID_FIELD);
    CHECK(F.nio==before && !F.io_writes);
    r45_frozen();
}
static void r46_ro_write(void)
{
    fresh(base_cfg()); CHECK(select_native()==0 && prep(W_BLOCKS)==0); guest_bringup();
    unsigned n=F.nio;
    CHECK(guest_write(0,1,1,0)==VNVME_SC_NS_WRITE_PROTECTED);
    CHECK(F.nio==n);r45_frozen();
}
static void r46_allowlist(int which)
{
    fresh(rw_cfg());CHECK(select_native_w(rw_window(0))==0);
    unsigned n=F.nio;
    const u8 op[]={0x04,0x08,0x09,0x80,0x81,0x84,0xff};
    CHECK(ans_ro_test_submit(false,op[which])==-2);
    CHECK(F.nio==n && ans_ro_stats()->reason==ANS_RO_E_NOT_ALLOWED);r45_frozen();
}
static void r46_guest_op(int op)
{
    rw_up(rw_cfg(),0); unsigned n=F.nio;
    CHECK(io_cmd(op,0,1,WDATA,0)==VNVME_SC_INVALID_OPCODE);
    CHECK(F.nio==n && F.io_writes==0);outside_untouched();
}
static void r46_flush_error(void)
{
    rw_up(rw_cfg(),0); F.cfg.flush_error=true;
    CHECK(!ans_ro_flush());CHECK(ans_ro_stats()->reason==ANS_RO_E_STATUS);r45_frozen();
}
static void r46_stage_gate(void)
{
    fresh(rw_cfg());CHECK(select_native_w(rw_window(0))==0);
    CHECK(s1n_writable()==(S1N_STAGE==3));
#if S1N_STAGE != 3
    unsigned n=F.nio;
    CHECK(!ans_ro_xfer(true,S1N_ZONE_START,1,0));CHECK(F.nio==n && !F.io_writes);r45_frozen();
#endif
}
static void r46_stage0_flush_cap(void)
{
#if S1N_STAGE == 0
    fresh(rw_cfg());CHECK(select_native_w(rw_window(0))==0);
    for(unsigned i=0;i<4;i++)CHECK(ans_ro_flush());
    CHECK(!ans_ro_flush() && F.io_flushes==4 && !F.io_writes);
    CHECK(ans_ro_stats()->reason==ANS_RO_E_CAP);r45_frozen();
#endif
}
static bool r46_dispatch(const char *s)
{
    if (!strcmp(s,"r46_nested")) r46_reentry(false);
    else if (!strcmp(s,"r46_concurrent")) r46_reentry(true);
    else if (!strncmp(s,"r46_context_",12)) r46_context(atoi(s+12));
    else if (!strcmp(s,"r46_nested_canary")) r46_nested_canary();
    else if (!strcmp(s,"r46_canary_pattern_corrupt")) r46_canary_pattern_corrupt();
    else if (!strcmp(s,"r46_proxy")) r46_proxy();
    else if (!strcmp(s,"r46_canary")) r46_canary(false);
    else if (!strcmp(s,"r46_canary_save")) r46_canary(true);
    else if (!strncmp(s,"r46_timeout_",12)) r46_timeout(atoi(s+12));
    else if (!strncmp(s,"r46_raw_range_",14)) r46_raw_range(atoi(s+14));
    else if (!strncmp(s,"r46_admin_bad_",14)) r46_admin_bad(atoi(s+14));
    else if (!strcmp(s,"r46_ro_cap")) r46_ro_cap();
    else if (!strcmp(s,"r46_stage_gate")) r46_stage_gate();
    else if (!strcmp(s,"r46_stage0_flush_cap")) r46_stage0_flush_cap();
    else if (!strcmp(s,"r46_split")) r46_split();
    else if (!strcmp(s,"r46_read_probe")) r46_read_probe();
    else if (!strncmp(s,"r46_write_refusal_",18)) r46_write_refusal(atoi(s+18));
    else if (!strcmp(s,"r46_ro_write")) r46_ro_write();
    else if (!strncmp(s,"r46_allowlist_",14)) r46_allowlist(atoi(s+14));
    else if (!strncmp(s,"r46_guest_op_",13)) r46_guest_op(atoi(s+13));
    else if (!strcmp(s,"r46_flush_error")) r46_flush_error();
    else return false;
    return true;
}
