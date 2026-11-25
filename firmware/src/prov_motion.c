/* SPDX-License-Identifier: Apache-2.0 */
#include "prov_motion.h"
#include <stdlib.h>

bool prov_motion_sample(struct prov_motion *s, uint32_t now,
                        const int32_t mg[3], int32_t threshold) {
  if (!s->initialized) {
    for (unsigned i = 0; i < 3; ++i)
      s->gravity_q8[i] = mg[i] * 256;
    s->initialized = true;
    s->start_ms = now;
    return false;
  }
  if (now - s->start_ms >= PROV_MOTION_WINDOW_MS)
    return false;
  if (s->peaks && now - s->last_peak_ms > 800U) {
    s->peaks = s->axes = 0;
  }
  unsigned axes = 0;
  bool released = true;
  for (unsigned i = 0; i < 3; ++i) {
    /* Track gravity slowly; do not mistake a stationary 1 g axis for shaking.
     */
    s->gravity_q8[i] += (mg[i] * 256 - s->gravity_q8[i]) / 16;
    int32_t delta = abs(mg[i] - s->gravity_q8[i] / 256);
    if (delta >= threshold)
      axes |= 1U << i;
    if (delta > threshold / 2)
      released = false;
  }
  if (released)
    s->high = false;
  if (axes && !s->high) {
    if (!s->peaks || now - s->last_peak_ms >= 80U) {
      s->last_peak_ms = now;
      s->peaks++;
      s->axes |= axes;
    }
    s->high = true;
  }
  /* Three distinct strong excursions, with at least two axes represented. */
  return s->peaks >= 3 && (s->axes & (s->axes - 1U)) != 0;
}
