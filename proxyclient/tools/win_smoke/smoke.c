/* SPDX-License-Identifier: MIT */
/*
 * win_smoke: a tiny EL1 guest for the Windows EL2 runtime (m5pro-windows MILESTONES M0 steps 0 and 3).
 * It answers, in one attended run, what the Mu bring-up needs to know about the EL1 environment our
 * m1n1 hypervisor gives a non-XNU guest on the T6050 (OPEN-QUESTIONS Q4, Q6, Q7, Q8 and M0-RUNTIME-PLAN
 * open item 1) and whether Windows' unconditional PMUv3 accesses trap to EL2 or UNDEF at EL1 (EL2-RUNTIME
 * item 6, phase 12), then ends with PSCI SYSTEM_OFF over HVC.
 *
 * Output: one line per fact on the m1n1 console through HVC #0x4d35 (M5W_DEBUG_PUTC), every line starting
 * with "M5W-SMOKE ", fields as key=value; tools/win_smoke/smoke_report.py turns a log into answers.
 * Banner lines also go to the Samsung vuart (x1) and the emulated PL011 (x2), i.e. the second CDC port.
 *
 * Runs with the MMU and caches off: every data access is Device-nGnRnE, so everything here is naturally
 * aligned (built with -mstrict-align) and nothing uses exclusives or FP/SIMD.
 */

typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

#define BIT(n) (1UL << (n))

#define M5W_SMOKE_VERSION 2
#define SMOKE_X3_MAGIC    0x4d355753UL

#define PSCI_VERSION       0x84000000UL
#define PSCI_SYSTEM_OFF    0x84000008UL
#define PSCI_FEATURES      0x8400000aUL
#define PSCI_SYSTEM_RESET2 0xc4000012UL

#define MU_HANDOFF_BASE 0x10400000000UL

#define CNT_CTL_ENABLE  BIT(0)
#define CNT_CTL_IMASK   BIT(1)
#define CNT_CTL_ISTATUS BIT(2)

#define SPSR_F BIT(6)
#define SPSR_I BIT(7)

#define sysreg_read(reg)                                                                           \
    ({                                                                                             \
        u64 __v;                                                                                   \
        __asm__ volatile("mrs %0, " reg : "=r"(__v)::"memory");                                    \
        __v;                                                                                       \
    })
#define sysreg_write(reg, val) __asm__ volatile("msr " reg ", %0" ::"r"((u64)(val)) : "memory")
#define isb()                  __asm__ volatile("isb" ::: "memory")

extern u64 entry_regs[12]; /* x0..x7, SCTLR_EL1, CurrentEL, DAIF, SP at entry (start.S) */
void wfi_probe(u64 out[32], u64 seed);

/* ---- exception plumbing ---------------------------------------------------------------------------- */

struct frame {
    u64 x[31];
    u64 elr;
    u64 spsr;
    u64 pad;
};

static volatile u64 g_expect;  /* the next synchronous exception is a probed UNDEF: skip the instruction */
static volatile u64 g_faulted; /* set by the handler when it skipped one */
static volatile u64 g_esr;
static volatile u64 g_phase;   /* printed with any unexpected exception */
static volatile u64 g_fiq_count, g_irq_count, g_fiq_t, g_irq_t, g_fiq_ctl, g_irq_isr;

static __attribute__((noinline)) void putc_hvc(u64 c)
{
    register u64 x0 __asm__("x0") = c;
    __asm__ volatile("hvc #0x4d35" : "+r"(x0)::"memory");
}

static __attribute__((noinline)) void puts_(const char *s)
{
    while (*s)
        putc_hvc((u8)*s++);
}

static void puthex(u64 v)
{
    char buf[19];
    int i = 18;

    buf[i] = 0;
    do {
        u64 d = v & 0xf;
        buf[--i] = d < 10 ? '0' + d : 'a' + d - 10;
        v >>= 4;
    } while (v);
    buf[--i] = 'x';
    buf[--i] = '0';
    puts_(&buf[i]);
}

static void putdec(u64 v)
{
    char buf[21];
    int i = 20;

    buf[i] = 0;
    do {
        buf[--i] = '0' + v % 10;
        v /= 10;
    } while (v);
    puts_(&buf[i]);
}

