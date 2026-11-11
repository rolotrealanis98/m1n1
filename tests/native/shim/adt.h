/* Host shim: the subset of m1n1's ADT API used by asc/sart/rtkit/nvme, backed by a fake tree. */
#ifndef XDT_H
#define XDT_H
#include "types.h"
extern void *adt;
struct adt_property { char name[32]; u32 size; u8 value[]; };
int adt_path_offset_trace(const void *adt, const char *path, int *offsets);
const struct adt_property *adt_get_property(const void *adt, int nodeoffset, const char *name);
const void *adt_getprop(const void *adt, int nodeoffset, const char *name, u32 *lenp);
int adt_getprop_copy(const void *adt, int nodeoffset, const char *name, void *out, size_t len);
int adt_get_reg(const void *adt, int *path, const char *prop, int idx, u64 *addr, u64 *size);
bool adt_is_compatible(const void *adt, int nodeoffset, const char *compat);
int adt_first_child_offset(const void *adt, int offset);
#define ADT_GETPROP(adt, nodeoffset, name, val) adt_getprop_copy(adt, nodeoffset, name, (val), sizeof(*(val)))
#endif
