/* SPDX-License-Identifier: MIT */
/* host stand-in for src/utils.h (tests/heapblock-pool-n5c): printf, panic -> longjmp */
#ifndef UTILS_H
#define UTILS_H
#include <stdio.h>
#include "types.h"
void host_panic(const char *fmt, ...);
#define panic host_panic
#endif
