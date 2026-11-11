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
u32 s1n_test_smp_timeout_us; /* nightly-7: host tests set the SMP data-command bound (0 = cap 1) */
#endif
/* r4.8: the storage lease's guest-RAM containment query (hv_win_storage.c; host tests provide a stub). */
bool hv_win_storage_guest_ram_contains(u64 addr, u64 size);
bool hv_win_storage_smp_context(void); /* smp-storage */
#ifndef S1N_TEST_SMP_CONTEXT
#define S1N_TEST_SMP_CONTEXT() false /* host tests: the stress harness defines it */
#endif

#ifndef ANS_RO_HOST_TEST
u32 hv_win_storage_smp_cmd_timeout_us(void); /* nightly-7: hv_win_storage.c */
#endif
/* nightly-7: ans_ro.c's data-command wait bound while more than one vCPU is admitted (0 = none) */
u32 s1n_smp_cmd_timeout_us(void)
{
#ifdef ANS_RO_HOST_TEST
    return s1n_test_smp_timeout_us;
#else
    return hv_win_storage_smp_cmd_timeout_us();
#endif
}

static bool locked, prep_used, start_used;
static u64 refusals;
static u64 exc_ctx_addr, exc_ctx_size;
bool s1n_storage_context(void)
{
/* smp-storage: or any CPU holding the storage lock when more than one vCPU is admitted (hv_win_storage.c) */
#ifdef ANS_RO_HOST_TEST
    return (s1n_test_cpu == 0 || S1N_TEST_SMP_CONTEXT()) && !s1n_test_exception;
#else
    return (smp_id() == boot_cpu_idx || hv_win_storage_smp_context()) &&
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

void s1n_link_exc_ctx(u64 addr, u64 size)
{
    exc_ctx_addr = addr;
    exc_ctx_size = size;
}

static bool within(u64 base, u64 len, u64 addr, u64 size)
{
    /* [addr, addr+size) entirely inside [base, base+len); all precomputed, no overflow (size>0). */
    return len && addr >= base && size <= len && addr - base <= len - size;
}

bool s1n_link_mem_ok(u64 addr, u64 size)
{
    if (!locked || size == 0)
        return true;
    if (addr > UINT64_MAX - size) { refusals++; return false; }
    /* Admitted ONLY during a guest-exception session (exc_ctx_size != 0 is the session bracket), and
     * only for the context block the resident passed for THIS exception, or guest RAM registered by the
     * active storage lease. Outside an exception session, and for controller MMIO / the resident / the
     * image in any case, access stays refused. */
    if (exc_ctx_size) {
        if (within(exc_ctx_addr, exc_ctx_size, addr, size))
            return true;
        if (hv_win_storage_guest_ram_contains(addr, size))
            return true;
    }
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
    /* nightly-7: cable-independent session end. Only in a host-interrupt or exception session (the guest is
     * running); they end the guest through the SYSTEM_OFF path and never reach storage state themselves. */
    case P_HV_WIN_SESSION_END:
    case P_HV_WIN_HEARTBEAT:
    case P_HV_WIN_SESSION_STAT:
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
