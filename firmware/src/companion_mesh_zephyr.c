/* SPDX-License-Identifier: Apache-2.0 */
#include "companion_mesh_radio.h"
#include "companion_mesh_platform.h"
#include "ble_companion.h"
#include "ble_prov.h"
#include "factory_data.h"
#include "hardware_config.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(companion_mesh, LOG_LEVEL_INF);
struct command {
 bool configure;
 struct companion_mesh_profile profile;
 uint64_t destination;
 size_t size;
 uint8_t packet[COMPANION_PACKET_MAX];
};
K_MSGQ_DEFINE(commands, sizeof(struct command), 2, 4);
K_SEM_DEFINE(wakeup, 0, 1);
K_SEM_DEFINE(stopped, 0, 1);
static atomic_t connected, stop_requested;
static bool station_detached;
static int station_region(const char *country) {
 struct net_if *iface=net_if_get_first_wifi();if(!iface) return -ENODEV;
 struct wifi_reg_domain domain={.oper=WIFI_MGMT_SET,.country_code={country[0],country[1]}};
 int rc=net_mgmt(NET_REQUEST_WIFI_REG_DOMAIN,iface,&domain,sizeof(domain));
 if(!rc && country[0]!='0') rc=net_if_up(iface);
 return rc==-EALREADY?0:rc;
}
static K_MUTEX_DEFINE(session_lock);
/* Zephyr initializes the SPI/GPIO HAL as a device before application threads. */
void mmhal_init(void) {}
uint32_t companion_mesh_platform_random(void) {return sys_rand32_get();}
static int configure(const struct companion_mesh_profile *profile) {
 if(!atomic_get(&connected)) return -EACCES;
 struct command command={.configure=true,.profile=*profile};
 int rc=k_msgq_put(&commands,&command,K_NO_WAIT);if(!rc) k_sem_give(&wakeup);
 if(rc) LOG_WRN("Profile queue failed: %d",rc);
 return rc;
}
static int send(uint64_t destination,const uint8_t *packet,size_t size) {
 if(!atomic_get(&connected)) return -EACCES;
 if(size>COMPANION_PACKET_MAX) return -EMSGSIZE;
 struct command command={.destination=destination,.size=size};memcpy(command.packet,packet,size);
 int rc=k_msgq_put(&commands,&command,K_NO_WAIT);if(!rc) k_sem_give(&wakeup);return rc;
}
static const struct companion_mesh_ops ops={configure,send,companion_mesh_radio_status};
void companion_mesh_session(bool enabled) {
 k_mutex_lock(&session_lock,K_FOREVER);
 if(enabled) {
  if (!atomic_get(&connected)) {LOG_INF("Companion session claimed");companion_mesh_bind(&ops);atomic_set(&connected,1);}
  k_mutex_unlock(&session_lock);return;
 }
 atomic_clear(&connected);
 LOG_INF("Companion session stopping");
 k_sem_reset(&stopped);atomic_set(&stop_requested,1);k_sem_give(&wakeup);
 k_sem_take(&stopped,K_FOREVER);
 companion_mesh_bind(NULL);
 k_mutex_unlock(&session_lock);
}
static void worker(void *a,void *b,void *c) {
 ARG_UNUSED(a);ARG_UNUSED(b);ARG_UNUSED(c);
 char version[96];
 for(;;) {
  struct companion_mesh_status status;companion_mesh_radio_status(&status);
  k_sem_take(&wakeup,status.initialized?K_MSEC(20):K_FOREVER);
  if(atomic_cas(&stop_requested,1,0)) {
   companion_mesh_radio_stop();
   if(station_detached) {
    int rc=station_region(ez_factory_get()->country);
    if(rc) LOG_ERR("Station restore: %d",rc);else station_detached=false;
   }
   k_msgq_purge(&commands);k_sem_give(&stopped);continue;
  }
  struct command command;
  while(!k_msgq_get(&commands,&command,K_NO_WAIT)) {
   if(!atomic_get(&connected) || atomic_get(&stop_requested)) break;
   int rc;
   if(command.configure) {
    snprintk(version,sizeof(version),"%s/%s",ez_factory_model(),EZ_DEV54_APP_REV);
    struct companion_identity id={
     .node=strtoull(ez_factory_device_serial(),NULL,16),.name=ez_prov_device_name(),
     .version=version,.country=ez_factory_get()->country,
     .icon=hardware_mode_get()==HARDWARE_CAMERA?"camera":"sensor",
     .gps=hardware_mode_get()==HARDWARE_GPS};
    rc=0;
    if(!station_detached) {
     rc=station_region("00");if(!rc) station_detached=true;
     LOG_INF("Station handoff: %d",rc);
    }
    if(!rc) rc=companion_mesh_radio_start(&id,&command.profile,ez_companion_deliver);
    LOG_INF("Mesh start: %d",rc);
   } else {rc=companion_mesh_radio_send(command.destination,command.packet,command.size);if(rc) LOG_WRN("Mesh TX: %d",rc);}
  }
  companion_mesh_radio_tick(k_uptime_get_32());
 }
}
K_THREAD_DEFINE(companion_mesh_worker,16384,worker,NULL,NULL,NULL,7,0,0);
