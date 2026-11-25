/* SPDX-License-Identifier: Apache-2.0
 * EdgeZ carrier LSM6DS3TR-C and switched 100k/100k battery divider.
 * IMU VDD/VDDIO are wired to 3.3 V: ODR=0 is power-down, not a rail cut.
 */
#include "sensors_hw.h"
#include "prov_motion.h"
#include "factory_data.h"
#include <hal/nrf_gpio.h>
#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/byteorder.h>
#include <hal/nrf_saadc.h>

static const struct i2c_dt_spec imu = I2C_DT_SPEC_GET(DT_ALIAS(imu0));
static const struct adc_dt_spec battery = ADC_DT_SPEC_GET(DT_PATH(zephyr_user));
static const struct gpio_dt_spec battery_en =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), battery_enable_gpios);
static const struct gpio_dt_spec battery_pin =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), battery_input_gpios);
static const struct gpio_dt_spec imu_irq = GPIO_DT_SPEC_GET(DT_ALIAS(imu0), irq_gpios);
PINCTRL_DT_DEV_CONFIG_DECLARE(DT_NODELABEL(i2c30));
static const struct pinctrl_dev_config *const imu_pins =
	PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(i2c30));
static unsigned int observed_mask, hardware_mask;
static bool initialized;
static volatile int last_imu_error, last_voltage_error;

int board_sensors_error(void) { return last_imu_error ? last_imu_error : last_voltage_error; }

static int pm_action(const struct device *dev, enum pm_device_action action)
{
	int rc = pm_device_action_run(dev, action);
	return rc == -EALREADY ? 0 : rc;
}
static int bus_start(void)
{
	if (!device_is_ready(imu.bus)) {
		int rc = device_init(imu.bus);
		if (rc) return rc;
	}
	return pm_action(imu.bus, PM_DEVICE_ACTION_RESUME);
}
static int bus_stop(void)
{
	int rc = device_is_ready(imu.bus) ? pm_action(imu.bus, PM_DEVICE_ACTION_SUSPEND) : 0;
	int pins = pinctrl_apply_state(imu_pins, PINCTRL_STATE_SLEEP);
	return rc ? rc : pins;
}
static int write_reg(uint8_t reg, uint8_t value)
{
	return i2c_reg_write_byte_dt(&imu, reg, value);
}
static int set_odr(unsigned int mask)
{
	/* Always attempt BOTH writes, including shutdown after a failed read. */
	int accel = write_reg(0x10, (mask & EZ_IMU_ACCEL) ? 0x10 : 0);
	int gyro = write_reg(0x11, (mask & EZ_IMU_GYRO) ? 0x12 : 0);
	if (!accel && !gyro) hardware_mask = mask;
	return accel ? accel : gyro;
}
static int configure_imu(void)
{
	uint8_t who;
	int rc = write_reg(0x01, 0); /* main register bank */
	if (!rc) rc = i2c_reg_read_byte_dt(&imu, 0x0f, &who);
	if (!rc && who != 0x6a) rc = -ENODEV;
	if (!rc) rc = write_reg(0x12, 0x44); /* BDU + register auto-increment */
	if (!rc) rc = write_reg(0x0a, 0); /* FIFO bypass */
	if (!rc) rc = write_reg(0x0d, 0); /* INT1 off */
	if (!rc) rc = write_reg(0x0e, 0); /* INT2 off */
	if (!rc) rc = write_reg(0x19, 0); /* embedded functions off */
	if (!rc) rc = write_reg(0x58, 0); /* disable latched motion wake */
	if (!rc) rc = write_reg(0x5e, 0); /* MD1_CFG */
	if (!rc) rc = write_reg(0x5f, 0); /* MD2_CFG */
	if (!rc) rc = write_reg(0x15, 0x10); /* accel low-power mode */
	if (!rc) rc = write_reg(0x16, 0x80); /* gyro low-power mode */
	int off = set_odr(0);
	if (!rc) rc = off;
	initialized = rc == 0;
	return rc;
}

