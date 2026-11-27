/* SPDX-License-Identifier: Apache-2.0 */
#include "companion_mesh.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "companion_mesh_vectors.h"
static unsigned configured, sent;
static struct companion_mesh_profile applied;
static const struct companion_identity identity={.node=88199923866550ULL,.name="Device",.version="mesh-test",.icon="sensor",.country="US"};
static int configure(const struct companion_mesh_profile *value) {applied=*value;++configured;return 0;}
static int send_packet(uint64_t to,const uint8_t *packet,size_t size) {
 assert(to==198162819078700ULL);assert(size==sizeof(mesh_message));
 assert(!memcmp(packet,mesh_message,size));++sent;return 0;
}
static void status(struct companion_mesh_status *value) {*value=(struct companion_mesh_status){true,true,true};}
static const struct companion_mesh_ops ops={configure,send_packet,status};
static int exchange(const uint8_t *packet,size_t size,uint8_t *out,size_t cap) {
 return companion_protocol_exchange(&identity,packet,size,out,cap);
}
int main(void) {
 uint8_t out[512];
 companion_mesh_bind(NULL);
 assert(exchange(mesh_message,sizeof(mesh_message),out,sizeof(out))==-EACCES);
 companion_mesh_bind(&ops);
 assert(exchange(mesh_init,sizeof(mesh_init),out,sizeof(out))>0);
 assert(configured==1 && !strcmp(applied.country,"US") && !strcmp(applied.mesh_id,"mesh-chat-test"));
 assert(!strcmp(applied.passphrase,"mesh-test-pass") && applied.frequency_khz==915000 && applied.bandwidth_mhz==2);
 assert(applied.marker==2 && applied.public_key_size==32 && applied.public_key[0]==3);
 assert(exchange(mesh_get,sizeof(mesh_get),out,sizeof(out))>0);
 assert(exchange(mesh_status,sizeof(mesh_status),out,sizeof(out))>0);
 assert(exchange(mesh_bad_init,sizeof(mesh_bad_init),out,sizeof(out))==-EINVAL);
 assert(configured==1);
 uint8_t broken[sizeof(mesh_init)];memcpy(broken,mesh_init,sizeof(broken));
 for(size_t i=2;i+1<sizeof(broken);++i) if(broken[i]=='U' && broken[i+1]=='S') {broken[i-2]=8;break;}
 assert(exchange(broken,sizeof(broken),out,sizeof(out))==-EBADMSG);
 assert(configured==1);
 assert(exchange(mesh_private_set,sizeof(mesh_private_set),out,sizeof(out))>0);
 assert(configured==2 && !strcmp(applied.name,"Renamed"));
 assert(applied.public_key[0]==3); /* Private-key field cannot overwrite public identity. */
 assert(exchange(mesh_message,sizeof(mesh_message),out,sizeof(out))==0 && sent==1);
 assert(exchange(mesh_get,sizeof(mesh_get),out,2)==-ENOSPC);
 assert(companion_mesh_beacon(&identity,&applied,out,sizeof(out))>0);
 assert(companion_mesh_beacon(&identity,&applied,out,1)==-ENOSPC);
 struct companion_mesh_status ready={true,true,true};
 assert(companion_mesh_status_packet(&identity,applied.mesh_id,&ready,0,out,sizeof(out))>0);
 assert(companion_mesh_status_packet(&identity,applied.mesh_id,&ready,0,out,1)==-ENOSPC);
 assert(companion_mesh_status_packet(NULL,applied.mesh_id,&ready,0,out,sizeof(out))==-EINVAL);
 for(size_t n=0;n<sizeof(mesh_init);++n) assert(exchange(mesh_init,n,out,sizeof(out))<0);
 companion_mesh_bind(NULL);
 assert(exchange(mesh_message,sizeof(mesh_message),out,sizeof(out))==-EACCES);
 puts("SDK mesh profile interoperability, malformed input, private-key isolation and opaque forwarding passed");
 return 0;
}
