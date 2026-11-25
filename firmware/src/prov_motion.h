/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define PROV_MOTION_WINDOW_MS 3000U
#define PROV_GYRO_SETTLE_MS 200U
#define PROV_GYRO_CONFIRM_MS 100U
struct prov_motion {
  uint32_t start_ms, high_since_ms, last_sample_ms;
  bool initialized, high;
};
void prov_motion_init(struct prov_motion *, uint32_t now_ms);
/* Gyroscope samples in milli-degrees/second. Two axes must simultaneously
 * exceed the factory degrees/second threshold for a continuous 100 ms. */
bool prov_motion_sample(struct prov_motion *, uint32_t now_ms,
                        const int32_t mdps[3], uint16_t threshold_dps);
