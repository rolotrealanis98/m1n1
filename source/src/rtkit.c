/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "../config.h"

#include "rtkit.h"
#include "adt.h"
#include "asc.h"
#include "dart.h"
#include "iova.h"
#include "malloc.h"
#include "sart.h"
#include "string.h"
#include "types.h"
#include "utils.h"

#define rtkit_printf(...)                                                                          \
    do {                                                                                           \
        if (rtk->mtp && !rtk->mtp_boot_log) break; /* MTP: no runtime/key-bearing logs */   \
        debug_printf("rtkit(%s): ", rtk->name);                                                    \
        debug_printf(__VA_ARGS__);                                                                 \
    } while (0)

#define RTKIT_EP_MGMT     0
#define RTKIT_EP_CRASHLOG 1
#define RTKIT_EP_SYSLOG   2
#define RTKIT_EP_DEBUG    3
#define RTKIT_EP_IOREPORT 4
#define RTKIT_EP_OSLOG    8

#define MGMT_TYPE GENMASK(59, 52)

#define MGMT_PWR_STATE GENMASK(15, 0)

#define MSG_BUFFER_REQUEST      1
#define MSG_BUFFER_REQUEST_SIZE GENMASK(51, 44)
#define MSG_BUFFER_REQUEST_IOVA GENMASK(41, 0)

#define MSG_SYSLOG_INIT           8
#define MSG_SYSLOG_INIT_ENTRYSIZE GENMASK(39, 24)
#define MSG_SYSLOG_INIT_COUNT     GENMASK(15, 0)
#define MSG_SYSLOG_LOG            5
#define MSG_SYSLOG_LOG_INDEX      GENMASK(7, 0)

#define MSG_OSLOG_INIT 0x10
#define MSG_OSLOG_ACK  0x30

#define MGMT_MSG_HELLO        1
#define MGMT_MSG_HELLO_ACK    2
#define MGMT_MSG_HELLO_MINVER GENMASK(15, 0)
#define MGMT_MSG_HELLO_MAXVER GENMASK(31, 16)

#define MGMT_MSG_IOP_PWR_STATE     6
#define MGMT_MSG_IOP_PWR_STATE_ACK 7

#define MGMT_MSG_EPMAP        8
#define MGMT_MSG_EPMAP_DONE   BIT(51)
#define MGMT_MSG_EPMAP_BASE   GENMASK(34, 32)
#define MGMT_MSG_EPMAP_BITMAP GENMASK(31, 0)

#define MGMT_MSG_EPMAP_REPLY      8
#define MGMT_MSG_EPMAP_REPLY_DONE BIT(51)
#define MGMT_MSG_EPMAP_REPLY_MORE BIT(0)

#define MGMT_MSG_AP_PWR_STATE     0xb
#define MGMT_MSG_AP_PWR_STATE_ACK 0xb

#define MGMT_MSG_START_EP      5
#define MGMT_MSG_START_EP_IDX  GENMASK(39, 32)
#define MGMT_MSG_START_EP_FLAG BIT(1)

#define RTKIT_MIN_VERSION 11
#define RTKIT_MAX_VERSION 12

#define IOVA_MASK GENMASK(35, 0)

enum rtkit_power_state {
    RTKIT_POWER_OFF = 0x00,
    RTKIT_POWER_SLEEP = 0x01,
    RTKIT_POWER_QUIESCED = 0x10,
    RTKIT_POWER_ON = 0x20,
    RTKIT_POWER_INIT = 0x220,
};

struct rtkit_dev {
    char *name;

    asc_dev_t *asc;
    dart_dev_t *dart;
    iova_domain_t *dart_iovad;
    sart_dev_t *sart;
    bool sram;
    bool mtp, tx_pending, oslog_buffer, mtp_boot_log;
    u64 sram_base, sram_size, tx_deadline;
    struct asc_message pending_tx;
    /* MTP: host-provided buffers for requests with IOVA 0 (Linux rtkit-helper
     * shmem_setup -> dma_alloc_coherent through dart-mtp). NULL = refuse, as before. */
    bool (*mtp_alloc)(size_t sz, void **cpu, u64 *iova);
    struct rtkit_buffer oslog_bfr;

    u64 dva_base;

    enum rtkit_power_state iop_power;
    enum rtkit_power_state ap_power;

    struct rtkit_buffer syslog_bfr;
    struct rtkit_buffer crashlog_bfr;
    struct rtkit_buffer ioreport_bfr;

    u32 syslog_cnt, syslog_size;

    bool crashed;
};

struct syslog_log {
    u32 hdr;
    u32 unk;
    char context[24];
    char msg[];
};

struct crashlog_hdr {
    u32 type;
    u32 ver;
    u32 total_size;
    u32 flags;
    u8 _padding[16];
};

struct crashlog_entry {
    u32 type;
    u32 _padding;
    u32 flags;
    u32 len;
    u8 payload[];
};

