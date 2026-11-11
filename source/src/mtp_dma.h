/* SPDX-License-Identifier: MIT */
#ifndef MTP_DMA_H
#define MTP_DMA_H
/* includer provides types.h (u64, size_t, bool) */
enum mtp_dma_mode { MTP_DMA_OFF, MTP_DMA_BYPASS, MTP_DMA_IBOOT_TABLES, MTP_DMA_OWN_TABLES };
/* Preguest, before the MTP CPU starts. Idempotent. */
enum mtp_dma_mode mtp_dma_init(void);
/* Zeroed, 16 KiB-aligned resident memory mapped for the MTP; *iova is the DVA to
 * hand to the coprocessor. Never freed (the firmware may keep using it). */
bool mtp_dma_alloc(size_t sz, void **cpu, u64 *iova);
enum mtp_dma_mode mtp_dma_mode(void);
struct rtkit_dev;
void mtp_dma_rtkit_hook(struct rtkit_dev *rtk);
#endif
