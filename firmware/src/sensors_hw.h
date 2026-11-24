/* SPDX-License-Identifier: Apache-2.0 */
#ifndef EZ_DEV54_SENSORS_HW_H
#define EZ_DEV54_SENSORS_HW_H
#define EZ_IMU_ACCEL 1U
#define EZ_IMU_GYRO 2U
int board_sensors_init(void);
int board_sensors_error(void);
int board_imu_observe(unsigned int mask);
int board_imu_read(unsigned int sensor, double axes[3]);
int board_voltage_read(double *volts);
#endif