rtkit_dev_t *rtkit_init(const char *name, asc_dev_t *asc, dart_dev_t *dart,
                        iova_domain_t *dart_iovad, sart_dev_t *sart, bool sram)
{
    if (dart && sart) {
        printf("rtkit: Cannot use both SART and DART simultaneously\n");
        return NULL;
    }

    if (dart && !dart_iovad) {
        printf("rtkit: if DART is used iovad is already required\n");
        return NULL;
    }

    if (sram && (dart || sart)) {
        printf("rtkit: cannot use SRAM with DART or SART \n");
        return NULL;
    }

    rtkit_dev_t *rtk = calloc(1, sizeof(*rtk));
    if (!rtk)
        return NULL;

    size_t name_len = strlen(name);
    rtk->name = calloc(name_len + 1, 1);
    if (!rtk->name)
        goto out_free_rtk;
    strcpy(rtk->name, name);

    rtk->asc = asc;
    rtk->dart = dart;
    rtk->dart_iovad = dart_iovad;
    rtk->sart = sart;
    rtk->sram = sram;
    rtk->iop_power = RTKIT_POWER_OFF;
    rtk->ap_power = RTKIT_POWER_OFF;
    rtk->dva_base = 0;

    int iop_node = asc_get_iop_node(asc);
    ADT_GETPROP(adt, iop_node, "asc-dram-mask", &rtk->dva_base);

    return rtk;

out_free_rtk:
    free(rtk);
    return NULL;
}

void rtkit_free(rtkit_dev_t *rtk)
{
    rtkit_free_buffer(rtk, &rtk->syslog_bfr);
    rtkit_free_buffer(rtk, &rtk->crashlog_bfr);
    rtkit_free_buffer(rtk, &rtk->ioreport_bfr);
    free(rtk->name);
    free(rtk);
}

void rtkit_mtp_mode(rtkit_dev_t *rtk, u64 base, size_t size)
{
    rtk->mtp = true;
    rtk->sram_base = base;
    rtk->sram_size = size;
}

static bool rtkit_mail_send(rtkit_dev_t *rtk, const struct asc_message *msg);

void rtkit_mtp_host_buffers(rtkit_dev_t *rtk, bool (*alloc)(size_t sz, void **cpu, u64 *iova))
{
    rtk->mtp_alloc = alloc;
}

/*
 * MTP buffer request with IOVA 0: the coprocessor asks the host for memory
 * (Linux rtkit.c apple_rtkit_common_rx_get_buffer -> rtkit-helper shmem_setup ->
 * dma_alloc_coherent). Allocate, then reply with the same type, the size and the
 * DMA address. An identical repeat re-sends the reply; a changed one is refused.
 * oslog: size in bytes at bits 55:36, IOVA >> 12 at bits 35:0, type at 63:56.
 */
static bool rtkit_mtp_host_buffer(rtkit_dev_t *rtk, u8 ep, struct rtkit_buffer *bfr, size_t sz,
                                  bool oslog)
{
    if (!sz || !rtk->mtp_alloc)
        return false;
    if (bfr->bfr) {
        if (bfr->sz != sz) {
            printf("rtkit(mtp): buffer request ep 0x%x size 0x%lx while 0x%lx is live: refused\n", ep,
                   (unsigned long)sz, (unsigned long)bfr->sz);
            return false;
        }
    } else {
        void *cpu;
        u64 iova;
        if (!rtk->mtp_alloc(sz, &cpu, &iova) || (iova & ~GENMASK(43, 0)) || (oslog && (iova & 0xfff))) {
            printf("rtkit(mtp): host buffer for ep 0x%x size 0x%lx: allocation/map failed\n", ep,
                   (unsigned long)sz);
            return false;
        }
        bfr->bfr = cpu;
        bfr->dva = iova;
        bfr->sz = sz;
        rtkit_printf("host buffer ep 0x%x size 0x%lx at iova 0x%lx (Linux: dma_alloc_coherent via dart-mtp)\n",
                     ep, (unsigned long)sz, (unsigned long)iova);
    }
    struct asc_message reply;
    reply.msg1 = ep;
    if (oslog)
        reply.msg0 = (1ULL << 56) | ((u64)(sz & 0xfffff) << 36) | ((bfr->dva >> 12) & GENMASK(35, 0));
    else
        reply.msg0 = FIELD_PREP(MGMT_TYPE, MSG_BUFFER_REQUEST) |
                     FIELD_PREP(MSG_BUFFER_REQUEST_SIZE, sz >> 12) | (bfr->dva & GENMASK(43, 0));
    if (!rtkit_mail_send(rtk, &reply)) {
        rtkit_printf("unable to send host buffer reply\n");
        return false;
    }
    return true;
}

