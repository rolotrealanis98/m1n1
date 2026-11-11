/* SPDX-License-Identifier: MIT */
/*
 * cpuperf-A: the EL2-emulated CPPC page behind the firmware's per-CPU _CPC (Mu J716sAcpiGenLib,
 * /chosen/m5w,cppc). Pure logic, no hardware access: hv_win_cppc.c supplies the counter value and
 * the host test (tests/win-cppc) drives it directly.
 *
 * Page layout (16 KiB, in the synthetic window; CPU i's slot at +i * CPPC_SLOT):
 *   +0x00 DESIRED   32-bit RW  Desired Performance. Stored and counted; Stage A acts on nothing.
 *   +0x08 REFERENCE 64-bit RO  Reference Performance Counter = CNTPCT (1 GHz on T6050).
 *   +0x10 DELIVERED 64-bit RO  Delivered Performance Counter (n7cpu Stage C: scaled by the cluster's
 *                              current p-state, struct cppc_dl, when the launcher supplies reference perf;
 *                              otherwise as Stage A). Stage A: equal to REFERENCE, so
 *                              delivered/reference = 1 and Windows sees Reference Performance
 *                              (= nominal) delivered. No frequency is measured or changed.
 *   +0x18 LIMITED   32-bit RW  Performance Limited. Always 0 (writes accepted and dropped).
 * 32-bit reads of a counter's low (+0x08/+0x10) or high (+0x0c/+0x14) word are served. Every
 * other offset, width or alignment reads as zero and ignores writes; nothing faults the guest.
 *
 * Stage B (not in this build) hooks the DESIRED store; see perf/DESIGN.md.
 */
#ifndef HV_WIN_CPPC_CORE_H
#define HV_WIN_CPPC_CORE_H

#define CPPC_PAGE_SIZE 0x4000UL
#define CPPC_SLOT      0x40UL
#define CPPC_MAX_CPUS  (CPPC_PAGE_SIZE / CPPC_SLOT)

#define CPPC_DESIRED   0x00
#define CPPC_REFERENCE 0x08
#define CPPC_DELIVERED 0x10
#define CPPC_LIMITED   0x18

struct cppc_slot {
    u32 desired;
    u32 desired_min;
    u32 desired_max;
    u64 desired_writes;
    u64 counter_reads;
};

struct cppc_state {
    u64 base;
    u32 ncpus;
    bool configured;
    u64 odd_accesses; /* unknown offset, width or alignment */
    struct cppc_slot slot[CPPC_MAX_CPUS];
};

static inline int cppc_check_config(u64 base, u64 ncpus)
{
    if (!base || (base & (CPPC_PAGE_SIZE - 1)))
        return -1;
    if (!ncpus || ncpus > CPPC_MAX_CPUS)
        return -1;
    return 0;
}

static inline void cppc_reset(struct cppc_state *s, u64 base, u32 ncpus)
{
    u8 *p = (u8 *)s;
    for (unsigned long i = 0; i < sizeof(*s); i++)
        p[i] = 0;
    s->base = base;
    s->ncpus = ncpus;
    for (u32 i = 0; i < CPPC_MAX_CPUS; i++)
        s->slot[i].desired_min = 0xffffffffU;
    s->configured = true;
}

/*
 * n7cpu (Stage C): a scaled Delivered Performance Counter per cluster. Reference = CNTPCT, which
 * stands for the CPU's _CPC Reference Performance (= its Nominal). Delivered advances at
 * CNTPCT * num / den, num = the perf of the cluster's current p-state, den = that Reference
 * Performance, so delivered/reference over any interval is the clock actually run relative to nominal.
 * A piecewise-linear function of CNTPCT, rebased (with the remainder carried) whenever the rate changes
 * or at every EL2 tick: exact, continuous and monotonic. den == 0 = unscaled (delivered = CNTPCT).
 * Every caller holds the big HV lock (hooks run under it, and so does the boot CPU's tick).
 */
struct cppc_dl {
    u64 t0, d0, rem;
    u32 num, den;
};

static inline void cppc_dl_init(struct cppc_dl *c, u64 t)
{
    c->t0 = t;
    c->d0 = t; /* starts equal to REFERENCE, continuous with the unscaled counter */
    c->rem = 0;
    c->num = 0;
    c->den = 0;
}

static inline u64 cppc_dl_value(const struct cppc_dl *c, u64 t)
{
    if (t < c->t0)
        t = c->t0;
    if (!c->den)
        return c->d0 + (t - c->t0); /* unscaled: 1:1 from the base (== t until a rate was ever set) */
    return c->d0 + ((t - c->t0) * c->num + c->rem) / c->den;
}

/* Fold the elapsed time in at the old rate, then switch to num/den (den 0 = unscaled from here on). */
static inline void cppc_dl_rebase(struct cppc_dl *c, u64 t, u32 num, u32 den)
{
    if (t < c->t0)
        t = c->t0;
    if (c->den) {
        u64 x = (t - c->t0) * c->num + c->rem;
        c->d0 += x / c->den;
        c->rem = x % c->den;
    } else {
        c->d0 += t - c->t0; /* the unscaled counter advanced 1:1 */
        c->rem = 0;
    }
    c->t0 = t;
    if (den != c->den)
        c->rem = 0; /* a remainder is in units of the old den */
    c->num = num;
    c->den = den;
}

/*
 * One guest access. off = address - page base (< CPPC_PAGE_SIZE), width = log2(bytes) as in
 * hv_hook_t, now = the REFERENCE counter value, delivered = the DELIVERED counter value of the
 * accessed slot's CPU (equal to now when unscaled). Returns true (always handled).
 */
static inline bool cppc_access2(struct cppc_state *s, u64 off, u64 *val, bool write, int width,
                                u64 now, u64 delivered);

static inline bool cppc_access(struct cppc_state *s, u64 off, u64 *val, bool write, int width,
                               u64 now)
{
    return cppc_access2(s, off, val, write, width, now, now);
}

static inline bool cppc_access2(struct cppc_state *s, u64 off, u64 *val, bool write, int width,
                                u64 now, u64 delivered)
{
    u64 cpu = off / CPPC_SLOT;
    u64 reg = off % CPPC_SLOT;
    u64 out = 0;
    bool known = false;

    if (cpu < s->ncpus && width >= 2 && width <= 3 && !(reg & ((1UL << width) - 1))) {
        struct cppc_slot *sl = &s->slot[cpu];

        if (width == 2 && reg == CPPC_DESIRED) {
            known = true;
            if (write) {
                u32 v = (u32)*val;
                sl->desired = v;
                sl->desired_writes++;
                if (v < sl->desired_min)
                    sl->desired_min = v;
                if (v > sl->desired_max)
                    sl->desired_max = v;
            } else {
                out = sl->desired;
            }
        } else if (width == 2 && reg == CPPC_LIMITED) {
            known = true; /* reads 0; writes (clear-on-write by OSPM) are dropped */
        } else if (reg >= CPPC_REFERENCE && reg < CPPC_LIMITED) {
            known = true;
            if (!write) {
                /* Stage A: equal; n7cpu Stage C: DELIVERED scaled by the cluster's clock */
                u64 v = reg < CPPC_DELIVERED ? now : delivered;
                if (width == 3)
                    out = v;
                else
                    out = (reg & 4) ? (v >> 32) : (v & 0xffffffffUL);
                sl->counter_reads++;
            }
        }
    }

    if (!known)
        s->odd_accesses++;
    if (!write)
        *val = out;
    return true;
}

#endif
