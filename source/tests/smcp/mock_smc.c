/* SPDX-License-Identifier: MIT */
#include "mock_smc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t k4(const char *s) { return SMCP_KEY(s[0], s[1], s[2], s[3]); }

void mock_init(mock_smc *m)
{
    memset(m, 0, sizeof(*m));
    m->latency_us = 300;
    m->t = 1000000;
}

mock_key *mock_get(mock_smc *m, const char *name)
{
    for (int i = 0; i < m->nk; i++)
        if (m->k[i].key == k4(name))
            return &m->k[i];
    return NULL;
}

mock_key *mock_set(mock_smc *m, const char *name, const char *type, uint8_t size, const char *hex)
{
    mock_key *k = mock_get(m, name);
    if (!k) {
        assert(m->nk < MOCK_MAXKEYS);
        k = &m->k[m->nk++];
        memset(k, 0, sizeof(*k));
        k->key = k4(name);
    }
    k->type = k4(type);
    k->size = size;
    k->exists = true;
    memset(k->b, 0, sizeof(k->b));
    for (unsigned i = 0; i < size && hex[2 * i]; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        k->b[i] = (uint8_t)v;
    }
    return k;
}

void mock_set_u(mock_smc *m, const char *name, uint64_t v)
{
    mock_key *k = mock_get(m, name);
    assert(k && k->exists);
    for (unsigned i = 0; i < k->size && i < 8; i++)
        k->b[i] = (v >> (8 * i)) & 0xff;
}

void mock_remove(mock_smc *m, const char *name)
{
    mock_key *k = mock_get(m, name);
    if (k)
        k->exists = false;
}

static void push(mock_smc *m, uint64_t due, uint64_t msg)
{
    assert(m->nq < MOCK_MAXQ);
    int i = m->nq++;
    /* keep sorted by due time (stable) */
    while (i > 0 && m->q[i - 1].due > due) {
        m->q[i] = m->q[i - 1];
        i--;
    }
    m->q[i].due = due;
    m->q[i].msg = msg;
}

void mock_notify(mock_smc *m, uint32_t ev)
{
    push(m, m->t, (uint64_t)SMCP_MSG_NOTIFICATION | ((uint64_t)ev << 32));
}

static int m_send(void *ctx, uint64_t msg)
{
    mock_smc *m = ctx;
    m->sends++;
    if (m->send_fail)
        return -1;
    uint8_t type = msg & 0xff, id = (msg >> 12) & 0xf, size = (msg >> 16) & 0xff;
    uint32_t key = (uint32_t)(msg >> 32);
    assert(m->nlog < MOCK_MAXLOG);
    mock_log *l = &m->log[m->nlog++];
    l->msg = msg;
    l->len = 0;
    mock_key *k = NULL;
    for (int i = 0; i < m->nk; i++)
        if (m->k[i].key == key && m->k[i].exists)
            k = &m->k[i];
    uint64_t reply = ((uint64_t)id << 12);
    uint64_t due = m->t + m->latency_us;
    switch (type) {
        case SMCP_MSG_GET_KEY_INFO:
            if (!k) {
                reply |= 0x84;
            } else {
                m->shmem[0] = k->size;
                m->shmem[1] = k->type >> 24;
                m->shmem[2] = k->type >> 16;
                m->shmem[3] = k->type >> 8;
                m->shmem[4] = k->type;
                m->shmem[5] = 0x84;
            }
            break;
        case SMCP_MSG_READ_KEY:
            if (!k) {
                reply |= 0x84;
                break;
            }
            k->reads++;
            if (k->drop > 0) {
                k->drop--;
                return 0;
            }
            if (k->err) {
                reply |= k->err;
                break;
            }
            if (k->late_us)
                due = m->t + k->late_us;
            reply |= (uint64_t)k->size << 16;
            if (size <= 4) {
                uint32_t d = 0;
                for (unsigned i = 0; i < k->size && i < 4; i++)
                    d |= (uint32_t)k->b[i] << (8 * i);
                reply |= (uint64_t)d << 32;
            } else {
                memcpy(m->shmem, k->b, k->size);
            }
            break;
        case SMCP_MSG_WRITE_KEY:
            l->len = size > 8 ? 8 : size;
            memcpy(l->data, m->shmem, l->len);
            if (m->writes_no_reply)
                return 0;
            reply |= m->write_result;
            break;
        default:
            reply |= 0x81;
    }
    push(m, due, reply);
    return 0;
}

static int m_recv(void *ctx, uint64_t *msg)
{
    mock_smc *m = ctx;
    m->recv_calls++;
    if (m->dead)
        return -1;
    if (!m->nq || m->q[0].due > m->t)
        return 0;
    *msg = m->q[0].msg;
    memmove(&m->q[0], &m->q[1], sizeof(m->q[0]) * (size_t)(--m->nq));
    m->recvs++;
    /* the blocking path polls recv in a loop: let mock time pass so replies become due */
    return 1;
}

static void m_shr(void *ctx, uint8_t *dst, unsigned len)
{
    mock_smc *m = ctx;
    assert(len <= sizeof(m->shmem));
    memcpy(dst, m->shmem, len);
}

