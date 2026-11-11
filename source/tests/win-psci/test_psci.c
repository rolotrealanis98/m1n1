/* SPDX-License-Identifier: MIT */
/*
 * Host unit test for src/hv/win/hv_win_psci.c (smp2, SMP-PLAN M4).
 *
 * The boot sequence below is the one Windows (the same WinPE) issued on the offline twin with two
 * CPUs (tooling/offline-twin-20261109 run winpe-r2.2-cpu2, QEMU arm_psci_call trace): SMCCC_VERSION,
 * TRNG_VERSION, PSCI_VERSION, PSCI_FEATURES for ten function IDs, CPU_ON_64 for the second CPU,
 * then PSCI_VERSION/FEATURES from the new CPU. Topology = J716s: boot CPU 0 = MPIDR 0x80040000,
 * CPU 1 = 0x80040001, CPU 2 = 0x80040002 alive but not offered to the guest.
 */
#include <stdarg.h>
#include <stdlib.h>

#include "psci_host.h"

#include "hv_win.h"

static int checks, fails;
static const char *cur_test = "";
static bool verbose;

#define CHECK(c)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(c)) {                                                                                \
            fails++;                                                                               \
            printf("FAIL [%s] %s:%d: %s\n", cur_test, __FILE__, __LINE__, #c);                     \
        }                                                                                          \
    } while (0)
#define CHECK_EQ(a, b)                                                                             \
    do {                                                                                           \
        long _a = (long)(a), _b = (long)(b);                                                       \
        checks++;                                                                                  \
        if (_a != _b) {                                                                            \
            fails++;                                                                               \
            printf("FAIL [%s] %s:%d: %s = %ld (0x%lx), expected %ld\n", cur_test, __FILE__,         \
                   __LINE__, #a, _a, (unsigned long)_a, _b);                                       \
        }                                                                                          \
    } while (0)
#undef printf

/* --- mocks ------------------------------------------------------------------------------------ */

int host_cpu;
int boot_cpu_idx = 0;
bool hv_started_cpus[MAX_CPUS];
struct hv_win_state hv_win;

static u64 mpidr_of[MAX_CPUS];
static bool alive[MAX_CPUS];
static int log_lines;
static char last_log[256];

int host_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_log, sizeof(last_log), fmt, ap);
    va_end(ap);
    log_lines++;
    if (verbose)
        fputs(last_log, stdout);
    return 0;
}

bool smp_is_alive(int cpu)
{
    return cpu >= 0 && cpu < MAX_CPUS && alive[cpu];
}

u64 smp_get_mpidr(int cpu)
{
    return mpidr_of[cpu] & 0xffffff; /* the spin table keeps Aff2..Aff0 */
}

static u32 ipi_mask;
void smp_send_ipi(int cpu)
{
    ipi_mask |= 1U << cpu;
}

#define RAM_BASE 0x10000000000UL
#define RAM_SIZE 0x100000000UL
bool hv_pt_is_ram(u64 addr)
{
    return addr >= RAM_BASE && addr < RAM_BASE + RAM_SIZE;
}

void dc_civac_range(void *addr, size_t length)
{
    (void)addr;
    (void)length;
}

static int starts, start_cpu, start_result, flushes;
static u64 start_entry, start_x0;
static bool saw_received_before_start;
int hv_start_secondary_bounded(int cpu, void *entry, u64 regs[4])
{
    starts++;
    start_cpu = cpu;
    start_entry = (u64)entry;
    start_x0 = regs[0];
    saw_received_before_start = strstr(last_log, "CPU_ON received") != NULL;
    if (start_result)
        return start_result;         /* smp2b: bounded wait timed out */
    hv_started_cpus[cpu] = true; /* hv.c sets it before entering the guest */
    return 0;
}

void iodev_console_flush(void)
{
    flushes++;
}

static u32 exit_requests;
void hv_exit_cpu(int cpu)
{
    if (cpu == -1)
        cpu = host_cpu;
    exit_requests |= 1U << cpu;
}