static bool rtkit_mail_send(rtkit_dev_t *rtk, const struct asc_message *msg)
{
    if (!rtk->mtp)
        return asc_send(rtk->asc, msg);
    if (rtk->tx_pending)
        return false;
    if (asc_try_send(rtk->asc, msg))
        return true;
    rtk->pending_tx = *msg;
    rtk->tx_pending = true;
    rtk->tx_deadline = timeout_calculate(500000);
    return true;
}

bool rtkit_send(rtkit_dev_t *rtk, const struct rtkit_message *msg)
{
    struct asc_message asc_msg;

    asc_msg.msg0 = msg->msg;
    asc_msg.msg1 = msg->ep;

    return rtkit_mail_send(rtk, &asc_msg);
}

bool rtkit_map(rtkit_dev_t *rtk, void *phys, size_t sz, u64 *dva)
{
    sz = ALIGN_UP(sz, 16384);

    if (rtk->sart) {
        if (!sart_add_allowed_region(rtk->sart, phys, sz)) {
            rtkit_printf("sart_add_allowed_region failed (%p, 0x%lx)\n", phys, sz);
            return false;
        }
        *dva = (u64)phys;
        return true;
    } else if (rtk->dart) {
        u64 iova = iova_alloc(rtk->dart_iovad, sz);
        if (!iova) {
            rtkit_printf("failed to alloc iova (size 0x%lx)\n", sz);
            return false;
        }

        if (dart_map(rtk->dart, iova, phys, sz) < 0) {
            rtkit_printf("failed to DART map %p -> 0x%lx (0x%lx)\n", phys, iova, sz);
            iova_free(rtk->dart_iovad, iova, sz);
            return false;
        }

        *dva = iova | rtk->dva_base;
        return true;
    } else {
        rtkit_printf("TODO: implement no IOMMU buffers\n");
        return false;
    }
}

bool rtkit_unmap(rtkit_dev_t *rtk, u64 dva, size_t sz)
{
    if (rtk->sart) {
        if (!sart_remove_allowed_region(rtk->sart, (void *)dva, sz))
            rtkit_printf("sart_remove_allowed_region failed (0x%lx, 0x%lx)\n", dva, sz);
        return true;
    } else if (rtk->dart) {
        dva &= ~rtk->dva_base;
        dart_unmap(rtk->dart, dva & IOVA_MASK, sz);
        iova_free(rtk->dart_iovad, dva & IOVA_MASK, sz);
        return true;
    } else {
        rtkit_printf("TODO: implement no IOMMU buffers\n");
        return false;
    }
}

bool rtkit_alloc_buffer(rtkit_dev_t *rtk, struct rtkit_buffer *bfr, size_t sz)
{
    bfr->bfr = memalign(SZ_16K, sz);
    if (!bfr->bfr) {
        rtkit_printf("unable to allocate %zu buffer\n", sz);
        return false;
    }

    sz = ALIGN_UP(sz, 16384);

    bfr->sz = sz;
    if (!rtkit_map(rtk, bfr->bfr, sz, &bfr->dva))
        goto error;

    return true;

error:
    free(bfr->bfr);
    bfr->bfr = NULL;
    return false;
}

bool rtkit_free_buffer(rtkit_dev_t *rtk, struct rtkit_buffer *bfr)
{
    if (!bfr->bfr || !is_heap(bfr->bfr))
        return true;

    if (!rtkit_unmap(rtk, bfr->dva, bfr->sz))
        return false;

    free(bfr->bfr);

    return false;
}

static bool rtkit_handle_buffer_request(rtkit_dev_t *rtk, struct rtkit_message *msg,
                                        struct rtkit_buffer *bfr)
{
    size_t n_4kpages = FIELD_GET(MSG_BUFFER_REQUEST_SIZE, msg->msg);
    size_t sz = n_4kpages << 12;
    u64 addr = rtk->mtp ? (msg->msg & GENMASK(43, 0)) :
                         FIELD_GET(MSG_BUFFER_REQUEST_IOVA, msg->msg);

    if (rtk->sram) {
        if (rtk->mtp && !addr && rtk->mtp_alloc)
            return rtkit_mtp_host_buffer(rtk, msg->ep, bfr, sz, false);
        if (rtk->mtp && (!sz || addr < rtk->sram_base || sz > rtk->sram_size ||
                         addr - rtk->sram_base > rtk->sram_size - sz)) {
            printf("rtkit(mtp): buffer request ep 0x%x iova 0x%lx size 0x%lx not inside MTP SRAM: "
                   "refused (decision sram-buffers; Linux would DART-map host memory for iova 0)\n",
                   msg->ep, (unsigned long)addr, (unsigned long)sz);
            return false;
        }
        if (rtk->mtp)
            rtkit_printf("buffer ep 0x%x in SRAM iova 0x%lx size 0x%lx (no reply, as Linux)\n", msg->ep,
                         (unsigned long)addr, (unsigned long)sz);
        if (!addr) {
            rtkit_printf("SRAM buffers needs to be provided by the IOP\n");
            return false;
        }
        bfr->dva = addr;
        bfr->bfr = (void *)addr;
        bfr->sz = sz;
        return true;
    } else if (addr) {
        bfr->dva = addr & ~rtk->dva_base;
        bfr->sz = sz;
        bfr->bfr = dart_translate(rtk->dart, bfr->dva & IOVA_MASK);
        if (!bfr->bfr) {
            rtkit_printf("failed to translate pre-allocated buffer (ep 0x%x, buf 0x%lx)\n", msg->ep,
                         addr);
            return false;
        } else {
            rtkit_printf("pre-allocated buffer (ep 0x%x, dva 0x%lx, phys %p)\n", msg->ep, addr,
                         bfr->bfr);
        }
        return true;

    } else {
        if (!rtkit_alloc_buffer(rtk, bfr, sz)) {
            rtkit_printf("unable to allocate buffer\n");
            return false;
        }
    }