static __attribute__((noinline)) void begin(const char *tag)
{
    puts_("M5W-SMOKE ");
    puts_(tag);
}

static void end(void)
{
    putc_hvc('\n');
}

static __attribute__((noinline)) void kx(const char *k, u64 v)
{
    putc_hvc(' ');
    puts_(k);
    putc_hvc('=');
    puthex(v);
}

static __attribute__((noinline)) void kd(const char *k, u64 v)
{
    putc_hvc(' ');
    puts_(k);
    putc_hvc('=');
    putdec(v);
}

static __attribute__((noinline)) void ks(const char *k, const char *v)
{
    putc_hvc(' ');
    puts_(k);
    putc_hvc('=');
    puts_(v);
}

static u64 hvc_call(u64 fid, u64 a1)
{
    register u64 x0 __asm__("x0") = fid;
    register u64 x1 __asm__("x1") = a1;
    register u64 x2 __asm__("x2") = 0;
    register u64 x3 __asm__("x3") = 0;
    __asm__ volatile("hvc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)::"memory");
    return x0;
}

static void system_off(void)
{
    hvc_call(PSCI_SYSTEM_OFF, 0);
    for (;;)
        __asm__ volatile("wfe");
}

static void put_vector(u64 idx)
{
    /* if/else, not a switch: a switch over strings becomes a table of absolute pointers */
    u64 kind = idx & 3;

    if (kind == 0)
        ks("vec", "sync");
    else if (kind == 1)
        ks("vec", "irq");
    else if (kind == 2)
        ks("vec", "fiq");
    else
        ks("vec", "serror");
}

u64 exc_handler(struct frame *f, u64 idx);
u64 exc_handler(struct frame *f, u64 idx)
{
    u64 esr = sysreg_read("esr_el1");

    if (idx == 4 && g_expect) {
        /* a probed instruction faulted at EL1 (UNDEF, or anything else): skip it */
        g_esr = esr;
        g_faulted = 1;
        g_expect = 0;
        f->elr += 4;
        return 0;
    }

    if (idx == 6) {
        u64 ctl = sysreg_read("cntp_ctl_el0");
        if (!g_fiq_count) {
            isb();
            g_fiq_t = sysreg_read("cntvct_el0");
            g_fiq_ctl = ctl;
        }
        g_fiq_count++;
        /* quiet the source: the hypervisor drops HCR_EL2.VF once the guest timer is disabled */
        sysreg_write("cntp_ctl_el0", 0);
        sysreg_write("cntv_ctl_el0", 0);
        isb();
        if (g_fiq_count > 1000)
            f->spsr |= SPSR_F; /* storm: return with FIQs masked */
        return 0;
    }

    if (idx == 5) {
        if (!g_irq_count) {
            isb();
            g_irq_t = sysreg_read("cntvct_el0");
            g_irq_isr = sysreg_read("isr_el1");
        }
        g_irq_count++;
        /* no interrupt controller is programmed here: return with IRQs masked */
        f->spsr |= SPSR_I;
        return 0;
    }

    begin("fatal");
    put_vector(idx);
    kd("idx", idx);
    kx("esr", esr);
    kx("elr", f->elr);
    kx("far", sysreg_read("far_el1"));
    kx("spsr", f->spsr);
    kd("phase", g_phase);
    end();
    system_off();
    return 0;
}

#define GUARD_BEGIN()                                                                              \
    do {                                                                                           \
        g_faulted = 0;                                                                             \
        g_esr = 0;                                                                                 \
        g_expect = 1;                                                                              \
        __asm__ volatile("" ::: "memory");                                                         \
    } while (0)

#define GUARD_END()                                                                                \
    do {                                                                                           \
        __asm__ volatile("" ::: "memory");                                                         \
        g_expect = 0;                                                                              \
    } while (0)

/* mrs/msr under the UNDEF guard; evaluate to 1 when the instruction completed */
#define TRY_MRS(reg, out)                                                                          \
    ({                                                                                             \
        u64 __v = 0;                                                                               \
        GUARD_BEGIN();                                                                             \
        __asm__ volatile("mrs %0, " reg : "+r"(__v)::"memory");                                    \
        GUARD_END();                                                                               \
        (out) = __v;                                                                               \
        !g_faulted;                                                                                \
    })

#define TRY_MSR(reg, val)                                                                          \
    ({                                                                                             \
        GUARD_BEGIN();                                                                             \
        __asm__ volatile("msr " reg ", %0" ::"r"((u64)(val)) : "memory");                          \
        GUARD_END();                                                                               \
        !g_faulted;                                                                                \
    })

static u64 vct(void)
{
    isb();
    return sysreg_read("cntvct_el0");
}

static u64 pct(void)
{
    isb();
    return sysreg_read("cntpct_el0");
}

static u64 g_freq;
static int g_counter_ok = 1; /* cleared when the virtual counter does not move */
static int g_timer_ok;       /* set when the 100 ms timer probe saw an interrupt */

#define SPIN_CAP 2000000000UL /* bounds waits on a counter that moves but far too slowly */

static u64 ticks_to_us(u64 t)
{
    return g_freq ? (t / g_freq) * 1000000 + (t % g_freq) * 1000000 / g_freq : 0;
}

/* Busy-wait `ticks` of the virtual counter. Returns 0 if the counter does not move at all. */
static int wait_ticks(u64 ticks)
{
    u64 t0 = vct(), n = 0;

    while (vct() - t0 < ticks) {
        if (++n == 1000000 && vct() == t0)
            return 0;
        if (n > SPIN_CAP)
            break;
    }
    return 1;
}

static inline u64 rd64(u64 addr)
{
    return *(volatile u64 *)addr;
}

static inline void wr64(u64 addr, u64 v)
{
    *(volatile u64 *)addr = v;
}

static inline u32 rd32(u64 addr)
{
    return *(volatile u32 *)addr;
}

static inline void wr32(u64 addr, u32 v)
{
    *(volatile u32 *)addr = v;
}

/* ---- the probes ---------------------------------------------------------------------------------- */

static void probe_entry(void)
{
    begin("begin");
    kd("v", M5W_SMOKE_VERSION);
    kx("launcher", entry_regs[3] == SMOKE_X3_MAGIC);
    end();

    begin("regs");
    kx("x0", entry_regs[0]);
    kx("x1", entry_regs[1]);
    kx("x2", entry_regs[2]);
    kx("x3", entry_regs[3]);
    kx("x4", entry_regs[4]);
    kx("x5", entry_regs[5]);
    kx("x6", entry_regs[6]);
    kx("x7", entry_regs[7]);
    kx("daif", entry_regs[10]);
    kx("sp", entry_regs[11]);
    end();
}

static void probe_id(void)
{
    u64 v;

    begin("id");
    kx("CurrentEL", entry_regs[9]);
    kd("el", (entry_regs[9] >> 2) & 3);
    kx("SCTLR_EL1", entry_regs[8]);
    kx("MIDR_EL1", sysreg_read("midr_el1"));
    kx("MPIDR_EL1", sysreg_read("mpidr_el1"));
    end();

    v = sysreg_read("id_aa64pfr0_el1");
    begin("id");
    kx("ID_AA64PFR0_EL1", v);
    kd("EL3", (v >> 12) & 0xf);
    kd("GIC", (v >> 24) & 0xf);
    kd("AdvSIMD", (v >> 20) & 0xf);
    end();

    v = sysreg_read("id_aa64dfr0_el1");
    begin("id");
    kx("ID_AA64DFR0_EL1", v);
    kx("PMUVer", (v >> 8) & 0xf);
    end();

    v = sysreg_read("id_aa64mmfr0_el1");
    begin("id");
    kx("ID_AA64MMFR0_EL1", v);
    kx("PARange", v & 0xf);
    kx("ECV", (v >> 60) & 0xf);
    end();

    v = sysreg_read("id_aa64pfr1_el1");
    begin("id");
    kx("ID_AA64PFR1_EL1", v);
    kd("MTE", (v >> 8) & 0xf);
    end();

    g_freq = sysreg_read("cntfrq_el0");
    begin("id");
    kd("CNTFRQ_EL0", g_freq);
    end();

    u64 t0 = vct();
    int ok = TRY_MRS("s3_0_c12_c12_5", v); /* ICC_SRE_EL1 */
    u64 t1 = vct();
    begin("sysreg");
    ks("reg", "ICC_SRE_EL1");
    ks("read", ok ? "ok" : "undef");
    if (ok)
        kx("val", v);
    else
        kx("esr", g_esr);
    kd("ticks", t1 - t0);
    end();
}

static void probe_bootargs(void)
{
    u64 ba = entry_regs[0];
    u64 hdr = rd64(ba);
    u64 rev = hdr & 0xffff, ver = (hdr >> 16) & 0xffff;

    begin("bootargs");
    kx("at", ba);
    kd("rev", rev);
    kd("ver", ver);
    kx("virt_base", rd64(ba + 8));
    kx("phys_base", rd64(ba + 16));
    kx("mem_size", rd64(ba + 24));
    kx("top_of_kdata", rd64(ba + 32));
    kx("devtree", rd64(ba + 96));
    kx("devtree_size", rd32(ba + 104));
    ks("mu_x0", (rev >= 1 && rev <= 3 && ver >= 1 && ver <= 3) ? "ok" : "REJECT");
    end();

    /* Q8: the iBoot framebuffer as boot_args describes it */
    begin("video");
    kx("base", rd64(ba + 40));
    kd("display", rd64(ba + 48));
    kx("stride", rd64(ba + 56));
    kd("width", rd64(ba + 64));
    kd("height", rd64(ba + 72));
    kx("depth", rd64(ba + 80));
    kd("bpp", rd64(ba + 80) & 0xff);
    end();
}

static void probe_counters(void)
{
    /* Q6: is the EL1 view of either counter frozen or offset-drifting? */
    u64 p0 = pct(), v0 = vct();
    int moving = wait_ticks(g_freq);
    u64 p1 = pct(), v1 = vct();

    begin("q6");
    kd("freq", g_freq);
    kx("pct0", p0);
    kx("vct0", v0);
    kx("pct1", p1);
    kx("vct1", v1);
    kd("dpct", p1 - p0);
    kd("dvct", v1 - v0);
    kx("voff0", p0 - v0);
    kx("voff1", p1 - v1);
    ks("frozen", moving ? "no" : "yes");
    end();

    if (!moving) {
        g_counter_ok = 0;
        return;
    }

    /* 5 s by the guest's counter, bracketed by two lines the launcher timestamps with the Studio clock */
    begin("q6");
    kd("mark", 0);
    kx("vct", vct());
    end();
    wait_ticks(5 * g_freq);
    begin("q6");
    kd("mark", 1);
    kx("vct", vct());
    kd("expect_us", 5000000);
    end();
}

static void probe_timer(void)
{
    /* EL1 physical timer, 100 ms, delivered the M0 way (vFIQ through hv_update_fiq) */
    __asm__ volatile("msr daifset, #3");
    if (!g_counter_ok) {
        begin("timer");
        ks("kind", "skipped-frozen-counter");
        end();
        return;
    }
    g_fiq_count = g_irq_count = 0;
    g_fiq_ctl = 0;
    sysreg_write("cntp_ctl_el0", 0);
    sysreg_write("cntp_tval_el0", g_freq / 10);
    u64 t0 = vct();
    sysreg_write("cntp_ctl_el0", CNT_CTL_ENABLE);
    isb();
    __asm__ volatile("msr daifclr, #3");
    for (u64 n = 0; !g_fiq_count && !g_irq_count && vct() - t0 < g_freq && n < SPIN_CAP; n++)
        ;
    __asm__ volatile("msr daifset, #3");
    u64 ctl_after = sysreg_read("cntp_ctl_el0");
    sysreg_write("cntp_ctl_el0", 0);
    isb();

    begin("timer");
    ks("kind", g_fiq_count ? "fiq" : (g_irq_count ? "irq" : "none"));
    kd("fiq", g_fiq_count);
    kd("irq", g_irq_count);
    if (g_fiq_count) {
        kd("latency_us", ticks_to_us(g_fiq_t - t0));
        kx("ctl_seen", g_fiq_ctl);
        /* Mu's FIQ handler services the tick only on ENABLE = 1, IMASK = 0, ISTATUS = 1 */
        ks("mu_check", (g_fiq_ctl & 7) == (CNT_CTL_ENABLE | CNT_CTL_ISTATUS) ? "ok" : "FAIL");
    } else if (g_irq_count) {
        kd("latency_us", ticks_to_us(g_irq_t - t0));
        kx("isr", g_irq_isr);
    }
    kx("ctl_after", ctl_after);
    kd("expect_us", 100000);
    end();
    g_timer_ok = g_fiq_count || g_irq_count;
}

static void apple_report(const char *name, const char *enc, int rok, u64 val, u64 resr, u64 rt, int wtried,
                         int wok, u64 wesr, u64 wt)
{
    begin("apple");
    ks("reg", enc);
    ks("name", name);
    ks("read", rok ? "ok" : "undef");
    if (rok)
        kx("val", val);
    else
        kx("esr", resr);
    kd("read_us", ticks_to_us(rt));
    if (wtried) {
        ks("write", wok ? "ok" : "undef");
        if (!wok)
            kx("wesr", wesr);
        kd("write_us", ticks_to_us(wt));
    }
    end();
}

/* read (and for the read/write ones, write back what Mu's AIC driver would) under the UNDEF guard */
#define APPLE_PROBE(enc, name, writeback, wval_expr)                                               \
    do {                                                                                           \
        u64 v = 0, t0, t1, t2 = 0, t3 = 0, resr = 0, wesr = 0;                                     \
        int rok, wok = 0;                                                                          \
        t0 = vct();                                                                                \
        rok = TRY_MRS(enc, v);                                                                     \
        t1 = vct();                                                                                \
        resr = g_esr;                                                                              \
        if ((writeback) && rok) {                                                                  \
            u64 w = (wval_expr);                                                                   \
            t2 = vct();                                                                            \
            wok = TRY_MSR(enc, w);                                                                 \
            t3 = vct();                                                                            \
            wesr = g_esr;                                                                          \
        }                                                                                          \
        apple_report(name, enc, rok, v, resr, t1 - t0, (writeback) && rok, wok, wesr, t3 - t2);   \
    } while (0)

static void probe_apple(void)
{
    /* M0-RUNTIME-PLAN open item 1: the registers Mu's AIC FIQ handler touches at EL1 */
    __asm__ volatile("msr daifset, #3");
    APPLE_PROBE("s3_5_c15_c1_1", "FAST_IPI_STATUS", 1, v & 1); /* W1C, as AppleAicV2 acks */
    APPLE_PROBE("s3_1_c15_c0_0", "PMCR0", 1, v);
    APPLE_PROBE("s3_7_c15_c0_4", "UPMCR0", 1, v);
    APPLE_PROBE("s3_7_c15_c6_4", "UPMSR", 0, 0);
}

static void probe_wfi(void)
{
    /* Q7: does WFI (no HCR_EL2.TWI) keep the general registers? Woken by a 10 ms timer, FIQ masked. */
    static u64 out[32];
    const u64 seed = 0x5157464900000000UL; /* "QWFI" */
    int mism = 0, first = -1;

    __asm__ volatile("msr daifset, #3");
    if (!g_timer_ok) {
        /* nothing would wake the WFI */
        begin("q7");
        ks("wfi", "skipped-no-timer-interrupt");
        end();
        return;
    }
    begin("q7");
    ks("wfi", "begin");
    end();
    sysreg_write("cntp_ctl_el0", 0);
    sysreg_write("cntp_tval_el0", g_freq / 100);
    sysreg_write("cntp_ctl_el0", CNT_CTL_ENABLE);
    isb();
    wfi_probe(out, seed);
    u64 ctl = sysreg_read("cntp_ctl_el0");
    sysreg_write("cntp_ctl_el0", 0);
    isb();

    for (int i = 1; i <= 28; i++) {
        if (out[i] != seed + (u64)i) {
            if (first < 0)
                first = i;
            mism++;
        }
    }

    begin("q7");
    kd("wfi_us", ticks_to_us(out[30] - out[29]));
    kd("mismatches", mism);
    if (first >= 0) {
        kd("first", first);
        kx("got", out[first]);
        kx("expect", seed + (u64)first);
    }
    ks("sp", out[0] == out[31] ? "ok" : "CHANGED");
    kx("ctl_after", ctl);
    kd("expect_us", 10000);
    end();
}

static void probe_psci(void)
{
    u64 r;

    r = hvc_call(PSCI_VERSION, 0);
    begin("psci");
    ks("conduit", "hvc");
    ks("fid", "PSCI_VERSION");
    kx("ret", r);
    end();

    r = hvc_call(PSCI_FEATURES, PSCI_SYSTEM_RESET2);
    begin("psci");
    ks("conduit", "hvc");
    ks("fid", "PSCI_FEATURES(SYSTEM_RESET2)");
    kx("ret", r);
    end();

    /* Q4: does SMC from EL1 trap to EL2 (HCR_EL2.TSC), UNDEF (no EL3), or fall through? */
    register u64 x0 __asm__("x0") = PSCI_VERSION;
    register u64 x1 __asm__("x1") = 0;
    register u64 x2 __asm__("x2") = 0;
    register u64 x3 __asm__("x3") = 0;
    GUARD_BEGIN();
    __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)::"memory");
    GUARD_END();
    begin("q4");
    if (g_faulted) {
        ks("smc", "undef");
        kx("esr", g_esr);
        kx("ec", g_esr >> 26);
    } else if (x0 == PSCI_VERSION) {
        ks("smc", "nop");
    } else {
        ks("smc", "trap");
        kx("ret", x0);
    }
    end();
}