int board_sensors_init(void)
{
	if (!gpio_is_ready_dt(&battery_en) || !gpio_is_ready_dt(&battery_pin) ||
	    !gpio_is_ready_dt(&imu_irq)) return -ENODEV;
	int rc = gpio_pin_configure_dt(&battery_en, GPIO_OUTPUT_INACTIVE);
	if (!rc) rc = gpio_pin_configure_dt(&battery_pin, GPIO_DISCONNECTED);
	if (!rc) rc = gpio_pin_configure_dt(&imu_irq, GPIO_DISCONNECTED);
	if (rc) return rc;
	/* No ADC init/sample at boot. Clear inherited IMU ODR once, then park I2C.
	 * No worker, FIFO drain or periodic interrupt is enabled while unobserved. */
	rc = bus_start();
	if (!rc) rc = configure_imu();
	int stop = bus_stop();
	last_imu_error = rc ? rc : stop;
	return last_imu_error;
}
int board_imu_observe(unsigned int mask)
{
	if (mask & ~(EZ_IMU_ACCEL | EZ_IMU_GYRO)) return -EINVAL;
	observed_mask = mask;
	int rc = bus_start();
	if (!rc && !initialized) rc = configure_imu();
	if (!rc) rc = set_odr(mask);
	if (rc && device_is_ready(imu.bus)) (void)set_odr(0);
	int stop = bus_stop();
	last_imu_error = rc ? rc : stop;
	return last_imu_error;
}
int board_imu_read(unsigned int sensor, double axes[3])
{
	if (!axes || (sensor != EZ_IMU_ACCEL && sensor != EZ_IMU_GYRO)) return -EINVAL;
	uint8_t raw[6], status;
	int rc = bus_start();
	if (!rc && !initialized) rc = configure_imu();
	unsigned int wanted = observed_mask | sensor;
	if (!rc && wanted != hardware_mask) {
		rc = set_odr(wanted);
		/* Discard an old ready flag/sample after changing ODR. */
		if (!rc) rc = i2c_burst_read_dt(&imu, sensor == EZ_IMU_ACCEL ? 0x28 : 0x22, raw, sizeof(raw));
	}
	int64_t deadline = k_uptime_get() + 300;
	uint8_t ready_bit = sensor == EZ_IMU_ACCEL ? 1 : 2;
	while (!rc) {
		rc = i2c_reg_read_byte_dt(&imu, 0x1e, &status);
		if (rc || (status & ready_bit)) break;
		if (k_uptime_get() >= deadline) { rc = -ETIMEDOUT; break; }
		k_msleep(2);
	}
	if (!rc) rc = i2c_burst_read_dt(&imu, sensor == EZ_IMU_ACCEL ? 0x28 : 0x22, raw, sizeof(raw));
	if (!rc) {
		double scale = sensor == EZ_IMU_ACCEL ? 0.00059820565 : 0.00007635815;
		for (int i = 0; i < 3; ++i) axes[i] = (int16_t)sys_get_le16(&raw[2*i]) * scale;
	}
	/* Standalone Read powers both engines down; Observe retains only its ODRs.
	 * I2C is suspended between transfers even while IMU observation is active. */
	int off = device_is_ready(imu.bus) ? set_odr(rc ? 0 : observed_mask) : 0;
	int stop = bus_stop();
	if (!rc) rc = off ? off : stop;
	last_imu_error = rc;
	return rc;
}
int board_voltage_read(double *volts)
{
	if (!volts) return -EINVAL;
	int rc = 0;
	if (!adc_is_ready_dt(&battery)) rc = device_init(battery.dev);
	if (!rc) rc = pm_action(battery.dev, PM_DEVICE_ACTION_RESUME);
	if (!rc) {
		struct adc_channel_cfg cfg = battery.channel_cfg;
		cfg.acquisition_time = ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, 40);
		rc = adc_channel_setup(battery.dev, &cfg);
	}
	if (!rc) rc = gpio_pin_set_dt(&battery_en, 1);
	if (!rc) {
		int16_t raw = 0;
		struct adc_sequence sequence = {0};
		rc = adc_sequence_init_dt(&battery, &sequence);
		if (!rc) {
			sequence.buffer = &raw; sequence.buffer_size = sizeof(raw);
			sequence.calibrate = true;
			/* Calibrate/discard first, then allow the actual ADC reference and
			 * switched divider to settle before the measurement conversion. */
			rc = adc_read_dt(&battery, &sequence);
			sequence.calibrate = false;
			if (!rc) {
				nrf_saadc_enable(NRF_SAADC);
				k_msleep(100);
				rc = adc_read_dt(&battery, &sequence);
			}
		}
		int32_t mv = raw;
		if (!rc) rc = adc_raw_to_millivolts_dt(&battery, &mv);
		if (!rc && mv < 0) rc = -ERANGE;
		if (!rc) {
			/* R26=R29=100k. Optional factory/board calibration is separate
			 * from the physical divider ratio and defaults to unity. */
			int64_t corrected_mv = ((int64_t)mv * 2 * CONFIG_EZ_DEV54_VOLTAGE_SCALE_PPM + 500000) / 1000000;
			*volts = corrected_mv / 1000.0;
		}
	}
	/* Own the SAADC exclusively. nrfx also disables it on completion; enforce
	 * this on error exits too. Each subsequent nrfx conversion enables it. */
	if (device_is_ready(battery.dev)) {
		nrf_saadc_disable(NRF_SAADC);
		nrf_saadc_disable(NRF_SAADC);
	}
	int power = gpio_pin_set_dt(&battery_en, 0);
	int pin = gpio_pin_configure_dt(&battery_pin, GPIO_DISCONNECTED);
	int stop = device_is_ready(battery.dev) ? pm_action(battery.dev, PM_DEVICE_ACTION_SUSPEND) : 0;
	if (!rc) rc = power ? power : (pin ? pin : stop);
	last_voltage_error = rc;
	return rc;
}

