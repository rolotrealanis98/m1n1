/* SPDX-License-Identifier: MIT */
/* Read-only N1/apcie0 port 0 probe. Every read is cited in WRITES.md section "probe reads".
 * No register write, no SMC write. */
#include "n1_probe.h"

#define NOT_READ (~(n1_u64)0)
#define PS_ACTUAL(v) (((v) >> 4) & 0xf)

static int ps_on(n1_u64 v) { return v != NOT_READ && v != 0 && PS_ACTUAL(v) == 0xf; }

int n1_probe(const struct n1_hal *h, const struct n1_addrs *a, unsigned flags, n1_u64 out[N1P_COUNT])
{
    unsigned i;
    n1_u64 verdict = 0;
    for (i = 0; i < N1P_COUNT; i++) out[i] = NOT_READ;
    out[N1P_VERSION] = 1;
    out[N1P_FLAGS] = flags;
    /* the PERST# pad must be pin 80 of gpio0; pin 112 (+0x1c0) takes an SError when read */
    if (!a->rc || !a->port[0] || !a->ecam || a->perst_pin != 80 || a->perst != a->gpio_base + 4 * 80)
        return -1;

    /* 1. RC and port status: n1_port_up() order (kboot_n1.c:216-219) */
    out[N1P_RC_CTL] = N1_RD(h, a->rc + 0x50);
    out[N1P_RC_STAT] = N1_RD(h, a->rc + 0x58);
    out[N1P_P0_STATUS] = N1_RD(h, a->port[0] + 0x804);
    out[N1P_P0_LINKSTS] = N1_RD(h, a->port[0] + 0x208);
    out[N1P_P0_LTSSM] = N1_RD(h, a->port[0] + 0x080);
    for (i = 1; i < 4; i++)
        if (a->port[i]) out[N1P_P1_LINKSTS + (i - 1)] = N1_RD(h, a->port[i] + 0x208); /* stage S */
    /* 2. power states (PMGR, read only) */
    for (i = 0; i < N1_PS_COUNT; i++) out[N1P_PS_FIRST + i] = h->ps_read(h->ctx, (enum n1_ps)i);
    if (ps_on(out[N1P_PS_FIRST + N1_PS_APCIE_SYS_GP]) && ps_on(out[N1P_PS_FIRST + N1_PS_APCIE_PHY_SW]))
        verdict |= N1V_GP_POWER;
    if ((out[N1P_RC_CTL] & 1) && (out[N1P_RC_STAT] & 1)) verdict |= N1V_RC_READY;
    /* 3. PERST# pad (stage A entry check) */
    out[N1P_PERST_PAD] = N1_RD(h, a->perst);
    if ((out[N1P_PERST_PAD] & 0x27f) == 0x202) verdict |= N1V_PERST_LOW;
    /* 4. port 0 clock/reset words and shared PHY status: only with GP power up (stage B/S reads) */
    if (verdict & N1V_GP_POWER) {
        out[N1P_P0_APPCLK] = N1_RD(h, a->port[0] + 0x800);
        out[N1P_P0_INT_RESET] = N1_RD(h, a->port[0] + 0x82c);
        out[N1P_P0_CLKRST] = N1_RD(h, a->port[0] + 0x0a8);
        out[N1P_P0_MSICFG] = N1_RD(h, a->port[0] + 0x124);
    }
    if ((verdict & N1V_GP_POWER) || (verdict & N1V_RC_READY)) {
        out[N1P_PHY_CTL_STATUS] = N1_RD(h, a->phy_ctl + 0x08);
        out[N1P_PHY_REFCLK] = N1_RD(h, a->phy + 0x4000);
    }
    /* 5. live link only: root port, endpoint, RID2SID, DART (stages F and G, no bus-number write) */
    if ((verdict & N1V_RC_READY) && (out[N1P_P0_STATUS] & 1) && (out[N1P_P0_LINKSTS] & 5) == 1 &&
        (out[N1P_P0_LTSSM] & 1)) {
        n1_u64 rp = a->ecam; /* port 0 root port = ECAM + (0 << 15) */
        n1_u32 buses, valid = 0;
        verdict |= N1V_LINK_UP;
        out[N1P_RP_ID] = N1_RD(h, rp);
        out[N1P_RP_BUSES] = buses = N1_RD(h, rp + 0x18);
        if (((buses >> 8) & 0xff) == 1) {
            out[N1P_EP_ID] = N1_RD(h, a->ecam + (1ull << 20));
            out[N1P_EP_COMMAND] = N1_RD(h, a->ecam + (1ull << 20) + 0x04);
            if (out[N1P_EP_ID] == 0x1900106b) verdict |= N1V_EP_ROM;
            if (out[N1P_EP_ID] == 0x1901106b) verdict |= N1V_EP_RUNNING;
        }
        for (i = 0; i < 64; i++)
            if (N1_RD(h, a->port[0] + 0x3000 + 4 * i) & (1u << 31)) valid++;
        out[N1P_RID2SID_VALID] = valid;
        if (a->dart) {
            out[N1P_DART_PROTECT] = N1_RD(h, a->dart + 0x200);
            out[N1P_DART_208] = N1_RD(h, a->dart + 0x208);
            out[N1P_DART_STREAMS] = N1_RD(h, a->dart + 0xc00);
            if (!out[N1P_DART_PROTECT] && !out[N1P_DART_208] && !out[N1P_DART_STREAMS]) verdict |= N1V_DART_CLEAN;
        }
    }
    /* 6. storage words, read only (storage_check: ANS ctrl/status, BOOT_STATUS, CC, CSTS, A2I, I2A) */
    for (i = 0; i < 7; i++)
        if (a->storage[i]) out[N1P_ST_FIRST + i] = N1_RD(h, a->storage[i]);
    if (out[N1P_ST_FIRST] != NOT_READ && (out[N1P_ST_FIRST] & 0x10) && out[N1P_ST_FIRST + 2] == 0xde71ce55)
        verdict |= N1V_STORAGE_OK;
    /* 7. SMC reset key (mailbox: INITIALIZE + READ_KEY; m1n1 n1_smc_release does the same read) */
    if ((flags & N1_PROBE_SMC) && a->reset_key && h->smc_read) {
        n1_u32 v = 0;
        int rc = h->smc_read(h->ctx, a->reset_key, &v);
        out[N1P_SMC_KEY] = a->reset_key;
        out[N1P_SMC_RC] = (n1_u64)(n1_u32)rc;
        if (!rc) {
            out[N1P_SMC_VALUE] = v;
            if (v == (a->reset_template | 1)) verdict |= N1V_SMC_RELEASED;
        }
    }
    out[N1P_VERDICT] = verdict;
    return 0;
}
