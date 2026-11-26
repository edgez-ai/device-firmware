#include "dev01_diag_format.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
int main(void) {
  char text[DEV01_DIAG_TEXT_SIZE];
  struct dev01_idle_samples s = {0};
  assert(dev01_diag_format(text, sizeof(text), &s, 0, true, 10, 2, -39, 0, 0) > 0);
  assert(strstr(text, "wake=-1 busy=-1 irqlow=-1 quiet=-1"));
  dev01_idle_sample(&s, true, false, false);
  dev01_idle_sample(&s, false, true, true);
  dev01_idle_sample(&s, false, false, false);
  dev01_idle_sample(&s, false, false, false);
  assert(dev01_diag_format(text, sizeof(text), &s, 1000, true, 10, 2, -39, 0, 0) > 0);
  assert(strstr(text, "wake=2500 busy=2500 irqlow=2500 quiet=5000"));
  s = (struct dev01_idle_samples){UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
  dev01_idle_sample(&s, true, true, true);
  assert(s.count == UINT32_MAX && s.wake == UINT32_MAX);
  assert(dev01_diag_format(text, sizeof(text), &s, UINT32_MAX, true, UINT_MAX, INT_MIN, INT_MIN, UINT32_MAX, UINT32_MAX) > 0);
  assert(strstr(text, "wake=10000"));
  assert(dev01_diag_format(text, 4, &s, 0, true, 10, 2, 0, 0, 0) == -ENOSPC);
  assert(dev01_diag_format(NULL, 4, &s, 0, true, 10, 2, 0, 0, 0) == -EINVAL);
  return 0;
}
