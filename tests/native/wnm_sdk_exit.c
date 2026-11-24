#include "wnm_sdk_shim.h"
#include <stdio.h>
static bool chip_sleep, powered_down, paused, veto, monitor_disabled;
static unsigned int cold_boots, enters, exits, restores;
static uint16_t vif = 1;
struct umac_wnm_sleep_data *umac_data_get_wnm_sleep(struct umac_data *u) { return &u->wnm; }
void umac_config_set_chip_powerdown_enabled(struct umac_data *u,bool x) { u->powerdown=x; }
bool umac_config_is_chip_powerdown_enabled(struct umac_data *u) { return u->powerdown; }
int umac_connection_get_state(struct umac_data *u) { (void)u; return MMWLAN_STA_CONNECTED; }
void mmosal_semb_give(struct mmosal_semb *s) { ++s->value; }
uint16_t umac_interface_get_vif_id(struct umac_data *u,int which) { (void)u; assert(which==UMAC_INTERFACE_STA);return vif; }
void umac_health_check_set_veto(struct umac_data *u,int id) { (void)u;(void)id;veto=true; }
void umac_health_check_unset_veto(struct umac_data *u,int id) { (void)u;(void)id;veto=false; }
int mmdrv_set_chip_wnm_sleep_enabled(uint16_t id,bool enabled) { assert(id==vif);chip_sleep=enabled;return 0; }
bool umac_core_register_timeout(struct umac_data *u,int t,void (*f)(void *,void *),void *a,void *b) { (void)u;(void)t;(void)f;(void)a;(void)b;return true; }
bool umac_core_cancel_timeout(struct umac_data *u,void (*f)(void *,void *),void *a,void *b) { (void)u;(void)f;(void)a;(void)b;return true; }
void umac_supp_wnm_enter(struct umac_data *u) { (void)u;++enters; }
void umac_supp_wnm_exit(struct umac_data *u) { (void)u; assert(!powered_down);++exits; }
void umac_datapath_pause(struct umac_data *u,int id) { (void)u;(void)id;paused=true; }
void umac_datapath_unpause(struct umac_data *u,int id) { (void)u;(void)id;paused=false; }
void umac_connection_set_monitor_disable(struct umac_data *u,bool x) { (void)u;monitor_disabled=x; }
void mmdrv_deinit(void) { powered_down=true; }
int mmdrv_init(void *p,const char *country) { (void)p;assert(country[0]=='E');powered_down=false;chip_sleep=false;++cold_boots;return 0; }
const char *umac_regdb_get_country_code(struct umac_data *u) { (void)u;return "EU"; }
void umac_connection_handle_hw_restarted(struct umac_data *u) {
 ++restores; ++vif; /* VIF IDs need not survive a full hardware restart. */
 umac_wnm_sleep_report_event(u,UMAC_WNM_SLEEP_EVENT_HW_RESTARTED);
 assert(chip_sleep); /* Real restore path reinstates WNM while FSM still ACTIVE. */
}
int main(void) {
 struct umac_data u={0};umac_wnm_sleep_init(&u);
 for (int powerdown=0;powerdown<=1;++powerdown) {
  for (int round=0;round<2;++round) {
   umac_wnm_sleep_set_chip_powerdown(&u,powerdown);
   umac_wnm_sleep_report_event(&u,UMAC_WNM_SLEEP_EVENT_REQUEST_ENTRY);
   umac_wnm_sleep_report_event(&u,UMAC_WNM_SLEEP_EVENT_ENTRY_CONFIRMED);
   assert(paused && monitor_disabled && veto);
   assert(powerdown ? powered_down : chip_sleep);
   umac_wnm_sleep_report_event(&u,UMAC_WNM_SLEEP_EVENT_REQUEST_EXIT);
   assert(!powered_down && !chip_sleep && !paused && !monitor_disabled && !veto);
   umac_wnm_sleep_report_event(&u,UMAC_WNM_SLEEP_EVENT_EXIT_CONFIRMED);
   assert(u.wnm.fsm_inst.current_state==WNM_SLEEP_FSM_STATE_IDLE);
  }
 }
 assert(cold_boots==2 && restores==2 && enters==4 && exits==4);
 puts("Real SDK WNM FSM clears restored chip sleep on both exit paths, including changed VIF IDs");
}
