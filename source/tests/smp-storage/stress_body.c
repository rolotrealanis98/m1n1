/* smp-storage stress body (appended after stress_prelude.h + the real hv_win_storage.c + hv_win_storage_mem.c).
 *
 * N "vCPUs" drive the real emulated controller (MMIO hooks -> storage lock -> lease -> S1 model -> PRP planner ->
 * backend) concurrently against a flat-array fake disk:
 *   cpu 0       : the boot CPU: hypervisor tick (hv_win_storage_tick), CSTS reads, INTMS/INTMC toggles
 *   cpu 1..N-1  : guest workers sharing the one I/O SQ/CQ (guest-side locks only around SQ slot allocation and
 *                 CQ consumption, as stornvme does); each submits READ/WRITE/FLUSH, valid and invalid
 *   scribblers  : rewrite the shared WRITE source buffers continuously (generation-numbered words), and flip the
 *                 LBA of in-flight hostile SQEs / the first entry of in-flight hostile PRP lists
 * Properties (FAIL = exit 1):
 *   P1 every byte a completed WRITE put on the disk equals the value the corresponding source byte held at some
 *      instant between the doorbell write (generation sampled before it) and the observation of the completion
 *      (generation sampled after it); tracked per byte through the command's exact PRP layout;
 *   P2 no block outside the window changes (64 guard blocks either side, compared every 32 commands and at the
 *      end) and the backend is never asked for an LBA outside the window; the protected trap page never changes;
 *   P3 every completion is delivered exactly once, for an outstanding command of the queue (no torn CQE: the guest
 *      poisons consumed entries, a new phase with a poisoned body fails);
 *   P4 READ data equals the disk (static region: pattern; own region: the shadow of the last verified writes);
 *   P5 latch: after an injected backend write failure the backend refuses everything and the disk never changes
 *      again; retire: after hv_win_storage_retire() from another CPU nothing reaches the disk or the CQ.
 * Usage: test <nvcpus 2..18> <commands per worker> <seed> <main|latch|retire|retire_nested>
 */
#include <time.h>
#include <stdatomic.h>
#include <sys/mman.h>

__thread int t_cpu;
static __thread spinlock_t t_bhl;
spinlock_t *stress_bhl(void) { t_bhl.lock = t_cpu; t_bhl.count = 1; return &t_bhl; }

#define FAIL(...) do { fprintf(stderr, "FAIL cpu%d line %d: ", t_cpu, __LINE__); fprintf(stderr, __VA_ARGS__); \
                       fprintf(stderr, "\n"); exit(1); } while (0)
#define REQUIRE(x) do { if (!(x)) FAIL("%s", #x); } while (0)
#define GUESTFN __attribute__((no_sanitize("thread"), noinline))

/* ---------------------------------------------------------------- fake disk (backend) */
#define W_BLOCKS 4096u
#define GUARD 64u
#define BS 4096u
#define STATIC_BLOCKS 1024u
static u8 *disk;               /* (GUARD + W_BLOCKS + GUARD) blocks */
static u8 *disk_initial_guard; /* initial bytes of both guards */
static u8 xfer[256 * BS] __attribute__((aligned(16384)));
static struct {
    u64 writes, reads, flushes, polls, violations, poll_after_latch, mailbox;
    bool latched;
    u64 fail_at_write; /* 0 = never */
} fake;
static atomic_int latched_seen;
static u8 *latch_snapshot;
static u8 pattern_byte(u64 lba, u32 i) { return (u8)(lba * 131u + i * 7u + (i >> 8)); }
static u64 fk_blocks(void) { return W_BLOCKS; }
static bool fk_read(u64 lba, u32 n, void *buf)
{
    if (!n || lba >= W_BLOCKS || n > W_BLOCKS - lba) { fake.violations++; return false; }
    fake.mailbox++; /* every ANS command drains the mailbox the tick also drains (ans_ro exec / ans_ro_poll) */
    if (fake.latched) return false;
    fake.reads++;
    memcpy(buf, disk + (GUARD + lba) * BS, (size_t)n * BS);
    return true;
}
static bool fk_write(u64 lba, u32 n, const void *buf)
{
    if (!n || n > 32 || lba >= W_BLOCKS || n > W_BLOCKS - lba) {
        fake.violations++;
        fprintf(stderr, "FAIL backend write outside the window or > 32 blocks: lba %llu n %u\n",
                (unsigned long long)lba, n);
        exit(1);
    }
    if ((const u8 *)buf < xfer || (const u8 *)buf + (size_t)n * BS > xfer + sizeof(xfer))
        FAIL("backend write source is not the private transfer buffer");
    fake.mailbox++;
    if (fake.latched) return false;
    fake.writes++;
    if (fake.fail_at_write && fake.writes == fake.fail_at_write) {
        fake.latched = true; /* permanent stop, as ans_ro's freeze */
        memcpy(latch_snapshot, disk, (size_t)(GUARD + W_BLOCKS + GUARD) * BS);
        atomic_store(&latched_seen, 1);
        return false;
    }
    memcpy(disk + (GUARD + lba) * BS, buf, (size_t)n * BS);
    return true;
}
static bool fk_flush(void)
{
    fake.mailbox++;
    if (fake.latched) return false;
    fake.flushes++;
    return true;
}
static void fk_poll(void)
{
    fake.polls++;
    fake.mailbox++;
    if (fake.latched) fake.poll_after_latch++;
}
static void *fk_xfer(size_t *cap) { *cap = sizeof(xfer); return xfer; }
static void fk_caps(struct vnvme_caps *c) { c->max_blocks = 256; c->writable = true; c->vwc = true; c->native = true; }
static const struct s1_backend fake_backend = {
    .name = "stress flat disk", .blocks = fk_blocks, .read = fk_read, .poll = fk_poll,
    .write = fk_write, .flush = fk_flush, .xfer_buffer = fk_xfer, .caps = fk_caps,
};