static void m_shw(void *ctx, const uint8_t *src, unsigned len)
{
    mock_smc *m = ctx;
    assert(len <= sizeof(m->shmem));
    memcpy(m->shmem, src, len);
}

static uint64_t m_now(void *ctx)
{
    mock_smc *m = ctx;
    m->t += 1; /* every clock read advances 1 us, so blocking loops always terminate */
    return m->t;
}

smcp_ops mock_ops(mock_smc *m)
{
    smcp_ops o = {m, m_send, m_recv, m_shr, m_shw, m_now};
    return o;
}

static int p_r(void *ctx, uint16_t reg, uint8_t *v)
{
    mock_smc *m = ctx;
    m->pmu_reads++;
    if (m->pmu_fail)
        return -1;
    *v = m->pmu[reg];
    return 0;
}
static int p_w(void *ctx, uint16_t reg, uint8_t v)
{
    mock_smc *m = ctx;
    m->pmu_writes++;
    if (m->pmu_fail)
        return -1;
    m->pmu[reg] = v;
    return 0;
}
static void p_d(void *ctx, uint32_t us)
{
    mock_smc *m = ctx;
    m->delayed_us += us;
    m->t += us;
}

smcp_pmu_ops mock_pmu_ops(mock_smc *m)
{
    smcp_pmu_ops o = {m, p_r, p_w, p_d};
    return o;
}

int mock_count_type(const mock_smc *m, uint8_t type)
{
    int n = 0;
    for (int i = 0; i < m->nlog; i++)
        n += (m->log[i].msg & 0xff) == type;
    return n;
}

/* runs/20261110-145557-t1b-gpu/t1-evidence.json smc.raw_a + key_info (sizes/types) */
void mock_load_t1b(mock_smc *m)
{
    mock_set(m, "BUIC", "ui8 ", 1, "64");
    mock_set(m, "B0AV", "ui16", 2, "0c33");
    mock_set(m, "B0AC", "si16", 2, "0000");
    mock_set(m, "B0CT", "ui16", 2, "2100");
    mock_set(m, "B0FC", "ui16", 2, "801f");
    mock_set(m, "B0RM", "ui16", 2, "801f");
    mock_set(m, "B0DC", "ui16", 2, "8321");
    mock_set(m, "B0AT", "ui16", 2, "bf0b");
    mock_set(m, "BNCB", "si8 ", 1, "03");
    mock_set(m, "BBAD", "flag", 1, "00");
    mock_set(m, "BSFC", "flag", 1, "01");
    mock_set(m, "BCF0", "ui8 ", 1, "00");     /* not in raw_a; size 1 per t1b "b0rm native" */
    mock_set(m, "CHIS", "ui32", 4, "01000000");
    mock_set(m, "CHCE", "ui8 ", 1, "01");
    mock_set(m, "CHCC", "ui8 ", 1, "01");
    mock_set(m, "CHSC", "ui8 ", 1, "00");
    mock_set(m, "CH0R", "hex_", 4, "00000000");
    mock_set(m, "CHNC", "hex_", 8, "0100400000000000");
    mock_set(m, "CHLS", "hex_", 2, "0000");
    mock_set(m, "ACSt", "hex_", 4, "1f030000");
    mock_set(m, "ACPW", "ui32", 4, "c8210200");
    mock_set(m, "AC-W", "si8 ", 1, "04");     /* present but never read by SMCP */
    mock_set(m, "MSLD", "ui8 ", 1, "00");
    mock_set(m, "bHLD", "ui32", 4, "00000000");
    mock_set(m, "F0Ac", "flt ", 4, "00000000");
    mock_set(m, "F1Ac", "flt ", 4, "00000000");
    mock_set(m, "F0Mn", "flt ", 4, "00c0a844");
    mock_set(m, "F0Mx", "flt ", 4, "0028a745");
    mock_set(m, "F1Mn", "flt ", 4, "00c0a844");
    mock_set(m, "F1Mx", "flt ", 4, "0088b445");
    mock_set(m, "PSTR", "flt ", 4, "f9c41a41");
    mock_set(m, "TCMb", "flt ", 4, "9a593142");
    mock_set(m, "TB0T", "flt ", 4, "0000dc41");
    mock_set(m, "TCHP", "flt ", 4, "b8e70442");
    mock_set(m, "TW0P", "flt ", 4, "0caa0542");
    mock_set(m, "TH0x", "flt ", 4, "0090f741");
    mock_set(m, "CLKM", "hex_", 6, "a6d809486c00");
    mock_set(m, "MBSE", "hex_", 4, "00000000");
    mock_set(m, "NTAP", "flag", 1, "00");
    /* not read by t1b, read by Linux; values for a full battery on AC (inference) */
    mock_set(m, "B0TE", "ui16", 2, "ffff");
    mock_set(m, "B0TF", "ui16", 2, "ffff");
    /* AC-i and AC-n are absent on this machine (t1b: SMC result 0x84); CHWA unknown: absent here */
}
