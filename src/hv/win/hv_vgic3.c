/* SPDX-License-Identifier: MIT */
/*
 * GICv3 distributor/redistributor emulation for the Windows guest (M1+ interrupt model).
 *
 * Apple cores implement the GICv3 *virtual* CPU interface in hardware (ICH_*_EL2 list registers;
 * Linux KVM on T6050 reports "GIC system register CPU interface enabled" and a vgic maintenance
 * IRQ). So only the distributor, the per-CPU redistributors, and SGI routing need emulating; the
 * actual delivery to EL1 is done by the hardware virtual CPU interface through the list registers.
 *
 * SCAFFOLD. This compiles and holds the state machine's shape, but delivery (hv_vgic3_flush writing
 * real ICH_LRn_EL2), the GICD/GICR MMIO hooks, and the AIC-line -> SPI map are TODO.
 * hv_win_configure() refuses HV_WIN_IRQ_VGIC3 unless HV_WIN_VGIC3_READY is defined, so none of this
 * runs yet.
 *
 * Plan and numbering: m5pro-windows docs/EL2-RUNTIME.md.
 */

#include "../hv.h"
#include "hv_win.h"
#include "hv_win_regs.h"
#include "smp.h"
#include "string.h"
#include "utils.h"

#define VGIC_MAX_SPI     988 /* up to INTID 1019; 32 .. 1019 */
#define VGIC_MAX_INTID   1020
#define VGIC_INTID_WORDS ((VGIC_MAX_INTID + 31) / 32)

struct vgic_dist {
    bool enabled;
    u64 gicd_base;
    u64 gicr_base;
    u32 nr_vcpus;
    u32 typer;
    /* SPI state is global; SGI/PPI state (INTID < 32) is per redistributor (per CPU) */
    u32 spi_enabled[VGIC_INTID_WORDS];
    u32 spi_pending[VGIC_INTID_WORDS];
    u32 spi_active[VGIC_INTID_WORDS];
    u8 spi_priority[VGIC_MAX_INTID];
    u64 spi_router[VGIC_MAX_INTID]; /* GICD_IROUTER: affinity or IRM */
};

struct vgic_cpu {
    bool valid;
    u32 ppi_enabled; /* INTID 0..31 enable bits */
    u32 ppi_pending;
    u32 ppi_active;
    u32 ppi_level; /* current input level for level-triggered PPIs (timers) */
    u32 num_lrs;
};

static struct vgic_dist vgic;
static struct vgic_cpu vgic_cpu[MAX_CPUS];

int hv_vgic3_init(u64 gicd_base, u64 gicr_base, u32 nr_vcpus)
{
    memset(&vgic, 0, sizeof(vgic));
    memset(vgic_cpu, 0, sizeof(vgic_cpu));

    vgic.gicd_base = gicd_base;
    vgic.gicr_base = gicr_base;
    vgic.nr_vcpus = nr_vcpus;
    /* ITLinesNumber: (N+1)*32 - 1 >= highest INTID; 30 -> 992 lines is plenty for SPI use so far */
    vgic.typer = FIELD_PREP(GICD_TYPER_ITLINES, 30) | FIELD_PREP(GICD_TYPER_IDBITS, 9);
    vgic.enabled = true;

    /*
     * TODO: hv_map_hook() the GICD frame (gicd_base, GICD_SIZE) and each CPU's GICR frame pair
     * (gicr_base + cpu*GICR_STRIDE, 2*0x10000) to MMIO handlers. Needs the host to have carved
     * those IPAs out of guest RAM first (they must not be backed by DRAM). See EL2-RUNTIME.md
     * "Memory map".
     */
    printf("HV/win vGIC3: init GICD 0x%lx GICR 0x%lx for %u vCPU(s) [SCAFFOLD, no MMIO hooks]\n",
           gicd_base, gicr_base, nr_vcpus);
    return 0;
}

void hv_vgic3_cpu_init(void)
{
    int cpu = smp_id();
    if (cpu < 0 || cpu >= MAX_CPUS)
        return;

    struct vgic_cpu *vc = &vgic_cpu[cpu];
    vc->num_lrs = FIELD_GET(ICH_VTR_LISTREGS, mrs(SYS_ICH_VTR_EL2)) + 1;
    vc->valid = true;

    /*
     * TODO: enable the virtual CPU interface -- ICC_SRE_EL2.{SRE,Enable}, ICH_HCR_EL2.En, clear the
     * list registers, program ICH_VMCR_EL2 (VENG1). The hardware delivers whatever we put in the
     * LRs as a virtual IRQ while HCR_EL2.IMO is set.
     */
}

bool hv_vgic3_handle_sgi1r(u64 val)
{
    if (!vgic.enabled)
        return false;

    u32 intid = FIELD_GET(ICC_SGI1R_INTID, val);
    bool irm = val & ICC_SGI1R_IRM;

    /*
     * TODO: resolve (Aff3, Aff2, Aff1, RangeSelector, TargetList) -> target CPU set (or
     * all-but-self for IRM), matching MPIDR affinities, mark the SGI pending on each target's
     * redistributor and kick it. Aura's windows-sgi-routing.md failure model (dense frame index !=
     * physical CPU id; RangeSelector selects the upper nibble of Aff0; IRM excludes the sender)
     * applies here. For now, acknowledge so the guest's ICC_SGI1R_EL1 write does not fault.
     */
    UNUSED(intid);
    UNUSED(irm);
    return true;
}

void hv_vgic3_set_ppi_level(int cpu, u32 intid, bool level)
{
    if (cpu < 0 || cpu >= MAX_CPUS || intid >= GIC_NR_PRIVATE)
        return;

    struct vgic_cpu *vc = &vgic_cpu[cpu];
    if (level)
        vc->ppi_level |= BIT(intid);
    else
        vc->ppi_level &= ~BIT(intid);
}

void hv_vgic3_inject_spi(u32 intid)
{
    if (intid < GIC_NR_PRIVATE || intid >= VGIC_MAX_INTID)
        return;

    __atomic_fetch_or(&vgic.spi_pending[intid / 32], BIT(intid % 32), __ATOMIC_ACQ_REL);
    /* TODO: pick the target CPU from spi_router[intid] and kick it so it flushes to an LR */
}

void hv_vgic3_flush(void)
{
    int cpu = smp_id();
    if (cpu < 0 || cpu >= MAX_CPUS || !vgic_cpu[cpu].valid)
        return;

    /*
     * TODO: for each enabled+pending, not-yet-active INTID targeting this CPU (PPI from ppi_level
     * and the timer PPIs, SGIs, then SPIs within priority), find a free list register
     * (ICH_ELRSR_EL2) and write ICH_LRn_EL2 with STATE_PEND | GROUP1 | priority | vINTID. Set
     * ICH_HCR_EL2.UIE when more are pending than there are LRs. This is the only place real
     * injection happens.
     */
}
