/* SPDX-License-Identifier: MIT */
/*
 * QEMU virt EL2 harness for the stage-2 PMU rewrite (tests/win-pmu-rewrite/qemu).
 *
 * Runs the committed code: m1n1's hv_vm.c execute-tracking block (xt.c, extracted verbatim by
 * extract-xt.sh: descriptor macros, hv_pt_get_l2/l3, hv_xt_*) and src/hv/win/hv_win_pmu.c +
 * hv_win_pmu_rewrite.c, under QEMU's EL2 with VHE (E2H = 1, as m1n1), a 16 KiB-granule stage 2 in
 * m1n1's layout, and an EL1 guest (guest.S). QEMU's TCG reads PMU registers as zero with pmu=off
 * instead of UNDEFing like the T6050, so MDCR_EL2.TPM|TPMCR trap every PMU access to EL2 and any
 * such trap fails the run: an access the rewrite missed is caught at its first execution. What
 * QEMU cannot say: Apple's cache and TLB behaviour (QEMU models no caches), SMP, timing.
 */

#include <stdarg.h>

#include "hv_win.h"
#include "pmu_host.h"

#define UART      0x09000000UL
#define GUEST_RAM 0x44000000UL
#define GUEST_SZ  0x08000000UL /* 128 MiB = 4 L2 blocks of 32 MiB */

#define SCTLR_M BIT(0)
#define PAR_F   BIT(0)
#define PAR_PA  GENMASK(51, 12)

void bm_exit(int status) __attribute__((noreturn));
void guest_enter(u64 entry) __attribute__((noreturn));
extern char guest_start[];

static void bm_panic(const char *what) __attribute__((noreturn));

#define assert(x)                                                                                  \
    do {                                                                                           \
        if (!(x))                                                                                  \
            bm_panic("assert: " #x);                                                               \
    } while (0)

/* ---- console, libc bits ------------------------------------------------------------------------
 */

static void putc_uart(char c)
{
    volatile u32 *uart = (volatile u32 *)UART;
    while (uart[0x18 / 4] & (1 << 5))
        ;
    uart[0] = (u32)c;
}

static void puts_uart(const char *s)
{
    while (*s) {
        if (*s == '\n')
            putc_uart('\r');
        putc_uart(*s++);
    }
}

static void put_num(u64 v, int base, int width, char pad)
{
    char buf[24];
    int n = 0;
    do {
        buf[n++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v);
    while (n < width--)
        putc_uart(pad);
    while (n)
        putc_uart(buf[--n]);
}

int bm_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            if (*p == '\n')
                putc_uart('\r');
            putc_uart(*p);
            continue;
        }
        p++;
        char pad = ' ';
        int width = 0;
        if (*p == '0')
            pad = '0';
        while (*p >= '0' && *p <= '9')
            width = width * 10 + (*p++ - '0');
        bool lng = false;
        while (*p == 'l') {
            lng = true;
            p++;
        }
        switch (*p) {
            case 'x':
                put_num(lng ? va_arg(ap, u64) : va_arg(ap, u32), 16, width, pad);
                break;
            case 'u':
                put_num(lng ? va_arg(ap, u64) : va_arg(ap, u32), 10, width, pad);
                break;
            case 'd': {
                s64 v = lng ? va_arg(ap, s64) : va_arg(ap, int);
                if (v < 0) {
                    putc_uart('-');
                    v = -v;
                }
                put_num((u64)v, 10, width, pad);
                break;
            }
            case 's':
                puts_uart(va_arg(ap, const char *));
                break;
            case 'c':
                putc_uart((char)va_arg(ap, int));
                break;
            default:
                putc_uart(*p);
        }
    }
    va_end(ap);
    return 0;
}

static void bm_panic(const char *what)
{
    bm_printf("EL2 PANIC: %s\n", what);
    bm_exit(2);
}

void *memset(void *s, int c, size_t n)
{
    u8 *p = s;
    while (n--)
        *p++ = (u8)c;
    return s;
}

void *memcpy(void *d, const void *s, size_t n)
{
    u8 *dp = d;
    const u8 *sp = s;
    while (n--)
        *dp++ = *sp++;
    return d;
}

static void memset64(void *dst, u64 value, size_t size)
{
    for (u64 *p = dst; size >= 8; size -= 8)
        *p++ = value;
}

/* heap for stage-2 tables (the m1n1 heapblock/dlmalloc stand-in) */
static u8 heap[48 * 0x4000] __attribute__((aligned(0x4000)));
static size_t heap_used;