    struct asc_message reply;
    reply.msg1 = msg->ep;
    reply.msg0 = FIELD_PREP(MGMT_TYPE, MSG_BUFFER_REQUEST);
    reply.msg0 |= FIELD_PREP(MSG_BUFFER_REQUEST_SIZE, n_4kpages);
    if (!addr)
        reply.msg0 |= FIELD_PREP(MSG_BUFFER_REQUEST_IOVA, bfr->dva | rtk->dva_base);

    if (!rtkit_mail_send(rtk, &reply)) {
        rtkit_printf("unable to send buffer reply\n");
        rtkit_free_buffer(rtk, bfr);
        goto error;
    }

    return true;

error:
    return false;
}

static void rtkit_crashed(rtkit_dev_t *rtk)
{
    struct crashlog_hdr *hdr = rtk->crashlog_bfr.bfr;
    rtk->crashed = true;

    rtkit_printf("IOP crashed!\n");

    if (hdr->type != 'CLHE') {
        rtkit_printf("bad crashlog header 0x%x @ %p\n", hdr->type, hdr);
        return;
    }

    struct crashlog_entry *p = (void *)(hdr + 1);

    rtkit_printf("== CRASH INFO ==\n");
    while (p->type != 'CLHE') {
        switch (p->type) {
            case 'Cstr':
                rtkit_printf("  Message %d: %s\n", p->payload[0], &p->payload[4]);
                break;
            default:
                rtkit_printf("  0x%x\n", p->type);
                break;
        }
        p = ((void *)p) + p->len;
    }
}

bool rtkit_can_recv(rtkit_dev_t *rtk)
{
    if (rtk->crashed)
        return false;

    return asc_can_recv(rtk->asc);
}

