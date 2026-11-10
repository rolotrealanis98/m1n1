/* SPDX-License-Identifier: MIT */
/* Host stand-in for m1n1's src/exception.h (tests/win-vgic3 and tests/win-pmu-rewrite) */

#ifndef EXCEPTION_H
#define EXCEPTION_H

#include "types.h"

struct exc_info {
    u64 regs[32];
    u64 spsr;
    u64 elr;
    u64 esr;
    u64 far;
};

#endif
