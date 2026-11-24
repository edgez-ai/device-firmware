/* Verify the real CoAP parser and registration transaction matching. */
#include "halow_time_sync.h"
#include "er-coap-13/er-coap-13.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int called, synced;
static int64_t received, now;
int64_t k_uptime_get(void) { return now; }
static bool accept = true;
void *lwm2m_malloc(size_t n) { return malloc(n); }
void lwm2m_free(void *p) { free(p); }
void lwm2m_handle_packet(lwm2m_context_t *ctx, uint8_t *p, size_t n, void *peer)
{
    (void)p; (void)n; (void)peer; ++called;
    if (accept) ctx->serverList->status = STATE_REGISTERED;
}
void halow_device_sync_clock(lwm2m_context_t *ctx, int64_t unix_ms)
{ (void)ctx; ++synced; received = unix_ms; }
int main(void)
{
    const char *bad[] = {"{}", "{\"server_uptime_ms\":1790000000000}",
        "{\"server_unix_ms\":-1}", "{\"server_unix_ms\":999999999999999999999999}",
        "{\"server_unix_ms\":\"1790000000000\"}", "{\"server_unix_ms\":1790000000000.5}",
        "{\"server_unix_ms\":1790000000000,\"server_unix_ms\":1790000000000}",
        "{\"server_unix_ms\":1790000000000", "{\"server_unix_ms\":123}",
        "{\"note\":\"server_unix_ms\":1790000000000}"};
    for (int scenario = 0; scenario < 19; ++scenario) {
        called = synced = 0; accept = scenario != 7; halow_time_sync_reset();
        lwm2m_server_t server = {.shortID=1,.sessionH=(void *)1,.status=STATE_REG_PENDING};
        coap_packet_t request = {.type=COAP_TYPE_CON,.code=COAP_POST,.mid=0x1234,.token_len=1,.token={0x42}};
        coap_set_header_uri_path(&request, scenario == 6 ? "/dm" : "/rd");
        uint8_t request_wire[64];
        size_t request_length = coap_serialize_message(&request, request_wire);
        assert(request_length && !request.uri_path);
        lwm2m_transaction_t tx = {.mID=0x1234,.peerH=(void *)1,.message=&request,.buffer=request_wire,.buffer_len=request_length};
        lwm2m_context_t ctx = {.serverList=&server,.transactionList=&tx};
        uint8_t packet[1024] = {0x61,COAP_201_CREATED,0x12,0x34,0x42,0xff};
        const char *json = scenario >= 8 && scenario < 18 ? bad[scenario-8] : "{\"server_unix_ms\":1790000000123,\"server_uptime_ms\":42}";
        if (scenario == 1) { server.status=STATE_REG_UPDATE_PENDING;packet[1]=COAP_204_CHANGED; }
        if (scenario == 2) packet[3]++;
        if (scenario == 3) packet[4]++;
        if (scenario == 4) tx.peerH=(void *)2;
        if (scenario == 5) server.status=STATE_REGISTERED;
        if (scenario == 18) packet[1]=COAP_205_CONTENT;
        memcpy(packet+6,json,strlen(json));
        uint8_t original[1024];memcpy(original,packet,strlen(json)+6);
        halow_time_sync_packet(&ctx,packet,strlen(json)+6,(void *)1);
        assert(called == 1);
        assert(memcmp(packet,original,strlen(json)+6) == 0);
        assert(synced == (scenario == 0 || scenario == 1));
        if (synced) assert(received == 1790000000123LL);
    }
    for (int scenario = 0; scenario < 3; ++scenario) {
        halow_time_sync_reset(); called = synced = 0; now = 1000; accept = false;
        lwm2m_server_t server = {.shortID=1,.sessionH=(void *)1,.status=STATE_REG_PENDING};
        coap_packet_t request = {.type=COAP_TYPE_CON,.code=COAP_POST,.mid=0x1234,.token_len=1,.token={0x42}};
        coap_set_header_uri_path(&request,"/rd");
        uint8_t wire[64];size_t n=coap_serialize_message(&request,wire);
        lwm2m_transaction_t tx = {.mID=0x1234,.peerH=(void *)1,.message=&request,.buffer=wire,.buffer_len=n};
        lwm2m_context_t ctx = {.serverList=&server,.transactionList=&tx};
        /* Exact shape of the gateway's first 256-byte JSON block: complete
         * timestamp field, incomplete JSON object and Block2 M=1. */
        uint8_t packet[265] = {0x61,COAP_201_CREATED,0x12,0x34,0x42,0xd1,10,0x0c,0xff};
        memset(packet+9,' ',256);
        const char prefix[]="{\"server_unix_ms\":1790000000123,\"padding\":\"";
        memcpy(packet+9,prefix,sizeof(prefix)-1);
        halow_time_sync_packet(&ctx,packet,sizeof(packet),(void *)1);
        assert(synced==0); /* Do not set the clock before Wakaama accepts it. */
        if (scenario == 1) halow_time_sync_reset(); /* Context was discarded. */
        now = scenario == 2 ? 62000 : 3000; accept = true;
        uint8_t last[] = {0x61,COAP_201_CREATED,0x12,0x34,0x42,0xd1,10,0x14,0xff,'}',' '};
        halow_time_sync_packet(&ctx,last,sizeof(last),(void *)1);
        assert(synced == (scenario == 0));
        if (synced) assert(received == 1790000002123LL);
    }
    halow_time_sync_reset(); now=1000; accept=true;
    lwm2m_server_t server={.shortID=1,.sessionH=(void *)1,.status=STATE_REG_PENDING};
    coap_packet_t request={.type=COAP_TYPE_CON,.code=COAP_POST,.mid=0x1234,.token_len=1,.token={0x42}};
    coap_set_header_uri_path(&request,"/rd");uint8_t wire[64];size_t n=coap_serialize_message(&request,wire);
    lwm2m_transaction_t tx={.mID=0x1234,.peerH=(void *)1,.message=&request,.buffer=wire,.buffer_len=n};
    lwm2m_context_t ctx={.serverList=&server,.transactionList=&tx};
    uint8_t packet[256]={0x61,COAP_201_CREATED,0x12,0x34,0x42,0xff};
    const char payload[]="{\"server_unix_ms\":1790000000000}";
    memcpy(packet+6,payload,sizeof(payload)-1);
    halow_time_sync_sent((void *)1,wire,n); now=1200;
    halow_time_sync_packet(&ctx,packet,6+sizeof(payload)-1,(void *)1);
    assert(received==1790000000100LL); /* mean RTT 200 ms / 2 */
    wire[3]++; request.mid++;tx.mID++;packet[3]++; server.status=STATE_REG_PENDING;
    now=2000;halow_time_sync_sent((void *)1,wire,n);now=2400;
    halow_time_sync_packet(&ctx,packet,6+sizeof(payload)-1,(void *)1);
    assert(received==1790000000150LL); /* mean(200,400) / 2 */
    wire[3]++; request.mid++;tx.mID++;packet[3]++; server.status=STATE_REG_PENDING;
    now=3000;halow_time_sync_sent((void *)1,wire,n);now=3050;halow_time_sync_sent((void *)1,wire,n);now=3450;
    halow_time_sync_packet(&ctx,packet,6+sizeof(payload)-1,(void *)1);
    assert(received==1790000000150LL); /* exclude ambiguous retransmission */
    puts("25 registration time sync cases passed");
    return 0;
}
