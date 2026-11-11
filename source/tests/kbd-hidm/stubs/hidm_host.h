#include <stdio.h>
#define MAX_CPUS 24
struct exc_info { int unused; };
extern int boot_cpu_idx;
int smp_id(void);
bool hv_win_vgic_enabled(void);
u64 hv_win_pl011_gsiv(void);
bool hv_win_storage_claims_spi(u32);
bool hv_vgic3_spi_unassigned(u32);
void hv_vgic3_set_spi_level(u32, bool);
u64 hv_pt_walk(u64);
int hv_map_hook(u64, bool (*)(struct exc_info *, u64, u64 *, bool, int), u64);