/* ---------------------------------------------------------------- guest memory (identity IPA == host VA) */
#define GSIZE (32UL << 20)
#define ASQ (G + 0x0000)
#define ACQ (G + 0x1000)
#define IOSQ (G + 0x4000)
#define IOCQ (G + 0x8000)
#define TRAP (G + 0xc000) /* 16 KiB, declared protected: never admissible */
#define IDBUF (G + 0x10000)
#define WAREA(w) (G + 0x100000 + (u64)(w) * 0x80000) /* PRP list pages at +0, +0x1000; dest pages at +0x10000 */
#define NBUF 4
#define BUF_PAGES 64
#define SBUF(b) (G + 0x1000000 + (u64)(b) * BUF_PAGES * BS)
#define QD 64
static u64 G;
static u8 *trap_initial;

GUESTFN static void g_st64(u64 a, u64 v) { *(volatile u64 *)a = v; }
GUESTFN static u64 g_ld64(u64 a) { return *(volatile u64 *)a; }
GUESTFN static void g_st32(u64 a, u32 v) { *(volatile u32 *)a = v; }
GUESTFN static u32 g_ld32(u64 a) { return *(volatile u32 *)a; }
GUESTFN static void g_put(u64 a, const void *src, size_t n) /* n % 8 == 0 */
{
    const u64 *s = src;
    for (size_t i = 0; i < n / 8; i++) ((volatile u64 *)a)[i] = s[i];
}
GUESTFN static void g_get(void *dst, u64 a, size_t n)
{
    u64 *d = dst;
    for (size_t i = 0; i < n / 8; i++) d[i] = ((volatile u64 *)a)[i];
}
GUESTFN static void g_fill(u64 a, size_t n, u64 v)
{
    for (size_t i = 0; i < n / 8; i++) ((volatile u64 *)a)[i] = v;
}

/* shared WRITE source buffers: word w of buffer b holds value(b, w, g) with g its scribbler generation */
static atomic_ullong gen[NBUF];
static u64 splitmix(u64 x)
{
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}
static u64 salt(unsigned b, u64 w) { return splitmix(((u64)b << 32) | w); }
static u64 value(unsigned b, u64 w, u64 g) { return (g * 0x0101010101010101ULL) ^ salt(b, w); }
static atomic_int stop_scribblers;
GUESTFN static void scribble_pass(unsigned b, u64 g)
{
    u64 base = SBUF(b);
    for (u64 w = 0; w < BUF_PAGES * BS / 8; w++) ((volatile u64 *)base)[w] = value(b, w, g);
}
static void *scribbler(void *arg)
{
    t_cpu = 40 + (int)(long)arg;
    unsigned first = (unsigned)(long)arg * 2;
    while (!atomic_load(&stop_scribblers)) {
        for (unsigned b = first; b < first + 2; b++) {
            u64 g = atomic_load(&gen[b]) + 1;
            atomic_store(&gen[b], g); /* before the pass: every word holds g-1 or g from here on */
            scribble_pass(b, g);
        }
        sched_yield();
    }
    return NULL;
}

/* hostile in-flight mutation: flip a 32-bit guest word between two values until told to stop. flip.seq is odd
 * while a flip runs; flip_stop() makes it even and waits until the flipper acknowledges that value, which it does
 * only after re-checking seq following its last store, so no store of a finished flip happens after flip_stop. */
static struct { atomic_ullong addr; atomic_uint good, bad; atomic_ullong seq, acked; } flip;
GUESTFN static void flip_store(u64 a, u32 v) { *(volatile u32 *)a = v; }
static void *flipper(void *arg)
{
    (void)arg;
    t_cpu = 45;
    while (!atomic_load(&stop_scribblers)) {
        u64 sq = atomic_load(&flip.seq);
        if (!(sq & 1)) { atomic_store(&flip.acked, sq); sched_yield(); continue; }
        u64 a = atomic_load(&flip.addr);
        u32 g = atomic_load(&flip.good), b = atomic_load(&flip.bad);
        for (int i = 0; i < 64 && atomic_load(&flip.seq) == sq; i++) {
            flip_store(a, b);
            for (volatile int d = 0; d < (i & 7) * 40; d++) {}
            if (atomic_load(&flip.seq) != sq) break;
            flip_store(a, g);
            for (volatile int d = 0; d < (i & 3) * 40; d++) {}
        }
    }
    return NULL;
}
static void flip_start(u64 a, u32 good, u32 bad)
{
    atomic_store(&flip.addr, a); atomic_store(&flip.good, good); atomic_store(&flip.bad, bad);
    atomic_fetch_add(&flip.seq, 1); /* odd: running */
}
static void flip_stop(void)
{
    u64 even = atomic_fetch_add(&flip.seq, 1) + 1;
    while (atomic_load(&flip.acked) < even && !atomic_load(&stop_scribblers)) sched_yield();
}

/* ---------------------------------------------------------------- guest driver: MMIO, SQ, CQ */
static bool mmio(u64 off, u64 v, bool write, int width, u64 *out)
{
    bool ok = hooks[1](NULL, HV_S1_BAR + off, &v, write, width);
    if (out) *out = v;
    return ok;
}
static pthread_mutex_t sqm = PTHREAD_MUTEX_INITIALIZER, cqm = PTHREAD_MUTEX_INITIALIZER;
static int sq_tail;            /* under sqm */
static atomic_int sq_head_seen; /* from CQEs (under cqm), read under sqm */
static int cq_head, cq_phase = 1; /* under cqm */
static pthread_mutex_t dbm = PTHREAD_MUTEX_INITIALIZER;
static atomic_int cq_head_pub;
static int cq_head_rung; /* under dbm */
#define MAXW 17
struct outstanding { atomic_int cid; atomic_int done; atomic_int status; atomic_int count; };
static struct outstanding outst[MAXW + 1];
static atomic_ullong completions_total, torn, unknown_cid;
static atomic_int retired_flag;
static atomic_ullong completions_after_retire;

