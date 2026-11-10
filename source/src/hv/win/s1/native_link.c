/* SPDX-License-Identifier: MIT */
#include "native_link.h"
#include "s1_stage.h"
#include "proxy.h"
#ifndef ANS_RO_HOST_TEST
#include "smp.h"
#include "uartproxy.h"
#else
int s1n_test_cpu;
bool s1n_test_exception;
#endif

static bool locked, prep_used, start_used;
static u64 refusals;
bool s1n_storage_context(void)
{
#ifdef ANS_RO_HOST_TEST
    return s1n_test_cpu == 0 && !s1n_test_exception;
#else
    return smp_id() == boot_cpu_idx &&
           !__atomic_load_n(&uartproxy_storage_exceptions, __ATOMIC_ACQUIRE);
#endif
}
bool s1n_canary_context(void) { return s1n_storage_context() && !start_used; }
void s1n_link_lock(void) { locked = true; }
bool s1n_link_locked(void) { return locked; }
u64 s1n_link_refusals(void) { return refusals; }
bool s1n_link_raw(void)
{
    if (!locked) return true;
    refusals++;
    return false;
}
bool s1n_link_proxy(u64 opcode, bool guest_exception)
{
    switch (opcode) {
    case P_HV_START:
    case P_HV_WIN_STORAGE_NATIVE:
    case P_HV_WIN_STORAGE_NATIVE_QUIESCE:
    case P_HV_WIN_STORAGE_NATIVE_CANARY:
    case P_HV_WIN_STORAGE_NATIVE_STAT:
    case P_HV_WIN_STORAGE_STAT:
    case P_HV_WIN_STORAGE_PREP:
        if (guest_exception || !s1n_storage_context()) { refusals++; return false; }
        break;
    default: break;
    }
    if (!locked) return true;
    switch (opcode) {
    case P_NOP:
    case P_REBOOT:
    case P_HV_WIN_STORAGE_NATIVE_STAT:
    case P_HV_WIN_STORAGE_STAT:
    case P_HV_WIN_STATUS:
    case P_HV_WIN_FB_STAT:
    case P_HV_WIN_PL011_STATS:
    case P_HV_WIN_STORAGE_NATIVE_QUIESCE:
        return true;
    case P_HV_WIN_STORAGE_PREP:
        if (!prep_used) { prep_used = true; return true; }
        break;
    case P_HV_START:
        if (!start_used) { start_used = true; return true; }
        break;
#if S1N_STAGE_CANARY
    case P_HV_WIN_STORAGE_NATIVE_CANARY:
        if (!guest_exception && s1n_canary_context()) return true;
        break;
#endif
    case P_EXIT:
        if (guest_exception) return true;
        break;
    default:
        break;
    }
    refusals++;
    return false;
}
bool s1n_link_discard(u64 size, bool sentinel, ssize_t (*read_bytes)(void *, size_t))
{
    u8 discard[256];
    /* Separate loops avoid overflow when the requested size is UINT64_MAX. */
    for (unsigned part = 0; part < 2; part++) {
        u64 left = part ? (sentinel ? 4 : 0) : size;
        while (left) {
            size_t n = left < sizeof(discard) ? (size_t)left : sizeof(discard);
            ssize_t got = read_bytes(discard, n);
            if (got <= 0 || (size_t)got > n) return false;
            left -= (size_t)got;
        }
    }
    return true;
}