static void probe_mem(void)
{
    /* the stage-2 RAM the launcher planned: boot_args range ends and the Mu handoff block (non-destructive) */
    u64 ba = entry_regs[0];
    u64 base = rd64(ba + 16), size = rd64(ba + 24);
    for (int i = 0; i < 3; i++) {
        /* no pointer tables in data: the image is position independent */
        u64 a = i == 0 ? base : (i == 1 ? MU_HANDOFF_BASE : base + size - 0x4000);
        const char *name = i == 0 ? "phys_base" : (i == 1 ? "handoff" : "top");
        if (a < base || a + 8 > base + size) {
            begin("mem");
            ks("at", name);
            kx("addr", a);
            ks("result", "outside-range");
            end();
            continue;
        }
        begin("mem");
        ks("at", name);
        kx("addr", a);
        ks("probe", "start");
        end();
        u64 old = rd64(a);
        wr64(a, 0x4d35574d454d0000UL | (u64)i);
        u64 back = rd64(a);
        wr64(a, old);
        begin("mem");
        ks("at", name);
        ks("result", back == (0x4d35574d454d0000UL | (u64)i) ? "ok" : "MISMATCH");
        end();
    }
}

static void uart_banner_samsung(u64 base, const char *s)
{
    while (*s) {
        for (int n = 0; n < 100000 && !(rd32(base + 0x10) & BIT(1)); n++) /* UTRSTAT.TXBE */
            ;
        wr32(base + 0x20, (u8)*s++); /* UTXH */
    }
}

