/* Host shim: every allocation of the frozen code and the candidate comes from the fixed private arena. */
#ifndef MALLOC_H
#define MALLOC_H
#include "types.h"
void *arena_memalign(size_t align, size_t sz);
void *arena_calloc(size_t n, size_t sz);
void *arena_malloc(size_t sz);
void arena_free(void *p);
#define memalign(a, s) arena_memalign(a, s)
#define calloc(n, s)   arena_calloc(n, s)
#define malloc(s)      arena_malloc(s)
#define free(p)        arena_free(p)
#endif