static void deliver(u16 cid, u16 status)
{
    unsigned w = cid >> 10;
    if (w < 1 || w > MAXW) { atomic_fetch_add(&unknown_cid, 1); FAIL("completion for unknown cid %#x", cid); }
    struct outstanding *o = &outst[w];
    if (atomic_load(&o->cid) != cid || atomic_load(&o->done))
        FAIL("completion for cid %#x not outstanding (outstanding %#x, done %d): duplicate or stray", cid,
             atomic_load(&o->cid), atomic_load(&o->done));
    atomic_fetch_add(&o->count, 1);
    atomic_store(&o->status, status);
    atomic_store(&o->done, 1);
    atomic_fetch_add(&completions_total, 1);
    if (atomic_load(&retired_flag)) atomic_fetch_add(&completions_after_retire, 1);
}
GUESTFN static bool cq_take(u64 slot, u32 *dw)
{
    u32 d3 = *(volatile u32 *)(slot + 12);
    if (((d3 >> 16) & 1) != (u32)cq_phase) return false;
    __atomic_thread_fence(__ATOMIC_ACQUIRE); /* the guest's own ordering (stornvme reads the CQE after the phase) */
    dw[0] = *(volatile u32 *)slot; dw[1] = *(volatile u32 *)(slot + 4); dw[2] = *(volatile u32 *)(slot + 8);
    dw[3] = d3;
    /* poison the body: a later new phase with this body would be a torn entry */
    *(volatile u32 *)slot = 0xdeadbeef; *(volatile u32 *)(slot + 4) = 0xdeadbeef; *(volatile u32 *)(slot + 8) = 0xffffffff;
    return true;
}
static void process_cq(void)
{
    if (pthread_mutex_trylock(&cqm)) return;
    int consumed = 0, idle = 0;
    for (;;) {
        u32 dw[4];
        if (!cq_take(IOCQ + (u64)cq_head * 16, dw)) {
            if (consumed || ++idle > 24) break; /* linger a little: CQEs posted by another CPU's SQ doorbell land now */
            continue;
        }
        u16 sqid = (u16)(dw[2] >> 16), sqh = (u16)dw[2];
        if (sqid != 1 || sqh >= QD) { atomic_fetch_add(&torn, 1); FAIL("torn CQE: sq_id %#x sq_head %#x", sqid, sqh); }
        atomic_store(&sq_head_seen, sqh);
        deliver((u16)dw[3], (u16)(dw[3] >> 17));
        if (++cq_head == QD) { cq_head = 0; cq_phase ^= 1; }
        consumed++;
    }
    if (consumed) atomic_store(&cq_head_pub, cq_head);
    pthread_mutex_unlock(&cqm);
    /* the head doorbell goes out after the CQ lock is dropped (always the latest head, never backwards), so other
     * CPUs consume while the controller posts the next CQE from this doorbell */
    if (consumed) {
        pthread_mutex_lock(&dbm);
        int h = atomic_load(&cq_head_pub);
        if (h != cq_head_rung) { cq_head_rung = h; mmio(0x100c, (u64)h, true, 2, NULL); }
        pthread_mutex_unlock(&dbm);
    }
}
static int submit(struct vnvme_command *c, u64 *slot_addr)
{
    pthread_mutex_lock(&sqm);
    while ((sq_tail + 1) % QD == atomic_load(&sq_head_seen)) {
        pthread_mutex_unlock(&sqm);
        process_cq();
        if (atomic_load(&retired_flag)) return -1;
        sched_yield();
        pthread_mutex_lock(&sqm);
    }
    u64 slot = IOSQ + (u64)sq_tail * 64;
    g_put(slot, c, 64);
    if (slot_addr) *slot_addr = slot;
    sq_tail = (sq_tail + 1) % QD;
    mmio(0x1008, (u64)sq_tail, true, 2, NULL);
    pthread_mutex_unlock(&sqm);
    return 0;
}
static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
/* returns the status, or -1 if retired and nothing came back */
static int wait_done(unsigned w)
{
    double t0 = now_s();
    while (!atomic_load(&outst[w].done)) {
        process_cq();
        if (atomic_load(&retired_flag) && now_s() - t0 > 0.2) return -1;
        if (now_s() - t0 > 60) FAIL("no completion for cid %#x after 60 s (lost completion)", atomic_load(&outst[w].cid));
        sched_yield();
    }
    return atomic_load(&outst[w].status);
}

/* ---------------------------------------------------------------- oracle (reads the fake disk under the storage lock) */
static void disk_get(u64 lba, u32 n, u8 *out)
{
    hv_storage_lock_take();
    memcpy(out, disk + (GUARD + lba) * BS, (size_t)n * BS);
    hv_storage_lock_release();
}
static void check_guards(void)
{
    hv_storage_lock_take();
    bool ok = !memcmp(disk, disk_initial_guard, GUARD * BS) &&
              !memcmp(disk + (GUARD + W_BLOCKS) * BS, disk_initial_guard + GUARD * BS, GUARD * BS);
    u64 viol = fake.violations;
    hv_storage_lock_release();
    if (!ok) FAIL("a block outside the window changed");
    if (viol) FAIL("backend asked for an LBA outside the window");
    u8 t[0x4000];
    g_get(t, TRAP, sizeof(t));
    if (memcmp(t, trap_initial, sizeof(t))) FAIL("the protected trap page changed");
}

/* ---------------------------------------------------------------- workers */
struct seg { u64 addr; u32 len; };
struct wstate {
    unsigned w;
    u64 rng;
    u64 region, region_blocks;
    u8 *shadow;          /* own region as last verified */
    u8 *scratch;
    u16 seq;
    u64 n_write, n_read, n_flush, n_bad, n_hostile_w, n_hostile_r, n_refused, n_after_latch, n_loose_words, n_tight_words, n_torn_words, n_dup, n_mutated;
};
static u64 rnd(struct wstate *s) { s->rng = splitmix(s->rng); return s->rng; }
static int nworkers;
static const char *mode;

