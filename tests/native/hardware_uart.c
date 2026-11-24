#include "hardware_config.h"
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
static enum hardware_mode mode;
enum hardware_mode hardware_mode_get(void) {return mode;}
struct device {bool ready;};
static struct device test_uart;
struct gpio_dt_spec {int port;};
struct pinctrl_state {int id;};
struct pinctrl_dev_config {int unused;};
static struct pinctrl_dev_config test_pins;
typedef int NRF_UARTE_Type;
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define DT_NODELABEL(x) 0
#define DT_PATH(x) 0
#define DT_REG_ADDR(x) 0
#define DEVICE_DT_GET(x) (&test_uart)
#define GPIO_DT_SPEC_GET(n,p) {1}
#define GPIO_OUTPUT_INACTIVE 0
#define GPIO_DISCONNECTED 1
#define PINCTRL_DT_DEV_CONFIG_DECLARE(x)
#define PINCTRL_DT_STATE_PINS_DEFINE(n,p)
#define PINCTRL_DT_STATE_INIT(p,s) {s}
#define PINCTRL_STATE_DEFAULT 0
#define PINCTRL_STATE_SLEEP 1
#define PINCTRL_DT_DEV_CONFIG_GET(n) (&test_pins)
#define UART_CFG_PARITY_NONE 0
#define UART_CFG_STOP_BITS_1 1
#define UART_CFG_DATA_BITS_8 8
#define UART_CFG_FLOW_CTRL_NONE 0
struct uart_config {int baudrate,parity,stop_bits,data_bits,flow_ctrl;};
static int disabled, parked, powered_off, updated, initialized, baud, init_error;
static void nrf_uarte_disable(NRF_UARTE_Type *reg) {(void)reg;++disabled;}
static bool gpio_is_ready_dt(const struct gpio_dt_spec *p) {return p->port==1;}
static int gpio_pin_configure_dt(const struct gpio_dt_spec *p,int flags) {assert(p->port==1 && flags==0);++powered_off;return 0;}
static int gpio_pin_configure(int port,int pin,int flags) {assert(port==1 && (pin==11 || pin==12) && flags==1);++parked;return 0;}
static int pinctrl_update_states(struct pinctrl_dev_config *c,const struct pinctrl_state *s,size_t n) {assert(c==&test_pins && n==2 && s[0].id==0 && s[1].id==1);++updated;return 0;}
static bool device_is_ready(const struct device *d) {return d->ready;}
static int device_init(const struct device *d) {assert(d==&test_uart);++initialized;if(init_error)return init_error;test_uart.ready=true;return 0;}
static int uart_configure(const struct device *d,const struct uart_config *c) {assert(d->ready && c->data_bits==8 && c->flow_ctrl==0);baud=c->baudrate;return 0;}
#include "hardware_uart_under_test.c"
int main(void) {
 for (int m=0;m<4;++m) {
  mode=m;test_uart.ready=false;disabled=parked=powered_off=updated=initialized=baud=0;
  assert(!hardware_uart_prepare() && disabled==1 && parked==2 && powered_off==1);
  assert(updated==(m!=HARDWARE_CAMERA));
  if(m==0) {assert(hardware_uart_init()<0 && initialized==0 && baud==0);continue;}
  assert(!hardware_uart_init() && initialized==1);
  assert(baud==(m==1?921600:m==2?115200:9600));
 }
 test_uart.ready=false;init_error=-5;assert(hardware_uart_init()==-5);
 puts("UART base isolation, dynamic routing, lazy init and per-device baud passed");
 return 0;
}
