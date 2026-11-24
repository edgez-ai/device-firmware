/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EZ_CAM_HALOW_SENSORS_H
#define EZ_CAM_HALOW_SENSORS_H
#include "liblwm2m.h"
#include <stdbool.h>
extern lwm2m_object_t halow_accelerometer, halow_gyrometer;
/* Called exclusively by the LwM2M thread. -1 means no sampling deadline. */
int halow_sensors_step(lwm2m_context_t *ctx);
void halow_sensors_internal_read(bool internal);
bool halow_sensors_is_internal_read(void);
#endif
