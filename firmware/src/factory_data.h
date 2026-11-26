/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EZ_DEV54_FACTORY_DATA_H
#define EZ_DEV54_FACTORY_DATA_H
#include "factory_record.h"
int ez_factory_init(void);
const struct ez_factory_data *ez_factory_get(void);
int ez_factory_crypto_init(void);
const char *ez_factory_pop(void);
const char *ez_factory_ble_name(void);
const char *ez_factory_device_serial(void);
const char *ez_factory_model(void);
/* -1 for no valid hardware flag; base selection is handled by the caller. */
int ez_factory_hardware(void);
/* Read and validate the optional motion extension on each call. */
uint16_t ez_factory_gyro_threshold_dps(void);
uint8_t ez_factory_pose_angle_deg(void);
#endif
