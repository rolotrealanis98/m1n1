/* Host shim: no DART on ANS (SART is used); any call is a test failure. */
#ifndef DART_H
#define DART_H
#include "types.h"
typedef struct dart_dev dart_dev_t;
int dart_map(dart_dev_t *dart, u64 iova, void *bfr, size_t len);
void dart_unmap(dart_dev_t *dart, u64 iova, size_t len);
void *dart_translate(dart_dev_t *dart, u64 iova);
#endif
