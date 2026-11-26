/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EZ_DEV54_HALOW_GPS_H
#define EZ_DEV54_HALOW_GPS_H
#include "liblwm2m.h"
#include <stdbool.h>
#include "gps_nmea.h"
extern lwm2m_object_t halow_gps_location, halow_gps_control;
int halow_gps_init(void);
/* False while acquiring; otherwise the completed request result and fix. */
bool halow_gps_result(struct gps_fix *out, int *result);
/* Wakaama thread only. False cancels acquisition and powers the receiver off. */
int halow_gps_step(lwm2m_context_t *ctx, bool allow_acquisition);
#endif
