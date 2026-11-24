/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EZ_DEV54_GPS_HW_H
#define EZ_DEV54_GPS_HW_H
#include <stddef.h>
#include <stdint.h>
int gps_hw_init(void);
int gps_hw_start(void);
int gps_hw_stop(void);
int gps_hw_read(uint8_t *data, size_t length);
#endif
