/* SPDX-License-Identifier: Apache-2.0
 * HC33 implementation of the EZ-DEV54 one-shot WNM sleep object 31026.
 */
#include "dev01_board.h"
#include "dev01_sleep.h"
#include "dev01.h"
#include "dev01_sleep_request.h"
#include "dev01_power.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_rtc_time.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"
#include "lwip/netif.h"
#include "mmwlan.h"
#include <errno.h>
#include <string.h>
#define RESOURCE_COUNT 12
#define RESPONSE_GRACE_MS 2000
#define DRAIN_LIMIT_MS 10000
static enum { IDLE, PENDING, SLEEPING, WAKING } state;
static RTC_DATA_ATTR uint32_t retained_magic;
static RTC_DATA_ATTR bool deep_pending;
static RTC_DATA_ATTR int64_t duration_s = 60;
static int64_t requested_s, deadline, drain_deadline;
static RTC_DATA_ATTR int last_result, wake_reason;
static RTC_DATA_ATTR uint32_t cycles;
static RTC_DATA_ATTR int64_t entry_ms, exit_ms, slept_ms, accepted_until;
static RTC_DATA_ATTR int64_t deep_started_ms;
static struct dev01_sleep_request request;
static RTC_DATA_ATTR struct dev01_sleep_request accepted;
static esp_pm_lock_handle_t awake_lock;
static struct netif *gated_netif;
static netif_linkoutput_fn previous_output;
static int64_t now_ms(void) { return (int64_t)(esp_rtc_get_time_us() / 1000); }
#define RETAINED_MAGIC UINT32_C(0x534c5031)
static const gpio_num_t held_pins[] = {CONFIG_MM_RESET_N, CONFIG_MM_WAKE,
    CONFIG_MM_SPI_CS, CONFIG_MM_SPI_SCK, CONFIG_MM_SPI_MOSI, GPIO_NUM_20, DEV01_LED_GPIO};
void dev01_sleep_boot(void) {
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  bool waking = esp_reset_reason() == ESP_RST_DEEPSLEEP &&
                retained_magic == RETAINED_MAGIC && deep_pending;
  /* Release every held output before SPI/radio or board drivers initialize. */
  gpio_deep_sleep_hold_dis();
  ESP_ERROR_CHECK(rtc_gpio_hold_dis(DEV01_BUTTON_GPIO));
  ESP_ERROR_CHECK(rtc_gpio_deinit(DEV01_BUTTON_GPIO));
  for (unsigned i = 0; i < sizeof(held_pins)/sizeof(held_pins[0]); ++i)
    ESP_ERROR_CHECK(gpio_hold_dis(held_pins[i]));
  if (!waking) {
    duration_s = 60; cycles = 0; last_result = wake_reason = 0;
    entry_ms = exit_ms = slept_ms = accepted_until = 0;
    memset(&accepted, 0, sizeof(accepted));
  } else {
    int64_t elapsed = now_ms() - deep_started_ms - esp_log_timestamp();
    slept_ms = elapsed > 0 ? elapsed : 0;
    wake_reason = cause == ESP_SLEEP_WAKEUP_TIMER ? 1 :
        (cause == ESP_SLEEP_WAKEUP_EXT1 || cause == ESP_SLEEP_WAKEUP_EXT0) ? 4 : 0;
    ++cycles; last_result = wake_reason ? 0 : -EIO;
    state = WAKING;
    accepted_until = now_ms() + 300000; /* Suppress AP-buffered duplicates. */
    ESP_LOGI("dev01_sleep", "Deep WNM wake: elapsed=%lldms reason=%d cycles=%lu",
             (long long)slept_ms, wake_reason, (unsigned long)cycles);
  }
  deep_pending = false;
  retained_magic = RETAINED_MAGIC;
}
void dev01_sleep_online(void) {
  if (state == WAKING) {
    exit_ms = esp_log_timestamp();
    state = IDLE;
    ESP_LOGI("dev01_sleep", "Deep WNM reconnect complete in %lldms", (long long)exit_ms);
  }
}
void dev01_sleep_request_begin(uint32_t peer, const uint8_t *packet, size_t size) {
  request = dev01_sleep_request_decode(peer, packet, size);
}
void dev01_sleep_request_end(void) { request.valid = false; }
/* Run only on the TCP/IP thread. Block ARP, DHCP and queued IP packets without
 * disabling association: WNM management frames use the SDK's separate path. */
