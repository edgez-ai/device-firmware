/* SPDX-License-Identifier: Apache-2.0 */
#include "prov_power.h"
#include "sensors_hw.h"
#include <hal/nrf_gpio.h>
#include <hal/nrf_uarte.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/timer/system_timer.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/poweroff.h>

#define RADIO DT_NODELABEL(morse_wifi)
#define PIN(node, prop, i)                                                     \
  NRF_GPIO_PIN_MAP(DT_PROP(DT_GPIO_CTLR_BY_IDX(node, prop, i), port),          \
                   DT_GPIO_PIN_BY_IDX(node, prop, i))
#define PASSIVE(node, prop, i) PIN(node, prop, i),
static const uint32_t radio_signals[] = {
    PIN(RADIO, resetn_gpios, 0), PIN(RADIO, wakeup_gpios, 0),
    PIN(DT_BUS(RADIO), cs_gpios, 0),
    DT_FOREACH_PROP_ELEM(DT_PATH(zephyr_user), wnm_park_gpios, PASSIVE)};
/* Passive SWD diagnostics; no logging, timer or periodic wake-up. */
volatile int prov_power_error;
int prov_power_sleep(void) {
  int rc = board_prov_motion_arm();
  prov_power_error = rc;
  if (rc)
    return rc; /* Never power off without a working wake source. */
  /* From here no other thread/ISR may access the buses or re-power a rail.
   * This is terminal shutdown, not the connected-device WNM path. */
  (void)irq_lock();
  uint32_t radio_power = PIN(DT_NODELABEL(halow_power), enable_gpios, 0);
  nrf_gpio_pin_clear(radio_power);
  nrf_gpio_cfg_output(radio_power);
  for (unsigned i = 0; i < ARRAY_SIZE(radio_signals); ++i)
    nrf_gpio_cfg_default(radio_signals[i]);
  uint32_t external_power = PIN(DT_PATH(zephyr_user), radar_power_gpios, 0);
  nrf_gpio_pin_clear(external_power);
  nrf_gpio_cfg_output(external_power);
  nrf_uarte_disable((NRF_UARTE_Type *)DT_REG_ADDR(DT_NODELABEL(uart22)));
  nrf_gpio_cfg_default(NRF_GPIO_PIN_MAP(1, 11));
  nrf_gpio_cfg_default(NRF_GPIO_PIN_MAP(1, 12));
  /* Do not retain the normal button/indicator as alternate sleep wake sources.
   */
  nrf_gpio_cfg_sense_set(PIN(DT_ALIAS(sw0), gpios, 0), NRF_GPIO_PIN_NOSENSE);
  uint32_t led = PIN(DT_PATH(zephyr_user), presence_test_led_gpios, 0);
  nrf_gpio_pin_write(
      led, (DT_GPIO_FLAGS(DT_PATH(zephyr_user), presence_test_led_gpios) &
            GPIO_ACTIVE_LOW) != 0);
  nrf_gpio_cfg_output(led);
  board_prov_motion_sense();
  /* The kernel's GRTC compare must not periodically wake transport standby. */
  sys_clock_disable();
  sys_poweroff();
  CODE_UNREACHABLE;
}
int prov_power_boot_check(void) {
  uint32_t cause = 0;
  int rc = hwinfo_get_reset_cause(&cause);
  if (rc)
    return rc;
  (void)hwinfo_clear_reset_cause();
  if (!(cause & RESET_LOW_POWER_WAKE))
    return 0;
  rc = board_prov_motion_verify();
  if (rc == 1)
    return 0;
  /* An invalid gesture or failed validation never enables Bluetooth. */
  return prov_power_sleep();
}