int rtkit_recv(rtkit_dev_t *rtk, struct rtkit_message *msg)
{
    struct asc_message asc_msg;
    bool ok = true;

    if (rtk->crashed)
        return -1;

    unsigned budget = 8;
    u64 deadline = rtk->mtp ? timeout_calculate(50) : 0;
    if (rtk->mtp && rtk->tx_pending) {
        if (!asc_try_send(rtk->asc, &rtk->pending_tx))
            return timeout_expired(rtk->tx_deadline) ? -1 : 0;
        rtk->tx_pending = false;
    }
    while ((!rtk->mtp || (budget-- && !timeout_expired(deadline) && !rtk->tx_pending)) &&
           asc_recv(rtk->asc, &asc_msg)) {
        if (asc_msg.msg1 >= 0x100) {
            rtkit_printf("WARNING: received message for invalid endpoint %x >= 0x100\n",
                         asc_msg.msg1);
            continue;
        }

        msg->msg = asc_msg.msg0;
        msg->ep = (u8)asc_msg.msg1;

        /* if this is an app message we can just forward it to the caller */
        if (msg->ep >= 0x20)
            return rtk->mtp ? 0 : 1;

        u32 msgtype = FIELD_GET(MGMT_TYPE, msg->msg);
        switch (msg->ep) {
            case RTKIT_EP_MGMT:
                switch (msgtype) {
                    case MGMT_MSG_IOP_PWR_STATE_ACK:
                        rtk->iop_power = FIELD_GET(MGMT_PWR_STATE, msg->msg);
                        break;
                    case MGMT_MSG_AP_PWR_STATE_ACK:
                        rtk->ap_power = FIELD_GET(MGMT_PWR_STATE, msg->msg);
                        break;
                    default:
                        rtkit_printf("unknown management message %x\n", msgtype);
                }
                break;
            case RTKIT_EP_SYSLOG:
                switch (msgtype) {
                    case MSG_BUFFER_REQUEST:
                        ok = ok && rtkit_handle_buffer_request(rtk, msg, &rtk->syslog_bfr);
                        break;
                    case MSG_SYSLOG_INIT:
                        rtk->syslog_cnt = FIELD_GET(MSG_SYSLOG_INIT_COUNT, msg->msg);
                        rtk->syslog_size = FIELD_GET(MSG_SYSLOG_INIT_ENTRYSIZE, msg->msg);
                        break;
                    case MSG_SYSLOG_LOG:
#ifdef RTKIT_SYSLOG
                    {
                        if (rtk->mtp) goto skip_mtp_log;
                        u64 index = FIELD_GET(MSG_SYSLOG_LOG_INDEX, msg->msg);
                        u64 stride = rtk->syslog_size + sizeof(struct syslog_log);
                        struct syslog_log *log = rtk->syslog_bfr.bfr + stride * index;
                        rtkit_printf("syslog: [%s]%s", log->context, log->msg);
                        if (log->msg[strlen(log->msg) - 1] != '\n')
                            printf("\n");
                    }
skip_mtp_log:;
#endif
                        if (!rtkit_mail_send(rtk, &asc_msg)) {
                            if (rtk->mtp) return -1;
                            rtkit_printf("failed to ack syslog\n");
                        }
                        break;
                    default:
                        rtkit_printf("unknown syslog message %x\n", msgtype);
                }
                break;
            case RTKIT_EP_CRASHLOG:
                switch (msgtype) {
                    case MSG_BUFFER_REQUEST:
                        if (!rtk->crashlog_bfr.bfr) {
                            ok = ok && rtkit_handle_buffer_request(rtk, msg, &rtk->crashlog_bfr);
                        } else {
                            if (rtk->mtp) {
                                printf("rtkit(mtp): coprocessor CRASHED (second crashlog message)\n");
                                rtk->crashed = true;
                                return -1;
                            }
                            rtkit_crashed(rtk);
                            return -1;
                        }
                        break;
                    default:
                        rtkit_printf("unknown crashlog message %x\n", msgtype);
                }
                break;
            case RTKIT_EP_IOREPORT:
                switch (msgtype) {
                    case MSG_BUFFER_REQUEST:
                        ok = ok && rtkit_handle_buffer_request(rtk, msg, &rtk->ioreport_bfr);
                        break;
                    /* unknown but must be ACKed */
                    case 0x8:
                    case 0xc:
                        if (!rtkit_send(rtk, msg))
                            rtkit_printf("unable to ACK unknown ioreport message\n");
                        break;
                    default:
                        rtkit_printf("unknown ioreport message %x\n", msgtype);
                }
                break;
            case RTKIT_EP_OSLOG:
                if (rtk->mtp) {
                    u32 type = msg->msg >> 56;
                    if (type == 1) {
                        u64 addr = (msg->msg & 0xfffffffffULL) << 12;
                        u64 size = (msg->msg >> 36) & 0xfffff;
                        if (!addr && rtk->mtp_alloc) {
                            if (!rtkit_mtp_host_buffer(rtk, msg->ep, &rtk->oslog_bfr, size, true))
                                return -1;
                            rtk->oslog_buffer = true;
                            break;
                        }
                        if (!size || addr < rtk->sram_base || size > rtk->sram_size ||
                            addr - rtk->sram_base > rtk->sram_size - size)
                            return -1;
                        rtk->oslog_buffer = true; /* firmware-owned: no reply */
                    } else if (type == 2 && rtk->oslog_buffer &&
                               !(msg->msg & 0x00ffffff00000000ULL)) {
                        if (!rtkit_mail_send(rtk, &asc_msg)) return -1;
                    }
                    break;
                }
                rtkit_printf("unknown oslog message %lx\n", msg->msg);
                break;
            default:
                if (rtk->mtp) break;
                rtkit_printf("message to unknown system endpoint 0x%02x: %lx\n", msg->ep, msg->msg);
        }

        if (!ok) {
            rtkit_printf("failed to handle system message 0x%02x: %lx\n", msg->ep, msg->msg);
            return -1;
        }
    }

    return 0;
}

bool rtkit_start_ep(rtkit_dev_t *rtk, u8 ep)
{
    struct asc_message msg;

    msg.msg0 = FIELD_PREP(MGMT_TYPE, MGMT_MSG_START_EP);
    msg.msg0 |= MGMT_MSG_START_EP_FLAG;
    msg.msg0 |= FIELD_PREP(MGMT_MSG_START_EP_IDX, ep);
    msg.msg1 = RTKIT_EP_MGMT;

    if (!rtkit_mail_send(rtk, &msg)) {
        rtkit_printf("unable to start endpoint 0x%02x\n", ep);
        return false;
    }

    return true;
}

/* rtkit_boot()'s wait for HELLO and for each endpoint-map message */
static u64 rtkit_boot_timeout = USEC_PER_SEC;

void rtkit_set_boot_timeout(u64 us)
{
    rtkit_boot_timeout = us ? us : USEC_PER_SEC;
}

