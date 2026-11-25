/* SPDX-License-Identifier: Apache-2.0 */
#include "prov_motion.h"
#include <assert.h>
#include <stdio.h>

static bool sample(struct prov_motion *s, unsigned t, int x, int y, int z) {
  int32_t axes[] = {x, y, z};
  return prov_motion_sample(s, t, axes, 1000);
}
static void rest(struct prov_motion *s, unsigned from, unsigned until) {
  for (unsigned t = from; t < until; t += 10)
    assert(!sample(s, t, 0, 0, 1000));
}
static void pulses(bool second_axis, bool negative) {
  struct prov_motion s = {0};
  rest(&s, 0, 500);
  int amplitude = negative ? -4000 : 4000;
  assert(!sample(&s, 500, amplitude, 0, 1000));
  rest(&s, 510, 700);
  assert(!sample(&s, 700, second_axis ? 0 : amplitude,
                 second_axis ? amplitude : 0, 1000));
  rest(&s, 710, 900);
  assert(sample(&s, 900, amplitude, 0, 1000) == second_axis);
}
int main(void) {
  pulses(true, false);
  pulses(true, true);
  pulses(false, false);
  struct prov_motion s = {0};
  rest(&s, 0, 500);
  assert(!sample(&s, 500, 4000, 0, 1000));
  assert(
      !sample(&s, 510, 4000, 4000, 1000)); /* staggered axes, one excursion */
  rest(&s, 520, 700);
  assert(!sample(&s, 700, 4000, 0, 1000));
  rest(&s, 710, 900);
  assert(sample(&s, 900, 4000, 0, 1000));
  s = (struct prov_motion){0};
  rest(&s, 0, 3000); /* gravity and rest never count */
  s = (struct prov_motion){0};
  rest(&s, 0, 500);
  assert(!sample(&s, 500, 4000, 4000, 5000)); /* one impact on all axes */
  rest(&s, 510, 3000);
  s = (struct prov_motion){0};
  rest(&s, 0, 2600);
  assert(!sample(&s, 2600, 4000, 0, 1000));
  rest(&s, 2610, 2800);
  assert(!sample(&s, 2800, 0, 4000, 1000));
  rest(&s, 2810, 3000);
  assert(!sample(&s, 3000, 4000, 0, 1000)); /* exact deadline rejected */
  s = (struct prov_motion){0};
  rest(&s, 0, 300);
  assert(!sample(&s, 300, 4000, 0, 1000));
  rest(&s, 310, 1400);
  assert(!sample(&s, 1400, 0, 4000, 1000));
  rest(&s, 1410, 2600);
  assert(
      !sample(&s, 2600, 4000, 0, 1000)); /* isolated bumps are not continuous */
  s = (struct prov_motion){0};
  for (unsigned t = 0; t < 3000; t += 10)
    assert(!sample(&s, t, (int)t / 3, 0, 1000 - (int)t / 3)); /* slow tilt */
  s = (struct prov_motion){0};
  assert(!sample(&s, UINT32_MAX - 100, 0, 0, 1000));
  assert(!sample(&s, 3000, 5000, 5000, 5000)); /* uptime wrap, expired */
  puts("PASS: repeated two-axis positive/negative shakes; reject one axis, "
       "gravity, tilt, single shock, gaps and late shakes");
}
