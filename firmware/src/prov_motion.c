/* SPDX-License-Identifier: Apache-2.0 */
#include "prov_motion.h"
#include <stdlib.h>
#include <string.h>
void prov_motion_init(struct prov_motion *s, const int32_t reference[3],
                      uint32_t now) {
  memset(s, 0, sizeof(*s));
  memcpy(s->reference_mg, reference, sizeof(s->reference_mg));
  memcpy(s->previous_mg, reference, sizeof(s->previous_mg));
  s->start_ms = now;
  s->initialized = true;
}
bool prov_motion_sample(struct prov_motion *s, uint32_t now,
                        const int32_t mg[3], int32_t threshold) {
  if (!s->initialized || now - s->start_ms >= PROV_MOTION_WINDOW_MS)
    return false;
  int64_t magnitude2 = 0;
  unsigned axes = 0;
  bool quiet = true;
  for (unsigned i = 0; i < 3; ++i) {
    magnitude2 += (int64_t)mg[i] * mg[i];
    if (abs(mg[i] - s->reference_mg[i]) >= threshold)
      ++axes;
    if (abs(mg[i] - s->previous_mg[i]) > 100)
      quiet = false;
    s->previous_mg[i] = mg[i];
  }
  /* Reject free fall/transient acceleration. A three-axis accelerometer alone
   * cannot distinguish every sustained linear acceleration from gravity. */
  if (axes < 2 || !quiet || magnitude2 < 850LL * 850 ||
      magnitude2 > 1150LL * 1150) {
    s->stable = false;
    return false;
  }
  if (!s->stable) {
    s->stable = true;
    s->stable_since_ms = now;
  }
  return now - s->stable_since_ms >= 300U;
}