/* r3: opt-in bound (microseconds) for the power-state waits in rtkit_boot() and
 * rtkit_switch_power_state(). 0 (the default) keeps the original unbounded waits,
 * so every existing user (DCP, SMC, stage-1 NVMe) is unchanged. */
static u64 rtkit_power_timeout = 0;

void rtkit_set_power_timeout(u64 us)
{
    rtkit_power_timeout = us;
}

static bool rtkit_power_wait_expired(u64 deadline)
{
    return rtkit_power_timeout && timeout_expired(deadline);
}

static bool rtkit_boot_inner(rtkit_dev_t *rtk);
/* r3.5: which bounded wait timed out (RTKIT_TIMEOUT_* bits), so a caller can tell the HELLO,
 * endpoint-map and power-state waits apart without a serial log. Cleared by
 * rtkit_take_timeouts(); never read by the original m1n1 users. */
static u32 rtkit_timeouts;

u32 rtkit_take_timeouts(void)
{
    u32 t = rtkit_timeouts;
    rtkit_timeouts = 0;
    return t;
}

bool rtkit_boot(rtkit_dev_t *rtk)
{
    /* MTP: boot-time diagnostics only (preguest); runtime stays silent. */
    rtk->mtp_boot_log = rtk->mtp;
    bool ok = rtkit_boot_inner(rtk);
    if (rtk->mtp)
        printf("rtkit(mtp): boot %s (iop_power=%#x ap_power=%#x)\n", ok ? "OK" : "FAILED",
               rtk->iop_power, rtk->ap_power);
    rtk->mtp_boot_log = false;
    return ok;
}

