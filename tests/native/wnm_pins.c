#include "wnm_pins_shim.h"
#include "halow_wnm_pins.h"
#include <stdio.h>
#include <string.h>
const struct device spi_device;
NRF_GPIO_Type ports[3];
static enum pm_device_state state;
static int suspend_error,resume_error,suspends,resumes;
static bool locked;
extern volatile int32_t halow_wnm_pin_diag[4];
int pm_device_state_get(const struct device *d,enum pm_device_state *s) {
 assert(d==&spi_device);*s=state;return 0;
}
int pm_device_action_run(const struct device *d,enum pm_device_action a) {
 assert(d==&spi_device && !locked);
 if(a==PM_DEVICE_ACTION_SUSPEND) {
  assert(state==PM_DEVICE_STATE_ACTIVE);if(suspend_error)return suspend_error;
  ++suspends;state=PM_DEVICE_STATE_SUSPENDED;
 } else {
  assert(state==PM_DEVICE_STATE_SUSPENDED);if(resume_error)return resume_error;
  ++resumes;state=PM_DEVICE_STATE_ACTIVE;
 }
 /* Simulate peripheral PM overwriting the three SPI pin configurations. */
 ports[2].PIN_CNF[1]=ports[2].PIN_CNF[2]=ports[2].PIN_CNF[4]=a==PM_DEVICE_ACTION_SUSPEND?2:3;
 return 0;
}
unsigned int irq_lock(void) { assert(!locked);locked=true;return 7; }
void irq_unlock(unsigned int key) { assert(locked && key==7);locked=false; }
static void initial(void) {
 for(unsigned int p=0;p<3;++p) {
  ports[p].OUT=0x12345678u+p;
  for(unsigned int n=0;n<32;++n)ports[p].PIN_CNF[n]=0x10002u+(p*32+n)*256;
 }
}
int main(void) {
 NRF_GPIO_Type before[3];
 for(int cycle=0;cycle<4;++cycle) {
  initial();memcpy(before,ports,sizeof ports);
  state=cycle%2?PM_DEVICE_STATE_SUSPENDED:PM_DEVICE_STATE_ACTIVE;
  int prior=suspends;
  assert(halow_wnm_pins_park()==0 && halow_wnm_pin_diag[0]==1);
  assert(halow_wnm_pins_park()==-EALREADY);
  assert(suspends==prior+(cycle%2?0:1));
  assert(ports[1].PIN_CNF[4]==3 && !(ports[1].OUT&(1u<<4)));
  assert(ports[1].PIN_CNF[5]==3 && !(ports[1].OUT&(1u<<5)));
  assert(ports[1].PIN_CNF[7]==3 && (ports[1].OUT&(1u<<7)));
  assert(ports[1].PIN_CNF[6]==2 && ports[1].PIN_CNF[10]==2);
  assert(ports[2].PIN_CNF[1]==2 && ports[2].PIN_CNF[2]==2 && ports[2].PIN_CNF[4]==2);
  /* Module EN, LED, camera, button and all unrelated pins are untouched. */
  assert(ports[2].PIN_CNF[0]==before[2].PIN_CNF[0]);
  assert(halow_wnm_pins_restore()==0 && !halow_wnm_pin_diag[0]);
  assert(!memcmp(before,ports,sizeof ports));
  assert(halow_wnm_pins_restore()==0);
 }
 assert(suspends==2 && resumes==2);
 initial();memcpy(before,ports,sizeof ports);state=PM_DEVICE_STATE_ACTIVE;
 suspend_error=-EBUSY;assert(halow_wnm_pins_park()==-EBUSY);
 assert(!memcmp(before,ports,sizeof ports) && !halow_wnm_pin_diag[0]);
 suspend_error=0;state=PM_DEVICE_STATE_OFF;
 assert(halow_wnm_pins_park()==-EBUSY && !memcmp(before,ports,sizeof ports));
 state=PM_DEVICE_STATE_ACTIVE;assert(halow_wnm_pins_park()==0);
 resume_error=-EIO;assert(halow_wnm_pins_restore()==-EIO);
 assert(!memcmp(before,ports,sizeof ports) && !halow_wnm_pin_diag[0]);
 assert(halow_wnm_pin_diag[3]==-EIO);
 puts("WNM pin isolation, exact restoration, SPI PM ownership and failure cleanup passed");
}
