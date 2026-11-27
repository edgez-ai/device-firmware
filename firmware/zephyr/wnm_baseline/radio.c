/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#include <mmwlan.h>
#include <zephyr/device.h>
#include <zephyr/pm/device.h>
#include <hal/nrf_gpio.h>
#include "halow_wnm_pins.h"
static K_SEM_DEFINE(join_done, 0, 1);
static struct net_mgmt_event_callback cb;
volatile int wnm_baseline_join_result = -EINPROGRESS;
volatile int wnm_baseline_ps_result = -EINPROGRESS;
volatile int wnm_baseline_entry_result = -EINPROGRESS;
volatile int wnm_baseline_park_result = -EINPROGRESS;
volatile int dtim_baseline_dynamic_ps_result = -EINPROGRESS;
volatile int dtim_baseline_connection_state;
volatile unsigned dtim_baseline_link_events;
static void connected(struct net_mgmt_event_callback *callback, uint64_t event, struct net_if *iface)
{
 ARG_UNUSED(iface); ARG_UNUSED(event);
 const struct wifi_status *status = callback->info;
 wnm_baseline_join_result = status ? status->status : -EIO;
 dtim_baseline_connection_state = wnm_baseline_join_result == 0;
 ++dtim_baseline_link_events;
 k_sem_give(&join_done);
}
int baseline_radio_sleep(void)
{
 struct net_if *iface = net_if_get_first_wifi();
 if(!iface) return -ENODEV;
 net_mgmt_init_event_callback(&cb, connected, NET_EVENT_WIFI_CONNECT_RESULT);
 net_mgmt_add_event_callback(&cb);
 struct wifi_connect_req_params p = {0};
 p.ssid = (const uint8_t *)CONFIG_RADAR_HALOW_SSID;
 p.ssid_length = strlen(CONFIG_RADAR_HALOW_SSID);
 p.channel = WIFI_CHANNEL_ANY;
 p.band = WIFI_FREQ_BAND_UNKNOWN;
 p.security = WIFI_SECURITY_TYPE_SAE;
 p.mfp = WIFI_MFP_REQUIRED;
 p.sae_password = (const uint8_t *)CONFIG_RADAR_HALOW_PASSWORD;
 p.sae_password_length = strlen(CONFIG_RADAR_HALOW_PASSWORD);
 int rc = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &p, sizeof(p));
 if(rc) { wnm_baseline_join_result=rc; return rc; }
 if(k_sem_take(&join_done,K_SECONDS(120))) return -ETIMEDOUT;
 if(wnm_baseline_join_result) return wnm_baseline_join_result;
 wnm_baseline_ps_result=mmwlan_set_power_save_mode(MMWLAN_PS_ENABLED);
 if(wnm_baseline_ps_result) return wnm_baseline_ps_result;
#ifdef HALOW_DTIM_BASELINE
 /* Match normal firmware idle tuning. Follow the AP DTIM; no IP traffic,
  * keepalive or application timer. The SDK retains its normal health checks. */
 dtim_baseline_dynamic_ps_result=mmwlan_set_dynamic_ps_timeout(13);
 return dtim_baseline_dynamic_ps_result;
#else
 rc=net_if_suspend(iface);
 if(rc) return rc;
 struct mmwlan_set_wnm_sleep_enabled_args a=MMWLAN_SET_WNM_SLEEP_ENABLED_ARGS_INIT;
 a.wnm_sleep_enabled=true;
 a.chip_powerdown_enabled=true;
 wnm_baseline_entry_result=mmwlan_set_wnm_sleep_enabled_ext(&a);
 if(wnm_baseline_entry_result) return wnm_baseline_entry_result;
 wnm_baseline_park_result=halow_wnm_pins_park();
 return wnm_baseline_park_result;
#endif
}

#ifdef HALOW_RADIO_ONLY
/* One-way test only: the host will not process RX frames or WPA rekeys.
 * Wait until the SDK releases WAKE and the chip reports idle, then stop
 * host scheduling and isolate SPI without resetting the powered radio. */
volatile int dtim_radio_quiesce_result = -EINPROGRESS;
volatile unsigned dtim_radio_pin_levels;
volatile unsigned dtim_radio_spi_enabled;
int baseline_radio_quiesce(void)
{
 const unsigned wake=NRF_GPIO_PIN_MAP(1,5), busy=NRF_GPIO_PIN_MAP(1,10);
 int64_t deadline=k_uptime_get()+30000, quiet_since=0;
 while(k_uptime_get()<deadline) {
  if(nrf_gpio_pin_out_read(wake)==0 && nrf_gpio_pin_read(busy)==0) {
   if(!quiet_since) quiet_since=k_uptime_get();
   if(k_uptime_get()-quiet_since>=20) {
    k_sched_lock();
    if(nrf_gpio_pin_out_read(wake)!=0 || nrf_gpio_pin_read(busy)!=0) {
     k_sched_unlock(); quiet_since=0; continue;
    }
    int rc=pm_device_action_run(DEVICE_DT_GET(DT_BUS(DT_NODELABEL(morse_wifi))), PM_DEVICE_ACTION_SUSPEND);
    if(rc && rc!=-EALREADY) { k_sched_unlock(); return dtim_radio_quiesce_result=rc; }
    (void)irq_lock();
    /* Active-low RESETn must stay HIGH: do not use the WNM park helper,
     * which deliberately asserts reset for WNM chip powerdown. */
    nrf_gpio_pin_set(NRF_GPIO_PIN_MAP(1,4));
    nrf_gpio_cfg_output(NRF_GPIO_PIN_MAP(1,4));
    nrf_gpio_pin_clear(wake); nrf_gpio_cfg_output(wake);
    nrf_gpio_pin_set(NRF_GPIO_PIN_MAP(1,7));
    nrf_gpio_cfg_output(NRF_GPIO_PIN_MAP(1,7));
    const unsigned passive[]={NRF_GPIO_PIN_MAP(2,1),NRF_GPIO_PIN_MAP(2,2),NRF_GPIO_PIN_MAP(2,4),NRF_GPIO_PIN_MAP(1,6),busy};
    for(unsigned i=0;i<ARRAY_SIZE(passive);++i) {
     nrf_gpio_cfg_default(passive[i]);
     nrf_gpio_cfg_sense_set(passive[i],NRF_GPIO_PIN_NOSENSE);
    }
    dtim_radio_spi_enabled=NRF_SPIM00->ENABLE;
    dtim_radio_pin_levels=nrf_gpio_pin_out_read(NRF_GPIO_PIN_MAP(1,4)) |
       (nrf_gpio_pin_out_read(wake)<<1) |
       (nrf_gpio_pin_out_read(NRF_GPIO_PIN_MAP(1,7))<<2) |
       (nrf_gpio_pin_out_read(NRF_GPIO_PIN_MAP(2,0))<<3);
    return dtim_radio_quiesce_result=0;
   }
  } else quiet_since=0;
  k_msleep(1);
 }
 return dtim_radio_quiesce_result=-ETIMEDOUT;
}
#endif
