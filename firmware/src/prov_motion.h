/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define PROV_MOTION_WINDOW_MS 3000U
struct prov_motion {
  int32_t gravity_q8[3];
  uint32_t start_ms, last_peak_ms;
  unsigned axes, peaks;
  bool initialized, high;
};
/* Fresh acceleration samples in mg, typically 104 Hz. No timers or I/O. */
bool prov_motion_sample(struct prov_motion *state, uint32_t now_ms,
                        const int32_t mg[3], int32_t threshold_mg);
