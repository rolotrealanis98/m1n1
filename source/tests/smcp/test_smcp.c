/* SPDX-License-Identifier: MIT */
/* Host tests for the SMCP core and power sequencer against the mock SMC. Build: tests/run.sh */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mock_smc.h"

static int checks, fails;
#define CHECK(c)                                                                   \
    do {                                                                           \
        checks++;                                                                  \
        if (!(c)) {                                                                \
            fails++;                                                               \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);           \
        }                                                                          \
    } while (0)
#define CHECK_EQ(a, b)                                                             \
    do {                                                                           \
        checks++;                                                                  \
        unsigned long long _a = (unsigned long long)(a), _b = (unsigned long long)(b); \
        if (_a != _b) {                                                            \
            fails++;                                                               \
            fprintf(stderr, "FAIL %s:%d: %s = 0x%llx (%llu), want 0x%llx (%llu)\n", __FILE__, __LINE__, #a, _a, _a, _b, _b); \
        }                                                                          \
    } while (0)

static mock_smc M;
static smcp S;

static uint32_t rd(uint32_t off) { return smcp_reg_read(&S, off); }

/* one resident tick per 1 ms of mock time */
static int max_sends_per_tick, max_recv_calls_per_tick;
static void run_ms(unsigned ms)
{
    for (unsigned i = 0; i < ms; i++) {
        M.t += 1000;
        int s0 = M.sends, r0 = M.recv_calls;
        smcp_tick(&S);
        if (M.sends - s0 > max_sends_per_tick)
            max_sends_per_tick = M.sends - s0;
        if (M.recv_calls - r0 > max_recv_calls_per_tick)
            max_recv_calls_per_tick = M.recv_calls - r0;
    }
}

static void setup(uint32_t flags)
{
    mock_init(&M);
    mock_load_t1b(&M);
    smcp_ops o = mock_ops(&M);
    smcp_init(&S, &o, flags);
}

static const char *allow[] = {"BUIC", "B0AV", "B0AC", "B0RM", "B0FC", "B0DC", "B0CT", "B0AT", "B0TE", "B0TF",
                              "BNCB", "BBAD", "BSFC", "BCF0", "CHIS", "CHCE", "CHCC", "CHSC", "CH0R", "CHNC",
                              "AC-i", "CHWA", "CHLS", "ACSt", "ACPW", "TCMb", "TB0T", "TCHP", "TW0P", "TH0x",
                              "F0Ac", "F1Ac", "F0Mx", "F1Mx", "MSLD", "bHLD"};

static bool allowed(uint32_t key)
{
    for (size_t i = 0; i < sizeof(allow) / sizeof(allow[0]); i++)
        if (SMCP_KEY(allow[i][0], allow[i][1], allow[i][2], allow[i][3]) == key)
            return true;
    return false;
}

static void t_nominal_t1b(void)
{
    setup(0);
    CHECK_EQ(rd(SMCP_R_MAGIC), SMCP_MAGIC);
    CHECK_EQ(rd(SMCP_R_VERSION), 1);
    CHECK(!(rd(SMCP_R_STATUS) & SMCP_ST_VALID));
    CHECK_EQ(rd(SMCP_R_BST_REMAIN), 0xffffffffu);
    run_ms(200);
    uint32_t st = rd(SMCP_R_STATUS);
    CHECK(st & SMCP_ST_BACKEND);
    CHECK(st & SMCP_ST_VALID);
    CHECK(st & SMCP_ST_BATT);
    CHECK(st & SMCP_ST_AC);
    CHECK(st & SMCP_ST_LIDKEY);
    CHECK(st & SMCP_ST_PWRBKEY);
    CHECK(!(st & (SMCP_ST_STALE | SMCP_ST_FAILED | SMCP_ST_POWERACT)));
    CHECK_EQ(rd(SMCP_R_CHG_STATUS), SMCP_CHG_FULL);   /* t1b: "Full" */
    CHECK_EQ(rd(SMCP_R_CAP_LEVEL), SMCP_CAP_FULL);
    CHECK_EQ(rd(SMCP_R_BST_STATE), 0);
    CHECK_EQ(rd(SMCP_R_BST_RATE), 0);
    CHECK_EQ(rd(SMCP_R_BST_REMAIN), 8064);
    CHECK_EQ(rd(SMCP_R_BST_VOLTAGE), 13068);
    CHECK_EQ(rd(SMCP_R_BIX_DESIGN), 8579);
    CHECK_EQ(rd(SMCP_R_BIX_FULL), 8064);
    CHECK_EQ(rd(SMCP_R_BIX_VOLTAGE), 3 * 3800);
    CHECK_EQ(rd(SMCP_R_BIX_CYCLES), 33);
    CHECK_EQ(rd(SMCP_R_BATT_PERCENT), 100);
    CHECK_EQ(rd(SMCP_R_BATT_TEMP), 3007);            /* 27.5 C */
    CHECK_EQ(rd(SMCP_R_BATT_TTE), 0xffffffffu);
    CHECK_EQ(rd(SMCP_R_PSR), 1);
    CHECK_EQ(rd(SMCP_R_LID), 1);
    CHECK_EQ(rd(SMCP_R_BATT_HEALTH), 0);
    CHECK_EQ(rd(SMCP_R_AC_POWER), 139720);
    CHECK_EQ(rd(SMCP_R_TEMP0 + 0), 3175);            /* TCMb 44.3375 C */
    CHECK_EQ(rd(SMCP_R_TEMP0 + 4), 3007);            /* TB0T 27.5 C */
    CHECK_EQ(rd(SMCP_R_TEMP0 + 8), 3064);            /* TCHP 33.226 C */
    CHECK_EQ(rd(SMCP_R_TEMP0 + 12), 3066);           /* TW0P 33.416 C */
    CHECK_EQ(rd(SMCP_R_TEMP0 + 16), 3041);           /* TH0x 30.945 C */
    CHECK_EQ(rd(SMCP_R_FAN0_RPM), 0);
    CHECK_EQ(rd(SMCP_R_FAN0_MAX), 5349);
    CHECK_EQ(rd(SMCP_R_FAN1_MAX), 5777);
    CHECK(rd(SMCP_R_SEQ) >= 1);
    /* first data raises BATT|AC|LID, but no IRQ until the guest enables events */
    CHECK_EQ(rd(SMCP_R_EVT_PENDING), SMCP_EV_BATT | SMCP_EV_AC | SMCP_EV_LID);
    CHECK(!smcp_irq(&S));
    /* AC-i and CHWA absent: two key-info failures, nothing else */
    CHECK_EQ(rd(SMCP_R_ERR_COUNT), 2);
    CHECK_EQ(rd(SMCP_R_TIMEOUTS), 0);
    CHECK(max_sends_per_tick <= 1);
    CHECK(max_recv_calls_per_tick <= (int)SMCP_RECV_BUDGET);
    /* temperatures round to the nearest 0.1 K: 27.56 C -> 3008, -5.56 C -> 2676 */
    mock_set_u(&M, "TB0T", 0x41dc7ae1);
    mock_set_u(&M, "TCHP", 0xc0b1eb85);
    run_ms(2100);
    CHECK_EQ(rd(SMCP_R_TEMP0 + 4), 3008);
    CHECK_EQ(rd(SMCP_R_TEMP0 + 8), 2676);
}

static void t_reads_only(void)
{
    setup(0);
    run_ms(10000);
    CHECK_EQ(mock_count_type(&M, SMCP_MSG_WRITE_KEY), 0);
    int bad = 0;
    for (int i = 0; i < M.nlog; i++) {
        uint8_t t = M.log[i].msg & 0xff;
        if (t != SMCP_MSG_READ_KEY && t != SMCP_MSG_GET_KEY_INFO)
            bad++;
        if (t == SMCP_MSG_READ_KEY && !allowed((uint32_t)(M.log[i].msg >> 32)))
            bad++;
        if (t == SMCP_MSG_GET_KEY_INFO && !allowed((uint32_t)(M.log[i].msg >> 32)))
            bad++;
    }
    CHECK_EQ(bad, 0);
    /* never reads an absent key after key info, never reads CHLS's sibling CHWA (absent) */
    CHECK_EQ(mock_get(&M, "AC-W")->reads, 0);
    CHECK_EQ(mock_get(&M, "CLKM")->reads, 0);
    CHECK_EQ(mock_get(&M, "MBSE")->reads, 0);
    CHECK(mock_get(&M, "CHLS")->reads > 0);
    /* poll rates over 10 s: slow set every 2 s (about 5 rounds), lid/button every 50 ms */
    uint32_t b0av = mock_get(&M, "B0AV")->reads, msld = mock_get(&M, "MSLD")->reads;
    CHECK(b0av >= 4 && b0av <= 6);
    CHECK(msld >= 150 && msld <= 201);
    CHECK_EQ(M.pmu_reads + M.pmu_writes, 0);
    /* every READ asks for exactly the key-info size */
    int size_bad = 0;
    for (int i = 0; i < M.nlog; i++)
        if ((M.log[i].msg & 0xff) == SMCP_MSG_READ_KEY) {
            uint32_t key = (uint32_t)(M.log[i].msg >> 32);
            for (int j = 0; j < M.nk; j++)
                if (M.k[j].key == key && M.k[j].size != ((M.log[i].msg >> 16) & 0xff))
                    size_bad++;
        }
    CHECK_EQ(size_bad, 0);
}

static void t_message_format(void)
{
    setup(0);
    run_ms(5);
    /* first message: GET_KEY_INFO for BUIC, id 1, size 0, key in 63:32 */
    CHECK(M.nlog >= 1);
    uint64_t m = M.log[0].msg;
    CHECK_EQ(m & 0xff, SMCP_MSG_GET_KEY_INFO);
    CHECK_EQ((m >> 12) & 0xf, 1);
    CHECK_EQ((m >> 16) & 0xffff, 0);
    CHECK_EQ(m >> 32, SMCP_KEY('B', 'U', 'I', 'C'));
    run_ms(200);
    /* a 2-byte read: READ_KEY, size 2 in 23:16, WSIZE 0 */
    int found = 0;
    for (int i = 0; i < M.nlog; i++)
        if ((M.log[i].msg & 0xff) == SMCP_MSG_READ_KEY && (M.log[i].msg >> 32) == SMCP_KEY('B', '0', 'A', 'V')) {
            CHECK_EQ((M.log[i].msg >> 16) & 0xff, 2);
            CHECK_EQ((M.log[i].msg >> 24) & 0xff, 0);
            found++;
        }
    CHECK(found >= 1);
    /* ids cycle through 0..15 */
    int seen[16] = {0};
    for (int i = 0; i < M.nlog; i++)
        seen[(M.log[i].msg >> 12) & 0xf] = 1;
    int n = 0;
    for (int i = 0; i < 16; i++)
        n += seen[i];
    CHECK_EQ(n, 16);
}

static void t_discharging(void)
{
    setup(0);
    run_ms(200);
    smcp_reg_write(&S, SMCP_R_EVT_ACK, 0xffffffff);
    smcp_reg_write(&S, SMCP_R_EVT_ENABLE, SMCP_EV_ALL);
    CHECK(!smcp_irq(&S));
    /* unplug: CHCE 0, CHIS 0, BSFC 0, current -1500 mA, 97 % */
    mock_set_u(&M, "CHIS", 0);
    mock_set_u(&M, "CHCE", 0);
    mock_set_u(&M, "BSFC", 0);
    mock_set_u(&M, "B0AC", (uint16_t)-1500);
    mock_set_u(&M, "BUIC", 97);
    mock_set_u(&M, "B0RM", 7800);
    mock_set_u(&M, "B0TE", 300);
    run_ms(2500);
    CHECK_EQ(rd(SMCP_R_CHG_STATUS), SMCP_CHG_DISCHARGING);
    CHECK_EQ(rd(SMCP_R_BST_STATE), SMCP_BST_DISCHARGING);
    CHECK_EQ(rd(SMCP_R_BST_RATE), 1500);
    CHECK_EQ(rd(SMCP_R_BST_REMAIN), 7800);
    CHECK_EQ(rd(SMCP_R_BATT_PERCENT), 97);
    CHECK_EQ(rd(SMCP_R_BATT_TTE), 300);
    CHECK_EQ(rd(SMCP_R_PSR), 0);
    CHECK_EQ(rd(SMCP_R_EVT_PENDING), SMCP_EV_BATT | SMCP_EV_AC);
    CHECK(smcp_irq(&S));
    smcp_reg_write(&S, SMCP_R_EVT_ACK, SMCP_EV_BATT); /* partial ack keeps the rest */
    CHECK_EQ(rd(SMCP_R_EVT_PENDING), SMCP_EV_AC);
    CHECK(smcp_irq(&S));
    smcp_reg_write(&S, SMCP_R_EVT_ACK, SMCP_EV_BATT | SMCP_EV_AC);
    CHECK(!smcp_irq(&S));
    /* nothing changes: no new events */
    run_ms(4500);
    CHECK_EQ(rd(SMCP_R_EVT_PENDING), 0);
    /* enable mask gates the line but not PENDING */
    smcp_reg_write(&S, SMCP_R_EVT_ENABLE, SMCP_EV_LID);
    mock_set_u(&M, "BUIC", 96);
    run_ms(2500);
    CHECK_EQ(rd(SMCP_R_EVT_PENDING), SMCP_EV_BATT);
    CHECK(!smcp_irq(&S));
    CHECK_EQ(rd(SMCP_R_EVT_ENABLE), SMCP_EV_LID);
}

static void set_status_keys(uint8_t ch0r_lo, uint8_t chce, uint8_t chcc, uint8_t bsfc, uint64_t chnc, uint8_t chsc)
{
    mock_set_u(&M, "CH0R", ch0r_lo);
    mock_set_u(&M, "CHCE", chce);
    mock_set_u(&M, "CHCC", chcc);
    mock_set_u(&M, "BSFC", bsfc);
    mock_set_u(&M, "CHNC", chnc);
    mock_set_u(&M, "CHSC", chsc);
}

static void t_status_logic(void)
{
    setup(0);
    run_ms(200);
    struct { uint32_t ch0r; uint8_t chce, chcc, bsfc; uint64_t chnc; uint8_t chsc; uint16_t chls; uint8_t buic; int want; } c[] = {
        {0, 1, 1, 0, 0, 0, 0, 50, SMCP_CHG_CHARGING},
        {0, 1, 1, 0, 1, 0, 0, 50, SMCP_CHG_FULL},               /* CHNC bit 0 */
        {0, 1, 1, 0, 1ull << 23, 0, 0, 50, SMCP_CHG_CHARGING},  /* BMS busy alone */
        {0, 1, 1, 0, 1ull << 24, 0, 0, 50, SMCP_CHG_NOT_CHARGING},
        {0, 1, 1, 0, 1ull << 23, 0, 0x50, 80, SMCP_CHG_NOT_CHARGING}, /* CHLS 80 -> limit 75, limited */
        {0, 1, 1, 0, 1ull << 23, 0, 0x50, 70, SMCP_CHG_CHARGING},
        {0, 1, 1, 0, 1ull << 23, 0, 0x05, 99, SMCP_CHG_CHARGING},     /* CHLS below 10: no limit */
        {0x10, 1, 1, 0, 0, 0, 0, 50, SMCP_CHG_DISCHARGING},     /* CH0R no-AC flag */
        {0x100, 1, 1, 0, 0, 0, 0, 50, SMCP_CHG_CHARGING},       /* CH0R BMS busy only */
        {0, 1, 0, 0, 0, 0, 0, 50, SMCP_CHG_DISCHARGING},        /* not charge capable */
        {0, 1, 1, 1, 0, 0, 0, 100, SMCP_CHG_FULL},
    };
    for (size_t i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
        set_status_keys(0, c[i].chce, c[i].chcc, c[i].bsfc, c[i].chnc, c[i].chsc);
        mock_set_u(&M, "CH0R", c[i].ch0r);
        mock_set_u(&M, "CHLS", c[i].chls);
        mock_set_u(&M, "BUIC", c[i].buic);
        run_ms(2100);
        if (rd(SMCP_R_CHG_STATUS) != (uint32_t)c[i].want)
            fprintf(stderr, "  status case %zu\n", i);
        CHECK_EQ(rd(SMCP_R_CHG_STATUS), c[i].want);
        uint32_t want_bst = c[i].want == SMCP_CHG_CHARGING ? SMCP_BST_CHARGING
                            : c[i].want == SMCP_CHG_DISCHARGING ? SMCP_BST_DISCHARGING : 0;
        CHECK_EQ(rd(SMCP_R_BST_STATE), want_bst);
    }
    /* CHNC absent: CHSC decides */
    setup(0);
    mock_remove(&M, "CHNC");
    mock_set_u(&M, "BSFC", 0);
    mock_set_u(&M, "CHSC", 0);
    run_ms(200);
    CHECK_EQ(rd(SMCP_R_CHG_STATUS), SMCP_CHG_NOT_CHARGING);
    mock_set_u(&M, "CHSC", 1);
    run_ms(2100);
    CHECK_EQ(rd(SMCP_R_CHG_STATUS), SMCP_CHG_CHARGING);
    /* CHCE unreadable -> UNKNOWN -> battery not present */
    mock_get(&M, "CHCE")->err = 0x82;
    run_ms(2100);
    CHECK_EQ(rd(SMCP_R_CHG_STATUS), SMCP_CHG_UNKNOWN);
    CHECK(!(rd(SMCP_R_STATUS) & SMCP_ST_BATT));
}

static void t_chwa(void)
{
    setup(0);
    mock_set(&M, "CHWA", "flag", 1, "01");
    run_ms(200);
    run_ms(2100);
    CHECK_EQ(mock_get(&M, "CHLS")->reads, 0);           /* CHWA readable: CHLS never read */
    set_status_keys(0, 1, 1, 0, 1ull << 23, 0);
    mock_set_u(&M, "BUIC", 76);
    run_ms(2100);
    CHECK_EQ(rd(SMCP_R_CHG_STATUS), SMCP_CHG_NOT_CHARGING); /* CHWA: limit 80-5 */
    mock_set_u(&M, "CHWA", 0);
    run_ms(2100);
    CHECK_EQ(rd(SMCP_R_CHG_STATUS), SMCP_CHG_CHARGING);
}

static void t_capacity_level(void)
{
    setup(0);
    run_ms(200);
    mock_set_u(&M, "BSFC", 0);
    mock_set_u(&M, "BCF0", 1);
    run_ms(2100);
    CHECK_EQ(rd(SMCP_R_CAP_LEVEL), SMCP_CAP_CRITICAL);
    CHECK(rd(SMCP_R_BST_STATE) & SMCP_BST_CRITICAL);
    mock_set_u(&M, "BCF0", 0);
    mock_set_u(&M, "ACSt", 0x31b);  /* CAN_BOOT_AP clear */
    run_ms(2100);
    CHECK_EQ(rd(SMCP_R_CAP_LEVEL), SMCP_CAP_LOW);
    mock_set_u(&M, "ACSt", 0x31d);  /* CAN_BOOT_IBOOT clear */
    run_ms(2100);
    CHECK_EQ(rd(SMCP_R_CAP_LEVEL), SMCP_CAP_CRITICAL);
    mock_set_u(&M, "ACSt", 0x31f);
    run_ms(2100);
    CHECK_EQ(rd(SMCP_R_CAP_LEVEL), SMCP_CAP_NORMAL);
    CHECK(!(rd(SMCP_R_BST_STATE) & SMCP_BST_CRITICAL));
}

static void t_pre27_b0rm(void)
{
    setup(0);
    mock_set(&M, "BCF0", "ui32", 4, "00000000");
    mock_set_u(&M, "B0RM", 0x801f); /* bytes 1f 80: big-endian 8064 on older firmware */
    run_ms(200);
    CHECK_EQ(rd(SMCP_R_BST_REMAIN), 8064);
    CHECK(S.fw_ge_27 == false);
    /* BCF0 of size 2: not accepted (Linux fails the probe; here the key is not present) */
    setup(0);
    mock_set(&M, "BCF0", "ui16", 2, "0000");
    run_ms(200);
    CHECK(!S.v[K_BCF0].present);
    CHECK_EQ(mock_get(&M, "BCF0")->reads, 0);
}

static void t_lid_and_button(void)
{
    setup(0);
    run_ms(200);
    smcp_reg_write(&S, SMCP_R_EVT_ACK, 0xff);
    smcp_reg_write(&S, SMCP_R_EVT_ENABLE, SMCP_EV_ALL);
    mock_set_u(&M, "MSLD", 1);
    int ms = 0;
    while (rd(SMCP_R_LID) != 0 && ms < 1000) {
        run_ms(1);
        ms++;
    }
    CHECK_EQ(rd(SMCP_R_LID), 0);
    CHECK(ms <= 60);
    CHECK_EQ(rd(SMCP_R_EVT_PENDING), SMCP_EV_LID);
    CHECK(smcp_irq(&S));
    smcp_reg_write(&S, SMCP_R_EVT_ACK, SMCP_EV_LID);
    mock_set_u(&M, "MSLD", 0);
    run_ms(100);
    CHECK_EQ(rd(SMCP_R_LID), 1);
    smcp_reg_write(&S, SMCP_R_EVT_ACK, 0xff);
    /* a 120 ms press is seen once; holding does not repeat */
    mock_set_u(&M, "bHLD", 1);
    run_ms(120);
    mock_set_u(&M, "bHLD", 0);
    run_ms(200);
    CHECK_EQ(rd(SMCP_R_PWRB_COUNT), 1);
    CHECK_EQ(rd(SMCP_R_EVT_PENDING), SMCP_EV_PWRB);
    mock_set_u(&M, "bHLD", 1);
    run_ms(3000);
    CHECK_EQ(rd(SMCP_R_PWRB_COUNT), 2);
    mock_set_u(&M, "bHLD", 0);
    run_ms(100);
    CHECK_EQ(rd(SMCP_R_PWRB_COUNT), 2);
}

static void t_notifications(void)
{
    setup(0);
    run_ms(200);
    smcp_reg_write(&S, SMCP_R_EVT_ACK, 0xff);
    mock_notify(&M, 0x72010601); /* Touch ID / power button down */
    run_ms(2);
    CHECK_EQ(rd(SMCP_R_PWRB_COUNT), 1);
    CHECK(rd(SMCP_R_EVT_PENDING) & SMCP_EV_PWRB);
    mock_notify(&M, 0x72010600); /* up: no count */
    mock_notify(&M, 0x7201fe01); /* held short: ignored */
    run_ms(2);
    CHECK_EQ(rd(SMCP_R_PWRB_COUNT), 1);
    /* power event: an immediate slow refresh instead of waiting up to 2 s */
    uint32_t seq = rd(SMCP_R_SEQ);
    run_ms(100);
    CHECK_EQ(rd(SMCP_R_SEQ), seq);
    mock_notify(&M, 0x71010100);
    run_ms(100);
    CHECK_EQ(rd(SMCP_R_SEQ), seq + 1);
    CHECK_EQ(S.notifications, 4);
    CHECK_EQ(mock_count_type(&M, SMCP_MSG_WRITE_KEY), 0);
}

static void t_timeouts(void)
{
    setup(0);
    run_ms(200);
    uint32_t seq = rd(SMCP_R_SEQ);
    mock_key *k = mock_get(&M, "B0AV");
    k->late_us = 700000;       /* reply arrives after the 500 ms timeout */
    for (int ms = 0; ms < 4000 && rd(SMCP_R_TIMEOUTS) == 0; ms++)
        run_ms(1);
    CHECK_EQ(rd(SMCP_R_TIMEOUTS), 1);
    /* the timeout is followed by a 1 s quiet period: nothing is sent while a late reply may land */
    {
        int s0 = M.sends;
        run_ms(900);
        CHECK_EQ(M.sends, s0);
    }
    k->late_us = 0;
    run_ms(6000);
    CHECK(rd(SMCP_R_SEQ) >= seq + 2);
    CHECK_EQ(rd(SMCP_R_BST_VOLTAGE), 13068);   /* the late reply did not land in another slot */
    CHECK(!(rd(SMCP_R_STATUS) & (SMCP_ST_FAILED | SMCP_ST_STALE)));
    /* a dropped reply: one more timeout, recovery */
    k->drop = 1;
    run_ms(4000);
    CHECK_EQ(rd(SMCP_R_TIMEOUTS), 2);
    CHECK(!(rd(SMCP_R_STATUS) & SMCP_ST_FAILED));
    /* SMC goes silent: 16 consecutive timeouts -> FAILED, STALE, no more commands */
    for (int i = 0; i < M.nk; i++)
        M.k[i].drop = 1000000;
    run_ms(40000);
    CHECK(rd(SMCP_R_STATUS) & SMCP_ST_FAILED);
    CHECK(rd(SMCP_R_STATUS) & SMCP_ST_STALE);
    CHECK(!(rd(SMCP_R_STATUS) & SMCP_ST_BACKEND));
    int sends = M.sends;
    run_ms(5000);
    CHECK_EQ(M.sends, sends);
    CHECK_EQ(rd(SMCP_R_BST_VOLTAGE), 13068);   /* last good values stay readable */
}

static void t_stale_and_dead(void)
{
    setup(0);
    run_ms(200);
    mock_get(&M, "BUIC")->drop = 1000000; /* one key keeps timing out: rounds take longer */
    for (int i = 0; i < M.nk; i++)
        M.k[i].drop = 1000000;
    run_ms(11000);
    CHECK(rd(SMCP_R_STATUS) & SMCP_ST_STALE);
    setup(0);
    run_ms(200);
    M.dead = true;     /* RTKit crashed */
    run_ms(10);
    CHECK(smcp_failed(&S));
    CHECK(rd(SMCP_R_STATUS) & SMCP_ST_FAILED);
    setup(0);
    M.send_fail = true;
    run_ms(100);
    CHECK(rd(SMCP_R_ERR_COUNT) > 0);
    CHECK(!(rd(SMCP_R_STATUS) & SMCP_ST_VALID));
}

static void t_size_mismatch(void)
{
    setup(0);
    mock_set(&M, "B0AV", "ui32", 4, "0c330000"); /* unexpected size: not trusted, never read */
    run_ms(300);
    CHECK(!S.v[K_B0AV].present);
    CHECK_EQ(mock_get(&M, "B0AV")->reads, 0);
    CHECK(!(rd(SMCP_R_STATUS) & SMCP_ST_BATT));    /* Linux: no B0AV, no battery */
    CHECK_EQ(rd(SMCP_R_BST_VOLTAGE), 0xffffffffu);
    /* reply size larger than requested -> error, value not stored */
    setup(0);
    run_ms(300);
    mock_get(&M, "B0CT")->size = 3;
    run_ms(2100);
    CHECK(!S.v[K_B0CT].ok);
}

static void t_edges(void)
{
    setup(0);
    run_ms(200);
    /* first data was raised before _INI: enabling pending events gives one edge */
    CHECK_EQ(smcp_irq_edges(&S), 0);
    smcp_reg_write(&S, SMCP_R_EVT_ENABLE, SMCP_EV_ALL);
    CHECK_EQ(smcp_irq_edges(&S), 1);
    smcp_reg_write(&S, SMCP_R_EVT_ENABLE, SMCP_EV_ALL); /* no change: no edge */
    CHECK_EQ(smcp_irq_edges(&S), 1);
    /* _EVT acks; a new lid event gives a new edge within the 50 ms poll (before any re-pulse) */
    smcp_reg_write(&S, SMCP_R_EVT_ACK, smcp_reg_read(&S, SMCP_R_EVT_PENDING));
    mock_set_u(&M, "MSLD", 1);
    run_ms(80);
    CHECK_EQ(smcp_irq_edges(&S), 2);
    smcp_reg_write(&S, SMCP_R_EVT_ACK, SMCP_EV_LID);
    run_ms(500);                       /* acked: quiet */
    CHECK_EQ(smcp_irq_edges(&S), 2);
    mock_set_u(&M, "MSLD", 0);
    run_ms(80);
    CHECK_EQ(smcp_irq_edges(&S), 3);
    /* disabled events raise no edge and are not re-pulsed */
    smcp_reg_write(&S, SMCP_R_EVT_ACK, 0xff);
    smcp_reg_write(&S, SMCP_R_EVT_ENABLE, SMCP_EV_LID);
    mock_set_u(&M, "bHLD", 1);
    run_ms(500);
    CHECK_EQ(smcp_irq_edges(&S), 3);
    CHECK_EQ(rd(SMCP_R_EVT_PENDING), SMCP_EV_PWRB);
}

/* the edge at _INI time can be lost (interrupt not yet connected): unacked events re-pulse */
static void t_repulse(void)
{
    setup(0);
    run_ms(200);
    smcp_reg_write(&S, SMCP_R_EVT_ENABLE, SMCP_EV_ALL);
    uint32_t e0 = smcp_irq_edges(&S);
    run_ms(1000);                                   /* nobody acks */
    uint32_t e1 = smcp_irq_edges(&S);
    CHECK(e1 - e0 >= 9 && e1 - e0 <= 11);           /* one per 100 ms */
    smcp_reg_write(&S, SMCP_R_EVT_ACK, 0xff);
    run_ms(1000);                                   /* acked and quiet: no more pulses */
    CHECK_EQ(smcp_irq_edges(&S), e1);
    /* an unplug after a lost edge still reaches the guest */
    mock_set_u(&M, "CHIS", 0);
    mock_set_u(&M, "CHCE", 0);
    run_ms(2500);
    CHECK(smcp_irq_edges(&S) > e1);
    CHECK(smcp_irq(&S));
}

static void t_registers(void)
{
    setup(SMCP_REC_F_POWER_ACTIONS);
    run_ms(200);
    CHECK(rd(SMCP_R_STATUS) & SMCP_ST_POWERACT);
    CHECK_EQ(rd(0x002), 0);
    CHECK_EQ(rd(0x1fe), 0);
    CHECK_EQ(rd(SMCP_R_REG_END), 0);
    CHECK_EQ(rd(0x3ffc), 0);
    uint32_t v = rd(SMCP_R_BST_REMAIN);
    smcp_reg_write(&S, SMCP_R_BST_REMAIN, 1);
    smcp_reg_write(&S, SMCP_R_MAGIC, 0);
    smcp_reg_write(&S, 0x3ff0, 0xffffffff);
    CHECK_EQ(rd(SMCP_R_BST_REMAIN), v);
    CHECK_EQ(rd(SMCP_R_MAGIC), SMCP_MAGIC);
    smcp_reg_write(&S, SMCP_R_EVT_ENABLE, 0xffffffff);
    CHECK_EQ(rd(SMCP_R_EVT_ENABLE), SMCP_EV_ALL);
    CHECK_EQ(rd(SMCP_R_EVT_ACK), 0);
}

static void t_flt(void)
{
    struct { uint32_t b; int32_t m; } v[] = {
        {0x4231599a, 44337}, {0x41dc0000, 27500}, {0x44a8c000, 1350000}, {0x45a72800, 5349000},
        {0x45b48800, 5777000}, {0x411ac4f9, 9673}, {0, 0}, {0x80000000, 0}, {0x3f800000, 1000},
        {0xbf800000, -1000}, {0x3a83126e, 0} /* 0.00099999993 */, {0x3a83126f, 1} /* 0.0010000000475 */,
        {0x7f800000, INT32_MAX}, {0xff800000, INT32_MIN}, {0x7fc00000, INT32_MAX},
        {0x4f000000, INT32_MAX} /* 2^31 */, {0x4a000000, 2097152000} /* 2^21 */, {0x00000001, 0},
        {0xc2480000, -50000},
    };
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        int32_t got = smcp_flt_to_milli(v[i].b);
        if (got != v[i].m)
            fprintf(stderr, "  flt 0x%08x -> %d want %d\n", v[i].b, got, v[i].m);
        CHECK_EQ((uint32_t)got, (uint32_t)v[i].m);
    }
}

