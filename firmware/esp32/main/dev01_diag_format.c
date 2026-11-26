/* SPDX-License-Identifier: Apache-2.0 */
#include "dev01_diag_format.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
static int basis(uint32_t value, uint32_t count) {
  return count ? (int)((uint64_t)value * 10000 / count) : -1;
}
int dev01_diag_format(char *out, size_t size,
                      const struct dev01_idle_samples *s, uint32_t window_ms,
                      bool ps_requested, unsigned listen_interval, int state,
                      int rssi, uint32_t restarts, uint32_t rx_drops) {
  if (!out || !s || !size)
    return -EINVAL;
  int n = snprintf(
      out, size,
      "v1 idle t=%" PRIu32 "ms ps_req=%u li=%u sta=%d rssi=%d n=%" PRIu32
      " wake=%d busy=%d irqlow=%d quiet=%d rst=%" PRIu32 " rxdrop=%" PRIu32,
      window_ms, (unsigned)ps_requested, listen_interval, state, rssi, s->count,
      basis(s->wake, s->count), basis(s->busy, s->count),
      basis(s->irq, s->count), basis(s->quiet, s->count), restarts, rx_drops);
  return n < 0 || (size_t)n >= size ? -ENOSPC : n;
}
