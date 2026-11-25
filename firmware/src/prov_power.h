/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
/* 0 on ordinary boot or accepted shake; otherwise powers off, or returns an
 * error. */
int prov_power_boot_check(void);
/* Arms the low-power IMU and enters System OFF. Returns only on preparation
 * error. */
int prov_power_sleep(void);