#if defined(CONFIG_EZ_DEV54_PROV_MOTION_SLEEP)
int board_prov_motion_verify(void)
{
    if (observed_mask) return -EBUSY;
    int rc = bus_start();
    if (!rc) rc = configure_imu();
    /* 104 Hz, +/-8 g, low-power accelerometer; gyro remains off. */
    if (!rc) rc = write_reg(0x10, 0x4c);
    uint8_t raw[6], ready;
    struct prov_motion motion = {0};
    const uint16_t threshold_mg = ez_factory_shake_threshold_mg();
    bool accepted = false;
    int64_t deadline = k_uptime_get() + PROV_MOTION_WINDOW_MS;
    while (!rc && k_uptime_get() < deadline) {
        rc = i2c_reg_read_byte_dt(&imu, 0x1e, &ready);
        if (rc) break;
        if (ready & 1) {
            rc = i2c_burst_read_dt(&imu, 0x28, raw, sizeof(raw));
            if (rc) break;
            int32_t mg[3];
            for (unsigned i = 0; i < 3; ++i)
                mg[i] = (int16_t)sys_get_le16(&raw[i*2]) * 244 / 1000;
            accepted = prov_motion_sample(&motion, k_uptime_get_32(), mg,
                                          threshold_mg);
            if (accepted) break;
        }
        k_msleep(2);
    }
    /* Restore the normal on-demand scale and parked I2C before enabling BLE. */
    int restore = configure_imu();
    int stop = bus_stop();
    last_imu_error = rc ? rc : restore ? restore : stop;
    return last_imu_error ? last_imu_error : accepted ? 1 : 0;
}
int board_prov_motion_arm(void)
{
    if (observed_mask) return -EBUSY;
    int rc = bus_start();
    if (!rc) rc = configure_imu();
    /* AN5130: low-power 26 Hz / +/-8 g. Candidate slope threshold
     * tracks half the factory dynamic threshold (0.5 g with the 1 g default).
     * Only repeated multi-axis software validation can enable Bluetooth. */
    if (!rc) rc = write_reg(0x58, 0x81); /* LIR + INTERRUPTS_ENABLE, slope */
    uint16_t threshold = ez_factory_shake_threshold_mg();
    uint8_t candidate = (uint8_t)DIV_ROUND_UP(threshold, 250U);
    if (!rc) rc = write_reg(0x5b, candidate); /* 1 LSB = 8 g / 64 = 125 mg */
    if (!rc) rc = write_reg(0x5c, 0);    /* one wake sample */
    if (!rc) rc = write_reg(0x10, 0x2c); /* 26 Hz, +/-8 g */
    if (!rc) k_msleep(160);             /* settle slope filter before routing */
    uint8_t source;
    if (!rc) rc = i2c_reg_read_byte_dt(&imu, 0x1b, &source);
    if (!rc) rc = write_reg(0x5e, 0x20); /* wake-up on INT1 */
    if (rc) (void)set_odr(0);
    int stop = bus_stop();
    if (!rc) rc = gpio_pin_configure_dt(&imu_irq, GPIO_INPUT);
    last_imu_error = rc ? rc : stop;
    return last_imu_error;
}
void board_prov_motion_sense(void)
{
    /* Called with interrupts locked immediately before System OFF. No GPIOTE
     * channel or callback is needed: retained GPIO SENSE is the wake source. */
    uint32_t pin = NRF_GPIO_PIN_MAP(DT_PROP(DT_GPIO_CTLR(DT_ALIAS(imu0), irq_gpios), port),
                                    DT_GPIO_PIN(DT_ALIAS(imu0), irq_gpios));
    nrf_gpio_cfg_sense_set(pin, NRF_GPIO_PIN_SENSE_HIGH);
}
#endif
