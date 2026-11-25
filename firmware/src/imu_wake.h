/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
enum imu_wake_mode { IMU_WAKE_DISABLED, IMU_WAKE_MOTION, IMU_WAKE_ORIENTATION };
struct imu_wake_config { uint8_t mode; uint16_t motion_mg; uint8_t angle_deg; };
static inline bool imu_wake_config_valid(const struct imu_wake_config *c)
{
    return c && c->mode <= IMU_WAKE_ORIENTATION && c->motion_mg >= 32 && c->motion_mg <= 1968 &&
        (c->angle_deg == 50 || c->angle_deg == 60 || c->angle_deg == 70 || c->angle_deg == 80);
}
int imu_wake_config_init(void);
struct imu_wake_config imu_wake_config_get(void);
int imu_wake_config_set(const struct imu_wake_config *);
/* Wakaama-thread prepare/cleanup. ISR only records an event and queues notify. */
int board_imu_wake_arm(const struct imu_wake_config *, void (*notify)(void));
int board_imu_wake_disarm(void);
bool board_imu_wake_fired(void);
