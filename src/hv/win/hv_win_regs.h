/* SPDX-License-Identifier: MIT */
/*
 * GICv3 architectural definitions used by the Windows EL2 runtime.
 *
 * Apple cores (M1 and later, including the T6050 cores, where Linux KVM reports "GIC system
 * register CPU interface enabled" and a vgic maintenance interrupt) implement the GICv3 *virtual*
 * CPU interface: ICH_*_EL2 with list registers. There is no physical distributor or redistributor;
 * the runtime emulates those in software (hv_vgic3.c) and injects through the list registers.
 *
 * Encodings are spelled with sys_reg() so the assembler does not need to know the GIC names.
 */

#ifndef HV_WIN_REGS_H
#define HV_WIN_REGS_H

#include "types.h"

/* EL2 virtual CPU interface control */
#define SYS_ICC_SRE_EL2   sys_reg(3, 4, 12, 9, 5)
#define SYS_ICH_AP0R0_EL2 sys_reg(3, 4, 12, 8, 0)
#define SYS_ICH_AP0R1_EL2 sys_reg(3, 4, 12, 8, 1)
#define SYS_ICH_AP0R2_EL2 sys_reg(3, 4, 12, 8, 2)
#define SYS_ICH_AP0R3_EL2 sys_reg(3, 4, 12, 8, 3)
#define SYS_ICH_AP1R0_EL2 sys_reg(3, 4, 12, 9, 0)
#define SYS_ICH_AP1R1_EL2 sys_reg(3, 4, 12, 9, 1)
#define SYS_ICH_AP1R2_EL2 sys_reg(3, 4, 12, 9, 2)
#define SYS_ICH_AP1R3_EL2 sys_reg(3, 4, 12, 9, 3)
#define SYS_ICH_HCR_EL2   sys_reg(3, 4, 12, 11, 0)
#define SYS_ICH_VTR_EL2   sys_reg(3, 4, 12, 11, 1)
#define SYS_ICH_MISR_EL2  sys_reg(3, 4, 12, 11, 2)
#define SYS_ICH_EISR_EL2  sys_reg(3, 4, 12, 11, 3)
#define SYS_ICH_ELRSR_EL2 sys_reg(3, 4, 12, 11, 5)
#define SYS_ICH_VMCR_EL2  sys_reg(3, 4, 12, 11, 7)
#define SYS_ICH_LR0_EL2   sys_reg(3, 4, 12, 12, 0)
#define SYS_ICH_LR1_EL2   sys_reg(3, 4, 12, 12, 1)
#define SYS_ICH_LR2_EL2   sys_reg(3, 4, 12, 12, 2)
#define SYS_ICH_LR3_EL2   sys_reg(3, 4, 12, 12, 3)
#define SYS_ICH_LR4_EL2   sys_reg(3, 4, 12, 12, 4)
#define SYS_ICH_LR5_EL2   sys_reg(3, 4, 12, 12, 5)
#define SYS_ICH_LR6_EL2   sys_reg(3, 4, 12, 12, 6)
#define SYS_ICH_LR7_EL2   sys_reg(3, 4, 12, 12, 7)
#define SYS_ICH_LR8_EL2   sys_reg(3, 4, 12, 13, 0)
#define SYS_ICH_LR9_EL2   sys_reg(3, 4, 12, 13, 1)
#define SYS_ICH_LR10_EL2  sys_reg(3, 4, 12, 13, 2)
#define SYS_ICH_LR11_EL2  sys_reg(3, 4, 12, 13, 3)
#define SYS_ICH_LR12_EL2  sys_reg(3, 4, 12, 13, 4)
#define SYS_ICH_LR13_EL2  sys_reg(3, 4, 12, 13, 5)
#define SYS_ICH_LR14_EL2  sys_reg(3, 4, 12, 13, 6)
#define SYS_ICH_LR15_EL2  sys_reg(3, 4, 12, 13, 7)

