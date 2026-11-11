#ifndef TEST_IOVA_H
#define TEST_IOVA_H
#include "types.h"
typedef struct iova_domain iova_domain_t;
u64 iova_alloc(iova_domain_t *,size_t);
void iova_free(iova_domain_t *,u64,size_t);
#endif
