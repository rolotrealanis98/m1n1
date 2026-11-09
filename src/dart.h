/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef DART_H
#define DART_H

#include "types.h"

#define DART_PTR_ERR     BIT(63)
#define DART_IS_ERR(val) FIELD_GET(DART_PTR_ERR, val)

typedef struct dart_dev dart_dev_t;

enum dart_type_t {
    DART_T8020,
    DART_T8110,
    DART_T6000,
};

dart_dev_t *dart_init(uintptr_t base, u8 device, bool keep_pts, enum dart_type_t type);
dart_dev_t *dart_init_adt(const char *path, int instance, int device, bool keep_pts);
void dart_lock_adt(const char *path, int instance);
dart_dev_t *dart_init_fdt(void *dt, u32 phandle, int device, bool keep_pts);
int dart_setup_pt_region(dart_dev_t *dart, const char *path, int device, u64 vm_base);
int dart_map(dart_dev_t *dart, uintptr_t iova, void *bfr, size_t len);
int dart_map_flags(dart_dev_t *dart, uintptr_t iova, void *bfr, size_t len, u32 flags);
void dart_unmap(dart_dev_t *dart, uintptr_t iova, size_t len);
void dart_free_l2(dart_dev_t *dart, uintptr_t iova);
void *dart_translate(dart_dev_t *dart, uintptr_t iova);
void *dart_translate_silent(dart_dev_t *dart, uintptr_t iova);
u64 dart_search(dart_dev_t *dart, void *paddr);
u64 dart_find_iova(dart_dev_t *dart, s64 start, size_t len);
void dart_shutdown(dart_dev_t *dart);
u64 dart_vm_base(dart_dev_t *dart);

/*
 * Whole-instance helpers for handing a T8110-type DART's master to a guest that knows nothing
 * about DARTs (m5pro-windows docs/USB-HOST.md): every stream of the instance either bypasses
 * translation or walks a four-level identity table (DVA == PA, dart_identity.h) that covers only
 * the given RAM ranges. The tables live in m1n1's heap, outside every range, so the master can
 * neither reach them nor anything else that is not listed.
 */
#include "dart_identity.h"

/* T8110 register-level view of one instance (one "reg" entry of a dart,t8110 node). */
u32 dart_t8110_num_sids(uintptr_t base);
u32 dart_t8110_va_width(uintptr_t base);
bool dart_t8110_locked(uintptr_t base);
/* Every stream to bypass; read back, restored and -1 when the hardware refuses the bits. */
int dart_t8110_attach_bypass(uintptr_t base);
/* Every stream to the identity tables (TCR TRANSLATE_ENABLE | FOUR_LEVEL); -1 on refusal. */
int dart_t8110_attach_identity(uintptr_t base, const struct dart_identity *id);
/* Every stream disabled from translation and bypass (TCR = 0, TTBR = 0): blocks the master. */
void dart_t8110_block(uintptr_t base);
void dart_t8110_lock(uintptr_t base);
u32 dart_t8110_error(uintptr_t base);

#endif