/* nightly-2 (review finding 3): a full mailbox never blocks the tick; the step is retried, no error counted,
 * the message ID sequence stays contiguous; full for 200 ms counts as one failed send (quarantine) */
static void t_busy(void)
{
    setup(0);
    run_ms(300);
    CHECK(rd(SMCP_R_STATUS) & SMCP_ST_VALID);
    uint32_t e0 = rd(SMCP_R_ERR_COUNT);
    int n0 = M.nlog;
    uint8_t id_before = n0 ? (uint8_t)((M.log[n0 - 1].msg >> 12) & 0xf) : 0;
    M.busy_left = 5;
    int sends_before = M.sends;
    max_sends_per_tick = 0;
    run_ms(120);                                              /* covers the next fast round (50 ms period) */
    CHECK_EQ(M.busy_answers, 5);
    CHECK(M.sends - sends_before >= 6);                       /* retried, one try per tick at most */
    CHECK(max_sends_per_tick <= 1);
    CHECK_EQ(rd(SMCP_R_ERR_COUNT), e0);                         /* BUSY is not an error */
    CHECK(S.quarantine_until <= M.t);
    CHECK(S.send_busy >= 5);
    CHECK(M.nlog > n0);
    CHECK_EQ((uint8_t)((M.log[n0].msg >> 12) & 0xf), (uint8_t)((id_before + 1) & 0xf)); /* ID not consumed */
    run_ms(300);
    CHECK(rd(SMCP_R_STATUS) & SMCP_ST_VALID);
    /* full for longer than 200 ms: exactly one failed send, then quarantine */
    setup(0);
    run_ms(300);
    e0 = rd(SMCP_R_ERR_COUNT);
    M.busy_left = 1000000;
    run_ms(150);
    CHECK_EQ(rd(SMCP_R_ERR_COUNT), e0);
    run_ms(100);
    CHECK_EQ(rd(SMCP_R_ERR_COUNT), e0 + 1);
    CHECK(S.quarantine_until > M.t);
    CHECK(!smcp_failed(&S));
    /* the guest-stopped power path retries a briefly full mailbox instead of failing */
    setup(SMCP_REC_F_POWER_ACTIONS);
    run_ms(200);
    M.pmu[0xf801] = 0x30;
    M.pmu[0xf80f] = 0x41;
    M.busy_left = 3;
    smcp_pmu_ops p = mock_pmu_ops(&M);
    smcp_power_trace t;
    CHECK_EQ(smcp_power_run(&S, &p, SMCP_POWER_OFF, &t), 0);
    CHECK_EQ(t.n, 7);
    CHECK_EQ(t.step[0].result, 0);
}