static void *memalign(size_t align, size_t size)
{
    size_t start = (heap_used + align - 1) & ~(align - 1);
    if (start + size > sizeof(heap))
        bm_panic("harness heap exhausted");
    heap_used = start + size;
    return heap + start;
}

size_t heapblock_remaining(void)
{
    return sizeof(heap) - heap_used + (16UL << 20); /* + hv_win_pmu.c's 16 MiB margin */
}

void dc_civac_range(void *addr, size_t length)
{
    for (u64 p = (u64)addr; p < (u64)addr + length; p += 64)
        __asm__ volatile("dc civac, %0" : : "r"(p) : "memory");
}

void dc_cvac_range(void *addr, size_t length)
{
    for (u64 p = (u64)addr; p < (u64)addr + length; p += 64)
        __asm__ volatile("dc cvac, %0" : : "r"(p) : "memory");
}

struct hv_win_state hv_win;
u64 hv_cpus_in_guest;
struct boot_args cur_boot_args;

/* ---- the committed hv_vm.c code ----------------------------------------------------------------
 */

#include "xt.c"

/* m1n1's hv_map() for HW RAM: L2 blocks (what HV.start()'s pt_update leaves for aligned RAM) */
#define PTE_ATTRIBUTES (PTE_ACCESS | PTE_SH_NS | PTE_S2AP_RW | PTE_MEMATTR_UNCHANGED)

static void map_ram_blocks(u64 base, u64 size)
{
    u64 *l2 = hv_pt_get_l2(base);
    for (u64 a = base; a < base + size; a += BIT(VADDR_L2_OFFSET_BITS))
        l2[(a >> VADDR_L2_OFFSET_BITS) & MASK(VADDR_L2_INDEX_BITS)] =
            a | PTE_ATTRIBUTES | PTE_VALID | FIELD_PREP(PTE_TYPE, PTE_BLOCK);
}

/* ---- guest exits --------------------------------------------------------------------------------
 */

#define EC_HVC          0x16
#define EC_MSR          0x18
#define EC_IABORT_LOWER 0x20
#define EC_DABORT_LOWER 0x24

static int checks, failures;

static void check_stat(const char *what, enum hv_win_pmu_stat idx, u64 lo, u64 hi)
{
    u64 v = hv_win_pmu_stat(idx);
    bool ok = v >= lo && v <= hi;
    checks++;
    failures += !ok;
    bm_printf("XT-CHECK %s stat=%lu want=%lu..%lu %s\n", what, v, lo, hi, ok ? "PASS" : "FAIL");
}

static void finish(void)
{
    hv_win_pmu_report();
    /* page A: 6 PMU words; page B: 1 (written by the guest after its first run); page E: 1 (through
     * a VA alias, MMU on); main page: 1 data word that looks like mrs PMCCNTR (the documented risk)
     */
    check_stat("insns-rewritten", HV_WIN_PMU_INSNS, 9, 9);
    check_stat("pmccntr", HV_WIN_PMU_INSNS_PMCCNTR, 5, 5);
    check_stat("other-mrs", HV_WIN_PMU_INSNS_MRS, 2, 2);
    check_stat("msr", HV_WIN_PMU_INSNS_MSR, 2, 2);
    check_stat("pinned (page C)", HV_WIN_PMU_PINS, 1, 1);
    check_stat("walker flips (page D)", HV_WIN_PMU_WALK_FLIPS, 1, 1);
    check_stat("write flips", HV_WIN_PMU_WRITE_FLIPS, 4, 8);
    check_stat("stuck", HV_WIN_PMU_STUCK, 0, 0);
    check_stat("not ours", HV_WIN_PMU_NOT_OURS, 0, 0);
    bm_printf("XT-RESULT %d checks, %d failed: %s\n", checks, failures, failures ? "FAIL" : "PASS");
    bm_exit(failures ? 1 : 0);
}