#define ICC_SRE_EL2_SRE    BIT(0)
#define ICC_SRE_EL2_DFB    BIT(1)
#define ICC_SRE_EL2_DIB    BIT(2)
#define ICC_SRE_EL2_ENABLE BIT(3)

#define ICH_HCR_EN       BIT(0)
#define ICH_HCR_UIE      BIT(1)
#define ICH_HCR_LRENPIE  BIT(2)
#define ICH_HCR_NPIE     BIT(3)
#define ICH_HCR_TC       BIT(10)
#define ICH_HCR_TALL0    BIT(11)
#define ICH_HCR_TALL1    BIT(12)
#define ICH_HCR_TDIR     BIT(14)
#define ICH_HCR_EOICOUNT GENMASK(31, 27)

#define ICH_VTR_LISTREGS GENMASK(4, 0)
#define ICH_VTR_IDBITS   GENMASK(25, 23)
#define ICH_VTR_PREBITS  GENMASK(28, 26)
#define ICH_VTR_PRIBITS  GENMASK(31, 29)

#define ICH_MISR_EOI   BIT(0)
#define ICH_MISR_U     BIT(1)
#define ICH_MISR_LRENP BIT(2)
#define ICH_MISR_NP    BIT(3)

#define ICH_VMCR_VENG0 BIT(0)
#define ICH_VMCR_VENG1 BIT(1)
#define ICH_VMCR_VPMR  GENMASK(31, 24)

#define ICH_LR_VINTID       GENMASK(31, 0)
#define ICH_LR_EOI          BIT(41) /* software interrupt (HW=0): maintenance IRQ on EOI */
#define ICH_LR_PRIORITY     GENMASK(55, 48)
#define ICH_LR_GROUP1       BIT(60)
#define ICH_LR_HW           BIT(61)
#define ICH_LR_STATE        GENMASK(63, 62)
#define ICH_LR_STATE_INV    0
#define ICH_LR_STATE_PEND   1
#define ICH_LR_STATE_ACTIVE 2
#define ICH_LR_STATE_PA     3
#define ICH_LR_PENDING_BIT  BIT(62)
#define ICH_LR_ACTIVE_BIT   BIT(63)

/* EL1 CPU interface registers the guest uses; these trap when HCR_EL2.IMO/FMO are set */
#define SYS_ICC_SGI1R_EL1  sys_reg(3, 0, 12, 11, 5)
#define SYS_ICC_ASGI1R_EL1 sys_reg(3, 0, 12, 11, 6)
#define SYS_ICC_SGI0R_EL1  sys_reg(3, 0, 12, 11, 7)
#define SYS_ICC_DIR_EL1    sys_reg(3, 0, 12, 11, 1)

#define ICC_SGI1R_TARGETLIST GENMASK(15, 0)
#define ICC_SGI1R_AFF1       GENMASK(23, 16)
#define ICC_SGI1R_INTID      GENMASK(27, 24)
#define ICC_SGI1R_AFF2       GENMASK(39, 32)
#define ICC_SGI1R_IRM        BIT(40)
#define ICC_SGI1R_RS         GENMASK(47, 44)
#define ICC_SGI1R_AFF3       GENMASK(55, 48)

/* Distributor (GICD), 64 KiB frame */
#define GICD_CTLR       0x0000
#define GICD_TYPER      0x0004
#define GICD_IIDR       0x0008
#define GICD_TYPER2     0x000c
#define GICD_STATUSR    0x0010
#define GICD_IGROUPR    0x0080
#define GICD_ISENABLER  0x0100
#define GICD_ICENABLER  0x0180
#define GICD_ISPENDR    0x0200
#define GICD_ICPENDR    0x0280
#define GICD_ISACTIVER  0x0300
#define GICD_ICACTIVER  0x0380
#define GICD_IPRIORITYR 0x0400
#define GICD_ITARGETSR  0x0800
#define GICD_ICFGR      0x0c00
#define GICD_IGRPMODR   0x0d00
#define GICD_NSACR      0x0e00
#define GICD_IROUTER    0x6000 /* IROUTER<n> at 0x6000 + 8 * n, n = 32..1019 */
#define GICD_PIDR2      0xffe8
#define GICD_SIZE       0x10000