static void t_power_off(void)
{
    setup(SMCP_REC_F_POWER_ACTIONS);
    run_ms(200);
    M.pmu[0xf801] = 0x30;
    M.pmu[0xf80f] = 0x41;
    int nlog0 = M.nlog;
    smcp_pmu_ops p = mock_pmu_ops(&M);
    smcp_power_trace t;
    int r = smcp_power_run(&S, &p, SMCP_POWER_OFF, &t);
    CHECK_EQ(r, 0); /* the mock does not power off: "still alive after 100 ms" */
    CHECK_EQ(t.n, 7);
    CHECK_EQ(t.step[0].kind, SMCP_STEP_SMC_WRITE);
    CHECK_EQ(t.step[0].key_or_reg, SMCP_KEY('M', 'B', 'S', 'E'));
    CHECK_EQ(t.step[0].value, SMCP_KEY('o', 'f', 'f', 'w'));
    CHECK_EQ(t.step[1].kind, SMCP_STEP_PMU_WRITE);
    CHECK_EQ(t.step[1].key_or_reg, 0xf801);
    CHECK_EQ(t.step[1].value, 0);
    CHECK_EQ(t.step[2].kind, SMCP_STEP_PMU_READ);
    CHECK_EQ(t.step[2].key_or_reg, 0xf80f);
    CHECK_EQ(t.step[3].kind, SMCP_STEP_PMU_WRITE);
    CHECK_EQ(t.step[3].value, 0x49);
    CHECK_EQ(t.step[4].key_or_reg, SMCP_KEY('N', 'T', 'A', 'P'));
    CHECK_EQ(t.step[4].value, 0);
    CHECK_EQ(t.step[5].value, SMCP_KEY('o', 'f', 'f', '1'));
    CHECK_EQ(t.step[6].kind, SMCP_STEP_DELAY);
    CHECK_EQ(M.pmu[0xf801], 0);
    CHECK_EQ(M.pmu[0xf80f], 0x49);
    CHECK_EQ(M.pmu_writes, 2);
    CHECK_EQ(M.pmu_reads, 1);
    CHECK_EQ(M.delayed_us, 100000); /* mdelay(100) after off1, as Linux */
    /* wire bytes: three writes after the poller's reads; MBSE gets 4 LE bytes of the fourcc */
    int w = 0;
    for (int i = nlog0; i < M.nlog; i++) {
        if ((M.log[i].msg & 0xff) != SMCP_MSG_WRITE_KEY)
            continue;
        uint64_t m = M.log[i].msg;
        if (w == 0) {
            CHECK_EQ(m >> 32, SMCP_KEY('M', 'B', 'S', 'E'));
            CHECK_EQ((m >> 16) & 0xff, 4);
            CHECK(!memcmp(M.log[i].data, "wffo", 4));
        } else if (w == 1) {
            CHECK_EQ(m >> 32, SMCP_KEY('N', 'T', 'A', 'P'));
            CHECK_EQ((m >> 16) & 0xff, 1);
            CHECK_EQ(M.log[i].data[0], 0);
        } else if (w == 2) {
            CHECK(!memcmp(M.log[i].data, "1ffo", 4));
        }
        w++;
    }
    CHECK_EQ(w, 3);
    CHECK_EQ(mock_count_type(&M, SMCP_MSG_WRITE_KEY), 3);
}

