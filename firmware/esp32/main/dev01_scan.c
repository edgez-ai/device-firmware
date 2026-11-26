/* SPDX-License-Identifier: Apache-2.0 */
#include "dev01.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mmhalow.h"
#include <errno.h>
#include <string.h>
static SemaphoreHandle_t guard;
static EventGroupHandle_t done;
static struct prov_ap entries[PROV_SCAN_MAX];
static uint32_t count;
static bool running;
static int scan_error;
static void scan_rx(const struct mmwlan_scan_result *result, void *arg) {
  (void)arg;
  if (!result->ssid_len || result->ssid_len > 32)
    return;
  xSemaphoreTake(guard, portMAX_DELAY);
  uint32_t index;
  for (index = 0; index < count; ++index)
    if (!memcmp(entries[index].bssid, result->bssid, 6))
      break;
  if (index < PROV_SCAN_MAX) {
    if (index == count)
      ++count;
    struct prov_ap *ap = &entries[index];
    memset(ap, 0, sizeof(*ap));
    memcpy(ap->ssid, result->ssid, result->ssid_len);
    ap->ssid_len = result->ssid_len;
    memcpy(ap->bssid, result->bssid, 6);
    ap->rssi = result->rssi;
    ap->auth = (result->capability_info & 0x10) ? 6 : 0;
    const struct mmwlan_s1g_channel_list *list =
        mmwlan_lookup_regulatory_domain(get_regulatory_db(),
                                        dev01_factory_country());
    for (unsigned i = 0; list && i < list->num_channels; i++)
      if (list->channels[i].centre_freq_hz == result->channel_freq_hz) {
        ap->channel = list->channels[i].s1g_chan_num;
        break;
      }
  }
  xSemaphoreGive(guard);
}
static void scan_complete(enum mmwlan_scan_state state, void *arg) {
  (void)state;
  (void)arg;
  xEventGroupSetBits(done, 1);
}
static void scan_task(void *arg) {
  (void)arg;
  esp_err_t rc = dev01_radio_acquire();
  struct mmhalow_scan_args args = {.rx_cb = scan_rx,
                                   .complete_cb = scan_complete};
  if (!rc)
    rc = mmhalow_scan(&args);
  if (!rc &&
      !(xEventGroupWaitBits(done, 1, pdFALSE, pdTRUE, pdMS_TO_TICKS(30000)) &
        1)) {
    rc = ESP_ERR_TIMEOUT;
    mmwlan_scan_abort();
    xEventGroupWaitBits(done, 1, pdFALSE, pdTRUE, pdMS_TO_TICKS(2000));
  }
  dev01_radio_release();
  xSemaphoreTake(guard, portMAX_DELAY);
  running = false;
  scan_error = rc ? -EIO : 0;
  xSemaphoreGive(guard);
  xEventGroupSetBits(done, 2);
  vTaskDelete(NULL);
}
int dev01_scan_start(void *user, bool passive, bool blocking) {
  (void)user;
  if (passive)
    return -ENOTSUP;
  if (!guard) {
    guard = xSemaphoreCreateMutex();
    done = xEventGroupCreate();
  }
  if (!guard || !done)
    return -ENOMEM;
  xSemaphoreTake(guard, portMAX_DELAY);
  if (running) {
    xSemaphoreGive(guard);
    return -EBUSY;
  }
  running = true;
  scan_error = 0;
  count = 0;
  memset(entries, 0, sizeof(entries));
  xEventGroupClearBits(done, 3);
  xSemaphoreGive(guard);
  if (xTaskCreate(scan_task, "halow_scan", 4096, NULL, 4, NULL) != pdPASS) {
    xSemaphoreTake(guard, portMAX_DELAY);
    running = false;
    xSemaphoreGive(guard);
    return -ENOMEM;
  }
  if (blocking &&
      !(xEventGroupWaitBits(done, 2, pdFALSE, pdTRUE, pdMS_TO_TICKS(35000)) &
        2))
    return -ETIMEDOUT;
  xSemaphoreTake(guard, portMAX_DELAY);
  int rc = blocking ? scan_error : 0;
  xSemaphoreGive(guard);
  return rc;
}
void dev01_scan_status(void *user, bool *finished, uint32_t *total) {
  (void)user;
  *finished = true;
  *total = 0;
  if (!guard)
    return;
  xSemaphoreTake(guard, portMAX_DELAY);
  *finished = !running;
  *total = count;
  xSemaphoreGive(guard);
}
int dev01_scan_result(void *user, uint32_t index, struct prov_ap *ap) {
  (void)user;
  if (!guard)
    return -ENOENT;
  xSemaphoreTake(guard, portMAX_DELAY);
  int rc = index < count ? 0 : -ENOENT;
  if (!rc)
    *ap = entries[index];
  xSemaphoreGive(guard);
  return rc;
}
