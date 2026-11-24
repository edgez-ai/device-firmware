#include "hardware_config_shim.h"
#include "hardware_config.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
static int factory_value = -1, io_error, queued, reboots;
static uint8_t saved[8];
static bool exists;
#include "hardware_config_under_test.c"
int ez_factory_hardware(void) {return factory_value;}
int settings_subsys_init(void) {return 0;}
static int read_saved(void *arg,void *data,size_t len) {(void)arg;memcpy(data,saved,len);return (int)len;}
int settings_load_subtree(const char *key) {assert(!strcmp(key,"ezdev54"));return exists ? settings_test_load("hardware",8,read_saved,NULL) : 0;}
int settings_save_one(const char *key,const void *data,size_t len) {
 assert(!strcmp(key,"ezdev54/hardware") && len==8);
 if(io_error)return io_error;
 memcpy(saved,data,len);exists=true;return 0;
}
int settings_delete(const char *key) {assert(!strcmp(key,"ezdev54/hardware"));if(io_error)return io_error;exists=false;return 0;}
int k_work_reschedule(struct k_work_delayable *w,int delay) {(void)w;assert(delay==2);++queued;return 1;}
int k_work_cancel_delayable(struct k_work_delayable *w) {(void)w;queued=0;return 0;}
void sys_reboot(int mode) {assert(mode==0);++reboots;}
int main(void) {
 assert(!hardware_config_init() && hardware_mode_get()==HARDWARE_BASE && hardware_config_source()==0);
 for(int mode=0;mode<4;++mode) {
  exists=false;factory_value=mode;assert(!hardware_config_init());
  assert((int)hardware_mode_get()==mode && hardware_config_source()==1);
 }
 factory_value=1;exists=false;hardware_config_init();
 assert(!hardware_config_set(3) && hardware_mode_get()==1 && hardware_config_pending() && exists && queued);
 assert(hardware_config_desired()==3 && hardware_config_source()==2);
 assert(!hardware_config_init() && hardware_mode_get()==3);
 io_error=-EIO;assert(hardware_config_set(2)==-EIO && hardware_config_desired()==3);
 assert(hardware_config_clear()==-EIO && hardware_config_source()==2);
 io_error=0;assert(hardware_config_set(4)==-EINVAL);
 assert(!hardware_config_clear() && hardware_config_pending() && !exists && hardware_config_desired()==1);
 assert(!hardware_config_init() && hardware_mode_get()==1);
 assert(!hardware_config_set(3));hardware_config_init();
 /* Clearing schedules a reboot after removing the saved record. */
 assert(!hardware_config_set(3));
 factory_value=3;hardware_config_init();assert(!hardware_config_clear() && hardware_config_desired()==3 && hardware_config_source()==1);
 saved[0]='?';exists=true;factory_value=2;
 assert(hardware_config_init()<0 && hardware_mode_get()==2);
 factory_value=-1;exists=false;assert(!hardware_config_init() && hardware_mode_get()==0);
 reboot_handler(NULL);assert(reboots==1);
 puts("Hardware precedence, persistence, validation and deferred activation passed");
 return 0;
}
