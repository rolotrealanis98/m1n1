/* Host shim: no IOVA allocator on ANS; any call is a test failure. */
#ifndef IOVA_H
#define IOVA_H
#include "types.h"
typedef struct iova_domain iova_domain_t;
u64 iova_alloc(iova_domain_t *iovad, size_t sz);
void iova_free(iova_domain_t *iovad, u64 iova, size_t sz);
#endif
