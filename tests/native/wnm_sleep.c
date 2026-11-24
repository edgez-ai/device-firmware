/* Real object state machine and platform boundary, with a fake AP/clock. */
#include "halow_sleep.h"
#include <mmwlan.h>
#include <zephyr/net/net_if.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
static int64_t now = 1000;
static struct net_if iface;
static bool missing_iface;
static int suspend_error, enter_error, exit_error, enters, exits, resumes;
#if defined(CONFIG_EZ_DEV54_WNM_PARK_PINS)
static bool pins_parked;
static int park_error, restore_error;
int halow_wnm_pins_park(void) {
    assert(iface.suspended && !pins_parked);
    if (park_error) return park_error;
    pins_parked = true; return 0;
}
int halow_wnm_pins_restore(void) {
    assert(iface.suspended && pins_parked);
    pins_parked = false; return restore_error;
}
#endif
int64_t k_uptime_get(void) { return now; }
struct net_if *net_if_get_first_wifi(void) { return missing_iface ? NULL : &iface; }
int net_if_suspend(struct net_if *p) {
    assert(p == &iface && !p->suspended);
    if (suspend_error) return suspend_error;
    p->suspended = 1; return 0;
}
int net_if_resume(struct net_if *p) { assert(p == &iface && p->suspended); p->suspended = 0; ++resumes; return 0; }
int mmwlan_set_wnm_sleep_enabled_ext(const struct mmwlan_set_wnm_sleep_enabled_args *args) {
    assert(args->chip_powerdown_enabled && iface.suspended);
#if defined(CONFIG_EZ_DEV54_WNM_PARK_PINS)
    /* Entry negotiates before parking; exit restores before any SDK access. */
    assert(!pins_parked);
#endif
    now += 20;
    if (args->wnm_sleep_enabled) { ++enters; return enter_error; }
    ++exits; return exit_error;
}
lwm2m_data_t *lwm2m_data_new(int n) { return calloc(n, sizeof(lwm2m_data_t)); }
void lwm2m_data_encode_int(int64_t v, lwm2m_data_t *d) { d->type = LWM2M_TYPE_INTEGER; d->value.asInteger = v; }
int lwm2m_data_decode_int(const lwm2m_data_t *d, int64_t *v) {
    if (d->type != LWM2M_TYPE_INTEGER) return 0;
    *v = d->value.asInteger; return 1;
}
void lwm2m_resource_value_changed(lwm2m_context_t *ctx, lwm2m_uri_t *uri) {
    assert(ctx && !iface.suspended && uri->objectId == 31026 && uri->instanceId == 0 && uri->resourceId < 8);
}
static int write_value(lwm2m_context_t *ctx, int64_t value) {
    lwm2m_data_t data = {.id = 1}; lwm2m_data_encode_int(value, &data);
    return halow_sleep.writeFunc(ctx, 0, 1, &data, &halow_sleep, LWM2M_WRITE_REPLACE_RESOURCES);
}
static int exec(lwm2m_context_t *ctx) { return halow_sleep.executeFunc(ctx, 0, 0, NULL, 0, &halow_sleep); }
static int64_t read_value(lwm2m_context_t *ctx, uint16_t rid) {
    int n = 1; lwm2m_data_t *data = lwm2m_data_new(1); data->id = rid;
    assert(halow_sleep.readFunc(ctx, 0, &n, &data, &halow_sleep) == COAP_205_CONTENT);
    int64_t v = data->value.asInteger; free(data); return v;
}
int main(void) {
    lwm2m_context_t ctx = {0};
    assert(halow_sleep_step(&ctx, true) == -1 && enters == 0);
    assert(write_value(&ctx, 0) == COAP_400_BAD_REQUEST);
    assert(write_value(&ctx, 86401) == COAP_400_BAD_REQUEST);
    assert(write_value(&ctx, 86400) == COAP_204_CHANGED && read_value(&ctx,1) == 86400);
    assert(write_value(&ctx, 10) == COAP_204_CHANGED);
    assert(halow_sleep.executeFunc(&ctx,0,0,(uint8_t *)"x",1,&halow_sleep) == COAP_400_BAD_REQUEST);
    assert(halow_sleep.executeFunc(&ctx,1,0,NULL,0,&halow_sleep) == COAP_404_NOT_FOUND);
    assert(halow_sleep.executeFunc(&ctx,0,1,NULL,0,&halow_sleep) == COAP_405_METHOD_NOT_ALLOWED);
    assert(exec(&ctx) == COAP_204_CHANGED && enters == 0 && halow_sleep_pending());
    assert(exec(&ctx) == COAP_503_SERVICE_UNAVAILABLE);
    assert(write_value(&ctx, 20) == COAP_503_SERVICE_UNAVAILABLE);
    assert(halow_sleep_step(&ctx,true) == 2000 && enters == 0);
    now += 1999; assert(halow_sleep_step(&ctx,true) == 1 && enters == 0);
    lwm2m_transaction_t tx = {0}; ctx.transactionList = &tx;
    now++; assert(halow_sleep_step(&ctx,true) == 100 && enters == 0);
    ctx.transactionList = NULL;
    assert(halow_sleep_step(&ctx,true) == 10000 && halow_sleep_active() && enters == 1 && iface.suspended);
    now += 9999; assert(halow_sleep_step(&ctx,true) == 1 && exits == 0);
    now++; assert(halow_sleep_step(&ctx,true) == -1 && !halow_sleep_active() && exits == 1 && !iface.suspended);
    assert(read_value(&ctx,2) == 0 && read_value(&ctx,3) == 0 && read_value(&ctx,4) == 1);
    assert(read_value(&ctx,5) == 20 && read_value(&ctx,6) == 20 && read_value(&ctx,7) == 10000);
    /* AP rejection restores the IP interface and does not wait the sleep interval. */
    enter_error = 13; assert(exec(&ctx) == COAP_204_CHANGED); now += 2000;
    assert(halow_sleep_step(&ctx,true) == -1 && !halow_sleep_active() && !iface.suspended);
    assert(read_value(&ctx,3) == 13 && read_value(&ctx,4) == 1);
    enter_error = 0;
    /* Busy network TX is retried only within a bounded window. */
    suspend_error = -EBUSY; assert(exec(&ctx) == COAP_204_CHANGED); now += 2000;
    assert(halow_sleep_step(&ctx,true) == 100 && halow_sleep_pending()); now += 10000;
    assert(halow_sleep_step(&ctx,true) == -1 && read_value(&ctx,3) == -EBUSY);
    suspend_error = 0;
    /* Lost link aborts pending entry without calling the SDK. */
    int prior = enters; assert(exec(&ctx) == COAP_204_CHANGED);
    assert(halow_sleep_step(&ctx,false) == -1 && read_value(&ctx,3) == -ENETDOWN && enters == prior);
    /* Persistent CoAP TX prevents entry; no transaction is freed underneath Wakaama. */
    assert(exec(&ctx) == COAP_204_CHANGED); ctx.transactionList = &tx; now += 12000;
    assert(halow_sleep_step(&ctx,true) == -1 && read_value(&ctx,3) == -EBUSY && ctx.transactionList == &tx);
    ctx.transactionList = NULL;
    /* Exit error still ungates the interface and leaves the object usable. */
    exit_error = 6; assert(exec(&ctx) == COAP_204_CHANGED); now += 2000;
    assert(halow_sleep_step(&ctx,true) == 10000); now += 10000;
    assert(halow_sleep_step(&ctx,true) == -1 && !iface.suspended && read_value(&ctx,3) == 6);
    exit_error = 0;
    assert(exec(&ctx) == COAP_204_CHANGED); now += 2000;
    assert(halow_sleep_step(&ctx,true) == 10000); now += 500;
    assert(halow_sleep_step(&ctx,false) == 9500 && iface.suspended);
    now += 9500;
    assert(halow_sleep_step(&ctx,false) == -1 && !iface.suspended && read_value(&ctx,3) == -ENETDOWN);
    missing_iface = true; assert(exec(&ctx) == COAP_204_CHANGED); now += 2000;
    assert(halow_sleep_step(&ctx,true) == -1 && read_value(&ctx,3) == -ENODEV);
    missing_iface = false;
    uint8_t wire[] = {0x41,2,0x12,0x34,0x77};
    halow_sleep_request_begin((void *)1,wire,sizeof(wire));
    assert(exec(&ctx) == COAP_204_CHANGED);
    assert(exec(&ctx) == COAP_204_CHANGED); /* Same MID/token is a retransmission. */
    halow_sleep_request_end();
    assert(exec(&ctx) == COAP_503_SERVICE_UNAVAILABLE); /* A new request is busy. */
    now += 2000; assert(halow_sleep_step(&ctx,true) == 10000); now += 10000;
    assert(halow_sleep_step(&ctx,true) == -1);
    prior = enters;
    halow_sleep_request_begin((void *)1,wire,sizeof(wire));
    assert(exec(&ctx) == COAP_204_CHANGED && !halow_sleep_pending() && enters == prior);
    wire[3]++; halow_sleep_request_begin((void *)1,wire,sizeof(wire));
    assert(exec(&ctx) == COAP_204_CHANGED && halow_sleep_pending());
    assert(halow_sleep_step(&ctx,false) == -1);
    halow_sleep_forget_requests();
    halow_sleep_request_begin((void *)1,wire,sizeof(wire));
    assert(exec(&ctx) == COAP_204_CHANGED && halow_sleep_pending());
    assert(halow_sleep_step(&ctx,false) == -1);
    halow_sleep_request_end();
#if defined(CONFIG_EZ_DEV54_WNM_PARK_PINS)
    /* A failed park rolls back an already accepted WNM before ungating IP. */
    park_error = -EBUSY; prior = exits;
    assert(halow_wnm_enter() == -EBUSY && !iface.suspended && !pins_parked);
    assert(exits == prior + 1);
    park_error = 0;
    assert(halow_wnm_enter() == 0 && pins_parked);
    restore_error = -EIO; prior = exits;
    assert(halow_wnm_exit() == -EIO && !iface.suspended && !pins_parked);
    assert(exits == prior); /* Do not call SDK when host SPI failed to resume. */
    restore_error = 0;
#endif
    int count = 0; lwm2m_data_t *data = NULL;
    assert(halow_sleep.readFunc(&ctx,0,&count,&data,&halow_sleep) == COAP_205_CONTENT && count == 7 && data[0].id == 1);
    free(data); count = 0; data = NULL;
    assert(halow_sleep.discoverFunc(&ctx,0,&count,&data,&halow_sleep) == COAP_205_CONTENT && count == 8 && data[0].id == 0);
    free(data); assert(resumes == enters);
    puts("WNM object timing, AP negotiation, TX drain and failure recovery passed");
}
