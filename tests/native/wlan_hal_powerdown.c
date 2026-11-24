#include "wlan_hal_shim.h"
#include <stdio.h>
static int output[32], releases;
static uint32_t delay;
static int release_bus(void) {
 assert(output[4]==0); /* Active-low RESETn must be held LOW before releasing SPI. */
 ++releases;return 0;
}
static const struct morsemicro_bus_ops bus={.release=release_bus};
static const struct morsemicro_config cfg={.resetn={4,true},.wakeup={5,false},.busy={10,false},.bus_ops=&bus};
const struct morsemicro_config *morsemicro_config0=&cfg;
int gpio_pin_set_dt(const struct gpio_dt_spec *s,int logical) { output[s->pin]=(logical!=0)^s->active_low;return 0; }
int gpio_pin_get_dt(const struct gpio_dt_spec *s) { return output[s->pin]^s->active_low; }
int gpio_pin_interrupt_configure_dt(const struct gpio_dt_spec *s,int x) { (void)s;(void)x;return 0; }
int hwinfo_get_device_eui64(uint8_t *p) { (void)p;return -1; }
int hwinfo_get_device_id(uint8_t *p,size_t n) { (void)p;(void)n;return -1; }
uint32_t crc32_ieee(const uint8_t *p,size_t n) { (void)p;(void)n;return 1; }
uint32_t sys_rand32_get(void) { return 1; }
bool net_eth_is_addr_valid(const struct net_eth_addr *p) { (void)p;return false; }
void mmosal_task_sleep(uint32_t ms) { delay+=ms; }
int main(void) {
 for(int i=0;i<3;++i) {
  mmhal_wlan_init();
  mmhal_wlan_hard_reset();assert(output[4]==1); /* Running radio, reset released. */
  mmhal_wlan_wake_assert();assert(output[5]==1);
  mmhal_wlan_deinit();assert(output[4]==0); /* Powered-down SDK path must not leave it running. */
  mmhal_wlan_wake_deassert();assert(output[5]==0);
 }
 assert(releases==3 && delay==75);
 puts("Real Zephyr WLAN HAL holds active-low RESETn asserted through powerdown and releases it on restart");
}
