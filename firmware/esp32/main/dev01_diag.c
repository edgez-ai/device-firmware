/* SPDX-License-Identifier: Apache-2.0 */
#include "dev01_diag.h"
#include "dev01_power.h"
#include "dev01.h"
#include "dev01_diag_format.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "esp_freertos_hooks.h"
#include "esp_timer.h"
#include "mmhalow.h"
#include "mmwlan_stats.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>
static portMUX_TYPE guard = portMUX_INITIALIZER_UNLOCKED;
static struct dev01_idle_samples samples;
static int64_t sampled_us, window_us;
static bool started;
/* Reuse existing CPU0 idle entries. No new timer, allocation, logging, SPI
 * command or radio wakeup is introduced by measurement. Ratios describe the
 * sampled GPIO states while CPU0 is idle, not electrical current. */
static bool sample_idle(void) {
  int64_t now = esp_timer_get_time();
  if (now - sampled_us < 20000)
    return true;
  sampled_us = now;
  bool wake = (REG_READ(GPIO_OUT_REG) & (1U << CONFIG_MM_WAKE)) != 0;
  bool busy = gpio_get_level(CONFIG_MM_BUSY) != 0;
  bool irq_low = gpio_get_level(CONFIG_MM_SPI_IRQ) == 0;
  portENTER_CRITICAL(&guard);
  dev01_idle_sample(&samples, wake, busy, irq_low);
  portEXIT_CRITICAL(&guard);
  return true;
}
void dev01_diag_start(void) {
  if (started)
    return;
  window_us = esp_timer_get_time();
  ESP_ERROR_CHECK(esp_register_freertos_idle_hook_for_cpu(sample_idle, 0));
  started = true;
}
static void report(char text[DEV01_DIAG_TEXT_SIZE]) {
  struct dev01_idle_samples snapshot;
  int64_t now = esp_timer_get_time(), elapsed;
  portENTER_CRITICAL(&guard);
  snapshot = samples;
  memset(&samples, 0, sizeof(samples));
  elapsed = now - window_us;
  window_us = now;
  portEXIT_CRITICAL(&guard);
  struct mmwlan_stats_umac_data stats = {0};
  bool valid = mmwlan_get_umac_stats(&stats) == MMWLAN_SUCCESS;
  uint32_t ms =
      elapsed / 1000 > UINT32_MAX ? UINT32_MAX : (uint32_t)(elapsed / 1000);
  if (dev01_diag_format(text, DEV01_DIAG_TEXT_SIZE, &snapshot, ms,
                        dev01_power_save_requested(), dev01_listen_interval(),
                        mmwlan_get_sta_state(), valid ? stats.rssi : INT32_MIN,
                        valid ? stats.hw_restart_counter : 0,
                        valid ? stats.datapath_rxq_frames_dropped : 0) < 0)
    snprintf(text, DEV01_DIAG_TEXT_SIZE, "v1 error=diagnostic_overflow");
  size_t used = strlen(text);
  dev01_power_report(text + used, DEV01_DIAG_TEXT_SIZE - used);
}
static uint8_t discover(lwm2m_context_t *ctx, uint16_t id, int *count,
                        lwm2m_data_t **data, lwm2m_object_t *object) {
  (void)ctx;
  (void)object;
  if (id)
    return COAP_404_NOT_FOUND;
  if (!*count) {
    *data = lwm2m_data_new(3);
    if (!*data)
      return COAP_500_INTERNAL_SERVER_ERROR;
    *count = 3;
    (*data)[0].id = 5500;
    (*data)[1].id = 5501;
    (*data)[2].id = 5750;
  }
  for (int i = 0; i < *count; i++)
    if ((*data)[i].id != 5500 && (*data)[i].id != 5501 && (*data)[i].id != 5750)
      return COAP_404_NOT_FOUND;
  return COAP_205_CONTENT;
}
static uint8_t read_diag(lwm2m_context_t *ctx, uint16_t id, int *count,
                         lwm2m_data_t **data, lwm2m_object_t *object) {
  uint8_t rc = discover(ctx, id, count, data, object);
  if (rc != COAP_205_CONTENT)
    return rc;
  for (int i = 0; i < *count; i++)
    switch ((*data)[i].id) {
    case 5500:
      lwm2m_data_encode_bool(!gpio_get_level(GPIO_NUM_0), *data + i);
      break;
    case 5501:
      lwm2m_data_encode_int(dev01_button_presses(), *data + i);
      break;
    case 5750: {
      char text[DEV01_DIAG_TEXT_SIZE];
      report(text);
      lwm2m_data_encode_string(text, *data + i);
      break;
    }
    }
  return COAP_205_CONTENT;
}
static lwm2m_list_t instance = {.id = 0};
static lwm2m_object_t object = {.objID = 3347,
                                .instanceList = &instance,
                                .readFunc = read_diag,
                                .discoverFunc = discover};
lwm2m_object_t *dev01_diag_object(void) { return &object; }