void hv_win_pmu_report(void)
{
}

/* smp2d: psci_system_exit() now also dumps the WFI/debug/integrity counters and the exit ring */
void hv_win_wfi_report(void)
{
}

void hv_win_diag_dump(void)
{
}

/* smp2f: and the SMP dump (boot CPU only in the firmware); counted so the exit path is checked */
static int smp_dumps;
int hv_win_smp_dump(const char *why)
{
    (void)why;
    smp_dumps++;
    return 0;
}

void psci_host_reset(void);

/* --- helpers ---------------------------------------------------------------------------------- */

static void world(u32 nr_vcpus)
{
    memset(&hv_win, 0, sizeof(hv_win));
    memset(hv_started_cpus, 0, sizeof(hv_started_cpus));
    memset(alive, 0, sizeof(alive));
    for (int i = 0; i < MAX_CPUS; i++)
        hv_win.vcpu_cpu[i] = hv_win.cpu_vcpu[i] = -1;
    for (int i = 0; i < 3; i++) {
        mpidr_of[i] = 0x80040000UL | i;
        alive[i] = true;
    }
    for (u32 v = 0; v < nr_vcpus; v++)
        hv_win.vcpu_cpu[v] = hv_win.cpu_vcpu[v] = v;
    hv_win.nr_vcpus = nr_vcpus;
    hv_win.reserved_cpu = -1;
    hv_win.irq_model = HV_WIN_IRQ_VGIC3;
    psci_host_reset();
    hv_started_cpus[0] = true;
    host_cpu = 0;
    starts = 0;
    start_result = 0;
    flushes = 0;
    saw_received_before_start = false;
    exit_requests = 0;
    ipi_mask = 0;
    log_lines = 0;
}

/* One HVC #0: returns x0 as the guest sees it (hv_win_handle_hvc writes NOT_SUPPORTED if unhandled) */
static long call(u64 fid, u64 a1, u64 a2, u64 a3)
{
    struct exc_info ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.regs[0] = fid;
    ctx.regs[1] = a1;
    ctx.regs[2] = a2;
    ctx.regs[3] = a3;
    if (!hv_win_psci_call(&ctx))
        ctx.regs[0] = (u64)-1;
    return (long)ctx.regs[0];
}

#define SMCCC_VERSION     0x80000000UL
#define TRNG_VERSION      0x84000050UL
#define PSCI_VERSION      0x84000000UL
#define PSCI_CPU_SUSPEND  0xc4000001UL
#define PSCI_CPU_OFF      0x84000002UL
#define PSCI_CPU_ON_32    0x84000003UL
#define PSCI_CPU_ON_64    0xc4000003UL
#define PSCI_AFF_INFO_64  0xc4000004UL
#define PSCI_SYSTEM_OFF   0x84000008UL
#define PSCI_FEATURES     0x8400000aUL
#define ENTRY             (RAM_BASE + 0xe8ef010UL)

/* --- tests ------------------------------------------------------------------------------------ */

