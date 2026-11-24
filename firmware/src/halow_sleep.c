/* SPDX-License-Identifier: Apache-2.0
 * Vendor object 31026: one-shot WNM deep sleep over the existing DTIM link.
 */
#include "halow_sleep.h"
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <zephyr/kernel.h>

#define OBJECT_ID 31026
#define RESOURCE_COUNT 8
#define RESPONSE_GRACE_MS 2000
#define DRAIN_LIMIT_MS 10000

enum sleep_state { IDLE, PENDING, SLEEPING, WAKING };
static enum sleep_state state;
static int64_t duration_s = 60, requested_s, deadline, drain_deadline, sleep_started;
static int last_result;
static uint32_t cycles;
static int64_t entry_ms, exit_ms, slept_ms;
static struct request_key {
    bool valid;
    void *peer;
    uint16_t mid;
    uint8_t token[8], token_length;
} request, accepted;
static int64_t accepted_until;

void halow_sleep_request_begin(void *peer, const uint8_t *packet, size_t length)
{
    request.valid = false;
    /* Wakaama still validates options, routing and server authorization. This
     * only supplies a key to the Execute callback if native handling reaches it. */
    if (!packet || length < 4 || (packet[0] >> 6) != 1 ||
        ((packet[0] >> 4) & 3) > 1 || packet[1] != 2) return;
    unsigned int n = packet[0] & 15;
    if (n > sizeof(request.token) || length < 4 + n) return;
    request.peer = peer;
    request.mid = ((uint16_t)packet[2] << 8) | packet[3];
    request.token_length = n;
    memcpy(request.token, packet + 4, n);
    request.valid = true;
}
void halow_sleep_request_end(void) { request.valid = false; }
void halow_sleep_forget_requests(void) { request.valid = accepted.valid = false; }
static bool repeated_request(void)
{
    return request.valid && accepted.valid && k_uptime_get() < accepted_until &&
        request.peer == accepted.peer && request.mid == accepted.mid &&
        request.token_length == accepted.token_length &&
        !memcmp(request.token, accepted.token, request.token_length);
}

