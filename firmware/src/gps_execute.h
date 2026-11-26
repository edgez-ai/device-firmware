/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EZ_GPS_EXECUTE_H
#define EZ_GPS_EXECUTE_H
#include "liblwm2m.h"
/* Run on the Wakaama thread after GPS stepping, before a context is freed. */
void gps_execute_step(lwm2m_context_t *ctx);
#endif
