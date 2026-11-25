/* SPDX-License-Identifier: Apache-2.0 */
#include "prov_motion.h"
#include <assert.h>
#include <stdio.h>
static bool feed(struct prov_motion *s, unsigned t, int x, int y, int z,
                 int threshold) {
  int32_t axes[] = {x, y, z};
  return prov_motion_sample(s, t, axes, threshold);
}
static void start(struct prov_motion *s, unsigned now) {
  const int32_t reference[] = {0, 0, 1000};
  prov_motion_init(s, reference, now);
}
int main(void) {
  struct prov_motion s;
  start(&s, 0);
  for (unsigned t = 0; t < 3000; t += 40)
    assert(!feed(&s, t, 0, 0, 1000, 500));
  /* A slow 60-degree tilt changes X by 0.866 g and Z by 0.5 g.
   * Hold it, no shaking/gyroscope required. */
  start(&s, 0);
  for (unsigned t = 0; t < 340; t += 40)
    assert(!feed(&s, t, 866, 0, 500, 500));
  assert(feed(&s, 520, 866, 0, 500, 500));
  start(&s, 0);
  for (unsigned t = 0; t < 340; t += 40)
    assert(!feed(&s, t, -866, 0, 500, 500));
  assert(feed(&s, 520, -866, 0, 500, 500));
  /* Changing only one component, one short impact, and free fall fail. */
  start(&s, 0);
  for (unsigned t = 0; t < 3000; t += 40)
    assert(!feed(&s, t, 600, 0, 1000, 500));
  start(&s, 0);
  assert(!feed(&s, 100, 866, 0, 500, 500));
  for (unsigned t = 140; t < 3000; t += 40)
    assert(!feed(&s, t, 0, 0, 1000, 500));
  start(&s, 0);
  for (unsigned t = 0; t < 3000; t += 40)
    assert(!feed(&s, t, 0, 0, 0, 500));
  start(&s, 0);
  for (unsigned t = 0; t < 3000; t += 40)
    assert(!feed(&s, t, 2000, 2000, 1000, 500));
  /* Different axes at different instants must not be accumulated. */
  start(&s, 0);
  for (unsigned t = 0; t < 3000; t += 40)
    assert(!feed(&s, t, (t / 40) % 2 ? 600 : 0, (t / 40) % 2 ? 0 : 600, 1000,
                 500));
  start(&s, 0);
  for (unsigned t = 0; t < 500; t += 40)
    assert(!feed(&s, t, 866, 0, 500, 1000));
  /* A 90-degree tilt can meet a stricter 1 g factory setting. */
  start(&s, 0);
  for (unsigned t = 0; t < 340; t += 40)
    assert(!feed(&s, t, 1000, 0, 0, 1000));
  assert(feed(&s, 520, 1000, 0, 0, 1000));
  start(&s, 0);
  assert(!feed(&s, 3000, 866, 0, 500, 500));
  start(&s, UINT32_MAX - 100);
  assert(!feed(&s, 3000, 866, 0, 500, 500));
  puts("PASS: settled two-axis posture changes; reject gravity, single-axis "
       "changes, shocks, free fall, acceleration and expired verification");
}
