/* SPDX-License-Identifier: Apache-2.0 */
#include "companion_mesh_radio.h"
#include "edgez_umac_mesh_runtime.h"
#include "edgez_mesh_sae_provider.h"
#include "edgez_mesh_msdu.h"
#include "edgez_batman_adv_lite.h"
#include "edgez_rf_api.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#ifdef __ZEPHYR__
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(companion_radio, LOG_LEVEL_INF);
#define MESH_LOG(...) LOG_INF(__VA_ARGS__)
#define MESH_ERROR(...) LOG_ERR(__VA_ARGS__)
#else
#define MESH_LOG(...) ((void)0)
#define MESH_ERROR(...) ((void)0)
#endif
#define RELAY_HEADER 22U
#define ETH_HEADER 14U
#define RELAY_TYPE 0x88b5U
#define RELAY_MAGIC UINT32_C(0x455a4d51)
static struct edgez_umac_mesh_runtime radio;
static struct edgez_mesh_sae_provider security;
static struct companion_mesh_profile active_profile;
static struct companion_identity local_identity;
static companion_mesh_deliver_fn deliver_packet;
static atomic_bool active, linked, routed;
static bool security_ready, bound;
static uint32_t sequence, next_beacon, next_status;
static size_t previous_peers;
struct assembly {
 uint8_t source[6], payload[COMPANION_PACKET_MAX+1];
 uint32_t id, expires; uint16_t total, received; bool used;
};
static struct assembly assemblies[4];
static uint32_t clock_ms;
#ifdef __ZEPHYR__
static uint32_t next_diagnostic;
static void restart_required(void *ctx) {
 (void)ctx;
 LOG_ERR("RF hardware requested restart; driver=%d",radio.umac.last_driver_status);
}
#endif
static void mac_from_node(uint64_t node,uint8_t mac[6]) {for(int i=5;i>=0;--i) {mac[i]=(uint8_t)node;node>>=8;}}
static uint16_t le16(const uint8_t *p) {return (uint16_t)p[0]|((uint16_t)p[1]<<8);}
static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static void put16(uint8_t *p,uint16_t value) {p[0]=value;p[1]=value>>8;}
static void put32(uint8_t *p,uint32_t value) {for(unsigned i=0;i<4;++i) p[i]=(uint8_t)(value>>(8*i));}
static uint32_t mesh_crc(const char *s) {
 uint32_t crc=UINT32_MAX;
 for(;*s;++s) {crc^=(uint8_t)*s;for(unsigned b=0;b<8;++b) crc=(crc>>1)^((crc&1)?UINT32_C(0xedb88320):0);}
 return ~crc;
}
static size_t peers(void *ctx,uint8_t (*out)[6],size_t capacity) {
 (void)ctx;size_t count=0;
 for(size_t i=0;i<EDGEZ_UMAC_MAX_PEERS && count<capacity;++i) {
  const struct edgez_umac_peer *p=&radio.umac.peers[i];
  if(p->in_use && p->state==EDGEZ_UMAC_PEER_AUTHORIZED) memcpy(out[count++],p->mac_addr,6);
 }
 return count;
}
static void delivered(const uint8_t originator[6],const uint8_t *frame,size_t size,void *ctx) {
 (void)ctx;
 if(!atomic_load(&active) || size<ETH_HEADER+RELAY_HEADER || frame[12]!=0x88 || frame[13]!=0xb5) return;
 const uint8_t *header=frame+ETH_HEADER;size_t available=size-ETH_HEADER;
 if(le32(header)!=RELAY_MAGIC || header[4]!=1 || header[5]!=5 || le16(header+20)!=0) return;
 uint32_t id=le32(header+8), total=le32(header+12);
 uint16_t offset=le16(header+16), length=le16(header+18);
 if(!total || total>COMPANION_PACKET_MAX+1 || length!=available-RELAY_HEADER ||
    (size_t)offset+length>total) return;
 const uint8_t *payload=header+RELAY_HEADER;
 if(!offset && length==total) {
  if(payload[0]==2 && deliver_packet) deliver_packet(payload+1,length-1);
  return;
 }
 struct assembly *entry=NULL;
 for(size_t i=0;i<4;++i) if(assemblies[i].used && assemblies[i].id==id && !memcmp(assemblies[i].source,originator,6)) {entry=&assemblies[i];break;}
 if(!entry && !offset) {
  for(size_t i=0;i<4;++i) if(!assemblies[i].used || (int32_t)(clock_ms-assemblies[i].expires)>=0) {entry=&assemblies[i];break;}
  if(entry) {*entry=(struct assembly){.used=true,.id=id,.total=(uint16_t)total,.expires=clock_ms+5000};memcpy(entry->source,originator,6);}
 }
 if(!entry || entry->total!=total || entry->received!=offset) return;
 memcpy(entry->payload+offset,payload,length);entry->received+=length;
 if(entry->received==total) {
  if(entry->payload[0]==2 && deliver_packet) deliver_packet(entry->payload+1,total-1);
  entry->used=false;
 }
}
/* The platform worker owns routing state. The RF adapter only appends complete
 * authenticated packets to this single-producer/single-consumer queue. */
