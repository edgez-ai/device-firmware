/* SPDX-License-Identifier: Apache-2.0 */
#include "dev01.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_partition.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "factory_record.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mmhalow.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "psa/crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIFI_MAGIC 0x455a5701U
static const char *TAG = "ez_dev01";
edgez_esp_factory_identity_t dev01_identity;
static dev01_wifi_t active;
static char model[33] = "EZ-DEV01";
static char name[129], pop[33], address[16];
static bool committed, attempting, radio_ready, power_save_requested;
static uint32_t button_presses;
static unsigned state = 2;
static SemaphoreHandle_t lock, radio_lock;
static QueueHandle_t requests;
static EventGroupHandle_t events;
static const unsigned GOT_IP = 1;
const char *dev01_model(void) { return model; }
const char *dev01_name(void) { return name; }
const char *dev01_pop(void) { return pop; }
const char *dev01_factory_country(void) { return dev01_identity.country; }
static bool valid_wifi(const dev01_wifi_t *wifi) {
  return wifi->magic == WIFI_MAGIC &&
         memchr(wifi->ssid, 0, sizeof(wifi->ssid)) &&
         memchr(wifi->password, 0, sizeof(wifi->password)) &&
         strlen(wifi->ssid) > 0 && strlen(wifi->ssid) <= 32 &&
         strlen(wifi->password) >= 8 && strlen(wifi->password) <= 63;
}
bool dev01_configured(void) {
  xSemaphoreTake(lock, portMAX_DELAY);
  bool result = committed;
  xSemaphoreGive(lock);
  return result;
}
bool dev01_companion_available(void) {
  xSemaphoreTake(lock, portMAX_DELAY);
  bool result = !committed && !attempting;
  xSemaphoreGive(lock);
  return result;
}
bool dev01_power_save_requested(void) { return power_save_requested; }
uint32_t dev01_button_presses(void) {
  xSemaphoreTake(lock, portMAX_DELAY);
  uint32_t count = button_presses;
  xSemaphoreGive(lock);
  return count;
}
void dev01_wifi_status(unsigned *value, char ip[16]) {
  xSemaphoreTake(lock, portMAX_DELAY);
  *value = state;
  memcpy(ip, address, 16);
  xSemaphoreGive(lock);
}
esp_err_t dev01_set_name(const char *value) {
  size_t size = strlen(value);
  if (!size || size > 128)
    return ESP_ERR_INVALID_ARG;
  for (size_t i = 0; i < size; i++)
    if ((unsigned char)value[i] < 32 || value[i] == 127)
      return ESP_ERR_INVALID_ARG;
  nvs_handle_t nvs;
  esp_err_t rc = nvs_open("ez_dev01", NVS_READWRITE, &nvs);
  if (rc)
    return rc;
  rc = nvs_set_str(nvs, "name", value);
  if (!rc)
    rc = nvs_commit(nvs);
  nvs_close(nvs);
  if (!rc) {
    xSemaphoreTake(lock, portMAX_DELAY);
    memcpy(name, value, size + 1);
    xSemaphoreGive(lock);
  }
  return rc;
}
static void ip_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
  (void)arg;
  (void)base;
  (void)id;
  ip_event_got_ip_t *event = data;
  if (strcmp(esp_netif_get_ifkey(event->esp_netif), "WIFI_STA_DEF"))
    return;
  xSemaphoreTake(lock, portMAX_DELAY);
  snprintf(address, sizeof(address), IPSTR, IP2STR(&event->ip_info.ip));
  xSemaphoreGive(lock);
  xEventGroupSetBits(events, GOT_IP);
}
static void station_state(enum mmwlan_sta_state value) {
  if (value == MMWLAN_STA_DISABLED)
    xEventGroupClearBits(events, GOT_IP);
}
esp_err_t dev01_radio_acquire(void) {
  xSemaphoreTake(radio_lock, portMAX_DELAY);
  esp_err_t rc = ESP_OK;
  if (!radio_ready) {
    if (!mmwlan_lookup_regulatory_domain(get_regulatory_db(),
                                         dev01_identity.country))
      rc = ESP_ERR_INVALID_ARG;
    if (!rc)
      rc = mmhalow_init(NULL);
#if CONFIG_HALOW_PS_MODE
    if (!rc && mmwlan_set_listen_interval(10) != MMWLAN_SUCCESS)
      rc = ESP_FAIL;
    if (!rc && mmwlan_set_power_save_mode(MMWLAN_PS_ENABLED) != MMWLAN_SUCCESS)
      rc = ESP_FAIL;
    if (!rc)
      power_save_requested = true;
    if (!rc)
      ESP_LOGI(TAG, "HaLow power save enabled; listen interval 10 beacons (AP "
                    "DTIM controls wake schedule)");
#endif
    if (!rc)
      radio_ready = true;
  }
  return rc;
}
void dev01_radio_release(void) { xSemaphoreGive(radio_lock); }
static void station_task(void *arg) {
  (void)arg;
  dev01_wifi_t wifi;
  while (xQueueReceive(requests, &wifi, portMAX_DELAY) == pdTRUE) {
    esp_err_t rc = dev01_radio_acquire();
    if (!rc)
      (void)mmhalow_disconnect();
    xEventGroupClearBits(events, GOT_IP);
    mmhalow_wifi_config_t config = {.sta = MMWLAN_STA_ARGS_INIT};
    memcpy(config.sta.ssid, wifi.ssid, strlen(wifi.ssid));
    config.sta.ssid_len = strlen(wifi.ssid);
    memcpy(config.sta.passphrase, wifi.password, strlen(wifi.password));
    config.sta.passphrase_len = strlen(wifi.password);
    config.sta.security_type = MMWLAN_SAE;
    if (!rc)
      rc = mmhalow_set_config(WIFI_IF_STA, &config);
    if (!rc)
      rc = mmhalow_connect(station_state);
    if (!rc && !(xEventGroupWaitBits(events, GOT_IP, pdFALSE, pdTRUE,
                                     pdMS_TO_TICKS(30000)) &
                 GOT_IP))
      rc = ESP_ERR_TIMEOUT;
    if (rc && radio_ready) {
      (void)mmhalow_disconnect();
      xEventGroupClearBits(events, GOT_IP);
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!rc && !committed) {
      nvs_handle_t nvs;
      rc = nvs_open("ez_dev01", NVS_READWRITE, &nvs);
      if (!rc) {
        rc = nvs_set_blob(nvs, "wifi", &wifi, sizeof(wifi));
        if (!rc)
          rc = nvs_commit(nvs);
        nvs_close(nvs);
      }
      if (!rc) {
        active = wifi;
        committed = true;
      }
    }
    state = rc ? 3 : 0;
    attempting = false;
    xSemaphoreGive(lock);
    memset(&wifi, 0, sizeof(wifi));
    memset(&config, 0, sizeof(config));
    dev01_radio_release();
  }
}
esp_err_t dev01_request_wifi(const dev01_wifi_t *wifi) {
  if (!valid_wifi(wifi))
    return ESP_ERR_INVALID_ARG;
  xSemaphoreTake(lock, portMAX_DELAY);
  if (attempting) {
    xSemaphoreGive(lock);
    return ESP_ERR_INVALID_STATE;
  }
  attempting = true;
  state = 1;
  xSemaphoreGive(lock);
  if (xQueueSend(requests, wifi, 0) != pdTRUE) {
    xSemaphoreTake(lock, portMAX_DELAY);
    attempting = false;
    state = 3;
    xSemaphoreGive(lock);
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}
static void reboot_task(void *arg) {
  vTaskDelay(pdMS_TO_TICKS(500));
  if (arg) {
    nvs_handle_t nvs;
    if (nvs_open("ez_dev01", NVS_READWRITE, &nvs) == ESP_OK) {
      nvs_erase_key(nvs, "wifi");
      nvs_commit(nvs);
      nvs_close(nvs);
    }
  }
  esp_restart();
}
void dev01_reboot(bool forget_wifi) {
  xTaskCreate(reboot_task, "restart", 3072, (void *)(uintptr_t)forget_wifi, 4,
              NULL);
}
static void button_task(void *arg) {
  (void)arg;
  gpio_config_t config = {.pin_bit_mask = 1ULL << GPIO_NUM_0,
                          .mode = GPIO_MODE_INPUT,
                          .pull_up_en = GPIO_PULLUP_ENABLE};
  ESP_ERROR_CHECK(gpio_config(&config));
  /* A held wake button must be released before it can request a reset. */
  while (!gpio_get_level(GPIO_NUM_0))
    vTaskDelay(pdMS_TO_TICKS(50));
  unsigned held = 0;
  for (;;) {
    bool pressed = !gpio_get_level(GPIO_NUM_0);
    if (pressed && !held) {
      xSemaphoreTake(lock, portMAX_DELAY);
      if (button_presses < UINT32_MAX)
        button_presses++;
      xSemaphoreGive(lock);
    }
    held = pressed ? held + 1 : 0;
    if (held >= 100) {
      while (!gpio_get_level(GPIO_NUM_0))
        vTaskDelay(pdMS_TO_TICKS(50));
      dev01_reboot(true);
      vTaskDelete(NULL);
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
static void standby(void) {
  if (radio_ready)
    (void)mmhalow_deinit();
  gpio_set_direction(CONFIG_MM_RESET_N, GPIO_MODE_OUTPUT);
  gpio_set_level(CONFIG_MM_RESET_N, 0);
  gpio_hold_en(CONFIG_MM_RESET_N);
  gpio_deep_sleep_hold_en();
  while (!gpio_get_level(GPIO_NUM_0))
    vTaskDelay(pdMS_TO_TICKS(50));
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_ext1_wakeup_io(1ULL << 0, ESP_EXT1_WAKEUP_ANY_LOW);
  esp_deep_sleep_start();
}
void app_main(void) {
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis(CONFIG_MM_RESET_N);
  ESP_ERROR_CHECK(nvs_flash_init());
  ESP_ERROR_CHECK(edgez_esp_factory_data_init());
  ESP_ERROR_CHECK(edgez_esp_factory_data_get(&dev01_identity));
  const esp_partition_t *factory =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "factory_data");
  uint8_t page[EZ_FACTORY_MODEL_OFFSET + EZ_FACTORY_MODEL_SIZE];
  if (factory && esp_partition_read(factory, 0, page, sizeof(page)) == ESP_OK) {
    char saved_model[33];
    if (!ez_factory_model_parse(page, sizeof(page), saved_model))
      strlcpy(model, saved_model, sizeof(model));
  }
  memset(page, 0, sizeof(page));
  lock = xSemaphoreCreateMutex();
  radio_lock = xSemaphoreCreateMutex();
  requests = xQueueCreate(1, sizeof(dev01_wifi_t));
  events = xEventGroupCreate();
  if (!lock || !radio_lock || !requests || !events)
    abort();
  nvs_handle_t nvs;
  size_t size = sizeof(active);
  if (nvs_open("ez_dev01", NVS_READONLY, &nvs) == ESP_OK) {
    committed = nvs_get_blob(nvs, "wifi", &active, &size) == ESP_OK &&
                size == sizeof(active) && valid_wifi(&active);
    size = sizeof(name);
    if (nvs_get_str(nvs, "name", name, &size) != ESP_OK)
      name[0] = 0;
    nvs_close(nvs);
  }
  /* Factory credentials have the same precedence as EZ-DEV54. */
  if (dev01_identity.has_halow_profile &&
      strlen(dev01_identity.passphrase) >= 8 &&
      strlen(dev01_identity.passphrase) <= 63) {
    active.magic = WIFI_MAGIC;
    strlcpy(active.ssid, dev01_identity.mesh_id, sizeof(active.ssid));
    strlcpy(active.password, dev01_identity.passphrase,
            sizeof(active.password));
    committed = true;
  }
  ESP_ERROR_CHECK(psa_crypto_init() == PSA_SUCCESS ? ESP_OK : ESP_FAIL);
  const char label[] = "EZ-CAM01 BLE provisioning PoP v1";
  uint8_t digest[32];
  size_t length;
  psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
  psa_key_id_t key;
  psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
  psa_set_key_bits(&attributes, 256);
  psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
  psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_256));
  if (psa_import_key(&attributes, dev01_identity.private_key, 32, &key) !=
      PSA_SUCCESS)
    abort();
  if (psa_mac_compute(key, PSA_ALG_HMAC(PSA_ALG_SHA_256),
                      (const uint8_t *)label, sizeof(label) - 1, digest,
                      sizeof(digest), &length) != PSA_SUCCESS ||
      length != 32)
    abort();
  psa_destroy_key(key);
  const char hex[] = "0123456789abcdef";
  for (unsigned i = 0; i < 16; i++) {
    pop[2 * i] = hex[digest[i] >> 4];
    pop[2 * i + 1] = hex[digest[i] & 15];
  }
  memset(digest, 0, sizeof(digest));
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             ip_event, NULL));
  if (xTaskCreate(station_task, "halow_sta", 8192, NULL, 5, NULL) != pdPASS)
    abort();
  if (xTaskCreate(button_task, "user_button", 2048, NULL, 3, NULL) != pdPASS)
    abort();
  if (!committed) {
    esp_err_t rc = dev01_onboarding();
    if (rc || !dev01_configured())
      standby();
  } else {
    ESP_ERROR_CHECK(dev01_request_wifi(&active));
  }
  while (!(xEventGroupGetBits(events) & GOT_IP)) {
    /* DHCP can finish during the retry interval. Wake immediately and never
     * queue a reconnect over the successful association we just acquired. */
    if (xEventGroupWaitBits(events, GOT_IP, pdFALSE, pdTRUE,
                            pdMS_TO_TICKS(5000)) &
        GOT_IP)
      break;
    xSemaphoreTake(lock, portMAX_DELAY);
    bool retry = !attempting && !(xEventGroupGetBits(events) & GOT_IP);
    xSemaphoreGive(lock);
    if (retry)
      (void)dev01_request_wifi(&active);
  }
  ESP_LOGI(TAG, "HaLow connected; starting local gateway LwM2M client");
  dev01_lwm2m_start();
}
