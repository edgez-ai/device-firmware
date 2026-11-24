#include "gps_hw_shim.h"
#include "gps_hw.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
struct device gps_uart_device;
struct pinctrl_dev_config gps_pins;
static bool power,asleep,rx,isr,controller_active;
static int init_count,wakes,init_error,callback_error,resume_error;
static void (*callback)(const struct device *,void *);
static struct k_work *pending;
static bool fifo_has_data;
void halow_light_wake(void) { assert(!isr);++wakes; }
k_spinlock_key_t k_spin_lock(struct k_spinlock *l) {assert(!l->locked);l->locked=true;return 0;}
void k_spin_unlock(struct k_spinlock *l,k_spinlock_key_t key) {(void)key;assert(l->locked);l->locked=false;}
int k_work_submit(struct k_work *w) {pending=w;return 0;}
bool device_is_ready(const struct device *d) {return d->ready;}
int device_init(const struct device *d) {
 assert(d==&gps_uart_device && power);++init_count;
 if(init_error)return init_error;
 gps_uart_device.ready=true;controller_active=true;asleep=false;return 0;
}
int gpio_is_ready_dt(const struct gpio_dt_spec *p) {assert(p->pin==9);return 1;}
int gpio_pin_configure_dt(const struct gpio_dt_spec *p,int flags) {assert(p->pin==9 && flags==GPIO_OUTPUT_INACTIVE);power=false;return 0;}
int gpio_pin_set_dt(const struct gpio_dt_spec *p,int value) {assert(p->pin==9);power=value;return 0;}
int pinctrl_apply_state(const struct pinctrl_dev_config *p,int state) {assert(p==&gps_pins && state==PINCTRL_STATE_SLEEP);asleep=true;return 0;}
int pm_device_action_run(const struct device *d,enum pm_device_action action) {
 assert(d==&gps_uart_device && d->ready);
 if(action==PM_DEVICE_ACTION_SUSPEND) {bool was_active=controller_active;controller_active=false;return was_active?0:-EALREADY;}
 if(resume_error)return resume_error;
 if(controller_active)return -EALREADY;
 controller_active=true;asleep=false;return 0;
}
int uart_irq_callback_user_data_set(const struct device *d,void (*fn)(const struct device *,void *),void *arg) {
 assert(d->ready && arg==NULL);if(callback_error)return callback_error;callback=fn;return 0;
}
void uart_irq_rx_enable(const struct device *d) {assert(d->ready && power && controller_active);rx=true;}
void uart_irq_rx_disable(const struct device *d) {assert(d->ready);rx=false;}
void uart_irq_tx_disable(const struct device *d) {assert(d->ready);}
void uart_irq_err_disable(const struct device *d) {assert(d->ready);}
int uart_irq_update(const struct device *d) {assert(d->ready);return 1;}
int uart_irq_rx_ready(const struct device *d) {assert(d->ready);return rx;}
int uart_fifo_read(const struct device *d,uint8_t *data,int length) {
 assert(d->ready && isr && length>=3);
 if(!fifo_has_data)return 0;
 memcpy(data,"abc",3);fifo_has_data=false;return 3;
}
int main(int argc,char **argv) {
 assert(argc==2);int test=atoi(argv[1]);
 assert(gps_hw_init()==0 && !power && asleep && init_count==0 && wakes==0);
 if(test==2)init_error=-EIO;
 if(test==3)callback_error=-EIO;
 if(test==4)resume_error=-EIO;
 int rc=gps_hw_start();
 if(test!=1) {
  assert(rc==-EIO && !power && asleep && !rx && !controller_active);
 } else {
  assert(rc==0 && power && rx && !asleep && callback && init_count==1);
  isr=true;fifo_has_data=true;callback(&gps_uart_device,NULL);isr=false;
  assert(wakes==0 && pending);pending->handler(pending);assert(wakes==1);
  uint8_t data[8];assert(gps_hw_read(data,sizeof data)==3 && !memcmp(data,"abc",3));
  assert(gps_hw_stop()==0 && !power && asleep && !rx && !controller_active);
  assert(gps_hw_start()==0 && init_count==1 && power && rx && !asleep);
  assert(gps_hw_stop()==0 && gps_hw_stop()==0 && !power && asleep);
 }
 puts("Actual GPS UART deferral, ISR handoff, supply sequencing and error cleanup passed");
}
