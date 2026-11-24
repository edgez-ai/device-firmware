/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "liblwm2m.h"
/* Dispatch on the LwM2M thread and accept wall time only from a matching,
 * successfully processed registration/update response. */
void halow_time_sync_packet(lwm2m_context_t *ctx, uint8_t *packet, size_t length, void *peer);

void halow_time_sync_reset(void);

void halow_time_sync_sent(void *peer, const uint8_t *packet, size_t length);
