/* SPDX-License-Identifier: Apache-2.0 */
#include "companion_mesh_radio.h"
#include "companion_mesh_platform.h"
#include "dev01.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <errno.h>
#include <string.h>
struct command {bool configure; struct companion_mesh_profile profile;
 uint64_t destination; size_t size; uint8_t packet[COMPANION_PACKET_MAX];};
static QueueHandle_t commands;
static SemaphoreHandle_t stopped;
static TaskHandle_t task;
static atomic_bool connected, stop_requested;
extern void dev01_companion_deliver(const uint8_t *,size_t);
uint32_t companion_mesh_platform_random(void) {return esp_random();}
static int configure(const struct companion_mesh_profile *profile) {
 if(!atomic_load(&connected)) return -EACCES;
 struct command command={.configure=true,.profile=*profile};
 if(!xQueueSend(commands,&command,0)) return -ENOBUFS;
 xTaskNotifyGive(task);return 0;
}
static int send(uint64_t destination,const uint8_t *packet,size_t size) {
 if(!atomic_load(&connected)) return -EACCES;
 if(size>COMPANION_PACKET_MAX) return -EMSGSIZE;
 struct command command={.destination=destination,.size=size};memcpy(command.packet,packet,size);
 if(!xQueueSend(commands,&command,0)) return -ENOBUFS;
 xTaskNotifyGive(task);return 0;
}
static const struct companion_mesh_ops ops={configure,send,companion_mesh_radio_status};
static void worker(void *arg) {
 (void)arg;
 for(;;) {
  struct companion_mesh_status status;companion_mesh_radio_status(&status);
  ulTaskNotifyTake(pdTRUE,status.initialized?pdMS_TO_TICKS(20):portMAX_DELAY);
  if(atomic_exchange(&stop_requested,false)) {
   companion_mesh_radio_stop();xQueueReset(commands);xSemaphoreGive(stopped);continue;
  }
  struct command command;
  while(xQueueReceive(commands,&command,0)) {
   if(!atomic_load(&connected) || atomic_load(&stop_requested)) break;
   int rc;
   if(command.configure) {
    uint8_t mac[6];esp_read_mac(mac,ESP_MAC_WIFI_STA);uint64_t node=0;
    for(unsigned i=0;i<6;++i) node=(node<<8)|mac[i];
    struct companion_identity id={.node=node,.name=dev01_name(),
     .version=esp_app_get_description()->version,.country=dev01_factory_country(),.icon="sensor"};
    rc=companion_mesh_radio_start(&id,&command.profile,dev01_companion_deliver);
    ESP_LOGI("companion_mesh","Mesh start: %d",rc);
   } else {rc=companion_mesh_radio_send(command.destination,command.packet,command.size);if(rc) ESP_LOGW("companion_mesh","TX: %d",rc);}
  }
  companion_mesh_radio_tick((uint32_t)(esp_timer_get_time()/1000));
 }
}
void companion_mesh_session(bool enabled) {
 if(enabled) {
  if(atomic_load(&connected)) return;
  if(!task) {
   commands=xQueueCreate(2,sizeof(struct command));stopped=xSemaphoreCreateBinary();
   if(!commands || !stopped || xTaskCreate(worker,"mesh_companion",16384,NULL,5,&task)!=pdPASS) {
    ESP_LOGE("companion_mesh","Cannot allocate worker");return;
   }
  }
  companion_mesh_bind(&ops);atomic_store(&connected,true);return;
 }
 atomic_store(&connected,false);
 if(task) {xSemaphoreTake(stopped,0);atomic_store(&stop_requested,true);xTaskNotifyGive(task);xSemaphoreTake(stopped,portMAX_DELAY);}
 companion_mesh_bind(NULL);
}