static int uart_banner_pl011(u64 base, const char *s)
{
    int sent = 0;

    while (*s) {
        for (int n = 0; n < 100000 && (rd32(base + 0x18) & BIT(5)); n++) /* FR.TXFF */
            ;
        wr32(base + 0x000, (u8)*s++); /* DR */
        sent++;
    }
    return sent;
}

static void probe_uarts(void)
{
    u64 vuart = entry_regs[1], pl011 = entry_regs[2];

    if (vuart) {
        uart_banner_samsung(vuart, "M5W-SMOKE vuart hello (Samsung UART via hv_vuart)\r\n");
        begin("vuart");
        kx("base", vuart);
        ks("tx", "sent");
        end();
    }

    if (pl011) {
        u64 pid = 0, cid = 0;
        for (int i = 0; i < 4; i++) {
            pid |= (u64)(rd32(pl011 + 0xfe0 + 4 * i) & 0xff) << (8 * i);
            cid |= (u64)(rd32(pl011 + 0xff0 + 4 * i) & 0xff) << (8 * i);
        }
        u32 cr = rd32(pl011 + 0x30), fr = rd32(pl011 + 0x18);
        wr32(pl011 + 0x38, 0x50); /* IMSC = RX | RT */
        u32 imsc = rd32(pl011 + 0x38);
        wr32(pl011 + 0x38, 0);
        int sent = uart_banner_pl011(pl011, "M5W-SMOKE pl011 hello (emulated PL011)\r\n");
        u32 ris = rd32(pl011 + 0x3c);
        wr32(pl011 + 0x44, 0x7ff); /* ICR */
        begin("pl011");
        kx("base", pl011);
        kx("pid", pid);
        kx("cid", cid);
        ks("ids", (pid == 0x00141011 && cid == 0xb105f00d) ? "ok" : "WRONG");
        kx("cr", cr);
        kx("fr", fr);
        ks("imsc_rb", imsc == 0x50 ? "ok" : "WRONG");
        kx("ris_after_tx", ris);
        kd("tx", sent);
        end();
    }
}