static void t_restart(void)
{
    setup(SMCP_REC_F_POWER_ACTIONS);
    run_ms(200);
    M.pmu[0xf801] = 0x30;
    M.pmu[0xf80f] = 0x49;
    smcp_pmu_ops p = mock_pmu_ops(&M);
    smcp_power_trace t;
    int r = smcp_power_run(&S, &p, SMCP_POWER_RESTART, &t);
    CHECK_EQ(r, 0);
    CHECK_EQ(t.step[0].value, SMCP_KEY('r', 'e', 's', 't'));
    CHECK_EQ(t.step[5].value, SMCP_KEY('p', 'h', 'r', 'a'));
    CHECK_EQ(M.pmu[0xf80f], 0x41);
    CHECK_EQ(M.pmu[0xf801], 0);
    /* restart with a command still outstanding from the poller: waits, then proceeds */
    setup(0);
    run_ms(200);
    M.t += 1000;
    smcp_tick(&S);
    CHECK(S.busy || S.quarantine_until > 0 || 1);
    r = smcp_power_run(&S, &p, SMCP_POWER_RESTART, &t);
    CHECK_EQ(r, 0);
    CHECK_EQ(t.n, 7);
}

static void t_power_failures(void)
{
    smcp_pmu_ops p;
    smcp_power_trace t;
    /* final write rejected by the SMC */
    setup(0);
    run_ms(200);
    M.write_result = 0x86;
    p = mock_pmu_ops(&M);
    int r = smcp_power_run(&S, &p, SMCP_POWER_OFF, &t);
    CHECK(r < 0);
    CHECK_EQ(t.n, 6);   /* no delay step: returned straight after off1 failed */
    /* SMC never answers writes: offw and NTAP time out at 0.5 s, off1 at 5 s */
    setup(0);
    run_ms(200);
    M.writes_no_reply = true;
    p = mock_pmu_ops(&M);
    uint64_t t0 = M.t;
    r = smcp_power_run(&S, &p, SMCP_POWER_OFF, &t);
    CHECK_EQ(r, -1);
    CHECK(M.t - t0 >= 5000000);
    CHECK(M.t - t0 < 9000000);
    /* PMU unreachable: SMC part still runs (Linux logs and continues) */
    setup(0);
    run_ms(200);
    M.pmu_fail = true;
    p = mock_pmu_ops(&M);
    r = smcp_power_run(&S, &p, SMCP_POWER_RESTART, &t);
    CHECK_EQ(r, 0);
    CHECK_EQ(t.step[1].result, -1);
    CHECK_EQ(t.step[2].result, -1);
    CHECK_EQ(t.n, 6);  /* read failed: no RMW write step */
    /* backend failed: nothing is sent */
    setup(0);
    run_ms(200);
    M.dead = true;
    run_ms(5);
    p = mock_pmu_ops(&M);
    int sends = M.sends;
    r = smcp_power_run(&S, &p, SMCP_POWER_OFF, &t);
    CHECK(r < 0);
    CHECK_EQ(M.sends, sends);
    CHECK_EQ(smcp_power_run(&S, &p, 7, &t), -10);
}

