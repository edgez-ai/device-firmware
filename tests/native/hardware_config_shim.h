#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
struct k_work { int unused; };
struct k_work_delayable { int unused; };
#define K_WORK_DELAYABLE_DEFINE(n,handler) struct k_work_delayable n
#define K_SECONDS(n) (n)
#define SYS_REBOOT_COLD 0
#define SETTINGS_STATIC_HANDLER_DEFINE(n,sub,g,set,c,e) \
 static int (*settings_test_load)(const char *,size_t,settings_read_cb,void *) = set
 typedef int (*settings_read_cb)(void *,void *,size_t);
int settings_subsys_init(void);
int settings_load_subtree(const char *);
int settings_save_one(const char *,const void *,size_t);
int settings_delete(const char *);
int k_work_reschedule(struct k_work_delayable *,int);
int k_work_cancel_delayable(struct k_work_delayable *);
void sys_reboot(int);
