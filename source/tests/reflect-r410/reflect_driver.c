/* SPDX-License-Identifier: MIT */
/* r4.10: stdin "elr spsr esr far vbar sctlr" (hex) -> the firmware's hv_reflect_compute() result. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define TYPES_H
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#include "hv/hv_sysreg_logic.h"
int main(void)
{
    unsigned long long elr, spsr, esr, far, vbar, sctlr;
    while (scanf("%llx %llx %llx %llx %llx %llx", &elr, &spsr, &esr, &far, &vbar, &sctlr) == 6) {
        struct hv_reflect r;
        if (!hv_reflect_compute(elr, spsr, esr, far, vbar, sctlr, 0, &r)) {
            printf("refused\n");
            continue;
        }
        printf("%llx %llx %llx %llx %llx %llx %x\n", (unsigned long long)r.elr_el1,
               (unsigned long long)r.spsr_el1, (unsigned long long)r.esr_el1,
               (unsigned long long)r.far_el1, (unsigned long long)r.new_elr,
               (unsigned long long)r.new_spsr, r.offset);
    }
    return 0;
}
