/* SPDX-License-Identifier: Apache-2.0 */
#include "companion_mesh.h"
#include "companion_codec.h"
#include <limits.h>
#ifdef __ZEPHYR__
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(companion_profile, LOG_LEVEL_INF);
#endif
static const struct companion_mesh_ops *mesh;
static struct companion_mesh_profile profile;
void companion_mesh_bind(const struct companion_mesh_ops *ops)
{
 mesh = ops;
 memset(&profile, 0, sizeof(profile));
 profile.role = 2; profile.max_hop = 4; profile.beacon_seconds = 30;
 companion_protocol_set_mesh_handler(ops ? companion_mesh_exchange : NULL);
}
static int string_field(char *out, size_t capacity, int wire, struct reader value)
{
 if (wire != 2 || value.n >= capacity || memchr(value.p, 0, value.n)) return -EBADMSG;
 memcpy(out, value.p, value.n); out[value.n] = 0; return 0;
}
static uint32_t color_name(struct reader value)
{
 static const char *names[] = {"default","red","blue","purple","yellow","pink","brown","green","orange","deep_purple","light_blue","cyan","teal","lime","deep_orange","gray","blue_gray"};
 for (size_t i=0;i<sizeof(names)/sizeof(names[0]);++i)
  if (strlen(names[i])==value.n && !memcmp(names[i],value.p,value.n)) return (uint32_t)i;
 return 0;
}
static int parse_profile(struct companion_mesh_profile *next_profile,
                         const struct companion_request *request)
{
 struct reader input = {request->body, request->body_size};
 while (input.n) {
  uint32_t tag; uint64_t value; struct reader bytes;
  int wire = next(&input,&tag,&value,&bytes);
  if (wire<0) return wire;
  /* INIT and DeviceSettings use different field numbers on the same wire. */
  uint32_t field = tag;
  if (request->kind==104) {
   switch(tag) {
    case 3:field=2;break; case 4:field=10;break; case 5:field=7;break;
    case 6:field=9;break; case 7:field=30;break; case 8:field=5;break;
    case 9:field=6;break; case 10:field=8;break; case 12:field=11;break;
    case 13:field=12;break; case 14:field=4;break; case 19:field=3;break;
    case 24:field=31;break; case 26:field=14;break; case 27:field=13;break;
    case 29:field=21;break;
    default:continue; /* Never apply upstream Wi-Fi or private-key fields. */
   }
  }
  char *text_out=NULL; size_t capacity=0;
  switch(field) {
   case 1:text_out=next_profile->country;capacity=sizeof(next_profile->country);break;
   case 2:text_out=next_profile->mesh_id;capacity=sizeof(next_profile->mesh_id);break;
   case 3:text_out=next_profile->passphrase;capacity=sizeof(next_profile->passphrase);break;
   case 7:text_out=next_profile->name;capacity=sizeof(next_profile->name);break;
   case 21:text_out=next_profile->icon;capacity=sizeof(next_profile->icon);break;
  }
  if (text_out) {
   int rc=string_field(text_out,capacity,wire,bytes); if(rc) return rc;
   continue;
  }
  if (field==8) {
   if(wire!=2 || (bytes.n && bytes.n!=32)) return -EBADMSG;
   memset(next_profile->public_key,0,32);memcpy(next_profile->public_key,bytes.p,bytes.n);
   next_profile->public_key_size=bytes.n;continue;
  }
  if(field==9 && wire==2) { next_profile->marker=color_name(bytes);continue; }
  if(field==11 || field==12) {
   if(wire!=5 || bytes.n!=4) return -EBADMSG;
   uint32_t bits=(uint32_t)bytes.p[0]|((uint32_t)bytes.p[1]<<8)|((uint32_t)bytes.p[2]<<16)|((uint32_t)bytes.p[3]<<24);
   if(field==11) next_profile->latitude_bits=bits; else next_profile->longitude_bits=bits;
   continue;
  }
  if(field==5 || field==6) {
   if(wire) return -EBADMSG;
   if(field==5) next_profile->user_id_high=value; else next_profile->user_id_low=value;
   continue;
  }
  if(field==4 || field==9 || field==10 || field==13 || field==14 || field==30 || field==31) {
   if(wire || value>UINT32_MAX) return -EBADMSG;
   switch(field) {
    case 4:next_profile->max_hop=(uint32_t)value;break;
    case 9:next_profile->marker=(uint32_t)value;break;
    case 10:next_profile->share_location=value!=0;break;
    case 13:next_profile->bandwidth_mhz=(uint32_t)value;break;
    case 14:next_profile->frequency_khz=(uint32_t)value;break;
    case 30:next_profile->beacon_seconds=(uint32_t)value;break;
    case 31:next_profile->role=(uint32_t)value;break;
   }
  }
 }
 size_t password_length=strlen(next_profile->passphrase);
 if((password_length && password_length<8) || next_profile->max_hop>32 ||
    next_profile->marker>16 || next_profile->role<2 || next_profile->role>6 ||
    next_profile->beacon_seconds>3600) return -EINVAL;
 if(next_profile->country[0] &&
    (next_profile->country[0]<'A'||next_profile->country[0]>'Z'||
     next_profile->country[1]<'A'||next_profile->country[1]>'Z')) return -EINVAL;
 uint32_t bw=next_profile->bandwidth_mhz;
 if(bw && bw!=1 && bw!=2 && bw!=4 && bw!=8) return -EINVAL;
 return 0;
}
static void fixed32(struct writer *out, uint32_t tag, uint32_t bits)
{
 put(out, ((uint64_t)tag<<3)|5);
 for(unsigned i=0;i<4;++i) {
  if(out->n>=out->cap) {out->failed=true;return;}
  out->p[out->n++]=(uint8_t)(bits>>(8*i));
 }
}
static void settings(struct writer *fields,const struct companion_identity *id)
{
 integer(fields,1,3);text(fields,3,profile.mesh_id);
 integer(fields,4,profile.share_location);text(fields,5,profile.name[0]?profile.name:id->name);
 integer(fields,6,profile.marker);integer(fields,7,profile.beacon_seconds);
 integer(fields,8,profile.user_id_high);integer(fields,9,profile.user_id_low);
 if(profile.public_key_size) blob(fields,10,profile.public_key,profile.public_key_size);
 if(profile.share_location) {fixed32(fields,12,profile.latitude_bits);fixed32(fields,13,profile.longitude_bits);}
 integer(fields,14,profile.max_hop);integer(fields,24,profile.role);integer(fields,25,0);
 integer(fields,26,profile.frequency_khz);integer(fields,27,profile.bandwidth_mhz);
 integer(fields,28,id->gps!=0);text(fields,29,profile.icon[0]?profile.icon:id->icon);
}
int companion_mesh_status_packet(const struct companion_identity *id,const char *mesh_id,
                                const struct companion_mesh_status *state,uint64_t to,
                                uint8_t *response,size_t capacity)
{
 if(!id || !mesh_id || !state || !response) return -EINVAL;
 uint8_t payload[384];struct writer fields={payload,0,sizeof(payload),false};
 integer(&fields,1,1);integer(&fields,2,state->initialized);integer(&fields,3,state->initialized);
 integer(&fields,4,state->linked);integer(&fields,5,state->route_ready);integer(&fields,6,state->route_ready);
 integer(&fields,7,0x88b5);text(&fields,8,mesh_id);integer(&fields,11,id->node);
 integer(&fields,12,1);text(&fields,13,id->version);
 struct writer output={response,0,capacity,false};
 integer(&output,1,id->node);integer(&output,2,to);integer(&output,3,2);integer(&output,4,2);
 blob(&output,102,fields.p,fields.n);
 return fields.failed||output.failed?-ENOSPC:(int)output.n;
}
int companion_mesh_exchange(const struct companion_identity *id,
                            const struct companion_request *request,
                            uint8_t *response,size_t capacity)
{
 if(!mesh) return -ENOTSUP;
 bool local=!request->to || request->to==id->node;
 bool data=request->kind==100||request->kind==101||request->kind==106||request->kind==107;
 if(data || !local) {
  if(!mesh->send || !request->kind ||
     (request->operation!=1 && request->operation!=2 && request->operation!=3 &&
      request->operation!=4 && request->operation!=9)) return -EACCES;
  int rc=mesh->send(request->to,request->packet,request->packet_size);
  return rc<0?rc:0; /* Forwarding is asynchronous, never synthesize a chat ACK. */
 }
 if(request->operation!=1) return -EACCES;
 if(request->kind!=102 && request->kind!=103 && request->kind!=104) return -ENOTSUP;
 uint64_t action=0;
 if(request->kind==104) {
  struct reader input={request->body,request->body_size};
  while(input.n) {
   uint32_t tag;uint64_t value;struct reader bytes;int wire=next(&input,&tag,&value,&bytes);
   if(wire<0) return wire;
   if(tag==1) {if(wire) return -EBADMSG;action=value;}
  }
  if(action!=1 && action!=2) return -EACCES;
 }
 if((request->kind==103 && request->body_size) || (request->kind==104 && action==2)) {
  struct companion_mesh_profile candidate=profile;
  if(!candidate.country[0] && id->country && strlen(id->country)==2)
   memcpy(candidate.country,id->country,3);
  int rc=parse_profile(&candidate,request);if(rc) return rc;
#ifdef __ZEPHYR__
  LOG_INF("Profile received: kind=%u country=%s mesh=%s frequency=%u kHz bandwidth=%u MHz key_valid=%u",
          request->kind,candidate.country,candidate.mesh_id,candidate.frequency_khz,
          candidate.bandwidth_mhz,strlen(candidate.passphrase)>=8);
#endif
  /* A zero channel selects a deterministic legal channel in the backend. */
  if(candidate.country[0] && candidate.mesh_id[0] && strlen(candidate.passphrase)>=8) {
   if(!mesh->configure) return -ENOTSUP;
   rc=mesh->configure(&candidate);if(rc) return rc;
  }
#ifdef __ZEPHYR__
  else LOG_WRN("Incomplete mesh profile; radio not configured");
#endif
  profile=candidate;
 }
 if(request->kind!=104) {
  struct companion_mesh_status state={0};if(mesh->status) mesh->status(&state);
  return companion_mesh_status_packet(id,profile.mesh_id,&state,request->from,response,capacity);
 }
 uint8_t payload[384];struct writer fields={payload,0,sizeof(payload),false};
 settings(&fields,id);
 struct writer output={response,0,capacity,false};
 integer(&output,1,id->node);integer(&output,2,request->from);integer(&output,3,2);integer(&output,4,2);
 blob(&output,104,fields.p,fields.n);
 return fields.failed||output.failed?-ENOSPC:(int)output.n;
}
int companion_mesh_beacon(const struct companion_identity *id,
                           const struct companion_mesh_profile *current,
                           uint8_t *response,size_t capacity)
{
 if(!id||!current||!response) return -EINVAL;
 uint8_t payload[256];struct writer fields={payload,0,sizeof(payload),false};
 integer(&fields,1,current->user_id_high);integer(&fields,2,current->user_id_low);
 text(&fields,3,current->name[0]?current->name:id->name);
 if(current->public_key_size) blob(&fields,4,current->public_key,current->public_key_size);
 if(current->share_location) {fixed32(&fields,5,current->latitude_bits);fixed32(&fields,6,current->longitude_bits);}
 integer(&fields,7,current->marker);integer(&fields,8,current->role);
 text(&fields,11,current->icon[0]?current->icon:id->icon);
 struct writer output={response,0,capacity,false};
 integer(&output,1,id->node);integer(&output,3,9);integer(&output,4,5);
 blob(&output,106,fields.p,fields.n);
 return fields.failed||output.failed?-ENOSPC:(int)output.n;
}