bool halow_sleep_pending(void) { return state == PENDING; }
bool halow_sleep_active(void) { return state == SLEEPING || state == WAKING; }
static void changed(lwm2m_context_t *ctx, uint16_t resource)
{
    if (!ctx) return;
    lwm2m_uri_t uri; LWM2M_URI_RESET(&uri);
    uri.objectId = OBJECT_ID; uri.instanceId = 0; uri.resourceId = resource;
    lwm2m_resource_value_changed(ctx, &uri);
}
static void finished(lwm2m_context_t *ctx, int result)
{
    state = IDLE; last_result = result;
    for (uint16_t r = 2; r < RESOURCE_COUNT; ++r) changed(ctx, r);
}
static int remaining(int64_t until)
{
    int64_t ms = until - k_uptime_get();
    return ms <= 0 ? 0 : ms > INT_MAX ? INT_MAX : (int)ms;
}
int halow_sleep_step(lwm2m_context_t *ctx, bool online)
{
    if (state == IDLE) return -1;
    if (state == SLEEPING) {
        /* DHCP/server/AP state can expire while the radio is deliberately off.
         * Honour the requested timer; recover connectivity after wake. */
        if (remaining(deadline)) return remaining(deadline);
        state = WAKING;
        slept_ms = k_uptime_get() - sleep_started;
        int64_t start = k_uptime_get();
        int rc = halow_wnm_exit(); /* Always restore the IP interface, even on error. */
        exit_ms = k_uptime_get() - start;
        ++cycles;
        finished(ctx, rc ? rc : online ? 0 : -ENETDOWN);
        return -1;
    }
    if (!ctx || !online) { finished(ctx, -ENETDOWN); return -1; }
    if (remaining(deadline)) return remaining(deadline);
    /* Do not strand a confirmable notification or registration transaction. */
    if (ctx->transactionList) {
        if (!remaining(drain_deadline)) { finished(ctx, -EBUSY); return -1; }
        return 100;
    }
    int64_t start = k_uptime_get();
    int rc = halow_wnm_enter();
    entry_ms = k_uptime_get() - start;
    if (rc == -EBUSY && remaining(drain_deadline)) return 100;
    if (rc) { finished(ctx, rc); return -1; }
    state = SLEEPING;
    sleep_started = k_uptime_get();
    deadline = sleep_started + requested_s * 1000;
    return remaining(deadline);
}
static uint8_t discover(lwm2m_context_t *ctx, uint16_t instance, int *count,
                        lwm2m_data_t **data, lwm2m_object_t *object)
{
    (void)ctx; (void)object;
    if (instance) return COAP_404_NOT_FOUND;
    if (!*count) {
        *data = lwm2m_data_new(RESOURCE_COUNT);
        if (!*data) return COAP_500_INTERNAL_SERVER_ERROR;
        *count = RESOURCE_COUNT;
        for (int i = 0; i < *count; ++i) (*data)[i].id = i;
    }
    for (int i = 0; i < *count; ++i)
        if ((*data)[i].id >= RESOURCE_COUNT) return COAP_404_NOT_FOUND;
    return COAP_205_CONTENT;
}
static uint8_t read_sleep(lwm2m_context_t *ctx, uint16_t instance, int *count,
                         lwm2m_data_t **data, lwm2m_object_t *object)
{
    if (!*count) {
        if (instance) return COAP_404_NOT_FOUND;
        *data = lwm2m_data_new(RESOURCE_COUNT - 1);
        if (!*data) return COAP_500_INTERNAL_SERVER_ERROR;
        *count = RESOURCE_COUNT - 1;
        for (int i = 0; i < *count; ++i) (*data)[i].id = i + 1;
    }
    uint8_t rc = discover(ctx, instance, count, data, object);
    if (rc != COAP_205_CONTENT) return rc;
    for (int i = 0; i < *count; ++i) {
        lwm2m_data_t *r = &(*data)[i];
        switch (r->id) {
        case 0: return COAP_405_METHOD_NOT_ALLOWED;
        case 1: lwm2m_data_encode_int(duration_s, r); break;
        case 2: lwm2m_data_encode_int(state, r); break;
        case 3: lwm2m_data_encode_int(last_result, r); break;
        case 4: lwm2m_data_encode_int(cycles, r); break;
        case 5: lwm2m_data_encode_int(entry_ms, r); break;
        case 6: lwm2m_data_encode_int(exit_ms, r); break;
        case 7: lwm2m_data_encode_int(slept_ms, r); break;
        }
    }
    return COAP_205_CONTENT;
}
static uint8_t write_sleep(lwm2m_context_t *ctx, uint16_t instance, int count,
                          lwm2m_data_t *data, lwm2m_object_t *object, lwm2m_write_type_t type)
{
    (void)object; (void)type;
    if (instance) return COAP_404_NOT_FOUND;
    if (count != 1) return COAP_400_BAD_REQUEST;
    if (data[0].id != 1) return data[0].id < RESOURCE_COUNT ? COAP_405_METHOD_NOT_ALLOWED : COAP_404_NOT_FOUND;
    if (state != IDLE) return COAP_503_SERVICE_UNAVAILABLE;
    int64_t value;
    if (!lwm2m_data_decode_int(data, &value) || value < 1 || value > 86400) return COAP_400_BAD_REQUEST;
    duration_s = value;
    changed(ctx, 1);
    return COAP_204_CHANGED;
}
static uint8_t execute_sleep(lwm2m_context_t *ctx, uint16_t instance, uint16_t resource,
                            uint8_t *buffer, int length, lwm2m_object_t *object)
{
    (void)buffer; (void)object;
    if (instance || resource >= RESOURCE_COUNT) return COAP_404_NOT_FOUND;
    if (resource) return COAP_405_METHOD_NOT_ALLOWED;
    if (length) return COAP_400_BAD_REQUEST;
    if (repeated_request()) return COAP_204_CHANGED;
    if (state != IDLE || !ctx) return COAP_503_SERVICE_UNAVAILABLE;
    /* The normal Wakaama handler sends 2.04 before step can negotiate WNM.
     * Snapshot duration; no radio, delays, or teardown inside Execute. */
    requested_s = duration_s;
    accepted = request;
    /* Keep the one-shot result across the sleeping interval and the CoAP
     * exchange lifetime after wake, including AP-buffered retransmissions. */
    accepted_until = k_uptime_get() + (requested_s + 300) * 1000;
    state = PENDING; last_result = 0;
    deadline = k_uptime_get() + RESPONSE_GRACE_MS;
    drain_deadline = deadline + DRAIN_LIMIT_MS;
    changed(ctx, 2);
    return COAP_204_CHANGED;
}
static lwm2m_list_t instance = {.id = 0};
lwm2m_object_t halow_sleep = {
    .objID = OBJECT_ID, .instanceList = &instance, .readFunc = read_sleep,
    .discoverFunc = discover, .writeFunc = write_sleep, .executeFunc = execute_sleep,
};