static err_t blocked_output(struct netif *netif, struct pbuf *packet) {
  (void)netif; (void)packet; return ERR_IF;
}
static esp_err_t gate_ip(void *arg) {
  esp_netif_t *iface = arg;
  int index = esp_netif_get_netif_impl_index(iface);
  struct netif *netif = index > 0 && index <= UINT8_MAX ? netif_get_by_index(index) : NULL;
  if (!netif || !netif->linkoutput || gated_netif) return ESP_ERR_INVALID_STATE;
  gated_netif = netif; previous_output = netif->linkoutput;
  netif->linkoutput = blocked_output;
  return ESP_OK;
}
static esp_err_t ungate_ip(void *arg) {
  (void)arg;
  if (gated_netif) {
    gated_netif->linkoutput = previous_output;
    gated_netif = NULL; previous_output = NULL;
  }
  return ESP_OK;
}
static void changed(lwm2m_context_t *ctx, uint16_t rid) {
  lwm2m_uri_t uri; LWM2M_URI_RESET(&uri);
  uri.objectId = 31026; uri.instanceId = 0; uri.resourceId = rid;
  lwm2m_resource_value_changed(ctx, &uri);
}
static void finished(lwm2m_context_t *ctx, int result) {
  state = IDLE; last_result = result;
  for (uint16_t rid = 2; rid < RESOURCE_COUNT; ++rid) changed(ctx, rid);
}
static esp_err_t park_radio(void) {
  /* The SDK entry semaphore can be released before its WNM state machine
   * has finished destroying the transport. Wait for RESET-low and the final
   * WAKE/command-veto release before changing any bus pin. */
  int64_t until = now_ms() + 2000;
  while ((REG_READ(GPIO_OUT_REG) & (UINT32_C(1) << CONFIG_MM_RESET_N)) ||
         !dev01_power_radio_quiet()) {
    if (now_ms() >= until) return ESP_ERR_TIMEOUT;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  const gpio_config_t inputs_off = {
      .pin_bit_mask = (UINT64_C(1) << CONFIG_MM_SPI_MISO) |
          (UINT64_C(1) << CONFIG_MM_SPI_IRQ) | (UINT64_C(1) << CONFIG_MM_BUSY),
      .mode = GPIO_MODE_DISABLE};
  esp_err_t rc = gpio_config(&inputs_off);
  if (rc != ESP_OK) return rc;
  const gpio_config_t outputs = {
      .pin_bit_mask = (UINT64_C(1) << CONFIG_MM_SPI_CS) |
          (UINT64_C(1) << CONFIG_MM_SPI_SCK) | (UINT64_C(1) << CONFIG_MM_SPI_MOSI),
      .mode = GPIO_MODE_OUTPUT};
  rc = gpio_config(&outputs);
  if (rc != ESP_OK) return rc;
  if ((rc = gpio_set_level(CONFIG_MM_SPI_CS, 1)) != ESP_OK) return rc;
  if ((rc = gpio_set_level(CONFIG_MM_SPI_SCK, 0)) != ESP_OK) return rc;
  return gpio_set_level(CONFIG_MM_SPI_MOSI, 0);
}
static esp_err_t mcu_sleep(void) {
  dev01_led_off();
  esp_err_t rc = esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  if (rc != ESP_OK) return rc;
  rc = esp_sleep_enable_timer_wakeup((uint64_t)requested_s * 1000000);
  if (rc != ESP_OK) return rc;
  ESP_ERROR_CHECK(rtc_gpio_pullup_en(DEV01_BUTTON_GPIO));
  ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(DEV01_BUTTON_GPIO));
  rc = esp_sleep_enable_ext1_wakeup_io(UINT64_C(1) << DEV01_BUTTON_GPIO, ESP_EXT1_WAKEUP_ANY_LOW);
  if (rc != ESP_OK) {
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    return rc;
  }
  /* The transport has been destroyed by negotiated WNM power-down. Keep
   * powered radio inputs at safe levels; disable dangling MCU input buffers.
   * Flash/PSRAM pads are owned by IDF and must not be reconfigured here. */
  uint64_t low = (UINT64_C(1) << CONFIG_MM_RESET_N) |
      (UINT64_C(1) << CONFIG_MM_WAKE) | (UINT64_C(1) << CONFIG_MM_SPI_SCK) |
      (UINT64_C(1) << CONFIG_MM_SPI_MOSI) | (UINT64_C(1) << GPIO_NUM_20) |
      (UINT64_C(1) << DEV01_LED_GPIO);
  uint64_t high = UINT64_C(1) << CONFIG_MM_SPI_CS;
  gpio_config_t output = {.pin_bit_mask = low | high, .mode = GPIO_MODE_OUTPUT};
  ESP_ERROR_CHECK(gpio_config(&output));
  for (unsigned i = 0; i < sizeof(held_pins)/sizeof(held_pins[0]); ++i) {
    ESP_ERROR_CHECK(gpio_set_level(held_pins[i], (high & (UINT64_C(1) << held_pins[i])) != 0));
    ESP_ERROR_CHECK(gpio_hold_en(held_pins[i]));
  }
  ESP_LOGI("dev01_sleep", "Deep sleep %llds: HaLow/SPI shut down, RESET=0 WAKE=0 CS=1; parking unused GPIOs",
           (long long)requested_s);
  uart_wait_tx_idle_polling(CONFIG_ESP_CONSOLE_UART_NUM);
  uint64_t unused = 0;
  for (int pin = 0; pin < GPIO_NUM_MAX; ++pin) {
    if (!GPIO_IS_VALID_GPIO(pin) || pin == DEV01_BUTTON_GPIO || (pin >= 26 && pin <= 37)) continue;
    if ((low | high) & (UINT64_C(1) << pin)) continue;
    unused |= UINT64_C(1) << pin;
  }
  gpio_config_t off = {.pin_bit_mask = unused, .mode = GPIO_MODE_DISABLE};
  ESP_ERROR_CHECK(gpio_config(&off)); /* No drive, input buffers or pulls. */
  gpio_deep_sleep_hold_en();
  state = SLEEPING;
  deep_pending = true;
  deep_started_ms = now_ms();
  esp_deep_sleep_start(); /* Timer/USER wake reboots, restores holds and reconnects. */
  return ESP_FAIL;
}
void dev01_sleep_step(lwm2m_context_t *ctx, bool online) {
  if (state != PENDING) return;
  if (!online) { finished(ctx, -ENETDOWN); return; }
  if (now_ms() < deadline) return;
  if (ctx->transactionList) {
    if (now_ms() >= drain_deadline) finished(ctx, -EBUSY);
    return;
  }
  if (!awake_lock && esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "wnm", &awake_lock) != ESP_OK) {
    finished(ctx, -ENOMEM); return;
  }
  ESP_ERROR_CHECK(esp_pm_lock_acquire(awake_lock));
  esp_netif_t *iface = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (!iface || esp_netif_tcpip_exec(gate_ip, iface) != ESP_OK) {
    ESP_ERROR_CHECK(esp_pm_lock_release(awake_lock));
    finished(ctx, -ENETDOWN); return;
  }
  bool restore_ps = dev01_power_save_requested();
  if (!restore_ps && dev01_set_radio_power_save(true) != ESP_OK) {
    (void)esp_netif_tcpip_exec(ungate_ip, NULL);
    ESP_ERROR_CHECK(esp_pm_lock_release(awake_lock));
    finished(ctx, -EIO); return;
  }
  struct mmwlan_set_wnm_sleep_enabled_args args = MMWLAN_SET_WNM_SLEEP_ENABLED_ARGS_INIT;
  args.wnm_sleep_enabled = true;
  args.chip_powerdown_enabled = true; /* Same negotiated power-down mode as nRF54. */
  int64_t start = now_ms();
  enum mmwlan_status enter = mmwlan_set_wnm_sleep_enabled_ext(&args);
  entry_ms = now_ms() - start;
  if (enter != MMWLAN_SUCCESS) {
    (void)dev01_set_radio_power_save(restore_ps);
    (void)esp_netif_tcpip_exec(ungate_ip, NULL);
    ESP_ERROR_CHECK(esp_pm_lock_release(awake_lock));
    ESP_LOGW("dev01_sleep", "WNM entry failed: %d", enter);
    finished(ctx, -(int)enter); return;
  }
  ESP_LOGI("dev01_sleep", "WNM power-down entered; sleeping %lld seconds", (long long)requested_s);
  esp_err_t sleep_rc = park_radio();
  entry_ms = now_ms() - start;
  if (sleep_rc == ESP_OK) sleep_rc = mcu_sleep();
  state = WAKING;
  start = now_ms();
  args.wnm_sleep_enabled = false;
  enum mmwlan_status exit = mmwlan_set_wnm_sleep_enabled_ext(&args);
  exit_ms = now_ms() - start;
  dev01_power_restore_wake();
  esp_err_t restore_rc = dev01_set_radio_power_save(restore_ps);
  /* The SDK restores the chip even when the AP exit handshake times out. */
  (void)esp_netif_tcpip_exec(ungate_ip, NULL);
  ESP_ERROR_CHECK(esp_pm_lock_release(awake_lock));
  if (sleep_rc == ESP_OK) ++cycles;
  ESP_LOGI("dev01_sleep", "WNM exit=%d MCU sleep=%d elapsed=%lldms wake=%d", exit, sleep_rc, (long long)slept_ms, wake_reason);
  finished(ctx, exit != MMWLAN_SUCCESS ? -(int)exit : sleep_rc == ESP_OK && restore_rc == ESP_OK ? 0 : -EIO);
}
static uint8_t discover(lwm2m_context_t *ctx, uint16_t instance, int *count,
                        lwm2m_data_t **data, lwm2m_object_t *object) {
  (void)ctx; (void)object;
  if (instance) return COAP_404_NOT_FOUND;
  if (!*count) {
    *data = lwm2m_data_new(RESOURCE_COUNT);
    if (!*data) return COAP_500_INTERNAL_SERVER_ERROR;
    *count = RESOURCE_COUNT;
    for (int i = 0; i < *count; ++i) (*data)[i].id = i;
  }
  for (int i = 0; i < *count; ++i)
    if ((*data)[i].id >= RESOURCE_COUNT) return COAP_404_NOT_FOUND;
  return COAP_205_CONTENT;
}
static uint8_t read_sleep(lwm2m_context_t *ctx, uint16_t instance, int *count,
                         lwm2m_data_t **data, lwm2m_object_t *object) {
  if (!*count) {
    if (instance) return COAP_404_NOT_FOUND;
    *data = lwm2m_data_new(RESOURCE_COUNT - 1);
    if (!*data) return COAP_500_INTERNAL_SERVER_ERROR;
    *count = RESOURCE_COUNT - 1;
    for (int i = 0; i < *count; ++i) (*data)[i].id = i + 1;
  }
  uint8_t rc = discover(ctx, instance, count, data, object);
  if (rc != COAP_205_CONTENT) return rc;
  for (int i = 0; i < *count; ++i) {
    lwm2m_data_t *r = *data + i;
    int64_t value;
    switch (r->id) {
    case 0: return COAP_405_METHOD_NOT_ALLOWED;
    case 1: value = duration_s; break;
    case 2: value = state; break;
    case 3: value = last_result; break;
    case 4: value = cycles; break;
    case 5: value = entry_ms; break;
    case 6: value = exit_ms; break;
    case 7: value = slept_ms; break;
    case 8: value = 0; break; /* No HC33 IMU wake support. */
    case 9: value = 500; break;
    case 10: value = 60; break;
    case 11: value = wake_reason; break;
    default: return COAP_404_NOT_FOUND;
    }
    lwm2m_data_encode_int(value, r);
  }
  return COAP_205_CONTENT;
}
static uint8_t write_sleep(lwm2m_context_t *ctx, uint16_t instance, int count,
                          lwm2m_data_t *data, lwm2m_object_t *object, lwm2m_write_type_t type) {
  (void)object; (void)type;
  if (instance) return COAP_404_NOT_FOUND;
  if (state != IDLE) return COAP_503_SERVICE_UNAVAILABLE;
  if (count != 1 || !data) return COAP_400_BAD_REQUEST;
  if (data->id >= RESOURCE_COUNT) return COAP_404_NOT_FOUND;
  if (data->id != 1) return COAP_405_METHOD_NOT_ALLOWED;
  int64_t value;
  if (!lwm2m_data_decode_int(data, &value) || value < 1 || value > 86400) return COAP_400_BAD_REQUEST;
  duration_s = value; changed(ctx, 1);
  return COAP_204_CHANGED;
}
static uint8_t execute_sleep(lwm2m_context_t *ctx, uint16_t instance, uint16_t rid,
                            uint8_t *buffer, int length, lwm2m_object_t *object) {
  (void)buffer; (void)object;
  if (instance || rid >= RESOURCE_COUNT) return COAP_404_NOT_FOUND;
  if (rid) return COAP_405_METHOD_NOT_ALLOWED;
  if (length) return COAP_400_BAD_REQUEST;
  if (now_ms() < accepted_until && dev01_sleep_request_same(&request, &accepted)) return COAP_204_CHANGED;
  if (state != IDLE || !ctx || ctx->state != STATE_READY) return COAP_503_SERVICE_UNAVAILABLE;
  requested_s = duration_s; accepted = request;
  accepted_until = now_ms() + (requested_s + 300) * 1000;
  state = PENDING; last_result = wake_reason = 0; slept_ms = entry_ms = exit_ms = 0;
  deadline = now_ms() + RESPONSE_GRACE_MS;
  drain_deadline = deadline + DRAIN_LIMIT_MS;
  changed(ctx, 2);
  return COAP_204_CHANGED;
}
static lwm2m_list_t instance = {.id = 0};
static lwm2m_object_t object = {.objID = 31026, .instanceList = &instance,
    .readFunc = read_sleep, .writeFunc = write_sleep, .executeFunc = execute_sleep,
    .discoverFunc = discover};
lwm2m_object_t *dev01_sleep_object(void) { return &object; }
