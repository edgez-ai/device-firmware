/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define DEV01_DIAG_TEXT_SIZE 220
struct dev01_idle_samples {
  uint32_t count, wake, busy, irq, quiet;
};
static inline void dev01_idle_sample(struct dev01_idle_samples *s, bool wake,
                                     bool busy, bool irq_low) {
  if (s->count == UINT32_MAX)
    return;
  s->count++;
  s->wake += wake;
  s->busy += busy;
  s->irq += irq_low;
  s->quiet += !wake && !busy && !irq_low;
}
int dev01_diag_format(char *out, size_t size,
                      const struct dev01_idle_samples *s, uint32_t window_ms,
                      bool ps_requested, unsigned listen_interval, int state,
                      int rssi, uint32_t restarts, uint32_t rx_drops);