#define ROUTE_RX_DEPTH 4U
struct route_input {uint8_t transmitter[6];uint16_t length;uint8_t body[EDGEZ_BATADV_MAX_PACKET_LEN];};
static struct route_input route_rx[ROUTE_RX_DEPTH];
static atomic_uint route_head, route_tail;
static edgez_batadv_lite_t routing;
static uint8_t previous_addresses[EDGEZ_UMAC_MAX_PEERS][6];
static uint32_t route_deadline;
static bool peer_allowed(const uint8_t mac[6]) {
 uint8_t addresses[EDGEZ_UMAC_MAX_PEERS][6];size_t count=peers(NULL,addresses,EDGEZ_UMAC_MAX_PEERS);
 for(size_t i=0;i<count;++i) if(!memcmp(mac,addresses[i],6)) return true;
 return false;
}
static int route_transmit(const uint8_t destination[6],const uint8_t receiver[6],const uint8_t *body,size_t length) {
 if(length>EDGEZ_BATADV_MAX_PACKET_LEN) return -EMSGSIZE;
 uint8_t frame[ETH_HEADER+EDGEZ_BATADV_MAX_PACKET_LEN];
 memcpy(frame,destination,6);memcpy(frame+6,radio.radio_info.mac_addr,6);
 frame[12]=EDGEZ_BATADV_ETHERTYPE>>8;frame[13]=EDGEZ_BATADV_ETHERTYPE&255;
 memcpy(frame+ETH_HEADER,body,length);
 return edgez_mesh_sae_provider_transmit_ethernet(&security,receiver,frame,ETH_HEADER+length,0);
}
static int route_fanout(const uint8_t *body,size_t length,const uint8_t *exclude) {
 const uint8_t broadcast[6]={255,255,255,255,255,255};
 uint8_t addresses[EDGEZ_UMAC_MAX_PEERS][6];size_t count=peers(NULL,addresses,EDGEZ_UMAC_MAX_PEERS);int result=-ENOENT;
 for(size_t i=0;i<count;++i) {
  if(exclude && !memcmp(exclude,addresses[i],6)) continue;
  int rc=route_transmit(broadcast,addresses[i],body,length);if(!rc) result=0;
 }
 return result;
}
static void route_enqueue(const uint8_t *frame,size_t length,const uint8_t transmitter[6]) {
 if(length<ETH_HEADER || length-ETH_HEADER>EDGEZ_BATADV_MAX_PACKET_LEN ||
    frame[12]!=(EDGEZ_BATADV_ETHERTYPE>>8) || frame[13]!=(EDGEZ_BATADV_ETHERTYPE&255)) return;
 unsigned head=atomic_load_explicit(&route_head,memory_order_relaxed);
 unsigned tail=atomic_load_explicit(&route_tail,memory_order_acquire);
 if(head-tail>=ROUTE_RX_DEPTH) return;
 struct route_input *item=&route_rx[head%ROUTE_RX_DEPTH];
 memcpy(item->transmitter,transmitter,6);item->length=(uint16_t)(length-ETH_HEADER);
 memcpy(item->body,frame+ETH_HEADER,item->length);
 atomic_store_explicit(&route_head,head+1,memory_order_release);
}
static void route_step(uint32_t now) {
 unsigned tail=atomic_load_explicit(&route_tail,memory_order_relaxed);
 while(tail!=atomic_load_explicit(&route_head,memory_order_acquire)) {
  const struct route_input *item=&route_rx[tail%ROUTE_RX_DEPTH];
  if(peer_allowed(item->transmitter)) {
   edgez_batadv_rx_result_t result;
   edgez_batadv_lite_receive_into(&routing,item->transmitter,item->body,item->length,now,&result);
   if(result.forward_ready) {
    if(result.forward_directed && peer_allowed(result.forward_next_hop))
     (void)route_transmit(result.forward_next_hop,result.forward_next_hop,result.forward,result.forward_len);
    else if(!result.forward_directed)
     (void)route_fanout(result.forward,result.forward_len,result.forward_exclude_valid?result.forward_exclude:NULL);
   }
   if(result.deliver_ready) delivered(result.deliver_originator,result.deliver,result.deliver_len,NULL);
  }
  atomic_store_explicit(&route_tail,++tail,memory_order_release);
 }
 if((int32_t)(now-route_deadline)>=0) {
  uint8_t ogm[EDGEZ_BATADV_OGM_LEN];
  if(edgez_batadv_lite_build_periodic_ogm(&routing,now,ogm)) (void)route_fanout(ogm,sizeof(ogm),NULL);
  route_deadline=now+100;
 }
}
static int route_send(const uint8_t *destination,const uint8_t *frame,size_t length) {
 uint8_t packet[EDGEZ_BATADV_MAX_PACKET_LEN],next_hop[6];size_t size=0;
 if(!destination) {
  if(!edgez_batadv_lite_build_broadcast(&routing,frame,length,packet,&size)) return -EMSGSIZE;
  return route_fanout(packet,size,NULL);
 }
 if(!edgez_batadv_lite_build_unicast(&routing,destination,frame,length,clock_ms,packet,&size,next_hop) ||
    !peer_allowed(next_hop)) return -ENOENT;
 return route_transmit(next_hop,next_hop,packet,size);
}
static int receive_msdu(const uint8_t destination[6],const uint8_t source[6],
                        const uint8_t transmitter[6],const uint8_t *payload,size_t size) {
 struct edgez_mesh_msdu msdu;
 int rc=edgez_mesh_msdu_decode(&msdu,destination,source,payload,size);
 if(rc!=EDGEZ_UMAC_OK) return rc;
 uint8_t ethernet[1600];size_t length=0;
 rc=edgez_mesh_msdu_to_ethernet(&msdu,ethernet,sizeof(ethernet),&length);
 if(rc==EDGEZ_UMAC_OK) route_enqueue(ethernet,length,transmitter);
 return rc;
}
static int receive_data(void *ctx,const struct edgez_umac_rx_frame *raw,const struct edgez_mesh_rx_data *frame) {
 (void)ctx;(void)raw;
 /* Secure companion meshes never accept an unauthenticated data frame. */
 (void)frame;return EDGEZ_UMAC_INVALID_ARGUMENT;
}
static int receive_secure(void *ctx,const struct edgez_umac_rx_frame *raw,const struct edgez_mesh_rx_protected_data *frame) {
 (void)ctx;(void)raw;
 if(!atomic_load(&active) || frame->ccmp_body_length>1600) return EDGEZ_UMAC_INVALID_ARGUMENT;
 uint8_t plaintext[1600];size_t size=0;uint64_t packet_number=0;
 int rc=edgez_mesh_sae_provider_decrypt_data(&security,frame,plaintext,sizeof(plaintext),&size,&packet_number);
 if(rc==EDGEZ_UMAC_OK) rc=receive_msdu(frame->destination,frame->source,frame->transmitter,plaintext,size);
 return rc;
}
void companion_mesh_radio_status(struct companion_mesh_status *status) {
 *status=(struct companion_mesh_status){atomic_load(&active),atomic_load(&linked),atomic_load(&routed)};
}
static void notify_status(void) {
 if(!deliver_packet) return;
 struct companion_mesh_status state;companion_mesh_radio_status(&state);
 uint8_t packet[COMPANION_PACKET_MAX];
 int n=companion_mesh_status_packet(&local_identity,active_profile.mesh_id,&state,0,packet,sizeof(packet));
 if(n>0) deliver_packet(packet,(size_t)n);
 next_status=clock_ms+10000;
}
void companion_mesh_radio_stop(void) {
 if(atomic_load(&active)) MESH_LOG("Mesh stopping");
 atomic_store(&active,false);atomic_store(&linked,false);atomic_store(&routed,false);
 if(radio.running) (void)edgez_umac_mesh_runtime_stop(&radio);
 if(bound) {edgez_rf_unbind_umac();bound=false;}
 atomic_store(&route_head,0);atomic_store(&route_tail,0);
 if(security_ready) {edgez_mesh_sae_provider_deinit(&security);security_ready=false;}
 memset(&active_profile,0,sizeof(active_profile));memset(assemblies,0,sizeof(assemblies));
 deliver_packet=NULL;previous_peers=0;
}
int companion_mesh_radio_start(const struct companion_identity *identity,
                              const struct companion_mesh_profile *profile,
                              companion_mesh_deliver_fn deliver) {
 if (!profile) return -EINVAL;
 struct companion_mesh_profile resolved=*profile;
 if (!resolved.bandwidth_mhz) resolved.bandwidth_mhz=1;
 if (!resolved.frequency_khz) {
  for (unsigned channel=1;channel<256;++channel) {
   uint32_t frequency;uint8_t bandwidth;
   if (edgez_rf_resolve_s1g_channel(resolved.country,(uint8_t)channel,&frequency,&bandwidth) &&
       bandwidth==resolved.bandwidth_mhz) {resolved.frequency_khz=frequency;break;}
  }
 }
 profile=&resolved;
 if(!identity || !profile || !deliver || !identity->node || identity->node>>48 ||
    !profile->mesh_id[0] || strlen(profile->passphrase)<8 ||
    profile->frequency_khz<500000 || profile->frequency_khz>1000000) return -EINVAL;
 /* Phone location/name updates must not tear down a healthy mesh. */
 if(atomic_load(&active) && identity->node==local_identity.node &&
    !strcmp(profile->country,active_profile.country) &&
    !strcmp(profile->mesh_id,active_profile.mesh_id) &&
    !strcmp(profile->passphrase,active_profile.passphrase) &&
    profile->frequency_khz==active_profile.frequency_khz &&
    profile->bandwidth_mhz==active_profile.bandwidth_mhz) {
  bool changed=memcmp(profile,&active_profile,sizeof(*profile))!=0;
  active_profile=*profile;local_identity=*identity;deliver_packet=deliver;
  if(changed) next_beacon=clock_ms;
  return 0;
 }
 companion_mesh_radio_stop();
 MESH_LOG("Starting: country=%s mesh=%s frequency=%u kHz bandwidth=%u MHz",
          profile->country,profile->mesh_id,profile->frequency_khz,profile->bandwidth_mhz);
 memset(&radio,0,sizeof(radio));memset(&security,0,sizeof(security));
 local_identity=*identity;active_profile=*profile;deliver_packet=deliver;
 struct edgez_umac_mesh_runtime_config config={
  .radio=edgez_rf_driver_ops(), .channel={
   .operating_frequency_hz=profile->frequency_khz*1000,
   .operating_bw_mhz=(uint8_t)profile->bandwidth_mhz,.primary_bw_mhz=1},
  .mesh_id_length=strlen(profile->mesh_id),.beacon_interval_tu=100,.dtim_period=1,
  .compressed_ssid=mesh_crc(profile->mesh_id),.attempts=2,.secure=true,
  .rate_control={.enabled=true,.initial_mcs=0,.maximum_mcs=7,.successes_to_increase=4},
  .events={.data=receive_data,.protected_data=receive_secure,
#ifdef __ZEPHYR__
           .hardware_restart_required=restart_required,
#endif
  },
 };
 memcpy(config.country_code,profile->country,3);memcpy(config.mesh_id,profile->mesh_id,config.mesh_id_length);
 mac_from_node(identity->node,config.local_mac);
 struct edgez_mesh_sae_provider_config auth={.umac=&radio.umac,
  .password=(const uint8_t *)profile->passphrase,.password_length=strlen(profile->passphrase),
  .mesh_id=(const uint8_t *)profile->mesh_id,.mesh_id_length=config.mesh_id_length,
  .attempts=2,.bandwidth_mhz=(uint8_t)profile->bandwidth_mhz};
 mac_from_node(identity->node,auth.local_mac);
 memcpy(auth.bssid,auth.local_mac,6);
 const char *stage="platform";
 int rc=edgez_rf_platform_init();if(rc) goto failed;
 stage="SAE";rc=edgez_mesh_sae_provider_init(&security,&auth);if(rc) goto failed;
 security_ready=true;config.security=edgez_mesh_sae_provider_ops();config.security_ctx=&security;
 stage="runtime";rc=edgez_umac_mesh_runtime_init(&radio,&config);if(rc) goto failed;
 stage="bind";rc=edgez_rf_bind_umac(&radio.umac);if(rc) goto failed;bound=true;
 stage="radio start";rc=edgez_umac_mesh_runtime_start(&radio);if(rc) goto failed;
 edgez_mesh_sae_provider_set_local_address(&security,radio.radio_info.mac_addr);
 edgez_batadv_lite_init(&routing,radio.radio_info.mac_addr,companion_mesh_platform_random(),1000,clock_ms);
 memset(previous_addresses,0,sizeof(previous_addresses));route_deadline=clock_ms;
 sequence=0;next_beacon=0;atomic_store(&active,true);
#ifdef __ZEPHYR__
 next_diagnostic=clock_ms;
#endif
 MESH_LOG("Radio started");notify_status();return 0;
failed:
 MESH_ERROR("Mesh failed at %s: rc=%d driver=%d",stage,rc,radio.umac.last_driver_status);
 (void)stage;
 companion_mesh_radio_stop();return rc?rc:-EIO;
}
int companion_mesh_radio_send(uint64_t destination,const uint8_t *packet,size_t size) {
 if(!atomic_load(&active)) return -EHOSTDOWN;
 if(!packet || !size || size>COMPANION_PACKET_MAX || destination>>48) return -EINVAL;
 uint8_t frame[ETH_HEADER+RELAY_HEADER+1+COMPANION_PACKET_MAX];
 bool broadcast=!destination || destination==38801 || destination==38803 ||
                destination==38805 || destination==38807 || destination==38809;
 uint8_t mac[6];mac_from_node(destination,mac);
 if(broadcast) memset(mac,255,6);
 memcpy(frame,mac,6);memcpy(frame+6,radio.radio_info.mac_addr,6);frame[12]=0x88;frame[13]=0xb5;
 uint8_t *header=frame+ETH_HEADER;memset(header,0,RELAY_HEADER);
 put32(header,RELAY_MAGIC);header[4]=1;header[5]=5;put32(header+8,++sequence);
 put32(header+12,(uint32_t)size+1);put16(header+18,(uint16_t)size+1);
 header[RELAY_HEADER]=2;memcpy(header+RELAY_HEADER+1,packet,size);
 size_t length=ETH_HEADER+RELAY_HEADER+1+size;
 return route_send(broadcast?NULL:mac,frame,length);
}
void companion_mesh_radio_tick(uint32_t now_ms) {
 clock_ms=now_ms;if(!atomic_load(&active)) return;
 edgez_umac_mesh_runtime_tick(&radio,now_ms);
 uint8_t addresses[EDGEZ_UMAC_MAX_PEERS][6];size_t count=peers(NULL,addresses,EDGEZ_UMAC_MAX_PEERS);
 bool changed=count!=previous_peers || memcmp(previous_addresses,addresses,count*6);
 if(changed) {
  edgez_batadv_lite_init(&routing,radio.radio_info.mac_addr,companion_mesh_platform_random(),1000,now_ms);
  memset(previous_addresses,0,sizeof(previous_addresses));memcpy(previous_addresses,addresses,count*6);
  route_deadline=now_ms;
 }
 edgez_batadv_lite_set_connected(&routing,count>0,now_ms);route_step(now_ms);
 edgez_batadv_route_snapshot_t routes[EDGEZ_BATADV_MAX_ROUTES];
 size_t route_count=edgez_batadv_lite_snapshot_routes(&routing,now_ms,routes,EDGEZ_BATADV_MAX_ROUTES);
 bool link_now=count>0, route_now=count>0 && route_count>0;
 bool status_changed=atomic_load(&linked)!=link_now || atomic_load(&routed)!=route_now;
 atomic_store(&linked,link_now);atomic_store(&routed,route_now);
 /* INIT returns before RF startup completes. Notify transitions and refresh
  * periodically so a dropped BLE notification cannot leave an old UI state. */
 if(status_changed || (int32_t)(now_ms-next_status)>=0) notify_status();
#ifdef __ZEPHYR__
 if(changed || (int32_t)(now_ms-next_diagnostic)>=0) {
  const struct edgez_umac_mesh_controller_metrics *m=&radio.controller.metrics;
  LOG_INF("Mesh: peers=%u routes=%u driver=%d discoveries=%u auth=%u peering=%u rejected=%u malformed=%u state_errors=%u",
          (unsigned)count,(unsigned)route_count,radio.umac.last_driver_status,
          m->discoveries,m->authentication_frames,m->peering_frames,
          m->rejected_security_frames,m->malformed_peering_frames,m->state_errors);
  const struct edgez_umac_rx_pipeline_metrics *rx=&radio.rx_pipeline.metrics;
  LOG_INF("RX: frames=%u beacons=%u protected=%u malformed=%u handler_errors=%u",
          rx->received,rx->beacons,rx->protected_data,rx->malformed,rx->handler_failures);
  next_diagnostic=now_ms+10000;
 }
#endif
 if(count && (count!=previous_peers || (int32_t)(now_ms-next_beacon)>=0)) {
  uint8_t packet[COMPANION_PACKET_MAX];
  int length=companion_mesh_beacon(&local_identity,&active_profile,packet,sizeof(packet));
  if(length>0) (void)companion_mesh_radio_send(0,packet,(size_t)length);
  next_beacon=now_ms+(active_profile.beacon_seconds?active_profile.beacon_seconds:30)*1000;
 }
 previous_peers=count;
}