/* x0..x30 saved by the vector stub; returns with ctx->elr/spsr to ERET to */
void el2_sync(struct exc_info *ctx)
{
    ctx->esr = mrs(ESR_EL2);
    ctx->elr = mrs(ELR_EL2);
    ctx->far = mrs(FAR_EL2);
    ctx->spsr = mrs(SPSR_EL2);
    u32 ec = (ctx->esr >> 26) & 0x3f;
    bool ok = false;

    switch (ec) {
        case EC_HVC: {
            u32 imm = ctx->esr & 0xffff;
            ok = true;
            if (imm == 0 && ctx->regs[0] == 0x84000008) {
                bm_printf("guest: SYSTEM_OFF\n");
                finish();
            } else if (imm == M5W_HVC_DEBUG_PUTC) {
                putc_uart((char)ctx->regs[0]);
            } else if (imm == 0x4d40) { /* guest check: x0 id, x1 pass, x2 value */
                checks++;
                failures += !ctx->regs[1];
                bm_printf("XT-CHECK guest T%lu value=0x%lx %s\n", ctx->regs[0], ctx->regs[2],
                          ctx->regs[1] ? "PASS" : "FAIL");
            } else if (imm == M5W_HVC_FAULT_REPORT) {
                bm_printf("guest: EL1 exception ESR 0x%lx ELR 0x%lx (a PMU access the rewrite "
                          "missed?)\n",
                          ctx->regs[1], ctx->regs[2]);
                failures++;
                finish();
            } else {
                ok = false;
            }
            break; /* ELR already past the HVC */
        }
        case EC_MSR:
            bm_printf("XT-CHECK unrewritten PMU access (ISS 0x%lx) at PC 0x%lx FAIL\n",
                      ctx->esr & 0x1ffffff, ctx->elr);
            checks++;
            failures++;
            finish();
            break;
        case EC_IABORT_LOWER:
            ok = hv_win_handle_iabort(ctx); /* re-execute: ELR unchanged */
            break;
        case EC_DABORT_LOWER:
            ok = hv_win_handle_dabort(ctx, ctx->esr); /* re-execute: ELR unchanged */
            break;
    }

    if (!ok) {
        bm_printf("EL2: unhandled guest exit ESR 0x%lx ELR 0x%lx FAR 0x%lx HPFAR 0x%lx\n", ctx->esr,
                  ctx->elr, ctx->far, mrs(HPFAR_EL2));
        failures++;
        finish();
    }
    msr(ELR_EL2, ctx->elr);
    msr(SPSR_EL2, ctx->spsr);
}

void el2_bad_vector(u64 idx)
{
    bm_printf("EL2: exception at vector %lu ESR 0x%lx ELR 0x%lx FAR 0x%lx\n", idx, mrs(ESR_EL2),
              mrs(ELR_EL2), mrs(FAR_EL2));
    bm_exit(2);
}

void el2_main(void)
{
    bm_printf("win-pmu-rewrite/qemu: stage-2 PMU rewrite harness, EL2 with VHE, 16 KiB stage 2\n");

    /* m1n1's hv_pt_init() for a 36-bit IPA: start at L2, 16 KiB granule */
    vaddr_bits = 36;
    hv_Ltop = memalign(PAGE_SIZE, sizeof(u64) * ENTRIES_PER_L2_TABLE);
    memset(hv_Ltop, 0, sizeof(u64) * ENTRIES_PER_L2_TABLE);
    msr(VTCR_EL2, (1UL << 16) |     /* PS 36 bits */
                      (2UL << 14) | /* TG0 16 KiB */
                      (3UL << 12) | /* SH0 inner shareable */
                      (1UL << 10) | /* ORGN0 WB */
                      (1UL << 8) |  /* IRGN0 WB */
                      (1UL << 6) |  /* SL0: start at level 2 */
                      (64 - 36));   /* T0SZ */
    msr(VTTBR_EL2, (u64)hv_Ltop);
    map_ram_blocks(GUEST_RAM, GUEST_SZ);
    hv_xt_tlbi(true);

    /* the Windows personality, configured; arm the rewrite on all guest RAM */
    hv_win.irq_model = HV_WIN_IRQ_AIC_PASSTHROUGH;
#ifdef NO_ARM /* negative control: the guest's first PMU access must UNDEF at EL1 */
    int pages = GUEST_SZ / 0x4000;
    bm_printf("NO_ARM: rewrite not armed\n");
#else
    int pages = hv_win_pmu_arm(GUEST_RAM, GUEST_SZ);
#endif
    bm_printf("armed: %d pages, heap used %lu KiB\n", pages, (u64)heap_used >> 10);
    checks++;
    if (pages != (int)(GUEST_SZ / 0x4000)) {
        failures++;
        bm_printf("XT-CHECK arm FAIL\n");
    } else {
        bm_printf("XT-CHECK arm PASS\n");
    }

    /* RW, E2H (as m1n1), VM; TGE clear; every PMU access traps here (TPM | TPMCR) */
    msr(HCR_EL2, BIT(31) | BIT(34) | BIT(0));
    msr(MDCR_EL2, BIT(6) | BIT(5));
    sysop("isb");
    /* guest EL1 state through the VHE _EL12 names; MMU off, caches on */
    msr(SCTLR_EL12, 0x30d01804UL | BIT(12) | BIT(2));
    sysop("isb");
    guest_enter((u64)guest_start);
}
