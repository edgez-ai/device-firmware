/* SPDX-License-Identifier: Apache-2.0 */
#include "hardware_uart.h"
#include "hardware_config.h"
#include <errno.h>
#include <hal/nrf_uarte.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/pinctrl.h>
#define UART_NODE DT_NODELABEL(uart22)
PINCTRL_DT_DEV_CONFIG_DECLARE(UART_NODE);
PINCTRL_DT_STATE_PINS_DEFINE(DT_PATH(zephyr_user), pinctrl_0);
PINCTRL_DT_STATE_PINS_DEFINE(DT_PATH(zephyr_user), pinctrl_1);
static const struct pinctrl_state external_states[] = {
 PINCTRL_DT_STATE_INIT(pinctrl_0, PINCTRL_STATE_DEFAULT),
 PINCTRL_DT_STATE_INIT(pinctrl_1, PINCTRL_STATE_SLEEP),
};
static const struct device *const uart = DEVICE_DT_GET(UART_NODE);
static const struct gpio_dt_spec power = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), radar_power_gpios);
int hardware_uart_prepare(void)
{
 /* Release any UART state retained by a debug reset before parking pads. */
 nrf_uarte_disable((NRF_UARTE_Type *)DT_REG_ADDR(UART_NODE));
 if (!gpio_is_ready_dt(&power)) return -ENODEV;
 int rc = gpio_pin_configure_dt(&power, GPIO_OUTPUT_INACTIVE);
 if (rc) return rc;
 /* Disconnect both signal pins while the external supply is off. */
 rc = gpio_pin_configure(power.port, 11, GPIO_DISCONNECTED);
 if (!rc) rc = gpio_pin_configure(power.port, 12, GPIO_DISCONNECTED);
 if (rc) return rc;
 if (hardware_mode_get() != HARDWARE_CAMERA) {
  rc = pinctrl_update_states(PINCTRL_DT_DEV_CONFIG_GET(UART_NODE), external_states, ARRAY_SIZE(external_states));
 }
 return rc;
}
int hardware_uart_init(void)
{
 enum hardware_mode mode = hardware_mode_get();
 if (mode == HARDWARE_BASE) return -ENODEV;
 int rc = 0;
 if (!device_is_ready(uart)) rc = device_init(uart);
 if (rc) return rc;
 struct uart_config config = {.baudrate = mode == HARDWARE_CAMERA ? 921600 : mode == HARDWARE_GPS ? 9600 : 115200,
  .parity = UART_CFG_PARITY_NONE, .stop_bits = UART_CFG_STOP_BITS_1,
  .data_bits = UART_CFG_DATA_BITS_8, .flow_ctrl = UART_CFG_FLOW_CTRL_NONE};
 return uart_configure(uart, &config);
}