static void probe_fb(void)
{
    u64 ba = entry_regs[0];
    u64 base = rd64(ba + 40), stride = rd64(ba + 56), width = rd64(ba + 64), height = rd64(ba + 72);
    u64 bpp = rd64(ba + 80) & 0xff;
    u32 green = bpp == 30 ? (0x3ffU << 10) : (0xffU << 8);

    if (!base || !stride || width < 64 || height < 64) {
        begin("fb");
        ks("drawn", "no");
        end();
        return;
    }
    for (u64 y = 0; y < 64; y++)
        for (u64 x = 0; x < 64; x++)
            wr32(base + y * stride + x * 4, green);
    begin("fb");
    ks("drawn", "64x64");
    ks("at", "0,0");
    kx("color", green);
    kd("bpp", bpp);
    end();
}

/* ---- PMUv3 (EL2-RUNTIME item 6) ---------------------------------------------------------------------- */
/*
 * Windows' bootmgfw, winload and ntoskrnl read PMCCNTR_EL0 (and set up PMCR_EL0) without checking
 * ID_AA64DFR0_EL1.PMUVer. EL2 sets MDCR_EL2.TPM|TPMCR (and the FGT PMU bits when FEAT_FGT exists) and
 * emulates an absent PMUv3 when the access traps; M5W_HVC_PMU_TRAPS (HVC #0x4d37) returns how many PMU
 * accesses EL2 emulated on this CPU and the ESR of the last one. Per access:
 *   trap   the instruction completed and EL2's count went up (EL2 saw ESR EC 0x18 and emulated it)
 *   undef  the instruction faulted at EL1 (taken before any EL2 trap: Windows needs a fallback)
 *   ok     the instruction completed without EL2 (a real PMUv3 answered)
 */
