/* SPDX-License-Identifier: MIT */
/* Host stand-in for src/utils.h (tests/storage-pmu-admit-n5): the big HV lock's type */
#ifndef UTILS_H
#define UTILS_H
#include "types.h"
typedef struct {
    s64 lock;
    int count;
} spinlock_t;
#endif
