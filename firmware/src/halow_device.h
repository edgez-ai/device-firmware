/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EZ_DEV54_HALOW_DEVICE_H
#define EZ_DEV54_HALOW_DEVICE_H
#include "liblwm2m.h"
extern lwm2m_object_t halow_device;
/* Called only on the Wakaama thread after accepting gateway time metadata. */
void halow_device_sync_clock(lwm2m_context_t *ctx, int64_t unix_ms);
/* No deadline without an active Current Time (or enclosing Device) Observe. */
int halow_device_step(lwm2m_context_t *ctx);
#endif