#define M5W_HVC_PMU_TRAPS 0x4d37

static u64 g_pmu_el2_esr;
static u64 g_pmu_n[3]; /* trap, undef, ok */

static u64 pmu_traps(void)
{
    register u64 x0 __asm__("x0") = 0;
    register u64 x1 __asm__("x1") = 0;
    __asm__ volatile("hvc #0x4d37" : "+r"(x0), "+r"(x1)::"memory");
    g_pmu_el2_esr = x1;
    return x0;
}

static void pmu_report(const char *name, const char *op, int done, u64 n0, u64 n1, u64 val, u64 ticks)
{
    int kind = !done ? 1 : (n1 != n0 ? 0 : 2);

    g_pmu_n[kind]++;
    begin("pmu");
    ks("reg", name);
    ks("op", op);
    ks("result", kind == 0 ? "trap" : (kind == 1 ? "undef" : "ok"));
    if (kind == 1) {
        kx("esr", g_esr);
        kx("ec", g_esr >> 26);
    } else {
        if (op[0] == 'r')
            kx("val", val);
        if (kind == 0) {
            kx("el2_esr", g_pmu_el2_esr);
            kx("el2_ec", (g_pmu_el2_esr >> 26) & 0x3f);
        }
    }
    kd("us", ticks_to_us(ticks));
    end();
}

