/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EZ_CAM_HALOW_GPS_H
#define EZ_CAM_HALOW_GPS_H
#include "liblwm2m.h"
#include <stdbool.h>
extern lwm2m_object_t halow_gps_location, halow_gps_control;
int halow_gps_init(void);
/* Wakaama thread only. False cancels acquisition and powers the receiver off. */
int halow_gps_step(lwm2m_context_t *ctx, bool allow_acquisition);
#endif
