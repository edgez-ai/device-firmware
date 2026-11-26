/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "liblwm2m.h"
enum dev01_observe_change {
  DEV01_CHANGED_LED = 1U << 0,
  DEV01_CHANGED_BUTTON = 1U << 1,
  DEV01_CHANGED_PRESS_COUNT = 1U << 2,
};
void dev01_observe_init(void);
void dev01_observe_changed(unsigned resources);
int dev01_observe_fd(void);
/* Called only by the LwM2M task, before lwm2m_step(). */
void dev01_observe_step(lwm2m_context_t *context);