#define PMU_READ(enc, name, out)                                                                   \
    ({                                                                                             \
        u64 _pv = 0, _pn0 = pmu_traps(), _pt0 = vct();                                             \
        int _pok = TRY_MRS(enc, _pv); /* TRY_MRS has its own __v */                                \
        u64 _pt1 = vct(), _pn1 = pmu_traps();                                                      \
        pmu_report(name, "read", _pok, _pn0, _pn1, _pv, _pt1 - _pt0);                              \
        (out) = _pv;                                                                               \
        _pok;                                                                                      \
    })

#define PMU_WRITE(enc, name, val)                                                                  \
    ({                                                                                             \
        u64 _pn0 = pmu_traps(), _pt0 = vct();                                                      \
        int _pok = TRY_MSR(enc, val);                                                              \
        u64 _pt1 = vct(), _pn1 = pmu_traps();                                                      \
        pmu_report(name, "write", _pok, _pn0, _pn1, 0, _pt1 - _pt0);                               \
        _pok;                                                                                      \
    })

static void probe_pmu(void)
{
    u64 dfr0 = sysreg_read("id_aa64dfr0_el1"), mmfr0 = sysreg_read("id_aa64mmfr0_el1");
    u64 c0 = 0, c1 = 0, pmcr = 0, v;
    int ok0, ok1, okp;

    begin("pmu");
    ks("probe", "begin");
    kx("dfr0_pmuver", (dfr0 >> 8) & 0xf); /* the guest's view (EL2 shows 0 under TID3) */
    kx("mmfr0_fgt", (mmfr0 >> 56) & 0xf);
    kd("el2_count", pmu_traps());
    end();

    ok0 = PMU_READ("s3_3_c9_c13_0", "PMCCNTR_EL0", c0);
    wait_ticks(g_freq / 1000); /* 1 ms */
    ok1 = PMU_READ("s3_3_c9_c13_0", "PMCCNTR_EL0", c1);
    okp = PMU_READ("s3_3_c9_c12_0", "PMCR_EL0", pmcr);
    PMU_READ("s3_3_c9_c12_1", "PMCNTENSET_EL0", v);
    PMU_READ("s3_3_c9_c12_5", "PMSELR_EL0", v);
    PMU_READ("s3_3_c9_c14_0", "PMUSERENR_EL0", v);
    /* what Windows does next: E = 1 (and the read value's other writable bits) */
    PMU_WRITE("s3_3_c9_c12_0", "PMCR_EL0", (okp ? pmcr & 0xf9 : 0) | 1);
    (void)v;

    begin("pmu");
    ks("probe", "summary");
    kd("trap", g_pmu_n[0]);
    kd("undef", g_pmu_n[1]);
    kd("ok", g_pmu_n[2]);
    ks("pmccntr_monotonic", ok0 && ok1 ? (c1 > c0 ? "yes" : "NO") : "na");
    if (okp)
        kd("pmcr_n", (pmcr >> 11) & 0x1f);
    ks("verdict", g_pmu_n[1] ? (g_pmu_n[0] || g_pmu_n[2] ? "MIXED" : "UNDEF")
                             : (g_pmu_n[2] ? (g_pmu_n[0] ? "MIXED" : "NATIVE") : "TRAP"));
    end();
}