static bool rtkit_boot_inner(rtkit_dev_t *rtk)
{
    struct asc_message msg;

    /* boot the IOP if it isn't already */
    if (!rtk->mtp) asc_cpu_start(rtk->asc);
    /* can be sent unconditionally to wake up a possibly sleeping IOP */
    msg.msg0 = FIELD_PREP(MGMT_TYPE, MGMT_MSG_IOP_PWR_STATE) |
               FIELD_PREP(MGMT_PWR_STATE, RTKIT_POWER_INIT);
    msg.msg1 = RTKIT_EP_MGMT;
    if (!rtkit_mail_send(rtk, &msg)) {
        rtkit_printf("unable to send wakeup message\n");
        return false;
    }

    if (!asc_recv_timeout(rtk->asc, &msg, rtk->mtp ? 1000000 : rtkit_boot_timeout)) {
        rtkit_timeouts |= RTKIT_TIMEOUT_HELLO;
        rtkit_printf("did not receive HELLO\n");
        return false;
    }

    if (msg.msg1 != RTKIT_EP_MGMT) {
        rtkit_printf("expected HELLO but got message for EP 0x%x", msg.msg1);
        return false;
    }

    u32 msgtype;
    msgtype = FIELD_GET(MGMT_TYPE, msg.msg0);
    if (msgtype != MGMT_MSG_HELLO) {
        rtkit_printf("expected HELLO but got message with type 0x%02x", msgtype);

        return false;
    }

    u32 min_ver, max_ver, want_ver;
    min_ver = FIELD_GET(MGMT_MSG_HELLO_MINVER, msg.msg0);
    max_ver = FIELD_GET(MGMT_MSG_HELLO_MAXVER, msg.msg0);
    want_ver = min(RTKIT_MAX_VERSION, max_ver);

    if ((rtk->mtp && min_ver > max_ver) || min_ver > RTKIT_MAX_VERSION || max_ver < RTKIT_MIN_VERSION) {
        rtkit_printf("supported versions [%d,%d] must overlap versions [%d,%d]\n",
                     RTKIT_MIN_VERSION, RTKIT_MAX_VERSION, min_ver, max_ver);
        return false;
    }

    rtkit_printf("booting with version %d\n", want_ver);

    msg.msg0 = FIELD_PREP(MGMT_TYPE, MGMT_MSG_HELLO_ACK);
    msg.msg0 |= FIELD_PREP(MGMT_MSG_HELLO_MINVER, want_ver);
    msg.msg0 |= FIELD_PREP(MGMT_MSG_HELLO_MAXVER, want_ver);
    msg.msg1 = RTKIT_EP_MGMT;
    if (!rtkit_mail_send(rtk, &msg)) {
        rtkit_printf("couldn't send HELLO ack\n");
        return false;
    }

    bool has_crashlog = false;
    bool has_debug = false;
    bool has_ioreport = false;
    bool has_syslog = false;
    bool has_oslog = false;
    bool has_analytics = false;
    u64 mtp_boot_deadline = timeout_calculate(5000000);
    bool got_epmap = false;
    while (!got_epmap) {
        if (rtk->mtp && timeout_expired(mtp_boot_deadline)) {
            rtkit_printf("endpoint map not complete within 5 s\n");
            return false;
        }
        if (!asc_recv_timeout(rtk->asc, &msg, rtk->mtp ? 1000000 : rtkit_boot_timeout)) {
            rtkit_timeouts |= RTKIT_TIMEOUT_EPMAP;
            rtkit_printf("couldn't receive message while waiting for endpoint map\n");
            return false;
        }

        if (msg.msg1 != RTKIT_EP_MGMT) {
            rtkit_printf("expected management message while waiting for endpoint map but got "
                         "message for endpoint 0x%x\n",
                         msg.msg1);
            return false;
        }

        msgtype = FIELD_GET(MGMT_TYPE, msg.msg0);
        if (msgtype != MGMT_MSG_EPMAP) {
            rtkit_printf("expected endpoint map message but got 0x%x instead\n", msgtype);
            return false;
        }

        u32 bitmap = FIELD_GET(MGMT_MSG_EPMAP_BITMAP, msg.msg0);
        u32 base = FIELD_GET(MGMT_MSG_EPMAP_BASE, msg.msg0);
        for (unsigned int i = 0; i < 32; i++) {
            if (bitmap & (1U << i)) {
                u8 ep_idx = 32 * base + i;

                if (ep_idx >= 0x20)
                    continue;
                switch (ep_idx) {
                    case RTKIT_EP_CRASHLOG:
                        has_crashlog = true;
                        break;
                    case RTKIT_EP_DEBUG:
                        has_debug = true;
                        break;
                    case RTKIT_EP_IOREPORT:
                        has_ioreport = true;
                        break;
                    case RTKIT_EP_SYSLOG:
                        has_syslog = true;
                        break;
                    case RTKIT_EP_OSLOG:
                        has_oslog = true;
                    case RTKIT_EP_MGMT:
                        break;
                    case 0xa:
                        /* u1: non-MTP IOPs (ANS) keep the original "unknown system endpoint" line */
                        if (!rtk->mtp) {
                            rtkit_printf("unknown system endpoint 0x%02x\n", ep_idx);
                            break;
                        }
                        has_analytics = true;
                        break;
                    default:
                        rtkit_printf("unknown system endpoint 0x%02x\n", ep_idx);
                }
            }
        }

        if (msg.msg0 & MGMT_MSG_EPMAP_DONE)
            got_epmap = true;

        msg.msg0 = FIELD_PREP(MGMT_TYPE, MGMT_MSG_EPMAP_REPLY);
        msg.msg0 |= FIELD_PREP(MGMT_MSG_EPMAP_BASE, base);
        if (got_epmap)
            msg.msg0 |= MGMT_MSG_EPMAP_REPLY_DONE;
        else
            msg.msg0 |= MGMT_MSG_EPMAP_REPLY_MORE;

        msg.msg1 = RTKIT_EP_MGMT;

        if (!rtkit_mail_send(rtk, &msg)) {
            rtkit_printf("couldn't reply to endpoint map\n");
            return false;
        }
    }

    if (rtk->mtp) {
        /* u1: MTP-only diagnostics, so the ANS (storage) boot prints exactly what r4.14 printed */
        rtkit_printf("endpoint map: crashlog=%d syslog=%d debug=%d ioreport=%d oslog=%d 0xa=%d\n",
                     has_crashlog, has_syslog, has_debug, has_ioreport, has_oslog, has_analytics);
        /* Linux rtkit.c starts system endpoints in ascending order: 1,2,3,4,8,0xa. */
        if ((has_crashlog && !rtkit_start_ep(rtk, RTKIT_EP_CRASHLOG)) ||
            (has_syslog && !rtkit_start_ep(rtk, RTKIT_EP_SYSLOG)) ||
            (has_debug && !rtkit_start_ep(rtk, RTKIT_EP_DEBUG)) ||
            (has_ioreport && !rtkit_start_ep(rtk, RTKIT_EP_IOREPORT)) ||
            (has_oslog && !rtkit_start_ep(rtk, RTKIT_EP_OSLOG)) ||
            (has_analytics && !rtkit_start_ep(rtk, 0xa)))
            return false;
        has_debug = has_crashlog = has_syslog = has_ioreport = has_oslog = has_analytics = false;
    }
    /* start all required system endpoints */
    if (has_debug && !rtkit_start_ep(rtk, RTKIT_EP_DEBUG))
        return false;
    if (has_crashlog && !rtkit_start_ep(rtk, RTKIT_EP_CRASHLOG))
        return false;
    if (has_syslog && !rtkit_start_ep(rtk, RTKIT_EP_SYSLOG))
        return false;
    if (has_ioreport && !rtkit_start_ep(rtk, RTKIT_EP_IOREPORT))
        return false;
    if (has_oslog && !rtkit_start_ep(rtk, RTKIT_EP_OSLOG))
        return false;

    if (has_analytics && !rtkit_start_ep(rtk, 0xa)) return false;
    u64 power_deadline = timeout_calculate(rtk->mtp ? 5000000 : rtkit_power_timeout);
    while (rtk->iop_power != RTKIT_POWER_ON) {
        struct rtkit_message rtk_msg;
        if ((rtk->mtp && timeout_expired(power_deadline)) || rtkit_power_wait_expired(power_deadline)) {
            rtkit_timeouts |= RTKIT_TIMEOUT_IOP_POWER_ON;
            rtkit_printf("timed out waiting for IOP power on\n");
            return false;
        }
        int ret = rtkit_recv(rtk, &rtk_msg);
        if (ret == 1)
            rtkit_printf("unexpected message to non-system endpoint 0x%02x during boot: %lx\n",
                         rtk_msg.ep, rtk_msg.msg);
        else if (ret < 0)
            return false;
    }

    if (rtk->mtp)
        rtkit_printf("IOP power ON\n");
    /* this enables syslog */
    msg.msg0 =
        FIELD_PREP(MGMT_TYPE, MGMT_MSG_AP_PWR_STATE) | FIELD_PREP(MGMT_PWR_STATE, RTKIT_POWER_ON);
    msg.msg1 = RTKIT_EP_MGMT;
    if (!rtkit_mail_send(rtk, &msg)) {
        rtkit_printf("unable to send AP power message\n");
        return false;
    }

    if (rtk->mtp) {
        u64 ap_deadline = timeout_calculate(5000000);
        while (rtk->ap_power != RTKIT_POWER_ON) {
            struct rtkit_message received;
            if (timeout_expired(ap_deadline) || rtkit_recv(rtk, &received) < 0) {
                rtkit_printf("AP power ON not acknowledged (timeout 5 s or receive error)\n");
                return false;
            }
        }
    }
    return true;
}

