/* SPDX-License-Identifier: Apache-2.0
 * EdgeZ E73 nRF54L15 carrier: isolated, irreversible-until-reset power floor.
 * No factory/settings access. Radio-only variants initialize HaLow before
 * isolation; the default all-off build has no radio or networking.
 */
#include <errno.h>
#include <hal/nrf_gpio.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/timer/system_timer.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/poweroff.h>

#define PIN(port, bit) NRF_GPIO_PIN_MAP(port, bit)
#define IMU_ADDRESS 0x6a
static const struct device *const bus=DEVICE_DT_GET(DT_NODELABEL(i2c30));
/* Passive SWD diagnostics. Attaching SWD invalidates the power measurement. */
volatile unsigned baseline_stage;
volatile int baseline_imu_error, baseline_bus_error;
volatile uint8_t baseline_imu_whoami, baseline_imu_accel, baseline_imu_gyro;

static void output(unsigned pin, bool high) {
 nrf_gpio_pin_write(pin, high); /* Set latch before enabling output. */
 nrf_gpio_cfg_output(pin);
}
static void supplies_off(void) {
#ifndef HALOW_WNM_BASELINE
 output(PIN(2,0),false); /* HaLow supply switch. */
#endif
 output(PIN(1,9),false); /* Camera/GPS/radar connector supply. */
 output(PIN(1,14),false); /* Carrier battery-divider enable. */
 output(PIN(1,15),false); /* Unused XIAO battery-enable pin. */
 output(PIN(0,1),false); /* Unused XIAO IMU/PDM supply pin. */
 output(PIN(2,3),false); /* Unused XIAO RF-switch supply. */
 output(PIN(2,5),true);  /* Carrier active-low indicator OFF. */
}
static void park_signals(void) {
 static const unsigned pins[]={
#ifndef HALOW_WNM_BASELINE
  PIN(1,4),PIN(1,5),PIN(1,6),PIN(1,7),PIN(1,10), /* HaLow reset/wake/IRQ/CS/BUSY. */
  PIN(2,1),PIN(2,2),PIN(2,4), /* HaLow SPI. */
#endif
  PIN(1,11),PIN(1,12), /* External UART. */
  PIN(1,13),PIN(1,8), /* Battery input, key. */
  PIN(0,2),PIN(0,3),PIN(0,4), /* IMU IRQ/SCL/SDA. */
 };
 for(unsigned i=0;i<ARRAY_SIZE(pins);++i) {
  nrf_gpio_cfg_default(pins[i]);
  nrf_gpio_cfg_sense_set(pins[i],NRF_GPIO_PIN_NOSENSE);
 }
}
static int imu_off(void) {
 if(!device_is_ready(bus)) return -ENODEV;
 int first=0,rc;
 /* Enter main register bank, then attempt every shutdown write even if a
  * previous transaction failed. ODR=0 powers down both sensor engines. */
 static const uint8_t registers[][2]={
  {0x01,0},{0x10,0},{0x11,0},{0x0a,0},{0x0d,0},{0x0e,0},
  {0x19,0},{0x58,0},{0x5e,0},{0x5f,0},
 };
 for(unsigned i=0;i<ARRAY_SIZE(registers);++i) {
  rc=i2c_reg_write_byte(bus,IMU_ADDRESS,registers[i][0],registers[i][1]);
  if(rc && !first) first=rc;
 }
 uint8_t who=0,accel=0xff,gyro=0xff;
 rc=i2c_reg_read_byte(bus,IMU_ADDRESS,0x0f,&who);if(rc && !first) first=rc;
 rc=i2c_reg_read_byte(bus,IMU_ADDRESS,0x10,&accel);if(rc && !first) first=rc;
 rc=i2c_reg_read_byte(bus,IMU_ADDRESS,0x11,&gyro);if(rc && !first) first=rc;
 baseline_imu_whoami=who;baseline_imu_accel=accel;baseline_imu_gyro=gyro;
 if(!first && (who!=0x6a || (accel&0xf0) || (gyro&0xf0))) first=-EIO;
 return first;
}
int main(void) {
 supplies_off();baseline_stage=1;
 baseline_imu_error=imu_off();
 baseline_bus_error=pm_device_action_run(bus,PM_DEVICE_ACTION_SUSPEND);
 if(baseline_bus_error==-EALREADY) baseline_bus_error=0;
#ifdef HALOW_WNM_BASELINE
 extern int baseline_radio_sleep(void);
 volatile static int radio_result;
 radio_result=baseline_radio_sleep();
 if(radio_result) { baseline_stage=0xee; for(;;) k_sleep(K_FOREVER); }
#endif
 /* The bus PM callback applies its sleep state; park_signals also parks SDA/SCL. */
 park_signals();baseline_stage=2;
#ifdef HALOW_RADIO_ONLY
 extern int baseline_radio_quiesce(void);
 /* This returns with scheduling/IRQs locked only on successful isolation. */
 if(baseline_radio_quiesce()) {
  baseline_stage=0xef;
  for(;;) k_sleep(K_FOREVER);
 }
 supplies_off();park_signals();
 sys_clock_disable();baseline_stage=5;
 sys_poweroff();
 CODE_UNREACHABLE;
#elif defined(HALOW_DTIM_BASELINE)
 baseline_stage=4;
 for(;;) k_sleep(K_FOREVER);
#endif
 /* Short inspection interval; measure only AFTER it, without SWD attached. */
 k_msleep(3000);
 (void)irq_lock();supplies_off();park_signals();
 sys_clock_disable();baseline_stage=3;
 sys_poweroff();
 CODE_UNREACHABLE;
}
