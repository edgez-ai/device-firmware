/* SPDX-License-Identifier: Apache-2.0 */
#include "dev01_board.h"
#include "dev01_power.h"
#include "freertos/FreeRTOS.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"
#include <inttypes.h>
#include <stdio.h>

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE stats_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_pm_lock_handle_t radio_lock;
static uint32_t vetoes;
static bool radio_awake, locked, enabled;
static uint64_t attempts, successes, duration_us;
static unsigned last_wake;
static int64_t window_us;

/* The driver's command vetoes and WAKE transitions may originate on either
 * core. They are idempotent flags, unlike the ref-counted ESP-IDF PM lock. */
static void update_locked(void) {
  bool need = radio_awake || vetoes;
  if (need != locked) {
    ESP_ERROR_CHECK(need ? esp_pm_lock_acquire(radio_lock)
                         : esp_pm_lock_release(radio_lock));
    locked = need;
  }
}
void dev01_power_veto(uint8_t id, bool set) {
  if (!radio_lock || id >= 32)
    return;
  portENTER_CRITICAL(&lock);
  if (set) vetoes |= UINT32_C(1) << id;
  else vetoes &= ~(UINT32_C(1) << id);
  update_locked();
  portEXIT_CRITICAL(&lock);
}
void dev01_power_radio_awake(bool awake) {
  if (!radio_lock) return;
  portENTER_CRITICAL(&lock);
  radio_awake = awake;
  update_locked();
  portEXIT_CRITICAL(&lock);
}
bool dev01_power_radio_quiet(void) {
  portENTER_CRITICAL(&lock);
  bool quiet = !radio_awake && !vetoes;
  portEXIT_CRITICAL(&lock);
  return quiet;
}
#if CONFIG_EZ_DEV01_AUTO_SLEEP
static esp_err_t after_sleep(int64_t elapsed_us, void *arg) {
  (void)arg;
  /* EXT wakeup temporarily routes these RTC-capable pins away from GPIO. */
  rtc_gpio_deinit(CONFIG_MM_BUSY);
  rtc_gpio_deinit(DEV01_BUTTON_GPIO);
  /* An edge arriving while BUSY was RTC-routed can be absent from the
   * digital edge latch. Pend the normal GPIO ISR after restoring routing;
   * its FreeRTOS FromISR callback must run in real interrupt context. */
  if (elapsed_us > 0 && esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1 &&
      gpio_get_level(CONFIG_MM_BUSY))
    REG_WRITE(GPIO_STATUS_W1TS_REG, UINT32_C(1) << CONFIG_MM_BUSY);
  portENTER_CRITICAL(&stats_lock);
  if (elapsed_us > 0) {
    attempts++;
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause != ESP_SLEEP_WAKEUP_UNDEFINED) {
      successes++;
      duration_us += elapsed_us;
      last_wake = cause;
    }
  }
  portEXIT_CRITICAL(&stats_lock);
  return ESP_OK;
}
#endif
void dev01_power_init(void) {
  ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0,
                                   "halow_bus", &radio_lock));
}
void dev01_power_restore_wake(void) {
#if CONFIG_EZ_DEV01_AUTO_SLEEP
  ESP_ERROR_CHECK(rtc_gpio_pullup_en(DEV01_BUTTON_GPIO));
  ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(DEV01_BUTTON_GPIO));
  ESP_ERROR_CHECK(gpio_pulldown_dis(CONFIG_MM_BUSY));
  ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(
      UINT64_C(1) << CONFIG_MM_BUSY, ESP_EXT1_WAKEUP_ANY_HIGH));
  ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(DEV01_BUTTON_GPIO, 0));
#endif
}
void dev01_power_start(void) {
#if CONFIG_EZ_DEV01_AUTO_SLEEP
  /* Enable only after provisioning/BLE has finished and the radio is ready.
   * Preserve CS/RESET/WAKE and SPI output latches. Do not isolate a powered
   * radio's control lines or flash/PSRAM pins. Disable input/pull leakage on
   * the receive-only SPI pins during sleep; active configuration is restored
   * by the hardware on wake. */
  const gpio_num_t outputs[] = {CONFIG_MM_RESET_N, CONFIG_MM_WAKE,
      CONFIG_MM_SPI_CS, CONFIG_MM_SPI_SCK, CONFIG_MM_SPI_MOSI};
  for (unsigned i = 0; i < sizeof(outputs)/sizeof(outputs[0]); i++)
    ESP_ERROR_CHECK(gpio_sleep_sel_dis(outputs[i]));
  const gpio_num_t inputs[] = {CONFIG_MM_SPI_MISO, CONFIG_MM_SPI_IRQ};
  for (unsigned i = 0; i < sizeof(inputs)/sizeof(inputs[0]); i++) {
    ESP_ERROR_CHECK(gpio_sleep_set_direction(inputs[i], GPIO_MODE_DISABLE));
    ESP_ERROR_CHECK(gpio_sleep_set_pull_mode(inputs[i], GPIO_FLOATING));
    ESP_ERROR_CHECK(gpio_sleep_sel_en(inputs[i]));
  }
  /* External wake circuits are independent of the Morse BUSY rising-edge ISR.
   * IRQ is intentionally not a wake source: it stays low during radio sleep. */
  dev01_power_restore_wake();
  esp_pm_sleep_cbs_register_config_t callbacks = {.exit_cb = after_sleep};
  ESP_ERROR_CHECK(esp_pm_light_sleep_register_cbs(&callbacks));
  window_us = esp_timer_get_time();
  esp_pm_config_t config = {.max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
                            .min_freq_mhz = 40, .light_sleep_enable = true};
  ESP_ERROR_CHECK(esp_pm_configure(&config));
  enabled = true;
  ESP_LOGI("dev01_power", "Automatic MCU light sleep enabled; BUSY/button wake, radio command/WAKE veto");
#else
  esp_pm_config_t config = {.max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
                            .min_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
                            .light_sleep_enable = false};
  ESP_ERROR_CHECK(esp_pm_configure(&config));
  enabled = false;
  window_us = esp_timer_get_time();
  ESP_LOGI("dev01_power", "Automatic MCU light sleep disabled; CPU stays awake");
#endif
}
int dev01_power_report(char *out, size_t size) {
  int64_t now = esp_timer_get_time();
  portENTER_CRITICAL(&stats_lock);
  uint64_t count = attempts, ok = successes, us = duration_us;
  unsigned wake = last_wake;
  int64_t elapsed = now - window_us;
  attempts = successes = duration_us = 0;
  window_us = now;
  portEXIT_CRITICAL(&stats_lock);
  /* Require a real wake cause to count a successful sleep. PM time includes
   * entry/exit overhead; it is not precise hardware residency. */
  return snprintf(out, size, " mcu_ls=%u ls_try=%" PRIu64
      " ls_ok=%" PRIu64 " ls_pm_ms=%" PRIu64 " ls_win_ms=%" PRIu64 " ls_wake=%u",
      (unsigned)enabled, count, ok, us/1000, (uint64_t)(elapsed/1000), wake);
}
