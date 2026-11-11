/* SPDX-License-Identifier: MIT */
#ifndef HIDM_ABI_H
#define HIDM_ABI_H
#if defined(_MSC_VER)
typedef unsigned __int8 hm_u8;
typedef unsigned __int16 hm_u16;
typedef unsigned __int32 hm_u32;
typedef unsigned __int64 hm_u64;
#else
typedef unsigned char hm_u8;
typedef unsigned short hm_u16;
typedef unsigned int hm_u32;
typedef unsigned long long hm_u64;
#endif
#define HM_MAGIC 0x4d444948u /* HIDM */
#define HM_VERSION 1u
#define HM_PAGE 4096u
#define HM_MAX_IFACES 8u
#define HM_MAX_DESC 4096u
#define HM_PAYLOAD 256u
#define HM_SLOTS 8u
#define HM_REG_BASE 0xf38100000ull
#define HM_DATA_BASE 0xf38104000ull
#define HM_GSIV 68u
#define HM_SPI 36u
/* Register page, aligned 32-bit little-endian accesses only. */
#define HM_R_MAGIC 0x00u
#define HM_R_VERSION 0x04u
#define HM_R_READY 0x08u
#define HM_R_COUNT 0x0cu
#define HM_R_HEAD 0x10u
#define HM_R_TAIL 0x14u /* RW: validated acknowledgement */
#define HM_R_OVERFLOW 0x18u
#define HM_R_ENABLE 0x1cu /* RW bit 0 */
#define HM_R_DOORBELL 0x20u /* WO sequence; synchronous command */
#define HM_R_IRQ 0x24u /* RO: head != tail, independent of mask */
#define HM_REQ_OFF 0u
#define HM_RESP_OFF 288u
#define HM_RING_OFF 768u
#define HM_SLOT_BYTES 272u
#define HM_PRESENT 1u
/* All structures have naturally aligned fixed-width members; no pointers. */
typedef struct hm_info {
    hm_u32 id, flags;
    hm_u16 vendor, product, version;
    hm_u8 country, reserved;
    hm_u32 descriptor_length;
    hm_u32 reserved2;
} hm_info;
typedef struct hm_request {
    hm_u32 seq, abi, op, iface;
    hm_u32 offset, length, report_id, reserved;
    hm_u8 payload[HM_PAYLOAD];
} hm_request;
typedef struct hm_response {
    hm_u32 seq, status, length, reserved;
    hm_u8 payload[HM_PAYLOAD];
    hm_u32 pad[4];
} hm_response;
typedef struct hm_slot {
    hm_u32 iface, length;
    hm_u64 timestamp_ns;
    hm_u8 payload[HM_PAYLOAD];
} hm_slot;
#define HM_HELLO 1u
#define HM_ENUM 2u /* iface is ordinal, response hm_info */
#define HM_DESCRIPTOR 3u
#define HM_OUTPUT 4u
#define HM_GET_FEATURE 5u
#define HM_SET_FEATURE 6u
#define HM_ACTIVATE 7u
#define HM_DEACTIVATE 8u
#define HM_BYE 9u
#define HM_OK 0u
#define HM_BAD_VERSION 1u
#define HM_BAD_REQUEST 2u
#define HM_NOT_READY 3u
#define HM_NOT_FOUND 4u
#define HM_RANGE 5u
#define HM_DENIED 6u
#define HM_FULL 7u
#define HM_IO_ERROR 8u
#define HM_PENDING 0xffffffffu
#define HM_ASSERT2(c,n) typedef char hm_assert_##n[(c)?1:-1]
#define HM_ASSERT1(c,n) HM_ASSERT2(c,n)
#define HM_ASSERT(c) HM_ASSERT1(c,__LINE__)
HM_ASSERT(sizeof(hm_info)==24);
HM_ASSERT(sizeof(hm_request)==288);
HM_ASSERT(sizeof(hm_response)==288);
HM_ASSERT(sizeof(hm_slot)==HM_SLOT_BYTES);
HM_ASSERT(HM_RING_OFF+HM_SLOTS*HM_SLOT_BYTES<=HM_PAGE);
/* Local byte utilities; also used for wire encoding on non-LE test hosts. */
static inline void hm_zero(void *p, hm_u32 n) { hm_u8 *b=p; while(n--) *b++=0; }
static inline void hm_copy(void *d,const void *s,hm_u32 n) {
    hm_u8 *a=d; const hm_u8 *b=s; while(n--) *a++=*b++;
}
static inline hm_u32 hm_le32(const hm_u8 *b) {
    return (hm_u32)b[0]|((hm_u32)b[1]<<8)|((hm_u32)b[2]<<16)|((hm_u32)b[3]<<24);
}
static inline void hm_put32(hm_u8 *b,hm_u32 v) {
    b[0]=(hm_u8)v;b[1]=(hm_u8)(v>>8);b[2]=(hm_u8)(v>>16);b[3]=(hm_u8)(v>>24);
}
#endif