void smoke_main(void);
void smoke_main(void)
{
    g_phase = 1;
    probe_entry();
    g_phase = 2;
    probe_id();
    g_phase = 3;
    probe_bootargs();
    g_phase = 4;
    probe_counters();
    g_phase = 5;
    probe_timer();
    g_phase = 6;
    probe_apple();
    g_phase = 7;
    probe_wfi();
    g_phase = 8;
    probe_psci();
    g_phase = 9;
    probe_uarts();
    g_phase = 10;
    probe_mem();
    g_phase = 11;
    probe_fb();
    g_phase = 12;
    probe_pmu();

    begin("end");
    kd("fiq_total", g_fiq_count);
    kd("irq_total", g_irq_count);
    ks("next", "SYSTEM_OFF");
    end();
    system_off();
}

/* the compiler may emit these for aggregate initialisation */
void *memset(void *s, int c, unsigned long n);
void *memset(void *s, int c, unsigned long n)
{
    volatile u8 *p = s;
    while (n--)
        *p++ = (u8)c;
    return s;
}

void *memcpy(void *d, const void *s, unsigned long n);
void *memcpy(void *d, const void *s, unsigned long n)
{
    volatile u8 *dp = d;
    const volatile u8 *sp = s;
    while (n--)
        *dp++ = *sp++;
    return d;
}
