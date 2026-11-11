#ifndef TEST_DART_H
#define TEST_DART_H
#include "types.h"
typedef struct dart_dev dart_dev_t;
int dart_map(dart_dev_t *,u64,void *,size_t);
void dart_unmap(dart_dev_t *,u64,size_t);
void *dart_translate(dart_dev_t *,u64);
#endif
