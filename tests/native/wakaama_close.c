#include "liblwm2m.h"
#include "internals.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
static int callbacks;
void *lwm2m_malloc(size_t n) { return malloc(n); }
void lwm2m_free(void *p) { free(p); }
char *lwm2m_strdup(const char *s) { return strdup(s); }
int lwm2m_strncmp(const char *a,const char *b,size_t n) { return strncmp(a,b,n); }
int lwm2m_strcasecmp(const char *a,const char *b) { return strcasecmp(a,b); }
time_t lwm2m_gettime(void) { return 100; }
int lwm2m_seed(void) { return 1; }
void lwm2m_printf(const char *fmt,...) { (void)fmt; }
void *lwm2m_connect_server(uint16_t id,void *data) { (void)id;return data; }
void lwm2m_close_connection(void *peer,void *data) { (void)peer;(void)data; }
bool lwm2m_session_is_equal(void *a,void *b,void *data) { (void)data;return a==b; }
uint8_t lwm2m_buffer_send(void *p,uint8_t *data,size_t n,void *u) { (void)p;(void)data;(void)n;(void)u;return COAP_NO_ERROR; }
static void cancelled(lwm2m_context_t *ctx,lwm2m_transaction_t *tx,void *packet) {
 (void)ctx;assert(!packet);lwm2m_server_t *s=tx->userData;
 assert(s->shortID==1);++callbacks; /* Must run BEFORE the server is freed. */
}
int main(void) {
 for(int mode=0;mode<2;++mode) {
  lwm2m_context_t *ctx=lwm2m_init(NULL);assert(ctx);
  lwm2m_server_t *s=calloc(1,sizeof(*s));assert(s);
  s->shortID=1;s->sessionH=(void *)1;s->location=strdup("/rd/0");
  s->status=mode==0 ? STATE_REGISTERED : STATE_REG_UPDATE_PENDING;ctx->serverList=s;
  if(mode) {
   lwm2m_transaction_t *tx=transaction_new(s->sessionH,COAP_POST,NULL,NULL,1,0,NULL);assert(tx);
   tx->callback=cancelled;tx->userData=s;ctx->transactionList=tx;
  }
  lwm2m_close(ctx);
 }
 assert(callbacks==1);puts("Real Wakaama close cancels pending CoAP callbacks before freeing server state");
}
