/* SPDX-License-Identifier: Apache-2.0
 * Vendor object 31026: one-shot WNM deep sleep over the existing DTIM link.
 */
#include "halow_sleep.h"
#include "imu_wake.h"
#if defined(CONFIG_EZ_DEV54_IMU_WAKE)
#include "halow_light.h"
#endif
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <zephyr/kernel.h>

#define OBJECT_ID 31026
#define RESOURCE_COUNT 12
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
static int wake_reason;
#if defined(CONFIG_EZ_DEV54_IMU_WAKE)
static bool imu_armed;
static struct imu_wake_config wake_config(void) { return imu_wake_config_get(); }
static int arm_wake(void)
{
    struct imu_wake_config c = wake_config();
    if (!c.mode) return 0;
    int rc = board_imu_wake_arm(&c, halow_light_wake);
    imu_armed = rc == 0;
    return rc;
}
static int disarm_wake(void)
{
    if (!imu_armed) return 0;
    imu_armed = false;
    return board_imu_wake_disarm();
}
static bool imu_fired(void) { return imu_armed && board_imu_wake_fired(); }
#else
static struct imu_wake_config wake_config(void)
{ return (struct imu_wake_config){IMU_WAKE_DISABLED, 500, 60}; }
static int arm_wake(void) { return 0; }
static int disarm_wake(void) { return 0; }
static bool imu_fired(void) { return false; }
#endif


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
        bool interrupt = imu_fired();
        if (!interrupt && remaining(deadline)) return remaining(deadline);
        /* A latched IMU event wins a simultaneous timer expiry. */
        wake_reason = interrupt ? (wake_config().mode == IMU_WAKE_MOTION ? 2 : 3) : 1;
        state = WAKING;
        slept_ms = k_uptime_get() - sleep_started;
        int64_t start = k_uptime_get();
        int cleanup = disarm_wake();
        int rc = halow_wnm_exit(); /* Always restore IP even if IMU cleanup failed. */
        if (!rc) rc = cleanup;
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
    int rc = arm_wake();
    if (rc) { finished(ctx, rc); return -1; }
    rc = halow_wnm_enter();
    entry_ms = k_uptime_get() - start;
    if (rc) {
        int cleanup = disarm_wake();
        if (!cleanup && rc == -EBUSY && remaining(drain_deadline)) return 100;
        finished(ctx, cleanup ? cleanup : rc); return -1;
    }
    state = SLEEPING;
    sleep_started = k_uptime_get();
    deadline = sleep_started + requested_s * 1000;
    return imu_fired() ? 0 : remaining(deadline);
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
        case 8: lwm2m_data_encode_int(wake_config().mode, r); break;
        case 9: lwm2m_data_encode_int(wake_config().motion_mg, r); break;
        case 10: lwm2m_data_encode_int(wake_config().angle_deg, r); break;
        case 11: lwm2m_data_encode_int(wake_reason, r); break;
        }
    }
    return COAP_205_CONTENT;
}
static uint8_t write_sleep(lwm2m_context_t *ctx, uint16_t instance, int count,
                          lwm2m_data_t *data, lwm2m_object_t *object, lwm2m_write_type_t type)
{
    (void)object; (void)type;
    if (instance) return COAP_404_NOT_FOUND;
    if (count < 1 || count > 4 || !data) return COAP_400_BAD_REQUEST;
    if (state != IDLE) return COAP_503_SERVICE_UNAVAILABLE;
    struct imu_wake_config config = wake_config();
    int64_t duration = duration_s;
    unsigned seen = 0;
    bool config_changed = false;
    for (int i = 0; i < count; ++i) {
        unsigned rid = data[i].id;
        if (rid >= RESOURCE_COUNT) return COAP_404_NOT_FOUND;
        if (rid != 1 && (rid < 8 || rid > 10)) return COAP_405_METHOD_NOT_ALLOWED;
        if (seen & (1U << rid)) return COAP_400_BAD_REQUEST;
        seen |= 1U << rid;
        int64_t value;
        if (!lwm2m_data_decode_int(&data[i], &value)) return COAP_400_BAD_REQUEST;
        if (rid == 1) {
            if (value < 1 || value > 86400) return COAP_400_BAD_REQUEST;
            duration = value;
        } else {
            if (value < 0 || value > UINT16_MAX) return COAP_400_BAD_REQUEST;
            if (rid == 8) {
                if (value > IMU_WAKE_ORIENTATION) return COAP_400_BAD_REQUEST;
                config.mode = value;
            } else if (rid == 9) config.motion_mg = value;
            else {
                if (value > UINT8_MAX) return COAP_400_BAD_REQUEST;
                config.angle_deg = value;
            }
            config_changed = true;
        }
    }
    if (!imu_wake_config_valid(&config)) return COAP_400_BAD_REQUEST;
    if (config_changed) {
#if defined(CONFIG_EZ_DEV54_IMU_WAKE)
        if (imu_wake_config_set(&config)) return COAP_500_INTERNAL_SERVER_ERROR;
#else
        return COAP_405_METHOD_NOT_ALLOWED;
#endif
    }
    duration_s = duration;
    for (int i = 0; i < count; ++i) changed(ctx, data[i].id);
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
    state = PENDING; last_result = 0; wake_reason = 0;
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
