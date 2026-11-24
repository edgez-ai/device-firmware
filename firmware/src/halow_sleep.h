/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EZ_DEV54_HALOW_SLEEP_H
#define EZ_DEV54_HALOW_SLEEP_H
#include "liblwm2m.h"
#include <stdbool.h>
extern lwm2m_object_t halow_sleep;
/* All object callbacks and step calls run on the Wakaama thread. */
/* Bracket native packet handling to deduplicate Execute retransmissions. */
void halow_sleep_request_begin(void *peer, const uint8_t *packet, size_t length);
void halow_sleep_request_end(void);
void halow_sleep_forget_requests(void);
bool halow_sleep_pending(void);
bool halow_sleep_active(void);
/* Advance the deferred Execute/timed wake; -1 means no deadline. */
int halow_sleep_step(lwm2m_context_t *ctx, bool online);
/* Platform boundary: keep association, suspend IP TX, negotiate WNM powerdown.
 * Results: 0 success, positive mmwlan_status, negative local errno. */
int halow_wnm_enter(void);
int halow_wnm_exit(void);
#endif
