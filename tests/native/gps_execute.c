/* Real deferred adapter, fake CoAP transport and acquisition boundary. */
#include "gps_execute.h"
#include "halow_gps.h"
#include "internals.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static int64_t now;
static int calls, acks, responses, result, alloc_fail;
static bool acquiring;
static uint8_t real_status = COAP_204_CHANGED;
static coap_packet_t sent;
static char payload[256];
int64_t k_uptime_get(void) {return now;}
bool lwm2m_session_is_equal(void *a,void *b,void *unused) {(void)unused;return a==b;}
uint8_t __real_dm_handleRequest(lwm2m_context_t *c,lwm2m_uri_t *u,lwm2m_server_t *s,coap_packet_t *m,coap_packet_t *r) {
 (void)c;(void)u;(void)s;(void)m;(void)r;++calls;acquiring=real_status==COAP_204_CHANGED;return real_status;
}
uint8_t __wrap_dm_handleRequest(lwm2m_context_t *,lwm2m_uri_t *,lwm2m_server_t *,coap_packet_t *,coap_packet_t *);
bool halow_gps_result(struct gps_fix *f,int *r) {
 if(acquiring)return false;*r=result;*f=(struct gps_fix){.latitude=-0.0000001,.longitude=18.1,.timestamp=1795668500};return true;
}
void coap_init_message(void *p,coap_message_type_t type,uint8_t code,uint16_t mid) {
 coap_packet_t *m=p;memset(m,0,sizeof(*m));m->type=type;m->code=code;m->mid=mid;
}
int coap_set_header_content_type(void *p,unsigned int format) {((coap_packet_t *)p)->content_type=format;return 1;}
uint8_t message_send(lwm2m_context_t *c,coap_packet_t *m,void *peer) {
 (void)c;assert(peer && m->type==COAP_TYPE_ACK && !m->code && !m->token_len && !m->payload_len);++acks;return 0;
}
lwm2m_list_t *lwm2m_list_add(lwm2m_list_t *head,lwm2m_list_t *node) {node->next=head;return node;}
lwm2m_transaction_t *transaction_new(void *peer,coap_method_t code,char *alt,lwm2m_uri_t *uri,uint16_t mid,uint8_t n,uint8_t *token) {
 assert(!alt && !uri);if(alloc_fail)return NULL;
 lwm2m_transaction_t *t=calloc(1,sizeof(*t));t->message=calloc(1,sizeof(coap_packet_t));t->peerH=peer;t->mID=mid;
 coap_packet_t *m=t->message;coap_init_message(m,COAP_TYPE_CON,code,mid);m->token_len=n;memcpy(m->token,token,n);return t;
}
bool transaction_set_payload(lwm2m_transaction_t *t,uint8_t *p,size_t n) {t->payload=malloc(n);memcpy(t->payload,p,n);t->payload_len=n;return true;}
void transaction_free(lwm2m_transaction_t *t) {free(t->payload);free(t->message);free(t);}
int transaction_send(lwm2m_context_t *ctx,lwm2m_transaction_t *t) {
 ++responses;sent=*(coap_packet_t *)t->message;assert(t->payload_len<sizeof(payload));memcpy(payload,t->payload,t->payload_len);payload[t->payload_len]=0;
 ctx->transactionList=t->next;transaction_free(t);return 0;
}
int main(void) {
 lwm2m_context_t ctx={0};lwm2m_server_t server={.status=STATE_REGISTERED,.sessionH=(void *)1};
 lwm2m_uri_t uri;LWM2M_URI_RESET(&uri);uri.objectId=31028;uri.instanceId=0;uri.resourceId=0;
 coap_packet_t m={.type=COAP_TYPE_CON,.code=COAP_POST,.mid=42,.token_len=4,.payload=(uint8_t *)"0=295",.payload_len=5}, reply={0};
 memcpy(m.token,"gps1",4);
 assert(__wrap_dm_handleRequest(&ctx,&uri,&server,&m,&reply)==COAP_IGNORE && calls==1 && acks==1);
 gps_execute_step(&ctx);assert(!responses); /* Empty ACK does not publish a fix. */
 assert(__wrap_dm_handleRequest(&ctx,&uri,&server,&m,&reply)==COAP_IGNORE && calls==1 && acks==2); /* Retry. */
 m.mid++;assert(__wrap_dm_handleRequest(&ctx,&uri,&server,&m,&reply)==COAP_503_SERVICE_UNAVAILABLE && calls==1);m.mid--;
 acquiring=false;alloc_fail=1;gps_execute_step(&ctx);assert(!responses);alloc_fail=0;gps_execute_step(&ctx);
 assert(responses==1 && sent.code==COAP_204_CHANGED && sent.type==COAP_TYPE_CON && sent.content_type==0 && sent.mid!=m.mid);
 assert(sent.token_len==4 && !memcmp(sent.token,"gps1",4));
 assert(!strcmp(payload,"{\"latitude\":-0.0000001,\"longitude\":18.1000000,\"timestamp\":1795668500}"));
 assert(__wrap_dm_handleRequest(&ctx,&uri,&server,&m,&reply)==COAP_IGNORE && calls==1); /* Late duplicate cannot restart. */
 gps_execute_step(&ctx);assert(responses==1);
 m.mid++;assert(__wrap_dm_handleRequest(&ctx,&uri,&server,&m,&reply)==COAP_IGNORE);acquiring=false;result=-ETIMEDOUT;gps_execute_step(&ctx);
 assert(responses==2 && sent.code==COAP_503_SERVICE_UNAVAILABLE && strstr(payload,"GPS acquisition failed"));
 m.mid++;result=0;assert(__wrap_dm_handleRequest(&ctx,&uri,&server,&m,&reply)==COAP_IGNORE);
 gps_execute_step(NULL);acquiring=false;gps_execute_step(&ctx);assert(responses==2); /* No stale context reply. */
 real_status=COAP_400_BAD_REQUEST;m.mid++;int before=calls;
 assert(__wrap_dm_handleRequest(&ctx,&uri,&server,&m,&reply)==real_status && calls==before+1);gps_execute_step(&ctx);assert(responses==2);
 real_status=COAP_204_CHANGED;m.payload_len=0;before=acks;
 assert(__wrap_dm_handleRequest(&ctx,&uri,&server,&m,&reply)==COAP_204_CHANGED && acks==before); /* Legacy unchanged. */
 m.payload_len=5;m.token_len=0;assert(__wrap_dm_handleRequest(&ctx,&uri,&server,&m,&reply)==COAP_400_BAD_REQUEST);
 now=1000000;puts("Deferred GPS Execute ACK, payload, token, duplicate, busy, timeout, allocation retry and context cleanup passed");
}
