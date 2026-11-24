#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define ARG_UNUSED(x) ((void)(x))
#define LOG_MODULE_DECLARE(...)
#define LOG_ERR(...) ((void)0)
#define GPIO_INT_EDGE_TO_ACTIVE 1
#define GPIO_INT_DISABLE 0
struct device { int unused; };
struct gpio_callback { int unused; };
struct gpio_dt_spec { unsigned int pin; bool active_low; };
struct net_eth_addr { uint8_t addr[6]; };
typedef void (*mmhal_irq_handler_t)(void);
struct morsemicro_bus_ops { int (*release)(void); };
struct morsemicro_config {
 struct gpio_dt_spec resetn, wakeup, busy;
 const struct morsemicro_bus_ops *bus_ops;
};
extern const struct morsemicro_config *morsemicro_config0;
int gpio_pin_set_dt(const struct gpio_dt_spec *,int);
int gpio_pin_get_dt(const struct gpio_dt_spec *);
int gpio_pin_interrupt_configure_dt(const struct gpio_dt_spec *,int);
int hwinfo_get_device_eui64(uint8_t *);
int hwinfo_get_device_id(uint8_t *,size_t);
uint32_t crc32_ieee(const uint8_t *,size_t);
uint32_t sys_rand32_get(void);
bool net_eth_is_addr_valid(const struct net_eth_addr *);
void mmosal_task_sleep(uint32_t);
void mmhal_wlan_init(void);
void mmhal_wlan_deinit(void);
void mmhal_wlan_hard_reset(void);
void mmhal_wlan_wake_assert(void);
void mmhal_wlan_wake_deassert(void);