static bool rtkit_switch_power_state(rtkit_dev_t *rtk, enum rtkit_power_state target)
{
    struct asc_message msg;

    if (rtk->crashed)
        return false;

    /* AP power should always go to QUIESCED, otherwise rebooting doesn't work */
    msg.msg0 = FIELD_PREP(MGMT_TYPE, MGMT_MSG_AP_PWR_STATE) |
               FIELD_PREP(MGMT_PWR_STATE, RTKIT_POWER_QUIESCED);
    msg.msg1 = RTKIT_EP_MGMT;
    if (!rtkit_mail_send(rtk, &msg)) {
        rtkit_printf("unable to send shutdown message\n");
        return false;
    }

    u64 power_deadline = timeout_calculate(rtkit_power_timeout);
    while (rtk->ap_power != RTKIT_POWER_QUIESCED) {
        struct rtkit_message rtk_msg;
        if ((rtk->mtp && timeout_expired(power_deadline)) || rtkit_power_wait_expired(power_deadline)) {
            rtkit_timeouts |= RTKIT_TIMEOUT_AP_QUIESCE;
            rtkit_printf("timed out waiting for AP power quiesce\n");
            return false;
        }
        int ret = rtkit_recv(rtk, &rtk_msg);

        if (ret > 0) {
            rtkit_printf("unexpected message to non-system endpoint 0x%02x during shutdown: %lx\n",
                         rtk_msg.ep, rtk_msg.msg);
            continue;
        } else if (ret < 0) {
            rtkit_printf("IOP died during shutdown\n");
            return false;
        }
    }

    msg.msg0 = FIELD_PREP(MGMT_TYPE, MGMT_MSG_IOP_PWR_STATE) | FIELD_PREP(MGMT_PWR_STATE, target);
    if (!rtkit_mail_send(rtk, &msg)) {
        rtkit_printf("unable to send shutdown message\n");
        return false;
    }

    power_deadline = timeout_calculate(rtkit_power_timeout);
    while (rtk->iop_power != target) {
        struct rtkit_message rtk_msg;
        if ((rtk->mtp && timeout_expired(power_deadline)) || rtkit_power_wait_expired(power_deadline)) {
            rtkit_timeouts |= RTKIT_TIMEOUT_IOP_POWER_STATE;
            rtkit_printf("timed out waiting for IOP power state\n");
            return false;
        }
        int ret = rtkit_recv(rtk, &rtk_msg);

        if (ret > 0) {
            rtkit_printf("unexpected message to non-system endpoint 0x%02x during shutdown: %lx\n",
                         rtk_msg.ep, rtk_msg.msg);
            continue;
        } else if (ret < 0) {
            rtkit_printf("IOP died during shutdown\n");
            return false;
        }
    }

    return true;
}

bool rtkit_quiesce(rtkit_dev_t *rtk)
{
    if (rtk->mtp) return false;
    return rtkit_switch_power_state(rtk, RTKIT_POWER_QUIESCED);
}

bool rtkit_sleep(rtkit_dev_t *rtk)
{
    if (rtk->mtp) return false;
    int ret = rtkit_switch_power_state(rtk, RTKIT_POWER_SLEEP);
    if (ret < 0)
        return ret;

    asc_cpu_stop(rtk->asc);
    return 0;
}
