/* SPDX-License-Identifier: Apache-2.0 */
#include "prov_motion.h"
#include <stdlib.h>
#include <string.h>
void prov_motion_init(struct prov_motion *s, uint32_t now) {
  memset(s, 0, sizeof(*s));
  s->start_ms = now;
  s->initialized = true;
}
bool prov_motion_sample(struct prov_motion *s, uint32_t now,
                        const int32_t mdps[3], uint16_t threshold_dps) {
  if (!s->initialized || !threshold_dps ||
      now - s->start_ms >= PROV_MOTION_WINDOW_MS)
    return false;
  /* Sensor turn-on transients never count towards confirmation. */
  if (now - s->start_ms < PROV_GYRO_SETTLE_MS)
    return false;
  unsigned axes = 0;
  for (unsigned i = 0; i < 3; ++i)
    if (llabs((long long)mdps[i]) > (int32_t)threshold_dps * 1000)
      ++axes;
  if (axes < 2) {
    s->high = false;
    s->last_sample_ms = now;
    return false;
  }
  if (!s->high || now - s->last_sample_ms > 100U) {
    s->high = true;
    s->high_since_ms = now;
  }
  s->last_sample_ms = now;
  return now - s->high_since_ms >= PROV_GYRO_CONFIRM_MS;
}