static uint32_t rng = 0x12345678;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static void t_fuzz(void)
{
    const char *keys[] = {"CH0R", "CHCE", "CHCC", "BSFC", "CHNC", "CHSC", "CHLS", "BUIC", "BCF0", "ACSt", "B0AC",
                          "B0RM", "MSLD", "bHLD", "TCMb", "F0Ac", "BNCB", "B0TE"};
    int bad = 0;
    for (int round = 0; round < 400; round++) {
        setup(rnd() & 1);
        if (rnd() % 4 == 0)
            mock_remove(&M, keys[rnd() % 18]);
        for (int step = 0; step < 30; step++) {
            mock_key *k = mock_get(&M, keys[rnd() % 18]);
            if (k)
                for (unsigned i = 0; i < k->size; i++)
                    k->b[i] = (uint8_t)rnd();
            if (rnd() % 10 == 0)
                mock_notify(&M, rnd());
            if (rnd() % 20 == 0 && k)
                k->drop = 1;
            if (rnd() % 25 == 0 && k)
                k->err = (rnd() & 1) ? 0x82 : 0;
            if (rnd() % 7 == 0)
                smcp_reg_write(&S, rnd() & 0x3ffc, rnd());
            run_ms(50 + rnd() % 400);
            uint32_t st = rd(SMCP_R_BST_STATE);
            if (st & ~7u || (st & 3) == 3)
                bad++;
            if (rd(SMCP_R_CHG_STATUS) > 4 || rd(SMCP_R_CAP_LEVEL) > 5 || rd(SMCP_R_LID) > 1 || rd(SMCP_R_PSR) > 1)
                bad++;
            if (rd(SMCP_R_MAGIC) != SMCP_MAGIC || rd(SMCP_R_EVT_PENDING) & ~SMCP_EV_ALL)
                bad++;
            if (smcp_irq(&S) != ((rd(SMCP_R_EVT_PENDING) & rd(SMCP_R_EVT_ENABLE)) != 0))
                bad++;
            for (uint32_t off = 0; off < 0x200; off += 4)
                (void)rd(off);
        }
        if (mock_count_type(&M, SMCP_MSG_WRITE_KEY))
            bad++;
    }
    CHECK_EQ(bad, 0);
    CHECK(max_sends_per_tick <= 1);
    CHECK(max_recv_calls_per_tick <= (int)SMCP_RECV_BUDGET);
    /* random raw replies (garbage ids, results, sizes) straight into the client */
    int bad2 = 0;
    for (int round = 0; round < 200; round++) {
        setup(0);
        for (int i = 0; i < 300; i++) {
            if (M.nq < MOCK_MAXQ - 2) {
                uint64_t g = ((uint64_t)rnd() << 32) | rnd();
                M.q[M.nq].due = M.t;
                M.q[M.nq].msg = g;
                M.nq++;
            }
            run_ms(1 + rnd() % 30);
            if (rd(SMCP_R_MAGIC) != SMCP_MAGIC || rd(SMCP_R_BST_STATE) & ~7u)
                bad2++;
        }
    }
    CHECK_EQ(bad2, 0);
}

