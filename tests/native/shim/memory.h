/* Host shim: m1n1 cache maintenance (recorded by the fake ANS). */
#ifndef MEMORY_H
#define MEMORY_H
#include "types.h"
void dc_ivac_range(void *addr, size_t length);
void dc_cvac_range(void *addr, size_t length);
void dc_civac_range(void *addr, size_t length);
#endif
