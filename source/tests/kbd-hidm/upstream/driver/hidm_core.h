/* SPDX-License-Identifier: MIT */
#ifndef HIDM_CORE_H
#define HIDM_CORE_H
#include "../abi/hidm_abi.h"
/* Accessors must implement ordered little-endian MMIO. The write of DOORBELL
 * is synchronous. Caller serializes all core operations, including DPC. */
typedef struct hm_io {void *ctx;hm_u32 (*read)(void *,hm_u32,hm_u32);void (*write)(void *,hm_u32,hm_u32,hm_u32);} hm_io;
typedef struct hm_layout {hm_u16 length[3][256];hm_u8 ids;} hm_layout;
typedef struct hm_device {hm_info info;hm_u8 descriptor[HM_MAX_DESC];hm_layout layout;} hm_device;
typedef struct hm_client {hm_io io;hm_u32 seq,tail,count;hm_device devices[HM_MAX_IFACES];} hm_client;
hm_u32 hm_parse_descriptor(const hm_u8 *,hm_u32,hm_layout *);
hm_u32 hm_client_open(hm_client *,hm_io);
hm_u32 hm_client_command(hm_client *,hm_request *,hm_response *);
hm_u32 hm_client_pop(hm_client *,hm_slot *); /* NOT_READY empty; RANGE corrupt */
hm_u32 hm_client_report(hm_client *,hm_u32,hm_u32,hm_u32,hm_u8 *,hm_u32 *);
hm_u32 hm_copy_output(void *,hm_u32,const void *,hm_u32,hm_u32 *);
int hm_client_index(const hm_client *,hm_u32);
#endif
