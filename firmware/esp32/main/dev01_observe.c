/* SPDX-License-Identifier: Apache-2.0 */
#include "dev01_observe.h"
#include "esp_err.h"
#include "esp_vfs_eventfd.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int wake_fd = -1;
static atomic_uint pending;
void dev01_observe_init(void) {
  const esp_vfs_eventfd_config_t config = {.max_fds = 1};
  ESP_ERROR_CHECK(esp_vfs_eventfd_register(&config));
  /* IDF eventfds are nonblocking; only EFD_SUPPORT_ISR is an optional flag. */
  wake_fd = eventfd(0, 0);
  if (wake_fd < 0) abort();
}
int dev01_observe_fd(void) { return wake_fd; }
void dev01_observe_changed(unsigned resources) {
  atomic_fetch_or(&pending, resources);
  const uint64_t signal = 1;
  if (write(wake_fd, &signal, sizeof(signal)) != sizeof(signal)) abort();
}
static void notify(lwm2m_context_t *context, uint16_t object, uint16_t resource) {
  lwm2m_uri_t uri;
  LWM2M_URI_RESET(&uri);
  uri.objectId = object; uri.instanceId = 0; uri.resourceId = resource;
  lwm2m_resource_value_changed(context, &uri);
}
void dev01_observe_step(lwm2m_context_t *context) {
  uint64_t signal;
  if (read(wake_fd, &signal, sizeof(signal)) != sizeof(signal)) abort();
  /* A producer racing this exchange either joins this batch or leaves the
   * eventfd readable for the next loop. Never access Wakaama from its task. */
  unsigned changes = atomic_exchange(&pending, 0);
  if (changes & DEV01_CHANGED_LED) notify(context, 3311, 5850);
  if (changes & DEV01_CHANGED_BUTTON) notify(context, 3347, 5500);
  if (changes & DEV01_CHANGED_PRESS_COUNT) notify(context, 3347, 5501);
}
