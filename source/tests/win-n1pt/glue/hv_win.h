/* SPDX-License-Identifier: MIT */
/* tests/win-n1pt: stand-in for src/hv/win/hv_win.h and the resident services hv_win_n1pt.c uses (glue.c) */
#ifndef N1PT_GLUE_HV_WIN_H
#define N1PT_GLUE_HV_WIN_H
#include "n1pt_host.h"
#define MAX_CPUS 24
#define sysop(op) ((void)0)
typedef struct { int x; } spinlock_t;
#define DECLARE_SPINLOCK(n) spinlock_t n = {0}
void spin_lock(spinlock_t *l);
void spin_unlock(spinlock_t *l);
u64 get_ticks(void);
u64 ticks_to_usecs(u64 t);
u64 top_of_memory_alloc(size_t size);
bool hv_win_aic_claim_t6050(void);
bool hv_win_disk_path_used(void);
void hv_vgic3_set_spi_level(u32 intid, bool level);
#endif
