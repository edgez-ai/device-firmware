#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define ARG_UNUSED(x) ((void)(x))
struct device { bool ready; };
extern struct device gps_uart_device;
#define DT_ALIAS(x) 0
#define DT_PATH(x) 0
#define DEVICE_DT_GET(x) (&gps_uart_device)
struct gpio_dt_spec { int pin; };
#define GPIO_DT_SPEC_GET(n,p) {9}
#define GPIO_OUTPUT_INACTIVE 0
struct pinctrl_dev_config { int unused; };
extern struct pinctrl_dev_config gps_pins;
#define PINCTRL_DT_DEFINE(n)
#define PINCTRL_DT_DEV_CONFIG_GET(n) (&gps_pins)
#define PINCTRL_STATE_SLEEP 1
struct ring_buf { uint8_t data[512]; size_t count; };
#define RING_BUF_DECLARE(n,s) struct ring_buf n
static inline uint32_t ring_buf_put(struct ring_buf *r,const uint8_t *p,uint32_t n) {
 if(n>512-r->count)n=512-r->count;memcpy(r->data+r->count,p,n);r->count+=n;return n;
}
static inline uint32_t ring_buf_get(struct ring_buf *r,uint8_t *p,uint32_t n) {
 if(n>r->count)n=r->count;memcpy(p,r->data,n);r->count-=n;memmove(r->data,r->data+n,r->count);return n;
}
static inline void ring_buf_reset(struct ring_buf *r) { r->count=0; }
struct k_spinlock { bool locked; };
typedef unsigned int k_spinlock_key_t;
k_spinlock_key_t k_spin_lock(struct k_spinlock *);
void k_spin_unlock(struct k_spinlock *,k_spinlock_key_t);
struct k_work { void (*handler)(struct k_work *); };
#define K_WORK_DEFINE(n,fn) struct k_work n={fn}
int k_work_submit(struct k_work *);
enum pm_device_action {PM_DEVICE_ACTION_SUSPEND,PM_DEVICE_ACTION_RESUME};
int pm_device_action_run(const struct device *,enum pm_device_action);
bool device_is_ready(const struct device *);
int device_init(const struct device *);
int gpio_is_ready_dt(const struct gpio_dt_spec *);
int gpio_pin_configure_dt(const struct gpio_dt_spec *,int);
int gpio_pin_set_dt(const struct gpio_dt_spec *,int);
int pinctrl_apply_state(const struct pinctrl_dev_config *,int);
int uart_irq_callback_user_data_set(const struct device *,void (*)(const struct device *,void *),void *);
int uart_irq_update(const struct device *);
int uart_irq_rx_ready(const struct device *);
int uart_fifo_read(const struct device *,uint8_t *,int);
void uart_irq_rx_enable(const struct device *);
void uart_irq_rx_disable(const struct device *);
void uart_irq_tx_disable(const struct device *);
void uart_irq_err_disable(const struct device *);
