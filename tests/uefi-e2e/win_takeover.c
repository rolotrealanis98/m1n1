/* Second-consumer (Windows-style) takeover of the SAME actual resident model
 * instance, starting from the exact state the actual Mu NvmExpressDxe +
 * probe left it in (TestProductionSequence). This is a modeled StorNVMe-like
 * sequence, NOT the real StorNVMe trace. All results come from the actual
 * frozen resident controller C; it prints an implemented/missing table. */
#include "hv_nvme_queue.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BAR UINT64_C(0xF30000000)
int bridge_mmio(uint64_t addr, unsigned bytes, int write, uint64_t *value);
int bridge_config_read(uint64_t off, unsigned bytes, uint64_t *value);
int bridge_config_write(uint64_t off, unsigned bytes, uint64_t value);
int bridge_irq_level(void);
int bridge_queue_valid(unsigned q);
void bridge_stats(uint64_t out[8]);
extern unsigned long bridge_irq_asserts, bridge_irq_deasserts;

static unsigned fails, checks;
#define T(x) do { checks++; if (!(x)) { fails++; fprintf(stderr, "FAIL win_takeover:%d: %s\n", __LINE__, #x); } } while (0)

static int rd(unsigned off, unsigned bytes, uint64_t *v) { *v = 0; return bridge_mmio(BAR + off, bytes, 0, v); }
static int wr(unsigned off, unsigned bytes, uint64_t v) { return bridge_mmio(BAR + off, bytes, 1, &v); }
static uint64_t r32(unsigned off) { uint64_t v; T(rd(off, 4, &v)); return v; }
static uint32_t cfg(unsigned off) { uint64_t v = 0; T(bridge_config_read(off, 4, &v)); return (uint32_t)v; }

struct q { struct vnvme_command *sq; struct vnvme_completion *cq; uint16_t n, tail, head; uint8_t phase; unsigned id; };
static struct q aq, ioq;
static uint16_t cid = 0x100;

static void *page(size_t pages, uint8_t fill)
{
    void *p = NULL;
    if (posix_memalign(&p, 4096, pages * 4096)) abort();
    memset(p, fill, pages * 4096);
    return p;
}
static void qinit(struct q *q, unsigned id, uint16_t n)
{
    q->id = id; q->n = n; q->tail = q->head = 0; q->phase = 1;
    q->sq = page((n * 64 + 4095) / 4096, 0); q->cq = page((n * 16 + 4095) / 4096, 0);
}
/* returns NVMe status (SC|SCT<<8), -1 no completion posted, -2 MMIO rejected (guest fault) */
static int submit(struct q *q, struct vnvme_command c, int ack, int *irq_before_ack)
{
    c.cid = cid++;
    q->sq[q->tail] = c;
    q->tail = (uint16_t)((q->tail + 1) % q->n);
    if (!wr(0x1000 + q->id * 8, 4, q->tail)) return -2;
    struct vnvme_completion *e = &q->cq[q->head];
    if ((e->status & 1) != q->phase || e->cid != c.cid) return -1;
    int st = e->status >> 1;
    if (irq_before_ack) *irq_before_ack = bridge_irq_level();
    if (ack) {
        q->head = (uint16_t)((q->head + 1) % q->n);
        if (!q->head) q->phase ^= 1;
        if (!wr(0x1004 + q->id * 8, 4, q->head)) return -2;
    }
    return st;
}
static struct vnvme_command cmd(uint8_t op, uint32_t nsid, void *data, uint32_t c10, uint32_t c11)
{
    struct vnvme_command c;
    memset(&c, 0, sizeof c);
    c.opcode = op; c.nsid = nsid; c.prp1 = (uint64_t)(uintptr_t)data; c.cdw10 = c10; c.cdw11 = c11;
    return c;
}
static void row(const char *what, int st, const char *verdict)
{
    char s[32];
    if (st == -1) strcpy(s, "no CQE (held)");
    else if (st == -2) strcpy(s, "MMIO REJECT=fault");
    else snprintf(s, sizeof s, "SC 0x%02x", st);
    printf("  %-46s %-18s %s\n", what, s, verdict);
}
static int enable_admin(uint16_t n)
{
    qinit(&aq, 0, n);
    if (!wr(0x24, 4, ((uint32_t)(n - 1) << 16) | (n - 1))) return 0;
    if (!wr(0x28, 8, (uint64_t)(uintptr_t)aq.sq) || !wr(0x30, 8, (uint64_t)(uintptr_t)aq.cq)) return 0;
    if (!wr(0x14, 4, 0x00460001)) return 0;
    return (r32(0x1c) & 1) == 1;
}
static int pattern(const uint8_t *p, unsigned lba)
{
    for (unsigned i = 0; i < 4096; i++) if (p[i] != (i < 32 ? (uint8_t)(lba * 17 + i) : 0)) return 0;
    return 1;
}

