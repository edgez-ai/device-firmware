/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define PROV_MOTION_WINDOW_MS 3000U
struct prov_motion {
  int32_t reference_mg[3], previous_mg[3];
  uint32_t start_ms, stable_since_ms;
  bool initialized, stable;
};
void prov_motion_init(struct prov_motion *, const int32_t reference_mg[3],
                      uint32_t now_ms);
/* Accelerometer-only posture gate: two gravity components must differ from
 * the pre-sleep reference and the new near-1 g pose must settle for 300 ms. */
bool prov_motion_sample(struct prov_motion *, uint32_t now_ms,
                        const int32_t mg[3], int32_t threshold_mg);