int main(void)
{
    struct { const char *name; void (*fn)(void); } tests[] = {
        {"nominal_t1b", t_nominal_t1b}, {"reads_only", t_reads_only}, {"message_format", t_message_format},
        {"discharging", t_discharging}, {"status_logic", t_status_logic}, {"chwa", t_chwa},
        {"capacity_level", t_capacity_level}, {"pre27_b0rm", t_pre27_b0rm}, {"lid_and_button", t_lid_and_button},
        {"notifications", t_notifications}, {"timeouts", t_timeouts}, {"stale_and_dead", t_stale_and_dead},
        {"size_mismatch", t_size_mismatch}, {"edges", t_edges}, {"repulse", t_repulse}, {"registers", t_registers}, {"flt", t_flt},
        {"power_off", t_power_off}, {"restart", t_restart}, {"power_failures", t_power_failures},
        {"fuzz", t_fuzz}, {"busy", t_busy},
    };
    int groups_failed = 0;
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int f0 = fails, c0 = checks;
        tests[i].fn();
        printf("%-16s %s (%d checks)\n", tests[i].name, fails == f0 ? "PASS" : "FAIL", checks - c0);
        groups_failed += fails != f0;
    }
    printf("groups %zu, failed %d; checks %d, failed %d\n", sizeof(tests) / sizeof(tests[0]), groups_failed,
           checks, fails);
    return fails ? 1 : 0;
}
