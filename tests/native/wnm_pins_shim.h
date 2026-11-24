#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define NRF_GPIO_PIN_MAP(p,n) ((p)*32+(n))
#define DT_NODELABEL(x) 0
#define DT_PATH(x) 0
#define DT_BUS(x) 0
#define DT_PROP(node,prop) (node)
#define PORT_resetn_gpios(i) 1
#define PORT_wakeup_gpios(i) 1
#define PORT_cs_gpios(i) 1
#define PORT_wnm_park_gpios(i) ((i)<3?2:1)
#define PIN_resetn_gpios(i) 4
#define PIN_wakeup_gpios(i) 5
#define PIN_cs_gpios(i) 7
#define PIN_wnm_park_gpios(i) ((i)==0?1:(i)==1?2:(i)==2?4:(i)==3?6:10)
#define DT_GPIO_CTLR_BY_IDX(node,prop,i) PORT_##prop(i)
#define DT_GPIO_PIN_BY_IDX(node,prop,i) PIN_##prop(i)
#define DT_FOREACH_PROP_ELEM(node,prop,fn) \
 fn(0,prop,0) fn(0,prop,1) fn(0,prop,2) fn(0,prop,3) fn(0,prop,4)
struct device { int unused; };
extern const struct device spi_device;
#define DEVICE_DT_GET(node) (&spi_device)
typedef struct { uint32_t PIN_CNF[32], OUT; } NRF_GPIO_Type;
extern NRF_GPIO_Type ports[3];
static inline NRF_GPIO_Type *nrf_gpio_pin_port_decode(uint32_t *pin) {
 unsigned int port=*pin/32;*pin%=32;return &ports[port];
}
static inline uint32_t nrf_gpio_pin_out_read(uint32_t pin) {
 NRF_GPIO_Type *p=nrf_gpio_pin_port_decode(&pin);return (p->OUT>>pin)&1;
}
static inline void nrf_gpio_pin_write(uint32_t pin,uint32_t value) {
 NRF_GPIO_Type *p=nrf_gpio_pin_port_decode(&pin);
 p->OUT=(p->OUT&~(1u<<pin))|((value&1u)<<pin);
}
static inline void nrf_gpio_cfg_default(uint32_t pin) {
 NRF_GPIO_Type *p=nrf_gpio_pin_port_decode(&pin);p->PIN_CNF[pin]=2;
}
static inline void nrf_gpio_cfg_output(uint32_t pin) {
 NRF_GPIO_Type *p=nrf_gpio_pin_port_decode(&pin);p->PIN_CNF[pin]=3;
}
enum pm_device_state {PM_DEVICE_STATE_ACTIVE,PM_DEVICE_STATE_SUSPENDED,PM_DEVICE_STATE_OFF};
enum pm_device_action {PM_DEVICE_ACTION_SUSPEND,PM_DEVICE_ACTION_RESUME};
int pm_device_state_get(const struct device *,enum pm_device_state *);
int pm_device_action_run(const struct device *,enum pm_device_action);
unsigned int irq_lock(void);
void irq_unlock(unsigned int);