static void test_windows_boot_sequence(void)
{
    world(2);

    CHECK_EQ(call(SMCCC_VERSION, 0, 0, 0), 0x10001);
    CHECK_EQ(call(TRNG_VERSION, 0, 0, 0), -1); /* not ours: NOT_SUPPORTED, i.e. no TRNG */
    CHECK_EQ(call(PSCI_VERSION, 0, 0, 0), 0x10001);

    static const struct {
        u64 fid;
        long ret;
    } features[] = {
        {0xc4000001, 0},  /* CPU_SUSPEND_64: original power_state format (standby only) */
        {0x8400000b, -1}, /* CPU_FREEZE */
        {0xc400000c, -1}, /* CPU_DEFAULT_SUSPEND_64 */
        {0x8400000f, -1}, /* PSCI_SET_SUSPEND_MODE */
        {0xc4000010, -1}, /* PSCI_STAT_RESIDENCY_64 */
        {0xc4000011, -1}, /* PSCI_STAT_COUNT_64 */
        {0xc4000012, -1}, /* SYSTEM_RESET2_64 */
        {0x84000013, -1}, /* MEM_PROTECT */
        {0xc4000014, -1}, /* MEM_PROTECT_CHECK_RANGE_64 */
        {0x80000000, 0},  /* SMCCC_VERSION */
        {0xc4000003, 0},  /* CPU_ON_64 */
        {0xc400000e, -1}, /* SYSTEM_SUSPEND_64: never offered (hazards: no suspend) */
    };
    for (unsigned i = 0; i < sizeof(features) / sizeof(features[0]); i++)
        CHECK_EQ(call(PSCI_FEATURES, features[i].fid, 0, 0), features[i].ret);

    /* AFFINITY_INFO before: CPU 1 off, boot CPU on */
    CHECK_EQ(call(PSCI_AFF_INFO_64, 0x40001, 0, 0), 1);
    CHECK_EQ(call(PSCI_AFF_INFO_64, 0x40000, 0, 0), 0);

    /* CPU_ON with the MADT MPIDR (Mu publishes Aff2.Aff1.Aff0 without bit 31) */
    int lines = log_lines;
    CHECK_EQ(call(PSCI_CPU_ON_64, 0x40001, ENTRY, 0x4000c000), 0);
    CHECK_EQ(starts, 1);
    CHECK_EQ(start_cpu, 1);
    CHECK_EQ(start_entry, ENTRY);
    CHECK_EQ(start_x0, 0x4000c000);
    CHECK(log_lines > lines); /* the CPU_ON line */
    CHECK(strstr(last_log, "CPU_ON") != NULL);

    /* AFFINITY_INFO after, the second CPU's own calls, a repeated CPU_ON */
    CHECK_EQ(call(PSCI_AFF_INFO_64, 0x40001, 0, 0), 0);
    host_cpu = 1;
    CHECK_EQ(call(PSCI_VERSION, 0, 0, 0), 0x10001);
    CHECK_EQ(call(PSCI_FEATURES, SMCCC_VERSION, 0, 0), 0);
    host_cpu = 0;
    CHECK_EQ(call(PSCI_CPU_ON_64, 0x80040001, ENTRY, 0), -4); /* ALREADY_ON (bit 31 ignored) */
    CHECK_EQ(call(PSCI_CPU_ON_64, 0x40000, ENTRY, 0), -4);    /* the boot CPU */
    CHECK_EQ(starts, 1);
    CHECK_EQ(hv_win.psci_calls, 22); /* TRNG_VERSION is not counted */
}

static void test_cpu_on_refusals(void)
{
    world(2);
    CHECK_EQ(call(PSCI_CPU_ON_64, 0x40002, ENTRY, 0), -2);           /* alive, not a vCPU */
    CHECK_EQ(call(PSCI_CPU_ON_64, 0x40007, ENTRY, 0), -2);           /* no such CPU */
    CHECK_EQ(call(PSCI_CPU_ON_64, 0x100040001UL, ENTRY, 0), -2);     /* Aff3 != 0 */
    CHECK_EQ(call(PSCI_CPU_ON_64, 0x40001, ENTRY + 2, 0), -9);       /* misaligned */
    CHECK_EQ(call(PSCI_CPU_ON_64, 0x40001, 0, 0), -9);               /* null */
    CHECK_EQ(call(PSCI_CPU_ON_64, 0x40001, 0x80000000UL, 0), -9);    /* not guest RAM */
    CHECK_EQ(starts, 0);

    /* a vCPU-table CPU that never reached m1n1's spin table */
    alive[1] = false;
    CHECK_EQ(call(PSCI_CPU_ON_64, 0x40001, ENTRY, 0), -2); /* unknown: not alive -> no match */
    alive[1] = true;

    /* reserved for the host */
    hv_win.reserved_cpu = 1;
    CHECK_EQ(call(PSCI_CPU_ON_64, 0x40001, ENTRY, 0), -2);
    hv_win.reserved_cpu = -1;

    /* SMC32 CPU_ON: arguments truncated to 32 bits (entry above 4 GiB cannot be named) */
    CHECK_EQ(call(PSCI_CPU_ON_32, 0xffffffff00040001UL, ENTRY, 0), -9);
    CHECK_EQ(starts, 0);
}