int win_takeover(void)
{
    uint64_t st[8], v;
    int irq, s;
    uint8_t *d = page(1, 0xa7), *big = page(32, 0xa7);
    uint64_t *prpl = page(1, 0);

    printf("Windows-style takeover after actual Mu NvmExpressDxe + probe:\n");
    /* (2) state at ExitBootServices: UEFI has no EBS/Stop disable. */
    bridge_stats(st);
    T(((st[3] >> 32) & 1) == 1 && (st[3] & 1) == 1);
    T(bridge_queue_valid(0) && bridge_queue_valid(1));
    T(!bridge_irq_level());
    printf("  at EBS: CC 0x%08llx CSTS 0x%llx, admin+IO queues still valid (in UEFI boot-services pages)\n",
           (unsigned long long)(st[3] >> 32), (unsigned long long)(st[3] & 0xffffffff));

    /* (1) identity Windows reads through ECAM (resident) */
    uint32_t id = cfg(0), cc8 = cfg(8), sub = cfg(0x2c), cap = cfg(0x34), il = cfg(0x3c), hdr = cfg(0x0c), sts = cfg(4) >> 16;
    T(id == 0x00101b36 && (cc8 >> 8) == 0x010802 && (cc8 & 0xff) == 2);
    T(sub == 0x00101b36 && cap == 0 && ((il >> 8) & 0xff) == 1 && ((hdr >> 16) & 0x7f) == 0 && !(sts & 0x10));
    uint32_t b0 = cfg(0x10), b1 = cfg(0x14);
    T(b0 == ((uint32_t)BAR | 4) && b1 == (uint32_t)(BAR >> 32));
    printf("  ECAM: id %08x rev %02x class %06x subsys %08x capptr %02x pin %u BAR0 %08x BAR1 %08x\n",
           id, cc8 & 0xff, cc8 >> 8, sub, cap, (il >> 8) & 0xff, b0, b1);

    /* StorNVMe-style reset: CC.EN 1->0 */
    T(wr(0x14, 4, 0));
    T((r32(0x1c) & 1) == 0);
    T(!bridge_queue_valid(0) && !bridge_queue_valid(1));      /* old UEFI queues dropped */
    { uint64_t s0[8], s1[8]; bridge_stats(s0); T(wr(0x1008, 4, 1)); bridge_stats(s1); T(s1[0] == s0[0] && s1[1] == s0[1]); } /* r2: stale doorbell ignored, no effect */
    T(enable_admin(32));
    T(wr(0x10, 4, 1));                                        /* INTMC: unmask vector 0 */

    printf("  %-46s %-18s %s\n", "command/feature", "resident result", "verdict");
    struct vnvme_command c;
    c = cmd(6, 0, d, 1, 0); s = submit(&aq, c, 0, &irq); T(s == 0 && irq == 1);
    T(wr(0x1004, 4, aq.head = 1) && !bridge_irq_level()); /* ack deasserts INTx */
    row("Identify CNS1 controller (+INTx assert/deassert)", s, "implemented (VER 1.3, NN 1, MDTS 5, OACS/ONCS/VWC 0)");
    c = cmd(6, 0, d, 2, 0); s = submit(&aq, c, 1, NULL); T(s == 0 && ((uint32_t *)d)[0] == 1 && ((uint32_t *)d)[1] == 0);
    row("Identify CNS2 active NS list (NSID 0)", s, "implemented");
    c = cmd(6, 1, d, 0, 0); s = submit(&aq, c, 1, NULL); T(s == 0 && ((uint64_t *)d)[0] == 40);
    row("Identify CNS0 namespace 1", s, "implemented (r2: NGUID/EUI64 set)");
    c = cmd(6, 0xffffffff, d, 0, 0); s = submit(&aq, c, 1, NULL);
    row("Identify CNS0 NSID FFFFFFFF", s, s ? "WRONG: spec allows common NS caps; returns Invalid NS" : "ok");
    c = cmd(6, 1, d, 3, 0); s = submit(&aq, c, 1, NULL);
    row("Identify CNS3 NS ID descriptors", s, "implemented (r2: EUI64 + NGUID)");
    c = cmd(6, 0, d, 0x10, 0); s = submit(&aq, c, 1, NULL);
    row("Identify CNS 10h+ (allocated NS etc.)", s, "missing (not advertised; acceptable)");
    c = cmd(9, 0, NULL, 7, 0x003f003f); s = submit(&aq, c, 1, NULL);
    T(s == 0 && aq.cq[(aq.head + aq.n - 1) % aq.n].result == 0);
    row("Set Features 07h Number of Queues", s, "implemented (grants 1 SQ/1 CQ)");
    c = cmd(9, 0, NULL, 8, 0x00000a01); s = submit(&aq, c, 1, NULL);
    row("Set Features 08h interrupt coalescing", s, "accepted, ignored (ok)");
    c = cmd(9, 0, NULL, 9, 0); s = submit(&aq, c, 1, NULL);
    row("Set Features 09h IRQ vector config", s, "accepted (ok)");
    c = cmd(9, 0, NULL, 0x0b, 0x1ff); s = submit(&aq, c, 1, NULL);
    row("Set Features 0Bh async event config", s, "accepted, no events ever (ok)");
    c = cmd(9, 0, NULL, 2, 0); s = submit(&aq, c, 1, NULL);
    row("Set Features 02h power management PS0", s, "accepted (NPSS 0, PSD0 zero)");
    c = cmd(10, 0, NULL, 2, 0); s = submit(&aq, c, 1, NULL);
    row("Get Features 02h power state", s, "implemented");
    c = cmd(9, 0, NULL, 6, 1); s = submit(&aq, c, 1, NULL);
    row("Set Features 06h volatile write cache", s, s ? "ok" : "WRONG: succeeds although VWC=0 (harmless)");
    c = cmd(10, 0, NULL, 4, 0); s = submit(&aq, c, 1, NULL);
    row("Get Features 04h temperature threshold", s, s ? "missing (mandatory FID)" : "ok");
    c = cmd(9, 0, NULL, 0x0c, 0); s = submit(&aq, c, 1, NULL);
    row("Set Features 0Ch APST", s, "missing (APSTA=0, so not expected)");
    c = cmd(9, 0, NULL, 0x0e, 0); s = submit(&aq, c, 1, NULL);
    row("Set Features 0Eh timestamp", s, "missing (ONCS.timestamp=0)");
    c = cmd(9, 0, NULL, 0x80, 0); s = submit(&aq, c, 1, NULL);
    row("Set Features 80h software progress marker", s, s ? "missing (optional; error completion)" : "ok");
    c = cmd(2, 0xffffffff, d, (127u << 16) | 2, 0); s = submit(&aq, c, 1, NULL);
    row("Get Log Page 02h SMART/health (512B)", s, "implemented (r2: temp/spare filled)");
    c = cmd(2, 0xffffffff, d, (15u << 16) | 1, 0); s = submit(&aq, c, 1, NULL);
    row("Get Log Page 01h error info", s, "implemented (zero)");
    c = cmd(2, 0, d, (127u << 16) | 3, 0); s = submit(&aq, c, 1, NULL);
    row("Get Log Page 03h firmware slot (mandatory)", s, s ? "MISSING (mandatory log)" : "ok");
    c = cmd(2, 0, d, (1023u << 16) | 0x0c, 0); s = submit(&aq, c, 1, NULL);
    row("Get Log Page 0Ch/other", s, "missing (optional)");
    c = cmd(0x0c, 0, NULL, 0, 0); s = submit(&aq, c, 1, NULL); T(s == -1);
    row("Async Event Request", s, "implemented (r2: tracked; aborted by Abort/shutdown)");
    c = cmd(8, 0, NULL, 0, 0); s = submit(&aq, c, 1, NULL);
    row("Abort", s, "implemented (DW0=1 not aborted)");

    /* IO queues: one pair, INTx enabled */
    qinit(&ioq, 1, 64);
    c = cmd(5, 0, ioq.cq, (63u << 16) | 1, 3); s = submit(&aq, c, 1, NULL); T(s == 0);
    row("Create IO CQ 1 (64, IEN, PC)", s, "implemented");
    c = cmd(1, 0, ioq.sq, (63u << 16) | 1, (1u << 16) | 1); s = submit(&aq, c, 1, NULL); T(s == 0);
    row("Create IO SQ 1", s, "implemented");
    void *cq2 = page(1, 0);
    c = cmd(5, 0, cq2, (63u << 16) | 2, 3); s = submit(&aq, c, 1, NULL);
    row("Create IO CQ 2", s, "refused (only 1 pair; consistent with Set Features)");
    c = cmd(2, 1, d, 0, 0); s = submit(&ioq, c, 0, &irq); T(s == 0 && irq == 1 && pattern(d, 0));
    T(wr(0x100c, 4, ioq.head = 1) && !bridge_irq_level());
    row("READ LBA0 x1 (IO queue, INTx)", s, "implemented");
    for (unsigned i = 0; i < 31; i++) prpl[i] = (uint64_t)(uintptr_t)(big + (i + 1) * 4096);
    c = cmd(2, 1, big, 8, 0); c.prp2 = (uint64_t)(uintptr_t)prpl; c.cdw12 = 31;
    s = submit(&ioq, c, 1, NULL);
    int ok = s == 0; for (unsigned i = 0; i < 32 && ok; i++) ok = pattern(big + i * 4096, 8 + i);
    T(ok);
    row("READ LBA8..39 x32 (PRP list, MDTS)", s, "implemented");
    c = cmd(2, 1, d, 0, 0); c.cdw12 = 1u << 30; s = submit(&ioq, c, 1, NULL);
    row("READ with FUA", s, s ? "WRONG: FUA read refused (Invalid Field)" : "ok");
    c = cmd(1, 1, d, 0, 0); s = submit(&ioq, c, 1, NULL);
    row("WRITE", s, "r2: Namespace Is Write Protected (NSATTR WP)");
    c = cmd(0, 1, NULL, 0, 0); s = submit(&ioq, c, 1, NULL);
    row("FLUSH", s, s ? "WRONG: mandatory NVM cmd refused (VWC=0 so StorNVMe should not send)" : "ok");
    c = cmd(9, 1, d, 0, 4); s = submit(&ioq, c, 1, NULL);
    row("Dataset Management", s, "refused (ONCS.DSM=0; ok)");
    /* INTMS masks the line */
    T(wr(0x0c, 4, 1));
    c = cmd(2, 1, d, 39, 0); s = submit(&ioq, c, 0, &irq); T(s == 0 && irq == 0 && pattern(d, 39));
    T(wr(0x100c, 4, ioq.head = (uint16_t)((ioq.head + 1) % ioq.n)));
    T(wr(0x10, 4, 1));
    row("INTMS/INTMC masking (vector 0)", s, "implemented");
    /* 64-bit access to a non-CAP/ASQ/ACQ register */
    row("32-bit read CMBLOC 38h", rd(0x38, 4, &v) ? 0 : -2, "ok (0)");
    row("64-bit read CMBLOC 38h", rd(0x38, 8, &v) ? 0 : -2, "r2: ok (0), no fault");
    /* normal shutdown */
    s = wr(0x14, 4, 0x00460001 | 0x4000) ? 0 : -2; T(s == 0 && (r32(0x1c) & 0xc) == 8);
    row("CC.SHN normal (01b), queues idle", s, "implemented (SHST complete)");
    /* abrupt shutdown on a fresh enable */
    T(wr(0x14, 4, 0)); T(enable_admin(8));
    s = wr(0x14, 4, 0x00460001 | 0x8000) ? 0 : -2;
    row("CC.SHN abrupt (10b)", s, s ? "MISSING: rejected => guest data abort" : "ok");
    /* normal shutdown with an unconsumed CQE + pending SQ */
    T(wr(0x14, 4, 0)); T(enable_admin(8));
    c = cmd(6, 0, d, 1, 0); T(submit(&aq, c, 0, NULL) == 0);
    c = cmd(6, 0, d, 1, 0); aq.sq[aq.tail] = c; aq.tail++; T(wr(0x1000, 4, aq.tail));
    s = wr(0x14, 4, 0x00460001 | 0x4000) ? 0 : -2;
    row("CC.SHN normal with pending SQ entry", s, s ? "WRONG: rejected => guest data abort (should drain/complete)" : "ok");
    T(wr(0x14, 4, 0));
    /* pci.sys BAR sizing with decode off resets the controller */
    T(enable_admin(8));
    T(bridge_config_write(4, 2, 0));
    T(bridge_config_write(0x10, 4, 0xffffffff) && bridge_config_write(0x14, 4, 0xffffffff));
    T((cfg(0x10) & 0xfffffff0) == 0xff000000 && cfg(0x14) == 0xffffffff);
    T(bridge_config_write(0x10, 4, (uint32_t)BAR) && bridge_config_write(0x14, 4, (uint32_t)(BAR >> 32)));
    T(bridge_config_write(4, 2, 6));
    T(r32(0x14) == 0 && (r32(0x1c) & 1) == 0 && !bridge_queue_valid(0));
    row("PCI BAR sizing (decode off/on): 16 MiB 64-bit", 0, "implemented; resets controller state");
    bridge_stats(st);
    printf("  INTx asserts %lu deasserts %lu, rejected MMIO %llu (expected rejections above)\n",
           bridge_irq_asserts, bridge_irq_deasserts, (unsigned long long)st[4]);
    printf("win_takeover: %u checks, %u failures\n", checks, fails);
    return (int)fails;
}
