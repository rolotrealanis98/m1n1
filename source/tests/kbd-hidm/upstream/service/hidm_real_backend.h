/* SPDX-License-Identifier: MIT */
#ifndef HIDM_REAL_BACKEND_H
#define HIDM_REAL_BACKEND_H
#include "hidm_service.h"
/* INTEGRATION CONTRACT ONLY: deliberately no transport/hardware implementation.
 * Owner supplies complete stable descriptors and metadata after MTP INIT/READY.
 * Exclude comm/stm/actuator interfaces unless deliberately intended for HIDCLASS.
 * Keep descriptor storage alive until service teardown. Push channel 0x12 input
 * message bytes verbatim (including declared report ID), not FIFO framing.
 * Serialize callbacks and service accesses under hypervisor's device lock.
 * Report callback must not wait for physical ACK in a trapped guest MMIO store:
 * accept a bounded queue entry or return HM_DENIED; do not claim physical ACK.
 * GET_FEATURE needs a cached reply or returns HM_DENIED. MTP lifecycle, RTKit,
 * DAPF/DART, GPIO, firmware and translation for PTP all belong to that backend.
 */
#endif