static u16 next_cid(struct wstate *s)
{
    s->seq = (u16)((s->seq + 1) & 0x3ff);
    return (u16)((s->w << 10) | s->seq);
}
/* build PRP1/PRP2 (+ list in the worker's list page) for `segs` (first may be partial, the rest whole pages) */
static void build_prps(struct wstate *s, struct vnvme_command *c, const struct seg *segs, unsigned nseg)
{
    c->prp1 = segs[0].addr;
    if (nseg == 1) return;
    if (nseg == 2) { c->prp2 = segs[1].addr; return; }
    u64 list = WAREA(s->w);
    for (unsigned i = 1; i < nseg; i++) g_st64(list + 8 * (i - 1), segs[i].addr);
    c->prp2 = list;
}
static int run_cmd(struct wstate *s, struct vnvme_command *c, u64 *slot)
{
    struct outstanding *o = &outst[s->w];
    c->cid = next_cid(s);
    atomic_store(&o->done, 0);
    atomic_store(&o->count, 0);
    atomic_store(&o->cid, c->cid);
    if (submit(c, slot) < 0) return -1;
    int st = wait_done(s->w);
    if (st >= 0 && atomic_load(&o->count) != 1) FAIL("completion count %d", atomic_load(&o->count));
    return st;
}
/* P1 for one 4 KiB block at command offset cmd_off: every byte equals the source byte's value at some generation
 * in [gen at doorbell - 1, gen at completion] (each word is written whole by its scribbler; a copy may tear it
 * between generations, so each byte is checked on its own as well). */
