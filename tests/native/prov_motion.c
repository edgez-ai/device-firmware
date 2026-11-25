/* SPDX-License-Identifier: Apache-2.0 */
#include "prov_motion.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
static bool feed(struct prov_motion *s, unsigned t, int x, int y, int z,
                 unsigned threshold) {
  int32_t axes[] = {x, y, z};
  return prov_motion_sample(s, t, axes, threshold);
}
int main(void) {
  struct prov_motion s;
  prov_motion_init(&s, 0);
  for (unsigned t = 0; t < 3000; t += 40)
    assert(!feed(&s, t, 0, 0, 0, 30));
  /* Fresh gyro startup spikes do not count. Opposite directions do. */
  prov_motion_init(&s, 0);
  assert(!feed(&s, 0, 200000, 200000, 0, 30));
  assert(!feed(&s, 180, 200000, 200000, 0, 30));
  assert(!feed(&s, 200, 40000, -40000, 0, 30));
  assert(!feed(&s, 240, 40000, -40000, 0, 30));
  assert(!feed(&s, 280, 40000, -40000, 0, 30));
  assert(feed(&s, 320, 40000, -40000, 0, 30));
  /* Strictly greater than 30 dps on two axes is required. */
  prov_motion_init(&s, 0);
  for (unsigned t = 200; t < 3000; t += 40)
    assert(!feed(&s, t, 30000, -30000, 0, 30));
  prov_motion_init(&s, 0);
  for (unsigned t = 200; t < 3000; t += 40)
    assert(!feed(&s, t, 200000, 0, 0, 30));
  prov_motion_init(&s, 0);
  for (unsigned t = 200; t < 3000; t += 40)
    assert(!feed(&s, t, (t / 40) % 2 ? 40000 : 0, (t / 40) % 2 ? 0 : 40000, 0,
                 30));
  /* One short two-axis impact, or separate spikes, cannot accumulate. */
  prov_motion_init(&s, 0);
  assert(!feed(&s, 200, 100000, 100000, 0, 30));
  assert(!feed(&s, 240, 0, 0, 0, 30));
  assert(!feed(&s, 280, 100000, 100000, 0, 30));
  assert(!feed(&s, 320, 0, 0, 0, 30));
  /* A missing sample interval breaks continuity. */
  prov_motion_init(&s, 0);
  assert(!feed(&s, 200, 40000, 40000, 0, 30));
  assert(!feed(&s, 400, 40000, 40000, 0, 30));
  assert(!feed(&s, 440, 40000, 40000, 0, 30));
  assert(!feed(&s, 480, 40000, 40000, 0, 30));
  assert(feed(&s, 520, 40000, 40000, 0, 30));
  prov_motion_init(&s, 0);
  for (unsigned t = 200; t < 1000; t += 40)
    assert(!feed(&s, t, 40000, 40000, 0, 50));
  prov_motion_init(&s, 0);
  assert(!feed(&s, 3000, 100000, 100000, 0, 30));
  prov_motion_init(&s, UINT32_MAX - 100);
  assert(!feed(&s, 3000, 100000, 100000, 0, 30));
  prov_motion_init(&s, 0);
  assert(!feed(&s, 200, INT_MIN, INT_MIN, 0, 30));
  assert(!feed(&s, 240, INT_MIN, INT_MIN, 0, 0));
  puts("PASS: gyro startup guard, two-axis rate confirmation, strict "
       "threshold, spike/gap rejection and deadline");
}