/* One 32-bit register per 32 INTIDs (ISENABLER etc.), one byte per INTID (IPRIORITYR) */
#define GICD_IROUTER_END 0x7fe0 /* past IROUTER<1019> */
#define GICD_BITREG_SIZE 0x80
#define GICD_IPRIO_SIZE  0x400
#define GICD_ICFGR_SIZE  0x100

#define GICD_CTLR_ENABLEGRP0 BIT(0)
#define GICD_CTLR_ENABLEGRP1 BIT(1)
#define GICD_CTLR_ARE        BIT(4)
#define GICD_CTLR_DS         BIT(6)
#define GICD_CTLR_RWP        BIT(31)

#define GICD_TYPER_ITLINES GENMASK(4, 0)
#define GICD_TYPER_IDBITS  GENMASK(23, 19)
#define GICD_TYPER_A3V     BIT(24)
#define GICD_TYPER_No1N    BIT(25)

#define GICD_IIDR_IMPLEMENTER GENMASK(11, 0)
#define GICD_IIDR_PRODUCTID   GENMASK(31, 24)

#define GICD_IROUTER_AFF GENMASK(23, 0) /* Aff2.Aff1.Aff0; IRM (bit 31) and Aff3 are RAZ/WI */

/* Redistributor (GICR): RD_base frame + SGI_base frame per CPU */
#define GICR_CTLR                 0x0000
#define GICR_IIDR                 0x0004
#define GICR_TYPER                0x0008
#define GICR_STATUSR              0x0010
#define GICR_WAKER                0x0014
#define GICR_PROPBASER            0x0070
#define GICR_PENDBASER            0x0078
#define GICR_SYNCR                0x00c0
#define GICR_PIDR2                0xffe8
#define GICR_SGI_BASE             0x10000
#define GICR_STRIDE               0x20000
#define GICR_TYPER_LAST           BIT(4)
#define GICR_TYPER_PROCNUM        GENMASK(23, 8)
#define GICR_TYPER_AFFINITY       GENMASK(63, 32)
#define GICR_WAKER_PROCESSORSLEEP BIT(1)
#define GICR_WAKER_CHILDRENASLEEP BIT(2)

/* SGI_base frame: the GICD offsets, for INTIDs 0..31 */
#define GICR_IGROUPR0    GICD_IGROUPR
#define GICR_ISENABLER0  GICD_ISENABLER
#define GICR_ICENABLER0  GICD_ICENABLER
#define GICR_ISPENDR0    GICD_ISPENDR
#define GICR_ICPENDR0    GICD_ICPENDR
#define GICR_ISACTIVER0  GICD_ISACTIVER
#define GICR_ICACTIVER0  GICD_ICACTIVER
#define GICR_IPRIORITYR0 GICD_IPRIORITYR
#define GICR_ICFGR0      GICD_ICFGR
#define GICR_ICFGR1      (GICD_ICFGR + 4)

#define GIC_PIDR2_ARCH_GICV3 0x30
#define GIC_PIDR2_ARCH       GENMASK(7, 4)

/* Interrupt ID space */
#define GIC_NR_SGI         16
#define GIC_NR_PRIVATE     32
#define GIC_MAX_SPI        1019 /* INTIDs 1020..1023 are special */
#define GIC_SPURIOUS       1023
#define GIC_PPI_CNTP       30 /* EL1 physical timer, architectural default */
#define GIC_PPI_CNTV       27 /* EL1 virtual timer, architectural default */
#define GIC_PPI_VGIC_MAINT 25

#endif
