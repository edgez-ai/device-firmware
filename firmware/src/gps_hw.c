/* SPDX-License-Identifier: Apache-2.0 */
#if defined(CONFIG_EZ_CAM_UART_GPS)
#include "gps_hw.h"
#include "halow_light.h"
#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/ring_buffer.h>

static const struct device *const uart = DEVICE_DT_GET(DT_ALIAS(gps0));
static const struct gpio_dt_spec power = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), gps_power_gpios);
PINCTRL_DT_DEFINE(DT_ALIAS(gps0));
static const struct pinctrl_dev_config *const pins = PINCTRL_DT_DEV_CONFIG_GET(DT_ALIAS(gps0));
RING_BUF_DECLARE(gps_rx, 512);
static struct k_spinlock lock;
static bool configured;
static void wake_handler(struct k_work *work) { ARG_UNUSED(work); halow_light_wake(); }
K_WORK_DEFINE(gps_wake_work, wake_handler);
static void rx_handler(const struct device *dev, void *unused)
{
    ARG_UNUSED(unused);
    if (!uart_irq_update(dev) || !uart_irq_rx_ready(dev)) return;
    uint8_t data[32]; int count;
    while ((count = uart_fifo_read(dev, data, sizeof(data))) > 0) {
        k_spinlock_key_t key = k_spin_lock(&lock);
        (void)ring_buf_put(&gps_rx, data, count);
        k_spin_unlock(&lock, key);
    }
    /* eventfd operations run on the system workqueue, never in the UART ISR. */
    (void)k_work_submit(&gps_wake_work);
}
int gps_hw_stop(void)
{
    int rc = 0;
    if (device_is_ready(uart)) {
        uart_irq_rx_disable(uart); uart_irq_tx_disable(uart); uart_irq_err_disable(uart);
        rc = pm_device_action_run(uart, PM_DEVICE_ACTION_SUSPEND);
        if (rc == -EALREADY) rc = 0;
    }
    int pin_rc = pinctrl_apply_state(pins, PINCTRL_STATE_SLEEP);
    int power_rc = gpio_pin_set_dt(&power, 0);
    k_spinlock_key_t key = k_spin_lock(&lock);
    ring_buf_reset(&gps_rx);
    k_spin_unlock(&lock, key);
    return rc ? rc : pin_rc ? pin_rc : power_rc;
}
int gps_hw_init(void)
{
    if (!gpio_is_ready_dt(&power)) return -ENODEV;
    int rc = gpio_pin_configure_dt(&power, GPIO_OUTPUT_INACTIVE);
    return rc ? rc : gps_hw_stop();
}
int gps_hw_start(void)
{
    /* Bring up the supply while UART pads are still high impedance. */
    int rc = gpio_pin_set_dt(&power, 1);
    if (rc) return rc;
    if (!configured) {
        if (!device_is_ready(uart)) {
            rc = device_init(uart);
            if (rc) goto failed;
        }
        rc = uart_irq_callback_user_data_set(uart, rx_handler, NULL);
        if (rc) goto failed;
        configured = true;
    }
    rc = pm_device_action_run(uart, PM_DEVICE_ACTION_RESUME);
    if (rc == -EALREADY) rc = 0;
    if (!rc) { uart_irq_rx_enable(uart); return 0; }
failed:
    (void)gps_hw_stop();
    return rc;
}
int gps_hw_read(uint8_t *data, size_t length)
{
    k_spinlock_key_t key = k_spin_lock(&lock);
    int count = ring_buf_get(&gps_rx, data, length);
    k_spin_unlock(&lock, key);
    return count;
}
#endif
