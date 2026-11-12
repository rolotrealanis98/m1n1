/* SPDX-License-Identifier: MIT */
/* tests/win-n1pt: stand-in for src/aic.h (the fields hv_win_n1pt.c uses; AIC registers live in the model) */
#ifndef N1PT_GLUE_AIC_H
#define N1PT_GLUE_AIC_H
#include "n1pt_host.h"
struct aic_regs { u64 config; };
struct aic { u64 base; u32 max_irq; struct aic_regs regs; };
extern struct aic *aic;
void aic_mask(u32 irq);
void aic_unmask(u32 irq);
#endif