static bool block_ok(struct wstate *s, const u8 *blk, size_t cmd_off, const struct seg *segs, unsigned nseg,
                     const unsigned *segbuf, const u64 *gs, const u64 *ge, bool count)
{
    size_t pos = 0;
    unsigned i = 0;
    while (i < nseg && pos + segs[i].len <= cmd_off) pos += segs[i++].len;
    for (u32 x = 0; x < BS; x += 8) {
        size_t o = cmd_off + x;
        while (i < nseg && pos + segs[i].len <= o) pos += segs[i++].len;
        if (i >= nseg) return false;
        unsigned b = segbuf[i];
        u64 a = segs[i].addr + (o - pos), word;
        memcpy(&word, blk + x, 8);
        u64 widx = (a - SBUF(b)) / 8, lo = gs[b] ? gs[b] - 1 : 0, hi = ge[b], span = hi - lo, sl = salt(b, widx);
        if (span >= 255) { if (count) s->n_loose_words++; continue; }
        u64 g = lo + ((((word ^ sl) & 0xff) - (lo & 0xff)) & 0xff);
        if (g <= hi && word == value(b, widx, g)) { if (count) s->n_tight_words++; continue; }
        for (unsigned k = 0; k < 8; k++)
            if (((((word ^ sl) >> (8 * k)) & 0xff) - (lo & 0xff) & 0xff) > span) return false;
        if (count) s->n_torn_words++;
    }
    return true;
}
static void verify_write(struct wstate *s, u64 lba, u32 n, const struct seg *segs, unsigned nseg,
                         const unsigned *segbuf, const u64 *gs, const u64 *ge, bool partial_ok)
{
    u8 *d = s->scratch;
    disk_get(lba, n, d);
    for (u32 i = 0; i < n; i++) {
        const u8 *blk = d + (size_t)i * BS;
        if (block_ok(s, blk, (size_t)i * BS, segs, nseg, segbuf, gs, ge, !partial_ok)) continue;
        if (partial_ok && !memcmp(blk, s->shadow + (lba - s->region + i) * BS, BS)) continue; /* not written */
        {   /* diagnosis: was it simply not written (old shadow), and what does the first word decode to */
            u64 w0; memcpy(&w0, blk, 8);
            fprintf(stderr, "diag: equals old shadow %d; nseg %u seg0 %#llx+%u buf %u gs %llu ge %llu; word0 %#llx",
                    !memcmp(blk, s->shadow + (lba - s->region + i) * BS, BS), nseg, (unsigned long long)segs[0].addr,
                    segs[0].len, segbuf[0], (unsigned long long)gs[segbuf[0]], (unsigned long long)ge[segbuf[0]],
                    (unsigned long long)w0);
            for (unsigned bb = 0; bb < NBUF; bb++)
                for (u64 ww = 0; ww < BUF_PAGES * BS / 8; ww++) {
                    u64 g0 = (w0 ^ salt(bb, ww)) & 0xff;
                    if (value(bb, ww, g0) == w0 || value(bb, ww, g0 + 256) == w0)
                        fprintf(stderr, " | matches buf %u word %llu gen%%256 %llu", bb, (unsigned long long)ww, (unsigned long long)g0);
                }
            fprintf(stderr, "\n");
        }
        FAIL("WRITE block %u of lba %llu (n %u) holds bytes its source never held between doorbell and completion",
             i, (unsigned long long)lba, n);
    }
    memcpy(s->shadow + (lba - s->region) * BS, d, (size_t)n * BS);
}
static void op_write(struct wstate *s, bool hostile)
{
    u32 n = 1 + (u32)(rnd(s) % 8 ? rnd(s) % 8 : rnd(s) % 64);
    if (n > s->region_blocks) n = (u32)s->region_blocks;
    u64 lba = s->region + rnd(s) % (s->region_blocks - n + 1);
    unsigned b = (unsigned)(rnd(s) % NBUF);
    u32 off = (rnd(s) % 4 == 0) ? (u32)(8 * (rnd(s) % 511 + 1)) : 0;
    if (off && n >= BUF_PAGES) n = BUF_PAGES - 1; /* the source must stay inside one buffer */
    u32 pages = n + (off ? 1 : 0);
    u32 p0 = (u32)(rnd(s) % (BUF_PAGES - pages + 1));
    struct seg segs[300];
    unsigned segbuf[300], nseg = 0;
    size_t total = (size_t)n * BS, left = total;
    bool dup = !off && n >= 2 && rnd(s) % 6 == 0; /* nightly-4b: an exact whole-page source repeat */
    for (u32 i = 0; left; i++) {
        u64 a = SBUF(b) + (u64)(p0 + i) * BS + (i ? 0 : off);
        if (dup && i == 1) a = SBUF(b) + (u64)p0 * BS;
        u32 len = (u32)((i ? BS : BS - off) < left ? (i ? BS : BS - off) : left);
        segs[nseg] = (struct seg){a, len}; segbuf[nseg++] = b; left -= len;
    }
    struct vnvme_command c = {.opcode = VNVME_IO_WRITE, .nsid = 1, .cdw10 = (u32)lba, .cdw11 = (u32)(lba >> 32),
                              .cdw12 = (n - 1) | (rnd(s) % 8 == 0 ? (1u << 30) : 0)};
    build_prps(s, &c, segs, nseg);
    u64 gs[NBUF], ge[NBUF], slot = 0;
    for (unsigned i = 0; i < NBUF; i++) gs[i] = atomic_load(&gen[i]);
    int st;
    if (hostile) {
        /* the in-flight SQE's LBA flips between this value and one past the namespace: the controller copies the
         * SQE once, so it either writes here or refuses; it never writes outside */
        struct outstanding *o = &outst[s->w];
        c.cid = next_cid(s);
        atomic_store(&o->done, 0); atomic_store(&o->count, 0); atomic_store(&o->cid, c.cid);
        pthread_mutex_lock(&sqm);
        while ((sq_tail + 1) % QD == atomic_load(&sq_head_seen)) {
            pthread_mutex_unlock(&sqm); process_cq();
            if (atomic_load(&retired_flag)) return;
            sched_yield(); pthread_mutex_lock(&sqm);
        }
        slot = IOSQ + (u64)sq_tail * 64;
        g_put(slot, &c, 64);
        /* the two values differ only in bit 31, so even a torn copy is either this LBA or far past the end */
        flip_start(slot + offsetof(struct vnvme_command, cdw10), (u32)lba, (u32)lba | 0x80000000u);
        sq_tail = (sq_tail + 1) % QD;
        mmio(0x1008, (u64)sq_tail, true, 2, NULL);
        /* sqm stays held until the flip is stopped: no other CPU can reuse this slot while it may still be written */
        st = wait_done(s->w);
        flip_stop();
        pthread_mutex_unlock(&sqm);
        s->n_hostile_w++;
    } else {
        st = run_cmd(s, &c, &slot);
    }
    if (st < 0) return;
    for (unsigned i = 0; i < NBUF; i++) ge[i] = atomic_load(&gen[i]);
    if (st == 0) {
        verify_write(s, lba, n, segs, nseg, segbuf, gs, ge, false);
        if (ge[b] > gs[b]) s->n_mutated++; /* its source was being rewritten while the command ran */
        s->n_write++;
        if (dup) s->n_dup++;
    } else {
        bool latched = atomic_load(&latched_seen);
        if (!(hostile && st == VNVME_SC_INVALID_FIELD) && !latched) {
            hv_storage_lock_take();
            const struct vnvme_prp_refusal *r = &controller.queue.prp_log[(controller.queue.prp_refusal_total - 1) % VNVME_PRP_LOG];
            fprintf(stderr, "write lba %llu n %u off %u p0 %u dup %d nseg %u prp1 %#llx prp2 %#llx | last refusal rule %u idx %u addr %#llx len %llu other %#llx cid %#x (this %#x) sm_why %llu\n",
                    (unsigned long long)lba, n, off, p0, dup, nseg, (unsigned long long)c.prp1, (unsigned long long)c.prp2,
                    r->rule, r->index, (unsigned long long)r->addr, (unsigned long long)r->len, (unsigned long long)r->other, r->cid, c.cid,
                    (unsigned long long)hv_sm_last_refusal.why);
            hv_storage_lock_release();
            FAIL("valid WRITE failed: status %#x", st);
        }
        if (latched) {
            s->n_after_latch++;
            verify_write(s, lba, n, segs, nseg, segbuf, gs, ge, true); /* a split write may have landed partly */
        } else {
            u8 *d = s->scratch;
            disk_get(lba, n, d);
            if (memcmp(d, s->shadow + (lba - s->region) * BS, (size_t)n * BS)) FAIL("refused WRITE changed the disk");
            s->n_refused++;
        }
    }
}
GUESTFN static void g_get_dest(u8 *out, u64 a, size_t n) { for (size_t i = 0; i < n; i++) out[i] = ((volatile u8 *)a)[i]; }
static void op_read(struct wstate *s, bool hostile)
{
    u32 n = 1 + (u32)(rnd(s) % 4 ? rnd(s) % 8 : rnd(s) % 63);
    bool own = rnd(s) % 2;
    if (own && n > s->region_blocks) n = (u32)s->region_blocks;
    u64 lba = own ? s->region + rnd(s) % (s->region_blocks - n + 1) : rnd(s) % (STATIC_BLOCKS - n + 1);
    u32 off = (rnd(s) % 4 == 0) ? (u32)(8 * (rnd(s) % 511 + 1)) : 0;
    u64 dest = WAREA(s->w) + 0x10000;
    g_fill(dest, (size_t)(n + 1) * BS, 0x5a5a5a5a5a5a5a5aULL);
    struct seg segs[300];
    unsigned nseg = 0;
    size_t left = (size_t)n * BS;
    for (u32 i = 0; left; i++) {
        u32 len = (u32)((i ? BS : BS - off) < left ? (i ? BS : BS - off) : left);
        segs[nseg++] = (struct seg){dest + (u64)i * BS + (i ? 0 : off), len};
        left -= len;
    }
    if (hostile && nseg < 3) return; /* needs a list */
    struct vnvme_command c = {.opcode = VNVME_IO_READ, .nsid = 1, .cdw10 = (u32)lba, .cdw11 = (u32)(lba >> 32), .cdw12 = n - 1};
    build_prps(s, &c, segs, nseg);
    int st;
    if (hostile) {
        /* the list's first entry flips between its page and the protected trap page while in flight: the list is
         * copied once, so the READ either lands in the right pages or is refused; the trap never changes */
        u64 list = WAREA(s->w);
        if ((segs[1].addr >> 32) != (TRAP >> 32)) return; /* the flip changes the low half only */
        flip_start(list, (u32)segs[1].addr, (u32)TRAP);
        st = run_cmd(s, &c, NULL);
        flip_stop();
        g_st64(list, segs[1].addr);
        s->n_hostile_r++;
    } else {
        st = run_cmd(s, &c, NULL);
    }
    if (st < 0) return;
    if (st != 0) {
        if (atomic_load(&latched_seen)) { s->n_after_latch++; return; }
        if (hostile && st == VNVME_SC_DATA_TRANSFER_ERROR) { s->n_refused++; return; }
        FAIL("valid READ failed: status %#x", st);
    }
    u8 *got = s->scratch;
    g_get_dest(got, dest, (size_t)(n + 1) * BS);
    for (u32 i = 0; i < n; i++)
        for (u32 j = 0; j < BS; j++) {
            u8 want = own ? s->shadow[(lba - s->region + i) * BS + j] : pattern_byte(lba + i, j);
            if (got[off + (size_t)i * BS + j] != want)
                FAIL("READ data wrong: lba %llu block %u byte %u", (unsigned long long)lba, i, j);
        }
    for (u32 j = 0; j < off; j++) if (got[j] != 0x5a) FAIL("READ wrote before its first segment");
    for (size_t j = off + (size_t)n * BS; j < (size_t)(n + 1) * BS; j++) if (got[j] != 0x5a) FAIL("READ wrote past its end");
    s->n_read++;
}
static void op_flush(struct wstate *s)
{
    struct vnvme_command c = {.opcode = VNVME_IO_FLUSH, .nsid = 1};
    int st = run_cmd(s, &c, NULL);
    if (st < 0) return;
    if (st && !atomic_load(&latched_seen)) FAIL("FLUSH failed: %#x", st);
    if (st) s->n_after_latch++; else s->n_flush++;
}
static void op_bad(struct wstate *s)
{
    /* invalid commands: out-of-namespace WRITE/READ, WRITE source in the trap page, READ into the live CQ page */
    struct vnvme_command c = {.nsid = 1};
    unsigned k = (unsigned)(rnd(s) % 4);
    u64 lba = W_BLOCKS - 1 + rnd(s) % 3;
    if (k == 0) { c.opcode = VNVME_IO_WRITE; c.cdw10 = (u32)lba; c.cdw12 = 1; c.prp1 = SBUF(0); c.prp2 = SBUF(0) + BS; }
    if (k == 1) { c.opcode = VNVME_IO_READ; c.cdw10 = (u32)lba; c.cdw12 = 1; c.prp1 = WAREA(s->w) + 0x10000; c.prp2 = WAREA(s->w) + 0x11000; }
    if (k == 2) { c.opcode = VNVME_IO_WRITE; c.cdw10 = (u32)s->region; c.prp1 = TRAP; }
    if (k == 3) { c.opcode = VNVME_IO_READ; c.cdw10 = 0; c.prp1 = IOCQ; }
    int st = run_cmd(s, &c, NULL);
    if (st < 0) return;
    if (st == 0) FAIL("invalid command %u succeeded", k);
    if (k == 2) { /* nothing written to the region */
        disk_get(s->region, 1, s->scratch);
        if (memcmp(s->scratch, s->shadow, BS)) FAIL("refused trap-sourced WRITE changed the disk");
    }
    s->n_bad++;
}
static int iters;
static unsigned hostile_pct = 12; /* env SMPS_HOSTILE */
static atomic_int barrier_count, barrier_total; /* macOS has no pthread_barrier_t */
static void barrier_wait(void)
{
    atomic_fetch_add(&barrier_count, 1);
    while (atomic_load(&barrier_count) < atomic_load(&barrier_total)) sched_yield();
}
static atomic_int workers_done;
static void *worker(void *arg)
{
    struct wstate *s = arg;
    t_cpu = (int)s->w;
    barrier_wait();
    int after_retire = 0;
    for (int i = 0; i < iters; i++) {
        if (atomic_load(&retired_flag) && ++after_retire > 8) break; /* retired: a few more knocks, nothing may come back */
        unsigned r = (unsigned)(rnd(s) % 100);
        bool hostile = s->w == 1 && r < hostile_pct;
        if (hostile) {
            if (r < hostile_pct / 2) op_write(s, true); else op_read(s, true);
        } else if (r < 45) op_write(s, false);
        else if (r < 85) op_read(s, false);
        else if (r < 92) op_flush(s);
        else op_bad(s);
        if (i % 32 == 31) check_guards();
    }
    atomic_fetch_add(&workers_done, 1);
    return NULL;
}
static void *boot_cpu(void *arg)
{
    (void)arg;
    t_cpu = 0;
    barrier_wait();
    u64 k = 0;
    while (atomic_load(&workers_done) < nworkers) {
        hv_win_storage_tick();
        u64 v;
        mmio(0x1c, 0, false, 2, &v); /* CSTS */
        if (++k % 16 == 0) { mmio(0x0c, 1, true, 2, NULL); mmio(0x10, 1, true, 2, NULL); } /* INTMS, INTMC */
        sched_yield();
    }
    return NULL;
}
static atomic_ullong retire_at;
static u8 *retire_disk, *retire_cq;
static u64 retire_dispatches, retire_ctrl_completions;
static void *retirer(void *arg)
{
    (void)arg;
    t_cpu = 23; /* a CPU hosting no vCPU: retire needs only the storage lock */
    barrier_wait();
    while (atomic_load(&completions_total) < atomic_load(&retire_at) && atomic_load(&workers_done) < nworkers) sched_yield();
    if (!strcmp(mode, "retire_nested")) {
        /* as from inside a hook: the caller already holds the storage lock, retire nests; everything read after it
         * (the controller's counters, the disk) is still under the caller's lock */
        hv_storage_lock_take();
        hv_win_storage_retire();
        retire_dispatches = dispatches + rejections + controller.queue.stats.commands;
        retire_ctrl_completions = controller.queue.stats.completions;
        memcpy(retire_disk, disk, (size_t)(GUARD + W_BLOCKS + GUARD) * BS);
        g_get(retire_cq, IOCQ, QD * 16);
        atomic_store(&retired_flag, 1);
        hv_storage_lock_release();
    } else {
        /* as hv.c does it: a plain call, the storage lock taken inside */
        hv_win_storage_retire();
        hv_storage_lock_take();
        retire_ctrl_completions = controller.queue.stats.completions;
        memcpy(retire_disk, disk, (size_t)(GUARD + W_BLOCKS + GUARD) * BS);
        g_get(retire_cq, IOCQ, QD * 16);
        atomic_store(&retired_flag, 1);
        hv_storage_lock_release();
    }
    return NULL;
}