static void test_suspend_off_and_restart(void)
{
    world(2);
    CHECK_EQ(call(PSCI_CPU_SUSPEND, 0x40000000, 0, 0), -2); /* power-down: refused */
    CHECK_EQ(call(PSCI_CPU_SUSPEND, 0, 0, 0), 0);           /* standby: returns at once */

    CHECK_EQ(call(PSCI_CPU_OFF, 0, 0, 0), -3); /* boot CPU: DENIED */
    CHECK_EQ(exit_requests, 0);

    CHECK_EQ(call(PSCI_CPU_ON_64, 0x40001, ENTRY, 0), 0);
    host_cpu = 1;
    CHECK_EQ(call(PSCI_CPU_OFF, 0, 0, 0), 0);
    CHECK_EQ(exit_requests, BIT(1));
    host_cpu = 0;
    hv_started_cpus[1] = false; /* hv_enter_secondary after the exit */
    CHECK_EQ(call(PSCI_AFF_INFO_64, 0x40001, 0, 0), 1);
    /* hv_should_exit stays set for that core: a restart is refused instead of exiting at once */
    CHECK_EQ(call(PSCI_CPU_ON_64, 0x40001, ENTRY, 0), -6);
    CHECK_EQ(starts, 1);
}

static void test_system_off(void)
{
    world(2);
    CHECK_EQ(call(PSCI_CPU_ON_64, 0x40001, ENTRY, 0), 0);
    host_cpu = 1;
    int dumps_before = smp_dumps;
    call(PSCI_SYSTEM_OFF, 0, 0, 0);
    CHECK_EQ(hv_win.exit_reason, HV_WIN_EXIT_SYSTEM_OFF);
    CHECK_EQ(smp_dumps, dumps_before + (hv_win.nr_vcpus > 1 ? 1 : 0)); /* smp2f */
    CHECK_EQ(exit_requests, BIT(0) | BIT(1));
    CHECK_EQ(ipi_mask, BIT(0)); /* the other started CPU is kicked */
}

/* smp2b: a secondary that does not answer within the bound: CPU_ON fails, nothing hangs */
static void test_cpu_on_start_timeout(void)
{
    static const int results[] = {-1, -2};
    for (unsigned i = 0; i < 2; i++) {
        world(2);
        start_result = results[i];
        CHECK_EQ(call(PSCI_CPU_ON_64, 0x40001, ENTRY, 0x4000c000), -6); /* INTERNAL_FAILURE */
        CHECK_EQ(starts, 1);
        CHECK(saw_received_before_start); /* the request is logged and flushed before the start */
        CHECK(flushes > 0);
        CHECK(!hv_started_cpus[1]);
        CHECK_EQ(call(PSCI_AFF_INFO_64, 0x40001, 0, 0), 1); /* still OFF */
        /* retired: a second CPU_ON does not touch the core again */
        start_result = 0;
        CHECK_EQ(call(PSCI_CPU_ON_64, 0x40001, ENTRY, 0), -6);
        CHECK_EQ(starts, 1);
    }
}

static void test_single_vcpu_quiet(void)
{
    world(1);
    CHECK_EQ(call(SMCCC_VERSION, 0, 0, 0), 0x10001);
    CHECK_EQ(call(PSCI_VERSION, 0, 0, 0), 0x10001);
    CHECK_EQ(call(PSCI_FEATURES, PSCI_CPU_SUSPEND, 0, 0), 0);
    CHECK_EQ(call(TRNG_VERSION, 0, 0, 0), -1);
    CHECK_EQ(log_lines, 0); /* the parent's console: no per-call lines with one vCPU */
    CHECK_EQ(call(PSCI_CPU_ON_64, 0x40001, ENTRY, 0), -2);
    CHECK_EQ(starts, 0);
    CHECK_EQ(hv_win.psci_calls, 4);
}

