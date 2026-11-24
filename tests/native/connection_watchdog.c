/* Real watchdog policy: simulated AP/server loss, time and Wakaama status. */
#include "halow_connection.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static int64_t now = 1000;
int64_t k_uptime_get(void) { return now; }
lwm2m_data_t *lwm2m_data_new(int n) { return calloc(n,sizeof(lwm2m_data_t)); }
void lwm2m_data_encode_int(int64_t n,lwm2m_data_t *d) { d->type=LWM2M_TYPE_INTEGER;d->value.asInteger=n; }
static int64_t read_value(lwm2m_context_t *ctx,int rid) {
 int n=1;lwm2m_data_t *data=lwm2m_data_new(1);data->id=rid;
 assert(halow_connection.readFunc(ctx,0,&n,&data,&halow_connection)==COAP_205_CONTENT);
 int64_t v=data->value.asInteger;free(data);return v;
}
static void ack(lwm2m_context_t *ctx) {
 ctx->serverList->status=STATE_REGISTERED;ctx->serverList->registration=now/1000;
 assert(halow_connection_step(ctx,true)==HALOW_CONN_NONE);
 assert(read_value(ctx,0)==2 && read_value(ctx,3)==0);
}
int main(int argc,char **argv) {
 assert(argc==2);int test=atoi(argv[1]);
 lwm2m_server_t s={.shortID=1,.lifetime=300,.status=STATE_REG_PENDING};
 lwm2m_context_t ctx={.serverList=&s};
 if(test==1) {
  assert(halow_connection_step(&ctx,true)==HALOW_CONN_NONE);ack(&ctx);
  now+=59999;assert(halow_connection_step(&ctx,true)==HALOW_CONN_NONE);
  now++;assert(halow_connection_step(&ctx,true)==HALOW_CONN_UPDATE);
  s.status=STATE_REG_UPDATE_PENDING;
  now+=44999;assert(halow_connection_step(&ctx,true)==HALOW_CONN_NONE);
  now++;assert(halow_connection_step(&ctx,true)==HALOW_CONN_REGISTER && read_value(&ctx,4)==1);
  halow_connection_context_closed();s.status=STATE_REG_PENDING;
  now+=45000;assert(halow_connection_step(&ctx,true)==HALOW_CONN_RECONNECT && read_value(&ctx,5)==1);
  halow_connection_context_closed();now+=119999;
  assert(halow_connection_step(NULL,false)==HALOW_CONN_NONE);now++;
  assert(halow_connection_step(NULL,false)==HALOW_CONN_RECONNECT && read_value(NULL,5)==2);
  now+=1000;ack(&ctx);assert(halow_connection_wait_ms()==60000);
 } else if(test==2) {
  ack(&ctx);s.status=STATE_REG_FAILED;
  assert(halow_connection_step(&ctx,true)==HALOW_CONN_REGISTER && read_value(&ctx,6)==3);
  halow_connection_context_closed();s.status=STATE_REG_PENDING;
  now+=10;assert(halow_connection_step(&ctx,true)==HALOW_CONN_NONE);
  now+=10;ack(&ctx);assert(read_value(&ctx,5)==0);
  now+=60000;assert(halow_connection_step(&ctx,true)==HALOW_CONN_UPDATE);
  s.status=STATE_REG_UPDATE_PENDING;now+=1;ack(&ctx);
 } else if(test==3) {
  ack(&ctx);halow_connection_pause();assert(halow_connection_wait_ms()==-1);
  now+=3600000;assert(halow_connection_step(&ctx,false)==HALOW_CONN_NONE && read_value(&ctx,0)==6);
  halow_connection_wake();
  assert(halow_connection_step(&ctx,true)==HALOW_CONN_REGISTER && read_value(&ctx,6)==1);
  halow_connection_context_closed();s.status=STATE_REG_PENDING;now+=1000;ack(&ctx);
 } else if(test==4) {
  ack(&ctx);halow_connection_pause();now+=60000;halow_connection_wake();
  assert(halow_connection_step(&ctx,true)==HALOW_CONN_UPDATE);
  /* Old local REGISTERED state must not count as a fresh response. */
  now+=45000;assert(halow_connection_step(&ctx,true)==HALOW_CONN_REGISTER);
 } else if(test==5) {
  ack(&ctx);halow_connection_pause();now+=60000;halow_connection_wake();
  assert(halow_connection_step(&ctx,false)==HALOW_CONN_RECONNECT);
  halow_connection_context_closed();s.status=STATE_REG_PENDING;
  now+=1000;ack(&ctx);assert(read_value(&ctx,5)==1);
 } else if(test==6) {
  assert(halow_connection_step(NULL,false)==HALOW_CONN_NONE);
  now+=120000;assert(halow_connection_step(NULL,false)==HALOW_CONN_RECONNECT);
  now+=240000;assert(halow_connection_step(NULL,false)==HALOW_CONN_RECONNECT);
  now+=239999;assert(halow_connection_step(NULL,false)==HALOW_CONN_NONE);now++;
  assert(halow_connection_step(NULL,false)==HALOW_CONN_RECONNECT);
  assert(halow_connection_wait_ms()==300000);
 } else if(test==7) {
  ack(&ctx);now+=2000;s.registration=now/1000;assert(halow_connection_step(&ctx,true)==HALOW_CONN_NONE);
  now+=59000;assert(halow_connection_step(&ctx,true)==HALOW_CONN_NONE);
  int n=0;lwm2m_data_t *d=NULL;
  assert(halow_connection.discoverFunc(&ctx,0,&n,&d,&halow_connection)==COAP_205_CONTENT && n==8);free(d);
  assert(read_value(&ctx,7)==300);
 } else if(test==8) {
  /* AP restarted during ordinary DTIM operation: native events replace the
   * context, then a fresh registration ACK restores health without a reboot. */
  ack(&ctx);now+=5000;halow_connection_context_closed();
  assert(halow_connection_step(NULL,false)==HALOW_CONN_NONE);
  s.status=STATE_REG_PENDING;now+=1000;assert(halow_connection_step(&ctx,true)==HALOW_CONN_NONE);
  now+=1000;ack(&ctx);assert(read_value(&ctx,4)==0 && read_value(&ctx,5)==0);
 }
 puts("OK");
}
