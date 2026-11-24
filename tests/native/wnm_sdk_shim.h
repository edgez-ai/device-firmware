#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include "wnm_sleep_fsm.h"
#define MMLOG_DBG(...) ((void)0)
#define MMLOG_WRN(...) ((void)0)
#define MMLOG_ERR(...) ((void)0)
#define MMLOG_INF(...) ((void)0)
#define MM_UNUSED(x) ((void)(x))
#define MMOSAL_ASSERT(x) assert(x)
#define MMOSAL_DEV_ASSERT(x) assert(x)
#define MMOSAL_DEV_ASSERT_LOG_DATA(x,...) assert(x)
#define MMDRV_VIF_ID_INVALID 65535
#define UMAC_INTERFACE_STA 1
#define MMWLAN_STA_CONNECTED 1
#define UMAC_HEALTH_CHECK_VETO_ID_WNM_SLEEP 1
#define UMAC_DATAPATH_PAUSE_SOURCE_WNM_SLEEP 1
enum mmwlan_status { MMWLAN_SUCCESS, MMWLAN_ERROR, MMWLAN_INVALID_ARGUMENT, MMWLAN_UNAVAILABLE, MMWLAN_TIMED_OUT=6 };
enum umac_wnm_sleep_event { UMAC_WNM_SLEEP_EVENT_REQUEST_ENTRY, UMAC_WNM_SLEEP_EVENT_REQUEST_EXIT,
 UMAC_WNM_SLEEP_EVENT_CONNECTION_LOST, UMAC_WNM_SLEEP_EVENT_HW_RESTARTED,
 UMAC_WNM_SLEEP_EVENT_ENTRY_CONFIRMED, UMAC_WNM_SLEEP_EVENT_EXIT_CONFIRMED };
struct umac_data;
struct mmosal_semb { int value; };
typedef void (*umac_wnm_sleep_request_cb_t)(struct umac_data *,enum mmwlan_status,void *);
struct umac_wnm_sleep_data {
 struct wnm_sleep_fsm_instance fsm_inst; umac_wnm_sleep_request_cb_t callback;
 uint8_t retry_counter; void *arg; struct mmosal_semb *semb; volatile enum mmwlan_status *status;
};
struct umac_data { struct umac_wnm_sleep_data wnm; bool powerdown; };
struct umac_wnm_sleep_data *umac_data_get_wnm_sleep(struct umac_data *);
void umac_config_set_chip_powerdown_enabled(struct umac_data *,bool);
bool umac_config_is_chip_powerdown_enabled(struct umac_data *);
int umac_connection_get_state(struct umac_data *);
void umac_connection_handle_hw_restarted(struct umac_data *);
void mmosal_semb_give(struct mmosal_semb *);
uint16_t umac_interface_get_vif_id(struct umac_data *,int);
void umac_health_check_set_veto(struct umac_data *,int);
void umac_health_check_unset_veto(struct umac_data *,int);
int mmdrv_set_chip_wnm_sleep_enabled(uint16_t,bool);
bool umac_core_register_timeout(struct umac_data *,int,void (*)(void *,void *),void *,void *);
bool umac_core_cancel_timeout(struct umac_data *,void (*)(void *,void *),void *,void *);
void umac_supp_wnm_enter(struct umac_data *);
void umac_supp_wnm_exit(struct umac_data *);
void umac_datapath_pause(struct umac_data *,int);
void umac_datapath_unpause(struct umac_data *,int);
void umac_connection_set_monitor_disable(struct umac_data *,bool);
void mmdrv_deinit(void);
int mmdrv_init(void *,const char *);
const char *umac_regdb_get_country_code(struct umac_data *);
void umac_wnm_sleep_init(struct umac_data *);
void umac_wnm_sleep_set_chip_powerdown(struct umac_data *,bool);
void umac_wnm_sleep_report_event(struct umac_data *,enum umac_wnm_sleep_event);