/*
 * smpN: the full J716s spin table (cluster 0 = 0.4.0-5 boot cluster, cluster 1 = 0.4.1xx, cluster 2 =
 * 0.1.2xx) with N = 6 (cluster 0) and N = 18 vCPUs. Windows names each core by its MADT MPIDR (bit 31
 * clear); every vCPU must start, nothing outside the table may.
 */
static u64 j716s_mpidr(int cpu)
{
    static const u64 cl[3] = {0x80040000, 0x80040100, 0x80010200};
    return cl[cpu / 6] | (cpu % 6);
}

static void smpn_cpu_on_all(u32 n)
{
    world(n);
    for (int i = 0; i < 18; i++) {
        mpidr_of[i] = j716s_mpidr(i);
        alive[i] = true;
    }
    for (u32 v = 1; v < n; v++) {
        u64 madt = j716s_mpidr(v) & 0xffffff;
        CHECK_EQ(call(PSCI_AFF_INFO_64, madt, 0, 0), 1);
        CHECK_EQ(call(PSCI_CPU_ON_64, madt, ENTRY, 0x4000c000 + v), 0);
        CHECK_EQ(start_cpu, v);
        CHECK_EQ(start_x0, 0x4000c000 + v);
        CHECK_EQ(call(PSCI_AFF_INFO_64, madt, 0, 0), 0);
        CHECK_EQ(call(PSCI_CPU_ON_64, madt | 0x80000000UL, ENTRY, 0), -4); /* ALREADY_ON */
    }
    CHECK_EQ(starts, (int)n - 1);
    /* cores outside the vCPU table, and affinities that exist on no cluster */
    for (int i = n; i < 18; i++)
        CHECK_EQ(call(PSCI_CPU_ON_64, j716s_mpidr(i) & 0xffffff, ENTRY, 0), -2);
    CHECK_EQ(call(PSCI_CPU_ON_64, 0x40200, ENTRY, 0), -2); /* Aff2 4 Aff1 2: cluster 2 is Aff2 1 */
    CHECK_EQ(call(PSCI_CPU_ON_64, 0x10000, ENTRY, 0), -2);
    CHECK_EQ(call(PSCI_CPU_ON_64, 0x40006, ENTRY, 0), -2);
    CHECK_EQ(starts, (int)n - 1);
    /* SYSTEM_RESET from the last vCPU: every started CPU leaves, the others are kicked */
    host_cpu = n - 1;
    exit_requests = ipi_mask = 0;
    call(0x84000009UL, 0, 0, 0);
    CHECK_EQ(exit_requests, (u32)(BIT(n) - 1));
    CHECK_EQ(ipi_mask, (u32)(BIT(n - 1) - 1));
    host_cpu = 0;
}

static void test_smpn_cpu_on_6(void)
{
    smpn_cpu_on_all(6);
}

static void test_smpn_cpu_on_18(void)
{
    smpn_cpu_on_all(18);
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v");

#define RUN(t)                                                                                     \
    do {                                                                                           \
        int before = fails;                                                                        \
        cur_test = #t;                                                                             \
        t();                                                                                       \
        printf("%-40s %s\n", #t, fails == before ? "ok" : "FAILED");                               \
    } while (0)

    RUN(test_windows_boot_sequence);
    RUN(test_cpu_on_refusals);
    RUN(test_suspend_off_and_restart);
    RUN(test_system_off);
    RUN(test_cpu_on_start_timeout);
    RUN(test_single_vcpu_quiet);
    RUN(test_smpn_cpu_on_6);
    RUN(test_smpn_cpu_on_18);

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
