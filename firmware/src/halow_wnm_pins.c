/* SPDX-License-Identifier: Apache-2.0 */
#include "halow_wnm_pins.h"
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/device.h>
#include <hal/nrf_gpio.h>

#define RADIO DT_NODELABEL(morse_wifi)
#define ABS_PIN(node, prop, idx) \
    NRF_GPIO_PIN_MAP(DT_PROP(DT_GPIO_CTLR_BY_IDX(node, prop, idx), port), \
                     DT_GPIO_PIN_BY_IDX(node, prop, idx))
#define PASSIVE_PIN(node, prop, idx) ABS_PIN(node, prop, idx),
static const uint32_t pins[] = {
    /* Keep these three outputs defined while the module supply remains on. */
    ABS_PIN(RADIO, resetn_gpios, 0),
    ABS_PIN(RADIO, wakeup_gpios, 0),
    ABS_PIN(DT_BUS(RADIO), cs_gpios, 0),
    DT_FOREACH_PROP_ELEM(DT_PATH(zephyr_user), wnm_park_gpios, PASSIVE_PIN)
};
static const struct device *const spi = DEVICE_DT_GET(DT_BUS(RADIO));
static struct { uint32_t config, output; } saved[ARRAY_SIZE(pins)];
static bool parked, resume_spi;
/* Passive SWD diagnostics: parked, completed parks, completed restores, error.
 * No logging or extra wake timer is introduced for the measurement. */
volatile int32_t halow_wnm_pin_diag[4];

static uint32_t config_get(uint32_t pin)
{
    NRF_GPIO_Type *port = nrf_gpio_pin_port_decode(&pin);
    return port->PIN_CNF[pin];
}
static void config_set(uint32_t pin, uint32_t config)
{
    NRF_GPIO_Type *port = nrf_gpio_pin_port_decode(&pin);
    port->PIN_CNF[pin] = config;
}

int halow_wnm_pins_park(void)
{
    if (parked) return -EALREADY;
    enum pm_device_state state;
    int rc = pm_device_state_get(spi, &state);
    if (rc) return rc;
    if (state != PM_DEVICE_STATE_ACTIVE && state != PM_DEVICE_STATE_SUSPENDED)
        return -EBUSY;
    /* The successful WNM powerdown has stopped SDK transport/PS threads and
     * disabled IRQ/BUSY interrupts. Do not change nrfx GPIOTE allocations:
     * save/restore PIN_CNF directly, leaving its disabled IRQ bookkeeping intact. */
    for (unsigned int i = 0; i < ARRAY_SIZE(pins); ++i) {
        saved[i].config = config_get(pins[i]);
        saved[i].output = nrf_gpio_pin_out_read(pins[i]);
    }
    resume_spi = state == PM_DEVICE_STATE_ACTIVE;
    if (resume_spi) {
        rc = pm_device_action_run(spi, PM_DEVICE_ACTION_SUSPEND);
        if (rc) { resume_spi = false; halow_wnm_pin_diag[3] = rc; return rc; }
    }
    unsigned int key = irq_lock();
    for (unsigned int i = 0; i < 3; ++i) {
        nrf_gpio_pin_write(pins[i], i == 2); /* Physical RESETn/WAKE LOW, CS HIGH. */
        nrf_gpio_cfg_output(pins[i]); /* No pull or digital input buffer. */
    }
    for (unsigned int i = 3; i < ARRAY_SIZE(pins); ++i)
        nrf_gpio_cfg_default(pins[i]); /* High impedance, no pull, input disconnected. */
    parked = true;
    halow_wnm_pin_diag[0] = 1;
    ++halow_wnm_pin_diag[1];
    halow_wnm_pin_diag[3] = 0;
    irq_unlock(key);
    return 0;
}

int halow_wnm_pins_restore(void)
{
    if (!parked) return 0;
    int rc = 0;
    if (resume_spi) {
        rc = pm_device_action_run(spi, PM_DEVICE_ACTION_RESUME);
        resume_spi = false;
    }
    unsigned int key = irq_lock();
    /* Restore output latches before directions to avoid output-level glitches.
     * Includes pulls, drive strength, input buffers and SENSE from before park. */
    for (unsigned int i = 0; i < ARRAY_SIZE(pins); ++i)
        nrf_gpio_pin_write(pins[i], saved[i].output);
    for (unsigned int i = 0; i < ARRAY_SIZE(pins); ++i)
        config_set(pins[i], saved[i].config);
    parked = false;
    halow_wnm_pin_diag[0] = 0;
    ++halow_wnm_pin_diag[2];
    halow_wnm_pin_diag[3] = rc;
    irq_unlock(key);
    return rc;
}