/* ---------------------------------------------------------------- bring-up (single-threaded, cpu 0) */
static int admin_tail, admin_head, admin_phase = 1;
static u16 admin(struct vnvme_command c)
{
    static u16 cid = 0x8000;
    c.cid = ++cid;
    g_put(ASQ + (u64)admin_tail * 64, &c, 64);
    admin_tail = (admin_tail + 1) % 8;
    REQUIRE(mmio(0x1000, (u64)admin_tail, true, 2, NULL));
    u32 dw[4];
    REQUIRE(cq_take(ACQ + (u64)admin_head * 16, (cq_phase = admin_phase, dw)));
    if (++admin_head == 8) { admin_head = 0; admin_phase ^= 1; }
    REQUIRE(mmio(0x1004, (u64)admin_head, true, 2, NULL));
    REQUIRE((u16)dw[3] == c.cid);
    return (u16)(dw[3] >> 17);
}

int main(int argc, char **argv)
{
    if (argc != 5) { fprintf(stderr, "usage: test nvcpus iters seed main|latch|retire|retire_nested\n"); return 2; }
    int nv = atoi(argv[1]);
    iters = atoi(argv[2]);
    u64 seed = strtoull(argv[3], NULL, 0);
    mode = argv[4];
    if (getenv("SMPS_HOSTILE")) hostile_pct = (unsigned)atoi(getenv("SMPS_HOSTILE"));
    if (nv < 2 || nv > MAXW + 1) return 2;
    nworkers = nv - 1;
    t_cpu = 0;

    disk = calloc(GUARD + W_BLOCKS + GUARD, BS);
    disk_initial_guard = malloc(2 * GUARD * BS);
    latch_snapshot = malloc((size_t)(GUARD + W_BLOCKS + GUARD) * BS);
    retire_disk = malloc((size_t)(GUARD + W_BLOCKS + GUARD) * BS);
    retire_cq = malloc(QD * 16);
    REQUIRE(disk && disk_initial_guard && latch_snapshot && retire_disk && retire_cq);
    for (u64 l = 0; l < GUARD + W_BLOCKS + GUARD; l++)
        for (u32 i = 0; i < BS; i++) disk[l * BS + i] = l < GUARD || l >= GUARD + W_BLOCKS ? (u8)(0xa0 ^ i ^ l) : pattern_byte(l - GUARD, i);
    memcpy(disk_initial_guard, disk, GUARD * BS);
    memcpy(disk_initial_guard + GUARD * BS, disk + (GUARD + W_BLOCKS) * BS, GUARD * BS);

    void *g = mmap(NULL, GSIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    REQUIRE(g != MAP_FAILED && !((u64)g & 0x3fff));
    G = guest_base = (u64)g;
    guest_size = GSIZE;
    g_fill(TRAP, 0x4000, 0xeeeeeeeeeeeeeeeeULL);
    trap_initial = malloc(0x4000);
    g_get(trap_initial, TRAP, 0x4000);
    for (unsigned b = 0; b < NBUF; b++) scribble_pass(b, 0);

    /* resident: the binary's statics the PREP checks (marker, controller, fixture); must not overlap guest RAM */
    u64 lo = (u64)resident_marker, hi = lo + sizeof(resident_marker);
    u64 spans[2][2] = {{(u64)&controller, sizeof(controller)}, {(u64)s0_fixture.bytes, s0_fixture.byte_length}};
    for (int i = 0; i < 2; i++) { if (spans[i][0] < lo) lo = spans[i][0]; if (spans[i][0] + spans[i][1] > hi) hi = spans[i][0] + spans[i][1]; }
    lo &= ~0x3fffUL; hi = (hi + 0x3fff) & ~0x3fffUL;
    REQUIRE(hi <= G || lo >= G + GSIZE);

    hv_win.nr_vcpus = (u32)nv;
    for (int i = 0; i < MAX_CPUS; i++) hv_win.vcpu_cpu[i] = hv_win.cpu_vcpu[i] = -1;
    for (int v = 0; v < nv; v++) hv_win.vcpu_cpu[v] = hv_win.cpu_vcpu[v] = v;
    REQUIRE(hv_win_storage_max_cpus() == 1);
    REQUIRE(hv_win_storage_set_max_cpus((u32)nv));
    REQUIRE(!hv_win_storage_set_max_cpus(2)); /* once */
    REQUIRE(s1_select_backend(&fake_backend));
    static struct { struct hv_s1_registration head; struct hv_sm_range r[3]; } pk;
    pk.head = (struct hv_s1_registration){.device = {HV_S1_MAGIC, 1, HV_S1_ECAM, HV_S1_BAR, S1_BAR_SIZE, 66, 7, 4096, 0, W_BLOCKS, 0},
                                           .epoch = 17, .resident_base = lo, .resident_size = hi - lo, .nr_ram = 1, .nr_protected = 2};
    pk.r[0] = (struct hv_sm_range){G, GSIZE};
    pk.r[1] = (struct hv_sm_range){TRAP, 0x4000};
    pk.r[2] = (struct hv_sm_range){G + GSIZE + 0x100000, 0x4000};
    REQUIRE(hv_win_storage_prepare((u64)&pk, sizeof(pk)) == 0);
    for (int v = 0; v < nv; v++) hv_started_cpus[v] = true;
    hv_cpus_in_guest = (1UL << nv) - 1;

    REQUIRE(mmio(0x24, 0x00070007, true, 2, NULL));
    REQUIRE(mmio(0x28, ASQ, true, 3, NULL));
    REQUIRE(mmio(0x30, ACQ, true, 3, NULL));
    REQUIRE(mmio(0x14, S1_CC_VALID, true, 2, NULL));
    REQUIRE(admin((struct vnvme_command){.opcode = VNVME_ADMIN_IDENTIFY, .nsid = 1, .prp1 = IDBUF, .cdw10 = 0}) == 0);
    REQUIRE(g_ld64(IDBUF) == W_BLOCKS);
    REQUIRE(admin((struct vnvme_command){.opcode = VNVME_ADMIN_CREATE_CQ, .prp1 = IOCQ, .cdw10 = ((QD - 1) << 16) | 1, .cdw11 = 3}) == 0);
    REQUIRE(admin((struct vnvme_command){.opcode = VNVME_ADMIN_CREATE_SQ, .prp1 = IOSQ, .cdw10 = ((QD - 1) << 16) | 1, .cdw11 = (1 << 16) | 1}) == 0);
    cq_phase = 1;
    (void)g_ld32;

    if (!strcmp(mode, "latch")) fake.fail_at_write = 50 + seed % 400;
    atomic_store(&retire_at, !strncmp(mode, "retire", 6) ? 200 + seed % 600 : ~0ULL);

    struct wstate *ws = calloc((size_t)nworkers + 1, sizeof(*ws));
    u64 rb = (W_BLOCKS - STATIC_BLOCKS) / (u64)nworkers;
    atomic_store(&barrier_total, nworkers + 1 + !strncmp(mode, "retire", 6));
    pthread_t th[MAXW + 8];
    int nt = 0;
    for (long i = 0; i < 2; i++) pthread_create(&th[nt++], NULL, scribbler, (void *)i);
    pthread_t fl;
    pthread_create(&fl, NULL, flipper, NULL);
    for (int w = 1; w <= nworkers; w++) {
        struct wstate *s = &ws[w];
        s->w = (unsigned)w;
        s->rng = splitmix(seed * 1000 + (u64)w);
        s->region = STATIC_BLOCKS + (u64)(w - 1) * rb;
        s->region_blocks = rb;
        s->shadow = malloc(rb * BS);
        s->scratch = malloc(300 * BS);
        for (u64 l = 0; l < rb; l++) for (u32 i = 0; i < BS; i++) s->shadow[l * BS + i] = pattern_byte(s->region + l, i);
        pthread_create(&th[nt++], NULL, worker, s);
    }
    pthread_t boot, ret;
    pthread_create(&boot, NULL, boot_cpu, NULL);
    if (!strncmp(mode, "retire", 6)) pthread_create(&ret, NULL, retirer, NULL);
    for (int i = 2; i < nt; i++) pthread_join(th[i], NULL);
    pthread_join(boot, NULL);
    if (!strncmp(mode, "retire", 6)) pthread_join(ret, NULL);
    atomic_store(&stop_scribblers, 1);
    pthread_join(th[0], NULL); pthread_join(th[1], NULL); pthread_join(fl, NULL);

    t_cpu = 0;
    check_guards();
    u64 tot[13] = {0};
    for (int w = 1; w <= nworkers; w++) {
        struct wstate *s = &ws[w];
        u64 v[13] = {s->n_write, s->n_read, s->n_flush, s->n_bad, s->n_hostile_w, s->n_hostile_r, s->n_refused,
                     s->n_after_latch, s->n_tight_words, s->n_torn_words, s->n_loose_words, s->n_dup, s->n_mutated};
        for (int i = 0; i < 13; i++) tot[i] += v[i];
    }
    if (!strcmp(mode, "main")) {
        /* quiescent: every command completed exactly once, the line is low, nothing pending */
        process_cq();
        for (int w = 1; w <= nworkers; w++) REQUIRE(atomic_load(&outst[w].done) && atomic_load(&outst[w].count) == 1);
        REQUIRE(controller.queue.queues[1].cq_pending == 0 && !controller.queue.irq_asserted && !irq_level);
        REQUIRE(fake.violations == 0 && !fake.latched);
        REQUIRE(tot[0] > 0 && tot[1] > 0);
    }
    if (!strcmp(mode, "latch")) {
        REQUIRE(atomic_load(&latched_seen));
        if (memcmp(disk, latch_snapshot, (size_t)(GUARD + W_BLOCKS + GUARD) * BS)) FAIL("the disk changed after the latch");
        REQUIRE(fake.writes == fake.fail_at_write); /* nothing reached the backend after the latch */
    }
    if (!strncmp(mode, "retire", 6)) {
        REQUIRE(atomic_load(&retired_flag));
        if (memcmp(disk, retire_disk, (size_t)(GUARD + W_BLOCKS + GUARD) * BS)) FAIL("the disk changed after retire");
        u8 cq[QD * 16];
        g_get(cq, IOCQ, sizeof(cq));
        /* consumed entries are poisoned by the guest; compare only what the controller writes: phase dwords */
        for (int i = 0; i < QD; i++) if (memcmp(cq + i * 16 + 12, retire_cq + i * 16 + 12, 4)) FAIL("a CQE was written after retire");
        /* the guest may still consume CQEs posted before retire; none may be posted after it */
        REQUIRE(controller.queue.stats.completions == retire_ctrl_completions);
        REQUIRE(controller.faulted);
        REQUIRE(!strcmp(mode, "retire") || retire_dispatches > 0);
    }
    printf("{\"nvcpus\":%d,\"mode\":\"%s\",\"completions\":%llu,\"writes\":%llu,\"reads\":%llu,\"flushes\":%llu,"
           "\"invalid\":%llu,\"hostile_writes\":%llu,\"hostile_reads\":%llu,\"refused\":%llu,\"after_stop\":%llu,"
           "\"words_exact\":%llu,\"words_torn\":%llu,\"words_unbounded\":%llu,\"dup_src_writes\":%llu,\"src_mutated_writes\":%llu,\"backend_writes\":%llu,"
           "\"irq_edges\":%d,\"pmu_dma_calls\":%llu,\"polls\":%llu}\n",
           nv, mode, (unsigned long long)atomic_load(&completions_total), (unsigned long long)tot[0],
           (unsigned long long)tot[1], (unsigned long long)tot[2], (unsigned long long)tot[3], (unsigned long long)tot[4],
           (unsigned long long)tot[5], (unsigned long long)tot[6], (unsigned long long)tot[7], (unsigned long long)tot[8],
           (unsigned long long)tot[9], (unsigned long long)tot[10], (unsigned long long)tot[11],
           (unsigned long long)tot[12], (unsigned long long)fake.writes, irq_edges, (unsigned long long)pmu_dma_calls, (unsigned long long)fake.polls);
    return 0;
}
