/* SPDX-License-Identifier: MIT */
#ifndef XNUBOOT_H
#define XNUBOOT_H
#include "types.h"
struct boot_args { u64 phys_base, top_of_kernel_data; };
extern struct boot_args cur_boot_args;
#endif
